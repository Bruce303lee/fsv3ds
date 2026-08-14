/* viz.c - see viz.h */
#include "common.h"
#include "viz.h"
#include "mapv.h"
#include "treev.h"
#include "nav.h"
#include "scanfs.h"

static VizMode mode = VIZ_MAPV;


VizMode
viz_get_mode( void )
{
	return mode;
}


void
viz_set_mode( VizMode new_mode )
{
	if (new_mode == mode)
		return;

	mode = new_mode;

	/* Nothing scanned yet (nav_view_root() is only NULL before the
	 * first scan -- every nav action already guards on this the same
	 * way, see e.g. mapv_cycle_selection()) -- just remember the mode
	 * for when a scan does happen; there's no tree to lay out yet, and
	 * both modes' build_scene() dereference root_dnode unconditionally. */
	if (nav_view_root( ) == NULL)
		return;

	/* The mode being switched *to* needs a fresh scene: its geometry
	 * (NodeDesc.geomparams) was last overwritten by whichever mode ran
	 * most recently -- see mapv.h/treev.h's shared-storage comment --
	 * so it has to be laid out again before anything reads it. */
	if (mode == VIZ_MAPV)
		mapv_build_scene( );
	else
		treev_build_scene( );
}


void
viz_toggle_mode( void )
{
	viz_set_mode( mode == VIZ_MAPV ? VIZ_TREEV : VIZ_MAPV );
}


void
viz_scan_and_build( const char *root )
{
	GNode **node_table;
	unsigned int node_count;

	node_table = scanfs( root, &node_count );
	free( node_table ); /* see mapv_scan_and_build()'s comment -- nothing uses it yet */

	/* MUST happen before either mode's build_scene() -- scanfs() just
	 * freed the whole previous tree. mapv_reset_navigation() is
	 * mode-agnostic despite the name (nav_reset() + color_assign_recursive()
	 * -- see mapv.c). */
	mapv_reset_navigation( );

	if (mode == VIZ_MAPV)
		mapv_build_scene( );
	else
		treev_build_scene( );
}


void
viz_rebuild( void )
{
	if (nav_view_root( ) == NULL)
		return;

	if (mode == VIZ_MAPV)
		mapv_build_scene( );
	else
		treev_build_scene( );
}


void
viz_cycle_selection( int dir )
{
	if (mode == VIZ_MAPV)
		mapv_cycle_selection( dir );
	else
		treev_cycle_selection( dir );
}


void
viz_move_selection_row( int dir )
{
	if (mode == VIZ_MAPV)
		mapv_move_selection_row( dir );
	else
		treev_cycle_selection( dir ); /* see viz.h's comment */
}


gboolean
viz_drill_selected( void )
{
	return (mode == VIZ_MAPV) ? mapv_drill_selected( ) : treev_drill_selected( );
}


gboolean
viz_go_up( void )
{
	return (mode == VIZ_MAPV) ? mapv_go_up( ) : treev_go_up( );
}


void
viz_camera_orbit( float dtheta, float dphi, float distance_factor )
{
	if (mode == VIZ_MAPV)
		mapv_camera_orbit( dtheta, dphi, distance_factor );
	else
		treev_camera_orbit( dtheta, dphi, distance_factor );
}


void
viz_camera_tick( void )
{
	if (mode == VIZ_MAPV)
		mapv_camera_tick( );
	else
		treev_camera_tick( );
}

/* end viz.c */
