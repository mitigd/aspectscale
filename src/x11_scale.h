#ifndef X11_SCALE_H
#define X11_SCALE_H

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/extensions/Xrandr.h>
#include <stdbool.h>
#include <stddef.h>

typedef struct {
    int x;
    int y;
    int width;
    int height;
} Rect;

typedef struct {
    Window window;
    char title[256];
    bool was_scaled;
    Rect old_geom;      /* Client geometry before scaling */
    Rect new_geom;      /* Scaled geometry */
    double aspect_ratio;
    bool is_restored;
} WindowScaleInfo;

/* Initialize X11 atoms and resources */
bool x11_scale_init(Display *dpy);

/* Cleanup internal state/atoms/backdrops */
void x11_scale_cleanup(Display *dpy);

/* Find the currently focused/active window according to EWMH */
Window x11_get_active_window(Display *dpy);

/* Get human readable title of window */
bool x11_get_window_title(Display *dpy, Window w, char *buf, size_t max_len);

/* Get current window client geometry and screen coordinates */
bool x11_get_window_rect(Display *dpy, Window w, Rect *rect);

/* Get monitor bounds containing the window or mouse */
bool x11_get_target_monitor(Display *dpy, Window w, Rect *out_mon);

/* Scale window to borderless fullscreen with aspect ratio correction */
bool x11_scale_window(Display *dpy, Window w, WindowScaleInfo *info);

/* Restore window to its pre-scaled windowed size/position and decorations */
bool x11_restore_window(Display *dpy, Window w, WindowScaleInfo *info);

/* Toggle: if window was previously scaled by us, restore it; otherwise scale it */
bool x11_toggle_scale_window(Display *dpy, Window w, WindowScaleInfo *info);

/* Grab global hotkey on root window with all modifier permutations (NumLock, CapsLock) */
bool x11_grab_key(Display *dpy, unsigned int keycode, unsigned int modifiers);

/* Ungrab global hotkey */
void x11_ungrab_key(Display *dpy, unsigned int keycode, unsigned int modifiers);

#include <sys/types.h>

/* Get PID of the process owning the window */
pid_t x11_get_window_pid(Display *dpy, Window w);

/* Get process executable name or wine target exe for a window */
bool x11_get_window_exe(Display *dpy, Window w, char *buf, size_t max_len, bool *out_is_wine);

#endif /* X11_SCALE_H */
