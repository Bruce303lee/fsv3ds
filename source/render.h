/* render.h - fsv3ds Phase 2
 *
 * citro3d rendering layer -- replaces upstream's ogl.c wholesale (see
 * that file's header comment in geometry.c-era code for why: there is
 * no OpenGL on 3DS, only the PICA200 GPU via citro3d).
 */
#ifndef FSV3DS_RENDER_H
#define FSV3DS_RENDER_H

void render_init( void );
void render_frame( void );
void render_fini( void );

/* Dumps the current top-screen framebuffer to a binary PPM (P6) file on
 * the SD card, for pulling over FTP -- much faster to iterate on than
 * photographing the console. Call right after render_frame(). */
void render_screenshot( const char *path );

/* Same un-rotation/color-fix as render_screenshot(), but into a
 * caller-owned 400*240*3-byte RGB buffer instead of a file -- used by
 * rpc.c to stream a screenshot straight over the network. */
void render_capture_rgb( unsigned char *out_400x240x3 );

#endif /* FSV3DS_RENDER_H */
