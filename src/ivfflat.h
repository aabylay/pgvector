#ifndef IVFFLAT_H
#define IVFFLAT_H

#include "postgres.h"

#include "access/genam.h"
#include "access/generic_xlog.h"
#include "access/parallel.h"
#include "common/pg_prng.h"
#include "lib/pairingheap.h"
#include "nodes/execnodes.h"
#include "port.h"				/* for random() */
#include "storage/condition_variable.h"
#include "utils/sampling.h"
#include "utils/tuplesort.h"
#include "ivfprefilter.h"
#include "vector.h"

#if PG_VERSION_NUM >= 160000
#include "varatt.h"
#endif

#if PG_VERSION_NUM < 190000
#include "storage/shmem.h"		/* for add_size()/mul_size() in some versions */
#endif

#ifdef IVFFLAT_BENCH
#include "portability/instr_time.h"
#endif

#if PG_VERSION_NUM >= 190000
typedef Pointer Item;
#endif

#define IVFFLAT_MAX_DIM 2000

/* Support functions */
#define IVFFLAT_DISTANCE_PROC 1
#define IVFFLAT_NORM_PROC 2
#define IVFFLAT_KMEANS_DISTANCE_PROC 3
#define IVFFLAT_KMEANS_NORM_PROC 4
#define IVFFLAT_TYPE_INFO_PROC 5
#define IVFFLAT_NPROCS 5

#define IVFFLAT_VERSION	1
#define IVFFLAT_VERSION_INCLUDE 2
#define IVFFLAT_MAGIC_NUMBER 0x14FF1A7
#define IVFFLAT_PAGE_ID	0xFF84

/* INCLUDE column chains (version-2 indexes). See docs/filtered-ivf.md */
#define IVFFLAT_MAX_INCLUDE_COLS	(INDEX_MAX_KEYS - 1)
#define IVFFLAT_MAX_INCLUDE_WIDTH	512

/* Preserved page numbers */
#define IVFFLAT_METAPAGE_BLKNO	0
#define IVFFLAT_HEAD_BLKNO		1	/* first list page */

/* IVFFlat parameters */
#define IVFFLAT_DEFAULT_LISTS	100
#define IVFFLAT_MIN_LISTS		1
#define IVFFLAT_MAX_LISTS		32768
#define IVFFLAT_DEFAULT_PROBES	1

/* Build phases */
/* PROGRESS_CREATEIDX_SUBPHASE_INITIALIZE is 1 */
#define PROGRESS_IVFFLAT_PHASE_KMEANS	2
#define PROGRESS_IVFFLAT_PHASE_ASSIGN	3
#define PROGRESS_IVFFLAT_PHASE_LOAD		4

#define IVFFLAT_LIST_SIZE(size)	add_size(offsetof(IvfflatListData, center), size)

#define IvfflatPageGetOpaque(page)	((IvfflatPageOpaque) PageGetSpecialPointer(page))
#define IvfflatPageGetOpaqueV2(page) ((IvfflatPageOpaqueDataV2 *) PageGetSpecialPointer(page))
#define IvfflatPageGetMeta(page)	((IvfflatMetaPageData *) PageGetContents(page))

#ifdef IVFFLAT_BENCH
#define IvfflatBench(name, code) \
	do { \
		instr_time	start; \
		instr_time	duration; \
		INSTR_TIME_SET_CURRENT(start); \
		(code); \
		INSTR_TIME_SET_CURRENT(duration); \
		INSTR_TIME_SUBTRACT(duration, start); \
		elog(INFO, "%s: %.3f ms", name, INSTR_TIME_GET_MILLISEC(duration)); \
	} while (0)
#else
#define IvfflatBench(name, code) (code)
#endif

#define RandomDouble() pg_prng_double(&pg_global_prng_state)
#define RandomInt() pg_prng_uint32(&pg_global_prng_state)
#define SeedRandom(seed) pg_prng_seed(&pg_global_prng_state, seed)

#if PG_VERSION_NUM >= 190000
#define palloc_array_checked(type, count) palloc_array(type, count)
#define palloc0_array_checked(type, count) palloc0_array(type, count)
#else
#define palloc_array_checked(type, count) ((type *) palloc(mul_size(sizeof(type), count)))
#define palloc0_array_checked(type, count) ((type *) palloc0(mul_size(sizeof(type), count)))
#endif

/* Variables */
extern int	ivfflat_probes;
extern int	ivfflat_iterative_scan;
extern int	ivfflat_max_probes;

typedef enum IvfflatIterativeScanMode
{
	IVFFLAT_ITERATIVE_SCAN_OFF,
	IVFFLAT_ITERATIVE_SCAN_RELAXED
}			IvfflatIterativeScanMode;

typedef struct VectorArrayData
{
	int			length;
	int			maxlen;
	int			dim;
	Size		itemsize;
	char	   *items;
}			VectorArrayData;

typedef VectorArrayData * VectorArray;

typedef struct ListInfo
{
	BlockNumber blkno;
	OffsetNumber offno;
}			ListInfo;

/* IVFFlat index options */
typedef struct IvfflatOptions
{
	int32		vl_len_;		/* varlena header (do not touch directly!) */
	int			lists;			/* number of lists */
}			IvfflatOptions;

typedef struct IvfflatSpool
{
	Tuplesortstate *sortstate;
	Relation	heap;
	Relation	index;
}			IvfflatSpool;

typedef struct IvfflatShared
{
	/* Immutable state */
	Oid			heaprelid;
	Oid			indexrelid;
	bool		isconcurrent;
	int			scantuplesortstates;

	/* Worker progress */
	ConditionVariable workersdonecv;

	/* Mutex for mutable state */
	slock_t		mutex;

	/* Mutable state */
	int			nparticipantsdone;
	double		reltuples;
	double		indtuples;

#ifdef IVFFLAT_KMEANS_DEBUG
	double		inertia;
#endif
}			IvfflatShared;

#define ParallelTableScanFromIvfflatShared(shared) \
	(ParallelTableScanDesc) ((char *) (shared) + BUFFERALIGN(sizeof(IvfflatShared)))

typedef struct IvfflatLeader
{
	ParallelContext *pcxt;
	int			nparticipanttuplesorts;
	IvfflatShared *ivfshared;
	Sharedsort *sharedsort;
	Snapshot	snapshot;
	char	   *ivfcenters;
}			IvfflatLeader;

typedef struct IvfflatTypeInfo
{
	int			maxDimensions;
	Datum		(*dimensions) (PG_FUNCTION_ARGS);
	Datum		(*normalize) (PG_FUNCTION_ARGS);
	Size		(*itemSize) (int dimensions);
	void		(*updateCenter) (Pointer v, int dimensions, float *x);
	void		(*sumCenter) (Pointer v, int dimensions, float *x);
}			IvfflatTypeInfo;

typedef struct IvfflatBuildState
{
	/* Info */
	Relation	heap;
	Relation	index;
	IndexInfo  *indexInfo;
	const		IvfflatTypeInfo *typeInfo;
	TupleDesc	tupdesc;

	/* Settings */
	int			dimensions;
	int			lists;
	int			includeCount;	/* 0 keeps a version-1 index */
	int			version;
	uint16		includeWidth[IVFFLAT_MAX_INCLUDE_COLS];
	TupleDesc	vectorDesc;		/* one-attribute descriptor for the vector */
	TupleDesc  *includeDescs;	/* one-attribute descriptors, version 2 */

	/* Statistics */
	double		indtuples;
	double		reltuples;

	/* Support functions */
	FmgrInfo   *procinfo;
	FmgrInfo   *normprocinfo;
	FmgrInfo   *kmeansnormprocinfo;
	Oid			collation;

	/* Variables */
	VectorArray samples;
	VectorArray centers;
	ListInfo   *listInfo;
	Size		itemsize;

#ifdef IVFFLAT_KMEANS_DEBUG
	double		inertia;
	double	   *listSums;
	int		   *listCounts;
#endif

	/* Sampling */
	BlockSamplerData bs;
	ReservoirStateData rstate;
	double		samplerows;
	double		rowstoskip;

	/* Sorting */
	Tuplesortstate *sortstate;
	TupleDesc	sortdesc;
	TupleTableSlot *slot;

	/* Memory */
	Size		memoryUsed;
	MemoryContext tmpCtx;

	/* Parallel builds */
	IvfflatLeader *ivfleader;
}			IvfflatBuildState;

typedef struct IvfflatMetaPageData
{
	uint32		magicNumber;
	uint32		version;
	uint16		dimensions;
	uint16		lists;
}			IvfflatMetaPageData;

typedef IvfflatMetaPageData * IvfflatMetaPage;

typedef struct IvfflatPageOpaqueData
{
	BlockNumber nextblkno;
	uint16		unused;
	uint16		page_id;		/* for identification of IVFFlat indexes */
}			IvfflatPageOpaqueData;

typedef IvfflatPageOpaqueData * IvfflatPageOpaque;

typedef struct IvfflatListData
{
	BlockNumber startPage;
	BlockNumber insertPage;
	Vector		center;
}			IvfflatListData;

typedef IvfflatListData * IvfflatList;

typedef struct IvfflatMetaPageDataV2
{
	uint32		magicNumber;
	uint32		version;		/* IVFFLAT_VERSION_INCLUDE */
	uint16		dimensions;
	uint16		lists;
	uint16		includeCount;	/* 1 .. IVFFLAT_MAX_INCLUDE_COLS */
	uint16		includeWidth[IVFFLAT_MAX_INCLUDE_COLS]; /* acceptance bound,
														 * bytes. Slots at and
														 * above includeCount
														 * are zero. */
}			IvfflatMetaPageDataV2;

typedef struct IvfflatIncludeChain
{
	BlockNumber startPage;
	BlockNumber insertPage;
}			IvfflatIncludeChain;

typedef struct IvfflatListDataV2
{
	BlockNumber startPage;		/* vector entry chain */
	BlockNumber insertPage;
	BlockNumber dirStartPage;	/* vector-page directory */
	uint32		entry_hwm;		/* next entry_id to allocate; never decreases */
	uint32		live_count;		/* live vector entries in this list */
	/* IvfflatIncludeChain includeChains[includeCount] follows.
	 * The center Vector follows those, at a MAXALIGN offset. */
}			IvfflatListDataV2;

typedef struct IvfflatPageOpaqueDataV2
{
	BlockNumber nextblkno;
	uint32		min_entry_id;
	uint32		max_entry_id;
	uint16		live_items;
	uint16		page_id;		/* IVFFLAT_PAGE_ID */
}			IvfflatPageOpaqueDataV2;

typedef struct IvfflatEntryHeader
{
	uint32		entry_id;
	/* IndexTuple bytes follow immediately. There is no tuple member. */
}			IvfflatEntryHeader;

typedef struct IvfflatVectorDirEntry
{
	BlockNumber blkno;
	uint32		min_entry_id;
	uint32		max_entry_id;
	uint16		live_items;
	uint16		unused;
}			IvfflatVectorDirEntry;

typedef struct IvfflatMetaInfo
{
	uint32		version;
	int			lists;
	int			dimensions;
	int			includeCount;
	uint16		includeWidth[IVFFLAT_MAX_INCLUDE_COLS];
}			IvfflatMetaInfo;

/*
 * Installed by IvfIncludeScan. The access method does not free the
 * ExprStates. ivfflatrescan does not touch this, and does not touch
 * so->prefilter either.
 */
typedef struct IvfflatIncludeColumn
{
	AttrNumber	indexAttno;		/* 2 .. includeCount + 1 */
	int			chainIndex;		/* slot in includeChains[] */
	TupleDesc	tupdesc;		/* one attribute */
	TupleTableSlot *slot;
}			IvfflatIncludeColumn;

typedef enum IvfflatIncludeNodeKind
{
	IVF_INCLUDE_LEAF = 1,
	IVF_INCLUDE_AND = 2,
	IVF_INCLUDE_OR = 3
}			IvfflatIncludeNodeKind;

typedef struct IvfflatIncludeNode
{
	IvfflatIncludeNodeKind kind;
	int			chainIndex;		/* IVF_INCLUDE_LEAF: slot in includeChains[] */
	ExprState  *qual;			/* IVF_INCLUDE_LEAF: compiled predicate */
	int			nchildren;		/* IVF_INCLUDE_AND / IVF_INCLUDE_OR */
	struct IvfflatIncludeNode *children;
}			IvfflatIncludeNode;

typedef struct IvfflatIncludeFilter
{
	int			ncolumns;		/* includeCount */
	IvfflatIncludeNode *root;	/* NULL if no filter */
	IvfflatIncludeColumn columns[IVFFLAT_MAX_INCLUDE_COLS];
	ExprContext *econtext;
	const char *fallbackReason; /* NULL, "memory", or "disabled" */
}			IvfflatIncludeFilter;

/* Bytes before the center. includeCount comes from the metapage. */
static inline Size
IvfflatListHeaderBytes(int includeCount)
{
	return MAXALIGN(sizeof(IvfflatListDataV2) + includeCount * sizeof(IvfflatIncludeChain));
}

static inline IvfflatIncludeChain *
IvfflatListGetChains(IvfflatListDataV2 *list)
{
	return (IvfflatIncludeChain *) ((char *) list + sizeof(IvfflatListDataV2));
}

static inline Vector *
IvfflatListGetCenter(IvfflatListDataV2 *list, int includeCount)
{
	return (Vector *) ((char *) list + IvfflatListHeaderBytes(includeCount));
}

static inline uint32
IvfflatEntryGetId(Item item)
{
	return ((IvfflatEntryHeader *) item)->entry_id;
}

static inline IndexTuple
IvfflatEntryGetTuple(Item item)
{
	return (IndexTuple) ((char *) item + sizeof(uint32));
}

/* Stored item length: header plus the index tuple, maxaligned. */
static inline Size
IvfflatEntrySize(IndexTuple itup)
{
	return MAXALIGN(sizeof(uint32) + IndexTupleSize(itup));
}

typedef struct IvfflatScanList
{
	pairingheap_node ph_node;
	BlockNumber startPage;
	BlockNumber listBlkno;
	OffsetNumber listOffno;
	double		distance;
}			IvfflatScanList;

typedef struct IvfflatProbeList
{
	BlockNumber startPage;
	BlockNumber listBlkno;
	OffsetNumber listOffno;
}			IvfflatProbeList;

typedef struct IvfflatScanOpaqueData
{
	const		IvfflatTypeInfo *typeInfo;
	int			probes;
	int			maxProbes;
	int			dimensions;
	uint32		version;
	int			includeCount;
	bool		first;
	Datum		value;
	MemoryContext tmpCtx;

	/* Sorting */
	Tuplesortstate *sortstate;
	TupleDesc	tupdesc;
	TupleTableSlot *vslot;
	TupleTableSlot *mslot;
	BufferAccessStrategy bas;

	/* Support functions */
	FmgrInfo   *procinfo;
	FmgrInfo   *normprocinfo;
	Oid			collation;
	Datum		(*distfunc) (FmgrInfo *flinfo, Oid collation, Datum arg1, Datum arg2);

	/* Lists */
	pairingheap *listQueue;
	IvfflatProbeList *listPages;
	int			listIndex;
	IvfflatScanList *lists;

	/* Pre-filter installed by the IvfPrefilterScan custom node (may be NULL) */
	IvfflatPrefilter *prefilter;

	/* Include-column filter installed by IvfIncludeScan (may be NULL) */
	IvfflatIncludeFilter *includeFilter;
}			IvfflatScanOpaqueData;

typedef IvfflatScanOpaqueData * IvfflatScanOpaque;

#define VECTOR_ARRAY_SIZE(_length, _size) add_size(sizeof(VectorArrayData), mul_size(_length, MAXALIGN(_size)))

/* Use functions instead of macros to avoid double evaluation */

static inline Pointer
VectorArrayGet(VectorArray arr, int offset)
{
	/* Safety check */
	if (offset < 0 || offset >= arr->maxlen)
		elog(ERROR, "index out of bounds");

	return ((char *) arr->items) + (offset * arr->itemsize);
}

static inline void
VectorArraySet(VectorArray arr, int offset, Pointer val)
{
	Size		size = VARSIZE_ANY(val);

	/* Safety check */
	if (size > arr->itemsize)
		elog(ERROR, "item too large");

	memcpy(VectorArrayGet(arr, offset), val, size);
}

/* Methods */
VectorArray VectorArrayInit(int maxlen, int dimensions, Size itemsize);
void		VectorArrayFree(VectorArray arr);
void		IvfflatKmeans(Relation index, VectorArray samples, VectorArray centers, const IvfflatTypeInfo * typeInfo, Size memoryUsed);
FmgrInfo   *IvfflatOptionalProcInfo(Relation index, uint16 procnum);
Datum		IvfflatNormValue(const IvfflatTypeInfo * typeInfo, Oid collation, Datum value);
bool		IvfflatCheckNorm(FmgrInfo *procinfo, Oid collation, Datum value);
void		IvfflatNormVectors(const IvfflatTypeInfo * typeInfo, Oid collation, VectorArray arr, MemoryContext tmpCtx);
void		IvfflatCheckMemoryUsage(Size totalSize);
int			IvfflatGetLists(Relation index);
void		IvfflatGetMetaPageInfo(Relation index, int *lists, int *dimensions);
void		IvfflatReadMeta(Relation index, IvfflatMetaInfo *meta);
void		IvfflatUpdateList(Relation index, ListInfo listInfo, BlockNumber insertPage, BlockNumber originalInsertPage, BlockNumber startPage, ForkNumber forkNum);
void		IvfflatWriteListV2(Relation index, ListInfo listInfo, int includeCount, BlockNumber startPage, BlockNumber insertPage, BlockNumber dirStartPage, uint32 entry_hwm, uint32 live_count, IvfflatIncludeChain *chains, ForkNumber forkNum);
void		IvfflatCommitBuffer(Buffer buf, GenericXLogState *state);
void		IvfflatAppendPage(Relation index, Buffer *buf, Page *page, GenericXLogState **state, ForkNumber forkNum, int version);
Buffer		IvfflatNewBuffer(Relation index, ForkNumber forkNum);
void		IvfflatInitPage(Buffer buf, Page page, int version);
void		IvfflatInitRegisterPage(Relation index, Buffer *buf, Page *page, GenericXLogState **state, int version);
void		IvfflatPageOpaqueAdd(Page page, uint32 entry_id);
void		IvfflatPageOpaqueRecompute(Page page);
Size		IvfflatMaxItemSize(int version);
Datum		IvfflatPrepareIncludeDatum(Form_pg_attribute att, Datum value, bool isnull);
TupleDesc	IvfflatOneColumnDesc(Relation index, AttrNumber attnum);
IndexTuple	IvfflatFormColumnTuple(TupleDesc desc, Datum value, bool isnull, ItemPointer tid);
char	   *IvfflatFormEntryItem(uint32 entry_id, IndexTuple itup, Size *itemsz);
Size		IvfflatIncludeMemBudget(void);
void		IvfflatInit(void);
void		IvfflatCheckDim(int expected, const IvfflatTypeInfo * typeInfo, Oid collation, Datum value);
const		IvfflatTypeInfo *IvfflatGetTypeInfo(Relation index);
PGDLLEXPORT void IvfflatParallelBuildMain(dsm_segment *seg, shm_toc *toc);

/* Index access methods */
IndexBuildResult *ivfflatbuild(Relation heap, Relation index, IndexInfo *indexInfo);
void		ivfflatbuildempty(Relation index);
bool		ivfflatinsert(Relation index, Datum *values, bool *isnull, ItemPointer heap_tid, Relation heap, IndexUniqueCheck checkUnique, bool indexUnchanged, IndexInfo *indexInfo);
IndexBulkDeleteResult *ivfflatbulkdelete(IndexVacuumInfo *info, IndexBulkDeleteResult *stats, IndexBulkDeleteCallback callback, void *callback_state);
IndexBulkDeleteResult *ivfflatvacuumcleanup(IndexVacuumInfo *info, IndexBulkDeleteResult *stats);
IndexScanDesc ivfflatbeginscan(Relation index, int nkeys, int norderbys);
void		ivfflatrescan(IndexScanDesc scan, ScanKey keys, int nkeys, ScanKey orderbys, int norderbys);
bool		ivfflatgettuple(IndexScanDesc scan, ScanDirection dir);
void		ivfflatendscan(IndexScanDesc scan);
void		IvfflatScanSetIncludeFilter(IndexScanDesc scan, IvfflatIncludeFilter *filter);

#endif
