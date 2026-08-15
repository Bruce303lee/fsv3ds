/* imgview.h - fsv3ds Phase 7
 *
 * BMP/PNG/JPEG decode + citro3d texture upload for ui.c's image
 * viewer screen. See imgview.c for the decode/upload pipeline and the
 * size-cap rationale.
 */
#ifndef FSV3DS_IMGVIEW_H
#define FSV3DS_IMGVIEW_H

#include "common.h"

/* Decodes `path` (dispatched on extension: bmp/png/jpg/jpeg) and
 * uploads it to a citro3d texture ready for imgview_draw(). Replaces
 * whatever was previously open (calls imgview_close() itself first).
 * Returns FALSE on any failure -- bad/unsupported format, corrupt
 * file, too large to decode within the size cap, out of memory --
 * with a reason available from imgview_error(). */
gboolean imgview_open( const char *path );

/* Frees the current texture, if any. Safe to call with nothing open;
 * imgview_open() and ui_fini() both call this. */
void imgview_close( void );

/* NULL after a successful imgview_open(); otherwise a short
 * human-readable reason, valid until the next open()/close() call. */
const char *imgview_error( void );

/* Decoded pixel dimensions (post any JPEG DCT downscale, pre padding
 * to the power-of-two texture) -- 0/0 if nothing is open. */
int imgview_width( void );
int imgview_height( void );

/* Draws the currently open image via citro2d, uniformly scaled to fit
 * within box_w x box_h without ever upscaling past 1:1, top-left
 * corner at (x, y). No-op if nothing is open. Caller must already be
 * inside a citro2d scene (ui.c's ui_draw() always is). */
void imgview_draw( float x, float y, float box_w, float box_h );

#endif /* FSV3DS_IMGVIEW_H */
