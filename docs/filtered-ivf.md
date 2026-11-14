# Filtered IVFFlat Search

This document describes the two filtered-search execution paths for IVFFlat indexes in `pgvector`. Every detail below reflects the implementation on this branch.

---

## 1. The Problem: Post-Filtering in Plain IVFFlat

An IVFFlat index groups vectors into inverted lists around k-means centers. During a query, the scan compares the query vector to all list centers, opens the closest `ivfflat.probes` lists, computes distances for every vector in those lists, sorts them, and fetches the winning rows from the table (heap).

Because IVFFlat sets `amstrategies = 0`, PostgreSQL cannot pass `WHERE` clauses into the index scan. Instead, PostgreSQL evaluates the `WHERE` clause **after** the index scan as a table-level `Filter`:

1. Read every vector in the probed lists.
2. Run the distance function on every vector.
3. Sort all candidate `(distance, heap_tid)` pairs.
4. Fetch the heap row for each candidate in distance order and check the `Filter`.

When a filter matches only a small fraction of rows, most distance calculations and heap fetches are wasted on rows that fail the filter. Increasing `ivfflat.probes` or enabling `ivfflat.iterative_scan` to recover recall multiplies this wasted work.

---

## 2. Overview of the Two Filtered-Search Paths

`pgvector` adds two `CustomScan` nodes that reject non-matching rows **before** running the distance function and fetching from the heap:

| Feature | `IvfPrefilterScan` (B-tree Prefilter) | `IvfIncludeScan` (Included Columns) |
| --- | --- | --- |
| **Data source** | An existing B-tree index on the filter column(s) | Scalar columns stored inside the IVFFlat index via `INCLUDE` |
| **Index format** | Uses standard Version 1 (or Version 2) IVFFlat index | Requires a Version 2 IVFFlat index (`CREATE INDEX ... INCLUDE (...)`) |
| **When filtering happens** | Once before the IVFFlat scan starts (builds a table-wide TID hash set) | Inside `GetScanItems` for each probed list (builds a per-list `entry_id` set) |
| **What is skipped** | Distance calculation, tuplesort, and heap fetch | Distance calculation, tuplesort, heap fetch, and (when sparse) vector page reads |
| **Memory scope** | All matching TIDs across the entire table | Entries in a single IVF list (`entry_hwm` bits or passing IDs) |
| **GUC switches** | `ivfflat.prefilter`, `ivfflat.prefilter_mem` | `ivfflat.include_filter`, `ivfflat.include_mem` |

### Safety and Fallbacks

Both custom scans are pure optimizations:

- The child IVFFlat `IndexScan` keeps the full original `Filter`.
- Every returned row still passes PostgreSQL's normal visibility and filter checks.
- If a custom scan cannot run at execution time (for example, if it runs out of memory or its GUC is turned off on a cached plan), it falls back to running the unmodified child IVFFlat scan.
- The two plans are independent. Each custom scan sets only its own pointer on the child scan state (`so->prefilter` or `so->includeFilter`). Turning one GUC off does not affect the other.

---

## 3. Shared Behavior and Requirements

### Recall Still Requires Probing Enough Lists

Neither path changes how vectors are assigned to IVF lists or which lists are opened. The scan still opens lists in order of distance between the query vector and the list center.

If a selective filter eliminates most rows in the closest lists, the true nearest neighbors may sit in lists further away. To keep recall high, pair filtered scans with iterative scanning:

```sql
SET ivfflat.iterative_scan = relaxed_order;
SET ivfflat.max_probes = 100;
```

- `ivfflat.probes = 1` is too low for selective filters.
- `relaxed_order` opens additional batches of `ivfflat.probes` lists until enough rows pass the filter (up to `ivfflat.max_probes`). Because results across batches can be slightly out of distance order, you can re-sort in an outer query or materialized CTE if strict ordering is required.

### Planner Eligibility Rules

The planner offers `IvfPrefilterScan` and `IvfIncludeScan` only when all of the following hold:

- The target is a plain table or materialized view (partition children with `RELOPT_OTHER_MEMBER_REL` are supported) with no `TABLESAMPLE`.
- The table has no dropped columns (`attisdropped`) and no columns with missing default values (`atthasmissing`). The custom scan requires the child `IndexScan` to emit physical heap tuples (`build_physical_tlist`) so the parent node can project from them.
- The child path is an ordered, unparameterized IVFFlat `IndexPath`.
- Parallel execution is disabled (`parallel_safe = false`) on the custom path.
- The IVFFlat index still has exactly one key column (the vector). `amcanmulticol` remains `false`.

### Row-Level Security

Both paths test filter clauses before the table's row-level security policy runs: the B-tree scan and the column chains see every row. So a clause is used only when PostgreSQL would also use it as an index condition (`restriction_is_securely_promotable`): it is part of the policy, or it is leakproof. Other clauses stay on the child `Filter`, which applies the policy first. For example, enum and `numeric` comparisons are not leakproof, so a user restricted by a policy does not get them pushed.

---

## 4. Path 1: B-Tree Prefilter (`IvfPrefilterScan`)

### 4.1 When It Is Used

`IvfPrefilterScan` is considered when `ivfflat.prefilter = on` (default) and the table already has a suitable B-tree index. `pgvector` does not create B-tree indexes automatically.

A B-tree index is eligible when:

1. It is a real B-tree index (`BTREE_AM_OID`), not a hypothetical index.
2. If it is a partial index, the query implies its predicate (`predOK`).
3. At least one supported filter clause matches its **first key column** (column 0). Without a leading-column condition, a B-tree scan would have to read the entire index.
4. The estimated TID set size (`selectivity × table_rows × 16` bytes) fits within the memory budget (`ivfflat.prefilter_mem`, or `work_mem` when `-1`).

#### Supported Clause Shapes

A restriction clause is matched against a B-tree column when its operator belongs to the column's B-tree operator family and its collation matches:

- **Binary comparisons:** `col OP const` or `const OP col` (using the operator's commutator), such as `=`, `<`, `<=`, `>`, `>=`, and `BETWEEN` (which PostgreSQL expands into two comparison clauses).
- **Array `IN` lists:** `col OP ANY (array)`, which is how PostgreSQL plans `IN (...)`. The indexed column must be on the left. `ALL` is not matched.
- **Null checks:** `col IS NULL`. (`IS NOT NULL` is rejected because it is rarely selective.)

Additional matching rules:

- The non-column side of a comparison must be constant for the scan (a constant, query parameter, or non-volatile expression with no table columns and no subplans).
- Top-level `AND` clauses are split by PostgreSQL into separate restrictions, and each is matched independently to the first B-tree column it fits.
- `OR` / `NOT` / nested `AND` boolean expressions (`BoolExpr`) are explicitly rejected by `MatchClauseToBtreeColumn` (except `IN` / `= ANY`, which is a `ScalarArrayOpExpr`).
- Clauses held back by row-level security are not matched (see §3).
- Expression B-tree indexes work automatically because operand matching uses PostgreSQL's `match_index_to_operand`.
- Unmatched clauses remain on the child `Filter`.
- If multiple B-tree indexes match, the planner picks the one with the lowest estimated selectivity, breaking ties by fewer index pages.

### 4.2 Building and Using the TID Set

No on-disk format changes are needed. The B-tree is scanned through PostgreSQL's standard index AM API (`index_beginscan`, `index_rescan`, `index_getnext_tid`) using the query's MVCC snapshot. Dead heap TIDs may be added to the set; they use a small amount of memory but are never fetched from the heap and do not affect correctness.

- **Hash table format:** Each heap TID is packed into a 64-bit integer (`(block_number << 16) | offset`) and stored in a `simplehash` table inside a dedicated memory context. Each entry is budgeted at 16 bytes.
- **Lazy build:** The TID set is built on the first tuple pull in `ExecCustomScan`, not in `BeginCustomScan`. This ensures runtime keys (such as `InitPlan` parameters) are ready and avoids work if the scan is never executed.
- **Rescans:** If a rescan changes parameters (`chgParam != NULL`), the TID set is discarded and rebuilt. If parameters did not change, the existing TID set is reused across rescans.
- **Interrupts and fallbacks:** While draining the B-tree, the scan checks for interrupts every 1,024 TIDs. It falls back to an ordinary IVFFlat scan (leaving `so->prefilter = NULL`) in three cases:
  - `memory`: Inserting the next TID would exceed `mem_budget / 16` entries.
  - `disabled`: `ivfflat.prefilter` was turned off after a generic plan was cached.
  - `index`: The B-tree index was dropped, invalidated, or could not be opened.
- **`EvalPlanQual`:** During concurrent update rechecks (`EvalPlanQual`), the node skips building the TID set and lets the child `Filter` recheck the single test tuple.

### 4.3 Where Candidates Are Skipped

Before pulling the first tuple from the child `IndexScan`, `IvfPrefilterScan` initializes the child's scan descriptor (if not already open) and passes the TID set pointer via `IvfflatScanSetPrefilter`.

Inside `GetScanItems` (`ScoreVectorPage`), each index tuple's heap TID (`itup->t_tid`) is checked against the TID set **before** reading the vector datum, computing distance, or inserting into the tuplesort. On a Version 2 index, the 4-byte entry header is unwrapped first so `t_tid` can be read normally.

### 4.4 Cost Model (`CostPrefilterPath`)

The prefilter cost starts from the child IVFFlat path's cost and applies a delta for one batch of `ivfflat.probes` lists. With `N` table rows, `lists` and `dimensions` read from the metapage, selectivity `sel`, and `T = N × min(probes / lists, 1)`:

- **Added to startup and total cost:**
  - B-tree descent (`log2(N) × cpu_operator_cost`), draining `sel × N` tuples (`cpu_index_tuple_cost + cpu_operator_cost` per tuple), and reading `ceil(sel × btree_pages)` leaf pages at `random_page_cost`.
  - One hash lookup per candidate in the probed lists (`T × cpu_operator_cost`).
- **Subtracted from startup and total cost:**
  - Skipped distance evaluations: `T × (1 - sel) × dimensions × cpu_operator_cost`.
  - Smaller tuplesort: `2 × cpu_operator_cost × (T log2(T) - (sel × T) log2(sel × T))`.
- **Subtracted from total cost only:**
  - Skipped heap fetches on rejected rows: `(plain_fetches - pulled) × (0.5 × random_page_cost + cpu_tuple_cost)`, where `pulled` is the estimated rows needed (accounting for `LIMIT`) and `plain_fetches = min(T, pulled / sel)`.

---

## 5. Path 2: Included Columns (`IvfIncludeScan`)

### 5.1 Creating an Index with `INCLUDE`

IVFFlat sets `amcaninclude = true`, allowing non-key scalar columns to be stored in the index:

```sql
CREATE INDEX ON items USING ivfflat (embedding vector_l2_ops)
    INCLUDE (category_id, price)
    WITH (lists = 100);
```

- **Version 1 vs. Version 2:** An IVFFlat index without `INCLUDE` uses on-disk format Version 1 (`IVFFLAT_VERSION = 1`). An index with at least one included column uses Version 2 (`IVFFLAT_VERSION_INCLUDE = 2`). There is no in-place upgrade; `REINDEX` builds a fresh Version 2 index.
- **Supported column types:**
  - Fixed-length types (`attlen > 0`) up to 512 bytes (such as `int2`, `int4`, `int8`, `float4`, `float8`, `timestamp`, `uuid`, `bool`). The metapage records `attlen` as the column's width bound.
  - `varchar(n)` and `char(n)` with `n <= 512`. The metapage records `512` as the width bound. At build and insert time, any detoasted value whose stored size (including the varlena header) exceeds 512 bytes is rejected with an error—values are never truncated.
  - Unbounded `varchar`, `text`, `bytea`, `jsonb`, `numeric`, arrays, and other variable-length types are rejected at `CREATE INDEX`.
- **Column count limits:**
  - PostgreSQL caps total index columns at `INDEX_MAX_KEYS`, so an index can have at most `INDEX_MAX_KEYS - 1` (31 in a standard build) included columns.
  - In addition, the Version 2 list header, all per-column chain pointers, and the center vector must fit on a single 8 KB index page (at most 8,144 bytes). For example, `vector(2000)` (8,008 bytes) fits up to 14 included columns and rejects 15.
- **Null handling:** Rows with a `NULL` vector are omitted from the index (same as Version 1). Rows with `NULL` included column values are stored in the index using standard `IndexTuple` null bitmaps.

### 5.2 Version 2 On-Disk Layout

Instead of packing metadata into the vector `IndexTuple` (which would hit PostgreSQL's 8,191-byte tuple size limit and force every scan to read every column), Version 2 stores each included column in its own page chain per IVF list:

- **Metapage (`IvfflatMetaPageDataV2`):** Extends the Version 1 metapage with `includeCount` and `includeWidth[IVFFLAT_MAX_INCLUDE_COLS]`.
- **List header (`IvfflatListDataV2`):** For each IVF list, stores:
  - `startPage` and `insertPage` for the vector chain.
  - `dirStartPage` for the list's vector-page directory.
  - `entry_hwm`: the next `entry_id` (`uint32`) to assign in this list. Never decreases.
  - `live_count`: the number of live entries currently in this list.
  - `includeChains[includeCount]`: one `(startPage, insertPage)` pair per included column.
  - `center`: the list's center vector, stored immediately after `includeChains` at a `MAXALIGN` offset.
- **Chain items (`IvfflatEntryHeader` + `IndexTuple`):** Every item on a vector page or column-chain page begins with a 4-byte `uint32 entry_id`, followed immediately by a single-attribute `IndexTuple` whose `t_tid` holds the heap TID.
  - The vector chain stores only the vector attribute.
  - Column chain `c` stores only included column `c`. If a query does not filter on column `c`, that chain is never opened.
- **Page opaque (`IvfflatPageOpaqueDataV2`):** 16 bytes on every Version 2 page, holding `nextblkno`, `min_entry_id`, `max_entry_id`, `live_items`, and `page_id`.
- **Vector-page directory (`IvfflatVectorDirEntry`):** A small chain of pages (16 bytes per entry) with one record per vector page in the list: `(blkno, min_entry_id, max_entry_id, live_items)`. This allows a scan to see which vector pages contain surviving `entry_id`s before reading the vector pages themselves.

Because `VACUUM` compacts pages and changes item offsets, page offsets cannot align a vector with its included column values. Instead, `entry_id` uniquely identifies an entry within its list across the vector chain and all column chains. Deleted IDs are never reused, preventing concurrent scans from confusing a newly inserted row with an old ID in their passing set.

### 5.3 Build, Insert, and Vacuum

- **Build (`src/ivfbuild.c`):** K-means sampling and center calculation use only the vector. Parallel workers sort tuples by list number, carrying the detoasted included values in the sort tuple. For each list, the leader assigns sequential `entry_id`s (`0 .. n - 1`), writes each column chain, writes the vector chain, populates the vector-page directory, and updates the list header.
- **Insert (`src/ivfinsert.c`):** `InsertTupleV2` finds the nearest list center and takes an exclusive buffer lock on the list header page. While holding that lock, it:
  1. Allocates `entry_id = list->entry_hwm++` and increments `live_count`.
  2. Appends `(entry_id, column_tuple)` to each included column chain in `INCLUDE` order.
  3. Appends `(entry_id, vector_tuple)` to the vector chain and updates the vector-page directory in the same WAL record.
  4. Releases the list header lock.
  Inserts into different lists run concurrently; inserts into the same list serialize on the list header lock. Because included columns are index attributes, updating an included column prevents a HOT update and writes a new index entry with a new `entry_id`.
- **Vacuum (`src/ivfvacuum.c`):** For each list page (locked exclusively so concurrent inserts wait), `ivfflatbulkdelete_v2` scans the vector chain, records the `entry_id` of each dead tuple in a hash set, deletes those items from the vector page, and updates both the page opaque and the vector-page directory entry. It then walks each included column chain once, deleting items whose `entry_id` is in the dead set and recomputing each page's `min_entry_id`, `max_entry_id`, and `live_items`. Finally, it decrements `live_count` and points `insertPage` for the vector chain and each column chain to the first page where space was freed, leaving `entry_hwm` unchanged.

### 5.4 When `IvfIncludeScan` Is Used and How It Executes

`IvfIncludeScan` is offered when `ivfflat.include_filter = on` (default), the index is Version 2, and the bitmap memory estimate for a single list (`ceil((table_rows / lists) / 8)` bytes) fits within `ivfflat.include_mem` (or `work_mem` when `-1`).

#### Supported Pushed Clauses

Each restriction clause becomes a filter tree. Its leaves are single-column predicates on an included column (on the bare column `Var`, not an expression):

- Binary comparisons (`col OP expr` or `expr OP col`) where the operator belongs to the column's default B-tree operator family. Any collation works (for example, `label = 'x' COLLATE "C"`), because the leaf runs the clause itself on the stored value.
- `col OP ANY (array)` (including `IN (...)`).
- `col IS NULL`.

Leaves combine with `AND` and `OR` at any depth, across one or many included columns, for example `(category = 1 AND price < 50) OR price > 95`:

- An `AND` keeps the arms it can push and drops the rest. The pushed filter then passes more rows than the clause, which is safe because the child `Filter` checks every row again. For example, `(category = 1 AND note = 'keep') OR price < 10` pushes `(category = 1) OR (price < 10)`.
- An `OR` is pushed only when every arm can be pushed.
- Top-level `AND` clauses are separate restrictions. Their trees are joined under one root `AND`.

Not pushed (left on the child `Filter` only):

- `NOT` expressions and `IS NOT NULL`.
- `OR` expressions with an arm that cannot be pushed, such as `category = 1 OR note = 'keep'`.
- Expressions or functions over the column (such as `lower(code) = 'a'`), and whole clauses that contain volatile functions or subplans.
- Clauses held back by row-level security (see §3).

#### Per-List Execution in `GetScanItems`

The planner simplifies each tree before it runs:

- Nested nodes of the same kind are flattened.
- Leaves under one node that read the same column chain are merged into one leaf, so that chain is walked once for the node. For example, `category = 1 OR category = 2` is one leaf.
- Under each node, leaves run first, from narrowest metapage width bound (`includeWidth`) to widest, then nested `AND` / `OR` nodes.

The plan stores the tree as a prefix-order list of node kinds plus one qual list per leaf. At scan startup, `IvfIncludeScan` rebuilds the tree, remaps each leaf's `Var` to attribute 1 of a one-column scan slot (`INDEX_VAR`), compiles one `ExprState` per leaf, and installs the filter via `IvfflatScanSetIncludeFilter`.

For each probed list in the current batch, `ScanIncludedList` executes four steps and then frees the passing set before moving to the next list:

1. **Snapshot the list header:** Share-locks the list header buffer, copies `startPage`, `dirStartPage`, `entry_hwm`, `live_count`, and the chain pointers, then releases the lock. Any concurrent insert that happens afterwards receives an `entry_id >= entry_hwm` and is ignored by this scan.
2. **Allocate the passing set (`PassSet`):** Covers IDs in `[0, entry_hwm)`:
   - If `entry_hwm == 0`, the set is empty (`PASS_NONE`).
   - If a bitmap of `ceil(entry_hwm / 8)` bytes fits in `ivfflat.include_mem` and the list is not mostly dead holes (`entry_hwm <= 4 × live_count`), it allocates a dense bitmap (`PASS_BITMAP`).
   - Otherwise, it allocates a `simplehash` table (`PASS_HASH`) capped at `budget / 16` entries. If the budget is under 16 bytes (`maxEntries == 0`), it falls back to keeping all entries (`PASS_ALL`) and records `Include Fallback: memory`.
3. **Evaluate the filter tree (`EvalIncludeNode`):** Each node gets a candidate set (every ID at the root) and adds the IDs that pass to an output set.
   - A **leaf** walks its column chain, skips IDs that are not candidates or are already accepted, runs `ExecQual` on the stored value, and adds the IDs that pass.
   - An **`OR`** runs every child with the same candidate and output sets, so the output collects the union. Later children skip IDs that earlier ones accepted.
   - An **`AND`** runs each child except the last into a new scratch set, which becomes the next child's candidate set (intersection). The last child writes to the output set.
   - When a candidate set becomes empty, the remaining children are skipped. At the root this also skips the directory and vector reads for the list.
   - `ivfflat.include_mem` limits each set separately. Each `AND` node keeps up to two scratch sets of the same kind alive while it runs, so nesting adds memory.
   - If a `PASS_HASH` set hits its entry cap, the node stops chain filtering for that list, records `Include Fallback: memory`, and scans the list's vector chain directly (relying on the child `Filter`).
4. **Read and score surviving vectors:**
   - The scan walks the list's vector-page directory (`dirStartPage`) and counts total vector `pages` and `hits` (pages with `live_items > 0` whose `[min_entry_id, max_entry_id]` range intersects the passing set).
   - Using `local_page_cost = seq_page_cost + 0.5 × (random_page_cost - seq_page_cost)`, it compares directory lookup against a full chain walk:
     - If `hits × local_page_cost >= pages × seq_page_cost`, it walks the vector chain sequentially from `startPage` and computes distances only for items in the passing set.
     - Otherwise, it reads only the matching vector pages from the directory and computes distances for items in the passing set.

If a Version 2 index is scanned without an active include filter (for example, an unfiltered query or a query using `IvfPrefilterScan`), the scan simply walks `startPage`, unwraps the 4-byte `entry_id` header, and never opens the column chains or the directory.

### 5.5 Cost Model (`CostIncludePath`)

Unlike `CostPrefilterPath`, `CostIncludePath` computes the cost from scratch based on the expected number of lists `P` the scan will open.

Let `C = table_rows / lists`, `σ_push` be the selectivity of the filter pushed to `IvfIncludeScan` (for a partly pushed clause, the selectivity of the pushed part), and `σ_all` be the selectivity of all restriction clauses on the table:

1. **Number of probed lists (`P`):**
   - With `iterative_scan = off`: `P = min(probes, lists)`.
   - With `iterative_scan = relaxed_order` and no `LIMIT`: `P = min(max(max_probes, probes), lists)`.
   - With `iterative_scan = relaxed_order` and `LIMIT k`: enough lists so expected survivors `P × C × σ_all >= k`, rounded up to a multiple of `batch = min(probes, lists)` and capped at `min(max(max_probes, probes), lists)`.
2. **Startup cost (all index work):**
   - `center_io`: Sequential I/O to read all list centers once.
   - `P × C × cost_filter`: CPU cost (`leaves × cpu_operator_cost`) plus sequential page I/O to read one column chain per leaf (using the column's planner average width `get_attavgwidth` plus 4-byte entry header and 4-byte line pointer).
   - `vector_io`: The cheaper of sequential I/O across all vector pages in `P` lists (`vector_pages × seq_page_cost`) or directory-guided I/O on expected hit pages (`vector_pages × hit_fraction × local_page_cost`), where `hit_fraction = 1 - (1 - σ_push)^tuples_per_page`.
   - `P × C × σ_push × cost_distance`: Distance evaluations (`dimensions × cpu_operator_cost`) paid only for entries that pass the pushed clauses.
3. **Total cost:**
   - Adds heap fetches only for surviving rows: `min(path.rows, P × C × σ_all) × (random_page_cost + cpu_tuple_cost)`.

Because all index work is placed in `startup_cost`, a small `LIMIT k` does not artificially scale down the index scan cost when PostgreSQL interpolates `startup + (total - startup) × (k / rows)`.

> **Note on cost comparison with plain IVFFlat:** Plain `ivfflatcostestimate` always prices a single batch of `ivfflat.probes` lists, even when `iterative_scan` is enabled. For a very selective filter on short vectors with `iterative_scan = relaxed_order`, `CostIncludePath` honestly prices the extra lists needed to satisfy `LIMIT k`, which can make its estimate higher than the under-priced plain IVFFlat path.

---

## 6. How the Planner Chooses Between Paths

In `_PG_init`, `IvfflatPrefilterInit` registers its `set_rel_pathlist_hook` first, and `IvfflatIncludeInit` wraps it:

1. `IvfflatIncludePathlistHook` takes a snapshot copy of the relation's ordered, unparameterized IVFFlat `IndexPath`s.
2. It calls the previous hook (`IvfflatPrefilterPathlistHook`), which adds `IvfPrefilterScan` paths for any matching B-tree.
3. It then adds `IvfIncludeScan` paths wrapping the copied IVFFlat paths.

Neither custom scan wraps the other. When a query matches both a B-tree and an `INCLUDE` column, both custom paths compete in PostgreSQL's `add_path` alongside the plain IVFFlat path, B-tree + Sort, and Sequential Scan, and the lowest-cost plan wins.

---

## 7. Configuration and `EXPLAIN` Output

### GUC Settings

| Setting | Default | Description |
| --- | --- | --- |
| `ivfflat.prefilter` | `on` | Enables the `IvfPrefilterScan` B-tree prefilter plan. |
| `ivfflat.prefilter_mem` | `-1` (`work_mem`) | Maximum memory for the B-tree TID hash set, in kB. |
| `ivfflat.include_filter` | `on` | Enables the `IvfIncludeScan` included-column plan. |
| `ivfflat.include_mem` | `-1` (`work_mem`) | Maximum memory for one IVF list's passing set, in kB. |
| `ivfflat.iterative_scan` | `off` | Set to `relaxed_order` to probe additional list batches until `LIMIT` is met. |
| `ivfflat.max_probes` | `32768` | Maximum lists opened when `ivfflat.iterative_scan` is enabled. |
| `ivfflat.probes` | `1` | Number of lists probed in the initial batch and in each iterative batch. |

### `EXPLAIN` Output

**B-Tree Prefilter (`IvfPrefilterScan`):**

```text
Custom Scan (IvfPrefilterScan)
  Prefilter: (category_id = 123)
  Prefilter Index: items_category_id_idx
  Prefilter TIDs: 482
  Prefilter Rebuilds: 0
  -> Index Scan using items_embedding_idx
       Order By: (embedding <-> $1)
       Filter: (category_id = 123)
```

- `Prefilter` and `Prefilter Index` are always shown.
- `Prefilter TIDs`, `Prefilter Rebuilds` (rebuilds after the first build), and `Prefilter Fallback` (`memory`, `disabled`, or `index`, shown only when a fallback occurred) appear under `EXPLAIN ANALYZE`.

**Included Columns (`IvfIncludeScan`):**

```text
Custom Scan (IvfIncludeScan)
  Include Filter: ((category_id = 123) AND (price < 10))
  -> Index Scan using items_embedding_idx
       Order By: (embedding <-> $1)
       Filter: ((category_id = 123) AND (price < 10))
```

- `Include Filter` shows the pushed filter tree in the order it runs. It can differ from the query: leaves are reordered, same-column leaves are merged, and `AND` arms that cannot be pushed are left out. Unpushed clauses appear only on the child `Filter`, which always shows the full original clauses.
- `Include Fallback` (`memory` or `disabled`) appears under `EXPLAIN ANALYZE` if a fallback occurred.

---

## 8. Source Code Map

| Component | Files |
| --- | --- |
| B-tree clause matching, TID hash set, `IvfPrefilterScan` | `src/ivfprefilter.c`, `src/ivfprefilter.h` |
| Included-column matching, cost model, `IvfIncludeScan` | `src/ivfinclude.c`, `src/ivfinclude.h` |
| Per-list chain filtering, directory lookup, TID check | `src/ivfscan.c` |
| Version 2 index build and column-type validation | `src/ivfbuild.c` |
| Version 2 insert, list locking, and directory updates | `src/ivfinsert.c` |
| Version 2 vacuum (`entry_id` deletion across chains) | `src/ivfvacuum.c` |
| On-disk structs, metapage helpers, page opaque helpers | `src/ivfflat.h`, `src/ivfutils.c` |
| Access method flags (`amcaninclude`) and GUC definitions | `src/ivfflat.c` |
| Planner hook registration (`_PG_init`) | `src/vector.c` |

---

## 9. Current Limitations

- **One custom scan per query:** A single scan cannot combine `IvfPrefilterScan` and `IvfIncludeScan` on the same child `IndexScan` (though `ScoreVectorPage` supports checking both pointers if set).
- **Single B-tree prefilter:** `IvfPrefilterScan` uses at most one B-tree index and does not intersect TID sets from multiple B-tree indexes. Other index types (Hash, GiST, GIN, BRIN) are not used as TID sources.
- **No `OR` clauses in B-tree prefilter:** `IvfPrefilterScan` does not push `OR` expressions (other than `IN` / `= ANY`), whereas `IvfIncludeScan` pushes any `AND` / `OR` combination over included columns.
- **Repeated chain walks:** `IvfIncludeScan` walks one column chain per leaf. Leaves on the same column are merged only when they sit directly under the same `AND` or `OR` node. So in `(category = 1 AND price < 10) OR (category = 2 AND price > 90)`, each chain can be read once per branch for each list.
- **No expressions in `IvfIncludeScan`:** Included columns must be plain table columns, and pushed clauses must reference the bare column `Var`. Expression filtering is supported only via `IvfPrefilterScan` with an expression B-tree.
- **No index-only scans:** `amcanreturn` is `NULL`. Included column values are used only for filtering; surviving rows are still fetched from the heap.
- **No parameterized or parallel scans:** Neither custom path wraps parameterized index scans (such as the inner side of a nested loop join passing outer variables) or uses parallel workers.
- **Requires `REINDEX` to add `INCLUDE`:** Version 1 indexes are not upgraded in place.
