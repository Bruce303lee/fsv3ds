/* scanfs.h - fsv3ds Phase 1 */
#ifndef FSV3DS_SCANFS_H
#define FSV3DS_SCANFS_H

#include "compat/gnode.h"

/* Recursively scans dir, builds the GNode filesystem tree into
 * globals.fstree, and returns a node table indexed by node id (i.e.
 * table[NODE_DESC(node)->id] == node). *out_node_count receives the
 * table's length. Caller owns the returned array (free() it).
 *
 * This replaces upstream fsv's void scanfs(const char *dir) + its
 * viewport_pass_node_table() GUI callback -- there's no viewport yet,
 * so the table is just handed back directly. */
GNode **scanfs( const char *dir, unsigned int *out_node_count );

#endif /* FSV3DS_SCANFS_H */
