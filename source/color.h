/* color.h - fsv3ds Phase 4 / Phase 6
 *
 * Trimmed port of upstream color.c: file coloring by extension
 * (upstream's COLOR_BY_WPATTERN mode), with a fixed per-type color for
 * directories and other non-regular-file types (upstream's
 * "directory override", generalized). Dropped entirely: COLOR_BY_TIMESTAMP
 * (spectrum-by-mtime) and COLOR_BY_NODETYPE as a separate selectable
 * mode, config file load/save (nvstore -- no config file exists in this
 * port), and runtime mode switching (no menu system exists to switch
 * it from). See color.c for the extension->color table.
 *
 * Phase 6 adds ColorScheme: every category-of-file's color is looked
 * up through a swappable per-scheme table instead of one fixed pair,
 * so the Settings screen (ui.c) can offer a small preset picker
 * without touching the pattern-matching logic itself.
 */
#ifndef FSV3DS_COLOR_H
#define FSV3DS_COLOR_H

#include "compat/gnode.h"

typedef enum {
	COLOR_SCHEME_DEFAULT,
	COLOR_SCHEME_HIGH_CONTRAST,
	COLOR_SCHEME_MONOCHROME,
	NUM_COLOR_SCHEMES
} ColorScheme;

/* Cheap/no-op today (the palette is compile-time constant), but keeps
 * a clear one-time init call site in case that changes later. */
void color_init( void );

/* Assigns NODE_DESC(node)->color for every node under dnode
 * (recursively, the whole tree -- not gated by view/expand state, same
 * as upstream). Call once after a scan completes, and again any time
 * color_set_scheme() changes the active scheme (existing NodeDesc
 * pointers need to be re-resolved against the new table). */
void color_assign_recursive( GNode *dnode );

int color_get_scheme( void );

/* Out-of-range values are ignored. Only updates which table
 * match_color()/nodetype lookups read from -- caller (settings.c) is
 * responsible for re-running color_assign_recursive() and rebuilding
 * whichever viz mode is active afterward so the change is visible. */
void color_set_scheme( int scheme );

const char *color_scheme_name( int scheme );

#endif /* FSV3DS_COLOR_H */
