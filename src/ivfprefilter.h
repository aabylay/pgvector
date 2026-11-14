#ifndef IVFPREFILTER_H
#define IVFPREFILTER_H

#include "postgres.h"

#include "access/genam.h"
#include "storage/itemptr.h"
#include "utils/palloc.h"

/*
 * TID set built by the IvfPrefilterScan custom node from a B-tree index and
 * consulted by the ivfflat scan (GetScanItems) to skip candidates that
 * cannot pass the query's filter.
 *
 * See docs/filtered-ivf.md
 */
typedef struct IvfflatPrefilter IvfflatPrefilter;	/* opaque outside
													 * ivfprefilter.c */

IvfflatPrefilter *IvfflatPrefilterCreate(MemoryContext cxt, Size memBudget);
bool		IvfflatPrefilterAdd(IvfflatPrefilter * pf, ItemPointer tid);	/* false = budget hit */
bool		IvfflatPrefilterContains(const IvfflatPrefilter * pf, ItemPointer tid);
void		IvfflatPrefilterFree(IvfflatPrefilter * pf);
int64		IvfflatPrefilterCount(const IvfflatPrefilter * pf);

/* Setter for the ivfflat scan opaque (defined in ivfscan.c) */
void		IvfflatScanSetPrefilter(IndexScanDesc scan, IvfflatPrefilter * pf);

/* Hook installation and custom scan registration (called from _PG_init) */
void		IvfflatPrefilterInit(void);

/* GUCs (defined in ivfprefilter.c, registered in IvfflatInit) */
extern bool ivfflat_prefilter;
extern int	ivfflat_prefilter_mem;

#endif
