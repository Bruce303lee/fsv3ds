/* common.c - fsv3ds Phase 1
 *
 * Trimmed port of fsv/src/common.c. See common.h for what was dropped
 * and why (mainly: anything needing X11 pixmaps, gettext, or Unix
 * user/group lookups -- none of which exist on the 3DS/SD card).
 */
#include "common.h"

#include <unistd.h>

struct Globals globals;

const char *node_type_names[NUM_NODE_TYPES] = {
	NULL, /* Metanode */
	"Directory",
	"Regular file",
	"Symbolic link",
	"Named pipe (FIFO)",
	"Network socket",
	"Character device",
	"Block device",
	"Unknown"
};

const char *node_type_plural_names[NUM_NODE_TYPES] = {
	NULL, /* Metanodes */
	"Directories",
	"Regular files",
	"Symlinks",
	"Named pipes",
	"Sockets",
	"Char. devs.",
	"Block devs.",
	"Unknown"
};


void *
xmalloc( size_t size )
{
	void *block = malloc( size );

	if (block == NULL)
		quit( "Out of memory" );

	return block;
}


void *
xrealloc( void *block, size_t size )
{
	void *new_block = realloc( block, size );

	if (new_block == NULL)
		quit( "Out of memory" );

	return new_block;
}


char *
xstrdup( const char *string )
{
	char *new_string = strdup( string );

	if (new_string == NULL)
		quit( "Out of memory" );

	return new_string;
}


char *
xstrredup( char *old_string, const char *string )
{
	char *new_string = xrealloc( old_string, strlen( string ) + 1 );

	strcpy( new_string, string );

	return new_string;
}


void
xfree( void *block )
{
	free( block );
}


/* Like strcpy( ), but safe when the two strings overlap */
static char *
strmove( char *to, const char *from )
{
	memmove( to, from, strlen( from ) + 1 );
	return to;
}


/* Hybrid of strcat( ) and realloc( ) */
char *
strrecat( char *string, const char *add_string )
{
	int len = strlen( string ) + strlen( add_string ) + 1;

	RESIZE(string, len, char);
	strcat( string, add_string );

	return string;
}


/* Strips leading/trailing whitespace, trims allocation to fit */
char *
xstrstrip( char *string )
{
	int i;

	i = strspn( string, " \t" );
	if (i > 0)
		strmove( string, &string[i] );

	for (i = strlen( string ) - 1; i >= -1; --i) {
		char c = string[MAX(0, i)];
		if (c == ' ' || c == '\t')
			continue;
		break;
	}
	string[i + 1] = '\0';

	return xrealloc( string, (strlen( string ) + 1) * sizeof(char) );
}


const char *
xgetcwd( void )
{
	static char *cwd = NULL;
	int len = 256;
	char *p = NULL;

	while (p == NULL) {
		RESIZE(cwd, len, char);
		p = getcwd( cwd, len );
		len *= 2;
	}

	cwd = xstrstrip( cwd );

	return cwd;
}


/* Converts a 64-bit integer into a grouped number string
 * (e.g. 1000000 --> 1,000,000) */
const char *
i64toa( int64 number )
{
	static char strbuf1[256];
	int len, digit_count = 0;
	int n = 256, i;
	char strbuf0[256];
	char d;

	sprintf( strbuf0, "%lld", (long long)number );
	len = strlen( strbuf0 );
	for (i = len - 1; i >= 0; i--) {
		d = strbuf0[i];
		if ((digit_count % 3) == 0)
			strbuf1[--n] = ',';
		strbuf1[--n] = d;
		++digit_count;
	}
	strbuf1[255] = '\0';

	return &strbuf1[n];
}


/* Converts a byte quantity into abbreviated human-readable format
 * (e.g. 7632 --> 7.5kB, 1264245 --> 1.2MB) */
const char *
abbrev_size( int64 size )
{
	static const char *suffixes[] = { "B", "kB", "MB", "GB", "TB", "PB", "EB" };
	static char strbuf[64];
	double s = (double)size;
	int m = 0;

	while (s >= 1024.0) {
		++m;
		s /= 1024.0;
	}
	if ((m > 0) && (s < 100.0))
		sprintf( strbuf, "%.1f %s", s, suffixes[m] );
	else
		sprintf( strbuf, "%.0f %s", s, suffixes[m] );

	return strbuf;
}


/* Returns the absolute name of a node (with all leading directory
 * components) */
const char *
node_absname( GNode *node )
{
	static char *absname = NULL;
	GNode *up_node;
	int len, absname_len = 0;
	int i;
	const char *name;

	up_node = node;
	while (up_node != NULL) {
		name = NODE_DESC(up_node)->name;
		len = strlen( name );
		absname_len += len + 1;
		up_node = up_node->parent;
	}

	if (absname != NULL)
		xfree( absname );
	absname = NEW_ARRAY(char, absname_len);

	i = absname_len;
	up_node = node;
	while (up_node != NULL) {
		name = NODE_DESC(up_node)->name;
		len = strlen( name );
		absname[--i] = '/';
		i -= len;
		strncpy( &absname[i], name, len );
		up_node = up_node->parent;
	}
	absname[absname_len - 1] = '\0';

	/* Collapse runs of consecutive slashes into one. Upstream only ever
	 * special-cased a doubled slash at the very start of the string
	 * (root_dnode's basename is "" when the root is literally "/", so
	 * its separator collapses with its parent's). On 3DS, the SD card
	 * root is "sdmc:/" -- basename("sdmc:/") is *also* "" for the same
	 * reason, but the resulting doubled slash lands after "sdmc:", not
	 * at the start (e.g. "sdmc://luma"), so that start-only check never
	 * caught it. A malformed "//" path silently fails FSUSER_OpenDirectory
	 * (though FSUSER_OpenFile tolerates it, so files kept working while
	 * every directory silently vanished from scans). Collapsing
	 * anywhere in the string handles both cases uniformly. */
	{
		char *src = absname, *dst = absname;
		char prev = '\0';

		while (*src != '\0') {
			if (*src == '/' && prev == '/') {
				++src;
				continue;
			}
			prev = *src;
			*dst++ = *src++;
		}
		*dst = '\0';
	}

	return absname;
}


void
quit( const char *message )
{
	fprintf( stderr, "ERROR: %s\n", message );
	fflush( stderr );

	exit( EXIT_FAILURE );
}

/* end common.c */
