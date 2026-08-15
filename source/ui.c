/* ui.c - see ui.h */
#include <3ds.h>
#include <citro2d.h>

#include "common.h"
#include "ui.h"
#include "nav.h"
#include "viz.h"
#include "color.h"
#include "settings.h"
#include "rpc.h"

#include <sys/stat.h>
#include <strings.h>
#include "compat/scandir_compat.h"

#define SCREEN_W 320
#define SCREEN_H 240

#define STATUS_BAR_H 24
#define FOOTER_H     26
#define RAIL_W       72
#define RAIL_ROWS    4
#define RAIL_ROW_H   ((SCREEN_H - STATUS_BAR_H - FOOTER_H) / RAIL_ROWS)

#define CONTENT_X (RAIL_W + 6)
#define CONTENT_Y (STATUS_BAR_H + 6)
#define CONTENT_W (SCREEN_W - CONTENT_X - 6)
#define CONTENT_H (SCREEN_H - FOOTER_H - CONTENT_Y)

#define UI_TEXT_SCALE  0.5f
#define UI_LOG_SCALE   0.42f
#define LOG_LINE_H     14

#define COLOR_BG        C2D_Color32( 0x18, 0x18, 0x20, 0xFF )
#define COLOR_RAIL      C2D_Color32( 0x24, 0x24, 0x30, 0xFF )
#define COLOR_RAIL_HOT  C2D_Color32( 0x38, 0x50, 0x78, 0xFF )
#define COLOR_STATUSBAR C2D_Color32( 0x10, 0x10, 0x16, 0xFF )
#define COLOR_FOOTER    C2D_Color32( 0x10, 0x10, 0x16, 0xFF )
#define COLOR_TEXT      C2D_Color32( 0xE8, 0xE8, 0xE8, 0xFF )
#define COLOR_TEXT_DIM  C2D_Color32( 0x90, 0x90, 0x98, 0xFF )
#define COLOR_YELLOW    C2D_Color32( 0xF0, 0xD8, 0x40, 0xFF )
#define COLOR_BATTERY   C2D_Color32( 0x50, 0xD0, 0x60, 0xFF )
#define COLOR_BATT_LOW  C2D_Color32( 0xD0, 0x50, 0x50, 0xFF )
#define COLOR_WIFI      C2D_Color32( 0xE8, 0xE8, 0xE8, 0xFF )
#define COLOR_WIFI_OFF  C2D_Color32( 0x48, 0x48, 0x50, 0xFF )

typedef enum {
	UI_SCREEN_LOG,
	UI_SCREEN_SETTINGS,
	UI_SCREEN_INFO,
	UI_SCREEN_FOLDER_BROWSER,
	UI_SCREEN_TEXT_VIEWER,
	UI_SCREEN_HEX_VIEWER,
	UI_SCREEN_IMAGE_VIEWER
} UiScreen;

static UiScreen screen = UI_SCREEN_LOG;

/* --- Folder browser state: independent of nav.c's tree navigation --
 * this walks the raw filesystem from the SD card root via stat()/
 * fsv3ds_scandir(), same low-level tools scanfs.c uses, so it works
 * even before any scan has happened and can reach anywhere on the
 * card, not just what's under the current scan root. */
#define BROWSE_PATH_LEN     256
#define BROWSE_NAME_LEN     48
#define MAX_BROWSE_ENTRIES  64
#define BROWSE_ROOT         "sdmc:/"

static char browse_path[BROWSE_PATH_LEN] = BROWSE_ROOT;
static char browse_entries[MAX_BROWSE_ENTRIES][BROWSE_NAME_LEN];
static int browse_entry_count = 0;
static int browse_page = 0;

/* TRUE while a scan is pending/in-progress, so ui_draw() shows the
 * "Scanning..." overlay instead of the normal screen -- see
 * ui_request_scan()/ui_process_pending_scan() below. Without this, a
 * big scan (easy to trigger now that the folder browser can point at
 * anything on the card, not just the small sdmc:/3ds default) just
 * freezes the app with no feedback until it's done. */
static gboolean scanning = FALSE;
static char scanning_path[BROWSE_PATH_LEN];

/* --- Text/hex viewer state: opened from the Info screen for the
 * currently selected file. Reads a page at a time straight off the SD
 * card (fseek/fread) rather than loading the whole file into memory --
 * keeps memory bounded regardless of file size and needs no size cap. */
#define VIEWER_PATH_LEN  BROWSE_PATH_LEN
static char viewer_path[VIEWER_PATH_LEN];
static long viewer_file_size = 0;
static long viewer_offset = 0; /* byte offset the current page starts at */

#define TEXT_PAGE_BUF_SIZE 1024
#define TEXT_LINE_H     14
#define TEXT_MAX_LINES  ((CONTENT_H - 36) / TEXT_LINE_H)
static char text_page_buf[TEXT_PAGE_BUF_SIZE];
static int text_page_len = 0;
/* How many of text_page_buf's bytes the TEXT_MAX_LINES actually shown
 * on screen account for -- may be less than text_page_len (a 1024-byte
 * read routinely holds more lines than fit one screen). Paging by
 * text_page_len instead of this would silently skip whatever didn't
 * fit; see text_compute_consumed(). */
static int text_page_consumed = 0;

#define HEX_BYTES_PER_ROW 8
#define HEX_ROWS_PER_PAGE 9 /* 9, not 10 -- see draw_hex_viewer_screen()'s comment */
#define HEX_PAGE_BUF_SIZE (HEX_BYTES_PER_ROW * HEX_ROWS_PER_PAGE)
static unsigned char hex_page_buf[HEX_PAGE_BUF_SIZE];
static int hex_page_len = 0;

static C2D_TextBuf textBuf;
static gboolean ptmu_ok = FALSE;

static const char *rail_labels[RAIL_ROWS] = { "Folder", "Settings", "Info", "Log" };


void
ui_init( void )
{
	/* Sized generously for a full LOG screen (~13 lines x ~80 chars)
	 * plus rail/footer/settings text in the same frame -- matches
	 * render.c's LABEL_TEXTBUF_GLYPHS headroom. */
	textBuf = C2D_TextBufNew( 4096 );
	/* ptmuInit() can fail (e.g. already-initialized by another applet
	 * context in some launch paths) -- battery just won't be shown if
	 * so, rather than treating it as fatal. */
	ptmu_ok = (ptmuInit( ) == 0);
}


void
ui_fini( void )
{
	if (ptmu_ok)
		ptmuExit( );
	C2D_TextBufDelete( textBuf );
}


static void
draw_text( const char *s, float x, float y, float scale, u32 color )
{
	C2D_Text text;

	C2D_TextParse( &text, textBuf, s );
	C2D_TextOptimize( &text );
	C2D_DrawText( &text, C2D_WithColor, x, y, 0.5f, scale, scale, color );
}


static float
text_width( const char *s, float scale )
{
	C2D_Text text;
	float w, h;

	C2D_TextParse( &text, textBuf, s );
	C2D_TextOptimize( &text );
	C2D_TextGetDimensions( &text, scale, scale, &w, &h );
	return w;
}


/* --- status bar: battery (ptmu) + wifi (osGetWifiStrength) --- */

static void
draw_status_bar( void )
{
	int i;

	C2D_DrawRectSolid( 0, 0, 0.5f, SCREEN_W, STATUS_BAR_H, COLOR_STATUSBAR );

	/* Battery: PTMU_GetBatteryLevel() is a raw 0-5 gauge, not a
	 * percentage (libctru has no percentage API without touching the
	 * undocumented MCU/HWC service) -- drawn as 5 segments rather than
	 * inventing a fake percentage number. */
	if (ptmu_ok) {
		u8 level = 5, charging = 0;
		int seg_w = 5, seg_gap = 2, seg_h = 10;
		int x0 = 6, y0 = (STATUS_BAR_H - seg_h) / 2;

		PTMU_GetBatteryLevel( &level );
		PTMU_GetBatteryChargeState( &charging );

		for (i = 0; i < 5; i++) {
			u32 c = (i < level) ? (level <= 1 ? COLOR_BATT_LOW : COLOR_BATTERY) : COLOR_WIFI_OFF;
			C2D_DrawRectSolid( (float)(x0 + i * (seg_w + seg_gap)), (float)y0, 0.5f,
				(float)seg_w, (float)seg_h, c );
		}
		if (charging)
			draw_text( "CHG", (float)(x0 + 5 * (seg_w + seg_gap) + 4), 4.0f, 0.35f, COLOR_TEXT_DIM );
	}

	/* Wifi: osGetWifiStrength() is 0..3 (4 levels), always available
	 * (no init needed) -- mapped to 3 bars of increasing height, lit
	 * up to the current level (0 = none lit, 3 = all lit). */
	{
		u32 strength = osGetWifiStrength( );
		int bar_w = 5, bar_gap = 2, max_h = 12;
		int x0 = SCREEN_W - 6 - 3 * (bar_w + bar_gap);

		for (i = 0; i < 3; i++) {
			int h = 5 + i * ((max_h - 5) / 2);
			u32 c = ((u32)i < strength) ? COLOR_WIFI : COLOR_WIFI_OFF;

			C2D_DrawRectSolid( (float)(x0 + i * (bar_w + bar_gap)),
				(float)(STATUS_BAR_H - 2 - h), 0.5f, (float)bar_w, (float)h, c );
		}
	}
}


/* --- left button rail --- */

static void
draw_rail( void )
{
	int i;

	C2D_DrawRectSolid( 0, STATUS_BAR_H, 0.5f, RAIL_W, SCREEN_H - STATUS_BAR_H - FOOTER_H, COLOR_RAIL );

	for (i = 0; i < RAIL_ROWS; i++) {
		float y = (float)(STATUS_BAR_H + i * RAIL_ROW_H);
		gboolean active = (i == 0 && screen == UI_SCREEN_FOLDER_BROWSER) ||
		                  (i == 1 && screen == UI_SCREEN_SETTINGS) ||
		                  (i == 2 && (screen == UI_SCREEN_INFO || screen == UI_SCREEN_TEXT_VIEWER ||
		                              screen == UI_SCREEN_HEX_VIEWER || screen == UI_SCREEN_IMAGE_VIEWER)) ||
		                  (i == 3 && screen == UI_SCREEN_LOG);
		float tw;

		if (active)
			C2D_DrawRectSolid( 1, y + 1, 0.5f, RAIL_W - 2, RAIL_ROW_H - 2, COLOR_RAIL_HOT );

		tw = text_width( rail_labels[i], UI_TEXT_SCALE );
		draw_text( rail_labels[i], (RAIL_W - tw) * 0.5f, y + RAIL_ROW_H * 0.5f - 8.0f,
			UI_TEXT_SCALE, COLOR_TEXT );
	}
}


/* --- footer breadcrumb --- */

static void
draw_footer( void )
{
	GNode *vr = nav_view_root( );
	GNode *sel = nav_selected_node( );
	float y = (float)(SCREEN_H - FOOTER_H);
	float x = 6.0f;

	C2D_DrawRectSolid( 0, y, 0.5f, SCREEN_W, FOOTER_H, COLOR_FOOTER );

	if (vr == NULL) {
		draw_text( "(no scan yet -- tap Folder or press SELECT)", x, y + 6.0f, 0.38f, COLOR_TEXT_DIM );
		return;
	}

	{
		char path_buf[64];
		const char *full = node_absname( vr );
		size_t len = strlen( full );

		if (len >= sizeof(path_buf)) {
			path_buf[0] = path_buf[1] = path_buf[2] = '.';
			strncpy( path_buf + 3, full + (len - (sizeof(path_buf) - 4)), sizeof(path_buf) - 4 );
			path_buf[sizeof(path_buf) - 1] = '\0';
		}
		else
			strcpy( path_buf, full );

		draw_text( path_buf, x, y + 6.0f, 0.38f, COLOR_TEXT );
		x += text_width( path_buf, 0.38f );
	}

	if (sel != NULL) {
		char sel_buf[48];

		snprintf( sel_buf, sizeof(sel_buf), " > %s%s", NODE_DESC(sel)->name, NODE_IS_DIR(sel) ? "/" : "" );
		draw_text( sel_buf, x, y + 6.0f, 0.38f, COLOR_YELLOW );
	}
}


/* --- LOG screen: tail of rpc.c's log ring buffer --- */

static void
draw_log_screen( void )
{
	size_t len;
	const char *buf = rpc_log_buffer( &len );
	int max_lines = CONTENT_H / LOG_LINE_H;
	int total_lines = 1;
	size_t pos, line_start;
	int skip, drawn;

	if (max_lines < 1) max_lines = 1;

	for (pos = 0; pos < len; pos++) {
		if (buf[pos] == '\n' && pos + 1 < len)
			total_lines++;
	}
	skip = total_lines - max_lines;
	if (skip < 0) skip = 0;

	line_start = 0;
	for (pos = 0; skip > 0 && pos < len; pos++) {
		if (buf[pos] == '\n') {
			skip--;
			line_start = pos + 1;
		}
	}

	drawn = 0;
	pos = line_start;
	while (pos <= len && drawn < max_lines) {
		size_t seg_start = pos;
		size_t seg_end = pos;
		char linebuf[80];
		int linelen;

		while (seg_end < len && buf[seg_end] != '\n')
			seg_end++;

		linelen = (int)(seg_end - seg_start);
		if (linelen > 0 && buf[seg_start + linelen - 1] == '\r')
			linelen--;
		if (linelen >= (int)sizeof(linebuf))
			linelen = sizeof(linebuf) - 1;

		if (linelen > 0) {
			memcpy( linebuf, buf + seg_start, (size_t)linelen );
			linebuf[linelen] = '\0';
			draw_text( linebuf, (float)CONTENT_X, (float)(CONTENT_Y + drawn * LOG_LINE_H),
				UI_LOG_SCALE, COLOR_TEXT );
			drawn++;
		}

		if (seg_end >= len)
			break;
		pos = seg_end + 1;
	}

	if (total_lines == 1 && len == 0)
		draw_text( "(no log output yet)", (float)CONTENT_X, (float)CONTENT_Y, UI_LOG_SCALE, COLOR_TEXT_DIM );
}


/* --- SETTINGS screen: color scheme + label mode, both tap-to-cycle --- */

#define SETTINGS_ROW_H 44

static void
draw_settings_screen( void )
{
	char buf[48];
	float y = (float)CONTENT_Y;

	draw_text( "Color scheme:", (float)CONTENT_X, y, UI_TEXT_SCALE, COLOR_TEXT_DIM );
	snprintf( buf, sizeof(buf), "%s  (tap to change)", color_scheme_name( settings_get_color_scheme( ) ) );
	draw_text( buf, (float)CONTENT_X, y + 18.0f, UI_TEXT_SCALE, COLOR_TEXT );
	y += SETTINGS_ROW_H;

	draw_text( "Node labels:", (float)CONTENT_X, y, UI_TEXT_SCALE, COLOR_TEXT_DIM );
	snprintf( buf, sizeof(buf), "%s  (tap to change)", settings_label_mode_name( settings_get_label_mode( ) ) );
	draw_text( buf, (float)CONTENT_X, y + 18.0f, UI_TEXT_SCALE, COLOR_TEXT );
	y += SETTINGS_ROW_H;

	if (settings_get_label_mode( ) == LABEL_MODE_ALL)
		draw_text( "(may overlap with many nodes on screen)", (float)CONTENT_X, y, 0.35f, COLOR_TEXT_DIM );
	y += SETTINGS_ROW_H;

	/* Informational only -- not tap-to-cycle like the two above (a
	 * folder path isn't something you cycle through). Set via the
	 * Folder screen's "Use this", not from here. */
	draw_text( "Default folder:", (float)CONTENT_X, y, UI_TEXT_SCALE, COLOR_TEXT_DIM );
	{
		const char *root = settings_get_default_root( );
		char pbuf[40];
		size_t len = strlen( root );

		if (len >= sizeof(pbuf)) {
			pbuf[0] = pbuf[1] = pbuf[2] = '.';
			strncpy( pbuf + 3, root + (len - (sizeof(pbuf) - 4)), sizeof(pbuf) - 4 );
			pbuf[sizeof(pbuf) - 1] = '\0';
		}
		else
			strcpy( pbuf, root );

		draw_text( pbuf, (float)CONTENT_X, y + 18.0f, 0.4f, COLOR_TEXT );
	}
}


static void
handle_settings_touch( int x, int y )
{
	int rel_y = y - CONTENT_Y;
	int row;

	(void)x;
	if (rel_y < 0)
		return;
	row = rel_y / SETTINGS_ROW_H;

	if (row == 0)
		settings_cycle_color_scheme( );
	else if (row == 1)
		settings_cycle_label_mode( );
}


/* --- File type detection, for the Info screen's "Open as..." rows --- */

static gboolean
has_ext( const char *name, const char *ext )
{
	size_t nlen = strlen( name );
	size_t elen = strlen( ext );

	if (nlen < elen + 1 || name[nlen - elen - 1] != '.')
		return FALSE;
	return strcasecmp( name + nlen - elen, ext ) == 0;
}


static gboolean
is_text_file( const char *name )
{
	static const char *exts[] = {
		"txt", "md", "cfg", "ini", "json", "xml", "log",
		"c", "h", "cpp", "hpp", "py", "sh", "lua", "gitignore"
	};
	unsigned int i;

	for (i = 0; i < sizeof(exts) / sizeof(exts[0]); i++) {
		if (has_ext( name, exts[i] ))
			return TRUE;
	}
	return FALSE;
}


static gboolean
is_image_file( const char *name )
{
	/* BMP only for now -- PNG/JPEG need a real decoder library this
	 * port doesn't link yet. */
	return has_ext( name, "bmp" );
}


/* --- Text/hex viewer shared plumbing --- */

/* How many bytes of text_page_buf the first `max_lines` lines occupy
 * (including their newlines) -- the amount "Next" should actually
 * advance by, since it's routinely less than the full read. */
static int
text_compute_consumed( int max_lines )
{
	int pos = 0, lines = 0;

	while (pos < text_page_len && lines < max_lines) {
		while (pos < text_page_len && text_page_buf[pos] != '\n')
			pos++;
		if (pos < text_page_len)
			pos++; /* include the newline itself */
		lines++;
	}
	return pos;
}


static void
text_viewer_load_page( void )
{
	FILE *f = fopen( viewer_path, "rb" );

	text_page_len = 0;
	if (f == NULL) {
		text_page_consumed = 0;
		return;
	}
	fseek( f, viewer_offset, SEEK_SET );
	text_page_len = (int)fread( text_page_buf, 1, sizeof(text_page_buf), f );
	fclose( f );

	text_page_consumed = text_compute_consumed( TEXT_MAX_LINES );
}


static void
hex_viewer_load_page( void )
{
	FILE *f = fopen( viewer_path, "rb" );

	hex_page_len = 0;
	if (f == NULL)
		return;
	fseek( f, viewer_offset, SEEK_SET );
	hex_page_len = (int)fread( hex_page_buf, 1, sizeof(hex_page_buf), f );
	fclose( f );
}


static void
text_viewer_open( const char *path )
{
	struct stat st;

	strncpy( viewer_path, path, sizeof(viewer_path) - 1 );
	viewer_path[sizeof(viewer_path) - 1] = '\0';
	viewer_offset = 0;
	viewer_file_size = (stat( path, &st ) == 0) ? (long)st.st_size : 0;
	text_viewer_load_page( );
	screen = UI_SCREEN_TEXT_VIEWER;
}


static void
hex_viewer_open( const char *path )
{
	struct stat st;

	strncpy( viewer_path, path, sizeof(viewer_path) - 1 );
	viewer_path[sizeof(viewer_path) - 1] = '\0';
	viewer_offset = 0;
	viewer_file_size = (stat( path, &st ) == 0) ? (long)st.st_size : 0;
	hex_viewer_load_page( );
	screen = UI_SCREEN_HEX_VIEWER;
}


/* Shared header for both viewers: filename on row 0, "< Prev" /
 * byte-range / "Next >" paging control on row 1. Content starts at
 * VIEWER_LIST_Y. `page_len` is however many bytes the *current*
 * viewer's own page buffer actually holds (text and hex paginate at
 * different granularities, so this isn't shared state). */
#define VIEWER_LIST_Y (CONTENT_Y + 36)

static void
draw_viewer_header( int page_len )
{
	char pbuf[36];
	char rangebuf[24];
	size_t len = strlen( viewer_path );
	float tw;

	if (len >= sizeof(pbuf)) {
		pbuf[0] = pbuf[1] = pbuf[2] = '.';
		strncpy( pbuf + 3, viewer_path + (len - (sizeof(pbuf) - 4)), sizeof(pbuf) - 4 );
		pbuf[sizeof(pbuf) - 1] = '\0';
	}
	else
		strcpy( pbuf, viewer_path );
	draw_text( pbuf, (float)CONTENT_X, (float)CONTENT_Y, 0.36f, COLOR_TEXT );

	if (viewer_offset > 0)
		draw_text( "< Prev", (float)CONTENT_X, (float)(CONTENT_Y + 18), 0.34f, COLOR_YELLOW );

	snprintf( rangebuf, sizeof(rangebuf), "%ld-%ld/%ld",
		viewer_offset, viewer_offset + (long)page_len, viewer_file_size );
	tw = text_width( rangebuf, 0.3f );
	draw_text( rangebuf, (float)(CONTENT_X + (CONTENT_W - tw) * 0.5f), (float)(CONTENT_Y + 19), 0.3f, COLOR_TEXT_DIM );

	if (viewer_offset + (long)page_len < viewer_file_size) {
		tw = text_width( "Next >", 0.34f );
		draw_text( "Next >", (float)(CONTENT_X + CONTENT_W - tw), (float)(CONTENT_Y + 18), 0.34f, COLOR_YELLOW );
	}
}


/* Returns TRUE (and pages) if the tap landed on the header's Prev/Next
 * row -- shared hit-test for both viewers since the header layout is
 * identical. `page_len`/`page_size`/reload let each viewer supply its
 * own page-advance amount and reload function. */
static gboolean
handle_viewer_header_touch( int x, int y, int page_len, int page_size, void (*reload)( void ) )
{
	if (y - CONTENT_Y < 18 || y - CONTENT_Y >= 36)
		return FALSE;

	if (x < CONTENT_X + CONTENT_W / 2) {
		if (viewer_offset > 0) {
			viewer_offset -= page_size;
			if (viewer_offset < 0)
				viewer_offset = 0;
			reload( );
		}
	}
	else {
		if (viewer_offset + page_len < viewer_file_size) {
			viewer_offset += page_len;
			reload( );
		}
	}
	return TRUE;
}


/* --- TEXT viewer --- */

static void
draw_text_viewer_screen( void )
{
	float y = (float)VIEWER_LIST_Y;
	int pos = 0, drawn = 0;

	draw_viewer_header( text_page_consumed );

	if (text_page_len == 0) {
		draw_text( "(empty or unreadable)", (float)CONTENT_X, y, 0.36f, COLOR_TEXT_DIM );
		return;
	}

	while (pos < text_page_len && drawn < TEXT_MAX_LINES) {
		int start = pos;
		char linebuf[70];
		int linelen;

		while (pos < text_page_len && text_page_buf[pos] != '\n')
			pos++;

		linelen = pos - start;
		if (linelen > 0 && text_page_buf[start + linelen - 1] == '\r')
			linelen--;
		if (linelen >= (int)sizeof(linebuf))
			linelen = sizeof(linebuf) - 1;

		if (linelen > 0) {
			memcpy( linebuf, text_page_buf + start, (size_t)linelen );
			linebuf[linelen] = '\0';
			draw_text( linebuf, (float)CONTENT_X, y, 0.36f, COLOR_TEXT );
		}
		y += TEXT_LINE_H;
		drawn++;

		if (pos < text_page_len)
			pos++; /* skip the newline itself */
	}
}


static void
handle_text_viewer_touch( int x, int y )
{
	/* Advances by text_page_consumed (what's actually shown), not the
	 * full 1024-byte read -- see its declaration for why. */
	handle_viewer_header_touch( x, y, text_page_consumed, TEXT_PAGE_BUF_SIZE, text_viewer_load_page );
}


/* --- HEX viewer -- works on any file, not just recognized types ---
 *
 * HEX_ROWS_PER_PAGE * HEX_ROW_H must stay <= CONTENT_H - 36 (the
 * space below the header) or the last row draws into the footer's
 * territory -- 9 rows * 16px = 144, comfortably under 184 - 36 = 148. */

#define HEX_ROW_H 16

static void
draw_hex_viewer_screen( void )
{
	float y = (float)VIEWER_LIST_Y;
	int row, col;

	draw_viewer_header( hex_page_len );

	if (hex_page_len == 0) {
		draw_text( "(empty or unreadable)", (float)CONTENT_X, y, 0.36f, COLOR_TEXT_DIM );
		return;
	}

	for (row = 0; row * HEX_BYTES_PER_ROW < hex_page_len; row++) {
		char linebuf[64];
		int off = 0;
		int n = row * HEX_BYTES_PER_ROW;

		off += snprintf( linebuf + off, sizeof(linebuf) - off, "%06lx  ", (unsigned long)(viewer_offset + n) );
		for (col = 0; col < HEX_BYTES_PER_ROW; col++) {
			if (n + col < hex_page_len)
				off += snprintf( linebuf + off, sizeof(linebuf) - off, "%02x ", hex_page_buf[n + col] );
			else
				off += snprintf( linebuf + off, sizeof(linebuf) - off, "   " );
		}
		off += snprintf( linebuf + off, sizeof(linebuf) - off, " " );
		for (col = 0; col < HEX_BYTES_PER_ROW && n + col < hex_page_len; col++) {
			unsigned char c = hex_page_buf[n + col];

			if (off < (int)sizeof(linebuf) - 1)
				linebuf[off++] = (c >= 0x20 && c < 0x7f) ? (char)c : '.';
		}
		if (off < (int)sizeof(linebuf))
			linebuf[off] = '\0';
		else
			linebuf[sizeof(linebuf) - 1] = '\0';

		draw_text( linebuf, (float)CONTENT_X, y, 0.32f, COLOR_TEXT );
		y += HEX_ROW_H;
	}
}


static void
handle_hex_viewer_touch( int x, int y )
{
	handle_viewer_header_touch( x, y, hex_page_len, HEX_PAGE_BUF_SIZE, hex_viewer_load_page );
}


/* --- INFO screen: details for the currently selected node --- */

static void
format_time( time_t t, char *out, size_t outsz )
{
	struct tm *tm;

	/* The 3DS's sdmc FAT driver doesn't reliably populate st_mtime
	 * (observed returning exactly 0 for files that plainly aren't from
	 * 1970) -- showing the literal epoch date would read as a display
	 * bug rather than a filesystem limitation, so call it out instead. */
	if (t == 0) {
		snprintf( out, outsz, "unknown" );
		return;
	}

	tm = localtime( &t );
	if (tm == NULL) {
		snprintf( out, outsz, "unknown" );
		return;
	}
	strftime( out, outsz, "%Y-%m-%d %H:%M", tm );
}


static void
draw_info_screen( void )
{
	GNode *sel = nav_selected_node( );
	char buf[80];
	float y = (float)CONTENT_Y;
	const float line_h = 18.0f;

	if (sel == NULL) {
		draw_text( "(nothing selected)", (float)CONTENT_X, y, UI_TEXT_SCALE, COLOR_TEXT_DIM );
		return;
	}

	snprintf( buf, sizeof(buf), "%s%s", NODE_DESC(sel)->name, NODE_IS_DIR(sel) ? "/" : "" );
	draw_text( buf, (float)CONTENT_X, y, UI_TEXT_SCALE, COLOR_TEXT );
	y += line_h;

	snprintf( buf, sizeof(buf), "%s", node_type_names[NODE_DESC(sel)->type] );
	draw_text( buf, (float)CONTENT_X, y, 0.4f, COLOR_TEXT_DIM );
	y += line_h;

	snprintf( buf, sizeof(buf), "size: %s", abbrev_size( NODE_DESC(sel)->size ) );
	draw_text( buf, (float)CONTENT_X, y, 0.4f, COLOR_TEXT );
	y += line_h;

	snprintf( buf, sizeof(buf), "on disk: %s", abbrev_size( NODE_DESC(sel)->size_alloc ) );
	draw_text( buf, (float)CONTENT_X, y, 0.4f, COLOR_TEXT );
	y += line_h;

	{
		char tbuf[24];

		format_time( NODE_DESC(sel)->mtime, tbuf, sizeof(tbuf) );
		snprintf( buf, sizeof(buf), "modified: %s", tbuf );
		draw_text( buf, (float)CONTENT_X, y, 0.4f, COLOR_TEXT );
		y += line_h;
	}

	{
		const char *path = node_absname( sel );
		char pbuf[64];
		size_t len = strlen( path );

		if (len >= sizeof(pbuf)) {
			pbuf[0] = pbuf[1] = pbuf[2] = '.';
			strncpy( pbuf + 3, path + (len - (sizeof(pbuf) - 4)), sizeof(pbuf) - 4 );
			pbuf[sizeof(pbuf) - 1] = '\0';
		}
		else
			strcpy( pbuf, path );

		draw_text( pbuf, (float)CONTENT_X, y, 0.35f, COLOR_TEXT_DIM );
	}
	y += line_h;

	if (NODE_IS_DIR(sel))
		return; /* no "open as" actions for directories */

	if (is_text_file( NODE_DESC(sel)->name )) {
		draw_text( "Open as text", (float)CONTENT_X, y, UI_TEXT_SCALE, COLOR_YELLOW );
		y += line_h;
	}
	if (is_image_file( NODE_DESC(sel)->name )) {
		draw_text( "View image", (float)CONTENT_X, y, UI_TEXT_SCALE, COLOR_YELLOW );
		y += line_h;
	}
	draw_text( "Open as hex", (float)CONTENT_X, y, UI_TEXT_SCALE, COLOR_YELLOW );
}


/* Mirrors draw_info_screen()'s layout exactly: 6 fixed 18px detail
 * rows (name/type/size/on-disk/modified/path), then whichever "open
 * as" rows apply, in the same order they're drawn -- if one changes,
 * the other must too, or a tap lands on the wrong action. */
static void
handle_info_touch( int x, int y )
{
	GNode *sel = nav_selected_node( );
	float row_y = (float)CONTENT_Y + 6.0f * 18.0f;

	(void)x;
	if (sel == NULL || NODE_IS_DIR(sel))
		return;

	if (is_text_file( NODE_DESC(sel)->name )) {
		if ((float)y >= row_y && (float)y < row_y + 18.0f) {
			text_viewer_open( node_absname( sel ) );
			return;
		}
		row_y += 18.0f;
	}
	if (is_image_file( NODE_DESC(sel)->name )) {
		if ((float)y >= row_y && (float)y < row_y + 18.0f) {
			/* image_viewer_open() -- added alongside the BMP decoder */
			return;
		}
		row_y += 18.0f;
	}
	if ((float)y >= row_y && (float)y < row_y + 18.0f)
		hex_viewer_open( node_absname( sel ) );
}


/* --- Folder browser: pick a base scan folder from the SD card root --- */

#define BROWSE_LIST_Y        (CONTENT_Y + 22)
#define BROWSE_ROW_H         20
#define BROWSE_VISIBLE_ROWS  ((CONTENT_H - 22) / BROWSE_ROW_H)

/* Same "." / ".." filter as scanfs.c's de_select() -- kept as its own
 * copy since that one's static to scanfs.c and this doesn't need the
 * rest of scanfs.c's tree-building machinery, just a directory listing. */
static int
browse_de_select( const struct dirent *de )
{
	if (de->d_name[0] != '.')
		return 1;
	if (de->d_name[1] == '\0')
		return 0;
	if (de->d_name[1] != '.')
		return 1;
	if (de->d_name[2] == '\0')
		return 0;
	return 1;
}


/* Re-lists browse_path's subdirectories (files excluded -- this is a
 * folder picker, not a full file browser). Regular readdir() doesn't
 * report reliable d_type on this platform, so each candidate gets a
 * real stat() to confirm it's a directory, same as scanfs.c's own
 * stat_node(). */
static void
refresh_browse_listing( void )
{
	struct dirent **dir_entries;
	int num_entries, i;
	size_t plen = strlen( browse_path );

	browse_entry_count = 0;
	browse_page = 0;

	num_entries = fsv3ds_scandir( browse_path, &dir_entries, browse_de_select, fsv3ds_alphasort );
	if (num_entries < 0)
		return;

	for (i = 0; i < num_entries; i++) {
		if (browse_entry_count < MAX_BROWSE_ENTRIES) {
			char full[BROWSE_PATH_LEN];
			struct stat st;

			snprintf( full, sizeof(full), "%s%s%s", browse_path,
				(plen > 0 && browse_path[plen - 1] == '/') ? "" : "/",
				dir_entries[i]->d_name );

			if (stat( full, &st ) == 0 && S_ISDIR(st.st_mode)) {
				strncpy( browse_entries[browse_entry_count], dir_entries[i]->d_name, BROWSE_NAME_LEN - 1 );
				browse_entries[browse_entry_count][BROWSE_NAME_LEN - 1] = '\0';
				browse_entry_count++;
			}
		}
		free( dir_entries[i] );
	}
	free( dir_entries );
}


static void
browse_open( void )
{
	strcpy( browse_path, BROWSE_ROOT );
	refresh_browse_listing( );
	screen = UI_SCREEN_FOLDER_BROWSER;
}


static void
browse_go_up( void )
{
	char *slash = strrchr( browse_path, '/' );

	if (slash == NULL)
		return;
	/* "sdmc:/" itself -- keep the root slash rather than leaving a
	 * bare "sdmc:" that nothing downstream expects. */
	if (slash == browse_path + 5)
		slash[1] = '\0';
	else
		slash[0] = '\0';
	refresh_browse_listing( );
}


static void
browse_descend( const char *name )
{
	size_t plen = strlen( browse_path );

	if (plen + strlen( name ) + 2 >= sizeof(browse_path))
		return; /* pathologically deep -- just refuse rather than truncate silently */

	if (plen > 0 && browse_path[plen - 1] != '/')
		strcat( browse_path, "/" );
	strcat( browse_path, name );
	refresh_browse_listing( );
}


static void
action_use_browse_folder( void )
{
	settings_set_default_root( browse_path );
	ui_request_scan( browse_path );
}


static void
draw_folder_browser_screen( void )
{
	char pbuf[36];
	float y;
	gboolean has_up;
	int visible, start, shown, i;

	{
		size_t len = strlen( browse_path );

		if (len >= sizeof(pbuf)) {
			pbuf[0] = pbuf[1] = pbuf[2] = '.';
			strncpy( pbuf + 3, browse_path + (len - (sizeof(pbuf) - 4)), sizeof(pbuf) - 4 );
			pbuf[sizeof(pbuf) - 1] = '\0';
		}
		else
			strcpy( pbuf, browse_path );
	}
	draw_text( pbuf, (float)CONTENT_X, (float)CONTENT_Y, 0.36f, COLOR_TEXT );
	draw_text( "Use this", (float)(CONTENT_X + CONTENT_W - 62), (float)CONTENT_Y, 0.36f, COLOR_YELLOW );

	y = (float)BROWSE_LIST_Y;
	has_up = (strcmp( browse_path, BROWSE_ROOT ) != 0);
	if (has_up) {
		draw_text( ".. (up)", (float)CONTENT_X, y, 0.38f, COLOR_TEXT_DIM );
		y += BROWSE_ROW_H;
	}

	/* -1 unconditionally reserves a row for the "More" indicator below
	 * -- without it, a full page of real entries (common: SD card roots
	 * routinely have 10+ top-level folders) leaves "More" with nowhere
	 * to draw but the footer's territory. Wastes one row of headroom
	 * when there's nothing to page through, which is a fine trade. */
	visible = BROWSE_VISIBLE_ROWS - (has_up ? 1 : 0) - 1;
	if (visible < 1)
		visible = 1;
	start = browse_page * visible;
	shown = browse_entry_count - start;
	if (shown > visible) shown = visible;
	if (shown < 0) shown = 0;

	for (i = 0; i < shown; i++) {
		draw_text( browse_entries[start + i], (float)CONTENT_X, y, 0.38f, COLOR_TEXT );
		y += BROWSE_ROW_H;
	}

	if (browse_entry_count == 0)
		draw_text( "(no subfolders)", (float)CONTENT_X, y, 0.36f, COLOR_TEXT_DIM );
	else if (browse_entry_count > visible)
		draw_text( "More >", (float)CONTENT_X, y, 0.34f, COLOR_TEXT_DIM );
}


static void
handle_folder_browser_touch( int x, int y )
{
	int rel_y = y - CONTENT_Y;
	gboolean has_up;
	int list_row, visible, start, shown;

	if (rel_y < 22) {
		if (x >= CONTENT_X + CONTENT_W - 70)
			action_use_browse_folder( );
		return;
	}

	rel_y = y - BROWSE_LIST_Y;
	if (rel_y < 0)
		return;

	has_up = (strcmp( browse_path, BROWSE_ROOT ) != 0);
	list_row = rel_y / BROWSE_ROW_H;

	if (has_up) {
		if (list_row == 0) {
			browse_go_up( );
			return;
		}
		list_row--;
	}

	/* Must match draw_folder_browser_screen()'s reservation exactly, or
	 * a tap on what looks like the "More" row would silently land on a
	 * folder entry (or vice versa). */
	visible = BROWSE_VISIBLE_ROWS - (has_up ? 1 : 0) - 1;
	if (visible < 1)
		visible = 1;
	start = browse_page * visible;
	shown = browse_entry_count - start;
	if (shown > visible) shown = visible;
	if (shown < 0) shown = 0;

	if (list_row < shown) {
		browse_descend( browse_entries[start + list_row] );
	}
	else if (list_row == shown && browse_entry_count > visible) {
		browse_page++;
		if (browse_page * visible >= browse_entry_count)
			browse_page = 0; /* wrap back to the first page */
	}
}


static void
draw_scanning_overlay( void )
{
	char pbuf[36];
	size_t len = strlen( scanning_path );

	if (len >= sizeof(pbuf)) {
		pbuf[0] = pbuf[1] = pbuf[2] = '.';
		strncpy( pbuf + 3, scanning_path + (len - (sizeof(pbuf) - 4)), sizeof(pbuf) - 4 );
		pbuf[sizeof(pbuf) - 1] = '\0';
	}
	else
		strcpy( pbuf, scanning_path );

	draw_text( "Scanning...", (float)CONTENT_X, (float)CONTENT_Y, UI_TEXT_SCALE, COLOR_TEXT );
	draw_text( pbuf, (float)CONTENT_X, (float)(CONTENT_Y + 20.0f), 0.36f, COLOR_TEXT_DIM );
}


/* Shows a one-frame "Scanning..." overlay, then runs the blocking
 * scanfs() walk (via viz_scan_and_build()) that would otherwise freeze
 * the app with no feedback until it's done. Shared by main.c's SELECT
 * handler and the Folder screen's "Use this" (RPC's SCAN command
 * deliberately does NOT use this -- see rpc.c's comment).
 *
 * Deferred/2-phase rather than calling render_frame() directly here:
 * an earlier version called render_frame() itself (to present the
 * overlay) before the blocking scanfs() call, on top of the main
 * loop's own once-per-iteration render_frame() -- multiple citro3d
 * frame cycles stacked within one call chain, confirmed via a Luma3DS
 * crash dump to overflow libctru's GPU command buffer
 * (GPUCMD_AddInternal -> svcBreak panic) on a large enough scene. The
 * main loop already calls render_frame() exactly once per iteration;
 * this just rides that instead of adding more. ui_process_pending_scan()
 * (called right after that single render_frame(), see main.c) lets the
 * overlay actually get presented for a frame before doing the real
 * work one iteration later. */
static gboolean scan_pending = FALSE;
static gboolean scan_armed = FALSE; /* TRUE once the overlay has been through one render_frame() */
static char pending_scan_path[VIEWER_PATH_LEN];

void
ui_request_scan( const char *path )
{
	strncpy( pending_scan_path, path, sizeof(pending_scan_path) - 1 );
	pending_scan_path[sizeof(pending_scan_path) - 1] = '\0';
	strncpy( scanning_path, path, sizeof(scanning_path) - 1 );
	scanning_path[sizeof(scanning_path) - 1] = '\0';

	scanning = TRUE;
	scan_pending = TRUE;
	scan_armed = FALSE;
}


void
ui_process_pending_scan( void )
{
	if (!scan_pending)
		return;

	if (!scan_armed) {
		/* This iteration's render_frame() (already called by the time
		 * main.c reaches this) just presented the overlay for the
		 * first time -- do the actual blocking work next iteration. */
		scan_armed = TRUE;
		return;
	}

	scanning = FALSE;
	scan_pending = FALSE;
	scan_armed = FALSE;

	rpc_logf( "scanning %s ...\n", pending_scan_path );
	viz_scan_and_build( pending_scan_path );
	screen = UI_SCREEN_LOG;
}


void
ui_draw( C3D_RenderTarget *target )
{
	C2D_SceneBegin( target );
	C2D_Prepare( );
	C2D_TextBufClear( textBuf );

	C2D_DrawRectSolid( 0, 0, 0.4f, SCREEN_W, SCREEN_H, COLOR_BG );

	draw_status_bar( );
	draw_rail( );

	if (scanning) {
		draw_scanning_overlay( );
		draw_footer( );
		C2D_Flush( );
		return;
	}

	switch (screen) {
	case UI_SCREEN_SETTINGS:        draw_settings_screen( );        break;
	case UI_SCREEN_INFO:            draw_info_screen( );            break;
	case UI_SCREEN_FOLDER_BROWSER:  draw_folder_browser_screen( );  break;
	case UI_SCREEN_TEXT_VIEWER:     draw_text_viewer_screen( );     break;
	case UI_SCREEN_HEX_VIEWER:      draw_hex_viewer_screen( );      break;
	case UI_SCREEN_LOG:
	default:                        draw_log_screen( );             break;
	}

	draw_footer( );

	C2D_Flush( );
}


void
ui_handle_touch( int x, int y )
{
	if (x < 0 || x >= SCREEN_W || y < 0 || y >= SCREEN_H)
		return;

	if (y < STATUS_BAR_H || y >= SCREEN_H - FOOTER_H)
		return; /* status bar / footer: no touch targets */

	if (x < RAIL_W) {
		int row = (y - STATUS_BAR_H) / RAIL_ROW_H;

		switch (row) {
		case 0: browse_open( ); break;
		case 1: screen = UI_SCREEN_SETTINGS; break;
		case 2: screen = UI_SCREEN_INFO; break;
		case 3: screen = UI_SCREEN_LOG; break;
		default: break;
		}
		return;
	}

	if (screen == UI_SCREEN_SETTINGS)
		handle_settings_touch( x, y );
	else if (screen == UI_SCREEN_FOLDER_BROWSER)
		handle_folder_browser_touch( x, y );
	else if (screen == UI_SCREEN_INFO)
		handle_info_touch( x, y );
	else if (screen == UI_SCREEN_TEXT_VIEWER)
		handle_text_viewer_touch( x, y );
	else if (screen == UI_SCREEN_HEX_VIEWER)
		handle_hex_viewer_touch( x, y );
}

/* end ui.c */
