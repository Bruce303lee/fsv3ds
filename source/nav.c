/* nav.c - see nav.h */
#include "common.h"
#include "nav.h"

static GNode *view_root = NULL;
static GNode *selected_node = NULL;


void
nav_reset( void )
{
	view_root = root_dnode;
	selected_node = root_dnode->children;
}


GNode *
nav_view_root( void )
{
	return view_root;
}


GNode *
nav_selected_node( void )
{
	return selected_node;
}


void
nav_cycle_selection( int dir )
{
	if (selected_node == NULL || view_root == NULL)
		return;
	if (view_root->children == NULL || view_root->children->next == NULL)
		return; /* 0 or 1 children: nothing to cycle to */

	if (dir > 0) {
		selected_node = (selected_node->next != NULL) ? selected_node->next : view_root->children;
	}
	else if (dir < 0) {
		if (selected_node == view_root->children) {
			GNode *last = view_root->children;
			while (last->next != NULL)
				last = last->next;
			selected_node = last;
		}
		else
			selected_node = selected_node->prev;
	}
}


void
nav_set_selected( GNode *node )
{
	selected_node = node;
}


gboolean
nav_drill_selected( void )
{
	if (selected_node == NULL)
		return FALSE;
	if (!NODE_IS_DIR(selected_node) || selected_node->children == NULL)
		return FALSE;

	view_root = selected_node;
	selected_node = view_root->children;

	return TRUE;
}


gboolean
nav_go_up( void )
{
	GNode *came_from;

	if (view_root == NULL || view_root == root_dnode)
		return FALSE;

	came_from = view_root;
	view_root = view_root->parent;
	selected_node = came_from;

	return TRUE;
}

/* end nav.c */
