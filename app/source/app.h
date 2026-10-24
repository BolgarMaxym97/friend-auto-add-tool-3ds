/*
 * The app's screens and what moves between them. Layout after the approved mock-up
 * (https://claude.ai/artifact/2iZeEnFSEeJkaQFKbizkto), halved to console pixels.
 */
#ifndef APP_H
#define APP_H

#include <3ds.h>
#include <stdbool.h>

#include "planner.h"
#include "worker.h"

#define APP_VERSION "1.0.1"

typedef enum {
    SCREEN_CHECK,
    SCREEN_MAIN,
    SCREEN_LOG,
    SCREEN_PROGRESS,
    SCREEN_DONE,
    SCREEN_REMOVE,
    SCREEN_CONFIRM,
    SCREEN_DEBUG,
    SCREEN_COUNT,
} ScreenId;

typedef struct {
    u32 down, repeat, held;
    bool tapped;
    u16 tap_x, tap_y;
} Input;

typedef struct {
    ScreenId screen;
    ScreenId log_back;     /* where B leaves the Log screen to */
    int focus[SCREEN_COUNT];
    bool show_focus;       /* D-Pad was used since the last tap */
    Model model;
    Worker worker;
    Plan plan;             /* what "Додати друзів" would do now */
    bool startup;
    bool remove_all;
    int log_scroll, log_filter;
    int debug_scroll;
    char toast[200];
    u64 toast_until;
    u64 next_refresh;
    bool relogin;          /* the friends server dropped us: log in again once Wi-Fi is up */
    Handle notify;
    bool quit;
} App;

void app_init(App *app);
void app_update(App *app, const Input *in);
void app_draw_top(const App *app);
void app_draw_bottom(const App *app);
void app_exit(App *app);

#endif
