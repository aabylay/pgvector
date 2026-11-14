/*
 * IVF Pre-Filter Scan: a CustomScan over an unmodified core B-tree index
 *
 * For SELECT ... WHERE p(x) ORDER BY embedding <-> q LIMIT k over an ivfflat
 * index, this node evaluates the predicate *before* the distance computation:
 * once per (re)scan it drains an existing B-tree index on the filter column
 * through the public index AM API, collects the matching heap TIDs into an
 * in-memory hash set, and installs a pointer to that set in the child ivfflat
 * index scan's opaque state.  GetScanItems() then skips index tuples whose
 * TID is not in the set, before computing distances or heap fetches.
 *
 * The design is a "pure optimization": the child Index Scan keeps the full
 * Filter qual, so every fallback path (memory budget exceeded, index dropped,
 * GUC disabled, EvalPlanQual) is simply "run the child unmodified", which is
 * exactly the plan the planner builds today.
 *
 * See docs/filtered-ivf.md for the full design.
 */
#include "postgres.h"

#include <math.h>

#include "access/genam.h"
#include "access/relation.h"
#include "access/relscan.h"
#include "access/table.h"
#include "catalog/pg_am_d.h"
#include "commands/defrem.h"
#include "commands/explain.h"
#include "common/hashfn.h"
#include "executor/executor.h"
#include "executor/nodeIndexscan.h"
#include "ivfflat.h"
#include "miscadmin.h"
#include "nodes/extensible.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/plancat.h"
#include "optimizer/restrictinfo.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/ruleutils.h"
#include "utils/spccache.h"

#if PG_VERSION_NUM >= 180000
#include "commands/explain_format.h"
#include "commands/explain_state.h"
#endif

/*
 * Amortized bytes per TID-set entry (hash entry plus table slack).  Shared
 * between the planner's eligibility threshold and the runtime budget.
 */
#define PREFILTER_ENTRY_BYTES 16

/* Check for interrupts every this many drained TIDs */
#define PREFILTER_INTERRUPT_INTERVAL 1024

/* GUCs (registered in IvfflatInit) */
bool		ivfflat_prefilter;
int			ivfflat_prefilter_mem;

static set_rel_pathlist_hook_type prev_set_rel_pathlist_hook = NULL;

/*
 * The TID set: simplehash keyed by the TID packed into a uint64.
 * ip_posid is a 16-bit OffsetNumber, so the packing is lossless.
 */
typedef struct TidSetEntry
{
	uint64		key;
	char		status;
}			TidSetEntry;

static inline uint32
TidSetHashKey(uint64 key)
{
	return murmurhash32((uint32) key) ^ murmurhash32((uint32) (key >> 32));
}

#define SH_PREFIX tidset
#define SH_ELEMENT_TYPE TidSetEntry
#define SH_KEY_TYPE uint64
#define SH_KEY key
#define SH_HASH_KEY(tb, key) TidSetHashKey(key)
#define SH_EQUAL(tb, a, b) ((a) == (b))
#define SH_SCOPE static inline
#define SH_DECLARE
#define SH_DEFINE
#include "lib/simplehash.h"

struct IvfflatPrefilter
{
	tidset_hash *set;
	uint64		maxEntries;
};

static inline uint64
TidToKey(ItemPointer tid)
{
	return ((uint64) ItemPointerGetBlockNumberNoCheck(tid) << 16) |
		(uint64) ItemPointerGetOffsetNumberNoCheck(tid);
}

/*
 * Create a TID set constrained to memBudget bytes
 */
IvfflatPrefilter *
IvfflatPrefilterCreate(MemoryContext cxt, Size memBudget)
{
	IvfflatPrefilter *pf;
	MemoryContext oldCtx = MemoryContextSwitchTo(cxt);

	pf = palloc_object(IvfflatPrefilter);
	pf->maxEntries = memBudget / PREFILTER_ENTRY_BYTES;
	pf->set = tidset_create(cxt, 1024, NULL);

	MemoryContextSwitchTo(oldCtx);

	return pf;
}

/*
 * Add a TID; returns false when the memory budget is hit
 */
bool
IvfflatPrefilterAdd(IvfflatPrefilter * pf, ItemPointer tid)
{
	bool		found;

	if (pf->set->members >= pf->maxEntries)
		return false;

	tidset_insert(pf->set, TidToKey(tid), &found);

	return true;
}

/*
 * Test TID membership (hot path: called once per index tuple in probed lists)
 */
bool
IvfflatPrefilterContains(const IvfflatPrefilter * pf, ItemPointer tid)
{
	return tidset_lookup(((IvfflatPrefilter *) pf)->set, TidToKey(tid)) != NULL;
}

/*
 * Free a TID set
 */
void
IvfflatPrefilterFree(IvfflatPrefilter * pf)
{
	tidset_destroy(pf->set);
	pfree(pf);
}

/*
 * Number of entries in the set
 */
int64
IvfflatPrefilterCount(const IvfflatPrefilter * pf)
{
	return (int64) pf->set->members;
}

/*
 * Runtime memory budget for the TID set, in bytes
 */
static Size
PrefilterMemBudget(void)
{
	int			kb = ivfflat_prefilter_mem < 0 ? work_mem : ivfflat_prefilter_mem;

	return (Size) kb * 1024;
}

/*
 * Path-time private info; lives in CustomPath.custom_private (paths are
 * never copied or serialized, so a bare pointer is fine)
 */
typedef struct IvfPrefilterPathInfo
{
	IndexOptInfo *btidx;		/* chosen B-tree index */
	List	   *rinfos;			/* matched RestrictInfos */
	List	   *indexcols;		/* int list: matched B-tree column (0-based) */
	List	   *commuted;		/* bool list: does the clause need commuting? */
	Selectivity sel;			/* selectivity of matched clauses */
}			IvfPrefilterPathInfo;

/*
 * Executor node state
 */
typedef struct IvfPrefilterScanState
{
	CustomScanState css;		/* first field; css.custom_ps[0] = child
								 * IndexScanState */

	/* B-tree side */
	Oid			btindexoid;
	Relation	btindex;		/* opened in Begin, AccessShareLock */
	ScanKey		scanKeys;
	int			numScanKeys;
	IndexRuntimeKeyInfo *runtimeKeys;
	int			numRuntimeKeys;
	bool		runtimeKeysReady;
	ExprContext *runtimeContext;	/* not reset per tuple */

	/* TID set */
	IvfflatPrefilter *prefilter;	/* NULL until built */
	MemoryContext prefilterCtx; /* reset per rescan */
	bool		setBuilt;		/* build attempted for current params */
	bool		injected;		/* pointer installed into child scan opaque */

	/* instrumentation for EXPLAIN ANALYZE */
	int64		ntids;			/* entries in set */
	int64		nbuilds;		/* build attempts (first build + rebuilds) */
	const char *fallbackReason; /* NULL | "memory" | "index" | "disabled" */
}			IvfPrefilterScanState;

static Plan *IvfPrefilterPlanCustomPath(PlannerInfo *root, RelOptInfo *rel,
										CustomPath *best_path, List *tlist,
										List *clauses, List *custom_plans);
static Node *IvfPrefilterCreateScanState(CustomScan *cscan);
static void IvfPrefilterBegin(CustomScanState *css, EState *estate, int eflags);
static TupleTableSlot *IvfPrefilterExec(CustomScanState *css);
static void IvfPrefilterEnd(CustomScanState *css);
static void IvfPrefilterReScan(CustomScanState *css);
static void IvfPrefilterExplain(CustomScanState *css, List *ancestors,
								ExplainState *es);

static const CustomPathMethods ivf_prefilter_path_methods = {
	.CustomName = "IvfPrefilterScan",
	.PlanCustomPath = IvfPrefilterPlanCustomPath,
	.ReparameterizeCustomPathByChild = NULL,
};

static CustomScanMethods ivf_prefilter_scan_methods = {
	.CustomName = "IvfPrefilterScan",
	.CreateCustomScanState = IvfPrefilterCreateScanState,
};

static const CustomExecMethods ivf_prefilter_exec_methods = {
	.CustomName = "IvfPrefilterScan",
	.BeginCustomScan = IvfPrefilterBegin,
	.ExecCustomScan = IvfPrefilterExec,
	.EndCustomScan = IvfPrefilterEnd,
	.ReScanCustomScan = IvfPrefilterReScan,
	.MarkPosCustomScan = NULL,
	.RestrPosCustomScan = NULL,
	.EstimateDSMCustomScan = NULL,
	.InitializeDSMCustomScan = NULL,
	.ReInitializeDSMCustomScan = NULL,
	.InitializeWorkerCustomScan = NULL,
	.ShutdownCustomScan = NULL,
	.ExplainCustomScan = IvfPrefilterExplain,
};

/*
 * Get the ivfflat AM OID, caching positive lookups (AM OIDs are stable for
 * a session; a negative result can become positive after CREATE EXTENSION)
 */
static Oid
IvfflatAmOid(void)
{
	static Oid	amoid = InvalidOid;

	if (!OidIsValid(amoid))
		amoid = get_index_am_oid("ivfflat", true);

	return amoid;
}

/*
 * Match one restriction clause against one B-tree index column.
 * Accepted forms (see spec §3.3): OpExpr indexkey OP pseudoconstant,
 * ScalarArrayOpExpr indexkey = ANY (array), NullTest indexkey IS NULL.
 */
static bool
MatchClauseToBtreeColumn(IndexOptInfo *btidx, int col, RestrictInfo *rinfo,
						 bool *commute)
{
	Expr	   *clause = rinfo->clause;
	Oid			opfamily = btidx->opfamily[col];
	Oid			idxcollation = btidx->indexcollations[col];

	if (rinfo->pseudoconstant)
		return false;

	/* Boolean OR/AND/NOT trees cannot become single-scan B-tree keys */
	if (IsA(clause, BoolExpr))
		return false;

	*commute = false;

	if (IsA(clause, OpExpr))
	{
		OpExpr	   *op = (OpExpr *) clause;
		Node	   *leftop;
		Node	   *rightop;
		Node	   *other;
		Oid			opno;

		if (list_length(op->args) != 2)
			return false;

		leftop = (Node *) linitial(op->args);
		rightop = (Node *) lsecond(op->args);

		if (match_index_to_operand(leftop, col, btidx))
		{
			other = rightop;
			opno = op->opno;
		}
		else if (match_index_to_operand(rightop, col, btidx))
		{
			other = leftop;
			opno = get_commutator(op->opno);
			if (!OidIsValid(opno))
				return false;
			*commute = true;
		}
		else
			return false;

		/*
		 * The comparison side must be pseudoconstant relative to this rel:
		 * Consts, Params, and stable expressions over them are all fine
		 */
		if (contain_var_clause(other) ||
			contain_volatile_functions(other) ||
			contain_subplans(other))
			return false;

		if (!op_in_opfamily(opno, opfamily))
			return false;

		if (OidIsValid(idxcollation) && idxcollation != op->inputcollid)
			return false;

		return true;
	}
	else if (IsA(clause, ScalarArrayOpExpr))
	{
		ScalarArrayOpExpr *saop = (ScalarArrayOpExpr *) clause;
		Node	   *leftop = (Node *) linitial(saop->args);
		Node	   *arraynode = (Node *) lsecond(saop->args);

		if (!saop->useOr)
			return false;

		/* SAOP index form requires the index key on the left */
		if (!match_index_to_operand(leftop, col, btidx))
			return false;

		if (contain_var_clause(arraynode) ||
			contain_volatile_functions(arraynode) ||
			contain_subplans(arraynode))
			return false;

		if (!op_in_opfamily(saop->opno, opfamily))
			return false;

		if (OidIsValid(idxcollation) && idxcollation != saop->inputcollid)
			return false;

		return true;
	}
	else if (IsA(clause, NullTest))
	{
		NullTest   *ntest = (NullTest *) clause;

		/* IS NOT NULL is near-useless as a prefilter; skip (spec §3.3) */
		if (ntest->nulltesttype != IS_NULL || ntest->argisrow)
			return false;

		if (!match_index_to_operand((Node *) ntest->arg, col, btidx))
			return false;

		return true;
	}

	return false;
}

/*
 * Match the rel's restriction clauses against one B-tree index.
 * Returns NULL if the index is not usable (including the prefix rule:
 * at least one accepted clause on column 0).
 */
static IvfPrefilterPathInfo *
MatchBtreeIndex(PlannerInfo *root, RelOptInfo *rel, IndexOptInfo *btidx)
{
	IvfPrefilterPathInfo *info;
	List	   *rinfos = NIL;
	List	   *indexcols = NIL;
	List	   *commuted = NIL;
	bool		matchedCol0 = false;
	ListCell   *lc;

	if (btidx->relam != BTREE_AM_OID)
		return NULL;
	if (btidx->hypothetical)
		return NULL;
	/* Partial B-trees are valid TID sources only when predOK */
	if (btidx->indpred != NIL && !btidx->predOK)
		return NULL;

	foreach(lc, rel->baserestrictinfo)
	{
		RestrictInfo *rinfo = (RestrictInfo *) lfirst(lc);

		/* The B-tree scan runs before row-level security quals */
		if (!restriction_is_securely_promotable(rinfo, rel))
			continue;

		/* Take the first column each clause matches */
		for (int col = 0; col < btidx->nkeycolumns; col++)
		{
			bool		commute;

			if (MatchClauseToBtreeColumn(btidx, col, rinfo, &commute))
			{
				rinfos = lappend(rinfos, rinfo);
				indexcols = lappend_int(indexcols, col);
				commuted = lappend_int(commuted, (int) commute);

				if (col == 0)
					matchedCol0 = true;

				break;
			}
		}
	}

	/*
	 * Prefix rule: without a clause on column 0, nbtree would scan the whole
	 * index; not a candidate
	 */
	if (!matchedCol0)
		return NULL;

	info = palloc_object(IvfPrefilterPathInfo);
	info->btidx = btidx;
	info->rinfos = rinfos;
	info->indexcols = indexcols;
	info->commuted = commuted;
	info->sel = clauselist_selectivity(root, rinfos, 0, JOIN_INNER, NULL);

	return info;
}

/*
 * Pick the best usable B-tree index: minimize estimated drained entries
 * (sel × tuples), tie-broken by fewer index pages
 */
static IvfPrefilterPathInfo *
ChooseBtreeIndex(PlannerInfo *root, RelOptInfo *rel)
{
	IvfPrefilterPathInfo *best = NULL;
	ListCell   *lc;

	foreach(lc, rel->indexlist)
	{
		IndexOptInfo *btidx = (IndexOptInfo *) lfirst(lc);
		IvfPrefilterPathInfo *info = MatchBtreeIndex(root, rel, btidx);

		if (info == NULL)
			continue;

		if (best == NULL ||
			info->sel < best->sel ||
			(info->sel == best->sel && info->btidx->pages < best->btidx->pages))
			best = info;
	}

	return best;
}

/*
 * Cost the custom path by explicit adjustment of the child path's cost
 * (spec §3.5)
 */
static void
CostPrefilterPath(PlannerInfo *root, RelOptInfo *rel, CustomPath *cpath,
				  IndexPath *ivfpath, IvfPrefilterPathInfo *info)
{
	double		N = Max(rel->tuples, 1.0);
	double		sel = info->sel;
	double		T;
	double		ratio;
	int			lists = 1;
	int			dimensions = 0;
	Relation	index;
	double		btRandomPageCost;
	double		heapRandomPageCost;
	Cost		tidsetCost;
	Cost		lookupCost;
	Cost		distSaved;
	Cost		sortSaved;
	Cost		fetchSaved;
	double		prefEntries;
	double		fullSort;
	double		prefSort;
	double		pulled;
	double		plainFetches;
	Cost		startup;
	Cost		total;

	/* Read lists and dimensions from the metapage, like ivfflatcostestimate */
	index = index_open(ivfpath->indexinfo->indexoid, NoLock);
	IvfflatGetMetaPageInfo(index, &lists, &dimensions);
	index_close(index, NoLock);

	ratio = ((double) ivfflat_probes) / Max(lists, 1);
	if (ratio > 1.0)
		ratio = 1.0;
	T = N * ratio;

	get_tablespace_page_costs(info->btidx->reltablespace, &btRandomPageCost, NULL);
	get_tablespace_page_costs(rel->reltablespace, &heapRandomPageCost, NULL);

	/* B-tree descent + drain (CPU per entry + hash insert) + leaf page I/O */
	tidsetCost = log(N) / log(2.0) * cpu_operator_cost
		+ sel * N * (cpu_index_tuple_cost + cpu_operator_cost)
		+ ceil(sel * info->btidx->pages) * btRandomPageCost;

	/* One hash probe per tuple in probed lists */
	lookupCost = T * cpu_operator_cost;

	/* Distance computations skipped for rejected tuples; w(d) = d × cpu_operator_cost */
	distSaved = T * (1.0 - sel) * Max(dimensions, 1) * cpu_operator_cost;

	/* Tuplesort comparison delta: sel×T entries instead of T */
	prefEntries = sel * T;
	fullSort = T * (log(Max(T, 2.0)) / log(2.0));
	prefSort = prefEntries * (log(Max(prefEntries, 2.0)) / log(2.0));
	sortSaved = 2.0 * cpu_operator_cost * (fullSort - prefSort);
	if (sortSaved < 0)
		sortSaved = 0;

	/*
	 * Heap fetches the plain plan wastes: to surface `pulled` rows past the
	 * Filter it fetches pulled/sel candidates (capped at the tuples scanned).
	 * The 0.5 fudge on random_page_cost reflects partial buffer-cache hits.
	 */
	pulled = cpath->path.rows;
	if (root->tuple_fraction > 0.0)
	{
		if (root->tuple_fraction < 1.0)
			pulled = cpath->path.rows * root->tuple_fraction;
		else
			pulled = Min(cpath->path.rows, root->tuple_fraction);
	}
	pulled = clamp_row_est(pulled);
	plainFetches = Min(T, pulled / Max(sel, 1.0e-6));
	fetchSaved = (plainFetches - pulled) * (heapRandomPageCost * 0.5 + cpu_tuple_cost);
	if (fetchSaved < 0)
		fetchSaved = 0;

	startup = ivfpath->path.startup_cost + tidsetCost + lookupCost - distSaved - sortSaved;
	total = ivfpath->path.total_cost + tidsetCost + lookupCost - distSaved - sortSaved - fetchSaved;

	if (startup < 1.0e-10)
		startup = 1.0e-10;
	if (total < startup)
		total = startup;

	cpath->path.startup_cost = startup;
	cpath->path.total_cost = total;
}

/*
 * Build and add one CustomPath wrapping the given ivfflat IndexPath
 */
static void
AddPrefilterPath(PlannerInfo *root, RelOptInfo *rel, IndexPath *ivfpath,
				 IvfPrefilterPathInfo *info)
{
	CustomPath *cpath = makeNode(CustomPath);

	cpath->path.pathtype = T_CustomScan;
	cpath->path.parent = rel;
	cpath->path.pathtarget = rel->reltarget;
	cpath->path.param_info = NULL;	/* v1: unparameterized only */
	cpath->path.parallel_safe = false;	/* ivfflat has no parallel scan */
	cpath->path.parallel_aware = false;
	cpath->path.parallel_workers = 0;
	cpath->path.rows = ivfpath->path.rows;	/* rows after all restrictions */
	cpath->path.pathkeys = ivfpath->path.pathkeys;	/* forwards child order 1:1 */
#if PG_VERSION_NUM >= 180000
	cpath->path.disabled_nodes = ivfpath->path.disabled_nodes;
#endif
	cpath->flags = 0;			/* no backward scan, no mark/restore */
#ifdef CUSTOMPATH_SUPPORT_PROJECTION
	/* the executor projects generically from the child's physical tuples */
	cpath->flags |= CUSTOMPATH_SUPPORT_PROJECTION;
#endif
	cpath->custom_paths = list_make1(ivfpath);	/* child = stock ivfflat path */
	cpath->custom_private = list_make1(info);
	cpath->methods = &ivf_prefilter_path_methods;

	CostPrefilterPath(root, rel, cpath, ivfpath, info);

	add_path(rel, (Path *) cpath);
}

/*
 * set_rel_pathlist hook: offer an IvfPrefilterScan path for each qualifying
 * ivfflat ordered IndexPath (spec §3.2)
 */
static void
IvfflatPrefilterPathlistHook(PlannerInfo *root, RelOptInfo *rel, Index rti,
							 RangeTblEntry *rte)
{
	Oid			amoid;
	List	   *ivfpaths = NIL;
	IvfPrefilterPathInfo *info;
	Size		budget;
	ListCell   *lc;

	if (prev_set_rel_pathlist_hook)
		prev_set_rel_pathlist_hook(root, rel, rti, rte);

	if (!ivfflat_prefilter)
		return;

	if (rte->rtekind != RTE_RELATION || rte->tablesample != NULL)
		return;

	if (rte->relkind != RELKIND_RELATION && rte->relkind != RELKIND_MATVIEW)
		return;

	if (rel->reloptkind != RELOPT_BASEREL &&
		rel->reloptkind != RELOPT_OTHER_MEMBER_REL)
		return;

	/* Something to prefilter? */
	if (rel->baserestrictinfo == NIL)
		return;

	if (rel->indexlist == NIL)
		return;

	amoid = IvfflatAmOid();
	if (!OidIsValid(amoid))
		return;

	/*
	 * Collect qualifying ivfflat ordered paths first: add_path (called while
	 * building custom paths) may modify rel->pathlist
	 */
	foreach(lc, rel->pathlist)
	{
		Path	   *path = (Path *) lfirst(lc);
		IndexPath  *ipath;

		if (!IsA(path, IndexPath))
			continue;

		ipath = (IndexPath *) path;

		if (ipath->indexinfo->relam != amoid)
			continue;
		if (ipath->indexorderbys == NIL)
			continue;
		if (ipath->path.param_info != NULL)
			continue;			/* v1: unparameterized only */

		ivfpaths = lappend(ivfpaths, ipath);
	}

	if (ivfpaths == NIL)
		return;

	/*
	 * The plan relies on the child Index Scan emitting physical heap tuples
	 * (build_physical_tlist, §4.2); that is not possible for relations with
	 * dropped or missing-value attributes, so skip them entirely
	 */
	{
		Relation	heapRel = table_open(rte->relid, NoLock);
		TupleDesc	tupdesc = RelationGetDescr(heapRel);
		bool		usable = true;

		for (int i = 0; i < tupdesc->natts; i++)
		{
			Form_pg_attribute att = TupleDescAttr(tupdesc, i);

			if (att->attisdropped || att->atthasmissing)
			{
				usable = false;
				break;
			}
		}

		table_close(heapRel, NoLock);

		if (!usable)
			return;
	}

	/* Find a usable B-tree TID source; the match is child-path independent */
	info = ChooseBtreeIndex(root, rel);
	if (info == NULL)
		return;

	/*
	 * Bail-out threshold: with a broad filter, today's post-filter plan or
	 * the planner's bitmap/sort plan is the right answer; don't even enter
	 * the cost competition
	 */
	budget = PrefilterMemBudget();
	if (info->sel * rel->tuples * PREFILTER_ENTRY_BYTES > (double) budget)
		return;

	foreach(lc, ivfpaths)
		AddPrefilterPath(root, rel, (IndexPath *) lfirst(lc), info);
}

/*
 * Replace the indexed operand with an INDEX_VAR Var, the same way core's
 * fix_indexqual_operand does (spec §4.2)
 */
static Node *
FixBtreeQualOperand(Node *node, int indexcol)
{
	if (node && IsA(node, RelabelType))
		node = (Node *) ((RelabelType *) node)->arg;

	return (Node *) makeVar(INDEX_VAR, indexcol + 1, exprType(node),
							exprTypmod(node), exprCollation(node), 0);
}

/*
 * Rewrite one matched clause into index-qual form for
 * ExecIndexBuildScanKeys: index key strictly on the left, replaced by an
 * INDEX_VAR Var.  The clause is a fresh copy; modify in place.
 */
static Node *
FixBtreeQual(Node *clause, int indexcol, bool commute)
{
	if (IsA(clause, OpExpr))
	{
		OpExpr	   *op = (OpExpr *) clause;

		if (commute)
		{
			Oid			commutator = get_commutator(op->opno);
			Node	   *temp;

			Assert(OidIsValid(commutator));
			op->opno = commutator;
			op->opfuncid = get_opcode(commutator);

			temp = (Node *) linitial(op->args);
			linitial(op->args) = lsecond(op->args);
			lsecond(op->args) = temp;
		}

		linitial(op->args) = FixBtreeQualOperand((Node *) linitial(op->args), indexcol);
	}
	else if (IsA(clause, ScalarArrayOpExpr))
	{
		ScalarArrayOpExpr *saop = (ScalarArrayOpExpr *) clause;

		linitial(saop->args) = FixBtreeQualOperand((Node *) linitial(saop->args), indexcol);
	}
	else if (IsA(clause, NullTest))
	{
		NullTest   *ntest = (NullTest *) clause;

		ntest->arg = (Expr *) FixBtreeQualOperand((Node *) ntest->arg, indexcol);
	}
	else
		elog(ERROR, "unexpected prefilter clause type: %d", (int) nodeTag(clause));

	return clause;
}

/*
 * PlanCustomPath: turn the CustomPath into a CustomScan plan node.
 * custom_plans already contains the finished child IndexScan (including its
 * qpqual Filter).
 */
static Plan *
IvfPrefilterPlanCustomPath(PlannerInfo *root, RelOptInfo *rel,
						   CustomPath *best_path, List *tlist,
						   List *clauses, List *custom_plans)
{
	IvfPrefilterPathInfo *info = (IvfPrefilterPathInfo *) linitial(best_path->custom_private);
	CustomScan *cscan = makeNode(CustomScan);
	Plan	   *childPlan = (Plan *) linitial(custom_plans);
	List	   *physTlist;
	List	   *indexQuals = NIL;
	List	   *origQuals = NIL;
	ListCell   *lc1;
	ListCell   *lc2;
	ListCell   *lc3;

	/*
	 * Force the child Index Scan to emit physical heap tuples: the custom
	 * node forwards the child's slots (and projects from them when the
	 * planner gave us a narrower tlist), which is only sound when the child
	 * never projects and its output layout equals the relation's.  The hook
	 * refused to build paths for relations where a physical tlist is not
	 * available (dropped/missing attributes).
	 */
	physTlist = build_physical_tlist(root, rel);
	if (physTlist == NIL)		/* shouldn't happen; see hook check */
		elog(ERROR, "could not build physical tlist for prefilter scan");
	childPlan->targetlist = physTlist;

	forthree(lc1, info->rinfos, lc2, info->indexcols, lc3, info->commuted)
	{
		RestrictInfo *rinfo = (RestrictInfo *) lfirst(lc1);
		int			indexcol = lfirst_int(lc2);
		bool		commute = (bool) lfirst_int(lc3);

		/* Original form, for EXPLAIN */
		origQuals = lappend(origQuals, copyObject(rinfo->clause));

		/* Index-qual form, for ExecIndexBuildScanKeys */
		indexQuals = lappend(indexQuals,
							 FixBtreeQual((Node *) copyObject(rinfo->clause),
										  indexcol, commute));
	}

	cscan->scan.plan.targetlist = tlist;
	cscan->scan.plan.qual = NIL;	/* the child keeps the Filter */
	cscan->scan.scanrelid = rel->relid;
	cscan->flags = best_path->flags;
	cscan->custom_plans = custom_plans; /* [ IndexScan(ivfflat) ] */

	/*
	 * Both sublists go through setrefs' fix_scan_expr; INDEX_VAR Vars are
	 * left untouched, matching how indexqual lists behave
	 */
	cscan->custom_exprs = list_make2(indexQuals, origQuals);
	cscan->custom_private = list_make1(makeInteger((int) info->btidx->indexoid));
	cscan->custom_scan_tlist = NIL;
	cscan->methods = &ivf_prefilter_scan_methods;

	/*
	 * Plan-cache invalidation: core records IndexScan.indexid dependencies,
	 * but knows nothing about an OID buried in custom_private
	 */
	root->glob->relationOids = lappend_oid(root->glob->relationOids,
										   info->btidx->indexoid);

	return (Plan *) cscan;
}

/*
 * CreateCustomScanState
 */
static Node *
IvfPrefilterCreateScanState(CustomScan *cscan)
{
	IvfPrefilterScanState *node;

	node = (IvfPrefilterScanState *) newNode(sizeof(IvfPrefilterScanState),
											 T_CustomScanState);
	node->css.methods = &ivf_prefilter_exec_methods;

	return (Node *) node;
}

/*
 * BeginCustomScan
 */
static void
IvfPrefilterBegin(CustomScanState *css, EState *estate, int eflags)
{
	IvfPrefilterScanState *node = (IvfPrefilterScanState *) css;
	CustomScan *cscan = (CustomScan *) css->ss.ps.plan;
	List	   *indexQuals = (List *) linitial(cscan->custom_exprs);
	Relation	btindex;

	node->btindexoid = (Oid) intVal(linitial(cscan->custom_private));
	node->btindex = NULL;
	node->scanKeys = NULL;
	node->numScanKeys = 0;
	node->runtimeKeys = NULL;
	node->numRuntimeKeys = 0;
	node->runtimeKeysReady = false;
	node->runtimeContext = NULL;
	node->prefilter = NULL;
	node->prefilterCtx = NULL;
	node->setBuilt = false;
	node->injected = false;
	node->ntids = 0;
	node->nbuilds = 0;
	node->fallbackReason = NULL;

	/* Initialize the child ivfflat IndexScan */
	css->custom_ps = list_make1(ExecInitNode((Plan *) linitial(cscan->custom_plans),
											 estate, eflags));

	/*
	 * If the planner gave us a tlist narrower than the scan tuple, core has
	 * compiled a projection specialized to this node's scan slot type
	 * (buffer-heap).  We feed it the child's returned slot instead, which is
	 * usually buffer-heap but can be virtual (e.g. the EvalPlanQual test
	 * tuple), so recompile the projection without slot-type assumptions.
	 * The attribute numbers line up because the child emits physical tuples.
	 */
	if (css->ss.ps.ps_ProjInfo != NULL)
	{
		css->ss.ps.scanopsfixed = false;
		css->ss.ps.ps_ProjInfo =
			ExecBuildProjectionInfo(cscan->scan.plan.targetlist,
									css->ss.ps.ps_ExprContext,
									css->ss.ps.ps_ResultTupleSlot,
									&css->ss.ps,
									RelationGetDescr(css->ss.ss_currentRelation));
	}

	/* No B-tree work for EXPLAIN or an EvalPlanQual recheck subtree */
	if (eflags & EXEC_FLAG_EXPLAIN_ONLY)
		return;
#if PG_VERSION_NUM >= 140000
	if (estate->es_epq_active != NULL)
		return;
#else
	if (estate->es_epqTuple != NULL)
		return;
#endif

	/*
	 * Open the B-tree defensively: a cached plan is invalidated when the
	 * index is dropped, but fall back to no-prefilter rather than erroring
	 * if anything is off (spec §5.8)
	 */
	btindex = try_relation_open(node->btindexoid, AccessShareLock);
	if (btindex == NULL ||
		btindex->rd_rel->relkind != RELKIND_INDEX ||
		btindex->rd_rel->relam != BTREE_AM_OID ||
		!btindex->rd_index->indisvalid)
	{
		if (btindex != NULL)
			relation_close(btindex, AccessShareLock);
		node->fallbackReason = "index";
		return;
	}
	node->btindex = btindex;

	/* Build scan keys once; runtime keys are evaluated at first exec */
	ExecIndexBuildScanKeys((PlanState *) node, node->btindex, indexQuals, false,
						   &node->scanKeys, &node->numScanKeys,
						   &node->runtimeKeys, &node->numRuntimeKeys,
						   NULL, NULL);

	/*
	 * Runtime keys need an ExprContext that is not reset per tuple; borrow
	 * the swap trick from ExecInitIndexScan
	 */
	if (node->numRuntimeKeys != 0)
	{
		ExprContext *stdecontext = css->ss.ps.ps_ExprContext;

		ExecAssignExprContext(estate, &css->ss.ps);
		node->runtimeContext = css->ss.ps.ps_ExprContext;
		css->ss.ps.ps_ExprContext = stdecontext;
	}

	node->prefilterCtx = AllocSetContextCreate(CurrentMemoryContext,
											   "IvfPrefilterScan TID set",
											   ALLOCSET_DEFAULT_SIZES);

	/*
	 * Do not build the TID set here: runtime keys may not be evaluable yet
	 * (InitPlan params), and no work should happen for plans that are
	 * initialized but never executed
	 */
}

/*
 * Drain the B-tree into the TID set (spec §5.3)
 */
static void
BuildTidSet(IvfPrefilterScanState *node)
{
	EState	   *estate = node->css.ss.ps.state;
	IndexScanDesc scan;
	ItemPointer tid;
	int64		drained = 0;

	node->nbuilds++;

	/* Cached plan may predate ivfflat.prefilter = off */
	if (!ivfflat_prefilter)
	{
		node->fallbackReason = "disabled";
		return;
	}

	if (node->btindex == NULL)
		return;					/* fallbackReason already set in Begin */

	if (node->numRuntimeKeys != 0 && !node->runtimeKeysReady)
	{
		ResetExprContext(node->runtimeContext);
		ExecIndexEvalRuntimeKeys(node->runtimeContext, node->runtimeKeys,
								 node->numRuntimeKeys);
		node->runtimeKeysReady = true;
	}

	node->prefilter = IvfflatPrefilterCreate(node->prefilterCtx, PrefilterMemBudget());

	/*
	 * Same snapshot as the child's ivfflat scan and heap fetches.  TIDs of
	 * dead heap versions land in the set harmlessly (cost, not correctness);
	 * we never fetch heap tuples here.
	 */
#if PG_VERSION_NUM >= 180000
	scan = index_beginscan(node->css.ss.ss_currentRelation, node->btindex,
						   estate->es_snapshot, NULL, node->numScanKeys, 0);
#else
	scan = index_beginscan(node->css.ss.ss_currentRelation, node->btindex,
						   estate->es_snapshot, node->numScanKeys, 0);
#endif
	index_rescan(scan, node->scanKeys, node->numScanKeys, NULL, 0);

	while ((tid = index_getnext_tid(scan, ForwardScanDirection)) != NULL)
	{
		if ((++drained % PREFILTER_INTERRUPT_INTERVAL) == 0)
			CHECK_FOR_INTERRUPTS();

		if (!IvfflatPrefilterAdd(node->prefilter, tid))
		{
			node->fallbackReason = "memory";
			node->prefilter = NULL;
			MemoryContextReset(node->prefilterCtx);
			break;
		}
	}

	index_endscan(scan);

	if (node->prefilter != NULL)
		node->ntids = IvfflatPrefilterCount(node->prefilter);
}

/*
 * Install the TID set pointer into the child ivfflat scan (spec §5.4).
 * Safe because ivfflatgettuple does all its work lazily under so->first;
 * GetScanItems cannot have run before the child produced its first tuple.
 */
static void
InjectPrefilter(IvfPrefilterScanState *node)
{
	IndexScanState *child;

	if (node->prefilter == NULL)
		return;

	child = castNode(IndexScanState, linitial(node->css.custom_ps));

	/*
	 * The child creates its scan descriptor lazily on the first IndexNext()
	 * call; create it here exactly the way IndexNext() would, so there is
	 * something to inject into.  IndexNext() then finds it set and proceeds
	 * normally.
	 */
	if (child->iss_ScanDesc == NULL)
	{
		EState	   *estate = node->css.ss.ps.state;

#if PG_VERSION_NUM >= 180000
		child->iss_ScanDesc = index_beginscan(child->ss.ss_currentRelation,
											  child->iss_RelationDesc,
											  estate->es_snapshot,
											  &child->iss_Instrument,
											  child->iss_NumScanKeys,
											  child->iss_NumOrderByKeys);
#else
		child->iss_ScanDesc = index_beginscan(child->ss.ss_currentRelation,
											  child->iss_RelationDesc,
											  estate->es_snapshot,
											  child->iss_NumScanKeys,
											  child->iss_NumOrderByKeys);
#endif

		/*
		 * If no run-time keys to calculate or they are ready, go ahead and
		 * pass the scankeys to the index AM (same as IndexNext)
		 */
		if (child->iss_NumRuntimeKeys == 0 || child->iss_RuntimeKeysReady)
			index_rescan(child->iss_ScanDesc,
						 child->iss_ScanKeys, child->iss_NumScanKeys,
						 child->iss_OrderByKeys, child->iss_NumOrderByKeys);
	}

	IvfflatScanSetPrefilter(child->iss_ScanDesc, node->prefilter);
	node->injected = true;
}

/*
 * Clear the child's prefilter pointer (before freeing/rebuilding the set)
 */
static void
UninjectPrefilter(IvfPrefilterScanState *node)
{
	IndexScanState *child;

	if (!node->injected)
		return;

	child = castNode(IndexScanState, linitial(node->css.custom_ps));

	if (child->iss_ScanDesc != NULL)
		IvfflatScanSetPrefilter(child->iss_ScanDesc, NULL);

	node->injected = false;
}

/*
 * ExecCustomScan: build/inject once per (re)scan, then pass child tuples
 * through 1:1
 */
static TupleTableSlot *
IvfPrefilterExec(CustomScanState *css)
{
	IvfPrefilterScanState *node = (IvfPrefilterScanState *) css;
	TupleTableSlot *slot;

	/*
	 * Under EvalPlanQual the child returns the substitute test tuple and
	 * evaluates its Filter against it; building the set would be a full
	 * B-tree drain to recheck one row (spec §5.7)
	 */
#if PG_VERSION_NUM >= 140000
	if (!node->setBuilt && css->ss.ps.state->es_epq_active == NULL)
#else
	if (!node->setBuilt && css->ss.ps.state->es_epqTuple == NULL)
#endif
	{
		BuildTidSet(node);		/* may fall back */
		InjectPrefilter(node);	/* no-op on fallback */
		node->setBuilt = true;
	}

	slot = ExecProcNode((PlanState *) linitial(css->custom_ps));

	if (TupIsNull(slot))
		return NULL;

	/*
	 * Child and parent targetlists both describe the base relation row, so
	 * normally no projection is set up; handle the case where the planner
	 * gave us a narrower tlist
	 */
	if (css->ss.ps.ps_ProjInfo != NULL)
	{
		ExprContext *econtext = css->ss.ps.ps_ExprContext;

		ResetExprContext(econtext);
		econtext->ecxt_scantuple = slot;
		return ExecProject(css->ss.ps.ps_ProjInfo);
	}

	return slot;
}

/*
 * ReScanCustomScan (spec §5.6)
 */
static void
IvfPrefilterReScan(CustomScanState *css)
{
	IvfPrefilterScanState *node = (IvfPrefilterScanState *) css;
	PlanState  *child = (PlanState *) linitial(css->custom_ps);

	/*
	 * A rescan with changed params means the TID set may be stale; v1 always
	 * rebuilds.  A rescan without chgParam (e.g. plain nested loop) keeps
	 * the set: the child's ivfflatrescan does not touch so->prefilter.
	 */
	if (css->ss.ps.chgParam != NULL)
	{
		UninjectPrefilter(node);
		node->prefilter = NULL;
		if (node->prefilterCtx != NULL)
			MemoryContextReset(node->prefilterCtx);
		node->setBuilt = false;
		node->runtimeKeysReady = false;
		node->ntids = 0;

		/* custom_ps children are not covered by ExecReScan's propagation */
		UpdateChangedParamSet(child, css->ss.ps.chgParam);
	}

	/* If the child has changed params, ExecProcNode will rescan it lazily */
	if (child->chgParam == NULL)
		ExecReScan(child);
}

/*
 * EndCustomScan
 */
static void
IvfPrefilterEnd(CustomScanState *css)
{
	IvfPrefilterScanState *node = (IvfPrefilterScanState *) css;

	UninjectPrefilter(node);

	ExecEndNode((PlanState *) linitial(css->custom_ps));

	if (node->btindex != NULL)
		index_close(node->btindex, NoLock); /* lock released at xact end */

	if (node->prefilterCtx != NULL)
		MemoryContextDelete(node->prefilterCtx);
}

/*
 * ExplainCustomScan (spec §9)
 */
static void
IvfPrefilterExplain(CustomScanState *css, List *ancestors, ExplainState *es)
{
	IvfPrefilterScanState *node = (IvfPrefilterScanState *) css;
	CustomScan *cscan = (CustomScan *) css->ss.ps.plan;
	List	   *origQuals = (List *) lsecond(cscan->custom_exprs);
	char	   *indexname = get_rel_name(node->btindexoid);

	if (origQuals != NIL)
	{
		List	   *context;
		Node	   *qualexpr;
		bool		useprefix;
		char	   *exprstr;

#if PG_VERSION_NUM >= 140000
		context = set_deparse_context_plan(es->deparse_cxt, css->ss.ps.plan,
										   ancestors);
#else
		context = set_deparse_context_planstate(es->deparse_cxt,
												(Node *) css, ancestors);
#endif
		qualexpr = (Node *) make_ands_explicit(origQuals);
		useprefix = list_length(es->rtable) > 1 || es->verbose;
		exprstr = deparse_expression(qualexpr, context, useprefix, false);
		ExplainPropertyText("Prefilter", exprstr, es);
	}

	if (indexname != NULL)
		ExplainPropertyText("Prefilter Index", indexname, es);

	if (es->analyze)
	{
		ExplainPropertyInteger("Prefilter TIDs", NULL, node->ntids, es);
		ExplainPropertyInteger("Prefilter Rebuilds", NULL,
							   node->nbuilds > 0 ? node->nbuilds - 1 : 0, es);
		if (node->fallbackReason != NULL)
			ExplainPropertyText("Prefilter Fallback", node->fallbackReason, es);
	}
}

/*
 * Install the planner hook and register the custom scan methods
 * (called from _PG_init)
 */
void
IvfflatPrefilterInit(void)
{
	prev_set_rel_pathlist_hook = set_rel_pathlist_hook;
	set_rel_pathlist_hook = IvfflatPrefilterPathlistHook;

	/*
	 * Registration by name is required for any path where the plan tree is
	 * serialized/deserialized; cheap future-proofing
	 */
	RegisterCustomScanMethods(&ivf_prefilter_scan_methods);
}
