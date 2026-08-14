/* viz.h - fsv3ds Phase 5
 *
 * Visualization mode dispatcher: which of MapV/TreeV is currently on
 * screen, and routing for the nav actions/rescan that both modes need
 * to respond to identically (rebuild whichever mode is active, using
 * the shared identity in nav.h). render.c and rpc.c are the only other
 * places that need to know the mode directly (to pick which of
 * mapv_/treev_'s vertex/camera/label accessors to draw); main.c and
 * rpc.c's command handlers go through here instead of calling
 * mapv_/treev_ functions directly, so a button press or RPC command
 * doesn't have to know or care which mode is active.
 */
#ifndef FSV3DS_VIZ_H
#define FSV3DS_VIZ_H

#include "compat/gtypes.h"

typedef enum {
	VIZ_MAPV,
	VIZ_TREEV
} VizMode;

VizMode viz_get_mode( void );

/* Switches the active mode and rebuilds its scene/camera for the
 * current nav_view_root() (geometry for the mode you're switching
 * *away* from is left as-is -- stale but unused until you switch back
 * to it, at which point this rebuilds it fresh again; see viz.c). */
void viz_set_mode( VizMode mode );
void viz_toggle_mode( void );

/* scanfs() + shared nav reset (view root/selection + color_assign) +
 * rebuild whichever mode is currently active. Replaces direct calls to
 * mapv_scan_and_build(). */
void viz_scan_and_build( const char *root );

/* Dispatches to the active mode's mapv_/treev_ equivalents. */
void viz_cycle_selection( int dir );

/* MapV-only move (jumps to the nearest sibling in an adjacent treemap
 * row -- see mapv_move_selection_row()'s comment). TreeV has no
 * equivalent "row" concept ported yet, so this falls back to a plain
 * cycle when TreeV is active -- D-pad up/down still does *something*
 * useful rather than nothing. */
void viz_move_selection_row( int dir );

gboolean viz_drill_selected( void );
gboolean viz_go_up( void );
void viz_camera_orbit( float dtheta, float dphi, float distance_factor );
void viz_camera_tick( void );

#endif /* FSV3DS_VIZ_H */
