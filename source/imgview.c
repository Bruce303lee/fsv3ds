/* imgview.c - see imgview.h
 *
 * Three decoders feed one texture-upload path:
 *   - BMP:  hand-rolled (uncompressed 24/32bpp only -- no portlib for
 *           this format, and it's simple enough not to need one).
 *   - PNG:  libpng's "simplified API" (png_image_begin/finish_read),
 *           decoding straight to 8-bit RGBA in one call.
 *   - JPEG: libjpeg-turbo's classic API, using JCS_EXT_RGBA so the
 *           decoder itself emits RGBA scanlines (no manual per-pixel
 *           conversion), and scale_num/scale_denom (1/1..1/8) to keep
 *           a big photo cheap to decode instead of allocating its
 *           full native resolution just to throw most of it away.
 *
 * All three converge on the same DecodedImage{width,height,rgba}
 * (top-down, 4 bytes/pixel, plain malloc'd) which imgview_open() then
 * hands to upload_texture(), which swizzles it by hand into a
 * power-of-two C3D_Tex -- see upload_texture()'s own comment for why
 * that's done manually rather than via GX_DisplayTransfer.
 *
 * IMGVIEW_MAX_SRC_DIM caps decoded pixel dimensions (post JPEG
 * downscale) to keep worst-case memory bounded -- BMP/PNG have no
 * built-in progressive-scale decode the way JPEG does, so those two
 * just refuse anything bigger outright rather than risk a huge
 * allocation on the 3DS's limited linear heap. 512 keeps the decode
 * buffer (512*512*4 = 1MiB) comfortably small; the bottom screen's
 * content area is a small fraction of that anyway, so nothing bigger
 * would even be visible at native res.
 */
#include <3ds.h>
#include <citro3d.h>
#include <citro2d.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <setjmp.h>

#include <png.h>
#include <jpeglib.h> /* needs <stdio.h> already included -- see its own header comment */
#include <jerror.h>

#include "imgview.h"

#define IMGVIEW_MAX_SRC_DIM 512

typedef struct {
	int width, height;
	unsigned char *rgba; /* width*height*4, top-down, malloc'd */
} DecodedImage;

static C3D_Tex tex;
static Tex3DS_SubTexture subtex;
static C2D_Image image;
static gboolean image_open = FALSE;
static int img_w = 0, img_h = 0;
static char error_buf[128]; /* libpng's image.message can run fairly long */


const char *
imgview_error( void )
{
	return error_buf[0] != '\0' ? error_buf : NULL;
}


int
imgview_width( void )  { return img_w; }

int
imgview_height( void ) { return img_h; }


static gboolean
ext_is( const char *name, const char *ext )
{
	size_t nlen = strlen( name ), elen = strlen( ext );

	if (nlen < elen + 1 || name[nlen - elen - 1] != '.')
		return FALSE;
	return strcasecmp( name + nlen - elen, ext ) == 0;
}


/* --- BMP: uncompressed 24/32bpp only --- */

static u32
rd_u32le( const unsigned char *p ) { return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24; }

static u16
rd_u16le( const unsigned char *p ) { return (u16)(p[0] | p[1] << 8); }


static gboolean
decode_bmp( const char *path, DecodedImage *out, char *errbuf, size_t errbuf_sz )
{
	FILE *f;
	unsigned char fh[14], ih[40];
	u32 off_bits, compression;
	int width, height, bpp, top_down;
	u32 row_stride;
	unsigned char *row = NULL;
	int y;

	f = fopen( path, "rb" );
	if (f == NULL) {
		snprintf( errbuf, errbuf_sz, "cannot open file" );
		return FALSE;
	}

	if (fread( fh, 1, sizeof(fh), f ) != sizeof(fh) || fh[0] != 'B' || fh[1] != 'M') {
		snprintf( errbuf, errbuf_sz, "not a BMP file" );
		fclose( f );
		return FALSE;
	}
	off_bits = rd_u32le( fh + 10 );

	if (fread( ih, 1, sizeof(ih), f ) != sizeof(ih) || rd_u32le( ih ) < 40) {
		snprintf( errbuf, errbuf_sz, "unsupported BMP header" );
		fclose( f );
		return FALSE;
	}
	width = (int)rd_u32le( ih + 4 );
	height = (int)rd_u32le( ih + 8 ); /* biHeight is signed (negative = top-down); rd_u32le's
	                                    * bit pattern reinterpreted via this cast is what we want. */
	bpp = rd_u16le( ih + 14 );
	compression = rd_u32le( ih + 16 );

	top_down = height < 0;
	if (top_down)
		height = -height;

	if (compression != 0 /* BI_RGB */ || (bpp != 24 && bpp != 32)) {
		snprintf( errbuf, errbuf_sz, "unsupported BMP format (need uncompressed 24/32bpp)" );
		fclose( f );
		return FALSE;
	}
	if (width <= 0 || height <= 0 || width > IMGVIEW_MAX_SRC_DIM || height > IMGVIEW_MAX_SRC_DIM) {
		snprintf( errbuf, errbuf_sz, "image too large (max %dx%d)", IMGVIEW_MAX_SRC_DIM, IMGVIEW_MAX_SRC_DIM );
		fclose( f );
		return FALSE;
	}

	row_stride = ((u32)width * (bpp / 8) + 3) & ~3u;
	row = malloc( row_stride );
	out->rgba = malloc( (size_t)width * height * 4 );
	if (row == NULL || out->rgba == NULL) {
		snprintf( errbuf, errbuf_sz, "out of memory" );
		free( row );
		free( out->rgba );
		out->rgba = NULL;
		fclose( f );
		return FALSE;
	}
	out->width = width;
	out->height = height;

	fseek( f, (long)off_bits, SEEK_SET );
	for (y = 0; y < height; y++) {
		/* BMP rows are bottom-up by default (positive height); a
		 * negative height (top_down) means the file already stores
		 * top-to-bottom. Either way, out->rgba is always top-down. */
		int dst_row = top_down ? y : (height - 1 - y);
		unsigned char *dst = out->rgba + (size_t)dst_row * width * 4;
		int x;

		if (fread( row, 1, row_stride, f ) != row_stride) {
			snprintf( errbuf, errbuf_sz, "truncated BMP file" );
			free( row );
			free( out->rgba );
			out->rgba = NULL;
			fclose( f );
			return FALSE;
		}
		for (x = 0; x < width; x++) {
			unsigned char *sp = row + x * (bpp / 8);

			dst[x * 4 + 0] = sp[2]; /* B,G,R -> R,G,B */
			dst[x * 4 + 1] = sp[1];
			dst[x * 4 + 2] = sp[0];
			dst[x * 4 + 3] = (bpp == 32) ? sp[3] : 255;
		}
	}

	free( row );
	fclose( f );
	return TRUE;
}


/* --- PNG: libpng simplified API --- */

static gboolean
decode_png( const char *path, DecodedImage *out, char *errbuf, size_t errbuf_sz )
{
	png_image png;

	memset( &png, 0, sizeof(png) );
	png.version = PNG_IMAGE_VERSION;

	if (!png_image_begin_read_from_file( &png, path )) {
		snprintf( errbuf, errbuf_sz, "png: %s", png.message );
		return FALSE;
	}

	if (png.width > IMGVIEW_MAX_SRC_DIM || png.height > IMGVIEW_MAX_SRC_DIM) {
		png_image_free( &png );
		snprintf( errbuf, errbuf_sz, "image too large (max %dx%d)", IMGVIEW_MAX_SRC_DIM, IMGVIEW_MAX_SRC_DIM );
		return FALSE;
	}

	png.format = PNG_FORMAT_RGBA;
	out->width = (int)png.width;
	out->height = (int)png.height;
	out->rgba = malloc( (size_t)out->width * out->height * 4 );
	if (out->rgba == NULL) {
		png_image_free( &png );
		snprintf( errbuf, errbuf_sz, "out of memory" );
		return FALSE;
	}

	if (!png_image_finish_read( &png, NULL, out->rgba, 0, NULL )) {
		snprintf( errbuf, errbuf_sz, "png: %s", png.message );
		free( out->rgba );
		out->rgba = NULL;
		png_image_free( &png );
		return FALSE;
	}

	png_image_free( &png );
	return TRUE;
}


/* --- JPEG: libjpeg-turbo classic API + longjmp error handling
 * (the documented pattern from libjpeg's own example.c) --- */

struct jpeg_err_ctx {
	struct jpeg_error_mgr pub;
	jmp_buf jb;
};

static void
jpeg_longjmp_error_exit( j_common_ptr cinfo )
{
	struct jpeg_err_ctx *err = (struct jpeg_err_ctx *)cinfo->err;

	longjmp( err->jb, 1 );
}


static gboolean
decode_jpeg( const char *path, DecodedImage *out, char *errbuf, size_t errbuf_sz )
{
	struct jpeg_decompress_struct cinfo;
	struct jpeg_err_ctx jerr;
	FILE *f;
	int denom;

	out->rgba = NULL;
	out->width = out->height = 0;

	f = fopen( path, "rb" );
	if (f == NULL) {
		snprintf( errbuf, errbuf_sz, "cannot open file" );
		return FALSE;
	}

	cinfo.err = jpeg_std_error( &jerr.pub );
	jerr.pub.error_exit = jpeg_longjmp_error_exit;
	if (setjmp( jerr.jb )) {
		jpeg_destroy_decompress( &cinfo );
		fclose( f );
		free( out->rgba );
		out->rgba = NULL;
		snprintf( errbuf, errbuf_sz, "jpeg: decode error" );
		return FALSE;
	}

	jpeg_create_decompress( &cinfo );
	jpeg_stdio_src( &cinfo, f );
	jpeg_read_header( &cinfo, TRUE );

	/* Coarsest 1/1, 1/2, 1/4, 1/8 DCT scale that still fits the cap --
	 * libjpeg decodes directly at that resolution, so a big photo
	 * never costs more than a small one to decode. */
	for (denom = 1; denom <= 8; denom *= 2) {
		if (cinfo.image_width / (unsigned int)denom <= IMGVIEW_MAX_SRC_DIM &&
		    cinfo.image_height / (unsigned int)denom <= IMGVIEW_MAX_SRC_DIM)
			break;
	}
	if (denom > 8) {
		jpeg_destroy_decompress( &cinfo );
		fclose( f );
		snprintf( errbuf, errbuf_sz, "image too large (max %dx%d even at 1/8 scale)",
			IMGVIEW_MAX_SRC_DIM, IMGVIEW_MAX_SRC_DIM );
		return FALSE;
	}
	cinfo.scale_num = 1;
	cinfo.scale_denom = denom;
	cinfo.out_color_space = JCS_EXT_RGBA; /* decoder emits RGBA rows directly */

	jpeg_start_decompress( &cinfo );

	out->width = (int)cinfo.output_width;
	out->height = (int)cinfo.output_height;
	out->rgba = malloc( (size_t)out->width * out->height * 4 );
	if (out->rgba == NULL) {
		jpeg_destroy_decompress( &cinfo );
		fclose( f );
		snprintf( errbuf, errbuf_sz, "out of memory" );
		return FALSE;
	}

	while (cinfo.output_scanline < cinfo.output_height) {
		JSAMPROW row_pointer[1];

		row_pointer[0] = out->rgba + (size_t)cinfo.output_scanline * out->width * 4;
		jpeg_read_scanlines( &cinfo, row_pointer, 1 );
	}

	jpeg_finish_decompress( &cinfo );
	jpeg_destroy_decompress( &cinfo );
	fclose( f );

	return TRUE;
}


/* --- texture upload: pad to POT, manual Z-order swizzle into tex.data ---
 *
 * GX_DisplayTransfer's linear->tiled hardware path (the "textbook"
 * approach) turned out to have some undocumented edge case that
 * corrupted uploads when the source width exactly equalled the padded
 * POT width (no horizontal slack) -- reproducibly wrong on a 64x48
 * test image (64 already POT) but not on two other images that both
 * happened to need padding on both axes. Rather than fully chase the
 * hardware transfer engine's constraints (3dbrew documents several
 * alignment/size minimums for it that aren't obviously satisfied for
 * arbitrary small images anyway), this does the standard alternative:
 * compute each pixel's address in the GPU's 8x8-tile Z-order layout
 * by hand and write it directly into tex.data. No hardware transfer
 * engine involved, so none of its constraints apply. */

static int
next_pow2( int n )
{
	int p = 1;

	while (p < n)
		p <<= 1;
	return p;
}


/* Z-order (Morton) position of each (row,col) within an 8x8 tile --
 * the standard table for PICA200 texture swizzling. */
static const unsigned char tile_order[64] = {
	 0,  1,  8,  9,  2,  3, 10, 11,
	16, 17, 24, 25, 18, 19, 26, 27,
	 4,  5, 12, 13,  6,  7, 14, 15,
	20, 21, 28, 29, 22, 23, 30, 31,
	32, 33, 40, 41, 34, 35, 42, 43,
	48, 49, 56, 57, 50, 51, 58, 59,
	36, 37, 44, 45, 38, 39, 46, 47,
	52, 53, 60, 61, 54, 55, 62, 63
};

static void
swizzle_to_texture( u32 *dst, const unsigned char *rgba, int w, int h, int pot_w )
{
	int tiles_per_row = pot_w / 8;
	int x, y;

	for (y = 0; y < h; y++) {
		int tile_y = y >> 3, py = y & 7;

		for (x = 0; x < w; x++) {
			int tile_x = x >> 3, px = x & 7;
			int tile_idx = tile_y * tiles_per_row + tile_x;
			const unsigned char *sp = rgba + ((size_t)y * w + x) * 4;

			/* PICA200's GPU_RGBA8 texture format stores each pixel as
			 * A,B,G,R in ascending memory address -- the reverse of
			 * the R,G,B,A order every decoder here produces. Confirmed
			 * by decoding a 4-quadrant test BMP by hand from an RPC
			 * screenshot: red survived untouched (symmetric under this
			 * swap, R and A both 255), yellow became magenta-with-A=255,
			 * and green/blue became magenta/yellow-with-A=0 (invisible)
			 * -- an exact match for this exact reversal, not a rough one. */
			dst[tile_idx * 64 + tile_order[py * 8 + px]] =
				(u32)sp[3] | (u32)sp[2] << 8 | (u32)sp[1] << 16 | (u32)sp[0] << 24;
		}
	}
}

static gboolean
upload_texture( const DecodedImage *dec, char *errbuf, size_t errbuf_sz )
{
	int pot_w = next_pow2( dec->width );
	int pot_h = next_pow2( dec->height );

	if (!C3D_TexInit( &tex, (u16)pot_w, (u16)pot_h, GPU_RGBA8 )) {
		snprintf( errbuf, errbuf_sz, "out of GPU texture memory" );
		return FALSE;
	}
	C3D_TexSetFilter( &tex, GPU_LINEAR, GPU_LINEAR );

	memset( tex.data, 0, (size_t)pot_w * pot_h * 4 ); /* padding stays transparent */
	swizzle_to_texture( (u32 *)tex.data, dec->rgba, dec->width, dec->height, pot_w );
	/* GPU reads texture memory over a separate bus with no cache
	 * coherency with the CPU -- without this, it can see stale data
	 * instead of what was just written (same pattern citro3d's own
	 * tex3ds.c loader uses after writing tiled data into tex->data). */
	GSPGPU_FlushDataCache( tex.data, (u32)((size_t)pot_w * pot_h * 4) );

	/* Conventional citro2d subtexture mapping (top > bottom -- see
	 * Tex3DS_SubTextureRotated(), which a top < bottom would trip):
	 * swizzle_to_texture() puts source row 0 (top of our top-down
	 * decode buffers) at tile_y=0, and empirically (confirmed via RPC
	 * SHOT BOTTOM screenshots showing a real photo right-side up)
	 * tile_y=0 samples at high V, so top=1.0/bottom=1-h/potH is the
	 * correct mapping. */
	subtex.width = (u16)dec->width;
	subtex.height = (u16)dec->height;
	subtex.left = 0.0f;
	subtex.top = 1.0f;
	subtex.right = (float)dec->width / (float)pot_w;
	subtex.bottom = 1.0f - (float)dec->height / (float)pot_h;

	image.tex = &tex;
	image.subtex = &subtex;

	return TRUE;
}


void
imgview_close( void )
{
	if (image_open) {
		C3D_TexDelete( &tex );
		image_open = FALSE;
	}
	img_w = img_h = 0;
}


gboolean
imgview_open( const char *path )
{
	DecodedImage dec;
	gboolean ok;

	imgview_close( );
	error_buf[0] = '\0';
	memset( &dec, 0, sizeof(dec) );

	if (ext_is( path, "bmp" ))
		ok = decode_bmp( path, &dec, error_buf, sizeof(error_buf) );
	else if (ext_is( path, "png" ))
		ok = decode_png( path, &dec, error_buf, sizeof(error_buf) );
	else if (ext_is( path, "jpg" ) || ext_is( path, "jpeg" ))
		ok = decode_jpeg( path, &dec, error_buf, sizeof(error_buf) );
	else {
		snprintf( error_buf, sizeof(error_buf), "unrecognized image extension" );
		return FALSE;
	}

	if (!ok)
		return FALSE;

	ok = upload_texture( &dec, error_buf, sizeof(error_buf) );
	free( dec.rgba );

	if (!ok)
		return FALSE;

	img_w = dec.width;
	img_h = dec.height;
	image_open = TRUE;
	return TRUE;
}


void
imgview_draw( float x, float y, float box_w, float box_h )
{
	float scale;

	if (!image_open)
		return;

	scale = box_w / (float)img_w;
	if (box_h / (float)img_h < scale)
		scale = box_h / (float)img_h;
	if (scale > 1.0f)
		scale = 1.0f; /* never upscale past native res */

	x += (box_w - (float)img_w * scale) * 0.5f;
	y += (box_h - (float)img_h * scale) * 0.5f;

	C2D_DrawImageAt( image, x, y, 0.5f, NULL, scale, scale );
}

/* end imgview.c */
