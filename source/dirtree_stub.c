/* dirtree_stub.c - see dirtree_stub.h */
#include "common.h"
#include "dirtree_stub.h"
#include "nav.h"

/* TreeV needs more than just "== view_root" here: a platform's
 * absolute radius/angle (treev.c's treev_platform_r0()/_theta()) is a
 * running sum walked up the ancestor chain, and upstream's TreeV
 * layout (treev_arrange_recursive) only computes a directory's
 * platform.depth/theta when that directory is itself "expanded" --
 * so view_root's own ancestors need to read as expanded too, or the
 * walk sums in never-computed (garbage) depths. Widening the rule from
 * "== view_root" to "== view_root, or one of its ancestors" fixes
 * that; it's provably a no-op for MapV, which only ever asks this
 * about view_root's *children* (a child can never be its own
 * ancestor, so the answer for those is unchanged either way). */
boolean
dirtree_entry_expanded( GNode *dnode )
{
	GNode *up;

	for (up = nav_view_root( ); up != NULL; up = up->parent) {
		if (up == dnode)
			return TRUE;
	}

	return FALSE;
}
