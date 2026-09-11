#include "gl_scaler.h"
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/keysym.h>
#include <X11/extensions/Xcomposite.h>
#include <X11/extensions/Xrandr.h>
#include <GL/gl.h>
#include <GL/glx.h>
#include <GL/glxext.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <math.h>

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
    swa.event_mask = StructureNotifyMask | KeyPressMask | KeyReleaseMask |
                     ButtonPressMask | ButtonReleaseMask | PointerMotionMask;

    Window gl_win = XCreateWindow(dpy, root, mon.x, mon.y, mon.width, mon.height, 0,
                                  vi->depth, InputOutput, vi->visual,
                                  CWColormap | CWBackPixel | CWEventMask, &swa);

    /* Set fullscreen property so window managers place it above all panels and docks */
    Atom net_wm_state = XInternAtom(dpy, "_NET_WM_STATE", False);
    Atom net_wm_state_fullscreen = XInternAtom(dpy, "_NET_WM_STATE_FULLSCREEN", False);
    Atom net_wm_state_above = XInternAtom(dpy, "_NET_WM_STATE_ABOVE", False);

    Atom states[2] = { net_wm_state_fullscreen, net_wm_state_above };
    XChangeProperty(dpy, gl_win, net_wm_state, XA_ATOM, 32, PropModeReplace,
                    (unsigned char*)states, 2);

    /* Also remove decorations via Motif hints */
    Atom motif_hints = XInternAtom(dpy, "_MOTIF_WM_HINTS", False);
    unsigned long hints[5] = { 2, 0, 0, 0, 0 }; /* decorations = 0 */
    XChangeProperty(dpy, gl_win, motif_hints, motif_hints, 32, PropModeReplace,
                    (unsigned char*)hints, 5);

    /* Hide cursor if requested */
    Cursor blank_cursor = None;
    Pixmap blank_pix = None;
    if (s_hide_cursor) {
        blank_pix = XCreateBitmapFromData(dpy, gl_win, "\0", 1, 1);
        XColor dummy;
        memset(&dummy, 0, sizeof(dummy));
        blank_cursor = XCreatePixmapCursor(dpy, blank_pix, blank_pix, &dummy, &dummy, 0, 0);
        XDefineCursor(dpy, gl_win, blank_cursor);
    }

    XMapRaised(dpy, gl_win);
    XSetInputFocus(dpy, gl_win, RevertToPointerRoot, CurrentTime);
    XSync(dpy, False);

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

    while (s_running) {
        /* Process X events */
        while (XPending(dpy)) {
            XEvent ev;
            XNextEvent(dpy, &ev);

            if (ev.type == KeyPress) {
                KeySym sym = XLookupKeysym(&ev.xkey, 0);
                if (sym == XK_Escape) {
                    s_running = false;
                    break;
                }
                /* Check Ctrl+Alt+S toggle */
                if ((ev.xkey.state & (ControlMask | Mod1Mask)) == (ControlMask | Mod1Mask) &&
                    (sym == XK_s || sym == XK_S)) {
                    s_running = false;
                    break;
                }
                /* Forward key event to target window */
                ev.xkey.window = target;
                XSendEvent(dpy, target, False, KeyPressMask, &ev);
                XSync(dpy, False);
            } else if (ev.type == KeyRelease) {
                ev.xkey.window = target;
                XSendEvent(dpy, target, False, KeyReleaseMask, &ev);
                XSync(dpy, False);
            } else if (ev.type == ButtonPress || ev.type == ButtonRelease) {
                int cx = ev.xbutton.x - vp_x;
                int cy = ev.xbutton.y - y_from_top;
                if (cx >= 0 && cx < vp_w && cy >= 0 && cy < vp_h) {
                    ev.xbutton.window = target;
                    ev.xbutton.x = (int)lround((double)cx * (double)target_w / (double)vp_w);
                    ev.xbutton.y = (int)lround((double)cy * (double)target_h / (double)vp_h);
                    XSendEvent(dpy, target, False, (ev.type == ButtonPress) ? ButtonPressMask : ButtonReleaseMask, &ev);
                    XSync(dpy, False);
                }
            } else if (ev.type == MotionNotify) {
                int cx = ev.xmotion.x - vp_x;
                int cy = ev.xmotion.y - y_from_top;
                if (cx >= 0 && cx < vp_w && cy >= 0 && cy < vp_h) {
                    ev.xmotion.window = target;
                    ev.xmotion.x = (int)lround((double)cx * (double)target_w / (double)vp_w);
                    ev.xmotion.y = (int)lround((double)cy * (double)target_h / (double)vp_h);
                    XSendEvent(dpy, target, False, PointerMotionMask, &ev);
                    XSync(dpy, False);
                }
            }
        }

        if (!s_running) break;

        /* Verify target window still exists */
        unsigned int cur_w = 0, cur_h = 0;
        if (!XGetGeometry(dpy, target, &r_ret, &tx, &ty, &cur_w, &cur_h, &tb, &td)) {
            /* Target window closed */
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

    /* Cleanup */
    if (blank_cursor != None) {
        XUndefineCursor(dpy, gl_win);
        XFreeCursor(dpy, blank_cursor);
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

    /* Refocus target window */
    XSetInputFocus(dpy, target, RevertToPointerRoot, CurrentTime);
    XSync(dpy, False);

    XCloseDisplay(dpy);
    s_running = false;
    return NULL;
}

bool gl_scaler_start(Display *dpy, Window target_win) {
    if (s_running) return false;
    if (target_win == None) return false;

    if (!s_glXBindTexImage || !s_glXReleaseTexImage) {
        if (!gl_scaler_init(dpy)) return false;
    }

    s_target_win = target_win;
    s_running = true;

    if (pthread_create(&s_thread, NULL, gl_render_thread, NULL) != 0) {
        s_running = false;
        return false;
    }
    return true;
}

void gl_scaler_stop(void) {
    if (!s_running) return;
    s_running = false;
    pthread_join(s_thread, NULL);
}

bool gl_scaler_toggle(Display *dpy, Window target_win) {
    if (s_running) {
        gl_scaler_stop();
        return true;
    }
    return gl_scaler_start(dpy, target_win);
}
