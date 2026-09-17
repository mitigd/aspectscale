#ifndef GL_SCALER_H
#define GL_SCALER_H

#include <X11/Xlib.h>
#include <stdbool.h>
#include "config.h"

typedef enum {
    SCALER_MODE_FULLSCREEN = 0,
    SCALER_MODE_WINDOWED = 1
} ScalerTargetMode;

bool gl_scaler_init(Display *dpy);
void gl_scaler_cleanup(void);

bool gl_scaler_is_running(void);
Window gl_scaler_get_target(void);
ScalerTargetMode gl_scaler_get_mode(void);
int gl_scaler_get_factor(void);
int gl_scaler_get_max_factor(Display *dpy, Window target);

bool gl_scaler_start(Display *dpy, Window target_win, ScalerTargetMode mode, int factor);
bool gl_scaler_start_fullscreen(Display *dpy, Window target_win);
bool gl_scaler_start_windowed(Display *dpy, Window target_win, int factor);
void gl_scaler_stop(void);
bool gl_scaler_set_factor(int factor);

void gl_scaler_set_hide_cursor(bool hide);
void gl_scaler_set_scale_mode(ScaleFilterMode mode);
ScaleFilterMode gl_scaler_get_scale_mode(void);

#endif /* GL_SCALER_H */
