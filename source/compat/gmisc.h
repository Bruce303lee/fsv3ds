/* gmisc.h - remaining GLib odds and ends fsv relies on: the pooled
 * "chunk" allocators, string interning, path-splitting helpers, and
 * the g_assert/g_warning family of macros. */
#ifndef FSV3DS_GMISC_H
#define FSV3DS_GMISC_H

#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "gtypes.h"

/* ---- assert / logging macros ---- */
#define g_assert(expr)            assert(expr)
#define g_assert_not_reached()    assert(0)
#define g_return_if_fail(expr)    do { if (!(expr)) return; } while (0)
#define g_warning(...)            ((void)0)
#define g_message(...)            ((void)0)

/* ---- basic allocation wrappers ---- */
#define g_free(p)      free(p)
#define g_malloc(n)    malloc(n)
#define g_strdup(s)    strdup(s)

/* ---- GMemChunk: pooled fixed-size-record allocator ----
 * Real GLib supports G_ALLOC_ONLY / G_ALLOC_AND_FREE; we only ever see
 * G_ALLOC_ONLY in fsv, so per-atom free is not implemented. */
#define G_ALLOC_ONLY       1
#define G_ALLOC_AND_FREE   2

typedef struct _GMemChunk GMemChunk;

GMemChunk *g_mem_chunk_new( guint atom_size, guint area_size, guint alloc_type );
gpointer   g_mem_chunk_alloc( GMemChunk *chunk );
void       g_mem_chunk_reset( GMemChunk *chunk );
void       g_blow_chunks( void ); /* no-op: our chunks don't keep a free-list to trim */

#define g_mem_chunk_create(type, atoms_per_block, alloc_type) \
	g_mem_chunk_new( sizeof(type), sizeof(type) * (atoms_per_block), alloc_type )

/* ---- GStringChunk: string interning arena ----
 * Simplified to per-string strdup (no shared arena, no interning /
 * deduplication). g_string_chunk_free() does NOT free the individual
 * strings -- acceptable for fsv3ds's single-scan-per-run lifetime; if
 * re-scanning is added later this becomes a real (small) leak per
 * rescan and should be revisited. */
typedef struct _GStringChunk GStringChunk;

GStringChunk *g_string_chunk_new( guint size );
gchar        *g_string_chunk_insert( GStringChunk *chunk, const gchar *string );
void          g_string_chunk_free( GStringChunk *chunk );

/* ---- path helpers (deprecated GLib functions, reimplemented) ---- */
gchar *g_basename( const gchar *file_name ); /* points into file_name, not allocated */
gchar *g_dirname( const gchar *file_name );  /* allocated, caller g_free()s it */

#endif /* FSV3DS_GMISC_H */
