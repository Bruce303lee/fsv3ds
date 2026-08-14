/* settings.c - see settings.h */
#include "common.h"
#include "settings.h"
#include "color.h"
#include "viz.h"
#include "rpc.h"

#include <sys/stat.h>

#define SETTINGS_DIR  "sdmc:/3ds/fsv3ds"
#define SETTINGS_PATH SETTINGS_DIR "/settings.cfg"

static LabelMode label_mode = LABEL_MODE_SELECTED_ONLY;

/* settings.cfg stores label_mode as a plain integer (the LabelMode
 * enum value) -- LABEL_MODE_OFF was appended after SELECTED_ONLY/ALL
 * were already shipping on hardware rather than inserted before them,
 * so an existing saved "0"/"1" keeps meaning what it always meant. */
static const char *label_mode_names[NUM_LABEL_MODES] = {
	"Selected only", "All", "Off"
};


static void
save_to_disk( void )
{
	FILE *f;

	/* Best-effort -- mkdir() on an already-existing directory just
	 * fails with EEXIST, which is fine to ignore; there's no parent
	 * chain to worry about since sdmc:/3ds already exists in practice
	 * (it's SCAN_ROOT). */
	mkdir( SETTINGS_DIR, 0777 );

	f = fopen( SETTINGS_PATH, "w" );
	if (f == NULL) {
		rpc_logf( "settings: couldn't save to " SETTINGS_PATH "\n" );
		return;
	}

	fprintf( f, "color_scheme=%d\n", color_get_scheme( ) );
	fprintf( f, "label_mode=%d\n", (int)label_mode );
	fclose( f );
}


void
settings_init( void )
{
	FILE *f;
	char line[64];
	int loaded_scheme = COLOR_SCHEME_DEFAULT;
	LabelMode loaded_label_mode = LABEL_MODE_SELECTED_ONLY;

	f = fopen( SETTINGS_PATH, "r" );
	if (f != NULL) {
		while (fgets( line, sizeof(line), f ) != NULL) {
			int val;

			if (sscanf( line, "color_scheme=%d", &val ) == 1)
				loaded_scheme = val;
			else if (sscanf( line, "label_mode=%d", &val ) == 1)
				loaded_label_mode = (LabelMode)val;
		}
		fclose( f );
		rpc_logf( "settings: loaded from " SETTINGS_PATH "\n" );
	}
	else
		rpc_logf( "settings: no saved settings, using defaults\n" );

	if (loaded_scheme < 0 || loaded_scheme >= NUM_COLOR_SCHEMES)
		loaded_scheme = COLOR_SCHEME_DEFAULT;
	if (loaded_label_mode < 0 || loaded_label_mode >= NUM_LABEL_MODES)
		loaded_label_mode = LABEL_MODE_SELECTED_ONLY;

	color_set_scheme( loaded_scheme );
	label_mode = loaded_label_mode;
}


int
settings_get_color_scheme( void )
{
	return color_get_scheme( );
}


void
settings_set_color_scheme( int scheme )
{
	if (scheme < 0 || scheme >= NUM_COLOR_SCHEMES)
		return;

	color_set_scheme( scheme );

	/* Re-resolve every node's NodeDesc->color pointer against the new
	 * scheme's table, then re-bake vertex colors -- see viz_rebuild()'s
	 * comment. No-op (both functions) before the first scan; root_dnode
	 * (globals.fstree->children) unconditionally dereferences
	 * globals.fstree, so that has to be checked first. */
	if (globals.fstree != NULL)
		color_assign_recursive( root_dnode );
	viz_rebuild( );

	save_to_disk( );
}


void
settings_cycle_color_scheme( void )
{
	settings_set_color_scheme( (color_get_scheme( ) + 1) % NUM_COLOR_SCHEMES );
}


LabelMode
settings_get_label_mode( void )
{
	return label_mode;
}


void
settings_set_label_mode( LabelMode mode )
{
	if (mode < 0 || mode >= NUM_LABEL_MODES)
		return;

	label_mode = mode;
	save_to_disk( );
}


void
settings_cycle_label_mode( void )
{
	settings_set_label_mode( (label_mode + 1) % NUM_LABEL_MODES );
}


const char *
settings_label_mode_name( LabelMode mode )
{
	if (mode < 0 || mode >= NUM_LABEL_MODES)
		return "?";
	return label_mode_names[mode];
}

/* end settings.c */
