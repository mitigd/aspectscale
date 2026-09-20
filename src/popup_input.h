#ifndef POPUP_INPUT_H
#define POPUP_INPUT_H

#include <X11/Xlib.h>
#include <stdbool.h>

typedef struct {
    int source_x, source_y;
    int view_x, view_y; /* viewport origin in root coordinates */
    double scale_x, scale_y;
} PopupInputTransform;

typedef struct {
    bool active;
    PopupInputTransform transform;
} PopupInput;

/* Keep real events in application space while a native menu owns the grab.
 * The renderer displays the cursor using this same transform. */
bool popup_input_begin(Display *dpy, Window overlay, PopupInput *input,
                       const PopupInputTransform *transform);
/* Enter before dispatching a child click: focus/grabs may query or confine the
 * physical cursor immediately. x/y are the intended source root coordinates. */
bool popup_input_begin_at(Display *dpy, Window overlay, PopupInput *input,
                          const PopupInputTransform *transform, int x, int y);
void popup_input_end(Display *dpy, Window overlay, PopupInput *input);
void popup_input_cleanup(Display *dpy, Window overlay, PopupInput *input);
void popup_input_to_view(const PopupInputTransform *t, int x, int y, int *vx, int *vy);

#endif
