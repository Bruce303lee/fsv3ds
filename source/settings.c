/* settings.c - see settings.h */
#include "common.h"
#include "settings.h"
#include "color.h"
#include "viz.h"
#include "rpc.h"

#include <sys/stat.h>

#define SETTINGS_DIR  "sdmc:/3ds/fsv3ds"
#define SETTINGS_PATH SETTINGS_DIR "/settings.cfg"

#define DEFAULT_ROOT_LEN     256
#define DEFAULT_ROOT_FALLBACK "sdmc:/3ds"

static LabelMode label_mode = LABEL_MODE_SELECTED_ONLY;
static char default_root[DEFAULT_ROOT_LEN] = DEFAULT_ROOT_FALLBACK;

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
	fprintf( f, "default_root=%s\n", default_root );
	fclose( f );
}


void
settings_init( void )
{
	FILE *f;
	char line[DEFAULT_ROOT_LEN + 32]; /* long enough for "default_root=" + a full path */
	int loaded_scheme = COLOR_SCHEME_DEFAULT;
	LabelMode loaded_label_mode = LABEL_MODE_SELECTED_ONLY;

	f = fopen( SETTINGS_PATH, "r" );
	if (f != NULL) {
		while (fgets( line, sizeof(line), f ) != NULL) {
			int val;
			static const char root_prefix[] = "default_root=";

			if (sscanf( line, "color_scheme=%d", &val ) == 1)
				loaded_scheme = val;
			else if (sscanf( line, "label_mode=%d", &val ) == 1)
				loaded_label_mode = (LabelMode)val;
			else if (!strncmp( line, root_prefix, sizeof(root_prefix) - 1 )) {
				char *p = line + sizeof(root_prefix) - 1;
				size_t len = strlen( p );

				while (len > 0 && (p[len - 1] == '\n' || p[len - 1] == '\r'))
					p[--len] = '\0';
				if (len > 0) {
					strncpy( default_root, p, sizeof(default_root) - 1 );
					default_root[sizeof(default_root) - 1] = '\0';
				}
			}
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


const char *
settings_get_default_root( void )
{
	return default_root;
}


void
settings_set_default_root( const char *path )
{
	if (path == NULL || path[0] == '\0')
		return;

	strncpy( default_root, path, sizeof(default_root) - 1 );
	default_root[sizeof(default_root) - 1] = '\0';
	save_to_disk( );
}

/* end settings.c */
