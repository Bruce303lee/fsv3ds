/* treev.h - fsv3ds Phase 5
 *
 * Trimmed port of geometry.c's "TreeV" mode (the cylindrical/radial
 * view -- directories are wedge-shaped platforms fanned out around a
 * central axis, with files as boxes standing on them) plus the sliver
 * of camera.c needed to frame it. Mirrors mapv.h/mapv.c's structure
 * and the same one-level-at-a-time design (see dirtree_stub.h): only
 * nav_view_root()'s direct children are ever laid out/drawn, never a
 * whole expanded tree at once, so there's no need to port
 * treev_draw_recursive()'s matrix-stack accumulation -- geometry_treev_
 * platform_r0()/_theta() already compute a node's *absolute* radius/
 * angle by walking ancestors, which is all a single-level draw needs.
 *
 * Dropped for v1 (see geometry.c's treev_draw_recursive/treev_build_dir
 * upstream for the full version): node name labels on every box
 * (reuses MapV's simplification of labeling only the selected node),
 * the folder-tab outline on collapsed directories (treev_gldraw_folder),
 * the branch/loop tubes connecting platforms across levels
 * (treev_gldraw_loop/_inbranch/_outbranch -- meaningless when only one
 * level is ever drawn), the node cursor, and display-list caching.
 */
#ifndef FSV3DS_TREEV_H
#define FSV3DS_TREEV_H

#include "compat/gnode.h"

/* Geometry parameters for a node in TreeV mode -- reuses NodeDesc's
 * geomparams[5] scratch field for leaf.{distance,theta,height} (3 of
 * the 5 doubles; leaf-only fields fit in a plain file's geomparams
 * alone), and -- for directories only -- DirNodeDesc's geomparams2[3]
 * immediately following it in memory for the platform.* fields, giving
 * 8 contiguous doubles total. Exactly upstream's own trick (see
 * geometry.h's TreeVGeomParams/TREEV_GEOM_PARAMS); common.h's
 * DirNodeDesc.geomparams2[3] field has been sitting there unused by
 * MapV (which only needed 5) since Phase 1 in anticipation of this. */
typedef struct _TreeVGeomParams TreeVGeomParams;
struct _TreeVGeomParams {
	struct {
		double distance; /* leaf's radial distance from parent platform's inner edge (r0) */
		double theta;    /* leaf's angular offset from parent's centerline, degrees */
		double height;   /* leaf box height (bottom to top, not from z=0) */
	} leaf;

	/* Directories (platforms) only */
	struct {
		double theta;             /* centerline angle, relative to parent's centerline */
		double depth;             /* radial extent: platform spans [r0, r0+depth] */
		double arc_width;         /* angular extent in degrees, centered on theta */
		double height;            /* platform top surface height, measured from z=0 */
		double subtree_arc_width; /* layout scratch -- max(own, expanded children's) arc_width */
	} platform;
};

#define TREEV_GEOM_PARAMS(node) ((TreeVGeomParams *)(NODE_DESC(node)->geomparams))

/* One GPU vertex: world-space position + baked-flat-shaded RGBA.
 * Identical shape to MapVVertex (see mapv.h) -- kept as its own type
 * rather than shared so render.c's MapV and TreeV draw paths stay
 * fully independent of each other. */
typedef struct {
	float x, y, z;
	float r, g, b, a;
} TreeVVertex;

/* One node label: world-space (Cartesian, already converted from the
 * node's native RTZ position) anchor + name -- see mapv.h's MapVLabel,
 * same role. */
typedef struct {
	float x, y, z;
	const char *name;
	gboolean is_selected;
} TreeVLabel;

/* Orbit camera framing the current view root's platform. Kept in
 * native RTZ terms (target.r/theta/z) rather than pre-converted to
 * Cartesian, mirroring upstream's TreeVCamera/treev_get_camera_position()
 * (camera.c) closely so the eye-position formula in render.c can be
 * checked directly against it. orbit_theta is the camera's own
 * relative-heading input (circle pad); the absolute heading used to
 * place the eye is target.theta + orbit_theta - 180 (see
 * treev_get_camera_position()'s comment on why it's target-relative,
 * not world-absolute -- makes sense for a radially symmetric scene). */
typedef struct {
	float orbit_theta, phi;
	float distance;
	float fov;
	float near_clip, far_clip;
	float target_r, target_theta, target_z;
	float base_distance; /* see MapVCameraState's field of the same name/purpose */
} TreeVCameraState;

/* Runs the TreeV layout engine over the already-scanned globals.fstree,
 * then builds the world-space vertex buffer for nav_view_root()'s
 * direct children and the camera framing it. Call once after a scan
 * (see mapv_reset_navigation() -- shared, mode-agnostic despite the
 * name), and again any time nav_drill_selected()/nav_go_up() change
 * the view root. Layout is re-run from scratch every call (see
 * treev.c's comment on why this can't be incremental the way upstream's
 * TREEV_NEED_REARRANGE flag is -- no animation in this port). */
void treev_build_scene( void );

const TreeVVertex *treev_vertex_data( void );
unsigned int treev_vertex_count( void );
const TreeVCameraState *treev_camera( void );

const TreeVLabel *treev_label_data( void );
unsigned int treev_label_count( void );

/* Thin wrappers around nav.c's shared identity (see nav.h) that also
 * do TreeV's own geometry/camera work -- mirrors mapv.c's
 * mapv_cycle_selection()/mapv_drill_selected()/mapv_go_up(). viz.c
 * dispatches to these instead of mapv.c's versions when TreeV is the
 * active mode. */
void treev_cycle_selection( int dir );
gboolean treev_drill_selected( void );
gboolean treev_go_up( void );

/* See mapv_camera_orbit()/mapv_camera_tick() -- same role, TreeV's own
 * camera state. */
void treev_camera_orbit( float dtheta, float dphi, float distance_factor );
void treev_camera_tick( void );

#endif /* FSV3DS_TREEV_H */
