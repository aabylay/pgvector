# IVFFlat Pre-Filter Scan — Implementation Summary

Status: implemented on branch `ivf-prefilter` (see commit *Added IVFFlat pre-filtering via IvfPrefilterScan*)  
Spec: [`filtered-ann-ivf-prefilter-customscan-spec.md`](filtered-ann-ivf-prefilter-customscan-spec.md)

---

## Problem

For filtered ANN queries of the form:

```sql
SELECT ... FROM items
WHERE p(x)
ORDER BY embedding <-> q
LIMIT k;
```

IVFFlat cannot take `WHERE` clauses as index quals (`amstrategies = 0`, no `amgetbitmap`). Postgres applies the predicate as a post-scan `Filter:` after distance computation and heap fetch. Under a restrictive filter (selectivity σ), the scan wastes roughly `1/σ` candidates per surviving row. That waste also makes the recall cure — probing more lists via `ivfflat.iterative_scan` — too expensive to use in practice.

## Solution

Evaluate the filter **before** distance work, using TID membership from an existing, unmodified core B-tree:

1. **Once per (re)scan**, drain matching heap TIDs from a B-tree on the filter column into an in-memory hash set.
2. **In `GetScanItems()`**, skip index tuples whose `t_tid` is not in the set before computing distance or sorting.
3. Keep the child’s `Filter` for correctness; every fallback is “run today’s plain IVF plan.”
4. Rely on iterative scan (`relaxed_order` + `max_probes`) for recall once probes are cheap.

No core Postgres patch, no catalog/on-disk format changes, no B-tree fork. The glue is a **CustomScan** provider entirely inside pgvector (`IvfPrefilterScan`), same shared library as the IVF AM — so the custom node can install a pointer into `IvfflatScanOpaqueData`.

Prior art: TimescaleDB SkipScan (CustomScan wrapping IndexScan); FAISS `IDSelector` (TID-set pre-filter).

## What we shipped

### New code

| File | Role |
| --- | --- |
| `src/ivfprefilter.c` / `.h` | Planner hook, B-tree clause matcher, cost model, `CustomPath` / `PlanCustomPath`, executor lifecycle, TID hash set (`simplehash`) |
| `src/ivfscan.c` | `so->prefilter` init; membership test in `GetScanItems`; `IvfflatScanSetPrefilter()` |
| `src/ivfflat.h` | Opaque field + include |
| `src/ivfflat.c` | GUCs `ivfflat.prefilter`, `ivfflat.prefilter_mem` |
| `src/vector.c` | `IvfflatPrefilterInit()` from `_PG_init` (hook + `RegisterCustomScanMethods`) |
| `Makefile` / `Makefile.win` | Build `ivfprefilter.o` |
| `test/sql/ivfflat_prefilter.sql` + expected | Regression coverage |
| `README.md` / `CHANGELOG.md` | User-facing docs |

### Planner

- `set_rel_pathlist_hook` (chained) offers a `CustomPath` wrapping each qualifying ordered IVFFlat `IndexPath`.
- Matches restriction clauses to a **preexisting** B-tree (prefix rule, opfamily/collation checks).
- v1 clause forms: `=`, `<`, `<=`, `>`, `>=`, `BETWEEN`, `IN` / `= ANY`, `IS NULL`; expression and `predOK` partial indexes.
- One B-tree per path; unmatched predicates stay on the child `Filter`.
- Bail out if estimated TID-set size exceeds `ivfflat.prefilter_mem` (default: `work_mem`).
- Cost = child IVF cost ± TID drain / lookups / skipped distances / saved heap fetches.
- Unparameterized paths only in v1; `parallel_safe = false`.

### Executor

- Build scan keys via `ExecIndexBuildScanKeys`; drain with `index_getnext_tid` (exact TIDs, not lossy bitmaps).
- Inject set into child opaque before first pull; ownership stays with the custom node across `ivfflatrescan`.
- Fallbacks (never error): memory budget, missing/invalid B-tree, GUC off on cached plan, EPQ skip.
- EXPLAIN: `Prefilter`, `Prefilter Index`, and with ANALYZE: `Prefilter TIDs`, `Prefilter Rebuilds`, `Prefilter Fallback`.

### Configuration

```sql
SET ivfflat.prefilter = on;          -- default on
SET ivfflat.prefilter_mem = -1;      -- default: use work_mem (kB)
```

Recommended with iterative scan for recall:

```sql
SET ivfflat.iterative_scan = relaxed_order;
SET ivfflat.max_probes = 100;
```

## Measured behavior (summary)

On a ~200k-row, `vector(64)`, `lists=100`, `probes=10` workload:

- **Forced IVF on vs off:** restrictive/mid filters saw large runtime wins (often ~66–94%) from avoiding wasted filter removals; unfiltered scans unchanged.
- **Natural planner:** very small σ correctly prefers exact B-tree + sort; mid-band prefers `IvfPrefilterScan`.
- **Known cost-model gap:** around σ ≈ 0.005–0.01 the planner can prefer Custom Scan while exact B-tree + sort is slightly faster (cost underprices IVF / overprices exact). Spec risk §14.1.
- **Correctness:** result equivalence with prefilter on/off under full probes; memory fallback and DROP INDEX invalidation behave as specified.

Full `installcheck`: all tests passed, including `ivfflat_prefilter`.

## Out of scope / Phase 2+

Documented in [`filtered-ann-ivf-prefilter-v2.md`](filtered-ann-ivf-prefilter-v2.md):

- Multi-B-tree TID intersection; hash/GIN/BRIN sources with strict lossy rejection
- Parameterized paths (nested-loop inner side)
- Cost-model refinement ([`filtered-ann-cost-model-proposal.md`](filtered-ann-cost-model-proposal.md))
- HNSW filtered traversal (separate design)
- Possible upstream “AM accepts filter TID set” API to retire CustomScan plumbing

## Does this deserve a paper?

**Short answer: a workshop / systems note or industry experience paper, yes; a top-tier research paper on novelty alone, probably not.**

### What is publishable

- **Systems contribution in a real engine:** closing the “filtered IVF is too expensive to iterate” loop inside PostgreSQL/pgvector without forking B-tree or patching core, via CustomScan + public index AM APIs.
- **Engineering depth:** planner matching, cost competition with exact plans, fallback discipline, EPQ/rescan/plan-cache, observability — the kind of end-to-end integration story SIGMOD/VLDB industrial tracks and workshops (e.g. AIDB, DBML, Postgres-focused venues) care about.
- **Empirical angle:** when prefilter + iterative probes beats post-filter IVF and when exact B-tree + sort still wins; cost-model misplan band; recall/latency tradeoffs vs probe escalation.

### What is *not* novel as algorithms

- Pre-filtering ANN with an ID/TID set is standard (FAISS `IDSelector`, many vector DBs).
- IVF + metadata filter is a known product pattern.
- CustomScan wrapping another scan has precedent (SkipScan).

So the claim is not “we invented filtered search,” but “here is a correct, extension-only way to make filtered IVFFlat practical in Postgres, with planner integration and measured tradeoffs.”

### Stronger paper path (if pursuing)

1. Broad benchmarks (dimensions, N, σ, k, lists/probes) vs post-filter IVF, exact, and maybe HNSW.
2. Recall–latency curves with/without prefilter under iterative scan.
3. Honest cost-model analysis and a refined model (ties to the cost-model proposal doc).
4. Optional: comparison to partial IVF indexes / partitioning workarounds.
5. Position as **Postgres/pgvector systems + evaluation**, not pure ANN theory.

**Verdict:** Worth writing up as an implementation/experience report or workshop paper; with stronger eval and cost-model work it could fit an industrial track. Pure novelty is modest — the value is integration, correctness under Postgres executor semantics, and making iterative filtered IVF usable in practice.
