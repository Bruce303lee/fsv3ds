/* gnode.h - minimal GLib GNode (n-ary tree) shim.
 * fsv's filesystem tree is a GNode tree; only the operations scanfs.c
 * (and later geometry.c/camera.c) actually use are implemented. */
#ifndef FSV3DS_GNODE_H
#define FSV3DS_GNODE_H

#include "gtypes.h"

typedef struct _GNode GNode;
struct _GNode {
	gpointer data;
	GNode *next;
	GNode *prev;
	GNode *parent;
	GNode *children;
};

GNode *g_node_new( gpointer data );
GNode *g_node_append_data( GNode *parent, gpointer data );
GNode *g_node_prepend_data( GNode *parent, gpointer data );
void g_node_unlink( GNode *node );
void g_node_destroy( GNode *node );
guint g_node_depth( GNode *node );
gboolean g_node_is_ancestor( GNode *node, GNode *descendant );

/* Not a real GLib function: upstream fsv sorts a GNode's children by
 * reinterpret-casting the sibling chain as a GList (their struct layouts
 * happen to alias in real GLib). We don't rely on that trick; this does
 * the same job -- sort node->children by cmp(a->data, b->data). */
void g_node_sort_children( GNode *parent, GCompareFunc cmp );

#endif /* FSV3DS_GNODE_H */
