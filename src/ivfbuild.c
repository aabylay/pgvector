#include "postgres.h"

#include <float.h>

#include "access/genam.h"
#include "access/generic_xlog.h"
#include "access/itup.h"
#include "access/relscan.h"
#include "access/table.h"
#include "access/tableam.h"
#include "access/tupdesc.h"
#include "access/parallel.h"
#include "access/xact.h"
#include "access/xloginsert.h"
#include "catalog/index.h"
#include "catalog/pg_operator_d.h"
#include "catalog/pg_type_d.h"
#include "commands/progress.h"
#include "fmgr.h"
#include "ivfflat.h"
#include "miscadmin.h"
#include "nodes/execnodes.h"
#include "optimizer/optimizer.h"
#include "storage/bufmgr.h"
#include "storage/condition_variable.h"
#include "tcop/tcopprot.h"
#include "utils/backend_progress.h"
#include "utils/backend_status.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/sampling.h"
#include "utils/snapmgr.h"
#include "utils/tuplesort.h"
#include "utils/wait_event.h"

#if PG_VERSION_NUM >= 160000
#include "varatt.h"
#endif

#define PARALLEL_KEY_IVFFLAT_SHARED		UINT64CONST(0xA000000000000001)
#define PARALLEL_KEY_TUPLESORT			UINT64CONST(0xA000000000000002)
#define PARALLEL_KEY_IVFFLAT_CENTERS	UINT64CONST(0xA000000000000003)
#define PARALLEL_KEY_QUERY_TEXT			UINT64CONST(0xA000000000000004)

/*
 * Add sample
 */
static void
AddSample(Datum *values, IvfflatBuildState * buildstate)
{
	VectorArray samples = buildstate->samples;
	int			targsamples = samples->maxlen;

	/* Detoast once for all calls */
	Datum		value = PointerGetDatum(PG_DETOAST_DATUM(values[0]));

	/* Check dimensions match index */
	IvfflatCheckDim(buildstate->dimensions, buildstate->typeInfo, buildstate->collation, value);

	/*
	 * Check with KMEANS_NORM_PROC that the value can be normalized since
	 * spherical distance function expects unit vectors
	 */
	if (buildstate->kmeansnormprocinfo != NULL)
	{
		if (!IvfflatCheckNorm(buildstate->kmeansnormprocinfo, buildstate->collation, value))
			return;
	}

	if (samples->length < targsamples)
	{
		VectorArraySet(samples, samples->length, DatumGetPointer(value));
		samples->length++;
	}
	else
	{
		if (buildstate->rowstoskip < 0)
			buildstate->rowstoskip = reservoir_get_next_S(&buildstate->rstate, buildstate->samplerows, targsamples);

		if (buildstate->rowstoskip <= 0)
		{
			int			k = (int) (targsamples * sampler_random_fract(&buildstate->rstate.randstate));

			Assert(k >= 0 && k < targsamples);
			VectorArraySet(samples, k, DatumGetPointer(value));
		}

		buildstate->rowstoskip -= 1;
	}

	/* Increment after reservoir_get_next_S */
	buildstate->samplerows += 1;
}

/*
 * Callback for sampling
 */
static void
SampleCallback(Relation index, ItemPointer tid, Datum *values,
			   bool *isnull, bool tupleIsAlive, void *state)
{
	IvfflatBuildState *buildstate = (IvfflatBuildState *) state;
	MemoryContext oldCtx;

	/* Skip nulls */
	if (isnull[0])
		return;

	/* Use memory context since detoast can allocate */
	oldCtx = MemoryContextSwitchTo(buildstate->tmpCtx);

	/* Add sample */
	AddSample(values, buildstate);

	/* Reset memory context */
	MemoryContextSwitchTo(oldCtx);
	MemoryContextReset(buildstate->tmpCtx);
}

/*
 * Sample rows with same logic as ANALYZE
 */
static void
SampleRows(IvfflatBuildState * buildstate)
{
	int			targsamples = buildstate->samples->maxlen;
	BlockNumber totalblocks = RelationGetNumberOfBlocks(buildstate->heap);

	buildstate->samplerows = 0;
	buildstate->rowstoskip = -1;

	BlockSampler_Init(&buildstate->bs, totalblocks, targsamples, RandomInt());

	reservoir_init_selection_state(&buildstate->rstate, targsamples);
	while (BlockSampler_HasMore(&buildstate->bs))
	{
		BlockNumber targblock = BlockSampler_Next(&buildstate->bs);

		/* Set anyvisible to false like table_index_build_scan */
		table_index_build_range_scan(buildstate->heap, buildstate->index, buildstate->indexInfo,
									 false, false, false, targblock, 1, SampleCallback, (void *) buildstate, NULL);
	}

	/* Normalize if needed */
	if (buildstate->kmeansnormprocinfo != NULL)
		IvfflatNormVectors(buildstate->typeInfo, buildstate->collation, buildstate->samples, buildstate->tmpCtx);
}

/*
 * Add tuple to sort
 */
static void
AddTupleToSort(ItemPointer tid, Datum *values, bool *isnull, IvfflatBuildState * buildstate)
{
	double		distance;
	double		minDistance = DBL_MAX;
	int			closestCenter = 0;
	VectorArray centers = buildstate->centers;
	TupleTableSlot *slot = buildstate->slot;

	/* Detoast once for all calls */
	Datum		value = PointerGetDatum(PG_DETOAST_DATUM(values[0]));

	/* Normalize if needed */
	if (buildstate->normprocinfo != NULL)
	{
		if (!IvfflatCheckNorm(buildstate->normprocinfo, buildstate->collation, value))
			return;

		value = IvfflatNormValue(buildstate->typeInfo, buildstate->collation, value);
	}

	/* Check dimensions match index */
	IvfflatCheckDim(buildstate->dimensions, buildstate->typeInfo, buildstate->collation, value);

	/* Find the list that minimizes the distance */
	for (int i = 0; i < centers->length; i++)
	{
		distance = DatumGetFloat8(FunctionCall2Coll(buildstate->procinfo, buildstate->collation, value, PointerGetDatum(VectorArrayGet(centers, i))));

		if (distance < minDistance)
		{
			minDistance = distance;
			closestCenter = i;
		}
	}

#ifdef IVFFLAT_KMEANS_DEBUG
	buildstate->inertia += minDistance;
	buildstate->listSums[closestCenter] += minDistance;
	buildstate->listCounts[closestCenter]++;
#endif

	/* Create a virtual tuple */
	ExecClearTuple(slot);
	slot->tts_values[0] = Int32GetDatum(closestCenter);
	slot->tts_isnull[0] = false;
	slot->tts_values[1] = PointerGetDatum(tid);
	slot->tts_isnull[1] = false;
	slot->tts_values[2] = value;
	slot->tts_isnull[2] = false;

	for (int i = 0; i < buildstate->includeCount; i++)
	{
		Form_pg_attribute att = TupleDescAttr(buildstate->tupdesc, i + 1);

		slot->tts_isnull[3 + i] = isnull[i + 1];
		slot->tts_values[3 + i] = IvfflatPrepareIncludeDatum(att, values[i + 1], isnull[i + 1]);
	}

	ExecStoreVirtualTuple(slot);

	/*
	 * Add tuple to sort
	 *
	 * tuplesort_puttupleslot comment: Input data is always copied; the caller
	 * need not save it.
	 */
	tuplesort_puttupleslot(buildstate->sortstate, slot);

	buildstate->indtuples++;
}

/*
 * Callback for table_index_build_scan
 */
static void
BuildCallback(Relation index, ItemPointer tid, Datum *values,
			  bool *isnull, bool tupleIsAlive, void *state)
{
	IvfflatBuildState *buildstate = (IvfflatBuildState *) state;
	MemoryContext oldCtx;

	/* Skip nulls */
	if (isnull[0])
		return;

	/* Use memory context since detoast can allocate */
	oldCtx = MemoryContextSwitchTo(buildstate->tmpCtx);

	/* Add tuple to sort */
	AddTupleToSort(tid, values, isnull, buildstate);

	/* Reset memory context */
	MemoryContextSwitchTo(oldCtx);
	MemoryContextReset(buildstate->tmpCtx);
}

/*
 * Get index tuple from sort state
 */
static inline void
GetNextTuple(Tuplesortstate *sortstate, TupleDesc tupdesc, TupleTableSlot *slot, IndexTuple *itup, int *list)
{
	if (tuplesort_gettupleslot(sortstate, true, false, slot, NULL))
	{
		Datum		value;
		bool		isnull;

		*list = DatumGetInt32(slot_getattr(slot, 1, &isnull));
		value = slot_getattr(slot, 3, &isnull);

		/* Form the index tuple */
		*itup = index_form_tuple(tupdesc, &value, &isnull);
		(*itup)->t_tid = *((ItemPointer) DatumGetPointer(slot_getattr(slot, 2, &isnull)));
	}
	else
		*list = -1;
}

/*
 * One open page chain during a version-2 build
 */
typedef struct BuildChain
{
	Buffer		buf;
	Page		page;
	GenericXLogState *state;
	BlockNumber startPage;
	BlockNumber insertPage;
	bool		active;
}			BuildChain;

static void
BuildChainReset(BuildChain *chain)
{
	chain->buf = InvalidBuffer;
	chain->page = NULL;
	chain->state = NULL;
	chain->startPage = InvalidBlockNumber;
	chain->insertPage = InvalidBlockNumber;
	chain->active = false;
}

static void
BuildChainFinish(BuildChain *chain)
{
	if (!chain->active)
		return;

	IvfflatCommitBuffer(chain->buf, chain->state);
	chain->active = false;
	chain->buf = InvalidBuffer;
	chain->page = NULL;
	chain->state = NULL;
}

static void
BuildChainEnsure(Relation index, BuildChain *chain, ForkNumber forkNum, Size itemsz)
{
	if (!chain->active)
	{
		chain->buf = IvfflatNewBuffer(index, forkNum);
		IvfflatInitRegisterPage(index, &chain->buf, &chain->page, &chain->state, IVFFLAT_VERSION_INCLUDE);
		chain->startPage = chain->insertPage = BufferGetBlockNumber(chain->buf);
		chain->active = true;
		return;
	}

	if (PageGetFreeSpace(chain->page) < itemsz)
	{
		IvfflatAppendPage(index, &chain->buf, &chain->page, &chain->state, forkNum, IVFFLAT_VERSION_INCLUDE);
		chain->insertPage = BufferGetBlockNumber(chain->buf);
	}
}

static void
BuildChainAddEntry(Relation index, BuildChain *chain, ForkNumber forkNum, char *item, Size itemsz, uint32 entry_id)
{
	BuildChainEnsure(index, chain, forkNum, itemsz);

	if (PageAddItem(chain->page, (Item) item, itemsz, InvalidOffsetNumber, false, false) == InvalidOffsetNumber)
		elog(ERROR, "failed to add index item to \"%s\"", RelationGetRelationName(index));

	IvfflatPageOpaqueAdd(chain->page, entry_id);
}

static OffsetNumber
BuildDirAppend(Relation index, BuildChain *dir, ForkNumber forkNum, BlockNumber blkno, uint32 entry_id)
{
	IvfflatVectorDirEntry ent;
	OffsetNumber offno;

	ent.blkno = blkno;
	ent.min_entry_id = entry_id;
	ent.max_entry_id = entry_id;
	ent.live_items = 1;
	ent.unused = 0;

	BuildChainEnsure(index, dir, forkNum, sizeof(ent));
	offno = PageAddItem(dir->page, (Item) &ent, sizeof(ent), InvalidOffsetNumber, false, false);
	if (offno == InvalidOffsetNumber)
		elog(ERROR, "failed to add index item to \"%s\"", RelationGetRelationName(index));

	return offno;
}

static void
BuildDirBump(Page page, OffsetNumber offno, uint32 entry_id)
{
	IvfflatVectorDirEntry *ent;

	ent = (IvfflatVectorDirEntry *) PageGetItem(page, PageGetItemId(page, offno));

	if (entry_id < ent->min_entry_id)
		ent->min_entry_id = entry_id;
	if (entry_id > ent->max_entry_id)
		ent->max_entry_id = entry_id;
	ent->live_items++;
}

/*
 * Version-2 entry pages: one item per included column, then the vector item
 */
static void
InsertTuplesV2(Relation index, IvfflatBuildState * buildstate, ForkNumber forkNum)
{
	int			list;
	IndexTuple	itup = NULL;
	int64		inserted = 0;
	int			includeCount = buildstate->includeCount;
	TupleTableSlot *slot = MakeSingleTupleTableSlot(buildstate->sortdesc, &TTSOpsMinimalTuple);
	BuildChain *columns;
	BuildChain	vector;
	BuildChain	dir;

	columns = palloc_array_checked(BuildChain, includeCount);

	pgstat_progress_update_param(PROGRESS_CREATEIDX_SUBPHASE, PROGRESS_IVFFLAT_PHASE_LOAD);
	pgstat_progress_update_param(PROGRESS_CREATEIDX_TUPLES_TOTAL, buildstate->indtuples);

	GetNextTuple(buildstate->sortstate, buildstate->vectorDesc, slot, &itup, &list);

	for (int i = 0; i < buildstate->centers->length; i++)
	{
		uint32		entry_id = 0;
		BlockNumber vecBlk = InvalidBlockNumber;
		OffsetNumber dirOff = InvalidOffsetNumber;

		CHECK_FOR_INTERRUPTS();

		for (int c = 0; c < includeCount; c++)
			BuildChainReset(&columns[c]);
		BuildChainReset(&vector);
		BuildChainReset(&dir);

		while (list == i)
		{
			ItemPointer tid;
			bool		tidnull;
			BlockNumber now;

			tid = (ItemPointer) DatumGetPointer(slot_getattr(slot, 2, &tidnull));

			for (int c = 0; c < includeCount; c++)
			{
				bool		isnull;
				Datum		value = slot_getattr(slot, (AttrNumber) (4 + c), &isnull);
				IndexTuple	coltup = IvfflatFormColumnTuple(buildstate->includeDescs[c], value, isnull, tid);
				Size		itemsz;
				char	   *item = IvfflatFormEntryItem(entry_id, coltup, &itemsz);

				BuildChainAddEntry(index, &columns[c], forkNum, item, itemsz, entry_id);
				pfree(item);
				pfree(coltup);
			}

			{
				Size		itemsz;
				char	   *item = IvfflatFormEntryItem(entry_id, itup, &itemsz);

				BuildChainAddEntry(index, &vector, forkNum, item, itemsz, entry_id);
				pfree(item);
			}

			now = BufferGetBlockNumber(vector.buf);
			if (now != vecBlk)
			{
				dirOff = BuildDirAppend(index, &dir, forkNum, now, entry_id);
				vecBlk = now;
			}
			else
				BuildDirBump(dir.page, dirOff, entry_id);

			pfree(itup);

			pgstat_progress_update_param(PROGRESS_CREATEIDX_TUPLES_DONE, ++inserted);
			entry_id++;

			GetNextTuple(buildstate->sortstate, buildstate->vectorDesc, slot, &itup, &list);
		}

		for (int c = 0; c < includeCount; c++)
			BuildChainFinish(&columns[c]);
		BuildChainFinish(&vector);
		BuildChainFinish(&dir);

		if (entry_id > 0)
		{
			IvfflatIncludeChain *chains = palloc_array_checked(IvfflatIncludeChain, includeCount);

			for (int c = 0; c < includeCount; c++)
			{
				chains[c].startPage = columns[c].startPage;
				chains[c].insertPage = columns[c].insertPage;
			}

			IvfflatWriteListV2(index, buildstate->listInfo[i], includeCount,
							   vector.startPage, vector.insertPage, dir.startPage,
							   entry_id, entry_id, chains, forkNum);
			pfree(chains);
		}
	}

	pfree(columns);
	ExecDropSingleTupleTableSlot(slot);
}

/*
 * Create initial entry pages
 */
static void
InsertTuples(Relation index, IvfflatBuildState * buildstate, ForkNumber forkNum)
{
	int			list;
	IndexTuple	itup = NULL;	/* silence compiler warning */
	int64		inserted = 0;
	TupleTableSlot *slot;
	TupleDesc	tupdesc;

	if (buildstate->version == IVFFLAT_VERSION_INCLUDE)
	{
		InsertTuplesV2(index, buildstate, forkNum);
		return;
	}

	slot = MakeSingleTupleTableSlot(buildstate->sortdesc, &TTSOpsMinimalTuple);
	tupdesc = buildstate->tupdesc;

	pgstat_progress_update_param(PROGRESS_CREATEIDX_SUBPHASE, PROGRESS_IVFFLAT_PHASE_LOAD);

	pgstat_progress_update_param(PROGRESS_CREATEIDX_TUPLES_TOTAL, buildstate->indtuples);

	GetNextTuple(buildstate->sortstate, tupdesc, slot, &itup, &list);

	for (int i = 0; i < buildstate->centers->length; i++)
	{
		Buffer		buf;
		Page		page;
		GenericXLogState *state;
		BlockNumber startPage;
		BlockNumber insertPage;

		/* Can take a while, so ensure we can interrupt */
		/* Needs to be called when no buffer locks are held */
		CHECK_FOR_INTERRUPTS();

	buf = IvfflatNewBuffer(index, forkNum);
	IvfflatInitRegisterPage(index, &buf, &page, &state, buildstate->version);

	startPage = BufferGetBlockNumber(buf);

		/* Get all tuples for list */
		while (list == i)
		{
			/* Check for free space */
			Size		itemsz = MAXALIGN(IndexTupleSize(itup));

			if (PageGetFreeSpace(page) < itemsz)
				IvfflatAppendPage(index, &buf, &page, &state, forkNum, buildstate->version);

			/* Add the item */
			if (PageAddItem(page, (Item) itup, itemsz, InvalidOffsetNumber, false, false) == InvalidOffsetNumber)
				elog(ERROR, "failed to add index item to \"%s\"", RelationGetRelationName(index));

			pfree(itup);

			pgstat_progress_update_param(PROGRESS_CREATEIDX_TUPLES_DONE, ++inserted);

			GetNextTuple(buildstate->sortstate, tupdesc, slot, &itup, &list);
		}

		insertPage = BufferGetBlockNumber(buf);

		IvfflatCommitBuffer(buf, state);

		/* Set the start and insert pages */
		IvfflatUpdateList(index, buildstate->listInfo[i], insertPage, InvalidBlockNumber, startPage, forkNum);
	}
}

/*
 * Acceptance bound for one included column, or an error if the type cannot
 * live in a column chain
 */
static uint16
IncludeColumnBound(Form_pg_attribute att)
{
	if (att->attlen > 0)
	{
		if (att->attlen > IVFFLAT_MAX_INCLUDE_WIDTH)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("included column type is not supported for ivfflat index"),
					 errdetail("Fixed-length included columns must be at most %d bytes.",
							   IVFFLAT_MAX_INCLUDE_WIDTH)));

		return (uint16) att->attlen;
	}

	if ((att->atttypid == VARCHAROID || att->atttypid == BPCHAROID) &&
		att->atttypmod >= (int32) VARHDRSZ)
	{
		int			maxlen = att->atttypmod - VARHDRSZ;

		if (maxlen > IVFFLAT_MAX_INCLUDE_WIDTH)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("included column type is not supported for ivfflat index"),
					 errdetail("varchar and char included columns must be at most %d characters.",
							   IVFFLAT_MAX_INCLUDE_WIDTH)));

		return IVFFLAT_MAX_INCLUDE_WIDTH;
	}

	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("included column type is not supported for ivfflat index"),
			 errdetail("Included columns must be fixed-length, varchar(n), or char(n), with n at most %d.",
					   IVFFLAT_MAX_INCLUDE_WIDTH)));

	return 0;
}

/*
 * Version-2 indexes are exactly the ones with included columns
 */
static void
CheckIncludedColumns(IvfflatBuildState * buildstate)
{
	IndexInfo  *indexInfo = buildstate->indexInfo;
	TupleDesc	tupdesc = buildstate->tupdesc;
	int			nkeys;
	int			natts;
	Size		listBytes;

	buildstate->includeCount = 0;
	buildstate->version = IVFFLAT_VERSION;
	buildstate->vectorDesc = NULL;
	buildstate->includeDescs = NULL;
	memset(buildstate->includeWidth, 0, sizeof(buildstate->includeWidth));

	if (indexInfo == NULL)
		return;

	nkeys = indexInfo->ii_NumIndexKeyAttrs;
	natts = indexInfo->ii_NumIndexAttrs;

	if (nkeys != 1)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("ivfflat indexes support only one key column")));

	if (natts <= nkeys)
		return;

	buildstate->includeCount = natts - nkeys;

	if (buildstate->includeCount > IVFFLAT_MAX_INCLUDE_COLS)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("included columns do not fit on an ivfflat list page"),
				 errdetail("An ivfflat index supports at most %d included columns.",
						   IVFFLAT_MAX_INCLUDE_COLS)));

	for (int i = 0; i < buildstate->includeCount; i++)
		buildstate->includeWidth[i] = IncludeColumnBound(TupleDescAttr(tupdesc, i + 1));

	listBytes = IvfflatListHeaderBytes(buildstate->includeCount) + buildstate->typeInfo->itemSize(buildstate->dimensions);
	if (MAXALIGN(listBytes) > IvfflatMaxItemSize(IVFFLAT_VERSION_INCLUDE))
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("included columns do not fit on an ivfflat list page"),
				 errdetail("The list item for %d dimensions and %d included columns is larger than a page.",
						   buildstate->dimensions, buildstate->includeCount)));

	buildstate->version = IVFFLAT_VERSION_INCLUDE;
}

/*
 * Initialize the build state
 */
static void
InitBuildState(IvfflatBuildState * buildstate, Relation heap, Relation index, IndexInfo *indexInfo)
{
	buildstate->heap = heap;
	buildstate->index = index;
	buildstate->indexInfo = indexInfo;
	buildstate->typeInfo = IvfflatGetTypeInfo(index);
	buildstate->tupdesc = RelationGetDescr(index);

	buildstate->lists = IvfflatGetLists(index);
	buildstate->dimensions = TupleDescAttr(index->rd_att, 0)->atttypmod;

	/* Disallow varbit since require fixed dimensions */
	if (TupleDescAttr(index->rd_att, 0)->atttypid == VARBITOID)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("type not supported for ivfflat index")));

	/* Require column to have dimensions to be indexed */
	if (buildstate->dimensions < 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("column does not have dimensions")));

	if (buildstate->dimensions > buildstate->typeInfo->maxDimensions)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("column cannot have more than %d dimensions for ivfflat index", buildstate->typeInfo->maxDimensions)));

	buildstate->reltuples = 0;
	buildstate->indtuples = 0;

	/* Get support functions */
	buildstate->procinfo = index_getprocinfo(index, 1, IVFFLAT_DISTANCE_PROC);
	buildstate->normprocinfo = IvfflatOptionalProcInfo(index, IVFFLAT_NORM_PROC);
	buildstate->kmeansnormprocinfo = IvfflatOptionalProcInfo(index, IVFFLAT_KMEANS_NORM_PROC);
	buildstate->collation = index->rd_indcollation[0];

	/* Require more than one dimension for spherical k-means */
	if (buildstate->kmeansnormprocinfo != NULL && buildstate->dimensions == 1)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("dimensions must be greater than one for this opclass")));

	CheckIncludedColumns(buildstate);

	/* Create tuple description for sorting */
	buildstate->sortdesc = CreateTemplateTupleDesc(3 + buildstate->includeCount);
	TupleDescInitEntry(buildstate->sortdesc, (AttrNumber) 1, "list", INT4OID, -1, 0);
	TupleDescInitEntry(buildstate->sortdesc, (AttrNumber) 2, "tid", TIDOID, -1, 0);
	TupleDescInitEntry(buildstate->sortdesc, (AttrNumber) 3, "vector", TupleDescAttr(buildstate->tupdesc, 0)->atttypid, -1, 0);

	if (buildstate->includeCount > 0)
	{
		buildstate->vectorDesc = IvfflatOneColumnDesc(index, 1);
		buildstate->includeDescs = palloc_array_checked(TupleDesc, buildstate->includeCount);

		for (int i = 0; i < buildstate->includeCount; i++)
		{
			Form_pg_attribute att = TupleDescAttr(buildstate->tupdesc, i + 1);

			TupleDescInitEntry(buildstate->sortdesc, (AttrNumber) (4 + i), NameStr(att->attname),
							   att->atttypid, att->atttypmod, att->attndims);
			TupleDescInitEntryCollation(buildstate->sortdesc, (AttrNumber) (4 + i), att->attcollation);
			buildstate->includeDescs[i] = IvfflatOneColumnDesc(index, (AttrNumber) (i + 2));
		}
	}

#if PG_VERSION_NUM >= 190000
	TupleDescFinalize(buildstate->sortdesc);
#endif

	buildstate->slot = MakeSingleTupleTableSlot(buildstate->sortdesc, &TTSOpsVirtual);

	buildstate->memoryUsed = 0;
	buildstate->itemsize = buildstate->typeInfo->itemSize(buildstate->dimensions);

	buildstate->memoryUsed = add_size(buildstate->memoryUsed, VECTOR_ARRAY_SIZE(buildstate->lists, buildstate->itemsize));
	IvfflatCheckMemoryUsage(buildstate->memoryUsed);
	buildstate->centers = VectorArrayInit(buildstate->lists, buildstate->dimensions, buildstate->itemsize);

	/* TODO Move allocation to page creation */
	buildstate->listInfo = palloc_array_checked(ListInfo, buildstate->lists);

	buildstate->tmpCtx = AllocSetContextCreate(CurrentMemoryContext,
											   "Ivfflat build temporary context",
											   ALLOCSET_DEFAULT_SIZES);

#ifdef IVFFLAT_KMEANS_DEBUG
	buildstate->inertia = 0;
	buildstate->listSums = palloc0_array_checked(double, buildstate->lists);
	buildstate->listCounts = palloc0_array_checked(int, buildstate->lists);
#endif

	buildstate->ivfleader = NULL;
}

/*
 * Free resources
 */
static void
FreeBuildState(IvfflatBuildState * buildstate)
{
	VectorArrayFree(buildstate->centers);
	pfree(buildstate->listInfo);

	if (buildstate->vectorDesc != NULL)
		FreeTupleDesc(buildstate->vectorDesc);

	if (buildstate->includeDescs != NULL)
	{
		for (int i = 0; i < buildstate->includeCount; i++)
			FreeTupleDesc(buildstate->includeDescs[i]);
		pfree(buildstate->includeDescs);
	}

#ifdef IVFFLAT_KMEANS_DEBUG
	pfree(buildstate->listSums);
	pfree(buildstate->listCounts);
#endif

	MemoryContextDelete(buildstate->tmpCtx);
}

/*
 * Compute centers
 */
static void
ComputeCenters(IvfflatBuildState * buildstate)
{
	int			numSamples;

	pgstat_progress_update_param(PROGRESS_CREATEIDX_SUBPHASE, PROGRESS_IVFFLAT_PHASE_KMEANS);

	/* Skip samples for unlogged table */
	if (buildstate->heap == NULL)
		numSamples = 1;
	else
	{
		int64		maxTuples = (int64) RelationGetNumberOfBlocks(buildstate->heap) * MaxHeapTuplesPerPage;

		/* Target 50 samples per list, with at least 10000 samples */
		/* The number of samples has a large effect on index build time */
		numSamples = buildstate->lists * 50;
		if (numSamples < 10000)
			numSamples = 10000;

		/* Save memory since will not have more than max tuples */
		numSamples = Max(Min(numSamples, maxTuples), 1);
	}

	/* Sample rows */
	buildstate->memoryUsed = add_size(buildstate->memoryUsed, VECTOR_ARRAY_SIZE(numSamples, buildstate->itemsize));
	IvfflatCheckMemoryUsage(buildstate->memoryUsed);
	buildstate->samples = VectorArrayInit(numSamples, buildstate->dimensions, buildstate->itemsize);
	if (buildstate->heap != NULL)
	{
		IvfflatBench("sample rows", SampleRows(buildstate));

		if (buildstate->samples->length < buildstate->lists)
		{
			ereport(NOTICE,
					(errmsg("ivfflat index created with little data"),
					 errdetail("This will cause low recall."),
					 errhint("Drop the index until the table has more data.")));
		}
	}

	/* Calculate centers */
	IvfflatBench("k-means", IvfflatKmeans(buildstate->index, buildstate->samples, buildstate->centers, buildstate->typeInfo, buildstate->memoryUsed));

	/* Free samples before we allocate more memory */
	VectorArrayFree(buildstate->samples);
}

/*
 * Create the metapage
 */
static void
CreateMetaPage(Relation index, IvfflatBuildState * buildstate, ForkNumber forkNum)
{
	Buffer		buf;
	Page		page;
	GenericXLogState *state;
	IvfflatMetaPage metap;

	buf = IvfflatNewBuffer(index, forkNum);
	IvfflatInitRegisterPage(index, &buf, &page, &state, buildstate->version);

	/* Set metapage data */
	metap = IvfflatPageGetMeta(page);
	metap->magicNumber = IVFFLAT_MAGIC_NUMBER;
	metap->dimensions = buildstate->dimensions;
	metap->lists = buildstate->lists;

	if (buildstate->version == IVFFLAT_VERSION_INCLUDE)
	{
		IvfflatMetaPageDataV2 *metap2 = (IvfflatMetaPageDataV2 *) PageGetContents(page);

		metap->version = IVFFLAT_VERSION_INCLUDE;
		metap2->includeCount = buildstate->includeCount;
		memset(metap2->includeWidth, 0, sizeof(metap2->includeWidth));
		memcpy(metap2->includeWidth, buildstate->includeWidth,
			   sizeof(uint16) * buildstate->includeCount);
		((PageHeader) page)->pd_lower =
			((char *) metap2 + sizeof(IvfflatMetaPageDataV2)) - (char *) page;
	}
	else
	{
		metap->version = IVFFLAT_VERSION;
		((PageHeader) page)->pd_lower =
			((char *) metap + sizeof(IvfflatMetaPageData)) - (char *) page;
	}

	IvfflatCommitBuffer(buf, state);
}

/*
 * Create list pages
 */
static void
CreateListPages(Relation index, IvfflatBuildState * buildstate, ForkNumber forkNum)
{
	Buffer		buf;
	Page		page;
	GenericXLogState *state;
	Size		listSize;
	VectorArray centers = buildstate->centers;
	int			lists = buildstate->lists;
	int			version = buildstate->version;
	int			includeCount = buildstate->includeCount;
	char	   *storage;

	if (version == IVFFLAT_VERSION_INCLUDE)
		listSize = MAXALIGN(IvfflatListHeaderBytes(includeCount) + centers->itemsize);
	else
		listSize = MAXALIGN(IVFFLAT_LIST_SIZE(centers->itemsize));

	storage = palloc0(listSize);

	buf = IvfflatNewBuffer(index, forkNum);
	IvfflatInitRegisterPage(index, &buf, &page, &state, version);

	for (int i = 0; i < lists; i++)
	{
		OffsetNumber offno;

		/* Zero memory for each list */
		memset(storage, 0, listSize);

		if (version == IVFFLAT_VERSION_INCLUDE)
		{
			IvfflatListDataV2 *list = (IvfflatListDataV2 *) storage;
			IvfflatIncludeChain *chains = IvfflatListGetChains(list);

			list->startPage = InvalidBlockNumber;
			list->insertPage = InvalidBlockNumber;
			list->dirStartPage = InvalidBlockNumber;
			list->entry_hwm = 0;
			list->live_count = 0;

			for (int c = 0; c < includeCount; c++)
			{
				chains[c].startPage = InvalidBlockNumber;
				chains[c].insertPage = InvalidBlockNumber;
			}

			memcpy(IvfflatListGetCenter(list, includeCount), VectorArrayGet(centers, i),
				   VARSIZE_ANY(VectorArrayGet(centers, i)));
		}
		else
		{
			IvfflatList list = (IvfflatList) storage;

			list->startPage = InvalidBlockNumber;
			list->insertPage = InvalidBlockNumber;
			memcpy(&list->center, VectorArrayGet(centers, i), VARSIZE_ANY(VectorArrayGet(centers, i)));
		}

		if (PageGetFreeSpace(page) < listSize)
			IvfflatAppendPage(index, &buf, &page, &state, forkNum, version);

		offno = PageAddItem(page, (Item) storage, listSize, InvalidOffsetNumber, false, false);
		if (offno == InvalidOffsetNumber)
			elog(ERROR, "failed to add index item to \"%s\"", RelationGetRelationName(index));

		buildstate->listInfo[i].blkno = BufferGetBlockNumber(buf);
		buildstate->listInfo[i].offno = offno;
	}

	IvfflatCommitBuffer(buf, state);

	pfree(storage);
}

#ifdef IVFFLAT_KMEANS_DEBUG
/*
 * Print k-means metrics
 */
static void
PrintKmeansMetrics(IvfflatBuildState * buildstate)
{
	elog(INFO, "inertia: %.3e", buildstate->inertia);

	/* Calculate Davies-Bouldin index */
	if (buildstate->lists > 1 && !buildstate->ivfleader)
	{
		double		db = 0.0;

		/* Calculate average distance */
		for (int i = 0; i < buildstate->lists; i++)
		{
			if (buildstate->listCounts[i] > 0)
				buildstate->listSums[i] /= buildstate->listCounts[i];
		}

		for (int i = 0; i < buildstate->lists; i++)
		{
			double		max = 0.0;
			double		distance;

			for (int j = 0; j < buildstate->lists; j++)
			{
				if (j == i)
					continue;

				distance = DatumGetFloat8(FunctionCall2Coll(buildstate->procinfo, buildstate->collation, PointerGetDatum(VectorArrayGet(buildstate->centers, i)), PointerGetDatum(VectorArrayGet(buildstate->centers, j))));
				distance = (buildstate->listSums[i] + buildstate->listSums[j]) / distance;

				if (distance > max)
					max = distance;
			}
			db += max;
		}
		db /= buildstate->lists;
		elog(INFO, "davies-bouldin: %.3f", db);
	}
}
#endif

/*
 * Initialize build sort state
 */
static Tuplesortstate *
InitBuildSortState(TupleDesc tupdesc, int memory, SortCoordinate coordinate)
{
	AttrNumber	attNums[] = {1};
	Oid			sortOperators[] = {Int4LessOperator};
	Oid			sortCollations[] = {InvalidOid};
	bool		nullsFirstFlags[] = {false};

	return tuplesort_begin_heap(tupdesc, 1, attNums, sortOperators, sortCollations, nullsFirstFlags, memory, coordinate, false);
}

/*
 * Within leader, wait for end of heap scan
 */
static double
ParallelHeapScan(IvfflatBuildState * buildstate)
{
	IvfflatShared *ivfshared = buildstate->ivfleader->ivfshared;
	int			nparticipanttuplesorts;
	double		reltuples;

	nparticipanttuplesorts = buildstate->ivfleader->nparticipanttuplesorts;
	for (;;)
	{
		SpinLockAcquire(&ivfshared->mutex);
		if (ivfshared->nparticipantsdone == nparticipanttuplesorts)
		{
			buildstate->indtuples = ivfshared->indtuples;
			reltuples = ivfshared->reltuples;
#ifdef IVFFLAT_KMEANS_DEBUG
			buildstate->inertia = ivfshared->inertia;
#endif
			SpinLockRelease(&ivfshared->mutex);
			break;
		}
		SpinLockRelease(&ivfshared->mutex);

		ConditionVariableSleep(&ivfshared->workersdonecv,
							   WAIT_EVENT_PARALLEL_CREATE_INDEX_SCAN);
	}

	ConditionVariableCancelSleep();

	return reltuples;
}

/*
 * Perform a worker's portion of a parallel sort
 */
static void
IvfflatParallelScanAndSort(IvfflatSpool * ivfspool, IvfflatShared * ivfshared, Sharedsort *sharedsort, char *ivfcenters, int sortmem, bool progress)
{
	SortCoordinate coordinate;
	IvfflatBuildState buildstate;
	TableScanDesc scan;
	double		reltuples;
	IndexInfo  *indexInfo;

	/* Initialize local tuplesort coordination state */
	coordinate = palloc0_object(SortCoordinateData);
	coordinate->isWorker = true;
	coordinate->nParticipants = -1;
	coordinate->sharedsort = sharedsort;

	/* Join parallel scan */
	indexInfo = BuildIndexInfo(ivfspool->index);
	indexInfo->ii_Concurrent = ivfshared->isconcurrent;
	InitBuildState(&buildstate, ivfspool->heap, ivfspool->index, indexInfo);
	memcpy(buildstate.centers->items, ivfcenters, mul_size(buildstate.centers->itemsize, buildstate.centers->maxlen));
	buildstate.centers->length = buildstate.centers->maxlen;
	ivfspool->sortstate = InitBuildSortState(buildstate.sortdesc, sortmem, coordinate);
	buildstate.sortstate = ivfspool->sortstate;
	scan = table_beginscan_parallel(ivfspool->heap,
									ParallelTableScanFromIvfflatShared(ivfshared)
#if PG_VERSION_NUM >= 190000
									,SO_NONE
#endif
		);
	reltuples = table_index_build_scan(ivfspool->heap, ivfspool->index, indexInfo,
									   true, progress, BuildCallback,
									   (void *) &buildstate, scan);

	/* Execute this worker's part of the sort */
	tuplesort_performsort(ivfspool->sortstate);

	/* Record statistics */
	SpinLockAcquire(&ivfshared->mutex);
	ivfshared->nparticipantsdone++;
	ivfshared->reltuples += reltuples;
	ivfshared->indtuples += buildstate.indtuples;
#ifdef IVFFLAT_KMEANS_DEBUG
	ivfshared->inertia += buildstate.inertia;
#endif
	SpinLockRelease(&ivfshared->mutex);

	/* Log statistics */
	if (progress)
		ereport(DEBUG1, (errmsg("leader processed " INT64_FORMAT " tuples", (int64) reltuples)));
	else
		ereport(DEBUG1, (errmsg("worker processed " INT64_FORMAT " tuples", (int64) reltuples)));

	/* Notify leader */
	ConditionVariableSignal(&ivfshared->workersdonecv);

	/* We can end tuplesorts immediately */
	tuplesort_end(ivfspool->sortstate);

	FreeBuildState(&buildstate);
}

/*
 * Perform work within a launched parallel process
 */
void
IvfflatParallelBuildMain(dsm_segment *seg, shm_toc *toc)
{
	char	   *sharedquery;
	IvfflatSpool *ivfspool;
	IvfflatShared *ivfshared;
	Sharedsort *sharedsort;
	char	   *ivfcenters;
	Relation	heapRel;
	Relation	indexRel;
	LOCKMODE	heapLockmode;
	LOCKMODE	indexLockmode;
	int			sortmem;

	/* Set debug_query_string for individual workers first */
	sharedquery = shm_toc_lookup(toc, PARALLEL_KEY_QUERY_TEXT, true);
	debug_query_string = sharedquery;

	/* Report the query string from leader */
	pgstat_report_activity(STATE_RUNNING, debug_query_string);

	/* Look up shared state */
	ivfshared = shm_toc_lookup(toc, PARALLEL_KEY_IVFFLAT_SHARED, false);

	/* Open relations using lock modes known to be obtained by index.c */
	if (!ivfshared->isconcurrent)
	{
		heapLockmode = ShareLock;
		indexLockmode = AccessExclusiveLock;
	}
	else
	{
		heapLockmode = ShareUpdateExclusiveLock;
		indexLockmode = RowExclusiveLock;
	}

	/* Open relations within worker */
	heapRel = table_open(ivfshared->heaprelid, heapLockmode);
	indexRel = index_open(ivfshared->indexrelid, indexLockmode);

	/* Initialize worker's own spool */
	ivfspool = palloc0_object(IvfflatSpool);
	ivfspool->heap = heapRel;
	ivfspool->index = indexRel;

	/* Look up shared state private to tuplesort.c */
	sharedsort = shm_toc_lookup(toc, PARALLEL_KEY_TUPLESORT, false);
	tuplesort_attach_shared(sharedsort, seg);

	ivfcenters = shm_toc_lookup(toc, PARALLEL_KEY_IVFFLAT_CENTERS, false);

	/* Perform sorting */
	sortmem = maintenance_work_mem / ivfshared->scantuplesortstates;
	IvfflatParallelScanAndSort(ivfspool, ivfshared, sharedsort, ivfcenters, sortmem, false);

	/* Close relations within worker */
	index_close(indexRel, indexLockmode);
	table_close(heapRel, heapLockmode);
}

/*
 * End parallel build
 */
static void
IvfflatEndParallel(IvfflatLeader * ivfleader)
{
	/* Shutdown worker processes */
	WaitForParallelWorkersToFinish(ivfleader->pcxt);

	/* Free last reference to MVCC snapshot, if one was used */
	if (IsMVCCSnapshot(ivfleader->snapshot))
		UnregisterSnapshot(ivfleader->snapshot);
	DestroyParallelContext(ivfleader->pcxt);
	ExitParallelMode();
}

/*
 * Return size of shared memory required for parallel index build
 */
static Size
ParallelEstimateShared(Relation heap, Snapshot snapshot)
{
	return add_size(BUFFERALIGN(sizeof(IvfflatShared)), table_parallelscan_estimate(heap, snapshot));
}

/*
 * Within leader, participate as a parallel worker
 */
static void
IvfflatLeaderParticipateAsWorker(IvfflatBuildState * buildstate)
{
	IvfflatLeader *ivfleader = buildstate->ivfleader;
	IvfflatSpool *leaderworker;
	int			sortmem;

	/* Allocate memory and initialize private spool */
	leaderworker = palloc0_object(IvfflatSpool);
	leaderworker->heap = buildstate->heap;
	leaderworker->index = buildstate->index;

	/* Perform work common to all participants */
	sortmem = maintenance_work_mem / ivfleader->nparticipanttuplesorts;
	IvfflatParallelScanAndSort(leaderworker, ivfleader->ivfshared,
							   ivfleader->sharedsort, ivfleader->ivfcenters,
							   sortmem, true);
}

/*
 * Begin parallel build
 */
static void
IvfflatBeginParallel(IvfflatBuildState * buildstate, bool isconcurrent, int request)
{
	ParallelContext *pcxt;
	int			scantuplesortstates;
	Snapshot	snapshot;
	Size		estivfshared;
	Size		estsort;
	Size		estcenters;
	IvfflatShared *ivfshared;
	Sharedsort *sharedsort;
	char	   *ivfcenters;
	IvfflatLeader *ivfleader = palloc0_object(IvfflatLeader);
	bool		leaderparticipates = true;
	Size		querylen;

#ifdef DISABLE_LEADER_PARTICIPATION
	leaderparticipates = false;
#endif

	/* Enter parallel mode and create context */
	EnterParallelMode();
	Assert(request > 0);
	pcxt = CreateParallelContext("vector", "IvfflatParallelBuildMain", request);

	scantuplesortstates = leaderparticipates ? request + 1 : request;

	/* Get snapshot for table scan */
	if (!isconcurrent)
		snapshot = SnapshotAny;
	else
		snapshot = RegisterSnapshot(GetTransactionSnapshot());

	/* Estimate size of workspaces */
	estivfshared = ParallelEstimateShared(buildstate->heap, snapshot);
	shm_toc_estimate_chunk(&pcxt->estimator, estivfshared);
	estsort = tuplesort_estimate_shared(scantuplesortstates);
	shm_toc_estimate_chunk(&pcxt->estimator, estsort);
	estcenters = mul_size(buildstate->centers->itemsize, buildstate->centers->maxlen);
	shm_toc_estimate_chunk(&pcxt->estimator, estcenters);
	shm_toc_estimate_keys(&pcxt->estimator, 3);

	/* Finally, estimate PARALLEL_KEY_QUERY_TEXT space */
	if (debug_query_string)
	{
		querylen = strlen(debug_query_string);
		shm_toc_estimate_chunk(&pcxt->estimator, querylen + 1);
		shm_toc_estimate_keys(&pcxt->estimator, 1);
	}
	else
		querylen = 0;			/* keep compiler quiet */

	/* Everyone's had a chance to ask for space, so now create the DSM */
	InitializeParallelDSM(pcxt);

	/* If no DSM segment was available, back out (do serial build) */
	if (pcxt->seg == NULL)
	{
		if (IsMVCCSnapshot(snapshot))
			UnregisterSnapshot(snapshot);
		DestroyParallelContext(pcxt);
		ExitParallelMode();
		return;
	}

	/* Store shared build state, for which we reserved space */
	ivfshared = (IvfflatShared *) shm_toc_allocate(pcxt->toc, estivfshared);
	/* Initialize immutable state */
	ivfshared->heaprelid = RelationGetRelid(buildstate->heap);
	ivfshared->indexrelid = RelationGetRelid(buildstate->index);
	ivfshared->isconcurrent = isconcurrent;
	ivfshared->scantuplesortstates = scantuplesortstates;
	ConditionVariableInit(&ivfshared->workersdonecv);
	SpinLockInit(&ivfshared->mutex);
	/* Initialize mutable state */
	ivfshared->nparticipantsdone = 0;
	ivfshared->reltuples = 0;
	ivfshared->indtuples = 0;
#ifdef IVFFLAT_KMEANS_DEBUG
	ivfshared->inertia = 0;
#endif
	table_parallelscan_initialize(buildstate->heap,
								  ParallelTableScanFromIvfflatShared(ivfshared),
								  snapshot);

	/* Store shared tuplesort-private state, for which we reserved space */
	sharedsort = (Sharedsort *) shm_toc_allocate(pcxt->toc, estsort);
	tuplesort_initialize_shared(sharedsort, scantuplesortstates,
								pcxt->seg);

	ivfcenters = shm_toc_allocate(pcxt->toc, estcenters);
	memcpy(ivfcenters, buildstate->centers->items, estcenters);

	shm_toc_insert(pcxt->toc, PARALLEL_KEY_IVFFLAT_SHARED, ivfshared);
	shm_toc_insert(pcxt->toc, PARALLEL_KEY_TUPLESORT, sharedsort);
	shm_toc_insert(pcxt->toc, PARALLEL_KEY_IVFFLAT_CENTERS, ivfcenters);

	/* Store query string for workers */
	if (debug_query_string)
	{
		char	   *sharedquery;

		sharedquery = (char *) shm_toc_allocate(pcxt->toc, querylen + 1);
		memcpy(sharedquery, debug_query_string, querylen + 1);
		shm_toc_insert(pcxt->toc, PARALLEL_KEY_QUERY_TEXT, sharedquery);
	}

	/* Launch workers, saving status for leader/caller */
	LaunchParallelWorkers(pcxt);
	ivfleader->pcxt = pcxt;
	ivfleader->nparticipanttuplesorts = pcxt->nworkers_launched;
	if (leaderparticipates)
		ivfleader->nparticipanttuplesorts++;
	ivfleader->ivfshared = ivfshared;
	ivfleader->sharedsort = sharedsort;
	ivfleader->snapshot = snapshot;
	ivfleader->ivfcenters = ivfcenters;

	/* If no workers were successfully launched, back out (do serial build) */
	if (pcxt->nworkers_launched == 0)
	{
		IvfflatEndParallel(ivfleader);
		return;
	}

	/* Log participants */
	ereport(DEBUG1, (errmsg("using %d parallel workers", pcxt->nworkers_launched)));

	/* Save leader state now that it's clear build will be parallel */
	buildstate->ivfleader = ivfleader;

	/* Join heap scan ourselves */
	if (leaderparticipates)
		IvfflatLeaderParticipateAsWorker(buildstate);

	/* Wait for all launched workers */
	WaitForParallelWorkersToAttach(pcxt);
}

/*
 * Scan table for tuples to index
 */
static void
AssignTuples(IvfflatBuildState * buildstate)
{
	int			parallel_workers = 0;
	SortCoordinate coordinate = NULL;

	pgstat_progress_update_param(PROGRESS_CREATEIDX_SUBPHASE, PROGRESS_IVFFLAT_PHASE_ASSIGN);

	/* Calculate parallel workers */
	if (buildstate->heap != NULL)
		parallel_workers = plan_create_index_workers(RelationGetRelid(buildstate->heap), RelationGetRelid(buildstate->index));

	/* Attempt to launch parallel worker scan when required */
	if (parallel_workers > 0)
		IvfflatBeginParallel(buildstate, buildstate->indexInfo->ii_Concurrent, parallel_workers);

	/* Set up coordination state if at least one worker launched */
	if (buildstate->ivfleader)
	{
		coordinate = palloc0_object(SortCoordinateData);
		coordinate->isWorker = false;
		coordinate->nParticipants = buildstate->ivfleader->nparticipanttuplesorts;
		coordinate->sharedsort = buildstate->ivfleader->sharedsort;
	}

	/* Begin serial/leader tuplesort */
	buildstate->sortstate = InitBuildSortState(buildstate->sortdesc, maintenance_work_mem, coordinate);

	/* Add tuples to sort */
	if (buildstate->heap != NULL)
	{
		if (buildstate->ivfleader)
			buildstate->reltuples = ParallelHeapScan(buildstate);
		else
			buildstate->reltuples = table_index_build_scan(buildstate->heap, buildstate->index, buildstate->indexInfo,
														   true, true, BuildCallback, (void *) buildstate, NULL);

#ifdef IVFFLAT_KMEANS_DEBUG
		PrintKmeansMetrics(buildstate);
#endif
	}
}

/*
 * Create entry pages
 */
static void
CreateEntryPages(IvfflatBuildState * buildstate, ForkNumber forkNum)
{
	/* Assign */
	IvfflatBench("assign tuples", AssignTuples(buildstate));

	/* Sort */
	IvfflatBench("sort tuples", tuplesort_performsort(buildstate->sortstate));

	/* Load */
	IvfflatBench("load tuples", InsertTuples(buildstate->index, buildstate, forkNum));

	/* End sort */
	tuplesort_end(buildstate->sortstate);

	/* End parallel build */
	if (buildstate->ivfleader)
		IvfflatEndParallel(buildstate->ivfleader);
}

/*
 * Build the index
 */
static void
BuildIndex(Relation heap, Relation index, IndexInfo *indexInfo,
		   IvfflatBuildState * buildstate, ForkNumber forkNum)
{
	InitBuildState(buildstate, heap, index, indexInfo);

	ComputeCenters(buildstate);

	/* Create pages */
	CreateMetaPage(index, buildstate, forkNum);
	CreateListPages(index, buildstate, forkNum);
	CreateEntryPages(buildstate, forkNum);

	/* Write WAL for initialization fork since GenericXLog functions do not */
	if (forkNum == INIT_FORKNUM)
		log_newpage_range(index, forkNum, 0, RelationGetNumberOfBlocksInFork(index, forkNum), true);

	FreeBuildState(buildstate);
}

/*
 * Build the index for a logged table
 */
IndexBuildResult *
ivfflatbuild(Relation heap, Relation index, IndexInfo *indexInfo)
{
	IndexBuildResult *result;
	IvfflatBuildState buildstate;

#ifdef IVFFLAT_BENCH
	SeedRandom(42);
#endif

	BuildIndex(heap, index, indexInfo, &buildstate, MAIN_FORKNUM);

	result = palloc_object(IndexBuildResult);
	result->heap_tuples = buildstate.reltuples;
	result->index_tuples = buildstate.indtuples;

	return result;
}

/*
 * Build the index for an unlogged table
 */
void
ivfflatbuildempty(Relation index)
{
	IndexInfo  *indexInfo = BuildIndexInfo(index);
	IvfflatBuildState buildstate;

	BuildIndex(NULL, index, indexInfo, &buildstate, INIT_FORKNUM);
}
