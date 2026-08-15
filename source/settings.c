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

#define RPC_CRED_LEN 32
#define RPC_USER_FALLBACK "fsv3ds"
#define RPC_PASS_FALLBACK "fsv3ds-2026" /* change via settings.cfg -- see settings.h */

static LabelMode label_mode = LABEL_MODE_SELECTED_ONLY;
static char default_root[DEFAULT_ROOT_LEN] = DEFAULT_ROOT_FALLBACK;
static char rpc_user[RPC_CRED_LEN] = RPC_USER_FALLBACK;
static char rpc_pass[RPC_CRED_LEN] = RPC_PASS_FALLBACK;

/* settings.cfg stores label_mode as a plain integer (the LabelMode
 * enum value) -- LABEL_MODE_OFF was appended after SELECTED_ONLY/ALL
 * were already shipping on hardware rather than inserted before them,
 * so an existing saved "0"/"1" keeps meaning what it always meant. */
static const char *label_mode_names[NUM_LABEL_MODES] = {
	"Selected only", "All", "Off"
};


/* Shared parsing for the "prefix=value" string fields (default_root,
 * rpc_user, rpc_pass) -- trims the trailing newline and copies into
 * `dst` (size `dst_size`) if `line` starts with `prefix`. Returns
 * TRUE if it matched (regardless of whether the value was non-empty),
 * so callers can chain a series of these with else-if. */
static gboolean
parse_prefixed_field( const char *line, const char *prefix, char *dst, size_t dst_size )
{
	size_t prefix_len = strlen( prefix );
	char buf[DEFAULT_ROOT_LEN + 32]; /* matches settings_init()'s own line buffer */
	char *p;
	size_t len;

	if (strncmp( line, prefix, prefix_len ))
		return FALSE;

	strncpy( buf, line + prefix_len, sizeof(buf) - 1 );
	buf[sizeof(buf) - 1] = '\0';
	p = buf;
	len = strlen( p );

	while (len > 0 && (p[len - 1] == '\n' || p[len - 1] == '\r'))
		p[--len] = '\0';
	if (len > 0) {
		strncpy( dst, p, dst_size - 1 );
		dst[dst_size - 1] = '\0';
	}
	return TRUE;
}


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
	fprintf( f, "rpc_user=%s\n", rpc_user );
	fprintf( f, "rpc_pass=%s\n", rpc_pass );
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

			if (sscanf( line, "color_scheme=%d", &val ) == 1)
				loaded_scheme = val;
			else if (sscanf( line, "label_mode=%d", &val ) == 1)
				loaded_label_mode = (LabelMode)val;
			else if (parse_prefixed_field( line, "default_root=", default_root, sizeof(default_root) ))
				; /* handled */
			else if (parse_prefixed_field( line, "rpc_user=", rpc_user, sizeof(rpc_user) ))
				;
			else if (parse_prefixed_field( line, "rpc_pass=", rpc_pass, sizeof(rpc_pass) ))
				;
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

	/* Always write settings.cfg out (not just on a fresh install): an
	 * existing file saved before rpc_user/rpc_pass existed won't have
	 * those lines yet, and this backfills them (with the compiled-in
	 * defaults) so they're visible/editable over FTP without the user
	 * having to change some other setting first to trigger a save. */
	save_to_disk( );
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


const char *
settings_get_rpc_user( void )
{
	return rpc_user;
}


const char *
settings_get_rpc_pass( void )
{
	return rpc_pass;
}

/* end settings.c */
