/* scanfs.c - fsv3ds Phase 1
 *
 * Port of fsv/src/scanfs.c. The scanning/tree-building/sorting logic
 * (process_dir, compare_node, setup_fstree_recursive) is essentially
 * unchanged from upstream. What's gone:
 *   - all GTK/dirtree/filelist/gui/viewport/window calls (no UI yet --
 *     Phase 3). Progress reporting is a plain printf per directory.
 *   - user/group stat fields (no meaning on FAT/SD).
 *   - the gtk_timeout-driven periodic "stats/sec" readout.
 * scandir()/alphasort() come from compat/scandir_compat.c rather than
 * libc, since devkitARM/newlib doesn't promise the BSD scandir() extension.
 */
#include "common.h"
#include "scanfs.h"

#include <dirent.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

#include "compat/scandir_compat.h"
#include "rpc.h"

#define NODE_IS_SPECIAL(node) \
	(NODE_DESC(node)->type != NODE_DIRECTORY && NODE_DESC(node)->type != NODE_REGFILE)

/* Node descriptors and name strings are stored here */
static GMemChunk *ndesc_memchunk = NULL;
static GMemChunk *dir_ndesc_memchunk = NULL;
static GStringChunk *name_strchunk = NULL;

/* Node ID counter */
static unsigned int node_id;

/* Running totals, printed at the end of the scan */
static int node_counts[NUM_NODE_TYPES];
static int64 size_counts[NUM_NODE_TYPES];


/* Fills in a node's stat( )-derived fields. Returns 0 on success,
 * -1 on error (node should be discarded) */
static int
stat_node( GNode *node )
{
	struct stat st;

	if (lstat( node_absname( node ), &st ))
		return -1;

	if (S_ISDIR(st.st_mode))
		NODE_DESC(node)->type = NODE_DIRECTORY;
	else if (S_ISREG(st.st_mode))
		NODE_DESC(node)->type = NODE_REGFILE;
#ifdef S_ISLNK
	else if (S_ISLNK(st.st_mode))
		NODE_DESC(node)->type = NODE_SYMLINK;
#endif
#ifdef S_ISFIFO
	else if (S_ISFIFO(st.st_mode))
		NODE_DESC(node)->type = NODE_FIFO;
#endif
#ifdef S_ISSOCK
	else if (S_ISSOCK(st.st_mode))
		NODE_DESC(node)->type = NODE_SOCKET;
#endif
#ifdef S_ISCHR
	else if (S_ISCHR(st.st_mode))
		NODE_DESC(node)->type = NODE_CHARDEV;
#endif
#ifdef S_ISBLK
	else if (S_ISBLK(st.st_mode))
		NODE_DESC(node)->type = NODE_BLOCKDEV;
#endif
	else
		NODE_DESC(node)->type = NODE_UNKNOWN;

	NODE_DESC(node)->size = st.st_size;
#ifdef HAVE_STRUCT_STAT_ST_BLOCKS
	NODE_DESC(node)->size_alloc = 512 * (int64)st.st_blocks;
#else
	NODE_DESC(node)->size_alloc = st.st_size;
#endif
	NODE_DESC(node)->atime = st.st_atime;
	NODE_DESC(node)->mtime = st.st_mtime;
	NODE_DESC(node)->ctime = st.st_ctime;

	return 0;
}


/* scandir( ) selector: let everything through except "." and ".." */
static int
de_select( const struct dirent *de )
{
	if (de->d_name[0] != '.')
		return 1;
	if (de->d_name[1] == '\0')
		return 0;
	if (de->d_name[1] != '.')
		return 1;
	if (de->d_name[2] == '\0')
		return 0;

	return 1;
}


static int
process_dir( const char *dir, GNode *dnode )
{
	union AnyNodeDesc any_node_desc, *andesc;
	struct dirent **dir_entries;
	GNode *node;
	int num_entries, i;

	num_entries = fsv3ds_scandir( dir, &dir_entries, de_select, fsv3ds_alphasort );
	if (num_entries < 0)
		return -1;

	printf( "scanning: %s\n", dir );

	for (i = 0; i < num_entries; i++) {
		node = g_node_prepend_data( dnode, &any_node_desc );
		NODE_DESC(node)->id = node_id;
		NODE_DESC(node)->name = g_string_chunk_insert( name_strchunk, dir_entries[i]->d_name );

		if (stat_node( node )) {
			g_node_unlink( node );
			g_node_destroy( node );
			free( dir_entries[i] );
			continue;
		}
		++node_id;

		if (NODE_IS_DIR(node)) {
			DIR_NODE_DESC(node)->ctnode = NULL;
			DIR_NODE_DESC(node)->a_dlist = NULL_DLIST;
			DIR_NODE_DESC(node)->b_dlist = NULL_DLIST;
			DIR_NODE_DESC(node)->c_dlist = NULL_DLIST;

			process_dir( node_absname( node ), node );

			andesc = g_mem_chunk_alloc( dir_ndesc_memchunk );
			memcpy( andesc, DIR_NODE_DESC(node), sizeof(DirNodeDesc) );
			node->data = andesc;
		}
		else {
			andesc = g_mem_chunk_alloc( ndesc_memchunk );
			memcpy( andesc, NODE_DESC(node), sizeof(NodeDesc) );
			node->data = andesc;
		}

		++node_counts[NODE_DESC(node)->type];
		size_counts[NODE_DESC(node)->type] += NODE_DESC(node)->size;

		free( dir_entries[i] );
	}

	free( dir_entries );

	return 0;
}


/* Compare function for sorting nodes: directories first, then larger
 * to smaller, then alphabetically A-Z. Directories must always sort
 * before leaves -- geometry.c's recursion (Phase 2) depends on it. */
static int
compare_node( NodeDesc *a, NodeDesc *b )
{
	int64 a_size, b_size;
	int s = 0;

	a_size = a->size;
	if (a->type == NODE_DIRECTORY) {
		a_size += ((DirNodeDesc *)a)->subtree.size;
		s -= 2;
	}

	b_size = b->size;
	if (b->type == NODE_DIRECTORY) {
		b_size += ((DirNodeDesc *)b)->subtree.size;
		s += 2;
	}

	if (a_size > b_size)
		--s;
	if (a_size < b_size)
		++s;
	if (!s)
		return strcmp( a->name, b->name );

	return s;
}


/* Post-scan housekeeping: sorts everything, assigns subtree size/count
 * information to directory nodes, sets up the node table */
static void
setup_fstree_recursive( GNode *node, GNode **node_table )
{
	GNode *child_node;
	int i;

	node_table[NODE_DESC(node)->id] = node;

	if (NODE_IS_DIR(node) || NODE_IS_METANODE(node)) {
		DIR_NODE_DESC(node)->subtree.size = 0;
		for (i = 0; i < NUM_NODE_TYPES; i++)
			DIR_NODE_DESC(node)->subtree.counts[i] = 0;

		child_node = node->children;
		while (child_node != NULL) {
			setup_fstree_recursive( child_node, node_table );
			child_node = child_node->next;
		}
	}

	if (!NODE_IS_METANODE(node)) {
		DIR_NODE_DESC(node->parent)->subtree.size += NODE_DESC(node)->size;
		++DIR_NODE_DESC(node->parent)->subtree.counts[NODE_DESC(node)->type];
	}

	if (NODE_IS_DIR(node)) {
		g_node_sort_children( node, (GCompareFunc)compare_node );
		DIR_NODE_DESC(node->parent)->subtree.size += DIR_NODE_DESC(node)->subtree.size;
		for (i = 0; i < NUM_NODE_TYPES; i++)
			DIR_NODE_DESC(node->parent)->subtree.counts[i] += DIR_NODE_DESC(node)->subtree.counts[i];
	}
}


GNode **
scanfs( const char *dir, unsigned int *out_node_count )
{
	const char *root_dir;
	GNode **node_table;
	char *name;
	int i;

	if (globals.fstree != NULL) {
		/* NOTE: once geometry.c exists (Phase 2), this needs to call
		 * geometry_free_recursive(globals.fstree) first to release
		 * render state before the tree itself is torn down. */
		g_node_destroy( globals.fstree );
		globals.fstree = NULL;
	}

	if (ndesc_memchunk == NULL)
		ndesc_memchunk = g_mem_chunk_create( NodeDesc, 64, G_ALLOC_ONLY );
	else
		g_mem_chunk_reset( ndesc_memchunk );
	if (dir_ndesc_memchunk == NULL)
		dir_ndesc_memchunk = g_mem_chunk_create( DirNodeDesc, 16, G_ALLOC_ONLY );
	else
		g_mem_chunk_reset( dir_ndesc_memchunk );

	if (name_strchunk != NULL)
		g_string_chunk_free( name_strchunk );
	name_strchunk = g_string_chunk_new( 8192 );

	node_id = 0;
	for (i = 0; i < NUM_NODE_TYPES; i++) {
		node_counts[i] = 0;
		size_counts[i] = 0;
	}

	chdir( dir );
	root_dir = xgetcwd( );

	globals.fstree = g_node_new( g_mem_chunk_alloc( dir_ndesc_memchunk ) );
	NODE_DESC(globals.fstree)->type = NODE_METANODE;
	NODE_DESC(globals.fstree)->id = node_id++;
	name = g_dirname( root_dir );
	NODE_DESC(globals.fstree)->name = g_string_chunk_insert( name_strchunk, name );
	g_free( name );
	DIR_NODE_DESC(globals.fstree)->ctnode = NULL;
	DIR_NODE_DESC(globals.fstree)->a_dlist = NULL_DLIST;
	DIR_NODE_DESC(globals.fstree)->b_dlist = NULL_DLIST;
	DIR_NODE_DESC(globals.fstree)->c_dlist = NULL_DLIST;

	g_node_append_data( globals.fstree, g_mem_chunk_alloc( dir_ndesc_memchunk ) );
	NODE_DESC(root_dnode)->id = node_id++;
	name = g_basename( root_dir );
	NODE_DESC(root_dnode)->name = g_string_chunk_insert( name_strchunk, name );
	DIR_NODE_DESC(root_dnode)->a_dlist = NULL_DLIST;
	DIR_NODE_DESC(root_dnode)->b_dlist = NULL_DLIST;
	DIR_NODE_DESC(root_dnode)->c_dlist = NULL_DLIST;
	stat_node( root_dnode );

	process_dir( root_dir, root_dnode );

	node_table = NEW_ARRAY(GNode *, node_id);
	setup_fstree_recursive( globals.fstree, node_table );

	rpc_logf( "scan complete: %u nodes\n", node_id );
	for (i = 1; i < NUM_NODE_TYPES; i++) {
		if (node_counts[i] > 0)
			rpc_logf( "  %-18s %6d  %s\n", node_type_plural_names[i],
				node_counts[i], abbrev_size( size_counts[i] ) );
	}

	*out_node_count = node_id;
	return node_table;
}

/* end scanfs.c */
