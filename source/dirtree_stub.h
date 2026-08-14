/* dirtree_stub.h - fsv3ds Phase 2/3
 *
 * Upstream fsv's dirtree.c is the GTK directory-tree widget that tracks
 * which directories the user has expanded/collapsed, and geometry.c
 * consults it (dirtree_entry_expanded()) to decide how deep to draw.
 * There's no tree-widget UI (that's still unported -- navigation
 * happens by drilling the camera into one directory at a time instead,
 * see nav.h), so this stubs the query: exactly nav_view_root() is
 * "expanded", every other directory is collapsed. That bounds each
 * mode's draw pass to one level of the tree (view root's direct
 * children) regardless of how much is on the card, no matter how deep
 * view_root itself currently sits. Shared by both MapV and TreeV. */
#ifndef FSV3DS_DIRTREE_STUB_H
#define FSV3DS_DIRTREE_STUB_H

#include "compat/gnode.h"

boolean dirtree_entry_expanded( GNode *dnode );

#endif /* FSV3DS_DIRTREE_STUB_H */
