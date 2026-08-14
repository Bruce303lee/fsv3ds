/* gnode.c - see gnode.h */
#include <stdlib.h>
#include "gnode.h"

GNode *
g_node_new( gpointer data )
{
	GNode *node = (GNode *)malloc( sizeof(GNode) );

	node->data = data;
	node->next = NULL;
	node->prev = NULL;
	node->parent = NULL;
	node->children = NULL;

	return node;
}


static GNode *
link_child( GNode *parent, GNode *node, gboolean prepend )
{
	node->parent = parent;

	if (parent->children == NULL) {
		parent->children = node;
		node->next = NULL;
		node->prev = NULL;
		return node;
	}

	if (prepend) {
		node->next = parent->children;
		node->prev = NULL;
		parent->children->prev = node;
		parent->children = node;
	}
	else {
		GNode *last = parent->children;

		while (last->next != NULL)
			last = last->next;
		last->next = node;
		node->prev = last;
		node->next = NULL;
	}

	return node;
}


GNode *
g_node_append_data( GNode *parent, gpointer data )
{
	return link_child( parent, g_node_new( data ), FALSE );
}


GNode *
g_node_prepend_data( GNode *parent, gpointer data )
{
	return link_child( parent, g_node_new( data ), TRUE );
}


void
g_node_unlink( GNode *node )
{
	if (node->prev != NULL)
		node->prev->next = node->next;
	else if (node->parent != NULL)
		node->parent->children = node->next;

	if (node->next != NULL)
		node->next->prev = node->prev;

	node->parent = NULL;
	node->next = NULL;
	node->prev = NULL;
}


void
g_node_destroy( GNode *node )
{
	GNode *child = node->children;

	/* Must save ->next before recursing: g_node_destroy(child) frees
	 * child, so re-reading child->next (or node->children) afterward
	 * is a use-after-free -- this previously looped on a dangling
	 * pointer instead of advancing through the sibling list, which is
	 * why rescanning (the first thing to actually call this on a
	 * populated tree) crashed. */
	while (child != NULL) {
		GNode *next = child->next;
		g_node_destroy( child );
		child = next;
	}

	free( node );
}


guint
g_node_depth( GNode *node )
{
	guint depth = 0;

	while (node != NULL) {
		++depth;
		node = node->parent;
	}

	return depth;
}


gboolean
g_node_is_ancestor( GNode *node, GNode *descendant )
{
	while (descendant != NULL) {
		if (descendant->parent == node)
			return TRUE;
		descendant = descendant->parent;
	}

	return FALSE;
}


/* Not reentrant (matches the simplicity of the rest of this shim --
 * fsv3ds is single-threaded) */
static GCompareFunc sort_cmp;

static int
node_ptr_cmp( const void *a, const void *b )
{
	GNode *na = *(GNode *const *)a;
	GNode *nb = *(GNode *const *)b;

	return sort_cmp( na->data, nb->data );
}


void
g_node_sort_children( GNode *parent, GCompareFunc cmp )
{
	guint n = 0;
	GNode *c, **arr;
	guint i;

	for (c = parent->children; c != NULL; c = c->next)
		++n;
	if (n < 2)
		return;

	arr = (GNode **)malloc( n * sizeof(GNode *) );
	i = 0;
	for (c = parent->children; c != NULL; c = c->next)
		arr[i++] = c;

	sort_cmp = cmp;
	qsort( arr, n, sizeof(GNode *), node_ptr_cmp );

	parent->children = arr[0];
	arr[0]->prev = NULL;
	for (i = 0; i < n - 1; i++) {
		arr[i]->next = arr[i + 1];
		arr[i + 1]->prev = arr[i];
	}
	arr[n - 1]->next = NULL;

	free( arr );
}
