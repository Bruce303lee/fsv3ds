/* main.c - fsv3ds Phase 6 entry point
 *
 * Bottom screen: touch-driven UI (status bar, button rail, content
 * screen, footer breadcrumb) -- see ui.c. Top screen: citro3d render
 * of the scanned tree, in whichever of MapV/TreeV is currently active
 * (see viz.h).
 *
 * Controls:
 *   Circle Pad   orbit camera (theta/phi)
 *   L / R (held) zoom out / in
 *   D-pad L/R    cycle selection within the current view
 *   D-pad U/D    jump selection to the nearest block in the row above/below (MapV only -- see viz.h)
 *   A            drill into the selected directory
 *   B            go back up to the parent
 *   X            toggle MapV / TreeV
 *   Y            screenshot -> sdmc:/fsv3ds_screenshot.ppm
 *   SELECT       rescan the default folder from the top (Settings shows/
 *                sets it via the Folder screen's "Use this")
 *   START        exit
 *   Touch        bottom screen: Folder/Settings/Info/Log rail, settings toggles
 */
#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"
#include "render.h"
#include "rpc.h"
#include "color.h"
#include "settings.h"
#include "ui.h"
#include "viz.h"
#include "nav.h"

/* Logs where navigation currently stands -- the view root's absolute
 * path, and which child (if any) is selected. The bottom-screen
 * footer (ui.c's draw_footer()) reads nav_view_root()/
 * nav_selected_node() live every frame instead of being told to
 * refresh, so this is just for the RPC/on-screen Log scrollback now. */
static void
print_status( void )
{
	GNode *vr = nav_view_root( );
	GNode *sel = nav_selected_node( );

	if (vr == NULL)
		return;

	rpc_logf( "at: %s\n", node_absname( vr ) );
	if (sel != NULL) {
		rpc_logf( "  > %s%s  (%s)\n",
			NODE_DESC(sel)->name,
			NODE_IS_DIR(sel) ? "/" : "",
			abbrev_size( NODE_DESC(sel)->size ) );
	}
	else
		rpc_logf( "  (empty)\n" );
}


int
main( int argc, char **argv )
{
	(void)argc;
	(void)argv;

	gfxInitDefault( );

	render_init( );
	color_init( );
	rpc_init( ); /* dev-only remote control -- see rpc.h */
	ui_init( );
	settings_init( ); /* after color_init(): applies any saved color scheme */

	rpc_logf( "fsv3ds -- Phase 6\n" );

	while (aptMainLoop( )) {
		u32 kDown, kHeld;
		circlePosition circle;
		float dtheta, dphi, zoom;
		int touch_x = 0, touch_y = 0;
		gboolean touched = FALSE;

		hidScanInput( );
		kDown = hidKeysDown( ) | rpc_take_injected_keys( );
		kHeld = hidKeysHeld( );
		hidCircleRead( &circle );

		if (kDown & KEY_START)
			break;

		if (kDown & KEY_TOUCH) {
			touchPosition touch;

			hidTouchRead( &touch );
			touch_x = touch.px;
			touch_y = touch.py;
			touched = TRUE;
		}
		if (rpc_take_injected_touch( &touch_x, &touch_y ))
			touched = TRUE;
		if (touched)
			ui_handle_touch( touch_x, touch_y );

		if (kDown & KEY_SELECT) {
			/* Goes through ui_scan_with_feedback() (a "Scanning..."
			 * overlay frame, then viz.c's scanfs() + shared nav reset +
			 * rebuild whichever mode is active) rather than calling
			 * scanfs()/viz_scan_and_build() directly -- see
			 * mapv_scan_and_build()'s old comment for the class of bug
			 * that separating scan from nav-reset caused once, and
			 * ui_scan_with_feedback()'s comment for why the overlay
			 * frame matters now that the folder browser can point a
			 * scan at anything on the card. */
			ui_scan_with_feedback( settings_get_default_root( ) );
			print_status( );
		}

		if (kDown & KEY_A) {
			if (viz_drill_selected( ))
				print_status( );
		}

		if (kDown & KEY_B) {
			if (viz_go_up( ))
				print_status( );
		}

		if (kDown & KEY_X) {
			viz_toggle_mode( );
			rpc_logf( "mode: %s\n", viz_get_mode( ) == VIZ_MAPV ? "MapV" : "TreeV" );
		}

		/* Swapped from the "natural" RIGHT=+1/LEFT=-1 mapping: sibling
		 * order in the GNode list doesn't correspond to left-to-right
		 * screen position the way you'd expect, so the obvious mapping
		 * felt backwards on hardware. This matches what it should feel
		 * like, not what the underlying list order says. */
		if (kDown & KEY_DRIGHT)
			viz_cycle_selection( -1 );
		if (kDown & KEY_DLEFT)
			viz_cycle_selection( 1 );
		/* Untested direction sense (same caveat as the L/R swap above)
		 * -- if up/down jump to the wrong row, swap these two signs.
		 * MapV only -- see viz_move_selection_row()'s comment. */
		if (kDown & KEY_DUP)
			viz_move_selection_row( 1 );
		if (kDown & KEY_DDOWN)
			viz_move_selection_row( -1 );

		if (kDown & KEY_Y) {
			render_screenshot( "sdmc:/fsv3ds_screenshot.ppm" );
			rpc_logf( "screenshot -> sdmc:/fsv3ds_screenshot.ppm\n" );
		}

		/* Circle pad orbit, with a deadzone so an imperfectly-centered
		 * stick doesn't cause slow drift. Range is roughly +-156. */
		dtheta = 0.0f;
		dphi = 0.0f;
		if (circle.dx > 15 || circle.dx < -15)
			dtheta = -(float)circle.dx / 156.0f * 2.0f;
		if (circle.dy > 15 || circle.dy < -15)
			dphi = (float)circle.dy / 156.0f * 2.0f;

		zoom = 1.0f;
		if (kHeld & KEY_L)
			zoom = 1.02f;
		else if (kHeld & KEY_R)
			zoom = 0.98f;

		if (dtheta != 0.0f || dphi != 0.0f || zoom != 1.0f)
			viz_camera_orbit( dtheta, dphi, zoom );

		/* Unconditional, every frame: eases the camera toward whatever
		 * was last selected -- see mapv_camera_tick()'s comment. */
		viz_camera_tick( );

		/* C3D_FrameBegin(C3D_FRAME_SYNCDRAW) inside render_frame() paces
		 * this loop to vblank; no separate gfxFlushBuffers/SwapBuffers/
		 * gspWaitForVBlank needed once citro3d is driving the frame
		 * (confirmed against devkitPro's gpusprites example, which
		 * live-updates a GFX_BOTTOM console every frame the same way). */
		render_frame( );

		rpc_poll( );
	}

	ui_fini( );
	rpc_fini( );
	render_fini( );
	gfxExit( );

	return 0;
}

/* end main.c */
