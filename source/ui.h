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

#endif /* FSV3DS_UI_H */
