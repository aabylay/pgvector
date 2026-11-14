#include "postgres.h"

#include "access/genam.h"
#include "access/generic_xlog.h"
#include "access/itup.h"
#include "commands/vacuum.h"
#include "common/hashfn.h"
#include "ivfflat.h"
#include "storage/bufmgr.h"
#include "utils/relcache.h"

/*
 * Entry ids deleted from one list. Page offsets differ across chains, so
 * vacuum matches included-column items by entry_id.
 */
typedef struct DelSetEntry
{
	uint32		key;
	char		status;
}			DelSetEntry;

static inline uint32
DelSetHashKey(uint32 key)
{
	return murmurhash32(key);
}

#define SH_PREFIX delset
#define SH_ELEMENT_TYPE DelSetEntry
#define SH_KEY_TYPE uint32
#define SH_KEY key
#define SH_HASH_KEY(tb, key) DelSetHashKey(key)
#define SH_EQUAL(tb, a, b) ((a) == (b))
#define SH_SCOPE static inline
#define SH_DECLARE
#define SH_DEFINE
#include "lib/simplehash.h"

typedef struct VacList
{
	OffsetNumber offno;
	BlockNumber startPage;
	BlockNumber dirStartPage;
	uint32		live_count;
	BlockNumber vectorInsert;
	BlockNumber chainStart[IVFFLAT_MAX_INCLUDE_COLS];
	BlockNumber chainInsert[IVFFLAT_MAX_INCLUDE_COLS];
	uint32		removed;
}			VacList;

#if PG_VERSION_NUM >= 180000
#define vacuum_delay_point() vacuum_delay_point(false)
#endif

/*
 * Write the vector page's new range into its directory entry.
 */
static void
UpdateDirectoryEntry(Relation index, BlockNumber dirStart, BlockNumber vectorBlk,
					 uint32 min_entry_id, uint32 max_entry_id, uint16 live_items)
{
	BlockNumber blk = dirStart;

	if (!BlockNumberIsValid(dirStart))
		return;

	while (BlockNumberIsValid(blk))
	{
		Buffer		buf = ReadBuffer(index, blk);
		Page		page;
		OffsetNumber maxoffno;
		BlockNumber next;
		bool		updated = false;

		LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
		page = BufferGetPage(buf);
		maxoffno = PageGetMaxOffsetNumber(page);

		for (OffsetNumber offno = FirstOffsetNumber; offno <= maxoffno; offno = OffsetNumberNext(offno))
		{
			IvfflatVectorDirEntry *ent = (IvfflatVectorDirEntry *) PageGetItem(page, PageGetItemId(page, offno));

			if (ent->blkno == vectorBlk)
			{
				GenericXLogState *state = GenericXLogStart(index);
				Page		image = GenericXLogRegisterBuffer(state, buf, 0);
				IvfflatVectorDirEntry *slot = (IvfflatVectorDirEntry *) PageGetItem(image, PageGetItemId(image, offno));

				slot->min_entry_id = min_entry_id;
				slot->max_entry_id = max_entry_id;
				slot->live_items = live_items;
				GenericXLogFinish(state);
				updated = true;
				break;
			}
		}

		next = IvfflatPageGetOpaque(page)->nextblkno;
		UnlockReleaseBuffer(buf);

		if (updated)
			return;

		blk = next;
	}
}

/*
 * Delete dead vector entries and the included-column items that share their ids.
 * The list page stays exclusively locked so inserts into these lists wait.
 */
static void
VacuumOneListV2(Relation index, Buffer listbuf, VacList *info, int includeCount,
				IndexBulkDeleteResult *stats, IndexBulkDeleteCallback callback,
				void *callback_state, BufferAccessStrategy bas)
{
	delset_hash *dead = delset_create(CurrentMemoryContext, 128, NULL);
	BlockNumber searchPage = info->startPage;
	BlockNumber chainPage;

	while (BlockNumberIsValid(searchPage))
	{
		Buffer		buf;
		Page		page;
		GenericXLogState *state;
		OffsetNumber maxoffno;
		OffsetNumber deletable[MaxOffsetNumber];
		int			ndeletable;
		BlockNumber vectorBlk = searchPage;

		vacuum_delay_point();

		buf = ReadBufferExtended(index, MAIN_FORKNUM, searchPage, RBM_NORMAL, bas);
		LockBufferForCleanup(buf);

		state = GenericXLogStart(index);
		page = GenericXLogRegisterBuffer(state, buf, 0);
		maxoffno = PageGetMaxOffsetNumber(page);
		ndeletable = 0;

		for (OffsetNumber offno = FirstOffsetNumber; offno <= maxoffno; offno = OffsetNumberNext(offno))
		{
			Item		item = PageGetItem(page, PageGetItemId(page, offno));
			IndexTuple	itup = IvfflatEntryGetTuple(item);
			uint32		entry_id = IvfflatEntryGetId(item);

			if (callback(&itup->t_tid, callback_state))
			{
				bool		found;

				deletable[ndeletable++] = offno;
				delset_insert(dead, entry_id, &found);
				stats->tuples_removed++;
				info->removed++;
			}
			else
				stats->num_index_tuples++;
		}

		if (!BlockNumberIsValid(info->vectorInsert) && ndeletable > 0)
			info->vectorInsert = searchPage;

		searchPage = IvfflatPageGetOpaque(page)->nextblkno;

		if (ndeletable > 0)
		{
			uint32		min_entry_id;
			uint32		max_entry_id;
			uint16		live_items;

			PageIndexMultiDelete(page, deletable, ndeletable);
			IvfflatPageOpaqueRecompute(page);

			min_entry_id = IvfflatPageGetOpaqueV2(page)->min_entry_id;
			max_entry_id = IvfflatPageGetOpaqueV2(page)->max_entry_id;
			live_items = IvfflatPageGetOpaqueV2(page)->live_items;

			GenericXLogFinish(state);
			UnlockReleaseBuffer(buf);

			UpdateDirectoryEntry(index, info->dirStartPage, vectorBlk,
								 min_entry_id, max_entry_id, live_items);
		}
		else
		{
			GenericXLogAbort(state);
			UnlockReleaseBuffer(buf);
		}
	}

	for (int c = 0; c < includeCount; c++)
	{
		searchPage = info->chainStart[c];
		chainPage = InvalidBlockNumber;

		while (BlockNumberIsValid(searchPage))
		{
			Buffer		buf;
			Page		page;
			GenericXLogState *state;
			OffsetNumber maxoffno;
			OffsetNumber deletable[MaxOffsetNumber];
			int			ndeletable;

			vacuum_delay_point();

			buf = ReadBufferExtended(index, MAIN_FORKNUM, searchPage, RBM_NORMAL, bas);
			LockBufferForCleanup(buf);

			state = GenericXLogStart(index);
			page = GenericXLogRegisterBuffer(state, buf, 0);
			maxoffno = PageGetMaxOffsetNumber(page);
			ndeletable = 0;

			for (OffsetNumber offno = FirstOffsetNumber; offno <= maxoffno; offno = OffsetNumberNext(offno))
			{
				Item		item = PageGetItem(page, PageGetItemId(page, offno));
				uint32		entry_id = IvfflatEntryGetId(item);

				if (delset_lookup(dead, entry_id) != NULL)
					deletable[ndeletable++] = offno;
			}

			if (!BlockNumberIsValid(chainPage) && ndeletable > 0)
				chainPage = searchPage;

			searchPage = IvfflatPageGetOpaque(page)->nextblkno;

			if (ndeletable > 0)
			{
				PageIndexMultiDelete(page, deletable, ndeletable);
				IvfflatPageOpaqueRecompute(page);
				GenericXLogFinish(state);
			}
			else
				GenericXLogAbort(state);

			UnlockReleaseBuffer(buf);
		}

		info->chainInsert[c] = chainPage;
	}

	delset_destroy(dead);
}

static IndexBulkDeleteResult *
ivfflatbulkdelete_v2(IndexVacuumInfo *info, IndexBulkDeleteResult *stats,
					 IndexBulkDeleteCallback callback, void *callback_state,
					 int includeCount)
{
	Relation	index = info->index;
	BlockNumber blkno = IVFFLAT_HEAD_BLKNO;
	BufferAccessStrategy bas = GetAccessStrategy(BAS_BULKREAD);

	while (BlockNumberIsValid(blkno))
	{
		Buffer		cbuf;
		Page		cpage;
		OffsetNumber cmaxoffno;
		BlockNumber nextblkno;
		VacList    *lists;
		GenericXLogState *state;

		cbuf = ReadBuffer(index, blkno);
		LockBuffer(cbuf, BUFFER_LOCK_EXCLUSIVE);
		cpage = BufferGetPage(cbuf);
		cmaxoffno = PageGetMaxOffsetNumber(cpage);
		nextblkno = IvfflatPageGetOpaque(cpage)->nextblkno;

		lists = palloc0_array_checked(VacList, cmaxoffno);

		for (OffsetNumber coffno = FirstOffsetNumber; coffno <= cmaxoffno; coffno = OffsetNumberNext(coffno))
		{
			IvfflatListDataV2 *list = (IvfflatListDataV2 *) PageGetItem(cpage, PageGetItemId(cpage, coffno));
			IvfflatIncludeChain *chains = IvfflatListGetChains(list);
			VacList    *slot = &lists[coffno - FirstOffsetNumber];

			slot->offno = coffno;
			slot->startPage = list->startPage;
			slot->dirStartPage = list->dirStartPage;
			slot->live_count = list->live_count;
			slot->vectorInsert = InvalidBlockNumber;

			for (int c = 0; c < includeCount; c++)
			{
				slot->chainStart[c] = chains[c].startPage;
				slot->chainInsert[c] = InvalidBlockNumber;
			}
		}

		for (OffsetNumber coffno = FirstOffsetNumber; coffno <= cmaxoffno; coffno = OffsetNumberNext(coffno))
			VacuumOneListV2(index, cbuf, &lists[coffno - FirstOffsetNumber], includeCount,
							stats, callback, callback_state, bas);

		state = GenericXLogStart(index);
		cpage = GenericXLogRegisterBuffer(state, cbuf, 0);

		for (OffsetNumber coffno = FirstOffsetNumber; coffno <= cmaxoffno; coffno = OffsetNumberNext(coffno))
		{
			VacList    *slot = &lists[coffno - FirstOffsetNumber];
			IvfflatListDataV2 *list = (IvfflatListDataV2 *) PageGetItem(cpage, PageGetItemId(cpage, slot->offno));
			IvfflatIncludeChain *chains = IvfflatListGetChains(list);

			if (slot->removed > 0)
			{
				if (list->live_count >= slot->removed)
					list->live_count -= slot->removed;
				else
					list->live_count = 0;
			}

			if (BlockNumberIsValid(slot->vectorInsert))
				list->insertPage = slot->vectorInsert;

			for (int c = 0; c < includeCount; c++)
			{
				if (BlockNumberIsValid(slot->chainInsert[c]))
					chains[c].insertPage = slot->chainInsert[c];
			}
		}

		GenericXLogFinish(state);
		UnlockReleaseBuffer(cbuf);
		pfree(lists);

		blkno = nextblkno;
	}

	FreeAccessStrategy(bas);

	return stats;
}

/*
 * Bulk delete tuples from the index
 */
IndexBulkDeleteResult *
ivfflatbulkdelete(IndexVacuumInfo *info, IndexBulkDeleteResult *stats,
				  IndexBulkDeleteCallback callback, void *callback_state)
{
	Relation	index = info->index;
	IvfflatMetaInfo meta;
	BlockNumber blkno;
	BufferAccessStrategy bas;

	if (stats == NULL)
		stats = palloc0_object(IndexBulkDeleteResult);

	IvfflatReadMeta(index, &meta);

	if (meta.version == IVFFLAT_VERSION_INCLUDE)
		return ivfflatbulkdelete_v2(info, stats, callback, callback_state, meta.includeCount);

	blkno = IVFFLAT_HEAD_BLKNO;
	bas = GetAccessStrategy(BAS_BULKREAD);

	/* Iterate over list pages */
	while (BlockNumberIsValid(blkno))
	{
		Buffer		cbuf;
		Page		cpage;
		OffsetNumber coffno;
		OffsetNumber cmaxoffno;
		BlockNumber listPages[MaxOffsetNumber];
		ListInfo	listInfo;

		cbuf = ReadBuffer(index, blkno);
		LockBuffer(cbuf, BUFFER_LOCK_SHARE);
		cpage = BufferGetPage(cbuf);

		cmaxoffno = PageGetMaxOffsetNumber(cpage);

		/* Iterate over lists */
		for (coffno = FirstOffsetNumber; coffno <= cmaxoffno; coffno = OffsetNumberNext(coffno))
		{
			IvfflatList list = (IvfflatList) PageGetItem(cpage, PageGetItemId(cpage, coffno));

			listPages[coffno - FirstOffsetNumber] = list->startPage;
		}

		listInfo.blkno = blkno;
		blkno = IvfflatPageGetOpaque(cpage)->nextblkno;

		UnlockReleaseBuffer(cbuf);

		for (coffno = FirstOffsetNumber; coffno <= cmaxoffno; coffno = OffsetNumberNext(coffno))
		{
			BlockNumber searchPage = listPages[coffno - FirstOffsetNumber];
			BlockNumber insertPage = InvalidBlockNumber;

			/* Iterate over entry pages */
			while (BlockNumberIsValid(searchPage))
			{
				Buffer		buf;
				Page		page;
				GenericXLogState *state;
				OffsetNumber offno;
				OffsetNumber maxoffno;
				OffsetNumber deletable[MaxOffsetNumber];
				int			ndeletable;

				vacuum_delay_point();

				buf = ReadBufferExtended(index, MAIN_FORKNUM, searchPage, RBM_NORMAL, bas);

				/*
				 * ambulkdelete cannot delete entries from pages that are
				 * pinned by other backends
				 *
				 * https://www.postgresql.org/docs/current/index-locking.html
				 */
				LockBufferForCleanup(buf);

				state = GenericXLogStart(index);
				page = GenericXLogRegisterBuffer(state, buf, 0);

				maxoffno = PageGetMaxOffsetNumber(page);
				ndeletable = 0;

				/* Find deleted tuples */
				for (offno = FirstOffsetNumber; offno <= maxoffno; offno = OffsetNumberNext(offno))
				{
					IndexTuple	itup = (IndexTuple) PageGetItem(page, PageGetItemId(page, offno));
					ItemPointer htup = &(itup->t_tid);

					if (callback(htup, callback_state))
					{
						deletable[ndeletable++] = offno;
						stats->tuples_removed++;
					}
					else
						stats->num_index_tuples++;
				}

				/* Set to first free page */
				/* Must be set before searchPage is updated */
				if (!BlockNumberIsValid(insertPage) && ndeletable > 0)
					insertPage = searchPage;

				searchPage = IvfflatPageGetOpaque(page)->nextblkno;

				if (ndeletable > 0)
				{
					/* Delete tuples */
					PageIndexMultiDelete(page, deletable, ndeletable);
					GenericXLogFinish(state);
				}
				else
					GenericXLogAbort(state);

				UnlockReleaseBuffer(buf);
			}

			/*
			 * Update after all tuples deleted.
			 *
			 * We don't add or delete items from lists pages, so offset won't
			 * change.
			 */
			if (BlockNumberIsValid(insertPage))
			{
				listInfo.offno = coffno;
				IvfflatUpdateList(index, listInfo, insertPage, InvalidBlockNumber, InvalidBlockNumber, MAIN_FORKNUM);
			}
		}
	}

	FreeAccessStrategy(bas);

	return stats;
}

/*
 * Clean up after a VACUUM operation
 */
IndexBulkDeleteResult *
ivfflatvacuumcleanup(IndexVacuumInfo *info, IndexBulkDeleteResult *stats)
{
	Relation	rel = info->index;

	if (info->analyze_only)
		return stats;

	/* stats is NULL if ambulkdelete not called */
	/* OK to return NULL if index not changed */
	if (stats == NULL)
		return NULL;

	stats->num_pages = RelationGetNumberOfBlocks(rel);

	return stats;
}
