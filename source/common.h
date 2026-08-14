/* common.h - fsv3ds Phase 1
 *
 * Trimmed port of fsv/src/common.h. Dropped for now (deferred to the
 * phase that needs them):
 *   - node_type_xpms / mini_xpms, NodeInfo, get_node_info(), node_named(),
 *     rgb2hex/hex2rgb/rainbow_color/heat_color, g_list_replace()
 *     -> come back with color.c / dialog.c in a later phase.
 *   - uid_t/gid_t owner fields on NodeDesc -> the SD card has no Unix
 *     ownership, so these are meaningless on 3DS and were removed
 *     rather than stubbed.
 */
#ifndef FSV3DS_COMMON_H
#define FSV3DS_COMMON_H

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>

#include "compat/gtypes.h"
#include "compat/gnode.h"
#include "compat/glist.h"
#include "compat/gmisc.h"

/* No gettext on 3DS */
#define _(string)  (string)
#define __(string) (string)

/* Mathematical constants et al. */
#define PI          3.14159265358979323846
#define EPSILON     1.0e-6
#define NULL_DLIST  0

/* Alias for the root directory node */
#define root_dnode  globals.fstree->children

#ifndef MIN
#define MIN(a,b) ((a) < (b) ? (a) : (b))
#endif
#ifndef MAX
#define MAX(a,b) ((a) > (b) ? (a) : (b))
#endif
#ifndef CLAMP
#define CLAMP(x,lo,hi) ((x) < (lo) ? (lo) : ((x) > (hi) ? (hi) : (x)))
#endif

#define SQR(x)     ((x)*(x))
#define RAD(deg)   ((deg) * (PI / 180.0))
#define DEG(rad)   ((rad) * (180.0 / PI))

#define NEW(type)              (type *)xmalloc( sizeof(type) )
#define NEW_ARRAY(type,n)      (type *)xmalloc( (n) * sizeof(type) )
#define RESIZE(block,n,type)   block = (type *)xrealloc( block, (n) * sizeof(type) )

#define G_LIST_APPEND(l,d)     l = g_list_append( l, d )

/* Nonstandard but nice */
typedef gint64 int64;
typedef unsigned int bitfield;
typedef guint8 byte;
typedef gboolean boolean;

/* The various types of nodes */
typedef enum {
	NODE_METANODE,
	NODE_DIRECTORY,
	NODE_REGFILE,
	NODE_SYMLINK,
	NODE_FIFO,
	NODE_SOCKET,
	NODE_CHARDEV,
	NODE_BLOCKDEV,
	NODE_UNKNOWN,
	NUM_NODE_TYPES
} NodeType;

/* Returns information about the given node */
#define NODE_DESC(node)         ((NodeDesc *)(node)->data)
#define DIR_NODE_DESC(dnode)    ((DirNodeDesc *)(dnode)->data)
#define NODE_IS_DIR(node)       (NODE_DESC(node)->type == NODE_DIRECTORY)
#define NODE_IS_METANODE(node)  (NODE_DESC(node)->type == NODE_METANODE)

/* Base node descriptor. Describes a filesystem node
 * (file/symlink/whatever) */
typedef struct _NodeDesc NodeDesc;
struct _NodeDesc {
	NodeType    type;          /* Type of node */
	unsigned int id;           /* Unique ID number */
	const char  *name;         /* Base name (w/o directory) */
	int64       size;          /* Size (bytes) */
	int64       size_alloc;    /* Size allocation on storage medium */
	bitfield    perms : 10;    /* Permission flags */
	bitfield    flags : 2;     /* Extra (mode-specific) flags */
	time_t      atime;         /* Last access time */
	time_t      mtime;         /* Last modification time */
	time_t      ctime;         /* Last attribute change time */
	const void  *color;        /* Node color (RGBcolor*, unused until Phase 4) */
	double      geomparams[5]; /* Geometry parameters (Phase 2) */
};

/* Directories have their own extended descriptor */
typedef struct _DirNodeDesc DirNodeDesc;
struct _DirNodeDesc {
	NodeDesc node_desc;
	double   geomparams2[3];  /* More geometry parameters (Phase 2) */
	double   deployment;      /* 0 == collapsed, 1 == expanded */
	/* Subtree information. Does not include the contribution of the
	 * root of the subtree (i.e. THIS node) */
	struct {
		int64        size;
		unsigned int counts[NUM_NODE_TYPES];
	} subtree;
	void         *ctnode;       /* Directory tree UI entry (Phase 3) */
	unsigned int a_dlist;       /* Display list A (Phase 2) */
	unsigned int b_dlist;       /* Display list B (Phase 2) */
	unsigned int c_dlist;       /* Display list C (Phase 2) */
	bitfield     geom_expanded : 1;
	bitfield     a_dlist_stale : 1;
	bitfield     b_dlist_stale : 1;
	bitfield     c_dlist_stale : 1;
};

/* Generalized node descriptor */
union AnyNodeDesc {
	NodeDesc    node_desc;
	DirNodeDesc dir_node_desc;
};

/* Global variables container */
struct Globals {
	GNode *fstree;         /* The filesystem tree */
	GNode *current_node;   /* Current node of interest */
	GList *history;        /* History of previously visited nodes (GNode*) */
	boolean need_redraw;
};

extern struct Globals globals;
extern const char *node_type_names[NUM_NODE_TYPES];
extern const char *node_type_plural_names[NUM_NODE_TYPES];

/* Common library functions */
void *xmalloc( size_t size );
void *xrealloc( void *block, size_t size );
char *xstrdup( const char *string );
char *xstrredup( char *old_string, const char *string );
void xfree( void *block );
char *strrecat( char *string, const char *add_string );
char *xstrstrip( char *string );
const char *xgetcwd( void );
const char *i64toa( int64 number );
const char *abbrev_size( int64 size );
const char *node_absname( GNode *node );
void quit( const char *message );

#endif /* FSV3DS_COMMON_H */
