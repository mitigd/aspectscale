#ifndef GL_SCALER_H
#define GL_SCALER_H

#include <X11/Xlib.h>
#include <stdbool.h>
#include "config.h"

bool gl_scaler_init(Display *dpy);
void gl_scaler_cleanup(void);

bool gl_scaler_is_running(void);
Window gl_scaler_get_target(void);
bool gl_scaler_start(Display *dpy, Window target_win);
void gl_scaler_stop(void);
bool gl_scaler_toggle(Display *dpy, Window target_win);

void gl_scaler_set_hide_cursor(bool hide);
void gl_scaler_set_scale_mode(ScaleFilterMode mode);
ScaleFilterMode gl_scaler_get_scale_mode(void);

#endif /* GL_SCALER_H */
