#include "postgres.h"

#include <float.h>
#include <math.h>

#include "access/amapi.h"
#include "access/genam.h"
#include "access/itup.h"
#include "access/relscan.h"
#include "access/tupdesc.h"
#include "catalog/pg_operator_d.h"
#include "catalog/pg_type_d.h"
#include "fmgr.h"
#include "lib/pairingheap.h"
#include "common/hashfn.h"
#include "executor/executor.h"
#include "ivfflat.h"
#include "ivfinclude.h"
#include "miscadmin.h"
#include "optimizer/optimizer.h"
#include "pgstat.h"
#include "storage/bufmgr.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/tuplesort.h"

#if PG_VERSION_NUM >= 160000
#include "varatt.h"
#endif

#define GetScanList(ptr) pairingheap_container(IvfflatScanList, ph_node, ptr)
#define GetScanListConst(ptr) pairingheap_const_container(IvfflatScanList, ph_node, ptr)

/*
 * Passing entry ids for one list. A bitmap is used when it fits and the list
 * is not mostly holes; otherwise a hash of the ids that passed, until that
 * exceeds the budget and the scan stops filtering.
 */
typedef struct IdHashEntry
{
	uint32		key;
	char		status;
}			IdHashEntry;

static inline uint32
IdHashKey(uint32 key)
{
	return murmurhash32(key);
}

#define SH_PREFIX idset
#define SH_ELEMENT_TYPE IdHashEntry
#define SH_KEY_TYPE uint32
#define SH_KEY key
#define SH_HASH_KEY(tb, key) IdHashKey(key)
#define SH_EQUAL(tb, a, b) ((a) == (b))
#define SH_SCOPE static inline
#define SH_DECLARE
#define SH_DEFINE
#include "lib/simplehash.h"

typedef enum PassKind
{
	PASS_NONE = 0,
	PASS_BITMAP,
	PASS_HASH,
	PASS_ALL
}			PassKind;

typedef struct PassSet
{
	PassKind	kind;
	uint32		hwm;
	uint32		maxEntries;
	uint32		members;
	unsigned char *bits;
	idset_hash *hash;
}			PassSet;

/*
 * Compare list distances
 */
static int
CompareLists(const pairingheap_node *a, const pairingheap_node *b, void *arg)
{
	if (GetScanListConst(a)->distance > GetScanListConst(b)->distance)
		return 1;

	if (GetScanListConst(a)->distance < GetScanListConst(b)->distance)
		return -1;

	return 0;
}

/*
 * Get lists and sort by distance
 */
static void
GetScanLists(IndexScanDesc scan, Datum value)
{
	IvfflatScanOpaque so = (IvfflatScanOpaque) scan->opaque;
	BlockNumber nextblkno = IVFFLAT_HEAD_BLKNO;
	int			listCount = 0;
	double		maxDistance = DBL_MAX;

	/* Search all list pages */
	while (BlockNumberIsValid(nextblkno))
	{
		Buffer		cbuf;
		Page		cpage;
		OffsetNumber maxoffno;
		BlockNumber listBlk = nextblkno;

		cbuf = ReadBuffer(scan->indexRelation, nextblkno);
		LockBuffer(cbuf, BUFFER_LOCK_SHARE);
		cpage = BufferGetPage(cbuf);

		maxoffno = PageGetMaxOffsetNumber(cpage);

		for (OffsetNumber offno = FirstOffsetNumber; offno <= maxoffno; offno = OffsetNumberNext(offno))
		{
			IvfflatList list = (IvfflatList) PageGetItem(cpage, PageGetItemId(cpage, offno));
			Pointer		center;
			double		distance;

			if (so->version == IVFFLAT_VERSION_INCLUDE)
				center = (Pointer) IvfflatListGetCenter((IvfflatListDataV2 *) list, so->includeCount);
			else
				center = (Pointer) &list->center;

			/* Use procinfo from the index instead of scan key for performance */
			distance = DatumGetFloat8(so->distfunc(so->procinfo, so->collation, PointerGetDatum(center), value));

			if (listCount < so->maxProbes)
			{
				IvfflatScanList *scanlist;

				scanlist = &so->lists[listCount];
				scanlist->startPage = list->startPage;
				scanlist->listBlkno = listBlk;
				scanlist->listOffno = offno;
				scanlist->distance = distance;
				listCount++;

				/* Add to heap */
				pairingheap_add(so->listQueue, &scanlist->ph_node);

				/* Calculate max distance */
				if (listCount == so->maxProbes)
					maxDistance = GetScanList(pairingheap_first(so->listQueue))->distance;
			}
			else if (distance < maxDistance)
			{
				IvfflatScanList *scanlist;

				/* Remove */
				scanlist = GetScanList(pairingheap_remove_first(so->listQueue));

				/* Reuse */
				scanlist->startPage = list->startPage;
				scanlist->listBlkno = listBlk;
				scanlist->listOffno = offno;
				scanlist->distance = distance;
				pairingheap_add(so->listQueue, &scanlist->ph_node);

				/* Update max distance */
				maxDistance = GetScanList(pairingheap_first(so->listQueue))->distance;
			}
		}

		nextblkno = IvfflatPageGetOpaque(cpage)->nextblkno;

		UnlockReleaseBuffer(cbuf);
	}

	for (int i = listCount - 1; i >= 0; i--)
	{
		IvfflatScanList *picked = GetScanList(pairingheap_remove_first(so->listQueue));

		so->listPages[i].startPage = picked->startPage;
		so->listPages[i].listBlkno = picked->listBlkno;
		so->listPages[i].listOffno = picked->listOffno;
	}

	Assert(pairingheap_is_empty(so->listQueue));
}

static PassSet *
PassCreate(uint32 hwm, uint32 live, Size budget, const char **fallback)
{
	PassSet    *set = palloc0_object(PassSet);
	Size		bytes;

	set->hwm = hwm;

	if (hwm == 0)
	{
		set->kind = PASS_NONE;
		return set;
	}

	bytes = ((Size) hwm + 7) / 8;

	if (bytes <= budget && (uint64) hwm <= 4ull * (uint64) live)
	{
		set->kind = PASS_BITMAP;
		set->bits = palloc0(bytes);
		return set;
	}

	set->maxEntries = (uint32) (budget / 16);

	if (set->maxEntries == 0)
	{
		set->kind = PASS_ALL;
		*fallback = "memory";
		return set;
	}

	set->kind = PASS_HASH;
	set->hash = idset_create(CurrentMemoryContext, 128, NULL);
	return set;
}

static PassSet *
PassEmptyLike(PassSet *src)
{
	PassSet    *set = palloc0_object(PassSet);

	set->kind = src->kind;
	set->hwm = src->hwm;
	set->maxEntries = src->maxEntries;

	if (set->kind == PASS_BITMAP)
		set->bits = palloc0(((Size) set->hwm + 7) / 8);
	else if (set->kind == PASS_HASH)
		set->hash = idset_create(CurrentMemoryContext, 128, NULL);

	return set;
}

static void
PassFree(PassSet *set)
{
	if (set == NULL)
		return;

	if (set->bits != NULL)
		pfree(set->bits);
	if (set->hash != NULL)
		idset_destroy(set->hash);
	pfree(set);
}

static bool
PassAdd(PassSet *set, uint32 id)
{
	if (set->kind == PASS_BITMAP)
	{
		if (id < set->hwm)
		{
			unsigned char mask = (unsigned char) (1u << (id & 7));

			if ((set->bits[id >> 3] & mask) == 0)
			{
				set->bits[id >> 3] |= mask;
				set->members++;
			}
		}
		return true;
	}

	if (set->kind == PASS_HASH)
	{
		bool		found;

		if (set->members >= set->maxEntries)
		{
			if (idset_lookup(set->hash, id) != NULL)
				return true;
			return false;
		}

		idset_insert(set->hash, id, &found);
		if (!found)
			set->members++;
		return true;
	}

	return true;
}

static bool
PassHas(PassSet *set, uint32 id)
{
	if (set == NULL || set->kind == PASS_ALL)
		return true;
	if (set->kind == PASS_NONE || set->members == 0 || id >= set->hwm)
		return false;
	if (set->kind == PASS_BITMAP)
		return (set->bits[id >> 3] & (1u << (id & 7))) != 0;

	return idset_lookup(set->hash, id) != NULL;
}

static bool
PassRange(PassSet *set, uint32 minid, uint32 maxid, uint16 live)
{
	if (live == 0)
		return false;
	if (set == NULL || set->kind == PASS_ALL)
		return true;
	if (set->kind == PASS_NONE || set->members == 0 || minid > maxid)
		return false;

	if (set->kind == PASS_BITMAP)
	{
		uint32		id;

		if (minid >= set->hwm)
			return false;
		if (maxid >= set->hwm)
			maxid = set->hwm - 1;

		for (id = minid; id <= maxid; id++)
		{
			if (PassHas(set, id))
				return true;
		}

		return false;
	}

	{
		idset_iterator iter;
		IdHashEntry *ent;

		idset_start_iterate(set->hash, &iter);
		while ((ent = idset_iterate(set->hash, &iter)) != NULL)
		{
			if (ent->key >= minid && ent->key <= maxid)
				return true;
		}
	}

	return false;
}

/* True when the set passes no id. NULL means no restriction. */
static bool
PassIsEmpty(PassSet *set)
{
	if (set == NULL || set->kind == PASS_ALL)
		return false;

	return set->kind == PASS_NONE || set->members == 0;
}

/*
 * Score the entries on one vector page. Version-2 items are unwrapped first.
 * checkSet skips ids the column chains rejected. The TID prefilter is
 * independent and still applies.
 */
static void
ScoreVectorPage(IndexScanDesc scan, Page page, Datum value, PassSet *set, bool checkSet)
{
	IvfflatScanOpaque so = (IvfflatScanOpaque) scan->opaque;
	TupleDesc	tupdesc = RelationGetDescr(scan->indexRelation);
	TupleTableSlot *slot = so->vslot;
	OffsetNumber maxoffno = PageGetMaxOffsetNumber(page);
	bool		version2 = so->version == IVFFLAT_VERSION_INCLUDE;

	for (OffsetNumber offno = FirstOffsetNumber; offno <= maxoffno; offno = OffsetNumberNext(offno))
	{
		Item		item = PageGetItem(page, PageGetItemId(page, offno));
		IndexTuple	itup;
		Datum		datum;
		bool		isnull;

		if (version2)
		{
			if (checkSet && !PassHas(set, IvfflatEntryGetId(item)))
				continue;

			itup = IvfflatEntryGetTuple(item);
		}
		else
			itup = (IndexTuple) item;

		if (so->prefilter != NULL &&
			!IvfflatPrefilterContains(so->prefilter, &itup->t_tid))
			continue;

		datum = index_getattr(itup, 1, tupdesc, &isnull);

		if (isnull)
			continue;

		ExecClearTuple(slot);
		slot->tts_values[0] = so->distfunc(so->procinfo, so->collation, datum, value);
		slot->tts_isnull[0] = false;
		slot->tts_values[1] = PointerGetDatum(&itup->t_tid);
		slot->tts_isnull[1] = false;
		ExecStoreVirtualTuple(slot);

		tuplesort_puttupleslot(so->sortstate, slot);
	}
}

static void
ScanVectorChain(IndexScanDesc scan, BlockNumber searchPage, Datum value, PassSet *set, bool checkSet)
{
	IvfflatScanOpaque so = (IvfflatScanOpaque) scan->opaque;

	while (BlockNumberIsValid(searchPage))
	{
		Buffer		buf;
		Page		page;

		buf = ReadBufferExtended(scan->indexRelation, MAIN_FORKNUM, searchPage, RBM_NORMAL, so->bas);
		LockBuffer(buf, BUFFER_LOCK_SHARE);
		page = BufferGetPage(buf);
		ScoreVectorPage(scan, page, value, set, checkSet);
		searchPage = IvfflatPageGetOpaque(page)->nextblkno;
		UnlockReleaseBuffer(buf);
	}
}

/*
 * Count directory hits, then either walk the vector chain or read the
 * surviving pages. pages/hits are filled on the counting pass.
 */
static void
ScanVectorsUsingDirectory(IndexScanDesc scan, BlockNumber startPage, BlockNumber dirStart,
						  PassSet *set, Datum value)
{
	IvfflatScanOpaque so = (IvfflatScanOpaque) scan->opaque;
	double		seqCost = seq_page_cost;
	double		localCost = seq_page_cost + 0.5 * (random_page_cost - seq_page_cost);
	double		pages = 0;
	double		hits = 0;
	BlockNumber dirBlk = dirStart;

	if (!BlockNumberIsValid(dirStart))
		return;
	if (PassIsEmpty(set))
		return;

	while (BlockNumberIsValid(dirBlk))
	{
		Buffer		buf;
		Page		page;
		OffsetNumber maxoffno;

		buf = ReadBufferExtended(scan->indexRelation, MAIN_FORKNUM, dirBlk, RBM_NORMAL, so->bas);
		LockBuffer(buf, BUFFER_LOCK_SHARE);
		page = BufferGetPage(buf);
		maxoffno = PageGetMaxOffsetNumber(page);

		for (OffsetNumber offno = FirstOffsetNumber; offno <= maxoffno; offno = OffsetNumberNext(offno))
		{
			IvfflatVectorDirEntry *ent = (IvfflatVectorDirEntry *) PageGetItem(page, PageGetItemId(page, offno));

			pages++;
			if (ent->live_items > 0 &&
				PassRange(set, ent->min_entry_id, ent->max_entry_id, ent->live_items))
				hits++;
		}

		dirBlk = IvfflatPageGetOpaque(page)->nextblkno;
		UnlockReleaseBuffer(buf);
	}

	if (pages == 0)
		return;

	if (hits * localCost >= pages * seqCost)
	{
		ScanVectorChain(scan, startPage, value, set, true);
		return;
	}

	dirBlk = dirStart;

	while (BlockNumberIsValid(dirBlk))
	{
		Buffer		buf;
		Page		page;
		OffsetNumber maxoffno;

		buf = ReadBufferExtended(scan->indexRelation, MAIN_FORKNUM, dirBlk, RBM_NORMAL, so->bas);
		LockBuffer(buf, BUFFER_LOCK_SHARE);
		page = BufferGetPage(buf);
		maxoffno = PageGetMaxOffsetNumber(page);

		for (OffsetNumber offno = FirstOffsetNumber; offno <= maxoffno; offno = OffsetNumberNext(offno))
		{
			IvfflatVectorDirEntry *ent = (IvfflatVectorDirEntry *) PageGetItem(page, PageGetItemId(page, offno));
			Buffer		vbuf;
			Page		vpage;

			if (ent->live_items == 0 ||
				!PassRange(set, ent->min_entry_id, ent->max_entry_id, ent->live_items))
				continue;

			vbuf = ReadBufferExtended(scan->indexRelation, MAIN_FORKNUM, ent->blkno, RBM_NORMAL, so->bas);
			LockBuffer(vbuf, BUFFER_LOCK_SHARE);
			vpage = BufferGetPage(vbuf);
			ScoreVectorPage(scan, vpage, value, set, true);
			UnlockReleaseBuffer(vbuf);
		}

		dirBlk = IvfflatPageGetOpaque(page)->nextblkno;
		UnlockReleaseBuffer(buf);
	}
}

static void
CopyListHeaderV2(Relation index, IvfflatProbeList *ref, int includeCount,
				 IvfflatListDataV2 *header, IvfflatIncludeChain *chains)
{
	Buffer		buf = ReadBuffer(index, ref->listBlkno);
	Page		page;
	IvfflatListDataV2 *list;

	LockBuffer(buf, BUFFER_LOCK_SHARE);
	page = BufferGetPage(buf);
	list = (IvfflatListDataV2 *) PageGetItem(page, PageGetItemId(page, ref->listOffno));
	memcpy(header, list, sizeof(IvfflatListDataV2));

	if (includeCount > 0)
		memcpy(chains, IvfflatListGetChains(list), includeCount * sizeof(IvfflatIncludeChain));

	UnlockReleaseBuffer(buf);
}

/* Chain walk state for one list */
typedef struct IncludeEval
{
	IndexScanDesc scan;
	IvfflatIncludeFilter *filter;
	IvfflatIncludeChain *chains;
	uint32		hwm;
	PassSet    *proto;			/* kind and capacity of scratch sets */
}			IncludeEval;

/*
 * Walk the leaf's chain and add to out each id in cand (every id when NULL)
 * whose value passes the leaf quals. Ids in out or done are skipped. False
 * when out is over budget.
 */
static bool
EvalIncludeLeaf(IncludeEval *ev, IvfflatIncludeNode *node, PassSet *cand,
				PassSet *done, PassSet *out)
{
	IvfflatScanOpaque so = (IvfflatScanOpaque) ev->scan->opaque;
	IvfflatIncludeColumn *col = &ev->filter->columns[node->chainIndex];
	ExprContext *econtext = ev->filter->econtext;
	BlockNumber searchPage = ev->chains[node->chainIndex].startPage;

	while (BlockNumberIsValid(searchPage))
	{
		Buffer		buf;
		Page		page;
		OffsetNumber maxoffno;

		CHECK_FOR_INTERRUPTS();

		buf = ReadBufferExtended(ev->scan->indexRelation, MAIN_FORKNUM, searchPage, RBM_NORMAL, so->bas);
		LockBuffer(buf, BUFFER_LOCK_SHARE);
		page = BufferGetPage(buf);
		maxoffno = PageGetMaxOffsetNumber(page);
		ResetExprContext(econtext);

		for (OffsetNumber offno = FirstOffsetNumber; offno <= maxoffno; offno = OffsetNumberNext(offno))
		{
			Item		item = PageGetItem(page, PageGetItemId(page, offno));
			uint32		entry_id = IvfflatEntryGetId(item);
			Datum		datum;
			bool		isnull;

			if (entry_id >= ev->hwm)
				continue;
			if (cand != NULL && !PassHas(cand, entry_id))
				continue;
			if (PassHas(out, entry_id) || (done != NULL && PassHas(done, entry_id)))
				continue;

			datum = index_getattr(IvfflatEntryGetTuple(item), 1, col->tupdesc, &isnull);
			ExecClearTuple(col->slot);
			col->slot->tts_values[0] = datum;
			col->slot->tts_isnull[0] = isnull;
			ExecStoreVirtualTuple(col->slot);
			econtext->ecxt_scantuple = col->slot;

			if (!ExecQual(node->qual, econtext))
				continue;

			if (!PassAdd(out, entry_id))
			{
				UnlockReleaseBuffer(buf);
				return false;
			}
		}

		searchPage = IvfflatPageGetOpaque(page)->nextblkno;
		UnlockReleaseBuffer(buf);
	}

	return true;
}

/*
 * Add to out each id in cand (every id when NULL) that passes node. Ids in
 * done are already accepted higher up and are skipped. An AND narrows a
 * scratch set child by child and its last child writes to out. False when a
 * set is over budget.
 */
static bool
EvalIncludeNode(IncludeEval *ev, IvfflatIncludeNode *node, PassSet *cand,
				PassSet *done, PassSet *out)
{
	PassSet    *cur;
	int			last;
	bool		ok = true;

	check_stack_depth();

	if (PassIsEmpty(cand))
		return true;

	if (node->kind == IVF_INCLUDE_LEAF)
		return EvalIncludeLeaf(ev, node, cand, done, out);

	if (node->kind == IVF_INCLUDE_OR)
	{
		for (int i = 0; i < node->nchildren; i++)
		{
			if (!EvalIncludeNode(ev, &node->children[i], cand, done, out))
				return false;
		}

		return true;
	}

	cur = cand;
	last = node->nchildren - 1;

	for (int i = 0; i < last && ok && !PassIsEmpty(cur); i++)
	{
		PassSet    *next = PassEmptyLike(ev->proto);

		/* Ids already in out need no more work at this level */
		ok = EvalIncludeNode(ev, &node->children[i], cur, out, next);
		if (cur != cand)
			PassFree(cur);
		cur = next;
	}

	if (ok && !PassIsEmpty(cur))
		ok = EvalIncludeNode(ev, &node->children[last], cur, done, out);

	if (cur != cand)
		PassFree(cur);

	return ok;
}

/*
 * Build the passing set for one list and score the vectors that survive.
 * A budget miss scores every vector in the list and lets the child Filter
 * reject them.
 */
static void
ScanIncludedList(IndexScanDesc scan, IvfflatProbeList *ref, Datum value)
{
	IvfflatScanOpaque so = (IvfflatScanOpaque) scan->opaque;
	IvfflatIncludeFilter *filter = so->includeFilter;
	IvfflatListDataV2 header;
	IncludeEval ev;
	PassSet    *set;
	const char *fallback = NULL;

	ev.scan = scan;
	ev.filter = filter;
	ev.chains = palloc0_array_checked(IvfflatIncludeChain, so->includeCount);
	CopyListHeaderV2(scan->indexRelation, ref, so->includeCount, &header, ev.chains);
	ev.hwm = header.entry_hwm;

	set = PassCreate(header.entry_hwm, header.live_count, IvfflatIncludeMemBudget(), &fallback);
	ev.proto = set;

	if (fallback != NULL && filter->fallbackReason == NULL)
		filter->fallbackReason = fallback;

	if (set->kind == PASS_ALL)
		ScanVectorChain(scan, header.startPage, value, NULL, false);
	else if (set->kind != PASS_NONE)
	{
		if (EvalIncludeNode(&ev, filter->root, NULL, NULL, set))
			ScanVectorsUsingDirectory(scan, header.startPage, header.dirStartPage, set, value);
		else
		{
			if (filter->fallbackReason == NULL)
				filter->fallbackReason = "memory";
			ScanVectorChain(scan, header.startPage, value, NULL, false);
		}
	}

	PassFree(set);
	pfree(ev.chains);
}

/*
 * Get items
 */
static void
GetScanItems(IndexScanDesc scan, Datum value)
{
	IvfflatScanOpaque so = (IvfflatScanOpaque) scan->opaque;
	int			batchProbes = 0;
	bool		useInclude = so->includeFilter != NULL && so->includeFilter->root != NULL;

	if (useInclude && !ivfflat_include_filter)
	{
		if (so->includeFilter->fallbackReason == NULL)
			so->includeFilter->fallbackReason = "disabled";
		useInclude = false;
	}

	tuplesort_reset(so->sortstate);

	/* Search closest probes lists */
	while (so->listIndex < so->maxProbes && (++batchProbes) <= so->probes)
	{
		IvfflatProbeList ref = so->listPages[so->listIndex++];

		if (so->version == IVFFLAT_VERSION_INCLUDE && useInclude)
			ScanIncludedList(scan, &ref, value);
		else
			ScanVectorChain(scan, ref.startPage, value, NULL, false);
	}

	tuplesort_performsort(so->sortstate);

#if defined(IVFFLAT_MEMORY)
	elog(INFO, "memory: %zu MB", MemoryContextMemAllocated(CurrentMemoryContext, true) / (1024 * 1024));
#endif
}

/*
 * Zero distance
 */
static Datum
ZeroDistance(FmgrInfo *flinfo, Oid collation, Datum arg1, Datum arg2)
{
	return Float8GetDatum(0.0);
}

/*
 * Get scan value
 */
static Datum
GetScanValue(IndexScanDesc scan)
{
	IvfflatScanOpaque so = (IvfflatScanOpaque) scan->opaque;
	Datum		value;

	if (scan->orderByData->sk_flags & SK_ISNULL)
	{
		value = PointerGetDatum(NULL);
		so->distfunc = ZeroDistance;
	}
	else
	{
		value = scan->orderByData->sk_argument;
		so->distfunc = FunctionCall2Coll;

		/* Value should not be compressed or toasted */
		Assert(!VARATT_IS_COMPRESSED(DatumGetPointer(value)));
		Assert(!VARATT_IS_EXTENDED(DatumGetPointer(value)));

		/* Normalize if needed */
		if (so->normprocinfo != NULL)
		{
			MemoryContext oldCtx = MemoryContextSwitchTo(so->tmpCtx);

			value = IvfflatNormValue(so->typeInfo, so->collation, value);

			MemoryContextSwitchTo(oldCtx);
		}

		/* Check dimensions match index */
		IvfflatCheckDim(so->dimensions, so->typeInfo, so->collation, value);
	}

	return value;
}

/*
 * Initialize scan sort state
 */
static Tuplesortstate *
InitScanSortState(TupleDesc tupdesc)
{
	AttrNumber	attNums[] = {1};
	Oid			sortOperators[] = {Float8LessOperator};
	Oid			sortCollations[] = {InvalidOid};
	bool		nullsFirstFlags[] = {false};

	return tuplesort_begin_heap(tupdesc, 1, attNums, sortOperators, sortCollations, nullsFirstFlags, work_mem, NULL, false);
}

/*
 * Prepare for an index scan
 */
IndexScanDesc
ivfflatbeginscan(Relation index, int nkeys, int norderbys)
{
	IndexScanDesc scan;
	IvfflatScanOpaque so;
	int			lists;
	int			dimensions;
	int			probes = ivfflat_probes;
	int			maxProbes;
	MemoryContext oldCtx;
	IvfflatMetaInfo meta;

	scan = RelationGetIndexScan(index, nkeys, norderbys);

	/* Get lists and dimensions from metapage */
	IvfflatReadMeta(index, &meta);
	lists = meta.lists;
	dimensions = meta.dimensions;

	if (ivfflat_iterative_scan != IVFFLAT_ITERATIVE_SCAN_OFF)
		maxProbes = Max(ivfflat_max_probes, probes);
	else
		maxProbes = probes;

	if (probes > lists)
		probes = lists;

	if (maxProbes > lists)
		maxProbes = lists;

	so = palloc_object(IvfflatScanOpaqueData);
	so->typeInfo = IvfflatGetTypeInfo(index);
	so->first = true;
	so->probes = probes;
	so->maxProbes = maxProbes;
	so->dimensions = dimensions;
	so->value = PointerGetDatum(NULL);

	/*
	 * Owned and managed exclusively by the IvfPrefilterScan custom node;
	 * nothing else in the AM (including ivfflatrescan) writes it
	 */
	so->prefilter = NULL;
	so->includeFilter = NULL;
	so->version = meta.version;
	so->includeCount = meta.includeCount;

	/* Set support functions */
	so->procinfo = index_getprocinfo(index, 1, IVFFLAT_DISTANCE_PROC);
	so->normprocinfo = IvfflatOptionalProcInfo(index, IVFFLAT_NORM_PROC);
	so->collation = index->rd_indcollation[0];

	so->tmpCtx = AllocSetContextCreate(CurrentMemoryContext,
									   "Ivfflat scan temporary context",
									   ALLOCSET_DEFAULT_SIZES);

	oldCtx = MemoryContextSwitchTo(so->tmpCtx);

	/* Create tuple description for sorting */
	so->tupdesc = CreateTemplateTupleDesc(2);
	TupleDescInitEntry(so->tupdesc, (AttrNumber) 1, "distance", FLOAT8OID, -1, 0);
	TupleDescInitEntry(so->tupdesc, (AttrNumber) 2, "heaptid", TIDOID, -1, 0);
#if PG_VERSION_NUM >= 190000
	TupleDescFinalize(so->tupdesc);
#endif

	/* Prep sort */
	so->sortstate = InitScanSortState(so->tupdesc);

	/* Need separate slots for puttuple and gettuple */
	so->vslot = MakeSingleTupleTableSlot(so->tupdesc, &TTSOpsVirtual);
	so->mslot = MakeSingleTupleTableSlot(so->tupdesc, &TTSOpsMinimalTuple);

	/*
	 * Reuse same set of shared buffers for scan
	 *
	 * See postgres/src/backend/storage/buffer/README for description
	 */
	so->bas = GetAccessStrategy(BAS_BULKREAD);

	so->listQueue = pairingheap_allocate(CompareLists, scan);
	so->listPages = palloc_array_checked(IvfflatProbeList, maxProbes);
	so->listIndex = 0;
	so->lists = palloc_array_checked(IvfflatScanList, maxProbes);

	MemoryContextSwitchTo(oldCtx);

	scan->opaque = so;

	return scan;
}

/*
 * Start or restart an index scan
 */
void
ivfflatrescan(IndexScanDesc scan, ScanKey keys, int nkeys, ScanKey orderbys, int norderbys)
{
	IvfflatScanOpaque so = (IvfflatScanOpaque) scan->opaque;

	so->first = true;
	pairingheap_reset(so->listQueue);
	so->listIndex = 0;

	if (so->normprocinfo != NULL && DatumGetPointer(so->value) != NULL)
	{
		pfree(DatumGetPointer(so->value));
		so->value = PointerGetDatum(NULL);
	}

	if (keys && scan->numberOfKeys > 0)
		memmove(scan->keyData, keys, scan->numberOfKeys * sizeof(ScanKeyData));

	if (orderbys && scan->numberOfOrderBys > 0)
		memmove(scan->orderByData, orderbys, scan->numberOfOrderBys * sizeof(ScanKeyData));
}

/*
 * Fetch the next tuple in the given scan
 */
bool
ivfflatgettuple(IndexScanDesc scan, ScanDirection dir)
{
	IvfflatScanOpaque so = (IvfflatScanOpaque) scan->opaque;
	ItemPointer heaptid;
	bool		isnull;

	/*
	 * Index can be used to scan backward, but Postgres doesn't support
	 * backward scan on operators
	 */
	Assert(ScanDirectionIsForward(dir));

	if (so->first)
	{
		Datum		value;

		/* Count index scan for stats */
		pgstat_count_index_scan(scan->indexRelation);
#if PG_VERSION_NUM >= 180000
		if (scan->instrument)
			scan->instrument->nsearches++;
#endif

		/* Safety check */
		if (scan->orderByData == NULL)
			elog(ERROR, "cannot scan ivfflat index without order");

		/* Requires MVCC-compliant snapshot as not able to pin during sorting */
		/* https://www.postgresql.org/docs/current/index-locking.html */
		if (!IsMVCCSnapshot(scan->xs_snapshot))
			elog(ERROR, "non-MVCC snapshots are not supported with ivfflat");

		value = GetScanValue(scan);
		IvfflatBench("GetScanLists", GetScanLists(scan, value));
		IvfflatBench("GetScanItems", GetScanItems(scan, value));
		so->first = false;
		so->value = value;
	}

	while (!tuplesort_gettupleslot(so->sortstate, true, false, so->mslot, NULL))
	{
		if (so->listIndex == so->maxProbes)
			return false;

		IvfflatBench("GetScanItems", GetScanItems(scan, so->value));
	}

	heaptid = (ItemPointer) DatumGetPointer(slot_getattr(so->mslot, 2, &isnull));

	scan->xs_heaptid = *heaptid;
	scan->xs_recheck = false;
	scan->xs_recheckorderby = false;
	return true;
}

/*
 * Install (or clear) a prefilter TID set for a scan
 *
 * Used only by the IvfPrefilterScan custom node; a failure here would be a
 * pgvector bug, not a user error
 */
void
IvfflatScanSetPrefilter(IndexScanDesc scan, IvfflatPrefilter * pf)
{
	IvfflatScanOpaque so;

	if (scan == NULL || scan->indexRelation == NULL ||
		scan->indexRelation->rd_indam == NULL ||
		scan->indexRelation->rd_indam->ambeginscan != ivfflatbeginscan)
		elog(ERROR, "cannot set prefilter on non-ivfflat scan");

	so = (IvfflatScanOpaque) scan->opaque;
	so->prefilter = pf;
}

/*
 * Install the included-column filter for this scan. Only the include custom
 * scan writes this pointer. NULL clears it.
 */
void
IvfflatScanSetIncludeFilter(IndexScanDesc scan, IvfflatIncludeFilter *filter)
{
	IvfflatScanOpaque so;

	if (scan == NULL || scan->indexRelation == NULL ||
		scan->indexRelation->rd_indam == NULL ||
		scan->indexRelation->rd_indam->ambeginscan != ivfflatbeginscan)
		elog(ERROR, "cannot set include filter on non-ivfflat scan");

	so = (IvfflatScanOpaque) scan->opaque;
	so->includeFilter = filter;
}

/*
 * End a scan and release resources
 */
void
ivfflatendscan(IndexScanDesc scan)
{
	IvfflatScanOpaque so = (IvfflatScanOpaque) scan->opaque;

	/* Free any temporary files */
	tuplesort_end(so->sortstate);

	MemoryContextDelete(so->tmpCtx);

	pfree(so);
	scan->opaque = NULL;
}
