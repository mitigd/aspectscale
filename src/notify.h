#ifndef NOTIFY_H
#define NOTIFY_H

#include <stdbool.h>
#include "x11_scale.h"

void notify_init(void);
void notify_window_action(const WindowScaleInfo *info, bool show_notifications);
void notify_message(const char *summary, const char *body);

#endif /* NOTIFY_H */
