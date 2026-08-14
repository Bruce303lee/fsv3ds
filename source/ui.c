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
	UI_SCREEN_FOLDER_BROWSER
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
		                  (i == 2 && screen == UI_SCREEN_INFO) ||
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

	if (settings_get_label_mode( ) == LABEL_MODE_ALL) {
		y += SETTINGS_ROW_H;
		draw_text( "(may overlap with many nodes on screen)", (float)CONTENT_X, y, 0.35f, COLOR_TEXT_DIM );
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
	rpc_logf( "ui: loading folder %s\n", browse_path );
	viz_scan_and_build( browse_path );
	screen = UI_SCREEN_LOG;
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


void
ui_draw( C3D_RenderTarget *target )
{
	C2D_SceneBegin( target );
	C2D_Prepare( );
	C2D_TextBufClear( textBuf );

	C2D_DrawRectSolid( 0, 0, 0.4f, SCREEN_W, SCREEN_H, COLOR_BG );

	draw_status_bar( );
	draw_rail( );

	switch (screen) {
	case UI_SCREEN_SETTINGS:        draw_settings_screen( );        break;
	case UI_SCREEN_INFO:            draw_info_screen( );            break;
	case UI_SCREEN_FOLDER_BROWSER:  draw_folder_browser_screen( );  break;
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
}

/* end ui.c */
