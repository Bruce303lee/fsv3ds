/* mapv.c - fsv3ds Phase 2
 *
 * Port of geometry.c's MapV layout engine (mapv_init/mapv_init_recursive
 * are close to a 1:1 port -- squarified-treemap layout, no GL calls in
 * the original either) plus a from-scratch replacement for its GL
 * *drawing* functions (mapv_gldraw_node et al), which can't be ported
 * as-is since there's no OpenGL on 3DS.
 *
 * Rather than emulate glPushMatrix/glTranslated/glScaled generically,
 * this exploits a simplification specific to a static (non-animating)
 * v1: with no expand/collapse animation, every directory's "deployment"
 * value from dirtree_stub.h is always exactly 0.0 or 1.0, never
 * in-between, so glScaled(1,1,deployment) in the original never
 * actually scales anything visible. That leaves only a cumulative
 * Z-translate by ancestor heights -- which geometry_mapv_node_z0()
 * already computes -- so node boxes can be emitted directly in world
 * space with no matrix stack at all. If Phase 3 adds expand/collapse
 * *animation* (as opposed to just the instant toggle it needs for
 * navigation), this simplification stops holding and the draw path
 * needs revisiting.
 *
 * Also dropped for v1 (see geometry.c's mapv_draw_recursive/mapv_draw
 * upstream for the full version): node name labels (tmaptext.c isn't
 * ported), the folder-tab outline on collapsed directories
 * (mapv_gldraw_folder), the node cursor, and display-list caching
 * (the whole scene is small enough -- bounded to one tree level by
 * dirtree_stub.h -- to just rebuild every time mapv_build_scene() runs).
 */
#include "common.h"
#include "mapv.h"
#include "dirtree_stub.h"
#include "scanfs.h"
#include "rpc.h"
#include "color.h"

#include <stdlib.h>
#include <3ds.h> /* linearAlloc/linearFree -- GPU vertex buffers must live
                   * in linear-accessible memory, not the regular heap */

#define MAPV_BORDER_PROPORTION 0.01
#define MAPV_ROOT_ASPECT_RATIO 1.2

/* From upstream camera.h */
#define NEAR_TO_DISTANCE_RATIO 0.5
#define FAR_TO_NEAR_RATIO      128.0

/* Node side face offset ratios, by node type (obliqueness of side faces) */
static const float mapv_side_slant_ratios[NUM_NODE_TYPES] = {
	0.0,    /* Metanode (not used) */
	0.032,  /* Directory */
	0.064,  /* Regular file */
	0.333,  /* Symlink */
	0.0,    /* FIFO */
	0.0,    /* Socket */
	0.25,   /* Character device */
	0.25,   /* Block device */
	0.0     /* Unknown */
};

static double mapv_dir_height = 384.0;
static double mapv_leaf_height = 128.0;

/* Side faces get a darker shade of the node's own color (color.c) as a
 * cheap depth cue -- no lighting in v1. */
#define SIDE_SHADE 0.65f
/* Fallback if a node's color hasn't been assigned for some reason
 * (shouldn't happen post-Phase-4 -- color_assign_recursive() covers
 * the whole tree -- but emit_node_box() shouldn't read a NULL pointer
 * if it somehow does). Bright red so a real bug here is obvious rather
 * than silently rendering black. */
static const RGBcolor fallback_color = { 1.0f, 0.0f, 0.0f };


/* Returns the z-position of the bottom of a node (sum of ancestor heights) */
static double
geometry_mapv_node_z0( GNode *node )
{
	GNode *up_node;
	double z = 0.0;

	up_node = node->parent;
	while (up_node != NULL) {
		z += MAPV_GEOM_PARAMS(up_node)->height;
		up_node = up_node->parent;
	}

	return z;
}


/* Peak height of a directory's contents, if it were fully expanded
 * (measured relative to its own top face) */
static double
geometry_mapv_max_expanded_height( GNode *dnode )
{
	GNode *node;
	double height, max_height = 0.0;

	if (dirtree_entry_expanded( dnode )) {
		node = dnode->children;
		while (node != NULL) {
			height = MAPV_GEOM_PARAMS(node)->height;
			if (NODE_IS_DIR(node)) {
				height += geometry_mapv_max_expanded_height( node );
				max_height = MAX(max_height, height);
			}
			else {
				max_height = MAX(max_height, height);
				break;
			}
			node = node->next;
		}
	}

	return max_height;
}


/* Helper for mapv_init(). This is, in essence, the MapV layout engine --
 * a squarified treemap: lay out dnode's children as blocks whose area
 * is proportional to their (subtree) size, packed into rows sized to
 * fill dnode's own top face. Ported near-verbatim from upstream. */
static void
mapv_init_recursive( GNode *dnode )
{
	struct MapVBlock {
		GNode *node;
		double area;
	} *block, *next_first_block;
	struct MapVRow {
		struct MapVBlock *first_block;
		double area;
	} *row = NULL;
	MapVGeomParams *gparams;
	GNode *node;
	GList *block_list = NULL, *block_llink;
	GList *row_list = NULL, *row_llink;
	struct { double x, y; } dir_dims, block_dims;
	struct { double x, y; } start_pos, pos;
	double area, dir_area, total_block_area = 0.0;
	double nominal_border, border;
	double scale_factor;
	double a, b, k;
	int64 size;

	if (dirtree_entry_expanded( dnode ))
		DIR_NODE_DESC(dnode)->deployment = 1.0;
	else
		DIR_NODE_DESC(dnode)->deployment = 0.0;

	if (dnode->children == NULL)
		return;

	dir_dims.x = MAPV_NODE_WIDTH(dnode);
	dir_dims.y = MAPV_NODE_DEPTH(dnode);
	k = mapv_side_slant_ratios[NODE_DIRECTORY];
	dir_dims.x -= 2.0 * MIN(MAPV_GEOM_PARAMS(dnode)->height, k * dir_dims.x);
	dir_dims.y -= 2.0 * MIN(MAPV_GEOM_PARAMS(dnode)->height, k * dir_dims.y);

	a = MAPV_BORDER_PROPORTION * sqrt( dir_dims.x * dir_dims.y );
	b = MIN(dir_dims.x, dir_dims.y) / 3.0;
	nominal_border = MIN(a, b);

	dir_dims.x -= nominal_border;
	dir_dims.y -= nominal_border;
	dir_area = dir_dims.x * dir_dims.y;

	node = dnode->children;
	while (node != NULL) {
		size = MAX(256, NODE_DESC(node)->size);
		if (NODE_IS_DIR(node))
			size += DIR_NODE_DESC(node)->subtree.size;
		k = sqrt( (double)size ) + nominal_border;
		area = SQR(k);
		total_block_area += area;

		block = NEW(struct MapVBlock);
		block->node = node;
		block->area = area;
		G_LIST_APPEND(block_list, block);

		node = node->next;
	}

	scale_factor = dir_area / total_block_area;

	block_llink = block_list;
	while (block_llink != NULL) {
		block = (struct MapVBlock *)block_llink->data;
		block->area *= scale_factor;

		if (row == NULL) {
			row = NEW(struct MapVRow);
			row->first_block = block;
			row->area = 0.0;
			G_LIST_APPEND(row_list, row);
		}

		row->area += block->area;

		block_dims.y = row->area / dir_dims.x;
		block_dims.x = block->area / block_dims.y;

		if ((block_dims.x / block_dims.y) < 1.0)
			row = NULL;

		block_llink = block_llink->next;
	}

	start_pos.x = MAPV_NODE_CENTER_X(dnode) + 0.5 * dir_dims.x;
	start_pos.y = MAPV_NODE_CENTER_Y(dnode) + 0.5 * dir_dims.y;
	pos.y = start_pos.y;
	block_llink = block_list;
	row_llink = row_list;
	while (row_llink != NULL) {
		row = (struct MapVRow *)row_llink->data;
		block_dims.y = row->area / dir_dims.x;
		pos.x = start_pos.x;

		if (row_llink->next == NULL)
			next_first_block = NULL;
		else
			next_first_block = ((struct MapVRow *)row_llink->next->data)->first_block;

		while (block_llink != NULL) {
			block = (struct MapVBlock *)block_llink->data;
			if (block == next_first_block)
				break;
			block_dims.x = block->area / block_dims.y;

			size = MAX(256, NODE_DESC(block->node)->size);
			if (NODE_IS_DIR(block->node))
				size += DIR_NODE_DESC(block->node)->subtree.size;
			area = scale_factor * (double)size;

			k = block_dims.x + block_dims.y;
			border = 0.25 * (k - sqrt( SQR(k) - 4.0 * (block->area - area) ));

			gparams = MAPV_GEOM_PARAMS(block->node);
			gparams->c0.x = pos.x - block_dims.x + border;
			gparams->c0.y = pos.y - block_dims.y + border;
			gparams->c1.x = pos.x - border;
			gparams->c1.y = pos.y - border;

			if (NODE_IS_DIR(block->node)) {
				gparams->height = mapv_dir_height;
				mapv_init_recursive( block->node );
			}
			else
				gparams->height = mapv_leaf_height;

			pos.x -= block_dims.x;
			block_llink = block_llink->next;
		}

		pos.y -= block_dims.y;
		row_llink = row_llink->next;
	}

	block_llink = block_list;
	while (block_llink != NULL) {
		xfree( block_llink->data );
		block_llink = block_llink->next;
	}
	g_list_free( block_list );

	row_llink = row_list;
	while (row_llink != NULL) {
		xfree( row_llink->data );
		row_llink = row_llink->next;
	}
	g_list_free( row_list );
}


/* Top-level MapV layout init */
static void
mapv_init( void )
{
	MapVGeomParams *gparams;
	struct { double x, y; } root_dims;

	root_dims.y = sqrt( (double)DIR_NODE_DESC(globals.fstree)->subtree.size / MAPV_ROOT_ASPECT_RATIO );
	root_dims.x = MAPV_ROOT_ASPECT_RATIO * root_dims.y;

	MAPV_GEOM_PARAMS(globals.fstree)->height = 0.0;
	/* The metanode always shows through to root_dnode -- see dirtree_stub.h */
	DIR_NODE_DESC(globals.fstree)->deployment = 1.0;

	gparams = MAPV_GEOM_PARAMS(root_dnode);
	gparams->c0.x = -0.5 * root_dims.x;
	gparams->c0.y = -0.5 * root_dims.y;
	gparams->c1.x = 0.5 * root_dims.x;
	gparams->c1.y = 0.5 * root_dims.y;
	gparams->height = mapv_dir_height;

	mapv_init_recursive( root_dnode );
}


/* ---- vertex buffer emission ---- */

static MapVVertex *vbuf = NULL;
static unsigned int vbuf_count = 0;
static unsigned int vbuf_capacity = 0;

/* One label per drawn box -- see mapv.h. Plain xmalloc, not linearAlloc:
 * unlike vbuf this never touches the GPU, render.c just reads it. */
static MapVLabel *labels = NULL;
static unsigned int label_count = 0;

/* Phase 3 navigation state -- see mapv.h. Declared up here since
 * emit_boxes_recursive() (below) needs selected_node for highlighting. */
static GNode *view_root = NULL;
static GNode *selected_node = NULL;

#define VERTS_PER_BOX 30 /* 4 side quads + 1 top quad, 6 verts/quad */

static void
push_vertex( double x, double y, double z, const float *rgb )
{
	MapVVertex *v = &vbuf[vbuf_count++];

	v->x = (float)x;
	v->y = (float)y;
	v->z = (float)z;
	v->r = rgb[0];
	v->g = rgb[1];
	v->b = rgb[2];
	v->a = 1.0f;
}

static void
push_tri( const double *a, const double *b, const double *c, const float *rgb )
{
	push_vertex( a[0], a[1], a[2], rgb );
	push_vertex( b[0], b[1], b[2], rgb );
	push_vertex( c[0], c[1], c[2], rgb );
}

static void
push_quad( const double *a, const double *b, const double *c, const double *d, const float *rgb )
{
	/* a,b,c,d in order around the quad */
	push_tri( a, b, c, rgb );
	push_tri( a, c, d, rgb );
}


/* Blends a color toward white -- cheap "selected" highlight that needs
 * no separate outline primitive. */
static void
brighten( float *c )
{
	c[0] += (1.0f - c[0]) * 0.6f;
	c[1] += (1.0f - c[1]) * 0.6f;
	c[2] += (1.0f - c[2]) * 0.6f;
}


/* World-space equivalent of upstream's mapv_gldraw_node(): emits the
 * (possibly beveled) box for one node directly into vbuf. */
static void
emit_node_box( GNode *node, boolean is_selected )
{
	MapVGeomParams *gp = MAPV_GEOM_PARAMS(node);
	NodeType type = NODE_DESC(node)->type;
	double z0 = geometry_mapv_node_z0( node );
	double zt = z0 + gp->height;
	double dims_x = MAPV_NODE_WIDTH(node);
	double dims_y = MAPV_NODE_DEPTH(node);
	double k = mapv_side_slant_ratios[type];
	double offx = MIN(gp->height, k * dims_x);
	double offy = MIN(gp->height, k * dims_y);
	const RGBcolor *node_color = NODE_DESC(node)->color;
	float top_col[3], side_col[3];
	double bl[3], br[3], fr[3], fl[3];   /* bottom corners, z0 */
	double tbl[3], tbr[3], tfr[3], tfl[3]; /* top corners (inset), zt */

	if (node_color == NULL)
		node_color = &fallback_color;

	top_col[0] = node_color->r;
	top_col[1] = node_color->g;
	top_col[2] = node_color->b;

	side_col[0] = top_col[0] * SIDE_SHADE;
	side_col[1] = top_col[1] * SIDE_SHADE;
	side_col[2] = top_col[2] * SIDE_SHADE;

	if (is_selected) {
		brighten( top_col );
		brighten( side_col );
	}

	bl[0] = gp->c0.x; bl[1] = gp->c1.y; bl[2] = z0; /* rear-left */
	br[0] = gp->c1.x; br[1] = gp->c1.y; br[2] = z0; /* rear-right */
	fr[0] = gp->c1.x; fr[1] = gp->c0.y; fr[2] = z0; /* front-right */
	fl[0] = gp->c0.x; fl[1] = gp->c0.y; fl[2] = z0; /* front-left */

	tbl[0] = gp->c0.x + offx; tbl[1] = gp->c1.y - offy; tbl[2] = zt;
	tbr[0] = gp->c1.x - offx; tbr[1] = gp->c1.y - offy; tbr[2] = zt;
	tfr[0] = gp->c1.x - offx; tfr[1] = gp->c0.y + offy; tfr[2] = zt;
	tfl[0] = gp->c0.x + offx; tfl[1] = gp->c0.y + offy; tfl[2] = zt;

	if (vbuf_count + VERTS_PER_BOX > vbuf_capacity)
		return; /* shouldn't happen -- capacity is precomputed */

	push_quad( bl, tbl, tbr, br, side_col ); /* rear  */
	push_quad( br, tbr, tfr, fr, side_col ); /* right */
	push_quad( fr, tfr, tfl, fl, side_col ); /* front */
	push_quad( fl, tfl, tbl, bl, side_col ); /* left  */
	push_quad( tfl, tfr, tbr, tbl, top_col ); /* top  */
}


static void
push_label( GNode *node )
{
	MapVGeomParams *gp = MAPV_GEOM_PARAMS(node);
	MapVLabel *l = &labels[label_count++];

	l->x = (float)MAPV_NODE_CENTER_X(node);
	l->y = (float)MAPV_NODE_CENTER_Y(node);
	l->z = (float)(geometry_mapv_node_z0(node) + gp->height);
	l->name = NODE_DESC(node)->name;
	l->is_selected = (node == selected_node);
}


static unsigned int
count_boxes_recursive( GNode *dnode )
{
	GNode *node = dnode->children;
	unsigned int n = 0;

	while (node != NULL) {
		++n;
		if (NODE_IS_DIR(node) && dirtree_entry_expanded( node ))
			n += count_boxes_recursive( node );
		node = node->next;
	}

	return n;
}


static void
emit_boxes_recursive( GNode *dnode )
{
	GNode *node = dnode->children;

	while (node != NULL) {
		emit_node_box( node, node == selected_node );
		push_label( node );
		if (NODE_IS_DIR(node) && dirtree_entry_expanded( node ))
			emit_boxes_recursive( node );
		node = node->next;
	}
}


/* ---- camera ---- */

static MapVCameraState cam;

/* Where the camera is easing cam.target_{x,y,z}/distance toward -- see
 * mapv_camera_tick(). Kept separate from cam's own fields (which are
 * what render.c actually uses for the view matrix each frame) so
 * focusing on a node is a smooth multi-frame animation rather than an
 * instant snap. */
static float goal_target_x, goal_target_y, goal_target_z;
static float anim_peak_distance;   /* how far out CAM_ANIM_ZOOM_OUT backs up to */
static float anim_end_distance;    /* how close CAM_ANIM_ZOOM_IN settles to */
static boolean anim_cinematic;     /* does the current animation have a ZOOM_IN phase at all? */

typedef enum {
	CAM_ANIM_IDLE,     /* not animating -- mapv_camera_tick() is a no-op */
	CAM_ANIM_ZOOM_OUT, /* backing up to anim_peak_distance before panning */
	CAM_ANIM_PAN,      /* easing target_* toward goal_target_* at peak distance */
	CAM_ANIM_ZOOM_IN   /* easing distance down to anim_end_distance */
} CamAnimPhase;

static CamAnimPhase anim_phase = CAM_ANIM_IDLE;


/* Points the camera at a node's own center/top (same anchor point
 * mapv.h's label anchors use) and, if `cinematic`, choreographs a
 * zoom-out/pan/zoom-in move there instead of a flat pan -- ending
 * zoomed in close enough that the node's own box fills most of the
 * screen. Non-cinematic is used right after mapv_camera_init() resets
 * the whole-view-root framing (drill/up/rescan already gave an establishing
 * shot there; stacking a second zoom-out on top would be too much),
 * cinematic is used for mapv_cycle_selection() (nothing else just
 * *showed* you the wider scene, so the zoom-out is what sells the
 * "jumping to a different item" move). */
static void
set_camera_focus_goal( GNode *node, boolean cinematic )
{
	MapVGeomParams *gp = MAPV_GEOM_PARAMS(node);

	goal_target_x = (float)MAPV_NODE_CENTER_X(node);
	goal_target_y = (float)MAPV_NODE_CENTER_Y(node);
	goal_target_z = (float)(geometry_mapv_node_z0(node) + 0.5 * gp->height);

	anim_cinematic = cinematic;

	if (!cinematic) {
		anim_phase = CAM_ANIM_PAN;
		return;
	}

	{
		double node_span = MAX(MAPV_NODE_WIDTH(node), MAPV_NODE_DEPTH(node));
		double fill_distance = node_span * (0.5 / tan( RAD(0.5 * cam.fov) ));
		float min_end;

		/* A bit past exact-fill so the box reads as "focused on", not
		 * cropped at the edges. */
		anim_end_distance = (float)(1.25 * fill_distance);

		/* Floor relative to the *scene's* scale (base_distance, stable
		 * across live zoom), not a fixed absolute number: near-empty
		 * directories (config/save folders with barely any content --
		 * confirmed on hardware, e.g. "snes9x_3ds" got span=119 against
		 * siblings in the 1100-6000 range) get a treemap footprint so
		 * tiny that a purely size-derived end_distance put the camera
		 * almost inside the box. 6% of the overview distance keeps the
		 * "zoom in on this specific box" feeling without that. */
		min_end = cam.base_distance * 0.06f;
		if (anim_end_distance < min_end)
			anim_end_distance = min_end;

		rpc_logf( "focus: %s span=%.0f fill=%.0f end=%.0f\n",
			NODE_DESC(node)->name, node_span, fill_distance, (double)anim_end_distance );
	}
	anim_peak_distance = MAX( cam.distance, anim_end_distance ) * 1.5f;

	anim_phase = CAM_ANIM_ZOOM_OUT;
}


/* Call once per frame regardless of input -- advances whatever camera
 * animation set_camera_focus_goal() last kicked off. Frame-rate-dependent
 * (assumes a stable 60fps, which citro3d's C3D_FRAME_SYNCDRAW pacing
 * gives us) rather than delta-time-based: simpler, and fine for a
 * fixed-refresh-rate console. Each phase eases its one changing
 * quantity toward its goal and hands off to the next phase once
 * "close enough" (exponential easing is asymptotic -- it never
 * exactly arrives). */
void
mapv_camera_tick( void )
{
	const float EASE = 0.15f;

	switch (anim_phase) {
		case CAM_ANIM_ZOOM_OUT:
			cam.distance += (anim_peak_distance - cam.distance) * EASE;
			if (fabsf( anim_peak_distance - cam.distance ) < anim_peak_distance * 0.03f)
				anim_phase = CAM_ANIM_PAN;
			break;

		case CAM_ANIM_PAN: {
			float dx, dy, dz, remaining;

			cam.target_x += (goal_target_x - cam.target_x) * EASE;
			cam.target_y += (goal_target_y - cam.target_y) * EASE;
			cam.target_z += (goal_target_z - cam.target_z) * EASE;

			dx = goal_target_x - cam.target_x;
			dy = goal_target_y - cam.target_y;
			dz = goal_target_z - cam.target_z;
			remaining = sqrtf( dx * dx + dy * dy + dz * dz );
			if (remaining < cam.distance * 0.02f)
				anim_phase = anim_cinematic ? CAM_ANIM_ZOOM_IN : CAM_ANIM_IDLE;
			break;
		}

		case CAM_ANIM_ZOOM_IN:
			cam.distance += (anim_end_distance - cam.distance) * EASE;
			if (fabsf( anim_end_distance - cam.distance ) < anim_end_distance * 0.03f)
				anim_phase = CAM_ANIM_IDLE;
			break;

		case CAM_ANIM_IDLE:
		default:
			return; /* nothing changed -- skip the near/far recompute below */
	}

	cam.near_clip = (float)(NEAR_TO_DISTANCE_RATIO * cam.distance);
	cam.far_clip = (float)(FAR_TO_NEAR_RATIO * cam.near_clip);
}


/* Mirrors camera.c's camera_init(FSV_MAPV, initial_view=TRUE), but
 * generalized to frame any node (Phase 2 only ever framed root_dnode;
 * Phase 3's drill-down needs to reframe on whatever the view root is). */
static void
mapv_camera_init( GNode *for_node )
{
	double d1, d2, d;

	cam.fov = 60.0f;

	d1 = (MAPV_NODE_WIDTH(for_node)) * (0.5 / tan( RAD(0.5 * cam.fov) ));
	d2 = MAPV_GEOM_PARAMS(for_node)->height + geometry_mapv_max_expanded_height( for_node );
	d = MAX(d1, d2);

	cam.theta = 270.0f;
	/* Upstream's authentic initial_view uses phi=0 (dead eye-level).
	 * That's fine on a real desktop screen where node heights (128-384
	 * units) are comparable to typical directory footprints, but for a
	 * real-world folder the footprint (driven by total byte size) can
	 * be orders of magnitude wider than any box is tall, so an
	 * eye-level view collapses the whole scene to a sliver a few
	 * pixels tall -- confirmed on hardware, it rendered as a literal
	 * flat horizontal line. Angling the camera down trades a bit of
	 * upstream fidelity for an actually-legible v1 view. */
	cam.phi = 35.0f;
	/* Upstream's initial_view backs off to 4x the "fills the frame"
	 * distance `d` -- reasonable at its eye-level phi=0, which needs
	 * the extra margin so a wide flat scene doesn't clip at odd grazing
	 * angles, but at our steeper phi=35 it just reads as "too far
	 * back," confirmed on hardware. 1.6x still leaves a little breathing
	 * room around the edges without the default view being mostly
	 * empty background. */
	cam.distance = (float)(1.6 * d);
	cam.base_distance = cam.distance;
	cam.target_x = (float)MAPV_NODE_CENTER_X(for_node);
	cam.target_y = (float)MAPV_NODE_CENTER_Y(for_node);
	cam.target_z = (float)(geometry_mapv_node_z0(for_node) + 0.5 * MAPV_GEOM_PARAMS(for_node)->height);

	/* Snap the pan goal to match the just-reset live target, and drop
	 * any in-flight animation, for a clean baseline -- if there's a
	 * selected child (there usually is, drill/up/rescan all
	 * auto-select one), the caller calls set_camera_focus_goal() right
	 * after this returns, and the camera eases from "whole view root
	 * centered" onto "focused on the selected child" over the next few
	 * frames instead of jumping straight there. */
	goal_target_x = cam.target_x;
	goal_target_y = cam.target_y;
	goal_target_z = cam.target_z;
	anim_phase = CAM_ANIM_IDLE;

	cam.near_clip = (float)(NEAR_TO_DISTANCE_RATIO * cam.distance);
	cam.far_clip = (float)(FAR_TO_NEAR_RATIO * cam.near_clip);

	rpc_logf( "cam: node %.0fx%.0f h=%.0f\n",
		MAPV_NODE_WIDTH(for_node), MAPV_NODE_DEPTH(for_node), MAPV_GEOM_PARAMS(for_node)->height );
	rpc_logf( "cam: d1=%.0f d2=%.0f dist=%.0f\n", d1, d2, (double)cam.distance );
}


/* dtheta/dphi added directly, distance_factor multiplies -- see mapv.h */
void
mapv_camera_orbit( float dtheta, float dphi, float distance_factor )
{
	cam.theta += dtheta;
	cam.phi = CLAMP(cam.phi + dphi, 5.0f, 85.0f);
	cam.distance *= distance_factor;
	if (cam.distance < 10.0f)
		cam.distance = 10.0f;

	cam.near_clip = (float)(NEAR_TO_DISTANCE_RATIO * cam.distance);
	cam.far_clip = (float)(FAR_TO_NEAR_RATIO * cam.near_clip);
}


/* Re-emits the vertex buffer for view_root's children -- the part that
 * needs to happen both on a full scene rebuild AND whenever just the
 * selection changes (selected_node's highlight is baked into vbuf's
 * per-vertex colors at emit time, not looked up at draw time, so
 * changing selected_node alone doesn't change what's already emitted). */
static void
rebuild_vbuf( void )
{
	unsigned int nboxes = count_boxes_recursive( view_root );

	if (vbuf != NULL)
		linearFree( vbuf );
	vbuf_capacity = nboxes * VERTS_PER_BOX;
	vbuf_count = 0;
	vbuf = (MapVVertex *)linearAlloc( vbuf_capacity * sizeof(MapVVertex) );
	if (vbuf == NULL)
		quit( "Out of linear memory" );

	if (labels != NULL)
		free( labels );
	labels = (MapVLabel *)xmalloc( MAX(1u, nboxes) * sizeof(MapVLabel) );
	label_count = 0;

	emit_boxes_recursive( view_root );

	rpc_logf( "mapv: %u nodes, %u vertices\n", nboxes, vbuf_count );
}


void
mapv_build_scene( void )
{
	mapv_init( );

	/* Defense in depth against the class of bug fixed above (a caller
	 * that scans without calling mapv_reset_navigation() first leaves
	 * view_root dangling into freed memory, not NULL -- this can't
	 * detect that directly, but it can at least catch the GNode it
	 * lands on turning out not to be a directory, which is what that
	 * bug actually looked like in practice). */
	if (view_root == NULL || !NODE_IS_DIR(view_root)) {
		view_root = root_dnode;
		selected_node = root_dnode->children;
	}

	/* Start at view_root's children, not view_root itself: they're
	 * laid out to exactly tile view_root's own top face, so drawing
	 * view_root's box too just adds a tall opaque slab underneath the
	 * whole scene for no visual benefit. */
	rebuild_vbuf( );

	mapv_camera_init( view_root );
	if (selected_node != NULL)
		set_camera_focus_goal( selected_node, FALSE ); /* simple pan -- see set_camera_focus_goal()'s comment */
}


void
mapv_reset_navigation( void )
{
	view_root = root_dnode;
	selected_node = root_dnode->children;

	/* Piggybacked here (rather than called separately by every scan
	 * path) so it can't be forgotten by one of them the way the
	 * navigation reset itself once was -- see mapv_scan_and_build()'s
	 * comment. Assigns the whole tree, not just what's currently
	 * drawn, so colors are already there if you drill somewhere new. */
	color_assign_recursive( globals.fstree );
}


void
mapv_scan_and_build( const char *root )
{
	GNode **node_table;
	unsigned int node_count;

	/* Nothing uses the id->node lookup table yet (that's for touch/
	 * click picking, still unported). Free it immediately rather than
	 * making every caller manage its lifetime. */
	node_table = scanfs( root, &node_count );
	free( node_table );

	/* MUST happen before mapv_build_scene(): scanfs() just freed the
	 * entire previous tree (rescans destroy-and-rebuild from scratch),
	 * so view_root/selected_node are dangling pointers into freed
	 * memory at this point -- not NULL, so mapv_build_scene()'s own
	 * "if (view_root == NULL)" fallback doesn't catch it. Forgetting
	 * this call (main.c's own SELECT handler did, once) doesn't crash
	 * outright -- it silently reads whatever the allocator put in that
	 * freed memory next, which was a real-looking but wrong node deep
	 * in the new tree, and rendered an empty scene with no visible
	 * error. */
	mapv_reset_navigation( );

	mapv_build_scene( );
}


GNode *
mapv_view_root( void )
{
	return view_root;
}


GNode *
mapv_selected_node( void )
{
	return selected_node;
}


void
mapv_cycle_selection( int dir )
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

	rebuild_vbuf( ); /* re-bake highlight colors -- see rebuild_vbuf() comment */
	set_camera_focus_goal( selected_node, TRUE ); /* zoom-out/pan/zoom-in -- see mapv_camera_tick() */
}


/* mapv_cycle_selection() steps through view_root's children in sort
 * order, which -- since mapv_init_recursive() packs rows in exactly
 * that same order -- happens to walk the treemap row by row, left to
 * right within each row. This is the complementary move: jump
 * straight to the nearest sibling in an adjacent row (dir > 0 = the
 * row further from the camera's near side, dir < 0 = nearer), picking
 * whichever candidate is horizontally closest to the current selection
 * as a tiebreak. All of a row's blocks share an identical Y-span (the
 * layout algorithm assigns c0.y/c1.y per row, not per block), so "same
 * row" is an exact float comparison, not a fuzzy one. */
void
mapv_move_selection_row( int dir )
{
	GNode *node, *best = NULL;
	double x0, y0, best_dy = 0.0, best_dx = 0.0;

	if (selected_node == NULL || view_root == NULL)
		return;

	x0 = MAPV_NODE_CENTER_X(selected_node);
	y0 = MAPV_NODE_CENTER_Y(selected_node);

	for (node = view_root->children; node != NULL; node = node->next) {
		double x, y, dy, dx;

		if (node == selected_node)
			continue;

		x = MAPV_NODE_CENTER_X(node);
		y = MAPV_NODE_CENTER_Y(node);
		dy = y - y0;

		if (dir > 0 ? (dy <= EPSILON) : (dy >= -EPSILON))
			continue; /* same row, or the wrong direction */

		dx = fabs( x - x0 );
		if (best == NULL || fabs( dy ) < fabs( best_dy ) - EPSILON ||
			(fabs( fabs( dy ) - fabs( best_dy ) ) < EPSILON && dx < best_dx)) {
			best = node;
			best_dy = dy;
			best_dx = dx;
		}
	}

	if (best == NULL)
		return; /* already in the nearest/farthest row -- no wraparound */

	selected_node = best;
	rebuild_vbuf( );
	set_camera_focus_goal( selected_node, TRUE );
}


boolean
mapv_drill_selected( void )
{
	if (selected_node == NULL)
		return FALSE;
	if (!NODE_IS_DIR(selected_node) || selected_node->children == NULL)
		return FALSE;

	view_root = selected_node;
	selected_node = view_root->children;
	mapv_build_scene( );

	return TRUE;
}


boolean
mapv_go_up( void )
{
	GNode *came_from;

	if (view_root == NULL || view_root == root_dnode)
		return FALSE;

	came_from = view_root;
	view_root = view_root->parent;
	selected_node = came_from;
	mapv_build_scene( );

	return TRUE;
}


const MapVVertex *
mapv_vertex_data( void )
{
	return vbuf;
}


unsigned int
mapv_vertex_count( void )
{
	return vbuf_count;
}


const MapVCameraState *
mapv_camera( void )
{
	return &cam;
}


const MapVLabel *
mapv_label_data( void )
{
	return labels;
}


unsigned int
mapv_label_count( void )
{
	return label_count;
}

/* end mapv.c */
