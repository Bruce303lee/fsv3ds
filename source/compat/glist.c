/* glist.c - see glist.h */
#include <stdlib.h>
#include "glist.h"

GList *
g_list_append( GList *list, gpointer data )
{
	GList *node = (GList *)malloc( sizeof(GList) );
	GList *last;

	node->data = data;
	node->next = NULL;

	if (list == NULL) {
		node->prev = NULL;
		return node;
	}

	last = g_list_last( list );
	last->next = node;
	node->prev = last;

	return list;
}


GList *
g_list_prepend( GList *list, gpointer data )
{
	GList *node = (GList *)malloc( sizeof(GList) );

	node->data = data;
	node->next = list;
	node->prev = NULL;
	if (list != NULL)
		list->prev = node;

	return node;
}


GList *
g_list_insert_before( GList *list, GList *sibling, gpointer data )
{
	GList *node;

	if (sibling == NULL)
		return g_list_append( list, data );

	node = (GList *)malloc( sizeof(GList) );
	node->data = data;
	node->next = sibling;
	node->prev = sibling->prev;
	sibling->prev = node;
	if (node->prev != NULL)
		node->prev->next = node;

	return (list == sibling) ? node : list;
}


GList *
g_list_remove( GList *list, gconstpointer data )
{
	GList *node = g_list_find( list, data );

	if (node == NULL)
		return list;

	if (node->prev != NULL)
		node->prev->next = node->next;
	if (node->next != NULL)
		node->next->prev = node->prev;

	if (node == list)
		list = node->next;

	free( node );

	return list;
}


GList *
g_list_find( GList *list, gconstpointer data )
{
	while (list != NULL) {
		if (list->data == data)
			return list;
		list = list->next;
	}

	return NULL;
}


GList *
g_list_find_custom( GList *list, gconstpointer data, GCompareFunc cmp )
{
	while (list != NULL) {
		if (cmp( list->data, data ) == 0)
			return list;
		list = list->next;
	}

	return NULL;
}


GList *
g_list_last( GList *list )
{
	if (list == NULL)
		return NULL;
	while (list->next != NULL)
		list = list->next;

	return list;
}


guint
g_list_length( GList *list )
{
	guint n = 0;

	while (list != NULL) {
		++n;
		list = list->next;
	}

	return n;
}


GList *
g_list_sort( GList *list, GCompareFunc cmp )
{
	guint n = g_list_length( list );
	GList **arr, *l;
	guint i;

	if (n < 2)
		return list;

	arr = (GList **)malloc( n * sizeof(GList *) );
	l = list;
	for (i = 0; i < n; i++, l = l->next)
		arr[i] = l;

	/* insertion sort: n is small in practice (directory entry counts),
	 * and this keeps the comparator signature identical to GLib's
	 * (data, data) rather than needing a wrapper like gnode.c's */
	for (i = 1; i < n; i++) {
		GList *key = arr[i];
		gint j = (gint)i - 1;

		while (j >= 0 && cmp( arr[j]->data, key->data ) > 0) {
			arr[j + 1] = arr[j];
			--j;
		}
		arr[j + 1] = key;
	}

	arr[0]->prev = NULL;
	for (i = 0; i < n - 1; i++) {
		arr[i]->next = arr[i + 1];
		arr[i + 1]->prev = arr[i];
	}
	arr[n - 1]->next = NULL;

	list = arr[0];
	free( arr );

	return list;
}


void
g_list_free( GList *list )
{
	while (list != NULL) {
		GList *next = list->next;
		free( list );
		list = next;
	}
}
