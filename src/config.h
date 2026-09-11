#ifndef CONFIG_H
#define CONFIG_H

#include <stdbool.h>

#define MAX_AUTOSCALE_RULES 64

typedef struct {
    char identifier[128]; /* class name or window identifier */
    char label[128];      /* human readable title / label */
} AutoScaleRule;

typedef struct {
    bool hide_cursor;
    bool show_notifications;
    AutoScaleRule rules[MAX_AUTOSCALE_RULES];
    int rule_count;
} AppConfig;

void config_init(void);
void config_save(void);

bool config_get_hide_cursor(void);
void config_set_hide_cursor(bool hide);

bool config_get_notifications(void);
void config_set_notifications(bool enabled);

bool config_is_autoscale(const char *res_class, const char *res_name, const char *title);
bool config_add_autoscale(const char *res_class, const char *res_name, const char *title);
bool config_remove_autoscale(const char *identifier);
void config_clear_autoscale(void);

const AppConfig* config_get(void);

#endif /* CONFIG_H */
