/* main.c - fsv3ds Phase 3 entry point
 *
 * Bottom screen: text console (scan progress/tree dump/status).
 * Top screen: citro3d MapV render of the scanned tree.
 *
 * Controls:
 *   Circle Pad   orbit camera (theta/phi)
 *   L / R (held) zoom out / in
 *   D-pad L/R    cycle selection within the current treemap row
 *   D-pad U/D    jump selection to the nearest block in the row above/below
 *   A            drill into the selected directory
 *   B            go back up to the parent
 *   Y            screenshot -> sdmc:/fsv3ds_screenshot.ppm
 *   SELECT       rescan sdmc:/3ds from the top
 *   START        exit
 */
#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"
#include "scanfs.h"
#include "mapv.h"
#include "render.h"
#include "rpc.h"
#include "color.h"

#define SCAN_ROOT "sdmc:/3ds"

/* Bottom screen is split into two independent consoles sharing the same
 * physical framebuffer (see consoleSetWindow()'s doc comment -- this is
 * the standard libctru pattern for a fixed status area plus a
 * separately-scrolling log below it): breadcrumbConsole pins to the top
 * 2 rows and is rewritten in place on every navigation change; logConsole
 * owns the rest and behaves like a normal scrolling terminal. */
static PrintConsole breadcrumbConsole;
static PrintConsole logConsole;

static void
print_tree( GNode *node, int depth, int max_depth )
{
	GNode *child;

	if (depth > max_depth)
		return;

	if (!NODE_IS_METANODE(node)) {
		const char *name = NODE_DESC(node)->name;
		int i;

		if (name[0] == '\0')
			name = "/";

		for (i = 0; i < depth - 1; i++)
			printf( "  " );

		if (NODE_IS_DIR(node))
			printf( "[DIR] %s\n", name );
		else
			printf( "%s  (%s)\n", name, abbrev_size( NODE_DESC(node)->size ) );
	}

	for (child = node->children; child != NULL; child = child->next)
		print_tree( child, depth + 1, max_depth );
}


static void print_breadcrumb( void );

/* Prints where navigation currently stands: the view root's absolute
 * path, and which child (if any) is selected -- to the scrolling log,
 * and (via print_breadcrumb()) to the fixed breadcrumb line. */
static void
print_status( void )
{
	GNode *vr = mapv_view_root( );
	GNode *sel = mapv_selected_node( );

	print_breadcrumb( );

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


/* Redraws the fixed breadcrumb line at the top of the bottom screen:
 * the view root's path with "/" swapped for " > ", plus the currently
 * selected child highlighted in yellow. Doesn't touch logConsole's
 * scroll position -- switches consoles, clears just its own 2-row
 * window, prints, and switches back. */
static void
print_breadcrumb( void )
{
	GNode *vr = mapv_view_root( );
	GNode *sel = mapv_selected_node( );
	char path_buf[256];
	char *segment;
	int first = 1;

	consoleSelect( &breadcrumbConsole );
	consoleClear( );

	if (vr == NULL) {
		printf( "(no scan yet -- press SELECT)" );
		consoleSelect( &logConsole );
		return;
	}

	strncpy( path_buf, node_absname( vr ), sizeof(path_buf) - 1 );
	path_buf[sizeof(path_buf) - 1] = '\0';

	segment = strtok( path_buf, "/" );
	while (segment != NULL) {
		if (!first)
			printf( " > " );
		printf( "%s", segment );
		first = 0;
		segment = strtok( NULL, "/" );
	}
	if (first)
		printf( "/" );

	if (sel != NULL) {
		printf( CONSOLE_YELLOW " > %s%s" CONSOLE_RESET,
			NODE_DESC(sel)->name, NODE_IS_DIR(sel) ? "/" : "" );
	}

	consoleSelect( &logConsole );
}


int
main( int argc, char **argv )
{
	(void)argc;
	(void)argv;

	gfxInitDefault( );

	consoleInit( GFX_BOTTOM, &logConsole );
	consoleInit( GFX_BOTTOM, &breadcrumbConsole );
	consoleSetWindow( &breadcrumbConsole, 0, 0, 40, 2 );
	consoleSetWindow( &logConsole, 0, 2, 40, 28 );
	consoleSelect( &logConsole );

	render_init( );
	color_init( );
	rpc_init( ); /* dev-only remote control -- see rpc.h */

	printf( "fsv3ds -- Phase 3 (navigation)\n" );
	printf( "SELECT scan, A drill in, B up, Y shot, START exit\n\n" );
	print_breadcrumb( );

	while (aptMainLoop( )) {
		u32 kDown, kHeld;
		circlePosition circle;
		float dtheta, dphi, zoom;

		hidScanInput( );
		kDown = hidKeysDown( ) | rpc_take_injected_keys( );
		kHeld = hidKeysHeld( );
		hidCircleRead( &circle );

		if (kDown & KEY_START)
			break;

		if (kDown & KEY_SELECT) {
			GNode **node_table;
			unsigned int node_count;

			consoleClear( );
			printf( "fsv3ds -- scanning " SCAN_ROOT " ...\n\n" );

			node_table = scanfs( SCAN_ROOT, &node_count );
			free( node_table ); /* nothing uses it until touch/click picking exists */

			printf( "\n--- tree (first 3 levels) ---\n" );
			print_tree( root_dnode, 0, 3 );
			printf( "\nbuilding MapV scene...\n" );

			/* MUST happen before mapv_build_scene() -- see its comment
			 * in mapv.h. This is exactly the call the bug was missing. */
			mapv_reset_navigation( );
			mapv_build_scene( );
			print_status( );
		}

		if (kDown & KEY_A) {
			if (mapv_drill_selected( ))
				print_status( );
		}

		if (kDown & KEY_B) {
			if (mapv_go_up( ))
				print_status( );
		}

		/* Swapped from the "natural" RIGHT=+1/LEFT=-1 mapping: sibling
		 * order in the GNode list doesn't correspond to left-to-right
		 * screen position the way you'd expect, so the obvious mapping
		 * felt backwards on hardware. This matches what it should feel
		 * like, not what the underlying list order says. */
		if (kDown & KEY_DRIGHT) {
			mapv_cycle_selection( -1 );
			print_status( );
		}
		if (kDown & KEY_DLEFT) {
			mapv_cycle_selection( 1 );
			print_status( );
		}
		/* Untested direction sense (same caveat as the L/R swap above)
		 * -- if up/down jump to the wrong row, swap these two signs. */
		if (kDown & KEY_DUP) {
			mapv_move_selection_row( 1 );
			print_status( );
		}
		if (kDown & KEY_DDOWN) {
			mapv_move_selection_row( -1 );
			print_status( );
		}

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
			mapv_camera_orbit( dtheta, dphi, zoom );

		/* Unconditional, every frame: eases the camera toward whatever
		 * was last selected -- see mapv_camera_tick()'s comment. */
		mapv_camera_tick( );

		/* C3D_FrameBegin(C3D_FRAME_SYNCDRAW) inside render_frame() paces
		 * this loop to vblank; no separate gfxFlushBuffers/SwapBuffers/
		 * gspWaitForVBlank needed once citro3d is driving the frame
		 * (confirmed against devkitPro's gpusprites example, which
		 * live-updates a GFX_BOTTOM console every frame the same way). */
		render_frame( );

		rpc_poll( );
	}

	rpc_fini( );
	render_fini( );
	gfxExit( );

	return 0;
}

/* end main.c */
