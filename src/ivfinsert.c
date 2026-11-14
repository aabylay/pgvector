#include "postgres.h"

#include <float.h>

#include "access/genam.h"
#include "access/generic_xlog.h"
#include "access/itup.h"
#include "fmgr.h"
#include "ivfflat.h"
#include "nodes/execnodes.h"
#include "storage/bufmgr.h"
#include "storage/lmgr.h"
#include "utils/memutils.h"
#include "utils/rel.h"

/*
 * Find the list that minimizes the distance function
 */
static void
FindInsertPage(Relation index, Datum *values, int version, int includeCount,
			   BlockNumber *insertPage, ListInfo * listInfo)
{
	double		minDistance = DBL_MAX;
	BlockNumber nextblkno = IVFFLAT_HEAD_BLKNO;
	bool		chosen = false;
	FmgrInfo   *procinfo;
	Oid			collation;

	/* Avoid compiler warning */
	listInfo->blkno = nextblkno;
	listInfo->offno = FirstOffsetNumber;

	procinfo = index_getprocinfo(index, 1, IVFFLAT_DISTANCE_PROC);
	collation = index->rd_indcollation[0];

	/* Search all list pages */
	while (BlockNumberIsValid(nextblkno))
	{
		Buffer		cbuf;
		Page		cpage;
		OffsetNumber maxoffno;

		cbuf = ReadBuffer(index, nextblkno);
		LockBuffer(cbuf, BUFFER_LOCK_SHARE);
		cpage = BufferGetPage(cbuf);
		maxoffno = PageGetMaxOffsetNumber(cpage);

		for (OffsetNumber offno = FirstOffsetNumber; offno <= maxoffno; offno = OffsetNumberNext(offno))
		{
			IvfflatList list;
			double		distance;

			list = (IvfflatList) PageGetItem(cpage, PageGetItemId(cpage, offno));

			if (version == IVFFLAT_VERSION_INCLUDE)
			{
				Pointer		center = (Pointer) IvfflatListGetCenter((IvfflatListDataV2 *) list, includeCount);

				distance = DatumGetFloat8(FunctionCall2Coll(procinfo, collation, values[0], PointerGetDatum(center)));
			}
			else
				distance = DatumGetFloat8(FunctionCall2Coll(procinfo, collation, values[0], PointerGetDatum(&list->center)));

			/*
			 * An empty version-2 list has an invalid insert page. That is
			 * not "no list chosen yet": the first field of both list structs
			 * is startPage, and insertPage is the second.
			 */
			if (!chosen || distance < minDistance)
			{
				*insertPage = list->insertPage;
				listInfo->blkno = nextblkno;
				listInfo->offno = offno;
				minDistance = distance;
				chosen = true;
			}
		}

		nextblkno = IvfflatPageGetOpaque(cpage)->nextblkno;

		UnlockReleaseBuffer(cbuf);
	}
}

/*
 * Insert a tuple into the index
 */
static void
InsertTuple(Relation index, Datum *values, bool *isnull, ItemPointer heap_tid)
{
	const		IvfflatTypeInfo *typeInfo = IvfflatGetTypeInfo(index);
	Oid			collation = index->rd_indcollation[0];
	int			dimensions;
	IndexTuple	itup;
	Datum		value;
	FmgrInfo   *normprocinfo;
	Buffer		buf;
	Page		page;
	GenericXLogState *state;
	Size		itemsz;
	BlockNumber insertPage = InvalidBlockNumber;
	ListInfo	listInfo;
	BlockNumber originalInsertPage;

	/* Detoast once for all calls */
	value = PointerGetDatum(PG_DETOAST_DATUM(values[0]));

	/* Normalize if needed */
	normprocinfo = IvfflatOptionalProcInfo(index, IVFFLAT_NORM_PROC);
	if (normprocinfo != NULL)
	{
		if (!IvfflatCheckNorm(normprocinfo, collation, value))
			return;

		value = IvfflatNormValue(typeInfo, collation, value);
	}

	/* Ensure index is valid */
	IvfflatGetMetaPageInfo(index, NULL, &dimensions);

	/* Check dimensions match index */
	IvfflatCheckDim(dimensions, typeInfo, collation, value);

	/* Find the insert page - sets the page and list info */
	FindInsertPage(index, &value, IVFFLAT_VERSION, 0, &insertPage, &listInfo);
	Assert(BlockNumberIsValid(insertPage));
	originalInsertPage = insertPage;

	/* Form tuple */
	itup = index_form_tuple(RelationGetDescr(index), &value, isnull);
	itup->t_tid = *heap_tid;

	/* Get tuple size */
	itemsz = MAXALIGN(IndexTupleSize(itup));
	Assert(itemsz <= BLCKSZ - MAXALIGN(SizeOfPageHeaderData) - MAXALIGN(sizeof(IvfflatPageOpaqueData)) - sizeof(ItemIdData));

	/* Find a page to insert the item */
	for (;;)
	{
		buf = ReadBuffer(index, insertPage);
		LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);

		state = GenericXLogStart(index);
		page = GenericXLogRegisterBuffer(state, buf, 0);

		if (PageGetFreeSpace(page) >= itemsz)
			break;

		insertPage = IvfflatPageGetOpaque(page)->nextblkno;

		if (BlockNumberIsValid(insertPage))
		{
			/* Move to next page */
			GenericXLogAbort(state);
			UnlockReleaseBuffer(buf);
		}
		else
		{
			Buffer		newbuf;
			Page		newpage;

			/* Add a new page */
			LockRelationForExtension(index, ExclusiveLock);
			newbuf = IvfflatNewBuffer(index, MAIN_FORKNUM);
			UnlockRelationForExtension(index, ExclusiveLock);

			/* Init new page */
			newpage = GenericXLogRegisterBuffer(state, newbuf, GENERIC_XLOG_FULL_IMAGE);
			IvfflatInitPage(newbuf, newpage, IVFFLAT_VERSION);

			/* Update insert page */
			insertPage = BufferGetBlockNumber(newbuf);

			/* Update previous buffer */
			IvfflatPageGetOpaque(page)->nextblkno = insertPage;

			/* Commit */
			GenericXLogFinish(state);

			/* Unlock previous buffer */
			UnlockReleaseBuffer(buf);

			/* Prepare new buffer */
			state = GenericXLogStart(index);
			buf = newbuf;
			page = GenericXLogRegisterBuffer(state, buf, 0);
			break;
		}
	}

	/* Add to next offset */
	if (PageAddItem(page, (Item) itup, itemsz, InvalidOffsetNumber, false, false) == InvalidOffsetNumber)
		elog(ERROR, "failed to add index item to \"%s\"", RelationGetRelationName(index));

	IvfflatCommitBuffer(buf, state);

	/* Update the insert page */
	if (insertPage != originalInsertPage)
		IvfflatUpdateList(index, listInfo, insertPage, originalInsertPage, InvalidBlockNumber, MAIN_FORKNUM);
}

/*
 * Extra buffers pinned by one version-2 insert record. The list header is
 * not in this list: the caller keeps that lock until the vector item lands.
 */
typedef struct InsertWal
{
	GenericXLogState *state;
	Buffer		extra[MAX_GENERIC_XLOG_PAGES];
	int			nextra;
}			InsertWal;

static void
InsertWalInit(InsertWal *wal, Relation index)
{
	wal->state = GenericXLogStart(index);
	wal->nextra = 0;
}

static Page
InsertWalRegister(InsertWal *wal, Buffer buf, int flags)
{
	int			i;

	for (i = 0; i < wal->nextra; i++)
	{
		if (wal->extra[i] == buf)
			break;
	}

	if (i == wal->nextra)
	{
		if (wal->nextra >= MAX_GENERIC_XLOG_PAGES)
			elog(ERROR, "too many ivfflat pages in one insert");
		wal->extra[wal->nextra++] = buf;
	}

	return GenericXLogRegisterBuffer(wal->state, buf, flags);
}

static void
InsertWalFinish(InsertWal *wal)
{
	GenericXLogFinish(wal->state);
	wal->state = NULL;

	for (int i = 0; i < wal->nextra; i++)
		UnlockReleaseBuffer(wal->extra[i]);

	wal->nextra = 0;
}

static void
PatchListHwm(Relation index, Buffer listbuf, OffsetNumber offno, uint32 entry_id)
{
	GenericXLogState *state = GenericXLogStart(index);
	Page		page = GenericXLogRegisterBuffer(state, listbuf, 0);
	IvfflatListDataV2 *list = (IvfflatListDataV2 *) PageGetItem(page, PageGetItemId(page, offno));

	list->entry_hwm = entry_id + 1;
	list->live_count++;
	GenericXLogFinish(state);
}

static void
PatchChainPointer(Relation index, Buffer listbuf, OffsetNumber offno, int chainIndex,
				  bool isVector, BlockNumber startPage, bool updateStart,
				  BlockNumber insertPage, BlockNumber originalInsert)
{
	GenericXLogState *state = GenericXLogStart(index);
	Page		page = GenericXLogRegisterBuffer(state, listbuf, 0);
	IvfflatListDataV2 *list = (IvfflatListDataV2 *) PageGetItem(page, PageGetItemId(page, offno));
	BlockNumber *startp;
	BlockNumber *insertp;
	bool		changed = false;

	if (isVector)
	{
		startp = &list->startPage;
		insertp = &list->insertPage;
	}
	else
	{
		IvfflatIncludeChain *chain = &IvfflatListGetChains(list)[chainIndex];

		startp = &chain->startPage;
		insertp = &chain->insertPage;
	}

	if (updateStart && *startp != startPage)
	{
		*startp = startPage;
		changed = true;
	}

	if (BlockNumberIsValid(insertPage) && *insertp != insertPage &&
		(!BlockNumberIsValid(originalInsert) || insertPage >= originalInsert))
	{
		*insertp = insertPage;
		changed = true;
	}

	if (changed)
		GenericXLogFinish(state);
	else
		GenericXLogAbort(state);
}

/*
 * Find the directory entry for a vector page, or the last directory page.
 * The returned buffer is left locked. Earlier pages are released.
 */
static bool
FindDirEntry(Relation index, BlockNumber dirStart, BlockNumber vectorBlk,
			 Buffer *bufOut, OffsetNumber *offOut, bool *lastHasSpace)
{
	BlockNumber blk = dirStart;

	*bufOut = InvalidBuffer;
	*offOut = InvalidOffsetNumber;
	*lastHasSpace = false;

	while (BlockNumberIsValid(blk))
	{
		Buffer		buf = ReadBuffer(index, blk);
		Page		page;
		OffsetNumber maxoffno;
		BlockNumber next;

		LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
		page = BufferGetPage(buf);
		maxoffno = PageGetMaxOffsetNumber(page);

		for (OffsetNumber offno = FirstOffsetNumber; offno <= maxoffno; offno = OffsetNumberNext(offno))
		{
			IvfflatVectorDirEntry *ent = (IvfflatVectorDirEntry *) PageGetItem(page, PageGetItemId(page, offno));

			if (ent->blkno == vectorBlk)
			{
				*bufOut = buf;
				*offOut = offno;
				return true;
			}
		}

		next = IvfflatPageGetOpaque(page)->nextblkno;

		if (!BlockNumberIsValid(next))
		{
			*bufOut = buf;
			*lastHasSpace = PageGetFreeSpace(page) >= sizeof(IvfflatVectorDirEntry);
			return false;
		}

		UnlockReleaseBuffer(buf);
		blk = next;
	}

	return false;
}

static void
DirBumpEntry(IvfflatVectorDirEntry *ent, uint32 entry_id)
{
	if (ent->live_items == 0)
	{
		ent->min_entry_id = entry_id;
		ent->max_entry_id = entry_id;
		ent->live_items = 1;
		return;
	}

	if (entry_id > ent->max_entry_id)
		ent->max_entry_id = entry_id;
	ent->live_items++;
}

/*
 * Record the vector page in the directory, on the same WAL record as the item.
 */
static void
AttachDirectory(Relation index, InsertWal *wal, Buffer listbuf, OffsetNumber listoff,
				BlockNumber vectorBlk, uint32 entry_id, bool isNewPage)
{
	/* New pages are appended; an existing entry is updated in place. */
	Page		lpage = BufferGetPage(listbuf);
	IvfflatListDataV2 *list = (IvfflatListDataV2 *) PageGetItem(lpage, PageGetItemId(lpage, listoff));
	BlockNumber dirStart = list->dirStartPage;
	IvfflatVectorDirEntry ent;

	(void) isNewPage;

	ent.blkno = vectorBlk;
	ent.min_entry_id = entry_id;
	ent.max_entry_id = entry_id;
	ent.live_items = 1;
	ent.unused = 0;

	if (BlockNumberIsValid(dirStart))
	{
		Buffer		dbuf;
		OffsetNumber offno;
		bool		lastHasSpace;
		bool		found;

		found = FindDirEntry(index, dirStart, vectorBlk, &dbuf, &offno, &lastHasSpace);

		if (found)
		{
			Page		dpage = InsertWalRegister(wal, dbuf, 0);
			IvfflatVectorDirEntry *slot = (IvfflatVectorDirEntry *) PageGetItem(dpage, PageGetItemId(dpage, offno));

			/* A brand-new page has no entry. Bump is the right update if one exists. */
			DirBumpEntry(slot, entry_id);
			return;
		}

		if (lastHasSpace)
		{
			Page		dpage = InsertWalRegister(wal, dbuf, 0);

			if (PageAddItem(dpage, (Item) &ent, sizeof(ent), InvalidOffsetNumber, false, false) == InvalidOffsetNumber)
				elog(ERROR, "failed to add index item to \"%s\"", RelationGetRelationName(index));
			return;
		}

		if (!found)
		{
			Buffer		newbuf;
			Page		newpage;
			Page		oldpage;

			LockRelationForExtension(index, ExclusiveLock);
			newbuf = IvfflatNewBuffer(index, MAIN_FORKNUM);
			UnlockRelationForExtension(index, ExclusiveLock);

			oldpage = InsertWalRegister(wal, dbuf, 0);
			newpage = InsertWalRegister(wal, newbuf, GENERIC_XLOG_FULL_IMAGE);
			IvfflatInitPage(newbuf, newpage, IVFFLAT_VERSION_INCLUDE);
			IvfflatPageGetOpaque(oldpage)->nextblkno = BufferGetBlockNumber(newbuf);

			if (PageAddItem(newpage, (Item) &ent, sizeof(ent), InvalidOffsetNumber, false, false) == InvalidOffsetNumber)
				elog(ERROR, "failed to add index item to \"%s\"", RelationGetRelationName(index));
			return;
		}
	}

	{
		Buffer		newbuf;
		Page		newpage;
		Page		patched;
		IvfflatListDataV2 *patchedList;

		LockRelationForExtension(index, ExclusiveLock);
		newbuf = IvfflatNewBuffer(index, MAIN_FORKNUM);
		UnlockRelationForExtension(index, ExclusiveLock);

		newpage = InsertWalRegister(wal, newbuf, GENERIC_XLOG_FULL_IMAGE);
		IvfflatInitPage(newbuf, newpage, IVFFLAT_VERSION_INCLUDE);

		if (PageAddItem(newpage, (Item) &ent, sizeof(ent), InvalidOffsetNumber, false, false) == InvalidOffsetNumber)
			elog(ERROR, "failed to add index item to \"%s\"", RelationGetRelationName(index));

		patched = GenericXLogRegisterBuffer(wal->state, listbuf, 0);
		patchedList = (IvfflatListDataV2 *) PageGetItem(patched, PageGetItemId(patched, listoff));
		patchedList->dirStartPage = BufferGetBlockNumber(newbuf);
	}
}

/*
 * Append one version-2 entry item to a chain. withDirectory is only for the
 * vector chain: the directory update shares the item's WAL record.
 */
static void
WriteEntryOnChain(Relation index, Buffer listbuf, OffsetNumber listoff, int chainIndex,
				  bool isVector, bool withDirectory, Item item, Size itemsz, uint32 entry_id)
{
	Page		raw = BufferGetPage(listbuf);
	IvfflatListDataV2 *list = (IvfflatListDataV2 *) PageGetItem(raw, PageGetItemId(raw, listoff));
	BlockNumber startPage;
	BlockNumber insertPage;
	BlockNumber originalInsert;
	bool		updateStart = false;
	bool		created = false;
	InsertWal	wal;
	Buffer		buf;
	Page		page;

	if (isVector)
	{
		startPage = list->startPage;
		insertPage = list->insertPage;
	}
	else
	{
		IvfflatIncludeChain *chain = &IvfflatListGetChains(list)[chainIndex];

		startPage = chain->startPage;
		insertPage = chain->insertPage;
	}

	originalInsert = insertPage;

	if (!BlockNumberIsValid(insertPage))
	{
		Page		patched;
		IvfflatListDataV2 *patchedList;

		LockRelationForExtension(index, ExclusiveLock);
		buf = IvfflatNewBuffer(index, MAIN_FORKNUM);
		UnlockRelationForExtension(index, ExclusiveLock);

		InsertWalInit(&wal, index);
		page = InsertWalRegister(&wal, buf, GENERIC_XLOG_FULL_IMAGE);
		IvfflatInitPage(buf, page, IVFFLAT_VERSION_INCLUDE);
		insertPage = BufferGetBlockNumber(buf);
		startPage = insertPage;

		if (PageAddItem(page, item, itemsz, InvalidOffsetNumber, false, false) == InvalidOffsetNumber)
			elog(ERROR, "failed to add index item to \"%s\"", RelationGetRelationName(index));

		IvfflatPageOpaqueAdd(page, entry_id);

		patched = GenericXLogRegisterBuffer(wal.state, listbuf, 0);
		patchedList = (IvfflatListDataV2 *) PageGetItem(patched, PageGetItemId(patched, listoff));

		if (isVector)
		{
			patchedList->startPage = startPage;
			patchedList->insertPage = insertPage;
		}
		else
		{
			IvfflatIncludeChain *chain = &IvfflatListGetChains(patchedList)[chainIndex];

			chain->startPage = startPage;
			chain->insertPage = insertPage;
		}

		if (withDirectory)
			AttachDirectory(index, &wal, listbuf, listoff, insertPage, entry_id, true);

		InsertWalFinish(&wal);
		return;
	}

	for (;;)
	{
		buf = ReadBuffer(index, insertPage);
		LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
		InsertWalInit(&wal, index);
		page = GenericXLogRegisterBuffer(wal.state, buf, 0);

		if (PageGetFreeSpace(page) >= itemsz)
			break;

		insertPage = IvfflatPageGetOpaque(page)->nextblkno;

		if (BlockNumberIsValid(insertPage))
		{
			GenericXLogAbort(wal.state);
			UnlockReleaseBuffer(buf);
		}
		else
		{
			Buffer		newbuf;
			Page		newpage;

			LockRelationForExtension(index, ExclusiveLock);
			newbuf = IvfflatNewBuffer(index, MAIN_FORKNUM);
			UnlockRelationForExtension(index, ExclusiveLock);

			newpage = GenericXLogRegisterBuffer(wal.state, newbuf, GENERIC_XLOG_FULL_IMAGE);
			IvfflatInitPage(newbuf, newpage, IVFFLAT_VERSION_INCLUDE);
			insertPage = BufferGetBlockNumber(newbuf);
			IvfflatPageGetOpaque(page)->nextblkno = insertPage;

			GenericXLogFinish(wal.state);
			UnlockReleaseBuffer(buf);

			InsertWalInit(&wal, index);
			buf = newbuf;
			page = GenericXLogRegisterBuffer(wal.state, buf, 0);
			created = true;
			break;
		}
	}

	if (PageAddItem(page, item, itemsz, InvalidOffsetNumber, false, false) == InvalidOffsetNumber)
		elog(ERROR, "failed to add index item to \"%s\"", RelationGetRelationName(index));

	IvfflatPageOpaqueAdd(page, entry_id);

	if (withDirectory)
		AttachDirectory(index, &wal, listbuf, listoff, insertPage, entry_id, created);

	/* The chain page was not added to wal.extra. */
	GenericXLogFinish(wal.state);
	UnlockReleaseBuffer(buf);

	for (int i = 0; i < wal.nextra; i++)
		UnlockReleaseBuffer(wal.extra[i]);
	wal.nextra = 0;

	if (updateStart || insertPage != originalInsert)
		PatchChainPointer(index, listbuf, listoff, chainIndex, isVector,
						  startPage, updateStart, insertPage, originalInsert);
}

/*
 * Insert one tuple into a version-2 index.
 *
 * The list-header lock is held from id allocation through the vector write.
 * Column items are durable before the vector item. The directory entry shares
 * the vector item's WAL record, so a committed row is visible to both walks.
 */
static void
InsertTupleV2(Relation index, Datum *values, bool *isnull, ItemPointer heap_tid,
			  IvfflatMetaInfo *meta)
{
	const		IvfflatTypeInfo *typeInfo = IvfflatGetTypeInfo(index);
	FmgrInfo   *normprocinfo;
	Datum		value;
	int			includeCount = meta->includeCount;
	TupleDesc	vectorDesc;
	TupleDesc  *colDesc;
	IndexTuple	vectorTup;
	IndexTuple *colTups;
	BlockNumber insertPage = InvalidBlockNumber;
	ListInfo	listInfo;
	Buffer		listbuf;
	Page		lpage;
	IvfflatListDataV2 *list;
	uint32		entry_id;

	Oid			collation = index->rd_indcollation[0];

	value = PointerGetDatum(PG_DETOAST_DATUM(values[0]));

	normprocinfo = IvfflatOptionalProcInfo(index, IVFFLAT_NORM_PROC);
	if (normprocinfo != NULL)
	{
		if (!IvfflatCheckNorm(normprocinfo, collation, value))
			return;

		value = IvfflatNormValue(typeInfo, collation, value);
	}

	/* Check dimensions match index */
	IvfflatCheckDim(meta->dimensions, typeInfo, collation, value);

	colDesc = palloc_array_checked(TupleDesc, includeCount);
	colTups = palloc_array_checked(IndexTuple, includeCount);

	for (int c = 0; c < includeCount; c++)
	{
		Form_pg_attribute att = TupleDescAttr(RelationGetDescr(index), c + 1);
		bool		colnull = isnull[c + 1];
		Datum		colval = IvfflatPrepareIncludeDatum(att, values[c + 1], colnull);

		colDesc[c] = IvfflatOneColumnDesc(index, (AttrNumber) (c + 2));
		colTups[c] = IvfflatFormColumnTuple(colDesc[c], colval, colnull, heap_tid);
	}

	vectorDesc = IvfflatOneColumnDesc(index, 1);
	vectorTup = IvfflatFormColumnTuple(vectorDesc, value, false, heap_tid);

	FindInsertPage(index, &value, IVFFLAT_VERSION_INCLUDE, includeCount, &insertPage, &listInfo);

	listbuf = ReadBuffer(index, listInfo.blkno);
	LockBuffer(listbuf, BUFFER_LOCK_EXCLUSIVE);
	lpage = BufferGetPage(listbuf);
	list = (IvfflatListDataV2 *) PageGetItem(lpage, PageGetItemId(lpage, listInfo.offno));
	entry_id = list->entry_hwm;

	PatchListHwm(index, listbuf, listInfo.offno, entry_id);

	for (int c = 0; c < includeCount; c++)
	{
		Size		itemsz;
		char	   *item = IvfflatFormEntryItem(entry_id, colTups[c], &itemsz);

		WriteEntryOnChain(index, listbuf, listInfo.offno, c, false, false,
						  (Item) item, itemsz, entry_id);
		pfree(item);
		pfree(colTups[c]);
		FreeTupleDesc(colDesc[c]);
	}

	{
		Size		itemsz;
		char	   *item = IvfflatFormEntryItem(entry_id, vectorTup, &itemsz);

		WriteEntryOnChain(index, listbuf, listInfo.offno, 0, true, true,
						  (Item) item, itemsz, entry_id);
		pfree(item);
	}

	UnlockReleaseBuffer(listbuf);

	pfree(vectorTup);
	FreeTupleDesc(vectorDesc);
	pfree(colTups);
	pfree(colDesc);
}

/*
 * Insert a tuple into the index
 */
bool
ivfflatinsert(Relation index, Datum *values, bool *isnull, ItemPointer heap_tid,
			  Relation heap, IndexUniqueCheck checkUnique, bool indexUnchanged, IndexInfo *indexInfo)
{
	MemoryContext oldCtx;
	MemoryContext insertCtx;

	/* Skip nulls */
	if (isnull[0])
		return false;

	/*
	 * Use memory context since detoast, IvfflatNormValue, and
	 * index_form_tuple can allocate
	 */
	insertCtx = AllocSetContextCreate(CurrentMemoryContext,
									  "Ivfflat insert temporary context",
									  ALLOCSET_DEFAULT_SIZES);
	oldCtx = MemoryContextSwitchTo(insertCtx);

	/* Insert tuple */
	{
		IvfflatMetaInfo meta;

		IvfflatReadMeta(index, &meta);
		if (meta.version == IVFFLAT_VERSION_INCLUDE)
			InsertTupleV2(index, values, isnull, heap_tid, &meta);
		else
			InsertTuple(index, values, isnull, heap_tid);
	}

	/* Delete memory context */
	MemoryContextSwitchTo(oldCtx);
	MemoryContextDelete(insertCtx);

	return false;
}
