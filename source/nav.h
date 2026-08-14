/* nav.h - fsv3ds Phase 5
 *
 * Navigation identity shared between visualization modes: which
 * directory's children are currently being drawn (view root) and
 * which of its children is selected. Extracted out of mapv.c (which
 * used to own this alone, back when MapV was the only mode) so TreeV
 * can share the exact same state instead of keeping its own copy that
 * would drift out of sync the moment you drill/up/cycle in one mode
 * and then switch to the other -- dirtree_stub.c's
 * dirtree_entry_expanded() keys off nav_view_root() for the same
 * reason (both modes need to agree on what counts as "expanded").
 *
 * This only owns identity (which GNode*s), not geometry or camera
 * framing -- mapv.c/treev.c each still do their own layout/vertex/
 * camera work in response to these calls via their own
 * mapv_drill_selected()/treev_drill_selected() etc. wrappers, which
 * viz.c dispatches between based on the active mode.
 */
#ifndef FSV3DS_NAV_H
#define FSV3DS_NAV_H

#include "compat/gnode.h"

/* view root -> root_dnode, selection -> its first child. Call once
 * after a scan completes, before either mode's build_scene() runs. */
void nav_reset( void );

GNode *nav_view_root( void );
GNode *nav_selected_node( void );

/* Moves the selection to the next/previous sibling of the view root
 * (dir > 0 = next, dir < 0 = previous), wrapping around. No-op if the
 * view root has 0 or 1 children. */
void nav_cycle_selection( int dir );

/* Used by mode-specific "jump to nearest row" moves (MapV's
 * mapv_move_selection_row(), and any TreeV equivalent) that pick the
 * new selection using their own geometry -- nav.c doesn't know about
 * either mode's layout, so it can't compute the jump itself. */
void nav_set_selected( GNode *node );

/* If the selected node is a directory with children, makes it the new
 * view root (selecting its first child) and returns TRUE. Otherwise
 * returns FALSE and changes nothing. */
gboolean nav_drill_selected( void );

/* If the view root isn't already the scan root, moves back up to its
 * parent (selecting whichever child we just came from) and returns
 * TRUE. Otherwise returns FALSE. */
gboolean nav_go_up( void );

#endif /* FSV3DS_NAV_H */
