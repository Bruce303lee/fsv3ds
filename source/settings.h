/* settings.h - fsv3ds Phase 6
 *
 * User-facing settings exposed on the bottom-screen Settings panel
 * (ui.c): which color scheme is active (color.c) and whether/which
 * node labels are drawn (render.c's draw_labels()). Persisted to the
 * SD card so they survive relaunch -- see settings.c for the file
 * format/location.
 *
 * This module is the seam between ui.c (presentation) and the modules
 * that actually apply a setting (color.c, render.c): setters here do
 * whatever's needed to make the change visible immediately (re-run
 * color_assign_recursive() + viz_rebuild() on a scheme change) so ui.c
 * doesn't need to know those mechanics.
 */
#ifndef FSV3DS_SETTINGS_H
#define FSV3DS_SETTINGS_H

typedef enum {
	LABEL_MODE_SELECTED_ONLY,
	LABEL_MODE_ALL,
	LABEL_MODE_OFF, /* appended, not inserted -- see settings.c's file
	                 * format comment for why the numeric values of
	                 * existing entries can't move */
	NUM_LABEL_MODES
} LabelMode;

/* Loads sdmc:/3ds/fsv3ds/settings.cfg if present (defaults otherwise)
 * and applies the loaded color scheme to color.c. Call once at
 * startup, after color_init() and before the first scan/build. */
void settings_init( void );

int settings_get_color_scheme( void );

/* Applies immediately (color_assign_recursive() + viz_rebuild()) and
 * persists to the SD card. */
void settings_set_color_scheme( int scheme );
void settings_cycle_color_scheme( void );

LabelMode settings_get_label_mode( void );
void settings_set_label_mode( LabelMode mode );
void settings_cycle_label_mode( void );

const char *settings_label_mode_name( LabelMode mode );

#endif /* FSV3DS_SETTINGS_H */
