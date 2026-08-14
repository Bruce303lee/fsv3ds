/* mapv.h - fsv3ds Phase 2
 *
 * Trimmed port of geometry.c's "MapV" mode (the 3D-treemap view) plus
 * the sliver of camera.c needed to frame it. See mapv.c for what was
 * dropped and why.
 */
#ifndef FSV3DS_MAPV_H
#define FSV3DS_MAPV_H

#include "compat/gnode.h"

/* Geometry parameters for a node in MapV mode -- reuses NodeDesc's
 * geomparams[5] scratch field (5 doubles = XYvec c0 + XYvec c1 + height,
 * exactly like upstream's MapVGeomParams sized for the same field). */
typedef struct _MapVGeomParams MapVGeomParams;
struct _MapVGeomParams {
	struct { double x, y; } c0; /* 2D left/front corner */
	struct { double x, y; } c1; /* 2D right/rear corner (c1 > c0) */
	double height;
};

#define MAPV_GEOM_PARAMS(node)     ((MapVGeomParams *)(NODE_DESC(node)->geomparams))
#define MAPV_NODE_WIDTH(node)      (MAPV_GEOM_PARAMS(node)->c1.x - MAPV_GEOM_PARAMS(node)->c0.x)
#define MAPV_NODE_DEPTH(node)      (MAPV_GEOM_PARAMS(node)->c1.y - MAPV_GEOM_PARAMS(node)->c0.y)
#define MAPV_NODE_CENTER_X(node)   (0.5 * (MAPV_GEOM_PARAMS(node)->c0.x + MAPV_GEOM_PARAMS(node)->c1.x))
#define MAPV_NODE_CENTER_Y(node)   (0.5 * (MAPV_GEOM_PARAMS(node)->c0.y + MAPV_GEOM_PARAMS(node)->c1.y))

/* One GPU vertex: world-space position + baked-flat-shaded RGBA. */
typedef struct {
	float x, y, z;
	float r, g, b, a;
} MapVVertex;

/* One node label: world-space anchor (top-center of its box) + the
 * node's own name. render.c projects the anchor to screen space itself
 * (needs a *non*-Tilt projection matrix, unlike the GPU path -- see its
 * comment) and draws the text via citro2d. `name` points directly at
 * the GNode's own NODE_DESC(node)->name (string-chunk-owned, stable
 * for the node's lifetime), not a copy. */
typedef struct {
	float x, y, z;
	const char *name;
	gboolean is_selected;
} MapVLabel;

/* Orbit camera framing the current view root. theta/phi/fov are in
 * degrees, world is +Z-up. Phase 3: theta/phi/distance are now live --
 * mapv_camera_orbit() mutates them directly every frame from circle pad
 * input, no scene rebuild needed (render.c re-reads this each frame). */
typedef struct {
	float theta, phi;
	float distance;
	float fov;
	float near_clip, far_clip;
	float target_x, target_y, target_z;
	/* Set once by mapv_camera_init() to the freshly-framed distance for
	 * this view level, and never touched by mapv_camera_orbit()'s live
	 * zoom -- a stable reference so render.c can scale label text by
	 * how far the live (zoomable) `distance` has drifted from it,
	 * rather than by `distance` alone (which stays proportionally
	 * constant relative to any given box as you orbit, never actually
	 * growing/shrinking on screen the way zoom should make it feel). */
	float base_distance;
} MapVCameraState;

/* Runs the MapV layout engine over the already-scanned globals.fstree,
 * then builds the world-space vertex buffer for mapv_view_root()'s
 * direct children (see dirtree_stub.h for the one-level-deep design)
 * and the camera framing it. Call once after scanfs(), and again any
 * time mapv_drill_selected()/mapv_go_up() change the view root. */
void mapv_build_scene( void );

/* Resets navigation to the top: view root -> root_dnode, selection ->
 * its first child. MUST be called after any scanfs() call before the
 * next mapv_build_scene() -- see mapv_scan_and_build()'s comment for
 * why this can't be skipped or merged into mapv_build_scene() itself. */
void mapv_reset_navigation( void );

/* Convenience wrapper: scanfs(root) + mapv_reset_navigation() +
 * mapv_build_scene(). rpc.c's SCAN command uses this directly; main.c's
 * SELECT handler calls scanfs() itself first (it wants the result for
 * an on-screen tree dump) so it calls mapv_reset_navigation() and
 * mapv_build_scene() separately -- both paths must still call
 * mapv_reset_navigation() between scanning and building. */
void mapv_scan_and_build( const char *root );

const MapVVertex *mapv_vertex_data( void );
unsigned int mapv_vertex_count( void );
const MapVCameraState *mapv_camera( void );

const MapVLabel *mapv_label_data( void );
unsigned int mapv_label_count( void );

/* ---- Phase 3: navigation ---- */

/* The directory whose direct children are currently being drawn.
 * dirtree_stub.c's dirtree_entry_expanded() treats exactly this node
 * as expanded -- everything else is collapsed, keeping the draw pass
 * bounded to one level regardless of scan size. */
GNode *mapv_view_root( void );

/* The child of the view root highlighted for selection; NULL if the
 * view root has no children. */
GNode *mapv_selected_node( void );

/* Moves the selection to the next/previous sibling of the view root
 * (dir > 0 = next, dir < 0 = previous), wrapping around. No-op if the
 * view root has 0 or 1 children. */
void mapv_cycle_selection( int dir );

/* Jumps to the nearest sibling in an adjacent treemap row (dir > 0 =
 * the farther row, dir < 0 = the nearer row) -- see mapv.c's comment
 * for why this is a meaningfully different move from
 * mapv_cycle_selection(), not just the same thing on another button.
 * No-op (no wraparound) if already in the outermost row that way. */
void mapv_move_selection_row( int dir );

/* If the selected node is a directory with children, makes it the new
 * view root (rebuilding the scene and reframing the camera) and
 * returns TRUE. Otherwise leaves everything alone and returns FALSE. */
gboolean mapv_drill_selected( void );

/* If the view root isn't already the scan root, moves back up to its
 * parent (rebuilding the scene, reframing the camera, and selecting
 * whichever child we just came from) and returns TRUE. Otherwise
 * returns FALSE. */
gboolean mapv_go_up( void );

/* Nudges the live camera: dtheta/dphi in degrees are added directly
 * (phi clamped to [5,85] to avoid flipping through the poles);
 * distance_factor multiplies the current distance (e.g. 1.02 to zoom
 * out, 0.98 to zoom in), clamped to a sane minimum. Recomputes
 * near/far clip from the new distance so zooming can't clip the scene. */
void mapv_camera_orbit( float dtheta, float dphi, float distance_factor );

/* Call once per frame, unconditionally (not gated on input): eases the
 * live camera target toward whatever node was last selected, giving
 * selection changes (cycle/drill/up) a smooth pan onto the newly
 * selected node instead of an instant jump. */
void mapv_camera_tick( void );

#endif /* FSV3DS_MAPV_H */
