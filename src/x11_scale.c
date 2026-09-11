#include "x11_scale.h"
#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <X11/keysym.h>

#define MAX_HISTORY 64

struct MotifHints {
    unsigned long flags;
    unsigned long functions;
    unsigned long decorations;
    long input_mode;
    unsigned long status;
};

#define MWM_HINTS_DECORATIONS (1L << 1)

typedef struct {
    Window window;
    Rect original_geom;
    Rect scaled_geom;
    bool has_saved_hints;
    struct MotifHints orig_hints;
    bool was_maximized;
    Window backdrop;
    bool is_scaled;
    time_t timestamp;
} SavedWindowState;

static SavedWindowState s_history[MAX_HISTORY];
static int s_history_count = 0;

/* Cached Atoms */
static Atom atom_net_active_window = None;
static Atom atom_net_wm_name = None;
static Atom atom_utf8_string = None;
static Atom atom_net_wm_state = None;
static Atom atom_net_wm_state_max_vert = None;
static Atom atom_net_wm_state_max_horz = None;
static Atom atom_net_wm_state_fullscreen = None;
static Atom atom_net_wm_state_above = None;
static Atom atom_net_moveresize_window = None;
static Atom atom_net_wm_window_type = None;
static Atom atom_net_wm_window_type_desktop = None;
static Atom atom_net_wm_window_type_dock = None;
static Atom atom_motif_wm_hints = None;

static SavedWindowState* find_history_entry(Window w) {
    for (int i = 0; i < s_history_count; i++) {
        if (s_history[i].window == w) {
            return &s_history[i];
        }
    }
    return NULL;
}

static SavedWindowState* get_or_create_history_entry(Window w) {
    SavedWindowState *entry = find_history_entry(w);
    if (entry) {
        return entry;
    }
    if (s_history_count < MAX_HISTORY) {
        entry = &s_history[s_history_count++];
    } else {
        /* Evict oldest */
        int oldest_idx = 0;
        time_t oldest_time = s_history[0].timestamp;
        for (int i = 1; i < MAX_HISTORY; i++) {
            if (s_history[i].timestamp < oldest_time) {
                oldest_time = s_history[i].timestamp;
                oldest_idx = i;
            }
        }
        entry = &s_history[oldest_idx];
    }
    memset(entry, 0, sizeof(SavedWindowState));
    entry->window = w;
    return entry;
}

static Window get_toplevel_parent(Display *dpy, Window root, Window w) {
    Window curr = w;
    Window parent = None, r = None, *children = NULL;
    unsigned int nchildren;
    while (XQueryTree(dpy, curr, &r, &parent, &children, &nchildren) && parent != root && parent != None) {
        if (children) XFree(children);
        curr = parent;
    }
    if (children) XFree(children);
    return curr;
}

bool x11_scale_init(Display *dpy) {
    if (!dpy) return false;

    atom_net_active_window = XInternAtom(dpy, "_NET_ACTIVE_WINDOW", False);
    atom_net_wm_name = XInternAtom(dpy, "_NET_WM_NAME", False);
    atom_utf8_string = XInternAtom(dpy, "UTF8_STRING", False);
    atom_net_wm_state = XInternAtom(dpy, "_NET_WM_STATE", False);
    atom_net_wm_state_max_vert = XInternAtom(dpy, "_NET_WM_STATE_MAXIMIZED_VERT", False);
    atom_net_wm_state_max_horz = XInternAtom(dpy, "_NET_WM_STATE_MAXIMIZED_HORZ", False);
    atom_net_wm_state_fullscreen = XInternAtom(dpy, "_NET_WM_STATE_FULLSCREEN", False);
    atom_net_wm_state_above = XInternAtom(dpy, "_NET_WM_STATE_ABOVE", False);
    atom_net_moveresize_window = XInternAtom(dpy, "_NET_MOVERESIZE_WINDOW", False);
    atom_net_wm_window_type = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE", False);
    atom_net_wm_window_type_desktop = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_DESKTOP", False);
    atom_net_wm_window_type_dock = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_DOCK", False);
    atom_motif_wm_hints = XInternAtom(dpy, "_MOTIF_WM_HINTS", False);

    return true;
}

void x11_scale_cleanup(Display *dpy) {
    if (!dpy) return;
    for (int i = 0; i < s_history_count; i++) {
        if (s_history[i].is_scaled) {
            x11_restore_window(dpy, s_history[i].window, NULL);
        }
        if (s_history[i].backdrop != None) {
            XDestroyWindow(dpy, s_history[i].backdrop);
            s_history[i].backdrop = None;
        }
    }
    s_history_count = 0;
}

Window x11_get_active_window(Display *dpy) {
    if (!dpy) return None;

    Window root = DefaultRootWindow(dpy);
    Window active_win = None;

    Atom actual_type;
    int actual_format;
    unsigned long nitems, bytes_after;
    unsigned char *prop = NULL;

    if (XGetWindowProperty(dpy, root, atom_net_active_window, 0, 1, False,
                           XA_WINDOW, &actual_type, &actual_format,
                           &nitems, &bytes_after, &prop) == Success) {
        if (prop && nitems > 0) {
            active_win = *(Window *)prop;
        }
        if (prop) XFree(prop);
    }

    if (active_win == None || active_win == root) {
        int revert_to;
        Window focus_win = None;
        XGetInputFocus(dpy, &focus_win, &revert_to);
        if (focus_win != PointerRoot && focus_win != None && focus_win != root) {
            active_win = focus_win;
        }
    }

    if (active_win == None || active_win == root) {
        return None;
    }

    /* Ignore desktop or dock windows */
    if (XGetWindowProperty(dpy, active_win, atom_net_wm_window_type, 0, 4, False,
                           XA_ATOM, &actual_type, &actual_format,
                           &nitems, &bytes_after, &prop) == Success) {
        if (prop && nitems > 0) {
            Atom *types = (Atom *)prop;
            for (unsigned long i = 0; i < nitems; i++) {
                if (types[i] == atom_net_wm_window_type_desktop ||
                    types[i] == atom_net_wm_window_type_dock) {
                    XFree(prop);
                    return None;
                }
            }
        }
        if (prop) XFree(prop);
    }

    return active_win;
}

bool x11_get_window_title(Display *dpy, Window w, char *buf, size_t max_len) {
    if (!dpy || w == None || !buf || max_len == 0) return false;
    buf[0] = '\0';

    Atom actual_type;
    int actual_format;
    unsigned long nitems, bytes_after;
    unsigned char *prop = NULL;

    if (XGetWindowProperty(dpy, w, atom_net_wm_name, 0, (max_len / 4) + 1, False,
                           atom_utf8_string, &actual_type, &actual_format,
                           &nitems, &bytes_after, &prop) == Success) {
        if (prop && nitems > 0) {
            snprintf(buf, max_len, "%s", (char *)prop);
            XFree(prop);
            return true;
        }
        if (prop) XFree(prop);
    }

    XTextProperty text_prop;
    if (XGetWMName(dpy, w, &text_prop) && text_prop.value) {
        snprintf(buf, max_len, "%s", (char *)text_prop.value);
        if (text_prop.value) XFree(text_prop.value);
        return true;
    }

    snprintf(buf, max_len, "Window [0x%lx]", (unsigned long)w);
    return true;
}

bool x11_get_window_rect(Display *dpy, Window w, Rect *rect) {
    if (!dpy || w == None || !rect) return false;

    Window root_return;
    int win_x, win_y;
    unsigned int win_w, win_h, border, depth;

    if (!XGetGeometry(dpy, w, &root_return, &win_x, &win_y, &win_w, &win_h, &border, &depth)) {
        return false;
    }

    Window child;
    int abs_x = 0, abs_y = 0;
    if (!XTranslateCoordinates(dpy, w, root_return, 0, 0, &abs_x, &abs_y, &child)) {
        abs_x = win_x;
        abs_y = win_y;
    }

    rect->x = abs_x;
    rect->y = abs_y;
    rect->width = (int)win_w;
    rect->height = (int)win_h;
    return true;
}

bool x11_get_target_monitor(Display *dpy, Window w, Rect *out_mon) {
    if (!dpy || !out_mon) return false;

    Window root = DefaultRootWindow(dpy);
    int screen_w = DisplayWidth(dpy, DefaultScreen(dpy));
    int screen_h = DisplayHeight(dpy, DefaultScreen(dpy));

    out_mon->x = 0;
    out_mon->y = 0;
    out_mon->width = screen_w;
    out_mon->height = screen_h;

    Rect win_rect;
    int center_x = screen_w / 2;
    int center_y = screen_h / 2;
    if (w != None && x11_get_window_rect(dpy, w, &win_rect)) {
        center_x = win_rect.x + win_rect.width / 2;
        center_y = win_rect.y + win_rect.height / 2;
    }

    XRRScreenResources *res = XRRGetScreenResourcesCurrent(dpy, root);
    if (!res) {
        res = XRRGetScreenResources(dpy, root);
    }

    if (res) {
        bool found = false;
        for (int i = 0; i < res->ncrtc; i++) {
            XRRCrtcInfo *crtc = XRRGetCrtcInfo(dpy, res, res->crtcs[i]);
            if (crtc) {
                if (crtc->mode != None && crtc->width > 0 && crtc->height > 0) {
                    if (center_x >= crtc->x && center_x < (crtc->x + (int)crtc->width) &&
                        center_y >= crtc->y && center_y < (crtc->y + (int)crtc->height)) {
                        out_mon->x = crtc->x;
                        out_mon->y = crtc->y;
                        out_mon->width = crtc->width;
                        out_mon->height = crtc->height;
                        found = true;
                    }
                }
                XRRFreeCrtcInfo(crtc);
                if (found) break;
            }
        }
        XRRFreeScreenResources(res);
        if (found) return true;
    }

    return true;
}

static void set_window_state(Display *dpy, Window w, Atom state_atom, bool add) {
    Window root = DefaultRootWindow(dpy);
    XEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = ClientMessage;
    ev.xclient.window = w;
    ev.xclient.message_type = atom_net_wm_state;
    ev.xclient.format = 32;
    ev.xclient.data.l[0] = add ? 1 : 0; /* 1 = ADD, 0 = REMOVE */
    ev.xclient.data.l[1] = (long)state_atom;
    ev.xclient.data.l[2] = 0;
    ev.xclient.data.l[3] = 1;
    XSendEvent(dpy, root, False, SubstructureNotifyMask | SubstructureRedirectMask, &ev);
    XSync(dpy, False);
}

static bool get_motif_hints(Display *dpy, Window w, struct MotifHints *hints) {
    Atom actual_type;
    int actual_format;
    unsigned long nitems, bytes_after;
    unsigned char *prop = NULL;

    if (XGetWindowProperty(dpy, w, atom_motif_wm_hints, 0, 5, False,
                           atom_motif_wm_hints, &actual_type, &actual_format,
                           &nitems, &bytes_after, &prop) == Success) {
        if (prop && nitems >= 5) {
            memcpy(hints, prop, sizeof(struct MotifHints));
            XFree(prop);
            return true;
        }
        if (prop) XFree(prop);
    }
    return false;
}

static void set_motif_decorations(Display *dpy, Window w, bool decorated) {
    struct MotifHints hints;
    memset(&hints, 0, sizeof(hints));
    hints.flags = MWM_HINTS_DECORATIONS;
    hints.decorations = decorated ? 1 : 0;
    XChangeProperty(dpy, w, atom_motif_wm_hints, atom_motif_wm_hints, 32,
                    PropModeReplace, (unsigned char *)&hints, 5);
    XSync(dpy, False);
}

bool x11_scale_window(Display *dpy, Window w, WindowScaleInfo *info) {
    if (!dpy || w == None) return false;

    if (info) {
        memset(info, 0, sizeof(WindowScaleInfo));
        info->window = w;
        x11_get_window_title(dpy, w, info->title, sizeof(info->title));
    }

    Rect cur_rect;
    if (!x11_get_window_rect(dpy, w, &cur_rect)) {
        return false;
    }

    if (cur_rect.width <= 0 || cur_rect.height <= 0) {
        return false;
    }

    Rect mon;
    x11_get_target_monitor(dpy, w, &mon);

    SavedWindowState *hist = get_or_create_history_entry(w);
    if (!hist->is_scaled) {
        hist->original_geom = cur_rect;
        hist->has_saved_hints = get_motif_hints(dpy, w, &hist->orig_hints);
    }

    if (info) {
        info->old_geom = cur_rect;
    }

    /* Aspect ratio calculation */
    double ar = (double)cur_rect.width / (double)cur_rect.height;
    if (info) info->aspect_ratio = ar;

    double mon_ar = (double)mon.width / (double)mon.height;

    int new_w, new_h, new_x, new_y;

    if (config_get_scale_mode() == SCALE_FILTER_INTEGER) {
        int scale_x = mon.width / cur_rect.width;
        int scale_y = mon.height / cur_rect.height;
        int scale = (scale_x < scale_y) ? scale_x : scale_y;
        if (scale < 1) scale = 1;
        new_w = cur_rect.width * scale;
        new_h = cur_rect.height * scale;
        if (new_w > mon.width) new_w = mon.width;
        if (new_h > mon.height) new_h = mon.height;
        new_x = mon.x + (mon.width - new_w) / 2;
        new_y = mon.y + (mon.height - new_h) / 2;
    } else if (mon_ar > ar) {
        /* Monitor is wider: pillarbox (full height, centered width) */
        new_h = mon.height;
        new_w = (int)lround((double)mon.height * ar);
        if (new_w > mon.width) new_w = mon.width;
        new_x = mon.x + (mon.width - new_w) / 2;
        new_y = mon.y;
    } else if (mon_ar < ar) {
        /* Monitor is taller: letterbox (full width, centered height) */
        new_w = mon.width;
        new_h = (int)lround((double)mon.width / ar);
        if (new_h > mon.height) new_h = mon.height;
        new_x = mon.x;
        new_y = mon.y + (mon.height - new_h) / 2;
    } else {
        /* Exact match */
        new_w = mon.width;
        new_h = mon.height;
        new_x = mon.x;
        new_y = mon.y;
    }

    /* Unmaximize if currently maximized */
    set_window_state(dpy, w, atom_net_wm_state_max_vert, false);
    set_window_state(dpy, w, atom_net_wm_state_max_horz, false);
    set_window_state(dpy, w, atom_net_wm_state_fullscreen, false);

    /* 1. Make window borderless (remove titlebar & borders) */
    set_motif_decorations(dpy, w, false);

    /* 2. Create black backdrop window if not filling 100% of the monitor */
    if (new_w < mon.width || new_h < mon.height) {
        if (hist->backdrop == None) {
            int screen = DefaultScreen(dpy);
            Window root = RootWindow(dpy, screen);
            XSetWindowAttributes attrs;
            attrs.override_redirect = True;
            attrs.background_pixel = BlackPixel(dpy, screen);

            hist->backdrop = XCreateWindow(dpy, root, mon.x, mon.y, mon.width, mon.height, 0,
                                           CopyFromParent, InputOutput, CopyFromParent,
                                           CWOverrideRedirect | CWBackPixel, &attrs);
            XMapWindow(dpy, hist->backdrop);
        }

        /* Stack backdrop right behind the top-level window frame */
        Window root = DefaultRootWindow(dpy);
        Window toplevel = get_toplevel_parent(dpy, root, w);
        XWindowChanges wc;
        wc.sibling = toplevel;
        wc.stack_mode = Below;
        XConfigureWindow(dpy, hist->backdrop, CWSibling | CWStackMode, &wc);
    } else if (hist->backdrop != None) {
        XDestroyWindow(dpy, hist->backdrop);
        hist->backdrop = None;
    }

    /* 3. Keep on top of panels */
    set_window_state(dpy, w, atom_net_wm_state_above, true);

    /* 4. Resize and move to borderless fullscreen aspect bounds */
    XMoveResizeWindow(dpy, w, new_x, new_y, (unsigned int)new_w, (unsigned int)new_h);
    XRaiseWindow(dpy, w);
    XSetInputFocus(dpy, w, RevertToPointerRoot, CurrentTime);
    XSync(dpy, False);

    /* Update history */
    hist->scaled_geom.x = new_x;
    hist->scaled_geom.y = new_y;
    hist->scaled_geom.width = new_w;
    hist->scaled_geom.height = new_h;
    hist->is_scaled = true;
    hist->timestamp = time(NULL);

    if (info) {
        info->new_geom.x = new_x;
        info->new_geom.y = new_y;
        info->new_geom.width = new_w;
        info->new_geom.height = new_h;
        info->was_scaled = true;
        info->is_restored = false;
    }

    return true;
}

bool x11_restore_window(Display *dpy, Window w, WindowScaleInfo *info) {
    if (!dpy || w == None) return false;

    SavedWindowState *hist = find_history_entry(w);
    if (!hist || !hist->is_scaled) {
        return false;
    }

    if (info) {
        memset(info, 0, sizeof(WindowScaleInfo));
        info->window = w;
        x11_get_window_title(dpy, w, info->title, sizeof(info->title));
        info->old_geom = hist->scaled_geom;
        info->new_geom = hist->original_geom;
        info->was_scaled = false;
        info->is_restored = true;
        info->aspect_ratio = (double)hist->original_geom.width / (double)hist->original_geom.height;
    }

    /* 1. Remove backdrop if active */
    if (hist->backdrop != None) {
        XDestroyWindow(dpy, hist->backdrop);
        hist->backdrop = None;
    }

    /* 2. Remove Above state */
    set_window_state(dpy, w, atom_net_wm_state_above, false);

    /* 3. Restore decorations */
    if (hist->has_saved_hints) {
        XChangeProperty(dpy, w, atom_motif_wm_hints, atom_motif_wm_hints, 32,
                        PropModeReplace, (unsigned char *)&hist->orig_hints, 5);
    } else {
        set_motif_decorations(dpy, w, true);
    }

    /* 4. Restore original window geometry */
    XMoveResizeWindow(dpy, w, hist->original_geom.x, hist->original_geom.y,
                      (unsigned int)hist->original_geom.width,
                      (unsigned int)hist->original_geom.height);
    XRaiseWindow(dpy, w);
    XSetInputFocus(dpy, w, RevertToPointerRoot, CurrentTime);
    XSync(dpy, False);

    hist->is_scaled = false;
    hist->timestamp = time(NULL);

    return true;
}

bool x11_toggle_scale_window(Display *dpy, Window w, WindowScaleInfo *info) {
    SavedWindowState *hist = find_history_entry(w);
    if (hist && hist->is_scaled) {
        return x11_restore_window(dpy, w, info);
    }
    return x11_scale_window(dpy, w, info);
}

bool x11_grab_key(Display *dpy, unsigned int keycode, unsigned int modifiers) {
    if (!dpy || keycode == 0) return false;

    unsigned int masks[] = {
        0,
        Mod2Mask,
        LockMask,
        Mod2Mask | LockMask
    };

    for (int s = 0; s < ScreenCount(dpy); s++) {
        Window root = RootWindow(dpy, s);
        for (int m = 0; m < 4; m++) {
            XGrabKey(dpy, keycode, modifiers | masks[m], root, True, GrabModeAsync, GrabModeAsync);
        }
    }
    XSync(dpy, False);
    return true;
}

void x11_ungrab_key(Display *dpy, unsigned int keycode, unsigned int modifiers) {
    if (!dpy || keycode == 0) return;

    unsigned int masks[] = {
        0,
        Mod2Mask,
        LockMask,
        Mod2Mask | LockMask
    };

    for (int s = 0; s < ScreenCount(dpy); s++) {
        Window root = RootWindow(dpy, s);
        for (int m = 0; m < 4; m++) {
            XUngrabKey(dpy, keycode, modifiers | masks[m], root);
        }
    }
    XSync(dpy, False);
}
