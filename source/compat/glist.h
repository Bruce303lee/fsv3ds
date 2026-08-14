/* glist.h - minimal GLib GList (doubly linked list) shim.
 * Covers the subset of the GList API used across the fsv codebase
 * (geometry.c, camera.c, common.c, ...), even though Phase 1 itself
 * only needs the GNode shim -- this exists so later phases can port
 * those files with minimal changes. */
#ifndef FSV3DS_GLIST_H
#define FSV3DS_GLIST_H

#include "gtypes.h"

typedef struct _GList GList;
struct _GList {
	gpointer data;
	GList *next;
	GList *prev;
};

GList *g_list_append( GList *list, gpointer data );
GList *g_list_prepend( GList *list, gpointer data );
GList *g_list_insert_before( GList *list, GList *sibling, gpointer data );
GList *g_list_remove( GList *list, gconstpointer data );
GList *g_list_find( GList *list, gconstpointer data );
GList *g_list_find_custom( GList *list, gconstpointer data, GCompareFunc cmp );
GList *g_list_last( GList *list );
guint  g_list_length( GList *list );
GList *g_list_sort( GList *list, GCompareFunc cmp );
void   g_list_free( GList *list );

#endif /* FSV3DS_GLIST_H */
