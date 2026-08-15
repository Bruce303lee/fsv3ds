/* ui.h - fsv3ds Phase 6
 *
 * Touch-driven bottom-screen UI: an iOS-ish status bar (battery/wifi),
 * a left-hand button rail (Folder/Settings/Info/Log), a content area
 * for whichever of those is active, and a footer breadcrumb. Replaces
 * main.c's old PrintConsole-based breadcrumb+log console entirely --
 * see main.c's history for that.
 *
 * Owns its own C2D text buffer and drawing, called once per frame from
 * render.c (which owns the bottom screen's C3D_RenderTarget and frame
 * lifecycle, same as it does for the top screen's stereo pair) so this
 * file doesn't need its own citro3d frame-management duplicated here.
 */
#ifndef FSV3DS_UI_H
#define FSV3DS_UI_H

#include <citro3d.h>

#include "common.h" /* for gboolean, used by ui_take_launch_request() */

void ui_init( void );
void ui_fini( void );

/* Draws the whole bottom screen into `target`, which render.c has
 * already bound via C3D_FrameDrawOn before calling this. Owns its own
 * C2D scene begin/prepare/flush -- caller doesn't need to do anything
 * citro2d-related around this call. */
void ui_draw( C3D_RenderTarget *target );

/* Hit-tests one tap against the button rail and whatever content
 * screen is currently active, applying whatever it lands on (switch
 * screen, cycle a setting, re-root+rescan at the selected folder).
 * x/y are bottom-screen pixel coordinates, 0..319 / 0..239 -- same
 * space hidTouchRead()/the RPC TOUCH command report in. */
void ui_handle_touch( int x, int y );

/* Requests a scan of `path` (via viz_scan_and_build()), showing a
 * "Scanning..." overlay first so the blocking scanfs() walk that
 * follows doesn't just freeze the app with no feedback. Deferred, not
 * immediate: only arms the overlay here; the caller MUST call
 * ui_process_pending_scan() once per main-loop iteration (right after
 * render_frame(), see main.c) for anything to actually happen -- that
 * indirection is deliberate, see ui_request_scan()'s comment in ui.c
 * for the GPU command buffer overflow it avoids. Used by main.c's
 * SELECT handler and the Folder screen's "Use this"; rpc.c's SCAN
 * command deliberately calls viz_scan_and_build() directly instead
 * (see its own comment). Switches to the Log screen once the scan
 * actually completes. */
void ui_request_scan( const char *path );

/* Call exactly once per main-loop iteration, after render_frame() --
 * no-op unless a scan is pending (see ui_request_scan()). */
void ui_process_pending_scan( void );

/* One-shot, consumed-once flag: TRUE if the Info screen's "Launch"
 * action has successfully handed a target off to launcher_launch()
 * (see launcher.h) since the last call. main.c should check this each
 * iteration (right after touch handling) and break its main loop on
 * TRUE, the same shutdown path as the physical START button -- that's
 * what lets Luma3DS's hb:ldr actually take over once this process
 * exits. */
gboolean ui_take_launch_request( void );

#endif /* FSV3DS_UI_H */
