#ifndef IVFINCLUDE_H
#define IVFINCLUDE_H

/*
 * Included-column chains for ivfflat. See docs/filtered-ivf.md.
 * The prefilter custom scan is a separate node and is not modified here.
 */

/* GUCs (defined in ivfinclude.c, registered in IvfflatInit) */
extern bool ivfflat_include_filter;
extern int	ivfflat_include_mem;

/* Hook installation and custom scan registration (called from _PG_init) */
void		IvfflatIncludeInit(void);

#endif
