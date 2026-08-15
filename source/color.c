/* color.c - see color.h
 *
 * Extension-based file coloring, replacing mapv.c's old flat
 * per-NodeType palette for regular files (directories and other
 * special types still get a flat per-type color -- see
 * nodetype_categories[] below, matching upstream's "directory
 * override").
 *
 * Patterns are matched in order, first match wins, via fnmatch()
 * (devkitARM/newlib has it, unlike scandir()/alphasort() back in
 * Phase 1 -- no compat shim needed here). Case-insensitive
 * (FNM_CASEFOLD) since FAT/SD card extensions show up in any case --
 * that flag is a GNU extension newlib gates behind __GNU_VISIBLE,
 * which devkitARM's default -std doesn't set, so _GNU_SOURCE has to be
 * defined before fnmatch.h (transitively via common.h) is first seen.
 *
 * Phase 6: colors are no longer literal RGBcolor values baked into
 * ext_colors[]/nodetype_categories[] -- both now reference a
 * ColorCategory, and the actual RGB for a category is looked up out of
 * whichever scheme's row of color_schemes[][] is active. This is what
 * lets the Settings screen (ui.c) offer a scheme picker without the
 * pattern-matching logic itself knowing or caring.
 */
#define _GNU_SOURCE
#include "common.h"
#include "color.h"

#include <fnmatch.h>

typedef enum {
	COLOR_CAT_DIRECTORY,
	COLOR_CAT_REGFILE_FALLBACK, /* shouldn't normally show -- see match_color() */
	COLOR_CAT_SYMLINK,
	COLOR_CAT_FIFO,
	COLOR_CAT_SOCKET,
	COLOR_CAT_CHARDEV,
	COLOR_CAT_BLOCKDEV,
	COLOR_CAT_UNKNOWN,
	COLOR_CAT_ROM,
	COLOR_CAT_HOMEBREW,
	COLOR_CAT_3DSX, /* .3dsx specifically -- split out of COLOR_CAT_HOMEBREW
	                  * on request, light blue, so the directly-launchable
	                  * files stand out from installables (.cia) and other
	                  * homebrew-adjacent files (.cxi/.cci/.smdh) that stay
	                  * gold */
	COLOR_CAT_CIA,  /* .cia specifically -- same idea as COLOR_CAT_3DSX,
	                  * split out on request so installable packages read
	                  * as their own thing rather than either "generic
	                  * homebrew" gold or .3dsx's light blue */
	COLOR_CAT_ARCHIVE,
	COLOR_CAT_IMAGE,
	COLOR_CAT_AUDIO_VIDEO,
	COLOR_CAT_TEXT,
	COLOR_CAT_DEFAULT_FILE,
	NUM_COLOR_CATEGORIES
} ColorCategory;

/* Index by NodeType (NODE_METANODE's entry is unused). */
static const ColorCategory nodetype_categories[NUM_NODE_TYPES] = {
	COLOR_CAT_DIRECTORY,          /* Metanode (not used) */
	COLOR_CAT_DIRECTORY,          /* Directory */
	COLOR_CAT_REGFILE_FALLBACK,   /* Regular file fallback */
	COLOR_CAT_SYMLINK,
	COLOR_CAT_FIFO,
	COLOR_CAT_SOCKET,
	COLOR_CAT_CHARDEV,
	COLOR_CAT_BLOCKDEV,
	COLOR_CAT_UNKNOWN
};

typedef struct {
	const char *pattern;
	ColorCategory category;
} ExtColor;

/* clang-format off */
static const ExtColor ext_colors[] = {
	/* ROMs / disc images -- this is a homebrew/emulation SD card in
	 * practice, so these get priority over more "generic" categories
	 * below where extensions collide (e.g. Genesis' *.gen). */
	{ "*.nes",  COLOR_CAT_ROM },
	{ "*.sfc",  COLOR_CAT_ROM },
	{ "*.smc",  COLOR_CAT_ROM },
	{ "*.gba",  COLOR_CAT_ROM },
	{ "*.gb",   COLOR_CAT_ROM },
	{ "*.gbc",  COLOR_CAT_ROM },
	{ "*.nds",  COLOR_CAT_ROM },
	{ "*.n64",  COLOR_CAT_ROM },
	{ "*.z64",  COLOR_CAT_ROM },
	{ "*.gen",  COLOR_CAT_ROM },
	{ "*.smd",  COLOR_CAT_ROM },
	{ "*.pce",  COLOR_CAT_ROM },
	{ "*.ws",   COLOR_CAT_ROM },
	{ "*.wsc",  COLOR_CAT_ROM },
	{ "*.iso",  COLOR_CAT_ROM },
	{ "*.cso",  COLOR_CAT_ROM },
	{ "*.chd",  COLOR_CAT_ROM },
	{ "*.bin",  COLOR_CAT_ROM }, /* generic elsewhere, but common for
	                              * N64/other ROMs -- confirmed on
	                              * hardware, DaedalusX64's Roms folder
	                              * is full of *.bin */

	/* 3DS homebrew/installables */
	{ "*.3dsx", COLOR_CAT_3DSX },
	{ "*.cia",  COLOR_CAT_CIA },
	{ "*.cxi",  COLOR_CAT_HOMEBREW },
	{ "*.cci",  COLOR_CAT_HOMEBREW },
	{ "*.3ds",  COLOR_CAT_HOMEBREW },
	{ "*.smdh", COLOR_CAT_HOMEBREW },

	/* Archives */
	{ "*.zip",  COLOR_CAT_ARCHIVE },
	{ "*.7z",   COLOR_CAT_ARCHIVE },
	{ "*.rar",  COLOR_CAT_ARCHIVE },
	{ "*.tar",  COLOR_CAT_ARCHIVE },
	{ "*.gz",   COLOR_CAT_ARCHIVE },
	{ "*.bz2",  COLOR_CAT_ARCHIVE },
	{ "*.xz",   COLOR_CAT_ARCHIVE },
	{ "*.zst",  COLOR_CAT_ARCHIVE },

	/* Images */
	{ "*.png",  COLOR_CAT_IMAGE },
	{ "*.jpg",  COLOR_CAT_IMAGE },
	{ "*.jpeg", COLOR_CAT_IMAGE },
	{ "*.gif",  COLOR_CAT_IMAGE },
	{ "*.bmp",  COLOR_CAT_IMAGE },
	{ "*.tga",  COLOR_CAT_IMAGE },

	/* Audio/video */
	{ "*.mp3",  COLOR_CAT_AUDIO_VIDEO },
	{ "*.ogg",  COLOR_CAT_AUDIO_VIDEO },
	{ "*.wav",  COLOR_CAT_AUDIO_VIDEO },
	{ "*.flac", COLOR_CAT_AUDIO_VIDEO },
	{ "*.mp4",  COLOR_CAT_AUDIO_VIDEO },
	{ "*.avi",  COLOR_CAT_AUDIO_VIDEO },
	{ "*.mkv",  COLOR_CAT_AUDIO_VIDEO },
	{ "*.mov",  COLOR_CAT_AUDIO_VIDEO },

	/* Text/config/code */
	{ "*.txt",  COLOR_CAT_TEXT },
	{ "*.md",   COLOR_CAT_TEXT },
	{ "*.cfg",  COLOR_CAT_TEXT },
	{ "*.ini",  COLOR_CAT_TEXT },
	{ "*.json", COLOR_CAT_TEXT },
	{ "*.xml",  COLOR_CAT_TEXT },
	{ "*.log",  COLOR_CAT_TEXT },
	{ "*.c",    COLOR_CAT_TEXT },
	{ "*.h",    COLOR_CAT_TEXT }
};

/* One row per ColorScheme (see color.h), one column per ColorCategory.
 * DEFAULT is the original hand-picked palette from Phase 4.
 * HIGH_CONTRAST pushes every category toward a maximally-saturated,
 * well-separated hue (a few categories that basically never occur
 * together on a real FAT SD card, e.g. CHARDEV/ARCHIVE, are allowed to
 * share a hue -- there's no dead space left once directories, ROMs,
 * homebrew, and the common file categories are all pulled apart).
 * MONOCHROME drops hue entirely and encodes "type" as brightness only
 * (directories/homebrew/ROMs brightest, generic files dimmest). */
static const RGBcolor color_schemes[NUM_COLOR_SCHEMES][NUM_COLOR_CATEGORIES] = {
	[COLOR_SCHEME_DEFAULT] = {
		[COLOR_CAT_DIRECTORY]        = { 0.85, 0.70, 0.30 }, /* tan */
		[COLOR_CAT_REGFILE_FALLBACK] = { 0.75, 0.75, 0.68 },
		[COLOR_CAT_SYMLINK]          = { 0.55, 0.85, 0.55 },
		[COLOR_CAT_FIFO]             = { 0.85, 0.55, 0.85 },
		[COLOR_CAT_SOCKET]           = { 0.85, 0.85, 0.45 },
		[COLOR_CAT_CHARDEV]          = { 0.85, 0.45, 0.45 },
		[COLOR_CAT_BLOCKDEV]         = { 0.65, 0.45, 0.85 },
		[COLOR_CAT_UNKNOWN]          = { 0.60, 0.60, 0.60 },
		[COLOR_CAT_ROM]              = { 0.35, 0.85, 0.35 }, /* green */
		[COLOR_CAT_HOMEBREW]         = { 0.95, 0.75, 0.25 }, /* gold */
		[COLOR_CAT_3DSX]             = { 0.45, 0.75, 1.00 }, /* light blue */
		[COLOR_CAT_CIA]              = { 0.95, 0.55, 0.15 }, /* orange */
		[COLOR_CAT_ARCHIVE]          = { 0.85, 0.30, 0.30 }, /* red */
		[COLOR_CAT_IMAGE]            = { 0.85, 0.35, 0.85 }, /* magenta */
		[COLOR_CAT_AUDIO_VIDEO]      = { 0.35, 0.85, 0.85 }, /* cyan */
		[COLOR_CAT_TEXT]             = { 0.80, 0.80, 0.80 }, /* grey */
		[COLOR_CAT_DEFAULT_FILE]     = { 0.35, 0.55, 0.85 }, /* blue */
	},
	[COLOR_SCHEME_HIGH_CONTRAST] = {
		[COLOR_CAT_DIRECTORY]        = { 1.00, 0.85, 0.00 },
		[COLOR_CAT_REGFILE_FALLBACK] = { 1.00, 1.00, 1.00 },
		[COLOR_CAT_SYMLINK]          = { 0.00, 1.00, 0.40 },
		[COLOR_CAT_FIFO]             = { 1.00, 0.00, 1.00 },
		[COLOR_CAT_SOCKET]           = { 1.00, 1.00, 0.00 },
		[COLOR_CAT_CHARDEV]          = { 1.00, 0.00, 0.00 },
		[COLOR_CAT_BLOCKDEV]         = { 0.60, 0.00, 1.00 },
		[COLOR_CAT_UNKNOWN]          = { 0.80, 0.80, 0.80 },
		[COLOR_CAT_ROM]              = { 0.00, 1.00, 0.00 },
		[COLOR_CAT_HOMEBREW]         = { 1.00, 0.60, 0.00 },
		[COLOR_CAT_3DSX]             = { 0.30, 0.80, 1.00 },
		[COLOR_CAT_CIA]              = { 1.00, 0.45, 0.00 },
		[COLOR_CAT_ARCHIVE]          = { 1.00, 0.00, 0.00 },
		[COLOR_CAT_IMAGE]            = { 1.00, 0.00, 1.00 },
		[COLOR_CAT_AUDIO_VIDEO]      = { 0.00, 1.00, 1.00 },
		[COLOR_CAT_TEXT]             = { 0.70, 0.90, 1.00 },
		[COLOR_CAT_DEFAULT_FILE]     = { 0.20, 0.50, 1.00 },
	},
	[COLOR_SCHEME_MONOCHROME] = {
		[COLOR_CAT_DIRECTORY]        = { 0.95, 0.95, 0.95 },
		[COLOR_CAT_REGFILE_FALLBACK] = { 0.55, 0.55, 0.55 },
		[COLOR_CAT_SYMLINK]          = { 0.75, 0.75, 0.75 },
		[COLOR_CAT_FIFO]             = { 0.65, 0.65, 0.65 },
		[COLOR_CAT_SOCKET]           = { 0.65, 0.65, 0.65 },
		[COLOR_CAT_CHARDEV]          = { 0.60, 0.60, 0.60 },
		[COLOR_CAT_BLOCKDEV]         = { 0.60, 0.60, 0.60 },
		[COLOR_CAT_UNKNOWN]          = { 0.45, 0.45, 0.45 },
		[COLOR_CAT_ROM]              = { 0.85, 0.85, 0.85 },
		[COLOR_CAT_HOMEBREW]         = { 0.90, 0.90, 0.90 },
		[COLOR_CAT_3DSX]             = { 0.78, 0.78, 0.78 },
		[COLOR_CAT_CIA]              = { 0.85, 0.85, 0.85 },
		[COLOR_CAT_ARCHIVE]          = { 0.50, 0.50, 0.50 },
		[COLOR_CAT_IMAGE]            = { 0.70, 0.70, 0.70 },
		[COLOR_CAT_AUDIO_VIDEO]      = { 0.70, 0.70, 0.70 },
		[COLOR_CAT_TEXT]             = { 0.60, 0.60, 0.60 },
		[COLOR_CAT_DEFAULT_FILE]     = { 0.40, 0.40, 0.40 },
	},
};

static const char *scheme_names[NUM_COLOR_SCHEMES] = {
	"Default", "High-Contrast", "Monochrome"
};
/* clang-format on */

#define NUM_EXT_COLORS (sizeof(ext_colors) / sizeof(ext_colors[0]))

static ColorScheme active_scheme = COLOR_SCHEME_DEFAULT;


void
color_init( void )
{
	/* Nothing to do -- the palettes above are compile-time constant.
	 * Kept as a call site for parity with the rest of the app's
	 * *_init() functions. Initial scheme is applied later by
	 * settings_init() once it's loaded (or defaulted) the saved
	 * choice -- see settings.c. */
}


int
color_get_scheme( void )
{
	return (int)active_scheme;
}


void
color_set_scheme( int scheme )
{
	if (scheme < 0 || scheme >= NUM_COLOR_SCHEMES)
		return;
	active_scheme = (ColorScheme)scheme;
}


const char *
color_scheme_name( int scheme )
{
	if (scheme < 0 || scheme >= NUM_COLOR_SCHEMES)
		return "?";
	return scheme_names[scheme];
}


static const RGBcolor *
match_color( const char *name )
{
	unsigned int i;

	for (i = 0; i < NUM_EXT_COLORS; i++) {
		if (fnmatch( ext_colors[i].pattern, name, FNM_CASEFOLD ) == 0)
			return &color_schemes[active_scheme][ext_colors[i].category];
	}

	return &color_schemes[active_scheme][COLOR_CAT_DEFAULT_FILE];
}


void
color_assign_recursive( GNode *dnode )
{
	GNode *node;

	node = dnode->children;
	while (node != NULL) {
		NodeType type = NODE_DESC(node)->type;

		if (type == NODE_REGFILE)
			NODE_DESC(node)->color = match_color( NODE_DESC(node)->name );
		else
			NODE_DESC(node)->color = &color_schemes[active_scheme][nodetype_categories[type]];

		if (NODE_IS_DIR(node))
			color_assign_recursive( node );

		node = node->next;
	}
}

/* end color.c */
