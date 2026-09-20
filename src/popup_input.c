#include "popup_input.h"
#include <X11/extensions/Xfixes.h>
#include <X11/extensions/shapeconst.h>
#include <math.h>

void popup_input_to_view(const PopupInputTransform *t, int x, int y, int *vx, int *vy) {
    *vx = t->view_x + (int)lround((x - t->source_x) * t->scale_x);
    *vy = t->view_y + (int)lround((y - t->source_y) * t->scale_y);
}

bool popup_input_begin(Display *dpy, Window overlay, PopupInput *input,
                       const PopupInputTransform *transform) {
    if (input->active) return true;
    if (transform->scale_x <= 0 || transform->scale_y <= 0) return false;
    Window root = DefaultRootWindow(dpy), rr, child;
    int rx, ry, wx, wy;
    unsigned int mask;
    if (!XQueryPointer(dpy, root, &rr, &child, &rx, &ry, &wx, &wy, &mask))
        return false;

    int x = transform->source_x + (int)lround((rx - transform->view_x) / transform->scale_x);
    int y = transform->source_y + (int)lround((ry - transform->view_y) / transform->scale_y);
    return popup_input_begin_at(dpy, overlay, input, transform, x, y);
}

bool popup_input_begin_at(Display *dpy, Window overlay, PopupInput *input,
                          const PopupInputTransform *transform, int x, int y) {
    if (input->active) return true;
    int major = 4, minor = 0;
    if (transform->scale_x <= 0 || transform->scale_y <= 0 ||
        !XFixesQueryVersion(dpy, &major, &minor) || major < 4) return false;
    Window root = DefaultRootWindow(dpy);
    XserverRegion empty = XFixesCreateRegion(dpy, NULL, 0);
    XFixesSetWindowShapeRegion(dpy, overlay, ShapeInput, 0, 0, empty);
    XFixesDestroyRegion(dpy, empty);
    XFixesHideCursor(dpy, overlay);
    /* Releases only our implicit press grab, never another client's grab. */
    XUngrabPointer(dpy, CurrentTime);
    XWarpPointer(dpy, None, root, 0, 0, 0, 0, x, y);
    input->transform = *transform;
    input->active = true;
    XFlush(dpy);
    return true;
}

static void end_input(Display *dpy, Window overlay, PopupInput *input, bool force) {
    if (!input->active) return;
    Window root = DefaultRootWindow(dpy), rr, child;
    int rx, ry, wx, wy, x, y;
    unsigned int mask;
    bool have_pointer = XQueryPointer(dpy, root, &rr, &child, &rx, &ry, &wx, &wy, &mask);
    /* A menu can dismiss on press. Do not warp its matching release into
     * overlay coordinates, or let it leak into a newly opened dialog. */
    if (!force && have_pointer && (mask & (Button1Mask | Button2Mask | Button3Mask |
                                           Button4Mask | Button5Mask))) return;
    XFixesSetWindowShapeRegion(dpy, overlay, ShapeInput, 0, 0, None);
    if (have_pointer) {
        popup_input_to_view(&input->transform, rx, ry, &x, &y);
        XWarpPointer(dpy, None, root, 0, 0, 0, 0, x, y);
    }
    XFixesShowCursor(dpy, overlay);
    input->active = false;
    XFlush(dpy);
}

void popup_input_end(Display *dpy, Window overlay, PopupInput *input) {
    end_input(dpy, overlay, input, false);
}

void popup_input_cleanup(Display *dpy, Window overlay, PopupInput *input) {
    end_input(dpy, overlay, input, true);
}
