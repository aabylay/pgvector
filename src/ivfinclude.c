/*
 * IvfIncludeScan: filter an ivfflat scan from per-column chains stored in
 * the index, before any distance is computed.
 *
 * The child Index Scan keeps the full Filter, so a miss (budget, GUC, a
 * clause this node did not push) still returns the right rows. This node
 * does not build a TID set and does not call IvfflatScanSetPrefilter.
 *
 * See docs/filtered-ivf.md.
 */
#include "postgres.h"

#include <math.h>

#include "access/genam.h"
#include "access/table.h"
#include "catalog/pg_am_d.h"
#include "commands/defrem.h"
#include "commands/explain.h"
#include "executor/executor.h"
#include "executor/nodeIndexscan.h"
#include "ivfflat.h"
#include "ivfinclude.h"
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

#if PG_VERSION_NUM >= 180000
#include "commands/explain_format.h"
#include "commands/explain_state.h"
#endif

/* Stored bytes charged per included value, on top of its average width. */
#define INCLUDE_ENTRY_HEADER	4
#define INCLUDE_ITEM_POINTER	4

bool		ivfflat_include_filter = true;
int			ivfflat_include_mem = -1;

static set_rel_pathlist_hook_type prev_set_rel_pathlist_hook = NULL;

Size
IvfflatIncludeMemBudget(void)
{
	int			kb = ivfflat_include_mem < 0 ? work_mem : ivfflat_include_mem;

	if (kb < 0)
		kb = 0;

	return (Size) kb * 1024;
}

/*
 * Planner form of the pushed filter. A leaf walks one chain and tests its
 * quals (implicitly ANDed). AND and OR nodes combine their children.
 */
typedef struct IncludeTreeNode
{
	IvfflatIncludeNodeKind kind;
	int			chainIndex;		/* leaf: slot in includeChains[] */
	int			boundWidth;		/* leaf: stored width bound */
	int32		avgWidth;		/* leaf: average width, for cost */
	List	   *quals;			/* leaf: clauses on that column */
	List	   *children;		/* AND / OR: IncludeTreeNode list */
}			IncludeTreeNode;

typedef struct IncludeMatchContext
{
	IndexOptInfo *index;
	Index		relid;
	Oid			heapOid;
	int			includeCount;
	const uint16 *includeWidth;
}			IncludeMatchContext;

/*
 * Path-private. custom_private holds this pointer; paths are not serialized.
 */
typedef struct IncludePathInfo
{
	int			lists;
	int			dimensions;
	int			nleaves;
	double		chainBytes;		/* bytes read per entry, all leaves */
	Selectivity sigmaPush;
	double		vectorItemSize;
	IncludeTreeNode *root;
}			IncludePathInfo;

typedef struct IvfIncludeScanState
{
	CustomScanState css;
	IvfflatIncludeFilter *filter;
	bool		injected;
}			IvfIncludeScanState;

/* Rebuilds the tree from the plan's shape list and per-leaf quals */
typedef struct IncludeDecodeState
{
	List	   *shape;
	int			shapePos;
	List	   *leafQuals;
	int			leafPos;
	IvfflatIncludeFilter *filter;
	Relation	indexRel;
	PlanState  *parent;
}			IncludeDecodeState;

static Plan *IvfIncludePlanCustomPath(PlannerInfo *root, RelOptInfo *rel,
									  CustomPath *best_path, List *tlist,
									  List *clauses, List *custom_plans);
static Node *IvfIncludeCreateScanState(CustomScan *cscan);
static void IvfIncludeBegin(CustomScanState *css, EState *estate, int eflags);
static TupleTableSlot *IvfIncludeExec(CustomScanState *css);
static void IvfIncludeEnd(CustomScanState *css);
static void IvfIncludeReScan(CustomScanState *css);
static void IvfIncludeExplain(CustomScanState *css, List *ancestors,
							  ExplainState *es);

static const CustomPathMethods ivf_include_path_methods = {
	.CustomName = "IvfIncludeScan",
	.PlanCustomPath = IvfIncludePlanCustomPath,
	.ReparameterizeCustomPathByChild = NULL,
};

static CustomScanMethods ivf_include_scan_methods = {
	.CustomName = "IvfIncludeScan",
	.CreateCustomScanState = IvfIncludeCreateScanState,
};

static const CustomExecMethods ivf_include_exec_methods = {
	.CustomName = "IvfIncludeScan",
	.BeginCustomScan = IvfIncludeBegin,
	.ExecCustomScan = IvfIncludeExec,
	.EndCustomScan = IvfIncludeEnd,
	.ReScanCustomScan = IvfIncludeReScan,
	.MarkPosCustomScan = NULL,
	.RestrPosCustomScan = NULL,
	.EstimateDSMCustomScan = NULL,
	.InitializeDSMCustomScan = NULL,
	.ReInitializeDSMCustomScan = NULL,
	.InitializeWorkerCustomScan = NULL,
	.ShutdownCustomScan = NULL,
	.ExplainCustomScan = IvfIncludeExplain,
};

static Oid
IvfflatAmOid(void)
{
	static Oid	amoid = InvalidOid;

	if (!OidIsValid(amoid))
		amoid = get_index_am_oid("ivfflat", true);

	return amoid;
}

static Node *
StripRelabel(Node *node)
{
	while (node != NULL && IsA(node, RelabelType))
		node = (Node *) ((RelabelType *) node)->arg;

	return node;
}

static Var *
BareColumnVar(Node *node, Index relid)
{
	Var		   *var;

	node = StripRelabel(node);
	if (node == NULL || !IsA(node, Var))
		return NULL;

	var = (Var *) node;
	if (var->varno != relid || var->varattno <= 0 || var->varlevelsup != 0)
		return NULL;

	return var;
}

static bool
OtherSideOk(Node *other)
{
	if (other == NULL)
		return false;
	if (contain_var_clause(other))
		return false;
	if (contain_volatile_functions(other))
		return false;
	if (contain_subplans(other))
		return false;

	return true;
}

static bool
OpInDefaultBtree(Oid opno, Oid typid)
{
	Oid			opclass;
	Oid			opfamily;

	opclass = GetDefaultOpClass(typid, BTREE_AM_OID);
	if (!OidIsValid(opclass))
		return false;

	opfamily = get_opclass_family(opclass);
	return op_in_opfamily(opno, opfamily);
}

/*
 * One comparison on a single column: column op value, value op column,
 * column = ANY (array), or column IS NULL. On success *heapAtt is that
 * column. A leaf runs the clause itself on the stored value, so any operator
 * collation is fine.
 */
static bool
MatchIncludeClause(Node *node, Index relid, AttrNumber *heapAtt)
{
	if (node == NULL)
		return false;

	if (IsA(node, OpExpr))
	{
		OpExpr	   *op = (OpExpr *) node;
		Var		   *var;
		Node	   *other;

		if (list_length(op->args) != 2)
			return false;

		var = BareColumnVar((Node *) linitial(op->args), relid);
		other = (Node *) lsecond(op->args);

		if (var == NULL)
		{
			var = BareColumnVar((Node *) lsecond(op->args), relid);
			other = (Node *) linitial(op->args);
		}

		if (var == NULL || !OtherSideOk(other))
			return false;

		if (!OpInDefaultBtree(op->opno, var->vartype))
			return false;

		*heapAtt = var->varattno;
		return true;
	}

	if (IsA(node, ScalarArrayOpExpr))
	{
		ScalarArrayOpExpr *saop = (ScalarArrayOpExpr *) node;
		Var		   *var;

		if (!saop->useOr || list_length(saop->args) != 2)
			return false;

		var = BareColumnVar((Node *) linitial(saop->args), relid);
		if (var == NULL || !OtherSideOk((Node *) lsecond(saop->args)))
			return false;

		if (!OpInDefaultBtree(saop->opno, var->vartype))
			return false;

		*heapAtt = var->varattno;
		return true;
	}

	if (IsA(node, NullTest))
	{
		NullTest   *ntest = (NullTest *) node;
		Var		   *var;

		if (ntest->nulltesttype != IS_NULL || ntest->argisrow)
			return false;

		var = BareColumnVar((Node *) ntest->arg, relid);
		if (var == NULL)
			return false;

		*heapAtt = var->varattno;
		return true;
	}

	return false;
}

static IncludeTreeNode *
MakeIncludeLeaf(IncludeMatchContext *ctx, Node *clause)
{
	IndexOptInfo *index = ctx->index;
	AttrNumber	heapAtt;

	if (!MatchIncludeClause(clause, ctx->relid, &heapAtt))
		return NULL;

	/* indexcollations, opfamily, and opcintype cover key columns only */
	for (int chain = 0; chain < ctx->includeCount; chain++)
	{
		int			col = index->nkeycolumns + chain;
		IncludeTreeNode *leaf;
		int32		avg;

		if (col >= index->ncolumns)
			break;
		if (index->indexkeys[col] != heapAtt)
			continue;

		leaf = palloc0_object(IncludeTreeNode);
		leaf->kind = IVF_INCLUDE_LEAF;
		leaf->chainIndex = chain;
		leaf->boundWidth = ctx->includeWidth[chain];
		avg = get_attavgwidth(ctx->heapOid, heapAtt);
		leaf->avgWidth = avg > 0 ? avg : leaf->boundWidth;
		leaf->quals = list_make1(clause);
		return leaf;
	}

	return NULL;
}

static List *
AppendFlat(List *args, Node *expr, bool isAnd)
{
	if (isAnd ? is_andclause(expr) : is_orclause(expr))
		return list_concat(args, ((BoolExpr *) expr)->args);

	return lappend(args, expr);
}

/* The tree as one boolean expression, in the order it runs */
static Node *
IncludeTreeExpr(IncludeTreeNode *node)
{
	bool		isAnd = node->kind != IVF_INCLUDE_OR;
	List	   *args = NIL;
	ListCell   *lc;

	if (node->kind == IVF_INCLUDE_LEAF)
	{
		if (list_length(node->quals) == 1)
			return (Node *) linitial(node->quals);

		return (Node *) make_andclause(list_copy(node->quals));
	}

	foreach(lc, node->children)
		args = AppendFlat(args, IncludeTreeExpr((IncludeTreeNode *) lfirst(lc)), isAnd);

	return (Node *) (isAnd ? make_andclause(args) : make_orclause(args));
}

static int
CompareIncludeLeaves(const ListCell *a, const ListCell *b)
{
	IncludeTreeNode *la = (IncludeTreeNode *) lfirst(a);
	IncludeTreeNode *lb = (IncludeTreeNode *) lfirst(b);

	if (la->boundWidth != lb->boundWidth)
		return la->boundWidth < lb->boundWidth ? -1 : 1;

	return la->chainIndex - lb->chainIndex;
}

/*
 * AND or OR of children. Children of the same kind are flattened, leaves on
 * one chain are merged so the chain is walked once, and leaves run before
 * composites, narrowest stored width first.
 */
static IncludeTreeNode *
MakeIncludeComposite(IvfflatIncludeNodeKind kind, List *children)
{
	IncludeTreeNode *byChain[IVFFLAT_MAX_INCLUDE_COLS] = {0};
	List	   *flat = NIL;
	List	   *leaves = NIL;
	List	   *composites = NIL;
	IncludeTreeNode *node;
	ListCell   *lc;

	foreach(lc, children)
	{
		IncludeTreeNode *child = (IncludeTreeNode *) lfirst(lc);

		if (child->kind == kind)
			flat = list_concat(flat, child->children);
		else
			flat = lappend(flat, child);
	}

	foreach(lc, flat)
	{
		IncludeTreeNode *child = (IncludeTreeNode *) lfirst(lc);
		IncludeTreeNode *same;

		if (child->kind != IVF_INCLUDE_LEAF)
		{
			composites = lappend(composites, child);
			continue;
		}

		same = byChain[child->chainIndex];
		if (same == NULL)
		{
			same = palloc_object(IncludeTreeNode);
			*same = *child;
			byChain[child->chainIndex] = same;
			leaves = lappend(leaves, same);
		}
		else if (kind == IVF_INCLUDE_AND)
			same->quals = list_concat_copy(same->quals, child->quals);
		else
		{
			List	   *args = AppendFlat(NIL, IncludeTreeExpr(same), false);

			args = AppendFlat(args, IncludeTreeExpr(child), false);
			same->quals = list_make1(make_orclause(args));
		}
	}

	list_sort(leaves, CompareIncludeLeaves);
	flat = list_concat(leaves, composites);

	if (list_length(flat) == 1)
		return (IncludeTreeNode *) linitial(flat);

	node = palloc0_object(IncludeTreeNode);
	node->kind = kind;
	node->children = flat;
	return node;
}

/*
 * The part of a clause the chains can test, or NULL. An AND keeps the arms
 * it can push and clears *exact if it drops any, so the result may pass more
 * rows than the clause. An OR is pushed only when every arm is.
 */
static IncludeTreeNode *
BuildIncludeTree(IncludeMatchContext *ctx, Node *clause, bool *exact)
{
	bool		isAnd = is_andclause(clause);
	List	   *children = NIL;
	ListCell   *lc;

	check_stack_depth();

	if (!isAnd && !is_orclause(clause))
		return MakeIncludeLeaf(ctx, clause);

	foreach(lc, ((BoolExpr *) clause)->args)
	{
		IncludeTreeNode *child = BuildIncludeTree(ctx, (Node *) lfirst(lc), exact);

		if (child != NULL)
			children = lappend(children, child);
		else if (isAnd)
			*exact = false;
		else
			return NULL;
	}

	if (children == NIL)
		return NULL;

	return MakeIncludeComposite(isAnd ? IVF_INCLUDE_AND : IVF_INCLUDE_OR, children);
}

static void
IncludeTreeStats(IncludeTreeNode *node, int *nleaves, double *chainBytes)
{
	ListCell   *lc;

	if (node->kind == IVF_INCLUDE_LEAF)
	{
		(*nleaves)++;
		*chainBytes += (double) node->avgWidth + INCLUDE_ENTRY_HEADER + INCLUDE_ITEM_POINTER;
		return;
	}

	foreach(lc, node->children)
		IncludeTreeStats((IncludeTreeNode *) lfirst(lc), nleaves, chainBytes);
}

/*
 * The AND of the pushable part of each restriction on this index's included
 * columns. NULL when nothing is pushable or the index is not version 2.
 */
static IncludePathInfo *
MatchIncludeIndex(PlannerInfo *root, RelOptInfo *rel, IndexOptInfo *index)
{
	IncludePathInfo *info;
	IncludeMatchContext ctx;
	Relation	irel;
	IvfflatMetaInfo meta;
	const		IvfflatTypeInfo *typeInfo;
	List	   *trees = NIL;
	List	   *selec = NIL;
	ListCell   *lc;

	if (index->relam != IvfflatAmOid())
		return NULL;
	if (index->hypothetical)
		return NULL;
	if (index->nkeycolumns != 1 || index->ncolumns <= index->nkeycolumns)
		return NULL;

	irel = index_open(index->indexoid, NoLock);
	IvfflatReadMeta(irel, &meta);
	typeInfo = IvfflatGetTypeInfo(irel);
	index_close(irel, NoLock);

	if (meta.version != IVFFLAT_VERSION_INCLUDE || meta.includeCount <= 0)
		return NULL;

	ctx.index = index;
	ctx.relid = rel->relid;
	ctx.heapOid = root->simple_rte_array[rel->relid]->relid;
	ctx.includeCount = meta.includeCount;
	ctx.includeWidth = meta.includeWidth;

	foreach(lc, rel->baserestrictinfo)
	{
		RestrictInfo *rinfo = (RestrictInfo *) lfirst(lc);
		IncludeTreeNode *tree;
		bool		exact = true;

		if (rinfo->pseudoconstant)
			continue;
		/* Chains are read before row-level security quals run */
		if (!restriction_is_securely_promotable(rinfo, rel))
			continue;
		if (contain_volatile_functions((Node *) rinfo->clause))
			continue;
		if (contain_subplans((Node *) rinfo->clause))
			continue;

		tree = BuildIncludeTree(&ctx, (Node *) rinfo->clause, &exact);
		if (tree == NULL)
			continue;

		trees = lappend(trees, tree);
		selec = lappend(selec, exact ? (Node *) rinfo : IncludeTreeExpr(tree));
	}

	if (trees == NIL)
		return NULL;

	info = palloc0_object(IncludePathInfo);
	info->lists = meta.lists;
	info->dimensions = meta.dimensions;
	info->vectorItemSize = (double) MAXALIGN(sizeof(uint32) + sizeof(IndexTupleData) +
											 typeInfo->itemSize(meta.dimensions));
	if (info->vectorItemSize < 1.0)
		info->vectorItemSize = 1.0;
	info->root = MakeIncludeComposite(IVF_INCLUDE_AND, trees);
	IncludeTreeStats(info->root, &info->nleaves, &info->chainBytes);
	info->sigmaPush = clauselist_selectivity(root, selec, (int) rel->relid,
											 JOIN_INNER, NULL);
	return info;
}

static void
CostIncludePath(PlannerInfo *root, RelOptInfo *rel, CustomPath *cpath,
				IncludePathInfo *info)
{
	bool		iterative = ivfflat_iterative_scan != IVFFLAT_ITERATIVE_SCAN_OFF;
	int			lists = Max(info->lists, 1);
	int			batch = Min(ivfflat_probes, lists);
	int			cap;
	double		N = Max(rel->tuples, 0.0);
	double		C = N / (double) lists;
	double		sigmaPush = info->sigmaPush;
	double		sigmaAll;
	double		k = 0.0;
	bool		hasLimit = false;
	double		P;
	double		costDistance;
	double		costFilter;
	double		vectorPages;
	double		tuplesPerPage;
	double		hitFraction;
	double		localPageCost;
	double		vectorIo;
	double		centerIo;
	double		startup;
	double		heapFetches;
	double		total;

	if (iterative)
		cap = Min(Max(ivfflat_max_probes, ivfflat_probes), lists);
	else
		cap = batch;

	sigmaAll = clauselist_selectivity(root, rel->baserestrictinfo,
									   (int) rel->relid, JOIN_INNER, NULL);
	if (sigmaPush < 0.0)
		sigmaPush = 0.0;
	if (sigmaPush > 1.0)
		sigmaPush = 1.0;
	if (sigmaAll < 0.0)
		sigmaAll = 0.0;
	if (sigmaAll > 1.0)
		sigmaAll = 1.0;

	if (root->tuple_fraction >= 1.0)
	{
		k = root->tuple_fraction;
		hasLimit = true;
	}
	else if (root->tuple_fraction > 0.0)
	{
		k = root->tuple_fraction * cpath->path.rows;
		hasLimit = true;
	}

	if (!iterative)
		P = (double) batch;
	else if (!hasLimit)
		P = (double) cap;
	else
	{
		double		praw = ceil(k / Max(sigmaAll * C, 1.0e-6));

		P = Min((double) cap, (double) batch * ceil(praw / (double) Max(batch, 1)));
	}

	costDistance = (double) Max(info->dimensions, 1) * cpu_operator_cost;
	costFilter = (double) info->nleaves * cpu_operator_cost +
		info->chainBytes / (double) BLCKSZ * seq_page_cost;

	if (P * C <= 0.0 || info->vectorItemSize <= 0.0)
		vectorPages = 0.0;
	else
		vectorPages = (P * C * info->vectorItemSize) / (double) BLCKSZ;

	tuplesPerPage = (P * C) / Max(vectorPages, 1.0);
	if (tuplesPerPage < 0.0)
		tuplesPerPage = 0.0;

	if (sigmaPush <= 0.0 || tuplesPerPage <= 0.0)
		hitFraction = 0.0;
	else if (sigmaPush >= 1.0)
		hitFraction = 1.0;
	else
		hitFraction = 1.0 - pow(1.0 - sigmaPush, tuplesPerPage);

	localPageCost = seq_page_cost + 0.5 * (random_page_cost - seq_page_cost);
	vectorIo = Min(vectorPages * seq_page_cost,
				   vectorPages * hitFraction * localPageCost);
	centerIo = ((double) lists * info->vectorItemSize) / (double) BLCKSZ * seq_page_cost;

	startup = centerIo + vectorIo + P * C * sigmaPush * costDistance + P * C * costFilter;
	heapFetches = Min(cpath->path.rows, P * C * sigmaAll);
	total = startup + heapFetches * (random_page_cost + cpu_tuple_cost);

	if (startup < 1.0e-10)
		startup = 1.0e-10;
	if (total < startup)
		total = startup;

	cpath->path.startup_cost = startup;
	cpath->path.total_cost = total;
}

static IndexPath *
CopyIndexPath(IndexPath *src)
{
	IndexPath  *copy = palloc_object(IndexPath);

	memcpy(copy, src, sizeof(IndexPath));
	return copy;
}

static void
AddIncludePath(PlannerInfo *root, RelOptInfo *rel, IndexPath *ivfpath,
			   IncludePathInfo *info)
{
	CustomPath *cpath = makeNode(CustomPath);

	cpath->path.pathtype = T_CustomScan;
	cpath->path.parent = rel;
	cpath->path.pathtarget = rel->reltarget;
	cpath->path.param_info = NULL;
	cpath->path.parallel_safe = false;
	cpath->path.parallel_aware = false;
	cpath->path.parallel_workers = 0;
	cpath->path.rows = ivfpath->path.rows;
	cpath->path.pathkeys = ivfpath->path.pathkeys;
#if PG_VERSION_NUM >= 180000
	cpath->path.disabled_nodes = ivfpath->path.disabled_nodes;
#endif
	cpath->flags = 0;
#ifdef CUSTOMPATH_SUPPORT_PROJECTION
	cpath->flags |= CUSTOMPATH_SUPPORT_PROJECTION;
#endif
	cpath->custom_paths = list_make1(ivfpath);
	cpath->custom_private = list_make1(info);
	cpath->methods = &ivf_include_path_methods;

	CostIncludePath(root, rel, cpath, info);
	add_path(rel, (Path *) cpath);
}

static List *
SnapshotIvfflatPaths(RelOptInfo *rel, Oid amoid)
{
	List	   *paths = NIL;
	ListCell   *lc;

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
			continue;

		paths = lappend(paths, CopyIndexPath(ipath));
	}

	return paths;
}

static bool
RelationSupportsPhysicalTlist(RangeTblEntry *rte)
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
	return usable;
}

static void
IvfflatIncludePathlistHook(PlannerInfo *root, RelOptInfo *rel, Index rti,
						   RangeTblEntry *rte)
{
	Oid			amoid;
	List	   *ivfpaths;
	ListCell   *lc;

	amoid = IvfflatAmOid();
	ivfpaths = OidIsValid(amoid) ? SnapshotIvfflatPaths(rel, amoid) : NIL;

	if (prev_set_rel_pathlist_hook)
		prev_set_rel_pathlist_hook(root, rel, rti, rte);

	if (!ivfflat_include_filter)
		return;
	if (ivfpaths == NIL)
		return;
	if (rte->rtekind != RTE_RELATION || rte->tablesample != NULL)
		return;
	if (rte->relkind != RELKIND_RELATION && rte->relkind != RELKIND_MATVIEW)
		return;
	if (rel->reloptkind != RELOPT_BASEREL &&
		rel->reloptkind != RELOPT_OTHER_MEMBER_REL)
		return;
	if (rel->baserestrictinfo == NIL)
		return;
	if (!RelationSupportsPhysicalTlist(rte))
		return;

	foreach(lc, ivfpaths)
	{
		IndexPath  *ipath = (IndexPath *) lfirst(lc);
		IncludePathInfo *info = MatchIncludeIndex(root, rel, ipath->indexinfo);
		double		entries;
		double		bitmapBytes;

		if (info == NULL)
			continue;

		entries = rel->tuples / (double) Max(info->lists, 1);
		if (entries < 0.0)
			entries = 0.0;
		bitmapBytes = ceil(entries / 8.0);

		if (bitmapBytes > (double) IvfflatIncludeMemBudget())
			continue;

		AddIncludePath(root, rel, ipath, info);
	}
}

/* Leaf quals read their column from a one-attribute scan slot */
static Node *
RemapIncludeMutator(Node *node, void *context)
{
	if (node == NULL)
		return NULL;

	if (IsA(node, Var))
	{
		Var		   *var = copyObject((Var *) node);

		var->varno = INDEX_VAR;
		var->varattno = 1;
		return (Node *) var;
	}

	return expression_tree_mutator(node, RemapIncludeMutator, context);
}

/*
 * Prefix order. shape gets the kind, then the chain index of a leaf or the
 * child count of an AND / OR. leafQuals gets one qual list per leaf.
 */
static void
EncodeIncludeTree(IncludeTreeNode *node, List **shape, List **leafQuals)
{
	ListCell   *lc;

	*shape = lappend_int(*shape, (int) node->kind);

	if (node->kind == IVF_INCLUDE_LEAF)
	{
		*shape = lappend_int(*shape, node->chainIndex);
		*leafQuals = lappend(*leafQuals,
							 RemapIncludeMutator((Node *) node->quals, NULL));
		return;
	}

	*shape = lappend_int(*shape, list_length(node->children));
	foreach(lc, node->children)
		EncodeIncludeTree((IncludeTreeNode *) lfirst(lc), shape, leafQuals);
}

static Plan *
IvfIncludePlanCustomPath(PlannerInfo *root, RelOptInfo *rel,
						 CustomPath *best_path, List *tlist,
						 List *clauses, List *custom_plans)
{
	IncludePathInfo *info = (IncludePathInfo *) linitial(best_path->custom_private);
	CustomScan *cscan = makeNode(CustomScan);
	Plan	   *childPlan = (Plan *) linitial(custom_plans);
	List	   *physTlist;
	List	   *shape = NIL;
	List	   *leafQuals = NIL;
	List	   *displayQuals;

	physTlist = build_physical_tlist(root, rel);
	if (physTlist == NIL)
		elog(ERROR, "could not build physical tlist for include scan");
	childPlan->targetlist = physTlist;

	EncodeIncludeTree(info->root, &shape, &leafQuals);
	displayQuals = make_ands_implicit((Expr *) copyObject(IncludeTreeExpr(info->root)));

	cscan->scan.plan.targetlist = tlist;
	cscan->scan.plan.qual = NIL;
	cscan->scan.scanrelid = rel->relid;
	cscan->flags = best_path->flags;
	cscan->custom_plans = custom_plans;
	cscan->custom_exprs = list_make2(leafQuals, displayQuals);
	cscan->custom_private = list_make1(shape);
	cscan->custom_scan_tlist = NIL;
	cscan->methods = &ivf_include_scan_methods;

	return (Plan *) cscan;
}

static Node *
IvfIncludeCreateScanState(CustomScan *cscan)
{
	IvfIncludeScanState *node;

	node = (IvfIncludeScanState *) newNode(sizeof(IvfIncludeScanState),
										   T_CustomScanState);
	node->css.methods = &ivf_include_exec_methods;
	node->filter = NULL;
	node->injected = false;

	return (Node *) node;
}

static bool
EpqActive(EState *estate)
{
#if PG_VERSION_NUM >= 140000
	return estate->es_epq_active != NULL;
#else
	return estate->es_epqTuple != NULL;
#endif
}

static int
NextShapeInt(IncludeDecodeState *ds)
{
	if (ds->shapePos >= list_length(ds->shape))
		elog(ERROR, "ivfflat include filter shape is truncated");

	return list_nth_int(ds->shape, ds->shapePos++);
}

static void
DecodeIncludeNode(IncludeDecodeState *ds, IvfflatIncludeNode *node)
{
	check_stack_depth();

	node->kind = (IvfflatIncludeNodeKind) NextShapeInt(ds);

	switch (node->kind)
	{
		case IVF_INCLUDE_LEAF:
			{
				int			chainIndex = NextShapeInt(ds);
				IvfflatIncludeColumn *col;

				if (chainIndex < 0 || chainIndex >= ds->filter->ncolumns)
					elog(ERROR, "ivfflat include chain index %d is out of range", chainIndex);
				if (ds->leafPos >= list_length(ds->leafQuals))
					elog(ERROR, "ivfflat include filter has too few leaf quals");

				col = &ds->filter->columns[chainIndex];
				if (col->tupdesc == NULL)
				{
					col->chainIndex = chainIndex;
					col->indexAttno = (AttrNumber) (chainIndex + 2);
					col->tupdesc = IvfflatOneColumnDesc(ds->indexRel, col->indexAttno);
					col->slot = MakeSingleTupleTableSlot(col->tupdesc, &TTSOpsVirtual);
				}

				node->chainIndex = chainIndex;
				node->qual = ExecInitQual((List *) list_nth(ds->leafQuals, ds->leafPos++),
										  ds->parent);
				break;
			}
		case IVF_INCLUDE_AND:
		case IVF_INCLUDE_OR:
			node->nchildren = NextShapeInt(ds);
			if (node->nchildren < 1)
				elog(ERROR, "ivfflat include filter node has %d children", node->nchildren);

			node->children = palloc0_array_checked(IvfflatIncludeNode, node->nchildren);
			for (int i = 0; i < node->nchildren; i++)
				DecodeIncludeNode(ds, &node->children[i]);
			break;
		default:
			elog(ERROR, "unrecognized ivfflat include node kind: %d", (int) node->kind);
	}
}

static void
IvfIncludeBegin(CustomScanState *css, EState *estate, int eflags)
{
	IvfIncludeScanState *node = (IvfIncludeScanState *) css;
	CustomScan *cscan = (CustomScan *) css->ss.ps.plan;
	IndexScanState *child;
	IvfflatIncludeFilter *filter;
	IvfflatMetaInfo meta;
	IncludeDecodeState ds;

	node->filter = NULL;
	node->injected = false;

	css->custom_ps = list_make1(ExecInitNode((Plan *) linitial(cscan->custom_plans),
											 estate, eflags));

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

	if (eflags & EXEC_FLAG_EXPLAIN_ONLY)
		return;
	if (EpqActive(estate))
		return;

	child = castNode(IndexScanState, linitial(css->custom_ps));
	IvfflatReadMeta(child->iss_RelationDesc, &meta);

	filter = palloc0_object(IvfflatIncludeFilter);
	filter->ncolumns = meta.includeCount;
	filter->econtext = CreateExprContext(estate);
	filter->fallbackReason = NULL;

	ds.shape = (List *) linitial(cscan->custom_private);
	ds.shapePos = 0;
	ds.leafQuals = (List *) linitial(cscan->custom_exprs);
	ds.leafPos = 0;
	ds.filter = filter;
	ds.indexRel = child->iss_RelationDesc;
	ds.parent = &css->ss.ps;

	filter->root = palloc0_object(IvfflatIncludeNode);
	DecodeIncludeNode(&ds, filter->root);

	if (ds.shapePos != list_length(ds.shape) || ds.leafPos != list_length(ds.leafQuals))
		elog(ERROR, "ivfflat include filter shape does not match its quals");

	node->filter = filter;
}

static void
InjectIncludeFilter(IvfIncludeScanState *node)
{
	IndexScanState *child;

	if (node->filter == NULL)
		return;

	child = castNode(IndexScanState, linitial(node->css.custom_ps));

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

		if (child->iss_NumRuntimeKeys == 0 || child->iss_RuntimeKeysReady)
			index_rescan(child->iss_ScanDesc,
						 child->iss_ScanKeys, child->iss_NumScanKeys,
						 child->iss_OrderByKeys, child->iss_NumOrderByKeys);
	}

	IvfflatScanSetIncludeFilter(child->iss_ScanDesc, node->filter);
	node->injected = true;
}

static void
UninjectIncludeFilter(IvfIncludeScanState *node)
{
	IndexScanState *child;

	if (!node->injected)
		return;

	child = castNode(IndexScanState, linitial(node->css.custom_ps));

	if (child->iss_ScanDesc != NULL)
		IvfflatScanSetIncludeFilter(child->iss_ScanDesc, NULL);

	node->injected = false;
}

static TupleTableSlot *
IvfIncludeExec(CustomScanState *css)
{
	IvfIncludeScanState *node = (IvfIncludeScanState *) css;
	TupleTableSlot *slot;

	if (!node->injected && !EpqActive(css->ss.ps.state))
		InjectIncludeFilter(node);

	slot = ExecProcNode((PlanState *) linitial(css->custom_ps));

	if (TupIsNull(slot))
		return NULL;

	if (css->ss.ps.ps_ProjInfo != NULL)
	{
		ExprContext *econtext = css->ss.ps.ps_ExprContext;

		ResetExprContext(econtext);
		econtext->ecxt_scantuple = slot;
		return ExecProject(css->ss.ps.ps_ProjInfo);
	}

	return slot;
}

static void
IvfIncludeReScan(CustomScanState *css)
{
	PlanState  *child = (PlanState *) linitial(css->custom_ps);

	if (css->ss.ps.chgParam != NULL)
		UpdateChangedParamSet(child, css->ss.ps.chgParam);

	if (child->chgParam == NULL)
		ExecReScan(child);
}

static void
IvfIncludeEnd(CustomScanState *css)
{
	IvfIncludeScanState *node = (IvfIncludeScanState *) css;
	IvfflatIncludeFilter *filter = node->filter;

	UninjectIncludeFilter(node);
	ExecEndNode((PlanState *) linitial(css->custom_ps));

	if (filter == NULL)
		return;

	for (int i = 0; i < filter->ncolumns; i++)
	{
		IvfflatIncludeColumn *col = &filter->columns[i];

		if (col->slot != NULL)
			ExecDropSingleTupleTableSlot(col->slot);
		if (col->tupdesc != NULL)
			FreeTupleDesc(col->tupdesc);
	}

	if (filter->econtext != NULL)
		FreeExprContext(filter->econtext, false);

	node->filter = NULL;
}

static void
IvfIncludeExplain(CustomScanState *css, List *ancestors, ExplainState *es)
{
	IvfIncludeScanState *node = (IvfIncludeScanState *) css;
	CustomScan *cscan = (CustomScan *) css->ss.ps.plan;
	List	   *displayQuals = (List *) lsecond(cscan->custom_exprs);

	if (displayQuals != NIL)
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
		qualexpr = (Node *) make_ands_explicit(displayQuals);
		useprefix = list_length(es->rtable) > 1 || es->verbose;
		exprstr = deparse_expression(qualexpr, context, useprefix, false);
		ExplainPropertyText("Include Filter", exprstr, es);
	}

	if (es->analyze && node->filter != NULL && node->filter->fallbackReason != NULL)
		ExplainPropertyText("Include Fallback", node->filter->fallbackReason, es);
}

void
IvfflatIncludeInit(void)
{
	prev_set_rel_pathlist_hook = set_rel_pathlist_hook;
	set_rel_pathlist_hook = IvfflatIncludePathlistHook;
	RegisterCustomScanMethods(&ivf_include_scan_methods);
}
