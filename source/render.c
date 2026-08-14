/* render.c - fsv3ds Phase 3
 *
 * Replaces upstream's ogl.c. Where ogl.c did glFrustum + hand-rolled
 * glTranslate/glRotate to build modelview, this computes an eye
 * position from the camera's theta/phi/distance (see mapv.c's
 * mapv_camera_init(), a port of camera.c's mapv_get_camera_position())
 * and feeds it straight into citro3d's Mtx_LookAt -- no need to
 * reproduce ogl.c's base-axis-remap rotation stack that way.
 *
 * No lighting (geometry colors are pre-shaded per-vertex by mapv.c),
 * so the shader is a flat passthrough and there's only one substage.
 *
 * Stereoscopic 3D: renders the whole scene twice per frame (once per
 * eye, GFX_LEFT/GFX_RIGHT) via Mtx_PerspStereoTilt's frustum shear,
 * scaled by the physical 3D slider. Node labels are a citro2d text
 * overlay drawn on top of each eye's 3D pass -- see draw_labels() for
 * why that needs its own, non-Tilt projection matrix.
 */
#include <3ds.h>
#include <citro3d.h>
#include <citro2d.h>
#include <math.h>
#include <string.h>
#include <stdio.h>

#include "render.h"
#include "mapv.h"
#include "vshader_shbin.h"

#define CLEAR_COLOR 0x181820FF

#define DISPLAY_TRANSFER_FLAGS \
	(GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) | \
	GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) | \
	GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO))

#define RENDER_PI 3.14159265358979323846f
#define LABEL_TEXTBUF_GLYPHS 4096
#define CLAMP(x,lo,hi) ((x) < (lo) ? (lo) : ((x) > (hi) ? (hi) : (x)))

static DVLB_s *vshader_dvlb;
static shaderProgram_s program;
static int uLoc_viewProjection;
static C3D_RenderTarget *targetLeft;
static C3D_RenderTarget *targetRight;
static C2D_TextBuf textBuf;


void
render_init( void )
{
	/* Matches DISPLAY_TRANSFER_FLAGS' GX_TRANSFER_OUT_FORMAT(RGB8) below,
	 * so render_screenshot() can assume 3 bytes/pixel without guessing.
	 * (GSP's 3-byte format is confusingly named "BGR8" -- byte order is
	 * B,G,R in memory -- render_screenshot() swaps it back for the PPM.) */
	gfxSetScreenFormat( GFX_TOP, GSP_BGR8_OES );
	gfxSet3D( true ); /* enable the top screen's stereoscopic 3D display */

	C3D_Init( C3D_DEFAULT_CMDBUF_SIZE );

	targetLeft = C3D_RenderTargetCreate( 240, 400, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8 );
	targetRight = C3D_RenderTargetCreate( 240, 400, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8 );
	C3D_RenderTargetSetOutput( targetLeft, GFX_TOP, GFX_LEFT, DISPLAY_TRANSFER_FLAGS );
	C3D_RenderTargetSetOutput( targetRight, GFX_TOP, GFX_RIGHT, DISPLAY_TRANSFER_FLAGS );

	vshader_dvlb = DVLB_ParseFile( (u32 *)vshader_shbin, vshader_shbin_size );
	shaderProgramInit( &program );
	shaderProgramSetVsh( &program, &vshader_dvlb->DVLE[0] );

	uLoc_viewProjection = shaderInstanceGetUniformLocation( program.vertexShader, "viewProjection" );

	/* citro2d's text pass (draw_labels(), interleaved with the 3D pass
	 * below) rebinds its own shader/attributes/texenv/depth state, so
	 * none of our pipeline setup can be "set once" here anymore -- see
	 * draw_scene(), which re-asserts all of it on every call. */
	C2D_Init( C2D_DEFAULT_MAX_OBJECTS );
	C2D_Prepare( );
	textBuf = C2D_TextBufNew( LABEL_TEXTBUF_GLYPHS );
}


/* Shared by both projection variants below. */
static void
compute_eye_target_fov( C3D_FVec *eye, C3D_FVec *target, float *fovy_rad )
{
	const MapVCameraState *cam = mapv_camera( );
	float theta_rad = cam->theta * (RENDER_PI / 180.0f);
	float phi_rad = cam->phi * (RENDER_PI / 180.0f);
	float fovx_rad = cam->fov * (RENDER_PI / 180.0f);

	*eye = FVec3_New(
		cam->target_x + cam->distance * cosf( theta_rad ) * cosf( phi_rad ),
		cam->target_y + cam->distance * sinf( theta_rad ) * cosf( phi_rad ),
		cam->target_z + cam->distance * sinf( phi_rad ) );
	*target = FVec3_New( cam->target_x, cam->target_y, cam->target_z );

	/* Upstream ogl.c's setup_projection_matrix(): dx = near*tan(0.5*fovX),
	 * dy = dx/aspect -- i.e. camera->fov is a *horizontal* FOV. citro3d's
	 * Persp variants want vertical FOV, so convert. */
	*fovy_rad = 2.0f * atanf( tanf( 0.5f * fovx_rad ) / C3D_AspectRatioTop );
}


/* iod (interocular distance, in the same world units as near/far/target)
 * is 0 for a mono/single-eye view, or +-half the eye separation for a
 * stereo pair -- Mtx_PerspStereoTilt shears the frustum by iod instead
 * of literally moving the eye, converging at the "screen" distance
 * passed alongside it (set to the camera's look-at distance: the
 * selected node ends up right at the screen plane with zero parallax,
 * nearer boxes pop out, farther ones recede). */
static void
compute_view_projection( C3D_Mtx *out, float iod )
{
	const MapVCameraState *cam = mapv_camera( );
	C3D_Mtx projection, view;
	C3D_FVec eye, target, up;
	float fovy_rad;

	compute_eye_target_fov( &eye, &target, &fovy_rad );

	Mtx_PerspStereoTilt( &projection, fovy_rad, C3D_AspectRatioTop,
		cam->near_clip, cam->far_clip, iod, cam->distance, false );

	up = FVec3_New( 0.0f, 0.0f, 1.0f ); /* world is +Z-up */
	Mtx_LookAt( &view, eye, target, up, false );

	Mtx_Multiply( out, &projection, &view );
}


/* Same view as compute_view_projection() but WITHOUT the Tilt baked in.
 * Mtx_PerspTilt/PerspStereoTilt pre-rotate clip space to match the
 * GPU's physically-rotated framebuffer, so NDC.x/NDC.y out of *that*
 * matrix don't correspond to normal horizontal/vertical screen axes.
 * citro2d expects normal (non-rotated) pixel coordinates and handles
 * the physical rotation itself internally, so CPU-side label placement
 * (draw_labels()) needs this separate, non-Tilt projection to land in
 * the axes citro2d actually expects. */
static void
compute_view_projection_notilt( C3D_Mtx *out, float iod, C3D_FVec *out_eye )
{
	const MapVCameraState *cam = mapv_camera( );
	C3D_Mtx projection, view;
	C3D_FVec eye, target, up;
	float fovy_rad;

	compute_eye_target_fov( &eye, &target, &fovy_rad );
	if (out_eye != NULL)
		*out_eye = eye;

	Mtx_PerspStereo( &projection, fovy_rad, C3D_AspectRatioTop,
		cam->near_clip, cam->far_clip, iod, cam->distance, false );

	up = FVec3_New( 0.0f, 0.0f, 1.0f );
	Mtx_LookAt( &view, eye, target, up, false );

	Mtx_Multiply( out, &projection, &view );
}


static void
draw_scene( C3D_RenderTarget *target, float iod )
{
	const MapVVertex *verts = mapv_vertex_data( );
	unsigned int nverts = mapv_vertex_count( );
	C3D_Mtx viewProjection;
	C3D_AttrInfo *attrInfo;
	C3D_TexEnv *env;
	C3D_BufInfo *bufInfo;

	C3D_RenderTargetClear( target, C3D_CLEAR_ALL, CLEAR_COLOR, 0 );
	C3D_FrameDrawOn( target );

	if (verts == NULL || nverts == 0)
		return;

	/* Re-assert our 3D pipeline state every call -- see render_init()'s
	 * comment on why this can no longer be set-once. */
	C3D_BindProgram( &program );

	attrInfo = C3D_GetAttrInfo( );
	AttrInfo_Init( attrInfo );
	AttrInfo_AddLoader( attrInfo, 0, GPU_FLOAT, 3 ); /* v0 = position */
	AttrInfo_AddLoader( attrInfo, 1, GPU_FLOAT, 4 ); /* v1 = color */

	/* Flat vertex-colored passthrough: fragment color = vertex color.
	 * Also explicitly clear texenv units 1-5: citro2d's text pass uses
	 * multiple combiner stages for font rendering, and devkitPro's own
	 * composite_scene example (3D + citro2d text + stereo, the same
	 * combination this file does) resets all of them before its own 3D
	 * draw for exactly that reason. */
	env = C3D_GetTexEnv( 0 );
	C3D_TexEnvInit( env );
	C3D_TexEnvSrc( env, C3D_Both, GPU_PRIMARY_COLOR, 0, 0 );
	C3D_TexEnvFunc( env, C3D_Both, GPU_REPLACE );
	C3D_TexEnvInit( C3D_GetTexEnv( 1 ) );
	C3D_TexEnvInit( C3D_GetTexEnv( 2 ) );
	C3D_TexEnvInit( C3D_GetTexEnv( 3 ) );
	C3D_TexEnvInit( C3D_GetTexEnv( 4 ) );
	C3D_TexEnvInit( C3D_GetTexEnv( 5 ) );

	C3D_DepthTest( true, GPU_GEQUAL, GPU_WRITE_ALL );
	/* No culling in v1: box winding wasn't verified against PICA200's
	 * front-face convention, and it's cheap enough at this vertex
	 * count not to matter yet. Revisit once TreeV needs the headroom. */
	C3D_CullFace( GPU_CULL_NONE );

	compute_view_projection( &viewProjection, iod );
	C3D_FVUnifMtx4x4( GPU_VERTEX_SHADER, uLoc_viewProjection, &viewProjection );

	bufInfo = C3D_GetBufInfo( );
	BufInfo_Init( bufInfo );
	BufInfo_Add( bufInfo, verts, sizeof(MapVVertex), 2, 0x10 );

	C3D_DrawArrays( GPU_TRIANGLES, 0, nverts );
}


/* Projects the selected node's label anchor to screen space by hand
 * (see compute_view_projection_notilt()'s comment) and draws its name
 * via citro2d, layered on top of whatever draw_scene() just put in
 * `target`. Not given any stereo depth of its own -- HUD/label text
 * stays flat on the screen plane in both eyes, which is the common
 * convention (and avoids doubling this same amount of complexity
 * again). Iterates mapv_label_data() looking for the one entry with
 * is_selected set rather than mapv.c exposing a single-label accessor,
 * since mapv.c already has to build the full per-box list for
 * emit_node_box()'s highlight anyway -- this is the only consumer that
 * currently cares about just the one. */
/* Base text scale at base_distance (the distance the camera was reset
 * to when this view was last framed) -- shrinks/grows from there as
 * the live camera distance and each label's own position diverge from
 * that reference, clamped so text never gets illegibly tiny or
 * absurdly huge at extreme zoom. */
/* 20% down from the 0.28/0.12/0.60 that preceded the previous (too
 * aggressive, ~40% down) 0.16/0.07/0.32 pass. */
#define LABEL_BASE_SCALE 0.22f
#define LABEL_MIN_SCALE  0.10f
#define LABEL_MAX_SCALE  0.48f

static void
draw_labels( C3D_RenderTarget *target )
{
	const MapVLabel *labels = mapv_label_data( );
	unsigned int n = mapv_label_count( );
	const MapVCameraState *cam = mapv_camera( );
	C3D_Mtx vp;
	C3D_FVec eye;
	unsigned int i;

	compute_view_projection_notilt( &vp, 0.0f, &eye );

	C2D_SceneBegin( target );
	C2D_Prepare( );
	C2D_TextBufClear( textBuf );

	for (i = 0; i < n; i++) {
		C3D_FVec world, clip;
		float sx, sy, dist, scale;
		C2D_Text text;

		/* Only the selected node gets a label now -- with ~40 boxes on
		 * screen and no overlap avoidance, labeling all of them was an
		 * unreadable pile of overlapping text (see the two screenshots
		 * that motivated this). */
		if (!labels[i].is_selected)
			continue;

		world = FVec4_New( labels[i].x, labels[i].y, labels[i].z, 1.0f );
		clip = Mtx_MultiplyFVec4( &vp, world );

		if (clip.w <= 1.0f)
			continue; /* behind, or right at, the camera */

		sx = (clip.x / clip.w * 0.5f + 0.5f) * 400.0f;
		sy = (1.0f - (clip.y / clip.w * 0.5f + 0.5f)) * 240.0f;

		if (sx < -80.0f || sx > 480.0f || sy < -20.0f || sy > 260.0f)
			continue; /* well off-screen */

		/* "distance" here is real eye-to-label distance (not just
		 * cam->distance, which is eye-to-*target*): zooming with the
		 * circle pad scales the eye's distance from every point in the
		 * scene together, so this alone is what actually makes labels
		 * grow/shrink as you zoom -- a ratio against cam->distance
		 * wouldn't, since that ratio stays ~constant through a zoom. */
		dist = FVec3_Distance( eye, FVec3_New( world.x, world.y, world.z ) );
		scale = CLAMP( LABEL_BASE_SCALE * (cam->base_distance / dist), LABEL_MIN_SCALE, LABEL_MAX_SCALE );

		C2D_TextParse( &text, textBuf, labels[i].name );
		C2D_TextOptimize( &text );

		/* Drop shadow for legibility against whatever color the box
		 * under it happens to be: same text, black, offset a bit and
		 * drawn first, then the real white text directly on top.
		 * Offset scales with `scale` so it stays proportional (a fixed
		 * pixel offset would look like a thick outline at the smallest
		 * sizes and be invisible at the largest). */
		{
			float shadow_off = scale * 3.0f;

			C2D_DrawText( &text, C2D_AlignCenter | C2D_WithColor,
				sx + shadow_off, sy + shadow_off, 0.4f, scale, scale,
				C2D_Color32( 0, 0, 0, 255 ) );
		}
		C2D_DrawText( &text, C2D_AlignCenter | C2D_WithColor, sx, sy, 0.5f, scale, scale,
			C2D_Color32( 255, 255, 255, 255 ) );
	}

	/* Without this, citro2d's batched text draws for this call never
	 * get flushed to the GPU command buffer before the next eye's 3D
	 * pass (or C3D_FrameEnd()) runs -- devkitPro's composite_scene
	 * example flushes at the same point, right after its C2D_DrawText
	 * calls. Suspected cause of an observed hard freeze after some
	 * time running: without a flush, citro2d's internal per-frame
	 * object batch (capped at C2D_DEFAULT_MAX_OBJECTS) would just keep
	 * accumulating unflushed objects, frame after frame, until it hit
	 * that ceiling. */
	C2D_Flush( );
}


void
render_frame( void )
{
	float slider, iod;

	slider = osGet3DSliderState( );
	/* Scaled to our world units (tens of thousands, driven by scanned
	 * byte sizes) rather than the small fixed constant devkitPro's
	 * examples use for their meters-ish scenes -- proportion, not
	 * magnitude, is what should carry over. 0.03 read as too subtle on
	 * hardware; doubled. */
	iod = slider * mapv_camera( )->distance * 0.06f;

	/* Always cycle Begin/End, even with nothing to draw yet (before the
	 * first scan) -- C3D_FRAME_SYNCDRAW is what paces the main loop to
	 * vblank now that main.c no longer calls gspWaitForVBlank itself. */
	C3D_FrameBegin( C3D_FRAME_SYNCDRAW );

	/* Confirmed backwards on hardware: with -iod on the left eye (the
	 * devkitPro example's convention), boxes read as receding into the
	 * screen instead of popping out. Swapped. */
	draw_scene( targetLeft, iod );
	draw_labels( targetLeft );

	if (iod > 0.0f) {
		draw_scene( targetRight, -iod );
		draw_labels( targetRight );
	}

	C3D_FrameEnd( 0 );
}


void
render_fini( void )
{
	C2D_TextBufDelete( textBuf );
	C2D_Fini( );

	shaderProgramFree( &program );
	DVLB_Free( vshader_dvlb );
	C3D_Fini( );
}


/* The 3DS top screen framebuffer is physically stored rotated 90 deg:
 * gfxGetFramebuffer reports it as `stride` x 400 (stride is nominally
 * 240), where consecutive bytes run down what's visually a *column* of
 * the real 400x240 landscape image, starting from the bottom. This
 * un-rotates it into normal top-to-bottom, left-to-right RGB8. */
void
render_capture_rgb( unsigned char *out )
{
	u16 stride, fb_h;
	u8 *fb;
	int x, y;

	fb = gfxGetFramebuffer( GFX_TOP, GFX_LEFT, &stride, &fb_h );
	if (fb == NULL)
		return;

	for (y = 0; y < 240; y++) {
		for (x = 0; x < 400; x++) {
			u32 srcOff = (u32)(stride * x + (stride - 1 - y)) * 3;
			unsigned char *dst = &out[(y * 400 + x) * 3];

			/* memory order is B,G,R (see GSP_BGR8_OES note above) */
			dst[0] = fb[srcOff + 2];
			dst[1] = fb[srcOff + 1];
			dst[2] = fb[srcOff + 0];
		}
	}
}


void
render_screenshot( const char *path )
{
	static unsigned char rgb[400 * 240 * 3];
	FILE *f;

	render_capture_rgb( rgb );

	f = fopen( path, "wb" );
	if (f == NULL)
		return;

	fprintf( f, "P6\n400 240\n255\n" );
	fwrite( rgb, 1, sizeof(rgb), f );

	fclose( f );
}

/* end render.c */
