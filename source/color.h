/* color.h - fsv3ds Phase 4
 *
 * Trimmed port of upstream color.c: file coloring by extension
 * (upstream's COLOR_BY_WPATTERN mode), with a fixed per-type color for
 * directories and other non-regular-file types (upstream's
 * "directory override", generalized). Dropped entirely: COLOR_BY_TIMESTAMP
 * (spectrum-by-mtime) and COLOR_BY_NODETYPE as a separate selectable
 * mode, config file load/save (nvstore -- no config file exists in this
 * port), and runtime mode switching (no menu system exists to switch
 * it from). See color.c for the extension->color table.
 */
#ifndef FSV3DS_COLOR_H
#define FSV3DS_COLOR_H

#include "compat/gnode.h"

/* Cheap/no-op today (the palette is compile-time constant), but keeps
 * a clear one-time init call site in case that changes later. */
void color_init( void );

/* Assigns NODE_DESC(node)->color for every node under dnode
 * (recursively, the whole tree -- not gated by view/expand state, same
 * as upstream). Call once after a scan completes. */
void color_assign_recursive( GNode *dnode );

#endif /* FSV3DS_COLOR_H */
