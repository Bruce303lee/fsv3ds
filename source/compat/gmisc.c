/* gmisc.c - see gmisc.h */
#include <stdlib.h>
#include <string.h>
#include "gmisc.h"

/* ---- GMemChunk ---- */

typedef struct _MCBlock MCBlock;
struct _MCBlock {
	MCBlock *next;
	guint used;
};

struct _GMemChunk {
	guint atom_size;
	guint area_size;
	MCBlock *blocks;
};

GMemChunk *
g_mem_chunk_new( guint atom_size, guint area_size, guint alloc_type )
{
	GMemChunk *mc = (GMemChunk *)malloc( sizeof(GMemChunk) );

	(void)alloc_type;
	mc->atom_size = atom_size;
	mc->area_size = area_size;
	mc->blocks = NULL;

	return mc;
}


static MCBlock *
new_block( GMemChunk *mc )
{
	MCBlock *b = (MCBlock *)malloc( sizeof(MCBlock) + mc->area_size );

	b->next = mc->blocks;
	b->used = 0;
	mc->blocks = b;

	return b;
}


gpointer
g_mem_chunk_alloc( GMemChunk *mc )
{
	MCBlock *b = mc->blocks;
	guint8 *base;
	gpointer p;

	if (b == NULL || b->used + mc->atom_size > mc->area_size)
		b = new_block( mc );

	base = (guint8 *)(b + 1);
	p = base + b->used;
	b->used += mc->atom_size;

	return p;
}


void
g_mem_chunk_reset( GMemChunk *mc )
{
	MCBlock *b = mc->blocks;

	while (b != NULL) {
		MCBlock *next = b->next;
		free( b );
		b = next;
	}
	mc->blocks = NULL;
}


void
g_blow_chunks( void )
{
	/* no-op: see gmisc.h */
}


/* ---- GStringChunk (simplified: plain strdup, see header note) ---- */

struct _GStringChunk {
	int unused;
};

GStringChunk *
g_string_chunk_new( guint size )
{
	(void)size;
	return (GStringChunk *)malloc( sizeof(GStringChunk) );
}


gchar *
g_string_chunk_insert( GStringChunk *chunk, const gchar *string )
{
	(void)chunk;
	return strdup( string );
}


void
g_string_chunk_free( GStringChunk *chunk )
{
	free( chunk );
}


/* ---- path helpers ---- */

gchar *
g_basename( const gchar *file_name )
{
	const gchar *base = strrchr( file_name, '/' );

	return (gchar *)(base ? base + 1 : file_name);
}


gchar *
g_dirname( const gchar *file_name )
{
	const gchar *base = strrchr( file_name, '/' );
	gchar *dir;
	size_t len;

	if (base == NULL)
		return strdup( "." );
	if (base == file_name)
		return strdup( "/" );

	len = (size_t)(base - file_name);
	dir = (gchar *)malloc( len + 1 );
	memcpy( dir, file_name, len );
	dir[len] = '\0';

	return dir;
}
