#include "gl_scaler.h"
#include "popup_input.h"
#include <stdint.h>
#include "x11_scale.h"
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/keysym.h>
#include <X11/extensions/Xcomposite.h>
#include <X11/extensions/Xrandr.h>
#include <X11/extensions/Xfixes.h>
#include <X11/Xcursor/Xcursor.h>
#include <GL/gl.h>
#include <GL/glx.h>
#include <GL/glxext.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <X11/Xproto.h>
#include <X11/extensions/record.h>
#include <string.h>
#include <unistd.h>
#include <math.h>

typedef struct {
    Window target;
    Window target_parent;
    volatile Cursor pending_cursor;
    volatile bool pending_valid;
} RecordCursorContext;

static void record_cursor_cb(XPointer closure, XRecordInterceptData *data) {
    RecordCursorContext *ctx = (RecordCursorContext *)closure;
    if (!ctx || !data) return;

    if (data->category == XRecordFromClient) {
        const unsigned char *buf = data->data;
        int len = data->data_len * 4;
        if (len >= (int)sizeof(xChangeWindowAttributesReq)) {
            const xChangeWindowAttributesReq *req = (const xChangeWindowAttributesReq *)buf;
            if (req->reqType == X_ChangeWindowAttributes && (req->valueMask & CWCursor)) {
                if (req->window == ctx->target || (ctx->target_parent != None && req->window == ctx->target_parent) ||
                    ((req->window & ~0x1fffff) == (ctx->target & ~0x1fffff))) {
                    int val_idx = 0;
                    for (int b = 0; b < 14; b++) {
                        if (req->valueMask & (1L << b)) val_idx++;
                    }
                    const CARD32 *values = (const CARD32 *)(buf + sizeof(xChangeWindowAttributesReq));
                    ctx->pending_cursor = (Cursor)values[val_idx];
                    ctx->pending_valid = true;
                }
            }
        }
    }
    XRecordFreeData(data);
}

typedef struct {
    Display *rec_dpy;
    XRecordContext rec_ctx;
    RecordCursorContext *cur_ctx;
} RecordThreadArg;

static void* record_thread_func(void *arg) {
    RecordThreadArg *rta = (RecordThreadArg *)arg;
    XRecordEnableContext(rta->rec_dpy, rta->rec_ctx, record_cursor_cb, (XPointer)rta->cur_ctx);
    return NULL;
}

typedef struct {
    Window win;
    Pixmap pix;
    GLXPixmap glx_pix;
    GLuint tex;
    int root_x;
    int root_y;
    unsigned int width;
    unsigned int height;
    bool active;
    bool internal; /* Already rendered in the target Composite image. */
} TrackedPopup;

#define MAX_TRACKED_POPUPS 16

/* X ancestry and Win32 transient ownership are different relationships. */
static bool is_descendant(Display *dpy, Window ancestor, Window w) {
    for (int depth = 0; w != None && depth < 64; depth++) {
        if (w == ancestor) return true;
        Window root, parent, *children = NULL;
        unsigned int count;
        if (!XQueryTree(dpy, w, &root, &parent, &children, &count)) return false;
        if (children) XFree(children);
        if (parent == w) break;
        w = parent;
    }
    return false;
}

/* Subscribe before enumerating, so later children cannot escape observation.
 * Seed existing mapped windows through the same classification path. */
static void watch_children(Display *dpy, Window w, int depth) {
    if (depth >= 64) return;
    XWindowAttributes wa;
    if (!XGetWindowAttributes(dpy, w, &wa)) return;
    XSelectInput(dpy, w, wa.your_event_mask | SubstructureNotifyMask |
                 StructureNotifyMask | PropertyChangeMask);
    Window root, parent, *children = NULL;
    unsigned int count;
    if (!XQueryTree(dpy, w, &root, &parent, &children, &count)) return;
    for (unsigned int i = 0; i < count; i++) {
        watch_children(dpy, children[i], depth + 1);
        XEvent ev = {0};
        ev.xmap.type = MapNotify;
        ev.xmap.window = children[i];
        ev.xmap.event = w;
        XPutBackEvent(dpy, &ev);
    }
    if (children) XFree(children);
}

/* Find the input-selecting child under a target-local point. Wine's drawable
 * client leaf may select only Exposure; its whole window owns mouse input. */
static Window pointer_receiver(Display *dpy, Window target, int x, int y, long mask) {
    Window w = target, receiver = target;
    for (int depth = 0; depth < 64; depth++) {
        XWindowAttributes wa;
        if (!XGetWindowAttributes(dpy, w, &wa)) break;
        if (wa.all_event_masks & mask) receiver = w;
        Window child;
        int wx, wy;
        if (!XTranslateCoordinates(dpy, target, w, x, y, &wx, &wy, &child) ||
            child == None || child == w) break;
        w = child;
    }
    return receiver;
}

static void forward_pointer(Display *dpy, Window root, Window target, Window receiver,
                            int x, int y, long mask, XEvent *ev) {
    Window child;
    int wx, wy, rx, ry;
    if (!XTranslateCoordinates(dpy, target, receiver, x, y, &wx, &wy, &child) ||
        !XTranslateCoordinates(dpy, target, root, x, y, &rx, &ry, &child)) return;
    if (ev->type == MotionNotify) {
        ev->xmotion.window = receiver;
        ev->xmotion.root = root;
        ev->xmotion.subwindow = None;
        ev->xmotion.x = wx; ev->xmotion.y = wy;
        ev->xmotion.x_root = rx; ev->xmotion.y_root = ry;
        ev->xmotion.same_screen = True;
    } else {
        ev->xbutton.window = receiver;
        ev->xbutton.root = root;
        ev->xbutton.subwindow = None;
        ev->xbutton.x = wx; ev->xbutton.y = wy;
        ev->xbutton.x_root = rx; ev->xbutton.y_root = ry;
        ev->xbutton.same_screen = True;
    }
    XSendEvent(dpy, receiver, False, mask, ev);
}

/* Follow explicit ownership, not resource-ID masks or process IDs: Wine's
 * IME and helper windows share those with the application. Bound the walk to
 * tolerate broken/cyclic transient hints. */
static bool is_target_subwindow(Display *dpy, Window target, Window w) {
    if (w == None || w == target) return false;
    for (int depth = 0; depth < MAX_TRACKED_POPUPS; depth++) {
        Window owner = None;
        if (!XGetTransientForHint(dpy, w, &owner) || owner == None || owner == w)
            return false;
        if (is_descendant(dpy, target, owner)) return true;
        w = owner;
    }
    return false;
}

/* Respect ICCCM input models. InputHint=False is not itself a BadMatch;
 * focusing an unmapped window is. Globally active clients choose their own
 * focus window after WM_TAKE_FOCUS. */
static void focus_client(Display *dpy, Window root, Window w, Time time) {
    XWindowAttributes wa;
    if (!XGetWindowAttributes(dpy, w, &wa) || wa.map_state != IsViewable)
        return;

    XEvent ev = {0};
    ev.xclient.type = ClientMessage;
    ev.xclient.window = w;
    ev.xclient.message_type = XInternAtom(dpy, "_NET_ACTIVE_WINDOW", False);
    ev.xclient.format = 32;
    ev.xclient.data.l[0] = 1;
    ev.xclient.data.l[1] = time;
    XSendEvent(dpy, root, False, SubstructureRedirectMask | SubstructureNotifyMask, &ev);

    Atom take_focus = XInternAtom(dpy, "WM_TAKE_FOCUS", False);
    Atom *protocols = NULL;
    int count = 0;
    if (XGetWMProtocols(dpy, w, &protocols, &count)) {
        for (int i = 0; i < count; i++) {
            if (protocols[i] != take_focus) continue;
            ev.xclient.message_type = XInternAtom(dpy, "WM_PROTOCOLS", False);
            ev.xclient.data.l[0] = take_focus;
            ev.xclient.data.l[1] = time;
            XSendEvent(dpy, w, False, NoEventMask, &ev);
            break;
        }
        XFree(protocols);
    }
    XWMHints *hints = XGetWMHints(dpy, w);
    bool accepts_input = !hints || !(hints->flags & InputHint) || hints->input;
    if (hints) XFree(hints);
    if (accepts_input) XSetInputFocus(dpy, w, RevertToPointerRoot, time);
}

static void cleanup_popup(Display *dpy, TrackedPopup *p, bool is_unmap) {
    if (!p || !p->active) return;
    if (p->tex) {
        glDeleteTextures(1, &p->tex);
        p->tex = 0;
    }
    if (p->glx_pix) {
        glXDestroyPixmap(dpy, p->glx_pix);
        p->glx_pix = 0;
    }
    if (p->pix) {
        XFreePixmap(dpy, p->pix);
        p->pix = 0;
    }
    if (is_unmap && !p->internal && p->win != None) {
        XCompositeUnredirectWindow(dpy, p->win, CompositeRedirectAutomatic);
    }
    p->win = None;
    p->active = false;
    p->internal = false;
}

/* Follow the WM's position for the real client. Never lower a managed
 * frame beneath an override-redirect overlay: the WM may raise it back into
 * its normal layer while leaving the overlay below the desktop. */
static void position_overlay(Display *dpy, Window root, Window frame,
                             Window overlay, const TrackedPopup *popups) {
    Window rr, parent, *children = NULL;
    unsigned int count = 0;
    if (!XQueryTree(dpy, root, &rr, &parent, &children, &count)) return;
    bool positioned = false, found_frame = false;
    for (unsigned int i = 0; i < count; i++) {
        if (children[i] != frame) continue;
        found_frame = true;
        unsigned int above = i + 1;
        while (above < count && children[above] != overlay) {
            bool popup = false;
            for (int j = 0; j < MAX_TRACKED_POPUPS; j++)
                if (popups && popups[j].active && popups[j].win == children[above])
                    popup = true;
            if (!popup) break;
            above++;
        }
        positioned = above < count && children[above] == overlay;
        break;
    }
    if (children) XFree(children);
    if (found_frame && !positioned) {
        XWindowChanges changes = { .sibling = frame, .stack_mode = Above };
        XConfigureWindow(dpy, overlay, CWSibling | CWStackMode, &changes);
    }
}

static Cursor create_cursor_from_xfixes(Display *dpy, XFixesCursorImage *img) {
    if (!dpy || !img || img->width == 0 || img->height == 0) return None;

    XcursorImage *xci = XcursorImageCreate(img->width, img->height);
    if (!xci) return None;

    xci->xhot = img->xhot;
    xci->yhot = img->yhot;
    for (int i = 0; i < img->width * img->height; i++) {
        xci->pixels[i] = (XcursorPixel)img->pixels[i];
    }
    Cursor c = XcursorImageLoadCursor(dpy, xci);
    XcursorImageDestroy(xci);
    return c;
}

static PFNGLXBINDTEXIMAGEEXTPROC s_glXBindTexImage = NULL;
static PFNGLXRELEASETEXIMAGEEXTPROC s_glXReleaseTexImage = NULL;
static PFNGLXSWAPINTERVALEXTPROC s_glXSwapIntervalEXT = NULL;
static PFNGLXSWAPINTERVALSGIPROC s_glXSwapIntervalSGI = NULL;
static PFNGLXSWAPINTERVALMESAPROC s_glXSwapIntervalMESA = NULL;

static volatile bool s_running = false;
static pthread_t s_thread;
static Window s_target_win = None;
static bool s_hide_cursor = true;
static volatile ScaleFilterMode s_scale_mode = SCALE_FILTER_BILINEAR;
static volatile ScalerTargetMode s_target_mode = SCALER_MODE_FULLSCREEN;
static volatile int s_scale_factor = 2;
static volatile int s_requested_factor = 0;
static volatile bool s_factor_change_pending = false;

typedef struct {
    int x;
    int y;
    int width;
    int height;
} MonitorBounds;

static MonitorBounds get_monitor_bounds(Display *dpy, Window w) {
    MonitorBounds mon = { 0, 0, DisplayWidth(dpy, DefaultScreen(dpy)), DisplayHeight(dpy, DefaultScreen(dpy)) };

    Window root = DefaultRootWindow(dpy);
    Window child;
    int abs_x = 0, abs_y = 0;
    XTranslateCoordinates(dpy, w, root, 0, 0, &abs_x, &abs_y, &child);

    Window r_ret;
    int wx, wy;
    unsigned int ww = 0, wh = 0, b, d;
    XGetGeometry(dpy, w, &r_ret, &wx, &wy, &ww, &wh, &b, &d);

    int center_x = abs_x + (int)ww / 2;
    int center_y = abs_y + (int)wh / 2;

    XRRScreenResources *res = XRRGetScreenResourcesCurrent(dpy, root);
    if (!res) res = XRRGetScreenResources(dpy, root);
    if (res) {
        for (int i = 0; i < res->ncrtc; i++) {
            XRRCrtcInfo *crtc = XRRGetCrtcInfo(dpy, res, res->crtcs[i]);
            if (crtc) {
                if (crtc->mode != None && crtc->width > 0 && crtc->height > 0) {
                    if (center_x >= crtc->x && center_x < (crtc->x + (int)crtc->width) &&
                        center_y >= crtc->y && center_y < (crtc->y + (int)crtc->height)) {
                        mon.x = crtc->x;
                        mon.y = crtc->y;
                        mon.width = crtc->width;
                        mon.height = crtc->height;
                        XRRFreeCrtcInfo(crtc);
                        break;
                    }
                }
                XRRFreeCrtcInfo(crtc);
            }
        }
        XRRFreeScreenResources(res);
    }
    return mon;
}

int gl_scaler_get_max_factor(Display *dpy, Window target) {
    if (!dpy || target == None) return 1;
    Window r_ret;
    int tx, ty;
    unsigned int tw = 0, th = 0, tb, td;
    if (!XGetGeometry(dpy, target, &r_ret, &tx, &ty, &tw, &th, &tb, &td) || tw == 0 || th == 0) {
        return 1;
    }
    MonitorBounds mon = get_monitor_bounds(dpy, target);
    int max_w = mon.width / (int)tw;
    int max_h = mon.height / (int)th;
    int max_f = (max_w < max_h) ? max_w : max_h;
    if (max_f < 1) max_f = 1;
    return max_f;
}

static void calculate_viewport(ScaleFilterMode mode, unsigned int target_w, unsigned int target_h,
                               const MonitorBounds *mon, int *out_vp_x, int *out_vp_y,
                               int *out_vp_w, int *out_vp_h, int *out_y_from_top) {
    int vp_w = 0, vp_h = 0, vp_x = 0, vp_y = 0;

    if (mode == SCALE_FILTER_INTEGER) {
        int scale_x = mon->width / (int)target_w;
        int scale_y = mon->height / (int)target_h;
        int scale = (scale_x < scale_y) ? scale_x : scale_y;
        if (scale < 1) scale = 1;

        vp_w = (int)target_w * scale;
        vp_h = (int)target_h * scale;
        if (vp_w > mon->width) vp_w = mon->width;
        if (vp_h > mon->height) vp_h = mon->height;
        vp_x = (mon->width - vp_w) / 2;
        vp_y = (mon->height - vp_h) / 2;
    } else {
        /* Aspect ratio calculation (FIT) */
        double target_ar = (double)target_w / (double)target_h;
        double mon_ar = (double)mon->width / (double)mon->height;

        if (mon_ar > target_ar) {
            vp_h = mon->height;
            vp_w = (int)lround((double)mon->height * target_ar);
            if (vp_w > mon->width) vp_w = mon->width;
            vp_x = (mon->width - vp_w) / 2;
            vp_y = 0;
        } else {
            vp_w = mon->width;
            vp_h = (int)lround((double)mon->width / target_ar);
            if (vp_h > mon->height) vp_h = mon->height;
            vp_x = 0;
            vp_y = (mon->height - vp_h) / 2;
        }
    }

    *out_vp_w = vp_w;
    *out_vp_h = vp_h;
    *out_vp_x = vp_x;
    *out_vp_y = vp_y;
    *out_y_from_top = mon->height - (vp_y + vp_h);
}

bool gl_scaler_init(Display *dpy) {
    if (!dpy) return false;

    s_glXBindTexImage = (PFNGLXBINDTEXIMAGEEXTPROC)glXGetProcAddress((const GLubyte*)"glXBindTexImageEXT");
    s_glXReleaseTexImage = (PFNGLXRELEASETEXIMAGEEXTPROC)glXGetProcAddress((const GLubyte*)"glXReleaseTexImageEXT");
    s_glXSwapIntervalEXT = (PFNGLXSWAPINTERVALEXTPROC)glXGetProcAddress((const GLubyte*)"glXSwapIntervalEXT");
    s_glXSwapIntervalSGI = (PFNGLXSWAPINTERVALSGIPROC)glXGetProcAddress((const GLubyte*)"glXSwapIntervalSGI");
    s_glXSwapIntervalMESA = (PFNGLXSWAPINTERVALMESAPROC)glXGetProcAddress((const GLubyte*)"glXSwapIntervalMESA");

    if (!s_glXBindTexImage || !s_glXReleaseTexImage) {
        fprintf(stderr, "AspectScale GL: glXBindTexImageEXT / glXReleaseTexImageEXT not available\n");
        return false;
    }
    return true;
}

void gl_scaler_cleanup(void) {
    if (s_running) {
        gl_scaler_stop();
    }
}

bool gl_scaler_is_running(void) {
    return s_running;
}

Window gl_scaler_get_target(void) {
    return s_target_win;
}

ScalerTargetMode gl_scaler_get_mode(void) {
    return s_target_mode;
}

int gl_scaler_get_factor(void) {
    return s_scale_factor;
}

bool gl_scaler_set_factor(int factor) {
    if (!s_running || s_target_mode != SCALER_MODE_WINDOWED) return false;
    s_requested_factor = factor;
    s_factor_change_pending = true;
    return true;
}

void gl_scaler_set_hide_cursor(bool hide) {
    s_hide_cursor = hide;
}

void gl_scaler_set_scale_mode(ScaleFilterMode mode) {
    s_scale_mode = mode;
}

ScaleFilterMode gl_scaler_get_scale_mode(void) {
    return s_scale_mode;
}

static void* gl_render_thread(void *arg) {
    (void)arg;
    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) {
        s_running = false;
        return NULL;
    }

    Window target = s_target_win;
    int screen = DefaultScreen(dpy);
    Window root = RootWindow(dpy, screen);

    /* Get target window initial geometry */
    Window r_ret;
    int tx = 0, ty = 0;
    unsigned int target_w = 0, target_h = 0, tb = 0, td = 0;
    if (!XGetGeometry(dpy, target, &r_ret, &tx, &ty, &target_w, &target_h, &tb, &td) ||
        target_w == 0 || target_h == 0) {
        s_running = false;
        XCloseDisplay(dpy);
        return NULL;
    }

    /* Translate target position to screen root coordinates */
    int target_orig_x = 0, target_orig_y = 0;
    Window dummy_child;
    XTranslateCoordinates(dpy, target, root, 0, 0, &target_orig_x, &target_orig_y, &dummy_child);

    MonitorBounds mon = get_monitor_bounds(dpy, target);
    ScalerTargetMode mode = s_target_mode;
    bool is_fullscreen = (mode == SCALER_MODE_FULLSCREEN);

    int max_factor = mon.width / (int)target_w;
    int max_h_factor = mon.height / (int)target_h;
    if (max_h_factor < max_factor) max_factor = max_h_factor;
    if (max_factor < 1) max_factor = 1;

    int current_factor = s_scale_factor;
    if (!is_fullscreen) {
        if (current_factor < 2) current_factor = 2;
        if (current_factor > max_factor) {
            fprintf(stderr, "AspectScale GL: Target factor %d exceeds max factor %d for screen\n",
                    current_factor, max_factor);
            s_running = false;
            XCloseDisplay(dpy);
            return NULL;
        }
        s_scale_factor = current_factor;
    }

    Window toplevel_frame = x11_get_toplevel_parent(dpy, root, target);

    /* Calculate gl_win size and position and viewport geometry */
    int win_w = 0, win_h = 0, win_x = 0, win_y = 0;
    int vp_w = 0, vp_h = 0, vp_x = 0, vp_y = 0, y_from_top = 0;

    if (is_fullscreen) {
        win_w = mon.width;
        win_h = mon.height;
        win_x = mon.x;
        win_y = mon.y;
        calculate_viewport(s_scale_mode, target_w, target_h, &mon, &vp_x, &vp_y, &vp_w, &vp_h, &y_from_top);
    } else {
        win_w = current_factor * (int)target_w;
        win_h = current_factor * (int)target_h;

        /* Each scale cycle starts centered on the target monitor. */
        win_x = mon.x + (mon.width - win_w) / 2;
        win_y = mon.y + (mon.height - win_h) / 2;
        vp_w = win_w;
        vp_h = win_h;
        vp_x = 0;
        vp_y = 0;
        y_from_top = 0;
    }

    /* Remove frame decorations from target so its borders/titlebars don't peek out */
    x11_set_motif_decorations(dpy, target, false);

    /* Position target frame centered directly under viewport so it is 100% physically covered */
    int target_under_x = win_x + vp_x + (vp_w - (int)target_w) / 2;
    int target_under_y = win_y + y_from_top + (vp_h - (int)target_h) / 2;
    XMoveWindow(dpy, toplevel_frame, target_under_x, target_under_y);
    XSync(dpy, False);

    /* Choose FBConfig that supports window and pixmap texturing */
    int fb_attribs[] = {
        GLX_RENDER_TYPE, GLX_RGBA_BIT,
        GLX_DRAWABLE_TYPE, GLX_WINDOW_BIT | GLX_PIXMAP_BIT,
        GLX_BIND_TO_TEXTURE_RGBA_EXT, True,
        GLX_BIND_TO_TEXTURE_TARGETS_EXT, GLX_TEXTURE_2D_BIT_EXT,
        GLX_DOUBLEBUFFER, True,
        None
    };

    int nconfigs = 0;
    GLXFBConfig *configs = glXChooseFBConfig(dpy, screen, fb_attribs, &nconfigs);
    if (!configs || nconfigs == 0) {
        /* Try RGB */
        fb_attribs[4] = GLX_BIND_TO_TEXTURE_RGB_EXT;
        configs = glXChooseFBConfig(dpy, screen, fb_attribs, &nconfigs);
        if (!configs || nconfigs == 0) {
            s_running = false;
            x11_set_motif_decorations(dpy, target, true);
            XMoveWindow(dpy, toplevel_frame, target_orig_x, target_orig_y);
            XRaiseWindow(dpy, target);
            XCloseDisplay(dpy);
            return NULL;
        }
    }

    GLXFBConfig fb_config = configs[0];
    XVisualInfo *vi = glXGetVisualFromFBConfig(dpy, fb_config);

    /* Control overlay geometry directly; position_overlay follows the managed
     * target's stacking layer without moving the WM's own frames. */
    XSetWindowAttributes swa;
    swa.colormap = XCreateColormap(dpy, root, vi->visual, AllocNone);
    swa.background_pixel = BlackPixel(dpy, screen);
    swa.override_redirect = True;
    swa.event_mask = StructureNotifyMask | KeyPressMask | KeyReleaseMask |
                     ButtonPressMask | ButtonReleaseMask | PointerMotionMask | FocusChangeMask;

    Window gl_win = XCreateWindow(dpy, root, win_x, win_y, win_w, win_h, 0,
                                  vi->depth, InputOutput, vi->visual,
                                  CWColormap | CWBackPixel | CWOverrideRedirect | CWEventMask, &swa);


    int dummy_ev = 0, dummy_err = 0;
    bool has_xfixes = XFixesQueryExtension(dpy, &dummy_ev, &dummy_err);

    Cursor custom_cursor = None;
    if ((!s_hide_cursor || !is_fullscreen) && has_xfixes) {
        XFixesCursorImage *img = XFixesGetCursorImage(dpy);
        if (img) {
            custom_cursor = create_cursor_from_xfixes(dpy, img);
            XFree(img);
        }
    }

    /* Cursor Setup: In windowed mode, always show pointer/game cursor! */
    Cursor blank_cursor = None;
    Pixmap blank_pix = None;
    if (is_fullscreen && s_hide_cursor) {
        blank_pix = XCreateBitmapFromData(dpy, gl_win, "\0", 1, 1);
        XColor dummy;
        memset(&dummy, 0, sizeof(dummy));
        blank_cursor = XCreatePixmapCursor(dpy, blank_pix, blank_pix, &dummy, &dummy, 0, 0);
        XDefineCursor(dpy, gl_win, blank_cursor);
    } else if (custom_cursor != None) {
        XDefineCursor(dpy, gl_win, custom_cursor);
    }

    XMapRaised(dpy, gl_win);

    /* Immediately ensure gl_win is stacked above target's frame window */
    position_overlay(dpy, root, toplevel_frame, gl_win, NULL);

    /* Track root substructure events to intercept Wine/app popup menus and new dialog windows */
    XSelectInput(dpy, root, SubstructureNotifyMask);
    XSelectInput(dpy, target, StructureNotifyMask | FocusChangeMask |
                 KeyPressMask | KeyReleaseMask);

    TrackedPopup popups[MAX_TRACKED_POPUPS];
    memset(popups, 0, sizeof(popups));
    watch_children(dpy, target, 0);
    Window pressed_receiver[256] = {0};
    struct { Window win, frame; } dialogs[MAX_TRACKED_POPUPS];
    int dialog_count = 0;
    Window active_dialog = None;
    Window active_dialog_frame = None;

    /* Keyboard input belongs to the focused application. Observing key
     * events does not consume them; the main thread owns the global hotkeys.
     * An active keyboard grab here would steal input after Alt+Tab. */
    focus_client(dpy, root, target, CurrentTime);
    XSync(dpy, False);

    /* Setup XRecord to monitor Wine's cursor updates on target window */
    RecordCursorContext rec_cur_ctx;
    memset(&rec_cur_ctx, 0, sizeof(rec_cur_ctx));
    rec_cur_ctx.target = target;
    XGetTransientForHint(dpy, target, &rec_cur_ctx.target_parent);

    Display *rec_dpy = XOpenDisplay(NULL);
    XRecordContext rec_ctx = 0;
    pthread_t rec_thread;
    bool rec_active = false;
    RecordThreadArg rta;

    if (rec_dpy) {
        XRecordRange *range = XRecordAllocRange();
        if (range) {
            range->core_requests.first = X_ChangeWindowAttributes;
            range->core_requests.last = X_ChangeWindowAttributes;
            XRecordClientSpec spec = XRecordAllClients;
            rec_ctx = XRecordCreateContext(dpy, 0, &spec, 1, &range, 1);
            XFree(range);
            XSync(dpy, False);

            if (rec_ctx != 0) {
                rta.rec_dpy = rec_dpy;
                rta.rec_ctx = rec_ctx;
                rta.cur_ctx = &rec_cur_ctx;
                if (pthread_create(&rec_thread, NULL, record_thread_func, &rta) == 0) {
                    rec_active = true;
                }
            }
        }
        if (!rec_active && rec_ctx != 0) {
            XRecordFreeContext(dpy, rec_ctx);
            rec_ctx = 0;
        }
        if (!rec_active && rec_dpy) {
            XCloseDisplay(rec_dpy);
            rec_dpy = NULL;
        }
    }

    GLXContext ctx = glXCreateNewContext(dpy, fb_config, GLX_RGBA_TYPE, NULL, GL_TRUE);
    glXMakeCurrent(dpy, gl_win, ctx);

    /* Enable VSync */
    bool vsync_active = false;
    if (s_glXSwapIntervalEXT) {
        s_glXSwapIntervalEXT(dpy, gl_win, 1);
        vsync_active = true;
    } else if (s_glXSwapIntervalSGI) {
        if (s_glXSwapIntervalSGI(1) == 0) vsync_active = true;
    } else if (s_glXSwapIntervalMESA) {
        if (s_glXSwapIntervalMESA(1) == 0) vsync_active = true;
    }

    const GLubyte *gl_renderer = glGetString(GL_RENDERER);
    if (gl_renderer) {
        fprintf(stderr, "AspectScale GL Scaler: Using GPU [%s] (VSync: %s, Mode: %s, Factor: %dx)\n",
                (const char *)gl_renderer, vsync_active ? "active" : "standard",
                is_fullscreen ? "Fullscreen" : "Windowed", current_factor);
    }

    /* Redirect target window to capture offscreen buffer */
    XCompositeRedirectWindow(dpy, target, CompositeRedirectAutomatic);

    Pixmap pix = XCompositeNameWindowPixmap(dpy, target);
    int pix_attribs[] = {
        GLX_TEXTURE_TARGET_EXT, GLX_TEXTURE_2D_EXT,
        GLX_TEXTURE_FORMAT_EXT, (td == 32) ? GLX_TEXTURE_FORMAT_RGBA_EXT : GLX_TEXTURE_FORMAT_RGB_EXT,
        None
    };
    GLXPixmap glx_pix = glXCreatePixmap(dpy, fb_config, pix, pix_attribs);

    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);

    ScaleFilterMode current_mode = s_scale_mode;
    GLint filter = (current_mode == SCALE_FILTER_BILINEAR) ? GL_LINEAR : GL_NEAREST;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glEnable(GL_TEXTURE_2D);

    if (is_fullscreen) {
        calculate_viewport(current_mode, target_w, target_h, &mon, &vp_x, &vp_y, &vp_w, &vp_h, &y_from_top);
    }

    /* Window dragging state for Alt + Drag */
    bool is_dragging = false;
    int drag_start_x = 0, drag_start_y = 0;
    int win_drag_orig_x = 0, win_drag_orig_y = 0;
    int toplevel_drag_orig_x = 0, toplevel_drag_orig_y = 0;

    bool target_pixmap_dirty = false;
    PopupInput popup_input = {0};
    GLuint menu_cursor_tex = 0;
    glGenTextures(1, &menu_cursor_tex);

    while (s_running) {
        /* Process X events */
        while (XPending(dpy)) {
            XEvent ev;
            XNextEvent(dpy, &ev);

            /* Check root substructure events for popups and dialogs */
            if (ev.type == CreateNotify || ev.type == MapNotify || ev.type == ConfigureNotify ||
                ev.type == PropertyNotify || ev.type == ReparentNotify) {
                Window w = (ev.type == CreateNotify) ? ev.xcreatewindow.window :
                           (ev.type == MapNotify) ? ev.xmap.window :
                           (ev.type == PropertyNotify) ? ev.xproperty.window :
                           (ev.type == ReparentNotify) ? ev.xreparent.window : ev.xconfigure.window;
                if (w == target) {
                    if (ev.type == MapNotify) target_pixmap_dirty = true;
                    if (ev.type == ReparentNotify)
                        toplevel_frame = x11_get_toplevel_parent(dpy, root, target);
                }
                /* A WM may reparent a client before it maps, and Wine may set
                 * WM_TRANSIENT_FOR after CreateNotify. Follow client events. */
                if (ev.type == CreateNotify && w != gl_win && w != target)
                    XSelectInput(dpy, w, StructureNotifyMask | PropertyChangeMask);
                XWindowAttributes event_wa;
                if (!XGetWindowAttributes(dpy, w, &event_wa) || event_wa.class != InputOutput)
                    continue;
                bool internal = w != target && is_descendant(dpy, target, w);
                if (internal && (ev.type == CreateNotify || ev.type == ReparentNotify))
                    watch_children(dpy, w, 0);
                int override_redirect = event_wa.override_redirect;

                if (w != gl_win && w != toplevel_frame && is_target_subwindow(dpy, target, w)) {
                    if (internal) {
                        /* Owned child menus AND dialogs stay in the desktop's
                         * native stacking and Composite image. Only input needs
                         * to switch to source coordinates during their lifetime. */
                        if (event_wa.map_state != IsViewable ||
                            event_wa.width <= 1 || event_wa.height <= 1) continue;
                        int slot = -1;
                        for (int i = 0; i < MAX_TRACKED_POPUPS; i++) {
                            if (popups[i].active && popups[i].win == w) { slot = i; break; }
                            if (!popups[i].active && slot < 0) slot = i;
                        }
                        if (slot >= 0) {
                            popups[slot].win = w;
                            popups[slot].active = true;
                            popups[slot].internal = true;
                            int sx, sy;
                            XTranslateCoordinates(dpy, target, root, 0, 0, &sx, &sy, &dummy_child);
                            PopupInputTransform transform = {
                                sx, sy, win_x + vp_x, win_y + y_from_top,
                                (double)vp_w / target_w, (double)vp_h / target_h
                            };
                            if (active_dialog == None && has_xfixes)
                                popup_input_begin(dpy, gl_win, &popup_input, &transform);
                        }
                    } else if (override_redirect) {
                        /* Lower popup behind gl_win so unscaled popup does not appear on screen */
                        if (ev.type == CreateNotify || ev.type == MapNotify) {
                            XLowerWindow(dpy, w);
                            Window s[] = { gl_win, w };
                            XRestackWindows(dpy, s, 2);
                        }

                        int slot = -1;
                        for (int i = 0; i < MAX_TRACKED_POPUPS; i++) {
                            if (popups[i].active && popups[i].win == w) { slot = i; break; }
                            if (!popups[i].active && slot == -1) slot = i;
                        }
                        if (slot != -1) {
                            XWindowAttributes wa;
                            Status s_wa = XGetWindowAttributes(dpy, w, &wa);
                            if (s_wa && wa.class == InputOutput && wa.map_state == IsViewable &&
                                wa.width > 1 && wa.height > 1) {
                                bool resized = wa.width != (int)popups[slot].width ||
                                               wa.height != (int)popups[slot].height;
                                popups[slot].win = w;
                                XTranslateCoordinates(dpy, w, root, 0, 0,
                                                      &popups[slot].root_x, &popups[slot].root_y,
                                                      &dummy_child);
                                popups[slot].width = wa.width;
                                popups[slot].height = wa.height;

                                if (wa.map_state == IsViewable) {
                                    if (!popups[slot].active || popups[slot].glx_pix == 0) {
                                        XCompositeRedirectWindow(dpy, w, CompositeRedirectAutomatic);
                                        if (popups[slot].pix) XFreePixmap(dpy, popups[slot].pix);
                                        popups[slot].pix = XCompositeNameWindowPixmap(dpy, w);
                                        int p_attribs[] = {
                                            GLX_TEXTURE_TARGET_EXT, GLX_TEXTURE_2D_EXT,
                                            GLX_TEXTURE_FORMAT_EXT, (wa.depth == 32) ? GLX_TEXTURE_FORMAT_RGBA_EXT : GLX_TEXTURE_FORMAT_RGB_EXT,
                                            None
                                        };
                                        if (popups[slot].glx_pix) glXDestroyPixmap(dpy, popups[slot].glx_pix);
                                        popups[slot].glx_pix = glXCreatePixmap(dpy, fb_config, popups[slot].pix, p_attribs);
                                        if (popups[slot].tex == 0) {
                                            glGenTextures(1, &popups[slot].tex);
                                        }
                                        glBindTexture(GL_TEXTURE_2D, popups[slot].tex);
                                        GLint p_filter = (current_mode == SCALE_FILTER_BILINEAR) ? GL_LINEAR : GL_NEAREST;
                                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, p_filter);
                                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, p_filter);
                                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                                        popups[slot].active = true;
                                        int sx, sy;
                                        XTranslateCoordinates(dpy, target, root, 0, 0, &sx, &sy, &dummy_child);
                                        PopupInputTransform transform = {
                                            sx, sy, win_x + vp_x, win_y + y_from_top,
                                            (double)vp_w / target_w, (double)vp_h / target_h
                                        };
                                        if (active_dialog == None && has_xfixes)
                                            popup_input_begin(dpy, gl_win, &popup_input, &transform);
                                        fprintf(stderr, "AspectScale Debug: Popup 0x%lx captured into slot %d (%ux%u at %d,%d) glx_pix=0x%lx\n",
                                                (unsigned long)w, slot, wa.width, wa.height, wa.x, wa.y, (unsigned long)popups[slot].glx_pix);
                                    } else if (resized) {
                                        if (popups[slot].glx_pix) glXDestroyPixmap(dpy, popups[slot].glx_pix);
                                        if (popups[slot].pix) XFreePixmap(dpy, popups[slot].pix);
                                        popups[slot].pix = XCompositeNameWindowPixmap(dpy, w);
                                        int p_attribs[] = {
                                            GLX_TEXTURE_TARGET_EXT, GLX_TEXTURE_2D_EXT,
                                            GLX_TEXTURE_FORMAT_EXT, (wa.depth == 32) ? GLX_TEXTURE_FORMAT_RGBA_EXT : GLX_TEXTURE_FORMAT_RGB_EXT,
                                            None
                                        };
                                        popups[slot].glx_pix = glXCreatePixmap(dpy, fb_config, popups[slot].pix, p_attribs);
                                    }
                                }
                            }
                        }
                    } else {
                        /* Creation/configuration also describes unmapped helper windows.
                         * Subscribe on the client before the WM reparents it. */
                        XSelectInput(dpy, w, StructureNotifyMask | PropertyChangeMask);
                        XWindowAttributes dialog_wa;
                        if (!XGetWindowAttributes(dpy, w, &dialog_wa) ||
                            dialog_wa.class != InputOutput ||
                            dialog_wa.map_state != IsViewable ||
                            dialog_wa.width <= 1 || dialog_wa.height <= 1)
                            continue;
                        popup_input_end(dpy, gl_win, &popup_input);
                        /* Managed dialog / new window */
                        Window frame = x11_get_toplevel_parent(dpy, root, w);
                        int slot = 0;
                        while (slot < dialog_count && dialogs[slot].win != w) slot++;
                        bool newly_mapped = slot == dialog_count;
                        if (newly_mapped) {
                            if (dialog_count == MAX_TRACKED_POPUPS) continue;
                            dialogs[dialog_count++].win = w;
                        }
                        dialogs[slot].frame = frame;
                        active_dialog = dialogs[dialog_count - 1].win;
                        active_dialog_frame = dialogs[dialog_count - 1].frame;
                        /* Geometry events from a parent must not activate it
                         * over its message box or steal focus after Alt+Tab. */
                        if (newly_mapped) {
                            XRaiseWindow(dpy, frame);
                            focus_client(dpy, root, w, CurrentTime);
                        }
                    }
                }
                continue;
            } else if (ev.type == UnmapNotify || ev.type == DestroyNotify) {
                Window w = (ev.type == UnmapNotify) ? ev.xunmap.window : ev.xdestroywindow.window;
                for (unsigned int button = 0; button < 256; button++)
                    if (pressed_receiver[button] == w) pressed_receiver[button] = None;
                for (int i = 0; i < MAX_TRACKED_POPUPS; i++) {
                    if (popups[i].active && popups[i].win == w) {
                        cleanup_popup(dpy, &popups[i], (ev.type == UnmapNotify));
                    }
                }
                for (int i = 0; i < dialog_count; i++) {
                    if (dialogs[i].win != w) continue;
                    memmove(&dialogs[i], &dialogs[i + 1],
                            (dialog_count - i - 1) * sizeof(dialogs[0]));
                    dialog_count--;
                    break;
                }
                active_dialog = dialog_count ? dialogs[dialog_count - 1].win : None;
                active_dialog_frame = dialog_count ? dialogs[dialog_count - 1].frame : None;
                if (w == target) {
                    if (ev.type == DestroyNotify) {
                        s_running = false;
                        break;
                    }
                    /* Wine/WM dialog transitions can briefly unmap the owner.
                     * Its next map creates a new Composite backing pixmap. */
                    target_pixmap_dirty = true;
                }
                continue;
            }

            if (popup_input.active && (ev.type == ButtonPress ||
                ev.type == ButtonRelease || ev.type == MotionNotify)) {
                /* A very short click may have queued its overlay release
                 * before the input shape changed. Native releases never arrive
                 * here, but this queued release still needs forwarding once. */
                if (ev.type == ButtonRelease && ev.xbutton.window == gl_win &&
                    ev.xbutton.button < 256 && pressed_receiver[ev.xbutton.button] != None) {
                    int gx = (int)lround((ev.xbutton.x - vp_x) * (double)target_w / vp_w);
                    int gy = (int)lround((ev.xbutton.y - y_from_top) * (double)target_h / vp_h);
                    if (gx < 0) gx = 0;
                    if (gy < 0) gy = 0;
                    if (gx >= (int)target_w) gx = target_w - 1;
                    if (gy >= (int)target_h) gy = target_h - 1;
                    forward_pointer(dpy, root, target, pressed_receiver[ev.xbutton.button],
                                    gx, gy, ButtonReleaseMask, &ev);
                    pressed_receiver[ev.xbutton.button] = None;
                }
                continue;
            }

            if (ev.type == KeyPress) {
                KeySym sym = XLookupKeysym(&ev.xkey, 0);
                if (sym == XK_Escape && !popup_input.active) {
                    fprintf(stderr, "AspectScale Debug: Exiting due to Escape key\n");
                    s_running = false;
                    break;
                }
                /* Restore hotkey: Ctrl + Alt + R */
                if ((ev.xkey.state & (ControlMask | Mod1Mask)) == (ControlMask | Mod1Mask) &&
                    (sym == XK_r || sym == XK_R)) {
                    fprintf(stderr, "AspectScale Debug: Exiting due to Ctrl+Alt+R\n");
                    s_running = false;
                    break;
                }
                /* Fullscreen toggle hotkey: Ctrl + Alt + F */
                if ((ev.xkey.state & (ControlMask | Mod1Mask)) == (ControlMask | Mod1Mask) &&
                    (sym == XK_f || sym == XK_F)) {
                    fprintf(stderr, "AspectScale Debug: Fullscreen hotkey Ctrl+Alt+F pressed\n");
                    s_running = false;
                    break;
                }
                /* Windowed cycle hotkey: Ctrl + Alt + S */
                if ((ev.xkey.state & (ControlMask | Mod1Mask)) == (ControlMask | Mod1Mask) &&
                    (sym == XK_s || sym == XK_S)) {
                    if (is_fullscreen) {
                        /* Switch from fullscreen to windowed mode */
                        s_running = false;
                        break;
                    } else {
                        /* Cycle to next factor */
                        int next_factor = current_factor + 1;
                        if (next_factor <= max_factor) {
                            s_requested_factor = next_factor;
                            s_factor_change_pending = true;
                        } else {
                            /* Reached maximum fit that stops before screen res -> cycle back to 1x (exit) */
                            fprintf(stderr, "AspectScale Debug: Windowed cycle reached max fit (%d). Exiting to 1x.\n", max_factor);
                            s_running = false;
                            break;
                        }
                        continue;
                    }
                }
                /* Target events already went to the application. Only keys
                 * delivered to our overlay need forwarding. */
                if (ev.xkey.window != gl_win) continue;
                /* Forward key event to target window */
                ev.xkey.window = target;
                ev.xkey.root = root;
                ev.xkey.subwindow = None;
                XSendEvent(dpy, target, True, KeyPressMask, &ev);
                XSync(dpy, False);
            } else if (ev.type == KeyRelease) {
                if (ev.xkey.window != gl_win) continue;
                ev.xkey.window = target;
                ev.xkey.root = root;
                ev.xkey.subwindow = None;
                XSendEvent(dpy, target, True, KeyReleaseMask, &ev);
                XSync(dpy, False);
            } else if (ev.type == ButtonPress) {
                if (!is_fullscreen && (ev.xbutton.state & Mod1Mask) && ev.xbutton.button == Button1) {
                    /* Alt + Left Click initiates window drag */
                    is_dragging = true;
                    drag_start_x = ev.xbutton.x_root;
                    drag_start_y = ev.xbutton.y_root;
                    win_drag_orig_x = win_x;
                    win_drag_orig_y = win_y;
                    int cur_target_x = 0, cur_target_y = 0;
                    XTranslateCoordinates(dpy, toplevel_frame, root, 0, 0, &cur_target_x, &cur_target_y, &dummy_child);
                    toplevel_drag_orig_x = cur_target_x;
                    toplevel_drag_orig_y = cur_target_y;
                    continue;
                }

                int cx = ev.xbutton.x - vp_x;
                int cy = ev.xbutton.y - y_from_top;
                int gx = (int)lround((double)cx * (double)target_w / (double)vp_w);
                int gy = (int)lround((double)cy * (double)target_h / (double)vp_h);
                if (gx < 0) gx = 0;
                if (gx >= (int)target_w) gx = (int)target_w - 1;
                if (gy < 0) gy = 0;
                if (gy >= (int)target_h) gy = (int)target_h - 1;

                XUngrabPointer(dpy, ev.xbutton.time);
                /* Ensure Wine has input focus so click is directly processed */
                Window receiver = pointer_receiver(dpy, target, gx, gy, ButtonPressMask);
                if (ev.xbutton.button < 256) pressed_receiver[ev.xbutton.button] = receiver;
                /* Wine can query/confine the real cursor during activation and
                 * menu tracking, before the popup's MapNotify reaches us. Put
                 * it at the intended source point BEFORE delivering the press.
                 * Keep native input through its release (and any open menus). */
                if (receiver != target && has_xfixes) {
                    int sx, sy;
                    XTranslateCoordinates(dpy, target, root, 0, 0, &sx, &sy, &dummy_child);
                    PopupInputTransform transform = {
                        sx, sy, win_x + vp_x, win_y + y_from_top,
                        (double)vp_w / target_w, (double)vp_h / target_h
                    };
                    if (popup_input_begin_at(dpy, gl_win, &popup_input, &transform, sx + gx, sy + gy)) {
                        XSync(dpy, False);
                    }
                }
                focus_client(dpy, root, receiver, CurrentTime);

                /* Send MotionNotify immediately preceding click so Wine knows cursor position */
                XEvent mev;
                memset(&mev, 0, sizeof(mev));
                mev.type = MotionNotify;
                mev.xmotion.time = ev.xbutton.time;
                mev.xmotion.state = ev.xbutton.state;
                forward_pointer(dpy, root, target, receiver, gx, gy, PointerMotionMask, &mev);
                forward_pointer(dpy, root, target, receiver, gx, gy, ButtonPressMask, &ev);

                if (active_dialog_frame == None) {
                    position_overlay(dpy, root, toplevel_frame, gl_win, popups);
                }
                XSync(dpy, False);
            } else if (ev.type == ButtonRelease) {
                if (is_dragging && ev.xbutton.button == Button1) {
                    is_dragging = false;
                    continue;
                }

                int cx = ev.xbutton.x - vp_x;
                int cy = ev.xbutton.y - y_from_top;
                int gx = (int)lround((double)cx * (double)target_w / (double)vp_w);
                int gy = (int)lround((double)cy * (double)target_h / (double)vp_h);
                if (gx < 0) gx = 0;
                if (gx >= (int)target_w) gx = (int)target_w - 1;
                if (gy < 0) gy = 0;
                if (gy >= (int)target_h) gy = (int)target_h - 1;

                Window receiver = ev.xbutton.button < 256 ? pressed_receiver[ev.xbutton.button] : None;
                if (receiver == None) receiver = pointer_receiver(dpy, target, gx, gy, ButtonReleaseMask);
                forward_pointer(dpy, root, target, receiver, gx, gy, ButtonReleaseMask, &ev);
                if (ev.xbutton.button < 256) pressed_receiver[ev.xbutton.button] = None;
                XSync(dpy, False);
            } else if (ev.type == MotionNotify) {
                if (is_dragging) {
                    int dx = ev.xmotion.x_root - drag_start_x;
                    int dy = ev.xmotion.y_root - drag_start_y;
                    win_x = win_drag_orig_x + dx;
                    win_y = win_drag_orig_y + dy;
                    XMoveWindow(dpy, gl_win, win_x, win_y);
                    XMoveWindow(dpy, toplevel_frame, toplevel_drag_orig_x + dx, toplevel_drag_orig_y + dy);
                    XSync(dpy, False);
                    continue;
                }

                int cx = ev.xmotion.x - vp_x;
                int cy = ev.xmotion.y - y_from_top;
                int gx = (int)lround((double)cx * (double)target_w / (double)vp_w);
                int gy = (int)lround((double)cy * (double)target_h / (double)vp_h);
                if (gx < 0) gx = 0;
                if (gx >= (int)target_w) gx = (int)target_w - 1;
                if (gy < 0) gy = 0;
                if (gy >= (int)target_h) gy = (int)target_h - 1;

                Window receiver = None;
                for (unsigned int button = 1; button <= 5; button++)
                    if ((ev.xmotion.state & (Button1Mask << (button - 1))) && pressed_receiver[button]) {
                        receiver = pressed_receiver[button];
                        break;
                    }
                if (receiver == None) receiver = pointer_receiver(dpy, target, gx, gy, PointerMotionMask);
                forward_pointer(dpy, root, target, receiver, gx, gy, PointerMotionMask, &ev);
                XSync(dpy, False);
            }
        }

        bool have_popup = false;
        for (int i = 0; i < MAX_TRACKED_POPUPS; i++)
            have_popup |= popups[i].active;
        if (!have_popup || active_dialog != None)
            popup_input_end(dpy, gl_win, &popup_input);

        if (rec_cur_ctx.pending_valid) {
            rec_cur_ctx.pending_valid = false;
            Cursor new_cur = rec_cur_ctx.pending_cursor;
            if (!s_hide_cursor || !is_fullscreen) {
                if (new_cur == None) {
                    if (custom_cursor != None) {
                        XDefineCursor(dpy, gl_win, custom_cursor);
                    } else {
                        XUndefineCursor(dpy, gl_win);
                    }
                } else {
                    XDefineCursor(dpy, gl_win, new_cur);
                }
                XFlush(dpy);
            }
        }

        if (!s_running) break;

        /* Handle dynamic factor change for windowed mode */
        if (!is_fullscreen && s_factor_change_pending) {
            s_factor_change_pending = false;
            int req = s_requested_factor;
            if (req >= 2 && req <= max_factor && req != current_factor) {
                current_factor = req;
                s_scale_factor = req;

                int new_w = current_factor * (int)target_w;
                int new_h = current_factor * (int)target_h;

                win_w = new_w;
                win_h = new_h;
                win_x = mon.x + (mon.width - win_w) / 2;
                win_y = mon.y + (mon.height - win_h) / 2;

                XMoveResizeWindow(dpy, gl_win, win_x, win_y, (unsigned int)win_w, (unsigned int)win_h);

                int target_under_x = win_x + (win_w - (int)target_w) / 2;
                int target_under_y = win_y + (win_h - (int)target_h) / 2;
                XMoveWindow(dpy, toplevel_frame, target_under_x, target_under_y);

                vp_w = win_w;
                vp_h = win_h;
                vp_x = 0;
                vp_y = 0;
                y_from_top = 0;
                XSync(dpy, False);
            }
        }

        /* Check if scale/filter mode changed dynamically */
        if (s_scale_mode != current_mode) {
            current_mode = s_scale_mode;
            GLint new_filter = (current_mode == SCALE_FILTER_BILINEAR) ? GL_LINEAR : GL_NEAREST;
            glBindTexture(GL_TEXTURE_2D, tex);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, new_filter);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, new_filter);
            for (int i = 0; i < MAX_TRACKED_POPUPS; i++) {
                if (popups[i].active && popups[i].tex != 0) {
                    glBindTexture(GL_TEXTURE_2D, popups[i].tex);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, new_filter);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, new_filter);
                }
            }
            if (is_fullscreen) {
                calculate_viewport(current_mode, target_w, target_h, &mon, &vp_x, &vp_y, &vp_w, &vp_h, &y_from_top);
            }
        }

        /* Verify target window still exists */
        unsigned int cur_w = 0, cur_h = 0;
        if (!XGetGeometry(dpy, target, &r_ret, &tx, &ty, &cur_w, &cur_h, &tb, &td)) {
            fprintf(stderr, "AspectScale Debug: Target 0x%lx closed or unmapped.\n", (unsigned long)target);
            s_running = false;
            break;
        }

        if (cur_w != target_w || cur_h != target_h) {
            target_pixmap_dirty = true;
            target_w = cur_w;
            target_h = cur_h;
            if (is_fullscreen) {
                calculate_viewport(current_mode, target_w, target_h, &mon, &vp_x, &vp_y, &vp_w, &vp_h, &y_from_top);
                int target_under_x = win_x + vp_x + (vp_w - (int)target_w) / 2;
                int target_under_y = win_y + y_from_top + (vp_h - (int)target_h) / 2;
                XMoveWindow(dpy, toplevel_frame, target_under_x, target_under_y);
            } else {
                win_w = current_factor * (int)target_w;
                win_h = current_factor * (int)target_h;
                XResizeWindow(dpy, gl_win, (unsigned int)win_w, (unsigned int)win_h);
                vp_w = win_w;
                vp_h = win_h;
                int target_under_x = win_x + (win_w - (int)target_w) / 2;
                int target_under_y = win_y + (win_h - (int)target_h) / 2;
                XMoveWindow(dpy, toplevel_frame, target_under_x, target_under_y);
            }
        }

        XWindowAttributes target_wa;
        if (!XGetWindowAttributes(dpy, target, &target_wa)) {
            s_running = false;
            break;
        }
        if (target_pixmap_dirty && target_wa.map_state == IsViewable) {
            glXDestroyPixmap(dpy, glx_pix);
            XFreePixmap(dpy, pix);
            pix = XCompositeNameWindowPixmap(dpy, target);
            glx_pix = glXCreatePixmap(dpy, fb_config, pix, pix_attribs);
            target_pixmap_dirty = false;
        }

        /* Only move our overlay and captured override-redirect menus.
         * The WM owns the relative order of the game and its native dialogs. */
        position_overlay(dpy, root, toplevel_frame, gl_win, popups);
        Window stack[MAX_TRACKED_POPUPS + 1];
        int stack_count = 0;
        stack[stack_count++] = gl_win;
        for (int i = MAX_TRACKED_POPUPS - 1; i >= 0; i--)
            if (popups[i].active && !popups[i].internal) stack[stack_count++] = popups[i].win;
        if (stack_count > 1) XRestackWindows(dpy, stack, stack_count);

        /* Render 2D canvas with orthographic projection */
        glViewport(0, 0, win_w, win_h);
        glMatrixMode(GL_PROJECTION);
        glLoadIdentity();
        glOrtho(0.0, (GLdouble)win_w, (GLdouble)win_h, 0.0, -1.0, 1.0);
        glMatrixMode(GL_MODELVIEW);
        glLoadIdentity();

        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        if (!target_pixmap_dirty) {
            /* Render main target quad */
            glBindTexture(GL_TEXTURE_2D, tex);
            s_glXBindTexImage(dpy, glx_pix, GLX_FRONT_LEFT_EXT, NULL);
            glBegin(GL_QUADS);
            glTexCoord2f(0.0f, 0.0f); glVertex2f((GLfloat)vp_x, (GLfloat)y_from_top);
            glTexCoord2f(1.0f, 0.0f); glVertex2f((GLfloat)(vp_x + vp_w), (GLfloat)y_from_top);
            glTexCoord2f(1.0f, 1.0f); glVertex2f((GLfloat)(vp_x + vp_w), (GLfloat)(y_from_top + vp_h));
            glTexCoord2f(0.0f, 1.0f); glVertex2f((GLfloat)vp_x, (GLfloat)(y_from_top + vp_h));
            glEnd();
            s_glXReleaseTexImage(dpy, glx_pix, GLX_FRONT_LEFT_EXT);
        }

        /* Render active popups */
        int cur_tgt_x = 0, cur_tgt_y = 0;
        Window dummy_c;
        XTranslateCoordinates(dpy, target, root, 0, 0, &cur_tgt_x, &cur_tgt_y, &dummy_c);

        for (int i = 0; i < MAX_TRACKED_POPUPS; i++) {
            if (popups[i].active && popups[i].glx_pix != 0) {
                int rel_x = popups[i].root_x - cur_tgt_x;
                int rel_y = popups[i].root_y - cur_tgt_y;
                float sx = (float)vp_x + (float)rel_x * ((float)vp_w / (float)target_w);
                float sy = (float)y_from_top + (float)rel_y * ((float)vp_h / (float)target_h);
                float sw = (float)popups[i].width * ((float)vp_w / (float)target_w);
                float sh = (float)popups[i].height * ((float)vp_h / (float)target_h);

                glBindTexture(GL_TEXTURE_2D, popups[i].tex);
                s_glXBindTexImage(dpy, popups[i].glx_pix, GLX_FRONT_LEFT_EXT, NULL);
                glBegin(GL_QUADS);
                glTexCoord2f(0.0f, 0.0f); glVertex2f(sx, sy);
                glTexCoord2f(1.0f, 0.0f); glVertex2f(sx + sw, sy);
                glTexCoord2f(1.0f, 1.0f); glVertex2f(sx + sw, sy + sh);
                glTexCoord2f(0.0f, 1.0f); glVertex2f(sx, sy + sh);
                glEnd();
                s_glXReleaseTexImage(dpy, popups[i].glx_pix, GLX_FRONT_LEFT_EXT);
            }
        }

        if (popup_input.active) {
            popup_input.transform = (PopupInputTransform){
                cur_tgt_x, cur_tgt_y, win_x + vp_x, win_y + y_from_top,
                (double)vp_w / target_w, (double)vp_h / target_h
            };
            XFixesCursorImage *cursor = XFixesGetCursorImage(dpy);
            if (cursor) {
                size_t count = (size_t)cursor->width * cursor->height;
                uint32_t *pixels = malloc(count * sizeof(*pixels));
                if (pixels && count) {
                    for (size_t i = 0; i < count; i++) pixels[i] = cursor->pixels[i];
                    int cx, cy;
                    popup_input_to_view(&popup_input.transform, cursor->x, cursor->y, &cx, &cy);
                    float sx = popup_input.transform.scale_x;
                    float sy = popup_input.transform.scale_y;
                    float x = cx - win_x - cursor->xhot * sx;
                    float y = cy - win_y - cursor->yhot * sy;
                    float w = cursor->width * sx, h = cursor->height * sy;
                    glBindTexture(GL_TEXTURE_2D, menu_cursor_tex);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, cursor->width, cursor->height,
                                 0, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, pixels);
                    glEnable(GL_BLEND);
                    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
                    glBegin(GL_QUADS);
                    glTexCoord2f(0, 0); glVertex2f(x, y);
                    glTexCoord2f(1, 0); glVertex2f(x + w, y);
                    glTexCoord2f(1, 1); glVertex2f(x + w, y + h);
                    glTexCoord2f(0, 1); glVertex2f(x, y + h);
                    glEnd();
                    glDisable(GL_BLEND);
                }
                free(pixels);
                XFree(cursor);
            }
        }

        glXSwapBuffers(dpy, gl_win);

        if (!vsync_active) {
            usleep(2000);
        }
    }

    popup_input_cleanup(dpy, gl_win, &popup_input);
    glDeleteTextures(1, &menu_cursor_tex);

    /* Teardown XRecord */
    if (rec_active) {
        XRecordDisableContext(dpy, rec_ctx);
        XSync(dpy, False);
        pthread_join(rec_thread, NULL);
        XRecordFreeContext(dpy, rec_ctx);
        XCloseDisplay(rec_dpy);
    }

    /* Restore target window position and decorations */
    x11_set_motif_decorations(dpy, target, true);
    XMoveWindow(dpy, toplevel_frame, target_orig_x, target_orig_y);
    XRaiseWindow(dpy, target);

    /* Cleanup tracked popups */
    for (int i = 0; i < MAX_TRACKED_POPUPS; i++) {
        if (popups[i].active) {
            cleanup_popup(dpy, &popups[i], true);
        }
    }

    /* Cleanup */
    if (blank_cursor != None) {
        XUndefineCursor(dpy, gl_win);
        XFreeCursor(dpy, blank_cursor);
    }
    if (custom_cursor != None) {
        XUndefineCursor(dpy, gl_win);
        XFreeCursor(dpy, custom_cursor);
    }
    if (blank_pix != None) {
        XFreePixmap(dpy, blank_pix);
    }

    glDeleteTextures(1, &tex);
    glXDestroyPixmap(dpy, glx_pix);
    XFreePixmap(dpy, pix);
    XCompositeUnredirectWindow(dpy, target, CompositeRedirectAutomatic);

    glXMakeCurrent(dpy, None, NULL);
    glXDestroyContext(dpy, ctx);
    XDestroyWindow(dpy, gl_win);
    XFree(configs);
    XSync(dpy, False);

    /* Refocus target */
    XWindowAttributes wa_final;
    if (XGetWindowAttributes(dpy, target, &wa_final)) {
        XRaiseWindow(dpy, target);
        focus_client(dpy, root, target, CurrentTime);
    }
    XSync(dpy, False);

    XCloseDisplay(dpy);
    s_running = false;
    return NULL;
}

static bool s_thread_created = false;

bool gl_scaler_start(Display *dpy, Window target_win, ScalerTargetMode mode, int factor) {
    if (target_win == None) return false;

    if (s_running) {
        if (s_target_mode == mode) {
            if (mode == SCALER_MODE_WINDOWED && factor > 0 && factor != s_scale_factor) {
                return gl_scaler_set_factor(factor);
            }
            return false;
        }
        /* Mode change: stop existing scaler first */
        gl_scaler_stop();
    }

    if (s_thread_created) {
        pthread_join(s_thread, NULL);
        s_thread_created = false;
    }

    if (!s_glXBindTexImage || !s_glXReleaseTexImage) {
        if (!gl_scaler_init(dpy)) return false;
    }

    s_target_win = target_win;
    s_target_mode = mode;
    s_scale_factor = (factor >= 2) ? factor : 2;
    s_requested_factor = s_scale_factor;
    s_factor_change_pending = false;
    s_running = true;

    if (pthread_create(&s_thread, NULL, gl_render_thread, NULL) != 0) {
        s_running = false;
        return false;
    }
    s_thread_created = true;
    return true;
}

bool gl_scaler_start_fullscreen(Display *dpy, Window target_win) {
    return gl_scaler_start(dpy, target_win, SCALER_MODE_FULLSCREEN, 0);
}

bool gl_scaler_start_windowed(Display *dpy, Window target_win, int factor) {
    return gl_scaler_start(dpy, target_win, SCALER_MODE_WINDOWED, factor);
}

void gl_scaler_stop(void) {
    if (!s_running && !s_thread_created) return;
    s_running = false;
    if (s_thread_created) {
        pthread_join(s_thread, NULL);
        s_thread_created = false;
    }
}
