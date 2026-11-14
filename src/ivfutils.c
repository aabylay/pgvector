#include "postgres.h"

#include "access/genam.h"
#include "access/generic_xlog.h"
#include "access/itup.h"
#include "access/tupdesc.h"
#include "fmgr.h"
#include "halfutils.h"
#include "halfvec.h"
#include "ivfflat.h"
#include "miscadmin.h"
#include "storage/bufmgr.h"
#include "utils/fmgrprotos.h"
#include "utils/memutils.h"
#include "utils/relcache.h"
#include "utils/varbit.h"
#include "vector.h"

#if PG_VERSION_NUM >= 160000
#include "varatt.h"
#endif

/*
 * Allocate a vector array
 */
VectorArray
VectorArrayInit(int maxlen, int dimensions, Size itemsize)
{
	VectorArray res;

	/* Safety check */
	if (maxlen < 1 || dimensions < 1 || itemsize == 0)
		elog(ERROR, "cannot create vector array");

	/* Ensure items are aligned to prevent UB */
	itemsize = MAXALIGN(itemsize);

	res = palloc_object(VectorArrayData);
	res->length = 0;
	res->maxlen = maxlen;
	res->dim = dimensions;
	res->itemsize = itemsize;
	res->items = palloc_extended(mul_size(maxlen, itemsize), MCXT_ALLOC_ZERO | MCXT_ALLOC_HUGE);
	return res;
}

/*
 * Free a vector array
 */
void
VectorArrayFree(VectorArray arr)
{
	pfree(arr->items);
	pfree(arr);
}

/*
 * Get the number of lists in the index
 */
int
IvfflatGetLists(Relation index)
{
	IvfflatOptions *opts = (IvfflatOptions *) index->rd_options;

	if (opts)
		return opts->lists;

	return IVFFLAT_DEFAULT_LISTS;
}

/*
 * Get proc
 */
FmgrInfo *
IvfflatOptionalProcInfo(Relation index, uint16 procnum)
{
	if (!OidIsValid(index_getprocid(index, 1, procnum)))
		return NULL;

	return index_getprocinfo(index, 1, procnum);
}

/*
 * Normalize value
 */
Datum
IvfflatNormValue(const IvfflatTypeInfo * typeInfo, Oid collation, Datum value)
{
	return DirectFunctionCall1Coll(typeInfo->normalize, collation, value);
}

/*
 * Check if non-zero norm
 */
bool
IvfflatCheckNorm(FmgrInfo *procinfo, Oid collation, Datum value)
{
	return DatumGetFloat8(FunctionCall1Coll(procinfo, collation, value)) > 0;
}

/*
 * Normalize vectors
 */
void
IvfflatNormVectors(const IvfflatTypeInfo * typeInfo, Oid collation, VectorArray arr, MemoryContext tmpCtx)
{
	MemoryContext oldCtx = MemoryContextSwitchTo(tmpCtx);

	for (int i = 0; i < arr->length; i++)
	{
		Datum		value = PointerGetDatum(VectorArrayGet(arr, i));
		Datum		newValue = IvfflatNormValue(typeInfo, collation, value);

		/* Safety check */
		IvfflatCheckDim(arr->dim, typeInfo, collation, newValue);

		VectorArraySet(arr, i, DatumGetPointer(newValue));
		MemoryContextReset(tmpCtx);
	}

	MemoryContextSwitchTo(oldCtx);
}

/*
 * Check memory usage
 */
void
IvfflatCheckMemoryUsage(Size totalSize)
{
	/* Add one to error message to ceil */
	if (totalSize / 1024 > (Size) maintenance_work_mem)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("memory required is %zu MB, maintenance_work_mem is %d MB",
						totalSize / (1024 * 1024) + 1, maintenance_work_mem / 1024)));
}

/*
 * New buffer
 */
Buffer
IvfflatNewBuffer(Relation index, ForkNumber forkNum)
{
#if PG_VERSION_NUM >= 160000
	return ExtendBufferedRel(BMR_REL(index), forkNum, NULL, EB_LOCK_FIRST | EB_SKIP_EXTENSION_LOCK);
#else
	Buffer		buf = ReadBufferExtended(index, forkNum, P_NEW, RBM_NORMAL, NULL);

	LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
	return buf;
#endif
}

/*
 * Init page
 *
 * Version 2 uses a wider special area on every page, including the metapage
 * and list pages. Callers pass the index version; a version-1 index never
 * contains a version-2 page.
 */
void
IvfflatInitPage(Buffer buf, Page page, int version)
{
	if (version == IVFFLAT_VERSION_INCLUDE)
	{
		IvfflatPageOpaqueDataV2 *opaque;

		PageInit(page, BufferGetPageSize(buf), sizeof(IvfflatPageOpaqueDataV2));
		opaque = IvfflatPageGetOpaqueV2(page);
		opaque->nextblkno = InvalidBlockNumber;
		opaque->min_entry_id = 0;
		opaque->max_entry_id = 0;
		opaque->live_items = 0;
		opaque->page_id = IVFFLAT_PAGE_ID;
	}
	else
	{
		PageInit(page, BufferGetPageSize(buf), sizeof(IvfflatPageOpaqueData));
		IvfflatPageGetOpaque(page)->nextblkno = InvalidBlockNumber;
		IvfflatPageGetOpaque(page)->page_id = IVFFLAT_PAGE_ID;
	}
}

/*
 * Init and register page
 */
void
IvfflatInitRegisterPage(Relation index, Buffer *buf, Page *page, GenericXLogState **state, int version)
{
	*state = GenericXLogStart(index);
	*page = GenericXLogRegisterBuffer(*state, *buf, GENERIC_XLOG_FULL_IMAGE);
	IvfflatInitPage(*buf, *page, version);
}

/*
 * Commit buffer
 */
void
IvfflatCommitBuffer(Buffer buf, GenericXLogState *state)
{
	GenericXLogFinish(state);
	UnlockReleaseBuffer(buf);
}

/*
 * Add a new page
 *
 * The order is very important!!
 */
void
IvfflatAppendPage(Relation index, Buffer *buf, Page *page, GenericXLogState **state, ForkNumber forkNum, int version)
{
	/* Get new buffer */
	Buffer		newbuf = IvfflatNewBuffer(index, forkNum);
	Page		newpage = GenericXLogRegisterBuffer(*state, newbuf, GENERIC_XLOG_FULL_IMAGE);

	/* Update the previous buffer */
	IvfflatPageGetOpaque(*page)->nextblkno = BufferGetBlockNumber(newbuf);

	/* Init new page */
	IvfflatInitPage(newbuf, newpage, version);

	/* Commit */
	GenericXLogFinish(*state);

	/* Unlock */
	UnlockReleaseBuffer(*buf);

	*state = GenericXLogStart(index);
	*page = GenericXLogRegisterBuffer(*state, newbuf, GENERIC_XLOG_FULL_IMAGE);
	*buf = newbuf;
}

/*
 * Largest maxaligned item that fits on one page of this index version
 */
Size
IvfflatMaxItemSize(int version)
{
	Size		special = (version == IVFFLAT_VERSION_INCLUDE) ? sizeof(IvfflatPageOpaqueDataV2) : sizeof(IvfflatPageOpaqueData);
	Size		usable;

	usable = (Size) BLCKSZ - MAXALIGN(SizeOfPageHeaderData) - MAXALIGN(special);
	if (usable <= sizeof(ItemIdData))
		return 0;

	usable -= sizeof(ItemIdData);

	return usable - (usable % MAXIMUM_ALIGNOF);
}

/*
 * Read the metapage, choosing the struct from the version
 */
void
IvfflatReadMeta(Relation index, IvfflatMetaInfo *meta)
{
	Buffer		buf;
	Page		page;
	IvfflatMetaPage metap;

	buf = ReadBuffer(index, IVFFLAT_METAPAGE_BLKNO);
	LockBuffer(buf, BUFFER_LOCK_SHARE);
	page = BufferGetPage(buf);
	metap = IvfflatPageGetMeta(page);

	if (unlikely(metap->magicNumber != IVFFLAT_MAGIC_NUMBER))
		elog(ERROR, "ivfflat index is not valid");

	memset(meta, 0, sizeof(IvfflatMetaInfo));
	meta->version = metap->version;
	meta->lists = metap->lists;
	meta->dimensions = metap->dimensions;

	if (meta->version == IVFFLAT_VERSION_INCLUDE)
	{
		IvfflatMetaPageDataV2 *v2 = (IvfflatMetaPageDataV2 *) PageGetContents(page);

		if (v2->includeCount < 1 || v2->includeCount > IVFFLAT_MAX_INCLUDE_COLS)
			elog(ERROR, "ivfflat index is not valid");

		meta->includeCount = v2->includeCount;
		memcpy(meta->includeWidth, v2->includeWidth, sizeof(uint16) * v2->includeCount);
	}
	else if (meta->version != IVFFLAT_VERSION)
		elog(ERROR, "ivfflat index has unsupported version %u", meta->version);

	UnlockReleaseBuffer(buf);
}

/*
 * Record one entry on a version-2 page opaque
 */
void
IvfflatPageOpaqueAdd(Page page, uint32 entry_id)
{
	IvfflatPageOpaqueDataV2 *opaque = IvfflatPageGetOpaqueV2(page);

	if (opaque->live_items == 0)
	{
		opaque->min_entry_id = entry_id;
		opaque->max_entry_id = entry_id;
		opaque->live_items = 1;
	}
	else
	{
		if (entry_id < opaque->min_entry_id)
			opaque->min_entry_id = entry_id;
		if (entry_id > opaque->max_entry_id)
			opaque->max_entry_id = entry_id;
		opaque->live_items++;
	}
}

/*
 * Recompute min, max, and live count from the items still on the page
 */
void
IvfflatPageOpaqueRecompute(Page page)
{
	IvfflatPageOpaqueDataV2 *opaque = IvfflatPageGetOpaqueV2(page);
	OffsetNumber maxoffno = PageGetMaxOffsetNumber(page);
	uint32		min = 0;
	uint32		max = 0;
	uint16		live = 0;

	for (OffsetNumber offno = FirstOffsetNumber; offno <= maxoffno; offno = OffsetNumberNext(offno))
	{
		Item		item = PageGetItem(page, PageGetItemId(page, offno));
		uint32		entry_id = IvfflatEntryGetId(item);

		if (live == 0 || entry_id < min)
			min = entry_id;
		if (live == 0 || entry_id > max)
			max = entry_id;
		live++;
	}

	opaque->min_entry_id = min;
	opaque->max_entry_id = max;
	opaque->live_items = live;
}

/*
 * Replace the version-2 list header after a build of one list
 */
void
IvfflatWriteListV2(Relation index, ListInfo listInfo, int includeCount,
				   BlockNumber startPage, BlockNumber insertPage,
				   BlockNumber dirStartPage, uint32 entry_hwm, uint32 live_count,
				   IvfflatIncludeChain *chains, ForkNumber forkNum)
{
	Buffer		buf;
	Page		page;
	GenericXLogState *state;
	IvfflatListDataV2 *list;

	buf = ReadBufferExtended(index, forkNum, listInfo.blkno, RBM_NORMAL, NULL);
	LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
	state = GenericXLogStart(index);
	page = GenericXLogRegisterBuffer(state, buf, 0);
	list = (IvfflatListDataV2 *) PageGetItem(page, PageGetItemId(page, listInfo.offno));

	list->startPage = startPage;
	list->insertPage = insertPage;
	list->dirStartPage = dirStartPage;
	list->entry_hwm = entry_hwm;
	list->live_count = live_count;

	if (includeCount > 0)
		memcpy(IvfflatListGetChains(list), chains, includeCount * sizeof(IvfflatIncludeChain));

	IvfflatCommitBuffer(buf, state);
}

/*
 * Detoast an included value and reject anything past the width cap
 */
Datum
IvfflatPrepareIncludeDatum(Form_pg_attribute att, Datum value, bool isnull)
{
	if (isnull)
		return (Datum) 0;

	if (att->attlen < 0)
	{
		Datum		detoasted = PointerGetDatum(PG_DETOAST_DATUM(value));

		if (VARSIZE_ANY(DatumGetPointer(detoasted)) > IVFFLAT_MAX_INCLUDE_WIDTH)
			ereport(ERROR,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("included column value is too large"),
					 errdetail("Included values must be at most %d bytes.", IVFFLAT_MAX_INCLUDE_WIDTH)));

		return detoasted;
	}

	return value;
}

/*
 * One-attribute tuple descriptor copied from an index column
 */
TupleDesc
IvfflatOneColumnDesc(Relation index, AttrNumber attnum)
{
	TupleDesc	src = RelationGetDescr(index);
	Form_pg_attribute att = TupleDescAttr(src, attnum - 1);
	TupleDesc	desc = CreateTemplateTupleDesc(1);

	TupleDescInitEntry(desc, 1, NameStr(att->attname), att->atttypid, att->atttypmod, att->attndims);
	TupleDescInitEntryCollation(desc, 1, att->attcollation);
#if PG_VERSION_NUM >= 190000
	TupleDescFinalize(desc);
#endif

	return desc;
}

/*
 * One-attribute index tuple carrying the heap TID
 */
IndexTuple
IvfflatFormColumnTuple(TupleDesc desc, Datum value, bool isnull, ItemPointer tid)
{
	IndexTuple	itup = index_form_tuple(desc, &value, &isnull);

	itup->t_tid = *tid;

	return itup;
}

/*
 * Version-2 item: entry_id followed immediately by the index tuple
 */
char *
IvfflatFormEntryItem(uint32 entry_id, IndexTuple itup, Size *itemsz)
{
	Size		tlen = IndexTupleSize(itup);
	Size		size = MAXALIGN(sizeof(uint32) + tlen);
	char	   *item = (char *) palloc0(size);

	memcpy(item, &entry_id, sizeof(uint32));
	memcpy(item + sizeof(uint32), itup, tlen);
	*itemsz = size;

	return item;
}

/*
 * Get the metapage info
 */
void
IvfflatGetMetaPageInfo(Relation index, int *lists, int *dimensions)
{
	Buffer		buf;
	Page		page;
	IvfflatMetaPage metap;

	buf = ReadBuffer(index, IVFFLAT_METAPAGE_BLKNO);
	LockBuffer(buf, BUFFER_LOCK_SHARE);
	page = BufferGetPage(buf);
	metap = IvfflatPageGetMeta(page);

	if (unlikely(metap->magicNumber != IVFFLAT_MAGIC_NUMBER))
		elog(ERROR, "ivfflat index is not valid");

	if (lists != NULL)
		*lists = metap->lists;

	if (dimensions != NULL)
		*dimensions = metap->dimensions;

	UnlockReleaseBuffer(buf);
}

/*
 * Update the start or insert page of a list
 */
void
IvfflatUpdateList(Relation index, ListInfo listInfo,
				  BlockNumber insertPage, BlockNumber originalInsertPage,
				  BlockNumber startPage, ForkNumber forkNum)
{
	Buffer		buf;
	Page		page;
	GenericXLogState *state;
	IvfflatList list;
	bool		changed = false;

	buf = ReadBufferExtended(index, forkNum, listInfo.blkno, RBM_NORMAL, NULL);
	LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
	state = GenericXLogStart(index);
	page = GenericXLogRegisterBuffer(state, buf, 0);
	list = (IvfflatList) PageGetItem(page, PageGetItemId(page, listInfo.offno));

	if (BlockNumberIsValid(insertPage) && insertPage != list->insertPage)
	{
		/* Skip update if insert page is lower than original insert page  */
		/* This is needed to prevent insert from overwriting vacuum */
		if (!BlockNumberIsValid(originalInsertPage) || insertPage >= originalInsertPage)
		{
			list->insertPage = insertPage;
			changed = true;
		}
	}

	if (BlockNumberIsValid(startPage) && startPage != list->startPage)
	{
		list->startPage = startPage;
		changed = true;
	}

	/* Only commit if changed */
	if (changed)
		IvfflatCommitBuffer(buf, state);
	else
	{
		GenericXLogAbort(state);
		UnlockReleaseBuffer(buf);
	}
}

static void
IvfflatCheckExpectedDim(int expected, int32 dim)
{
	if (dim != expected)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_EXCEPTION),
				 errmsg("expected %d dimensions, not %d", expected, dim)));
}

/*
 * Check dimensions match index
 */
void
IvfflatCheckDim(int expected, const IvfflatTypeInfo * typeInfo, Oid collation, Datum value)
{
	int32		dim = DatumGetInt32(DirectFunctionCall1Coll(typeInfo->dimensions, collation, value));

	IvfflatCheckExpectedDim(expected, dim);
}

PGDLLEXPORT Datum vector_dims(PG_FUNCTION_ARGS);
PGDLLEXPORT Datum halfvec_vector_dims(PG_FUNCTION_ARGS);

PGDLLEXPORT Datum l2_normalize(PG_FUNCTION_ARGS);
PGDLLEXPORT Datum halfvec_l2_normalize(PG_FUNCTION_ARGS);

static Size
VectorItemSize(int dimensions)
{
	return VECTOR_SIZE(dimensions);
}

static Size
HalfvecItemSize(int dimensions)
{
	return HALFVEC_SIZE(dimensions);
}

static Size
BitItemSize(int dimensions)
{
	return VARBITTOTALLEN(dimensions);
}

static void
VectorUpdateCenter(Pointer v, int dimensions, float *x)
{
	Vector	   *vec = (Vector *) v;

	SET_VARSIZE(vec, VECTOR_SIZE(dimensions));
	vec->dim = dimensions;

	for (int i = 0; i < dimensions; i++)
		vec->x[i] = x[i];
}

static void
HalfvecUpdateCenter(Pointer v, int dimensions, float *x)
{
	HalfVector *vec = (HalfVector *) v;

	SET_VARSIZE(vec, HALFVEC_SIZE(dimensions));
	vec->dim = dimensions;

	for (int i = 0; i < dimensions; i++)
		vec->x[i] = Float4ToHalfUnchecked(x[i]);
}

static void
BitUpdateCenter(Pointer v, int dimensions, float *x)
{
	VarBit	   *vec = (VarBit *) v;
	unsigned char *nx = VARBITS(vec);

	SET_VARSIZE(vec, VARBITTOTALLEN(dimensions));
	VARBITLEN(vec) = dimensions;

	for (uint32 i = 0; i < VARBITBYTES(vec); i++)
		nx[i] = 0;

	for (int i = 0; i < dimensions; i++)
		nx[i / 8] |= (x[i] > 0.5 ? 1 : 0) << (7 - (i % 8));
}

static void
VectorSumCenter(Pointer v, int dimensions, float *x)
{
	Vector	   *vec = (Vector *) v;

	/* Safety check */
	IvfflatCheckExpectedDim(dimensions, vec->dim);

	/* Auto-vectorized */
	for (int i = 0; i < dimensions; i++)
		x[i] += vec->x[i];
}

static void
HalfvecSumCenter(Pointer v, int dimensions, float *x)
{
	HalfVector *vec = (HalfVector *) v;

	/* Safety check */
	IvfflatCheckExpectedDim(dimensions, vec->dim);

	/* Auto-vectorized on aarch64 */
	for (int i = 0; i < dimensions; i++)
		x[i] += HalfToFloat4(vec->x[i]);
}

static void
BitSumCenter(Pointer v, int dimensions, float *x)
{
	VarBit	   *vec = (VarBit *) v;

	/* Safety check */
	IvfflatCheckExpectedDim(dimensions, VARBITLEN(vec));

	for (int i = 0; i < dimensions; i++)
		x[i] += (float) (((VARBITS(vec)[i / 8]) >> (7 - (i % 8))) & 0x01);
}

/*
 * Get type info
 */
const		IvfflatTypeInfo *
IvfflatGetTypeInfo(Relation index)
{
	FmgrInfo   *procinfo = IvfflatOptionalProcInfo(index, IVFFLAT_TYPE_INFO_PROC);

	if (procinfo == NULL)
	{
		static const IvfflatTypeInfo typeInfo = {
			.maxDimensions = IVFFLAT_MAX_DIM,
			.dimensions = vector_dims,
			.normalize = l2_normalize,
			.itemSize = VectorItemSize,
			.updateCenter = VectorUpdateCenter,
			.sumCenter = VectorSumCenter
		};

		return (&typeInfo);
	}
	else
		return (const IvfflatTypeInfo *) DatumGetPointer(FunctionCall0Coll(procinfo, InvalidOid));
}

FUNCTION_PREFIX PG_FUNCTION_INFO_V1(ivfflat_halfvec_support);
Datum
ivfflat_halfvec_support(PG_FUNCTION_ARGS)
{
	static const IvfflatTypeInfo typeInfo = {
		.maxDimensions = IVFFLAT_MAX_DIM * 2,
		.dimensions = halfvec_vector_dims,
		.normalize = halfvec_l2_normalize,
		.itemSize = HalfvecItemSize,
		.updateCenter = HalfvecUpdateCenter,
		.sumCenter = HalfvecSumCenter
	};

	PG_RETURN_POINTER(&typeInfo);
}

FUNCTION_PREFIX PG_FUNCTION_INFO_V1(ivfflat_bit_support);
Datum
ivfflat_bit_support(PG_FUNCTION_ARGS)
{
	static const IvfflatTypeInfo typeInfo = {
		.maxDimensions = IVFFLAT_MAX_DIM * 32,
		.dimensions = bitlength,
		.normalize = NULL,
		.itemSize = BitItemSize,
		.updateCenter = BitUpdateCenter,
		.sumCenter = BitSumCenter
	};

	PG_RETURN_POINTER(&typeInfo);
}
