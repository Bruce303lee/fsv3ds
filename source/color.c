/* color.c - see color.h
 *
 * Extension-based file coloring, replacing mapv.c's old flat
 * per-NodeType palette for regular files (directories and other
 * special types still get a flat per-type color -- see
 * nodetype_colors[] below, matching upstream's "directory override").
 *
 * Patterns are matched in order, first match wins, via fnmatch()
 * (devkitARM/newlib has it, unlike scandir()/alphasort() back in
 * Phase 1 -- no compat shim needed here). Case-insensitive
 * (FNM_CASEFOLD) since FAT/SD card extensions show up in any case --
 * that flag is a GNU extension newlib gates behind __GNU_VISIBLE,
 * which devkitARM's default -std doesn't set, so _GNU_SOURCE has to be
 * defined before fnmatch.h (transitively via common.h) is first seen. */
#define _GNU_SOURCE
#include "common.h"
#include "color.h"

#include <fnmatch.h>

/* Flat colors for directories and anything that isn't a regular file
 * (symlinks/fifo/socket/device nodes basically never occur on a FAT SD
 * card, but are handled for completeness -- same values mapv.c's old
 * hardcoded table used). Index by NodeType. */
static const RGBcolor nodetype_colors[NUM_NODE_TYPES] = {
	{ 0.0, 0.0, 0.0 },    /* Metanode (not used) */
	{ 0.85, 0.70, 0.30 }, /* Directory (tan) */
	{ 0.75, 0.75, 0.68 }, /* Regular file fallback -- shouldn't normally
	                       * show (match_color()'s own default below is
	                       * used instead), kept for completeness */
	{ 0.55, 0.85, 0.55 }, /* Symlink (green) */
	{ 0.85, 0.55, 0.85 }, /* FIFO */
	{ 0.85, 0.85, 0.45 }, /* Socket */
	{ 0.85, 0.45, 0.45 }, /* Char device */
	{ 0.65, 0.45, 0.85 }, /* Block device */
	{ 0.6, 0.6, 0.6 }     /* Unknown */
};

/* Regular files that don't match any pattern below */
static const RGBcolor default_file_color = { 0.35, 0.55, 0.85 }; /* blue */

typedef struct {
	const char *pattern;
	RGBcolor color;
} ExtColor;

/* clang-format off */
static const ExtColor ext_colors[] = {
	/* ROMs / disc images (green) -- this is a homebrew/emulation SD
	 * card in practice, so these get priority over more "generic"
	 * categories below where extensions collide (e.g. Genesis' *.gen). */
	{ "*.nes",  { 0.35, 0.85, 0.35 } },
	{ "*.sfc",  { 0.35, 0.85, 0.35 } },
	{ "*.smc",  { 0.35, 0.85, 0.35 } },
	{ "*.gba",  { 0.35, 0.85, 0.35 } },
	{ "*.gb",   { 0.35, 0.85, 0.35 } },
	{ "*.gbc",  { 0.35, 0.85, 0.35 } },
	{ "*.nds",  { 0.35, 0.85, 0.35 } },
	{ "*.n64",  { 0.35, 0.85, 0.35 } },
	{ "*.z64",  { 0.35, 0.85, 0.35 } },
	{ "*.gen",  { 0.35, 0.85, 0.35 } },
	{ "*.smd",  { 0.35, 0.85, 0.35 } },
	{ "*.pce",  { 0.35, 0.85, 0.35 } },
	{ "*.ws",   { 0.35, 0.85, 0.35 } },
	{ "*.wsc",  { 0.35, 0.85, 0.35 } },
	{ "*.iso",  { 0.35, 0.85, 0.35 } },
	{ "*.cso",  { 0.35, 0.85, 0.35 } },
	{ "*.chd",  { 0.35, 0.85, 0.35 } },
	{ "*.bin",  { 0.35, 0.85, 0.35 } }, /* generic elsewhere, but common
	                                     * for N64/other ROMs -- confirmed
	                                     * on hardware, DaedalusX64's Roms
	                                     * folder is full of *.bin */

	/* 3DS homebrew/installables (gold) */
	{ "*.3dsx", { 0.95, 0.75, 0.25 } },
	{ "*.cia",  { 0.95, 0.75, 0.25 } },
	{ "*.cxi",  { 0.95, 0.75, 0.25 } },
	{ "*.cci",  { 0.95, 0.75, 0.25 } },
	{ "*.3ds",  { 0.95, 0.75, 0.25 } },
	{ "*.smdh", { 0.95, 0.75, 0.25 } },

	/* Archives (red) */
	{ "*.zip",  { 0.85, 0.30, 0.30 } },
	{ "*.7z",   { 0.85, 0.30, 0.30 } },
	{ "*.rar",  { 0.85, 0.30, 0.30 } },
	{ "*.tar",  { 0.85, 0.30, 0.30 } },
	{ "*.gz",   { 0.85, 0.30, 0.30 } },
	{ "*.bz2",  { 0.85, 0.30, 0.30 } },
	{ "*.xz",   { 0.85, 0.30, 0.30 } },
	{ "*.zst",  { 0.85, 0.30, 0.30 } },

	/* Images (magenta) */
	{ "*.png",  { 0.85, 0.35, 0.85 } },
	{ "*.jpg",  { 0.85, 0.35, 0.85 } },
	{ "*.jpeg", { 0.85, 0.35, 0.85 } },
	{ "*.gif",  { 0.85, 0.35, 0.85 } },
	{ "*.bmp",  { 0.85, 0.35, 0.85 } },
	{ "*.tga",  { 0.85, 0.35, 0.85 } },

	/* Audio/video (cyan) */
	{ "*.mp3",  { 0.35, 0.85, 0.85 } },
	{ "*.ogg",  { 0.35, 0.85, 0.85 } },
	{ "*.wav",  { 0.35, 0.85, 0.85 } },
	{ "*.flac", { 0.35, 0.85, 0.85 } },
	{ "*.mp4",  { 0.35, 0.85, 0.85 } },
	{ "*.avi",  { 0.35, 0.85, 0.85 } },
	{ "*.mkv",  { 0.35, 0.85, 0.85 } },
	{ "*.mov",  { 0.35, 0.85, 0.85 } },

	/* Text/config/code (light grey) */
	{ "*.txt",  { 0.80, 0.80, 0.80 } },
	{ "*.md",   { 0.80, 0.80, 0.80 } },
	{ "*.cfg",  { 0.80, 0.80, 0.80 } },
	{ "*.ini",  { 0.80, 0.80, 0.80 } },
	{ "*.json", { 0.80, 0.80, 0.80 } },
	{ "*.xml",  { 0.80, 0.80, 0.80 } },
	{ "*.log",  { 0.80, 0.80, 0.80 } },
	{ "*.c",    { 0.80, 0.80, 0.80 } },
	{ "*.h",    { 0.80, 0.80, 0.80 } }
};
/* clang-format on */

#define NUM_EXT_COLORS (sizeof(ext_colors) / sizeof(ext_colors[0]))


void
color_init( void )
{
	/* Nothing to do -- the palette above is compile-time constant.
	 * Kept as a call site for parity with the rest of the app's
	 * *_init() functions, and in case this becomes configurable later. */
}


static const RGBcolor *
match_color( const char *name )
{
	unsigned int i;

	for (i = 0; i < NUM_EXT_COLORS; i++) {
		if (fnmatch( ext_colors[i].pattern, name, FNM_CASEFOLD ) == 0)
			return &ext_colors[i].color;
	}

	return &default_file_color;
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
			NODE_DESC(node)->color = &nodetype_colors[type];

		if (NODE_IS_DIR(node))
			color_assign_recursive( node );

		node = node->next;
	}
}

/* end color.c */
