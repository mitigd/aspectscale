#include "gl_scaler.h"
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
                if (req->window == ctx->target || (ctx->target_parent != None && req->window == ctx->target_parent)) {
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

static bool s_running = false;
static pthread_t s_thread;
static Window s_target_win = None;
static bool s_hide_cursor = true;

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

void gl_scaler_set_hide_cursor(bool hide) {
    s_hide_cursor = hide;
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
    int tx, ty;
    unsigned int target_w = 0, target_h = 0, tb, td;
    if (!XGetGeometry(dpy, target, &r_ret, &tx, &ty, &target_w, &target_h, &tb, &td) ||
        target_w == 0 || target_h == 0) {
        s_running = false;
        XCloseDisplay(dpy);
        return NULL;
    }

    MonitorBounds mon = get_monitor_bounds(dpy, target);

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
            XCloseDisplay(dpy);
            return NULL;
        }
    }

    GLXFBConfig fb_config = configs[0];
    XVisualInfo *vi = glXGetVisualFromFBConfig(dpy, fb_config);

    XSetWindowAttributes swa;
    swa.colormap = XCreateColormap(dpy, root, vi->visual, AllocNone);
    swa.background_pixel = BlackPixel(dpy, screen);
    swa.override_redirect = True;
    swa.event_mask = StructureNotifyMask | KeyPressMask | KeyReleaseMask |
                     ButtonPressMask | ButtonReleaseMask | PointerMotionMask;

    Window gl_win = XCreateWindow(dpy, root, mon.x, mon.y, mon.width, mon.height, 0,
                                  vi->depth, InputOutput, vi->visual,
                                  CWColormap | CWBackPixel | CWOverrideRedirect | CWEventMask, &swa);

    int dummy_ev = 0, dummy_err = 0;
    bool has_xfixes = XFixesQueryExtension(dpy, &dummy_ev, &dummy_err);

    Cursor custom_cursor = None;
    if (!s_hide_cursor && has_xfixes) {
        XFixesCursorImage *img = XFixesGetCursorImage(dpy);
        if (img) {
            custom_cursor = create_cursor_from_xfixes(dpy, img);
            XFree(img);
        }
    }

    /* Hide cursor or apply game's custom cursor */
    Cursor blank_cursor = None;
    Pixmap blank_pix = None;
    if (s_hide_cursor) {
        blank_pix = XCreateBitmapFromData(dpy, gl_win, "\0", 1, 1);
        XColor dummy;
        memset(&dummy, 0, sizeof(dummy));
        blank_cursor = XCreatePixmapCursor(dpy, blank_pix, blank_pix, &dummy, &dummy, 0, 0);
        XDefineCursor(dpy, gl_win, blank_cursor);
    } else if (custom_cursor != None) {
        XDefineCursor(dpy, gl_win, custom_cursor);
    }

    XMapRaised(dpy, gl_win);
    XSetInputFocus(dpy, gl_win, RevertToPointerRoot, CurrentTime);

    /* Grab keyboard so gl_win intercepts Escape, Alt+Tab, and Ctrl+Alt+S reliably without getting trapped */
    bool kbd_grabbed = false;
    for (int retry = 0; retry < 10; retry++) {
        if (XGrabKeyboard(dpy, gl_win, False, GrabModeAsync, GrabModeAsync, CurrentTime) == GrabSuccess) {
            kbd_grabbed = true;
            break;
        }
        usleep(10000);
    }

    /* Send ICCCM WM_TAKE_FOCUS so Wine / games activate internal keyboard input */
    Atom wm_protocols = XInternAtom(dpy, "WM_PROTOCOLS", False);
    Atom wm_take_focus = XInternAtom(dpy, "WM_TAKE_FOCUS", False);
    XEvent tf_ev;
    memset(&tf_ev, 0, sizeof(tf_ev));
    tf_ev.type = ClientMessage;
    tf_ev.xclient.window = target;
    tf_ev.xclient.message_type = wm_protocols;
    tf_ev.xclient.format = 32;
    tf_ev.xclient.data.l[0] = (long)wm_take_focus;
    tf_ev.xclient.data.l[1] = (long)CurrentTime;
    XSendEvent(dpy, target, False, 0, &tf_ev);
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

    /* Enable VSync to match monitor refresh rate (e.g. 144Hz) */
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
        fprintf(stderr, "AspectScale GL Scaler: Using GPU [%s] (VSync: %s)\n",
                (const char *)gl_renderer, vsync_active ? "144Hz hardware locked" : "standard");
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
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glEnable(GL_TEXTURE_2D);

    /* Aspect ratio calculation */
    double target_ar = (double)target_w / (double)target_h;
    double mon_ar = (double)mon.width / (double)mon.height;

    int vp_w, vp_h, vp_x, vp_y;
    if (mon_ar > target_ar) {
        vp_h = mon.height;
        vp_w = (int)lround((double)mon.height * target_ar);
        if (vp_w > mon.width) vp_w = mon.width;
        vp_x = (mon.width - vp_w) / 2;
        vp_y = 0;
    } else {
        vp_w = mon.width;
        vp_h = (int)lround((double)mon.width / target_ar);
        if (vp_h > mon.height) vp_h = mon.height;
        vp_x = 0;
        vp_y = (mon.height - vp_h) / 2;
    }

    int y_from_top = mon.height - (vp_y + vp_h);

    int target_abs_x = 0, target_abs_y = 0;
    Window dummy_child;
    XTranslateCoordinates(dpy, target, root, 0, 0, &target_abs_x, &target_abs_y, &dummy_child);

    while (s_running) {
        /* Process X events */
        while (XPending(dpy)) {
            XEvent ev;
            XNextEvent(dpy, &ev);

            if (ev.type == KeyPress) {
                KeySym sym = XLookupKeysym(&ev.xkey, 0);
                fprintf(stderr, "AspectScale Debug: KeyPress sym=0x%lx\n", (unsigned long)sym);
                if (sym == XK_Escape) {
                    fprintf(stderr, "AspectScale Debug: Exiting due to Escape key\n");
                    s_running = false;
                    break;
                }
                /* Check Alt+Tab - instantly restore to allow normal multitasking without locking */
                if ((ev.xkey.state & Mod1Mask) && sym == XK_Tab) {
                    fprintf(stderr, "AspectScale Debug: Exiting due to Alt+Tab\n");
                    s_running = false;
                    break;
                }
                /* Check Ctrl+Alt+S toggle */
                if ((ev.xkey.state & (ControlMask | Mod1Mask)) == (ControlMask | Mod1Mask) &&
                    (sym == XK_s || sym == XK_S)) {
                    fprintf(stderr, "AspectScale Debug: Exiting due to Ctrl+Alt+S\n");
                    s_running = false;
                    break;
                }
                /* Forward key event to target window */
                ev.xkey.window = target;
                ev.xkey.root = root;
                ev.xkey.subwindow = None;
                XSendEvent(dpy, target, True, KeyPressMask, &ev);
                XSync(dpy, False);
            } else if (ev.type == KeyRelease) {
                ev.xkey.window = target;
                ev.xkey.root = root;
                ev.xkey.subwindow = None;
                XSendEvent(dpy, target, True, KeyReleaseMask, &ev);
                XSync(dpy, False);
            } else if (ev.type == ButtonPress || ev.type == ButtonRelease) {
                int cx = ev.xbutton.x - vp_x;
                int cy = ev.xbutton.y - y_from_top;
                int gx = (int)lround((double)cx * (double)target_w / (double)vp_w);
                int gy = (int)lround((double)cy * (double)target_h / (double)vp_h);
                if (gx < 0) gx = 0;
                if (gx >= (int)target_w) gx = (int)target_w - 1;
                if (gy < 0) gy = 0;
                if (gy >= (int)target_h) gy = (int)target_h - 1;

                ev.xbutton.window = target;
                ev.xbutton.subwindow = None;
                ev.xbutton.root = root;
                ev.xbutton.x = gx;
                ev.xbutton.y = gy;
                ev.xbutton.x_root = target_abs_x + gx;
                ev.xbutton.y_root = target_abs_y + gy;
                fprintf(stderr, "AspectScale Debug: Forwarding click (%s) to target 0x%lx at local(%d, %d), root(%d, %d)\n",
                        (ev.type == ButtonPress) ? "Press" : "Release",
                        (unsigned long)target, gx, gy, ev.xbutton.x_root, ev.xbutton.y_root);
                XSendEvent(dpy, target, True, (ev.type == ButtonPress) ? ButtonPressMask : ButtonReleaseMask, &ev);
                XSync(dpy, False);
            } else if (ev.type == MotionNotify) {
                int cx = ev.xmotion.x - vp_x;
                int cy = ev.xmotion.y - y_from_top;
                int gx = (int)lround((double)cx * (double)target_w / (double)vp_w);
                int gy = (int)lround((double)cy * (double)target_h / (double)vp_h);
                if (gx < 0) gx = 0;
                if (gx >= (int)target_w) gx = (int)target_w - 1;
                if (gy < 0) gy = 0;
                if (gy >= (int)target_h) gy = (int)target_h - 1;

                ev.xmotion.window = target;
                ev.xmotion.subwindow = None;
                ev.xmotion.root = root;
                ev.xmotion.x = gx;
                ev.xmotion.y = gy;
                ev.xmotion.x_root = target_abs_x + gx;
                ev.xmotion.y_root = target_abs_y + gy;
                XSendEvent(dpy, target, True, PointerMotionMask, &ev);
                XSync(dpy, False);
            }
        }

        if (rec_cur_ctx.pending_valid) {
            rec_cur_ctx.pending_valid = false;
            Cursor new_cur = rec_cur_ctx.pending_cursor;
            if (!s_hide_cursor) {
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

        /* Verify target window still exists */
        unsigned int cur_w = 0, cur_h = 0;
        if (!XGetGeometry(dpy, target, &r_ret, &tx, &ty, &cur_w, &cur_h, &tb, &td)) {
            /* Target window closed */
            fprintf(stderr, "AspectScale Debug: XGetGeometry failed for target 0x%lx! Window may be closed or unmapped.\n", (unsigned long)target);
            s_running = false;
            break;
        }

        /* Full screen clear to pure black */
        glViewport(0, 0, mon.width, mon.height);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        /* Render aspect-corrected viewport */
        glViewport(vp_x, vp_y, vp_w, vp_h);

        glBindTexture(GL_TEXTURE_2D, tex);
        s_glXBindTexImage(dpy, glx_pix, GLX_FRONT_LEFT_EXT, NULL);

        glBegin(GL_QUADS);
        glTexCoord2f(0.0f, 1.0f); glVertex2f(-1.0f, -1.0f);
        glTexCoord2f(1.0f, 1.0f); glVertex2f(1.0f, -1.0f);
        glTexCoord2f(1.0f, 0.0f); glVertex2f(1.0f, 1.0f);
        glTexCoord2f(0.0f, 0.0f); glVertex2f(-1.0f, 1.0f);
        glEnd();

        s_glXReleaseTexImage(dpy, glx_pix, GLX_FRONT_LEFT_EXT);

        glXSwapBuffers(dpy, gl_win);

        if (!vsync_active) {
            usleep(2000); /* Fallback only if VSync unsupported */
        }
    }

    /* Teardown XRecord */
    if (rec_active) {
        XRecordDisableContext(dpy, rec_ctx);
        XSync(dpy, False);
        pthread_join(rec_thread, NULL);
        XRecordFreeContext(dpy, rec_ctx);
        XCloseDisplay(rec_dpy);
    }

    /* Ungrab keyboard */
    if (kbd_grabbed) {
        XUngrabKeyboard(dpy, CurrentTime);
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

    /* Refocus and raise target window */
    XRaiseWindow(dpy, target);
    XSetInputFocus(dpy, target, RevertToPointerRoot, CurrentTime);
    XSync(dpy, False);

    XCloseDisplay(dpy);
    s_running = false;
    return NULL;
}

static bool s_thread_created = false;

bool gl_scaler_start(Display *dpy, Window target_win) {
    if (s_running) return false;
    if (target_win == None) return false;

    if (s_thread_created) {
        pthread_join(s_thread, NULL);
        s_thread_created = false;
    }

    if (!s_glXBindTexImage || !s_glXReleaseTexImage) {
        if (!gl_scaler_init(dpy)) return false;
    }

    s_target_win = target_win;
    s_running = true;

    if (pthread_create(&s_thread, NULL, gl_render_thread, NULL) != 0) {
        s_running = false;
        return false;
    }
    s_thread_created = true;
    return true;
}

void gl_scaler_stop(void) {
    if (!s_running && !s_thread_created) return;
    s_running = false;
    if (s_thread_created) {
        pthread_join(s_thread, NULL);
        s_thread_created = false;
    }
}

bool gl_scaler_toggle(Display *dpy, Window target_win) {
    if (s_running) {
        gl_scaler_stop();
        return true;
    }
    return gl_scaler_start(dpy, target_win);
}
