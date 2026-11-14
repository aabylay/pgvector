# Code Walkthrough: How `IvfPrefilterScan` and `IvfIncludeScan` Are Implemented

This guide explains **what we changed in the C code** to add the two filtered IVFFlat algorithms. It focuses on how the pieces fit into PostgreSQL and `pgvector`, file by file and function by function.

---

## 1. The Big Picture: Why `CustomScan`?

Normally, PostgreSQL passes `WHERE` clauses to an index through its Index Access Method (`IndexAmRoutine`). We did **not** do that here for two reasons:

1. **PostgreSQL index scans only know how to AND simple `col OP const` keys (`ScanKey`).** They cannot open a second B-tree index on the side (`IvfPrefilterScan`), and they cannot walk an arbitrary `AND`/`OR` expression tree over extra column chains (`IvfIncludeScan`).
2. **Safety and zero-effort fallback.** By keeping the normal IVFFlat `IndexScan` (with its full `Filter`) as a child plan node and wrapping it in a PostgreSQL `CustomScan` node, every fallback path (out of memory, GUC turned off, dropped B-tree index) simply runs the child `IndexScan` as-is.

### How the Wrapper Talks to the Child IVFFlat Scan

Both algorithms use the exact same pattern to connect the outer `CustomScan` node to the inner IVFFlat `IndexScan`:

```
CustomScan (IvfPrefilterScan or IvfIncludeScan)
  └── child: IndexScan on ivfflat index (keeps full Filter)
```

1. **Startup (`_PG_init` in `src/vector.c`):**
   - Calls `IvfflatInit()` to register the GUCs (`ivfflat.prefilter`, `ivfflat.prefilter_mem`, `ivfflat.include_filter`, `ivfflat.include_mem`).
   - Calls `IvfflatPrefilterInit()` and `IvfflatIncludeInit()` to register the two custom scan nodes and install planner hooks on PostgreSQL's `set_rel_pathlist_hook`.
2. **Planning (`set_rel_pathlist_hook`):**
   - After PostgreSQL builds all normal access paths for a table, our hook looks for ordered IVFFlat `IndexPath`s (`ORDER BY embedding <-> q`).
   - If the query's `WHERE` clauses qualify, the hook wraps the `IndexPath` in a `CustomPath` and adds it with `add_path()`. PostgreSQL's planner compares its cost against the plain `IndexPath` and picks the cheaper one.
3. **Execution (`ExecCustomScan`):**
   - Before pulling the first row from the child `IndexScanState`, the `CustomScan` reaches into the child's scan descriptor (`child->iss_ScanDesc->opaque`, which is `IvfflatScanOpaque`) and installs a pointer:
     - `IvfflatScanSetPrefilter(scanDesc, node->prefilter)` sets `so->prefilter`.
     - `IvfflatScanSetIncludeFilter(scanDesc, &node->filter)` sets `so->includeFilter`.
   - Then the `CustomScan` simply calls `ExecProcNode(child)` in a loop and forwards the returned rows upward.
4. **Inside the IVFFlat scan (`src/ivfscan.c`):**
   - When the child `IndexScan` runs `ivfflatgettuple()` → `GetScanItems()`, it checks `so->prefilter` and `so->includeFilter` and skips non-matching entries before computing vector distances.

---

## 2. Where Everything Lives (File Map)

| File | Role |
| --- | --- |
| `src/vector.c` | Calls `IvfflatPrefilterInit()` and `IvfflatIncludeInit()` in `_PG_init()`. |
| `src/ivfflat.c` | Registers the 4 new GUCs in `IvfflatInit()` and sets `amroutine->amcaninclude = true`. |
| `src/ivfflat.h` | Defines Version 2 on-disk structs (`IvfflatMetaPageDataV2`, `IvfflatListDataV2`, `IvfflatPageOpaqueDataV2`, `IvfflatEntryHeader`, `IvfflatVectorDirEntry`), the include filter tree (`IvfflatIncludeNode`, `IvfflatIncludeFilter`), and the two pointers on `IvfflatScanOpaqueData` (`prefilter` and `includeFilter`). |
| `src/ivfprefilter.h` / `src/ivfprefilter.c` | **Algorithm 1 (`IvfPrefilterScan`):** planner hook, B-tree clause matcher, TID hash set, and `CustomScan` executor callbacks. |
| `src/ivfinclude.h` / `src/ivfinclude.c` | **Algorithm 2 (`IvfIncludeScan`):** planner hook, `AND`/`OR` filter tree builder, plan serialization, and `CustomScan` executor callbacks. |
| `src/ivfscan.c` | **Scan engine for both algorithms:** checks `so->prefilter` in `ScoreVectorPage()`, and runs `ScanIncludedList()` (`PassSet`, `EvalIncludeNode()`, `ScanVectorsUsingDirectory()`) when `so->includeFilter` is set. |
| `src/ivfutils.c` | Shared helpers for Version 2 pages, metapage reading (`IvfflatReadMeta()`), and building `entry_id`-prefixed index items (`IvfflatFormEntryItem()`). |
| `src/ivfbuild.c` | Builds Version 2 indexes when `INCLUDE (...)` is present (`CheckIncludedColumns()`, `AddTupleToSort()`, `InsertTuplesV2()`). |
| `src/ivfinsert.c` | Inserts a new row into a Version 2 index (`InsertTupleV2()`, `WriteEntryOnChain()`, `AttachDirectory()`). |
| `src/ivfvacuum.c` | Vacuums dead rows across the vector chain, directory, and included-column chains (`ivfflatbulkdelete_v2()`, `VacuumOneListV2()`). |
| `Makefile` / `Makefile.win` | Adds `src/ivfprefilter.o` and `src/ivfinclude.o` to the build. |

---

## 3. Algorithm 1: `IvfPrefilterScan` (B-tree Prefilter)

`IvfPrefilterScan` did **not** require any change to the IVFFlat on-disk index format. Almost all of its code lives in `src/ivfprefilter.c`, plus one check in `src/ivfscan.c`.

### Step 1: The TID Hash Set (`src/ivfprefilter.c`)

At the top of `src/ivfprefilter.c`, we instantiate PostgreSQL's built-in `simplehash.h` template (`SH_PREFIX tidset`) with a 64-bit integer key:

- `TidToKey(ItemPointer tid)` packs a 32-bit block number and 16-bit offset number into one `uint64`.
- `IvfflatPrefilterCreate(cxt, memBudget)` allocates the hash table and computes `maxEntries = memBudget / 16`.
- `IvfflatPrefilterAdd(pf, tid)` inserts a TID and returns `false` if `members >= maxEntries` (memory budget exceeded).
- `IvfflatPrefilterContains(pf, tid)` checks if a TID is in the hash table.

### Step 2: Planner Hook (`IvfPrefilterSetRelPathlist` in `src/ivfprefilter.c`)

When PostgreSQL finishes building paths for a table, `IvfPrefilterSetRelPathlist()` runs:

1. **Check table and clauses:** Verifies `ivfflat.prefilter` is on, the table has `WHERE` clauses (`rel->baserestrictinfo`), and the table can emit physical tuples (`CanBuildPhysicalTlist()`).
2. **Match `WHERE` clauses to a B-tree index (`ChooseBestBtreeIndex` / `MatchBtreeIndex`):**
   - Loops over all valid B-tree indexes on the table.
   - For each `RestrictInfo`, checks `restriction_is_securely_promotable()` (so row-level security policies cannot be bypassed) and calls `MatchClauseToBtreeColumn()`.
   - Accepts `col OP expr`, `expr OP col` (if the operator can be commuted), `col IN (...)` (`ScalarArrayOpExpr` with `useOr = true`), and `col IS [NOT] NULL`, as long as the operator belongs to the B-tree column's opfamily and collation matches.
   - Explicitly rejects `BoolExpr` (`OR`, `NOT`, nested `AND`) and subplans.
   - Enforces the B-tree leading-column rule (column 0 must match; only columns `0 .. k` are used) and checks that the estimated matching rows (`sel * rel->tuples`) fit inside `ivfflat.prefilter_mem`.
3. **Wrap each IVFFlat `IndexPath` (`AddPrefilterPath`):**
   - Estimates the cost of scanning the B-tree + hashing TIDs + the reduced IVFFlat scan (fewer distance calls and fewer heap fetches).
   - Creates a `CustomPath` (`ivf_prefilter_path_methods`) whose child is the IVFFlat `IndexPath` and calls `add_path(rel, (Path *) cpath)`.

### Step 3: Building the Plan Node (`IvfPrefilterPlanCustomPath` in `src/ivfprefilter.c`)

If PostgreSQL picks the `CustomPath` as the cheapest path, it calls `IvfPrefilterPlanCustomPath()`:

1. Forces the child `IndexScan` to output all table columns (`build_physical_tlist()`) so the parent `CustomScan` can project whatever columns the query requested.
2. Rewrites the matched `WHERE` clauses into B-tree index-qual format (`FixBtreeQual()` changes the table column `Var` into `varno = INDEX_VAR` with the 1-based B-tree column number, and commutes `const OP col` into `col OP' const`).
3. Saves the rewritten quals and original display quals in `cscan->custom_exprs`, saves the B-tree index OID in `cscan->custom_private`, and registers the B-tree OID in `root->glob->relationOids` so PostgreSQL invalidates cached plans if the B-tree index is dropped.

### Step 4: Executor Callbacks (`src/ivfprefilter.c`)

- **`IvfPrefilterBegin()`:**
  - Initializes the child IVFFlat `IndexScanState`.
  - Opens the B-tree index with `try_relation_open(btindexoid, AccessShareLock)`. If it was dropped or invalidated, sets `fallbackReason = "index"` and leaves the child to run unfiltered.
  - Calls PostgreSQL's `ExecIndexBuildScanKeys()` to turn the rewritten quals into B-tree `ScanKey`s (and `IndexRuntimeKeyInfo`s for parameters that aren't known until runtime).
- **`IvfPrefilterExec()` and `BuildTidSet()`:**
  - On the first tuple fetch (`!node->setBuilt`), calls `BuildTidSet()`:
    - Evaluates any runtime keys (`ExecIndexEvalRuntimeKeys()`).
    - Opens a B-tree scan with `index_beginscan()` + `index_rescan()` using the query's MVCC snapshot.
    - Loops over `index_getnext_tid()` (which reads **only the B-tree**, never the heap) and adds each TID with `IvfflatPrefilterAdd()`.
    - If `IvfflatPrefilterAdd()` returns `false` (budget exceeded), resets the memory context, sets `node->prefilter = NULL` and `fallbackReason = "memory"`, and stops draining.
  - Calls `InjectPrefilter()` → `IvfflatScanSetPrefilter(child->iss_ScanDesc, node->prefilter)` to hand the TID set pointer to the child IVFFlat scan.
  - Calls `ExecProcNode(child)` to fetch the next result tuple from the child IVFFlat scan.
- **`IvfPrefilterReScan()`:**
  - Clears `so->prefilter` on the child, resets `prefilterCtx`, marks `setBuilt = false`, and rescans the child so a new TID set is built on the next call.

### Step 5: Where the IVFFlat Scan Checks the TID Set (`src/ivfscan.c`)

Inside `ScoreVectorPage()` (`src/ivfscan.c:391`), right before extracting the vector datum and computing distance, we added a 3-line check:

```c
if (so->prefilter != NULL &&
    !IvfflatPrefilterContains(so->prefilter, &itup->t_tid))
    continue;
```

If the vector's heap TID is not in the B-tree TID set, `ScoreVectorPage()` skips the distance calculation and never puts the tuple into the tuplesort.

---

## 4. Algorithm 2: `IvfIncludeScan` (Included-Column Chains)

Unlike `IvfPrefilterScan`, `IvfIncludeScan` stores filter columns **inside the IVFFlat index itself** when you create an index with `INCLUDE (col1, col2, ...)`. That required changes in two places:

1. **Storage layer (Version 2 IVFFlat index):** `ivfflat.c`, `ivfflat.h`, `ivfutils.c`, `ivfbuild.c`, `ivfinsert.c`, `ivfvacuum.c`.
2. **Planner, `CustomScan`, and scan engine:** `ivfinclude.c` and `ivfscan.c`.

---

### Part A: Storage Layer Changes (Version 2 On-Disk Format)

#### 1. Allowing `INCLUDE` on IVFFlat (`src/ivfflat.c`)
In `ivfflathandler()`, we changed one flag on `IndexAmRoutine`:
```c
amroutine->amcaninclude = true;
```
PostgreSQL's `CREATE INDEX` parser now allows `USING ivfflat (v) INCLUDE (c1, c2)` and passes the extra columns to `ivfflatbuild()` and `ivfflatinsert()` in the `values[]` and `isnull[]` arrays (at indices `1 .. includeCount`, after the vector at index `0`).

#### 2. Version 1 vs Version 2 Data Structures (`src/ivfflat.h` and `src/ivfutils.c`)
To keep existing indexes 100% compatible:
- Indexes **without** `INCLUDE` still set `version = IVFFLAT_VERSION` (`1`) and use the original page and tuple layout.
- Indexes **with** `INCLUDE` set `version = IVFFLAT_VERSION_INCLUDE` (`2`).

In Version 2 (`src/ivfflat.h`):
- **Metapage (`IvfflatMetaPageDataV2`):** adds `includeCount` and `includeWidth[]` (the max byte width of each included column).
- **List Header (`IvfflatListDataV2`):** instead of storing all columns in one wide tuple, each IVF list has **separate linked lists of pages**:
  - `startPage` / `insertPage` for the **vector chain**.
  - `dirStartPage` for the **vector-page directory chain**.
  - `entry_hwm` (a monotonically increasing ID counter `0, 1, 2, ...` for entries in this list) and `live_count`.
  - Followed immediately by `IvfflatIncludeChain includeChains[includeCount]` (each has its own `startPage` and `insertPage`), and then the list's `center` vector.
- **Entry Items (`IvfflatEntryHeader` + `IndexTuple`):** every item on a vector page or an included-column page is built by `IvfflatFormEntryItem(entry_id, itup, &itemsz)` in `src/ivfutils.c`. It prefixes the 1-column `IndexTuple` with a 4-byte `uint32 entry_id`. The `k`-th row inserted into a list gets the same `entry_id` on every column chain and on the vector chain.
- **Page Footer (`IvfflatPageOpaqueDataV2`) & Directory (`IvfflatVectorDirEntry`):** every Version 2 page tracks `min_entry_id`, `max_entry_id`, and `live_items` in its page special area (`IvfflatPageOpaqueAdd()`). Vector pages also mirror `(blkno, min_entry_id, max_entry_id, live_items)` into the list's directory pages so a scan can see which `entry_id`s live on each vector page without reading the vector pages themselves.

#### 3. Index Build (`src/ivfbuild.c`)
- **`CheckIncludedColumns()`:** runs during `InitBuildState()`. If `ii_NumIndexAttrs > 1`, switches `buildstate->version` to `IVFFLAT_VERSION_INCLUDE`, checks that each included column is fixed-length or `varchar(n)`/`char(n)` with width $\le 512$ bytes (`IncludeColumnBound()`), and builds a 1-attribute `TupleDesc` for the vector (`vectorDesc`) and for each included column (`includeDescs[]`).
- **`AddTupleToSort()`:** appends the detoasted included-column datums into the tuplesort slot (attributes `4 .. 3 + includeCount`) alongside `(list, tid, vector)`.
- **`InsertTuplesV2()`:** when draining the sorted tuples for list `i`, assigns `entry_id = 0, 1, 2, ...`:
  - Writes each included column `(entry_id, single_col_itup)` to its own `BuildChain columns[c]`.
  - Writes `(entry_id, vector_itup)` to `BuildChain vector`.
  - Whenever `vector` starts a new page (or adds to the current one), records/updates the page's `[min_entry_id, max_entry_id]` in `BuildChain dir` (`BuildDirAppend()` / `BuildDirBump()`).
  - Writes the finished chain start/insert block numbers and `entry_hwm` into the list header (`IvfflatWriteListV2()`).

#### 4. Single-Row Insert (`src/ivfinsert.c`)
In `ivfflatinsert()`, `IvfflatReadMeta()` checks the index version. Version 2 calls `InsertTupleV2()`:
1. Finds the closest list center (`FindInsertPage()`), locks the list header buffer exclusively, and grabs `entry_id = list->entry_hwm`.
2. Calls `PatchListHwm()` to increment `entry_hwm` and `live_count` in its own GenericXLog record, so even if a crash happens mid-insert, `entry_id` is never reused.
3. For each included column `c`, forms the 1-column tuple prefixed with `entry_id` and calls `WriteEntryOnChain(..., isVector=false, withDirectory=false, ...)` to append it to `includeChains[c]`.
4. Forms the vector tuple prefixed with `entry_id` and calls `WriteEntryOnChain(..., isVector=true, withDirectory=true, ...)`, which appends the vector item and calls `AttachDirectory()` in the same WAL record to update or append the vector page's `IvfflatVectorDirEntry`.

#### 5. VACUUM (`src/ivfvacuum.c`)
Because items on the vector chain and items on the narrow included-column chains pack differently onto 8 KB pages, the same row sits at different page numbers and offsets in each chain. `ivfflatbulkdelete_v2()` → `VacuumOneListV2()` links them by `entry_id`:
1. Locks the list header page exclusively and creates a `simplehash` integer set (`delset_hash *dead`).
2. Walks the **vector chain** first. For each item whose `t_tid` is reported dead by PostgreSQL's `callback()`, records its offset for deletion and inserts its `entry_id` into `dead`.
3. Deletes the dead vector items with `PageIndexMultiDelete()`, recomputes the vector page's `min_entry_id` / `max_entry_id` / `live_items` (`IvfflatPageOpaqueRecompute()`), and updates the matching `IvfflatVectorDirEntry` (`UpdateDirectoryEntry()`).
4. Walks each **included-column chain** `0 .. includeCount - 1`, deletes every item whose `entry_id` is in `dead`, and recomputes each page's opaque header.
5. Decrements `list->live_count` (never `entry_hwm`) and rewinds each chain's `insertPage` to the first page that freed space.

---

### Part B: Planner, `CustomScan`, and Scan Engine (`src/ivfinclude.c` and `src/ivfscan.c`)

#### 1. Building the Filter Tree at Plan Time (`src/ivfinclude.c`)
When PostgreSQL finishes building paths for a table, `IvfIncludeSetRelPathlist()` checks if the table has a Version 2 IVFFlat index (`IvfflatReadMeta()`) and calls `MatchIncludeIndex()`:

- **Clause matching (`MatchIncludeClause` / `MakeIncludeLeaf`):**
  - Checks if a leaf predicate (`OpExpr`, `ScalarArrayOpExpr` with `useOr = true`, or `NullTest` `IS NULL`) touches a single column that is in the index's `INCLUDE` list (`index->indexkeys[nkeycolumns + chain]`), and uses an operator from that column's default B-tree opfamily.
  - Creates an `IncludeTreeNode` of kind `IVF_INCLUDE_LEAF` holding `chainIndex` (`0 .. includeCount - 1`), `boundWidth` (from the metapage), `avgWidth` (for I/O cost), and the qual expression.
- **Recursive `AND`/`OR` tree builder (`BuildIncludeTree`):**
  - Walks arbitrary nested `AND` and `OR` `BoolExpr`s:
    - For an **`AND`**, keeps every child branch that can be pushed and drops unpushable branches (setting `*exact = false`). Dropping an `AND` branch produces a superset filter, which is safe because the child `IndexScan` still runs the full `Filter`.
    - For an **`OR`**, pushes it only if **every** branch is pushable.
- **Simplifying and sorting the tree (`MakeIncludeComposite`):**
  - Flattens nested `AND`-inside-`AND` or `OR`-inside-`OR`.
  - **Merges same-column leaves** directly under the same `AND` or `OR` node into a single leaf, so that column's chain is walked only once for that node.
  - **Sorts children** (`CompareIncludeLeaves`) so leaves run before nested `AND`/`OR` nodes, ordered from narrowest column (`boundWidth`) to widest.
- **Costing (`AddIncludePath`):**
  - Walks the finished tree with `IncludeTreeStats()` to count total leaf passes (`nleaves`) and total chain bytes per entry (`chainBytes`).
  - Estimates the selectivity `sigmaPush` of the pushed filter (`clauselist_selectivity()`), adds the chain-walk cost (`nleaves * cpu_operator_cost + chainBytes/BLCKSZ * seq_page_cost`), and scales down the vector page I/O, distance CPU, and heap fetch costs by `sigmaPush`.

#### 2. Encoding the Tree into the Plan (`IvfIncludePlanCustomPath` in `src/ivfinclude.c`)
PostgreSQL plan nodes (`CustomScan`) can only store `Node`/`List` trees in `custom_private` and `custom_exprs`. `EncodeIncludeTree()` serializes the `IncludeTreeNode` in prefix order into two lists:
- `shape` (an `IntList` stored in `cscan->custom_private`):
  - For a leaf: `[IVF_INCLUDE_LEAF, chainIndex]`.
  - For an `AND` or `OR`: `[kind, nchildren, ...children...]`.
- `leafQuals` (stored in `cscan->custom_exprs` alongside `displayQuals` for `EXPLAIN`):
  - One qual list per leaf, with every table `Var` rewritten by `RemapIncludeMutator()` to `varno = INDEX_VAR, varattno = 1` (attribute 1 of a 1-column virtual slot).

#### 3. Rebuilding the Tree at Executor Startup (`IvfIncludeBegin` in `src/ivfinclude.c`)
When the query starts:
1. Initializes the child IVFFlat `IndexScanState` and builds a 1-attribute `TupleDesc` and virtual `TupleTableSlot` for each included column (`node->filter.columns[c]`).
2. Calls `DecodeIncludeNode()` to walk the prefix `shape` list and `leafQuals` in lockstep, allocating `IvfflatIncludeNode` structs and compiling each leaf's qual list into an executable `ExprState` with `ExecInitQual(quals, (PlanState *) node)`.
3. On the first `IvfIncludeExec()` call, `InjectIncludeFilter()` calls `IvfflatScanSetIncludeFilter(child->iss_ScanDesc, &node->filter)` to set `so->includeFilter` on the child IVFFlat scan.

#### 4. Per-List Execution Inside the IVFFlat Scan (`src/ivfscan.c`)
When `GetScanItems()` probes a list on a Version 2 index with `so->includeFilter` installed, it calls `ScanIncludedList()` (`src/ivfscan.c:676`):

1. **Read the list header (`CopyListHeaderV2`):** copies `startPage`, `dirStartPage`, `entry_hwm`, `live_count`, and the `includeChains[]` array out of the list page.
2. **Create the passing set (`PassCreate`):**
   - Chooses how to represent passing `entry_id`s for this list within `ivfflat.include_mem`:
     - `PASS_NONE` if the list is empty (`entry_hwm == 0`).
     - `PASS_BITMAP` (a flat bit array of `entry_hwm` bits) if it fits in the budget and `entry_hwm <= 4 * live_count`.
     - `PASS_HASH` (a `simplehash` of `uint32 entry_id`s, `SH_PREFIX idset`) if the bitmap would be too large or too sparse.
3. **Evaluate the filter tree (`EvalIncludeNode` and `EvalIncludeLeaf`):**
   - `EvalIncludeNode(ev, node, cand, done, out)` takes a candidate set `cand` (`NULL` at the root means all IDs `< hwm`), a `done` set of IDs that already passed an enclosing node, and an output set `out`:
     - **`IVF_INCLUDE_LEAF` (`EvalIncludeLeaf`):** walks the pages of `chains[node->chainIndex]`. For each item, reads `entry_id = IvfflatEntryGetId(item)`. Skips it immediately if it is not in `cand` or is already in `out` or `done`. Otherwise unpacks the single attribute into `col->slot`, sets `econtext->ecxt_scantuple = col->slot`, runs `ExecQual(node->qual, econtext)`, and adds passing `entry_id`s with `PassAdd(out, entry_id)`.
     - **`IVF_INCLUDE_OR`:** calls `EvalIncludeNode` on each child with the same `(cand, done, out)`, unioning matches into `out` while later branches automatically skip IDs already in `out`.
     - **`IVF_INCLUDE_AND`:** runs each child except the last into a fresh scratch set `next = PassEmptyLike(ev->proto)` and passes `next` as `cur` (`cand`) to the next child, intersecting the sets step by step. The last child writes directly into `out`. If `cur` becomes empty (`PassIsEmpty(cur)`), remaining children are skipped.
   - If `PassAdd()` hits the hash entry cap on any node, `EvalIncludeNode()` returns `false`, sets `filter->fallbackReason = "memory"`, and falls back to `ScanVectorChain()` for that list.
4. **Skip vector pages with the directory (`ScanVectorsUsingDirectory`):**
   - First walks the list's directory pages (`dirStartPage`) and counts how many vector pages have at least one passing `entry_id` in `[min_entry_id, max_entry_id]` (`PassRange()`).
   - If no vector pages match (`PassIsEmpty(set)` or `hits == 0`), returns immediately without touching any vector pages.
   - If reading only the matching pages (`hits * localCost`) is cheaper than streaming the whole vector chain (`pages * seqCost`), reads **only** the matching vector buffers (`ent->blkno`) and calls `ScoreVectorPage(scan, vpage, value, set, true)`. Otherwise walks `ScanVectorChain(scan, startPage, value, set, true)`.
5. **Skip non-matching vectors on the page (`ScoreVectorPage`):**
   - For each item on a visited vector page, checks:
     ```c
     if (checkSet && !PassHas(set, IvfflatEntryGetId(item)))
         continue;
     ```
   - Only surviving vectors are unpacked, scored with the distance function, and inserted into the tuplesort.
