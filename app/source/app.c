#include "app.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fc.h"
#include "log.h"
#include "net.h"
#include "store.h"
#include "ui.h"

#define REFRESH_MS 5000
#define TOAST_MS 3500
#define LOG_ROWS 11
#define DEBUG_ROWS 11
#define COL_RED_DARK RGBA(0x7A, 0x18, 0x10, 0xFF)

typedef struct {
    float x, y, w, h;
} Rect;

/* A button D-Pad can land on, and where each direction leads (-1: nowhere). */
typedef struct {
    Rect r;
    int up, down, left, right;
} Focus;

static bool inside(Rect r, u16 x, u16 y)
{
    return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

static bool hit(const Input *in, Rect r)
{
    return in->tapped && inside(r, in->tap_x, in->tap_y);
}

/* Moves the screen's focus with D-Pad / Circle Pad, or onto a tapped button. Returns the
 * button to press now (A, or a tap), or -1. */
static int navigate(App *app, const Input *in, const Focus *f, int n)
{
    int *cur = &app->focus[app->screen];
    if (*cur < 0 || *cur >= n)
        *cur = 0;
    if (in->tapped) {
        app->show_focus = false;
        for (int i = 0; i < n; i++) {
            if (inside(f[i].r, in->tap_x, in->tap_y)) {
                *cur = i;
                return i;
            }
        }
        return -1;
    }
    int next = -1;
    if (in->repeat & (KEY_DUP | KEY_CPAD_UP))
        next = f[*cur].up;
    else if (in->repeat & (KEY_DDOWN | KEY_CPAD_DOWN))
        next = f[*cur].down;
    else if (in->repeat & (KEY_DLEFT | KEY_CPAD_LEFT))
        next = f[*cur].left;
    else if (in->repeat & (KEY_DRIGHT | KEY_CPAD_RIGHT))
        next = f[*cur].right;
    bool always = app->screen == SCREEN_REMOVE || app->screen == SCREEN_CONFIRM;
    u32 dirs = KEY_DUP | KEY_DDOWN | KEY_DLEFT | KEY_DRIGHT | KEY_CPAD_UP | KEY_CPAD_DOWN | KEY_CPAD_LEFT | KEY_CPAD_RIGHT;
    if ((in->repeat & dirs) && !app->show_focus && !always) {
        app->show_focus = true; /* the first press only shows where the focus is */
        next = -1;
    }
    if (next >= 0) {
        *cur = next;
        app->show_focus = true;
    }
    return (in->down & KEY_A) ? *cur : -1;
}

/* The ring shows once D-Pad is used (a tap hides it again); on the two removal screens the
 * choice A would make is always marked. */
static bool focused(const App *app, int i)
{
    bool always = app->screen == SCREEN_REMOVE || app->screen == SCREEN_CONFIRM;
    return app->focus[app->screen] == i && (always || app->show_focus);
}

static void go(App *app, ScreenId screen, int focus)
{
    app->screen = screen;
    app->focus[screen] = focus;
}

static void toast(App *app, const char *text)
{
    snprintf(app->toast, sizeof(app->toast), "%s", text);
    app->toast_until = osGetTime() + TOAST_MS;
}

static const char *plural(int n, const char *one, const char *few, const char *many)
{
    int t = abs(n) % 100, u = t % 10;
    if (t > 10 && t < 20)
        return many;
    if (u == 1)
        return one;
    if (u >= 2 && u <= 4)
        return few;
    return many;
}

/* ---------------------------------------------------------------- state */

static int free_places(const App *app)
{
    int n = FRIENDS_MAX - app->model.friends.count;
    return n > 0 ? n : 0;
}

static void update_plan(App *app)
{
    static u64 codes[FRIENDS_MAX];
    const FriendList *l = &app->model.friends;
    for (int i = 0; i < l->count; i++)
        codes[i] = l->items[i].code;
    plan_make(&app->plan, &app->model.db, app->model.console.my_code, codes, l->count);
}

/* Re-reads the list and logs every request that turned into a friend since the last look.
 * The friends module does this itself in the background while online; we only notice. */
static void refresh_friends(App *app)
{
    static FriendList fresh;
    FriendList *old = &app->model.friends;
    if (app->worker.busy || !app->model.console.frd_admin || R_FAILED(friends_list(&fresh)))
        return;
    for (int i = 0; i < fresh.count; i++) {
        const Friend *f = &fresh.items[i];
        if (f->relationship != RELATIONSHIP_COMPLETE)
            continue;
        for (int j = 0; j < old->count; j++) {
            if (old->items[j].key.principalId == f->key.principalId && old->items[j].relationship != RELATIONSHIP_COMPLETE) {
                char code[FC_STR_LEN];
                fc_format(f->code, code);
                log_add(LOG_NOW_MUTUAL, f->code, "%s", f->name[0] ? f->name : code);
            }
        }
    }
    memcpy(old, &fresh, sizeof(fresh));
    app->model.friends_ok = true;
    update_plan(app);
}

static const char *add_block(const App *app, const char **shortly)
{
    const Model *m = &app->model;
    const char *s = NULL, *text = NULL;
    if (!m->console.frd_admin) {
        s = "немає доступу до друзів";
        text = "Немає доступу до сервісу друзів. Запустіть CIA-версію застосунку.";
    } else if (!m->console.my_code) {
        s = "немає облікового запису";
        text = "На консолі немає облікового запису друзів.";
    } else if (!m->db_present || m->db.count == 0) {
        s = "спершу оновіть базу";
        text = "Бази кодів ще немає: спершу натисніть «Оновити базу».";
    } else if (!m->console.wifi) {
        s = "немає Wi-Fi";
        text = "Увімкніть Wi-Fi: заявки надсилаються через сервер.";
    } else if (m->friends.count >= FRIENDS_MAX) {
        s = "список друзів повний";
        text = "У списку вже 100 друзів. Звільніть місця кнопкою «Видалити друзів».";
    } else if (app->plan.add == 0) {
        s = "усі коди з бази вже у списку";
        text = "Усі коди з бази вже у вашому списку. Оновіть базу пізніше.";
    }
    if (shortly)
        *shortly = s;
    return text;
}

static const char *update_block(const App *app)
{
    if (!app->model.console.wifi)
        return "Увімкніть Wi-Fi, щоб завантажити базу.";
    if (!app->model.console.sd_ok)
        return "Не вдається писати на SD-карту.";
    return NULL;
}

static const char *remove_block(const App *app)
{
    if (!app->model.console.frd_admin)
        return "Немає доступу до сервісу друзів. Запустіть CIA-версію застосунку.";
    if (app->model.friends.count == 0)
        return "Список друзів порожній.";
    return NULL;
}

static void start_job(App *app, Job job)
{
    if (app->worker.thread) {
        /* only the background re-login runs unasked */
        toast(app, "Зачекайте: вхід у сервіс друзів…");
        return;
    }
    worker_start(&app->worker, job, &app->model);
    if (!app->worker.thread) {
        toast(app, app->worker.message);
        return;
    }
    if (job != JOB_CHECK && job != JOB_LOGIN)
        go(app, SCREEN_PROGRESS, 0);
}

/* ---------------------------------------------------------------- check rows */

typedef struct {
    Level level;
    const char *label;
    char value[64];
    const char *advice;
} Row;

#define CHECK_ROWS 11

static int check_rows(const App *app, Row *rows)
{
    const Model *m = &app->model;
    const Console *c = &m->console;
    int n = 0;
    Row *r;

#define ROW(lvl, lbl, adv, ...)                                  \
    do {                                                         \
        r = &rows[n++];                                          \
        r->level = (lvl);                                        \
        r->label = (lbl);                                        \
        r->advice = (adv);                                       \
        snprintf(r->value, sizeof(r->value), __VA_ARGS__);       \
    } while (0)

    char buf[24];
    if (c->is_luma) {
        checks_luma_string(c, buf, sizeof(buf));
        ROW(LEVEL_OK, "Luma3DS", NULL, "%s", buf);
    } else {
        ROW(LEVEL_FAIL, "Luma3DS", "Не знайдено Luma3DS. Встановіть її за інструкцією 3ds.hacks.guide.", "не знайдено");
    }

    if (c->frd_admin)
        ROW(LEVEL_OK, "Сервіс друзів (frd:a)", NULL, "доступ є");
    else
        ROW(LEVEL_FAIL, "Сервіс друзів (frd:a)",
            "Немає доступу до сервісу друзів. Встановіть і запустіть CIA-версію застосунку, а не .3dsx.", "немає доступу");

    if (c->my_code) {
        fc_format(c->my_code, buf);
        ROW(LEVEL_OK, "Ваш код друга", NULL, "%s", buf);
    } else {
        ROW(LEVEL_FAIL, "Ваш код друга",
            "На консолі немає облікового запису друзів. Відкрийте «Список друзів» у меню HOME, щоб його створити.",
            "немає");
    }

    if (c->account == 2)
        ROW(LEVEL_OK, "Pretendo (Nimbus)", NULL, "увімкнено");
    else if (c->nimbus)
        ROW(LEVEL_WARN, "Pretendo (Nimbus)",
            "У Nimbus вибрано Nintendo Network, яка вже не працює. Відкрийте Nimbus і перемкніть на Pretendo.",
            "вибрано Nintendo");
    else
        ROW(LEVEL_WARN, "Pretendo (Nimbus)",
            "Не знайдено Nimbus - додаток для Pretendo Network. Без нього консоль не входить у сервіс друзів, і заявки "
            "не надсилаються. Встановіть Nimbus за інструкцією pretendo.network і перезапустіть консоль.",
            "не знайдено");

    if (c->game_patching || !c->is_luma)
        ROW(c->is_luma ? LEVEL_OK : LEVEL_WARN, "Патчі ігор у Luma", NULL, c->is_luma ? "увімкнено" : "-");
    else
        ROW(LEVEL_WARN, "Патчі ігор у Luma",
            "У Luma вимкнено «Enable game patching», а без нього Nimbus не працює. Утримуйте SELECT, вмикаючи "
            "консоль, і ввімкніть цей пункт.",
            "вимкнено");

    if (c->wifi)
        ROW(LEVEL_OK, "Wi-Fi", NULL, "підключено");
    else
        ROW(LEVEL_FAIL, "Wi-Fi", "Увімкніть Wi-Fi і зачекайте, поки консоль під'єднається, тоді перевірте знову.",
            "вимкнено");

    const char *login_advice =
        "Сервіс друзів не підтвердив вхід, а без нього коди не додаються. «Додати друзів» спробує увійти ще раз. "
        "Якщо «Список друзів» у меню HOME теж дає помилку - сервер Pretendo недоступний, спробуйте пізніше.";
    if (c->online)
        ROW(LEVEL_OK, "Вхід у сервіс друзів", NULL, "виконано");
    else if (!c->wifi)
        ROW(LEVEL_INFO, "Вхід у сервіс друзів", NULL, "потрібен Wi-Fi");
    else if (R_FAILED(c->login_result))
        ROW(LEVEL_WARN, "Вхід у сервіс друзів", login_advice, "не підтверджено 0x%08lX", c->login_result);
    else
        ROW(LEVEL_WARN, "Вхід у сервіс друзів", login_advice, "не підтверджено");

    if (!m->friends_ok)
        ROW(LEVEL_INFO, "Місця у списку друзів", NULL, "невідомо");
    else if (m->friends.count < FRIENDS_MAX)
        ROW(LEVEL_OK, "Місця у списку друзів", NULL, "%d з 100 вільно", FRIENDS_MAX - m->friends.count);
    else
        ROW(LEVEL_WARN, "Місця у списку друзів", "Список друзів повний. Звільніть місця кнопкою «Видалити друзів».",
            "список повний");

    if (c->sd_ok)
        ROW(LEVEL_OK, "SD-карта", NULL, "запис працює");
    else
        ROW(LEVEL_FAIL, "SD-карта", "Не вдається писати на SD-карту. Перевірте, чи вона не заблокована й не заповнена.",
            "не пише");

    if (m->db_present && m->db.count)
        ROW(LEVEL_OK, "База кодів", NULL, "%d %s", m->db.count, plural(m->db.count, "код", "коди", "кодів"));
    else
        ROW(LEVEL_INFO, "База кодів", "Бази кодів ще немає. На головному екрані натисніть «Оновити базу».",
            "оновіть базу");

    if (c->battery < BATTERY_MIN && !c->charging)
        ROW(LEVEL_WARN, "Заряд", "Низький заряд. Підключіть зарядку.", "низький");
    else
        ROW(LEVEL_OK, "Заряд", NULL, "%s", c->charging ? "заряджається" : "достатньо");
#undef ROW
    return n;
}

static Level worst(const Row *rows, int n, const Row **first)
{
    Level w = LEVEL_OK;
    *first = NULL;
    for (Level l = LEVEL_FAIL; l > LEVEL_OK && !*first; l--)
        for (int i = 0; i < n; i++)
            if (rows[i].level == l && rows[i].advice) {
                *first = &rows[i];
                w = l;
                break;
            }
    return w;
}

/* ---------------------------------------------------------------- drawing helpers */

static void draw_primary(Rect r, Icon icon, const char *title, const char *subtitle, bool enabled, bool focus, u32 color,
                         u32 shadow)
{
    if (focus)
        ui_focus(r.x, r.y, r.w, r.h + (enabled ? 3 : 0), 14);
    if (enabled) {
        ui_rrect(r.x, r.y + 3, r.w, r.h, 14, shadow);
        ui_rrect(r.x, r.y, r.w, r.h, 14, color);
    } else {
        ui_rrect(r.x, r.y, r.w, r.h, 14, COL_DISABLED);
    }
    u32 ink = enabled ? COL_WHITE : COL_MUTED;
    if (subtitle) {
        ui_icon(icon, r.x + 34, r.y + r.h / 2, 32, ink);
        ui_text(r.x + 66, r.y + r.h / 2 - 24, 22, FONT_HEAVY, ink, title);
        ui_text(r.x + 66, r.y + r.h / 2 + 5, 11, FONT_BOLD, ink, subtitle);
    } else {
        float w = 22 + 8 + ui_text_width(18, FONT_HEAVY, title);
        float x = r.x + (r.w - w) / 2;
        ui_icon(icon, x + 11, r.y + r.h / 2, 22, ink);
        ui_text(x + 30, r.y + r.h / 2 - 13, 18, FONT_HEAVY, ink, title);
    }
}

static void draw_card_button(Rect r, bool focus)
{
    if (focus)
        ui_focus(r.x - 1, r.y - 1, r.w + 2, r.h + 3.5f, 12);
    ui_card(r.x, r.y, r.w, r.h, 11, COL_CARD, 1.5f);
}

static void draw_tile(Rect r, Icon icon, const char *label, bool enabled, bool focus)
{
    u32 ink = enabled ? COL_INK : COL_FAINT;
    draw_card_button(r, focus);
    ui_icon(icon, r.x + 11 + 9, r.y + 9 + 9, 18, ink);
    /* 12 px where the label fits the tile, "Оновити базу" may need a step less */
    float px = 12;
    while (px > 10 && ui_text_width(px, FONT_HEAVY, label) > r.w - 20)
        px -= 0.5f;
    ui_text(r.x + 11, r.y + r.h - 24.5f, px, FONT_HEAVY, ink, label);
}

/* Up to four key hints spread across the bottom line. */
static void draw_hints(int n, const char *const keys[], const char *const labels[])
{
    float widths[4], total = 0;
    for (int i = 0; i < n; i++) {
        widths[i] = ui_key_hint_width(keys[i], labels[i]);
        total += widths[i];
    }
    float gap = n > 1 ? (BOTTOM_W - 28 - total) / (n - 1) : 0, x = 14;
    for (int i = 0; i < n; i++) {
        ui_key_hint(x, 212, keys[i], labels[i]);
        x += widths[i] + gap;
    }
}

static void draw_toast(const App *app)
{
    if (!app->toast[0] || osGetTime() > app->toast_until)
        return;
    float h = ui_text_wrap_height(276, 10.5f, FONT_BOLD, app->toast) + 10;
    float y = 206 - h;
    ui_rrect(14, y, 292, h, 8, COL_AMBER_BG);
    ui_text_wrap(22, y + 4, 276, 10.5f, FONT_BOLD, COL_AMBER_TEXT, app->toast);
}

static void draw_badge(float right, float cy, const char *text, LogKind kind)
{
    u32 bg = COL_LINE, fg = COL_MUTED;
    switch (kind) {
    case LOG_MUTUAL:
    case LOG_NOW_MUTUAL: bg = COL_GREEN_BG; fg = COL_GREEN_TEXT; break;
    case LOG_REQUEST: bg = COL_BLUE_BG; fg = COL_BLUE; break;
    case LOG_ERROR: bg = COL_AMBER_BG; fg = COL_AMBER_TEXT; break;
    default: break;
    }
    float w = ui_text_width(8.5f, FONT_HEAVY, text) + 12;
    ui_rrect(right - w, cy - 6.5f, w, 13, 6.5f, bg);
    ui_text(right - w + 6, cy - 6.5f, 8.5f, FONT_HEAVY, fg, text);
}

/* "2376-3094-6334 · vaflik" or the message itself. */
static void log_label(const LogLine *l, char *out, size_t cap)
{
    if (l->code) {
        char code[FC_STR_LEN];
        fc_format(l->code, code);
        snprintf(out, cap, "%s · %s", code, l->text);
    } else {
        snprintf(out, cap, "%s", l->text);
    }
}

/* ---------------------------------------------------------------- check */

static const Focus CHECK_FOCUS[] = {{{14, 130, 292, 56}, -1, -1, -1, -1}};

static void draw_status_dot(Level level, float cx, float cy)
{
    static const struct {
        u32 bg, fg;
        Icon icon;
    } look[] = {
        [LEVEL_OK] = {COL_GREEN_BG, COL_GREEN_TEXT, ICON_CHECK},
        [LEVEL_INFO] = {COL_BLUE_BG, COL_BLUE, ICON_INFO},
        [LEVEL_WARN] = {COL_YELLOW, COL_INK, ICON_INFO},
        [LEVEL_FAIL] = {COL_YELLOW, COL_INK, ICON_INFO},
    };
    ui_circle(cx, cy, 6.5f, look[level].bg);
    if (level == LEVEL_WARN || level == LEVEL_FAIL) {
        /* "!": the info glyph upside down */
        ui_rect(cx - 0.9f, cy - 4, 1.8f, 4.5f, COL_INK);
        ui_circle(cx, cy + 3, 1.1f, COL_INK);
    } else {
        ui_icon(look[level].icon, cx, cy, 9, look[level].fg);
    }
}

static void draw_check_top(const App *app)
{
    Row rows[CHECK_ROWS];
    int n = check_rows(app, rows);
    bool waiting = app->worker.busy && (app->worker.job == JOB_CHECK || app->worker.job == JOB_LOGIN);
    ui_text(18, 7, 17, FONT_HEAVY, COL_INK, "Перевірка консолі");
    for (int i = 0; i < n; i++) {
        float y = 34 + i * 17.5f, mid = y + 7.5f;
        bool bad = !waiting && (rows[i].level == LEVEL_FAIL || rows[i].level == LEVEL_WARN);
        ui_rrect(18, y, 364, 15, 7, bad ? COL_AMBER_BG : COL_CARD);
        if (waiting)
            ui_circle(33.5f, mid, 6.5f, COL_LINE);
        else
            draw_status_dot(rows[i].level, 33.5f, mid);
        ui_text(47, mid - 7.5f, 10.5f, bad ? FONT_HEAVY : FONT_BOLD, COL_INK, rows[i].label);
        u32 vc = bad ? COL_AMBER_TEXT : rows[i].level == LEVEL_INFO ? COL_BLUE : COL_MUTED;
        ui_text_right(373, mid - 7.5f, 10.5f, bad || rows[i].level == LEVEL_INFO ? FONT_HEAVY : FONT_BOLD,
                      waiting ? COL_FAINT : vc, waiting ? "…" : rows[i].value);
    }
}

static void draw_check_bottom(const App *app)
{
    bool waiting = app->worker.busy && (app->worker.job == JOB_CHECK || app->worker.job == JOB_LOGIN);
    Row rows[CHECK_ROWS];
    const Row *first;
    Level level = worst(rows, check_rows(app, rows), &first);
    const char *title, *text, *button;
    if (waiting) {
        title = "Перевіряю консоль…";
        text = "Вхід у сервіс друзів може зайняти до 15 секунд.";
        button = "Зачекайте…";
    } else if (first) {
        title = level == LEVEL_FAIL ? "Потрібна дія" : "Майже готово";
        text = first->advice;
        button = "Перевірити знову";
    } else {
        title = "Усе готово";
        text = "Консоль готова додавати друзів.";
        button = "До меню";
    }
    ui_text(14, 10, 16, FONT_HEAVY, COL_INK, title);
    ui_text_wrap(14, 34, 292, 11.5f, FONT_BOLD, COL_BODY, text);
    Rect b = CHECK_FOCUS[0].r;
    draw_primary(b, first || waiting ? ICON_UPDATE : ICON_CHECK, button, NULL, !waiting, false, COL_BLUE, COL_BLUE_DARK);
    if (waiting)
        return;
    if (first)
        draw_hints(2, (const char *[]){"A", "B"}, (const char *[]){"Перевірити знову", "До меню"});
    else
        draw_hints(1, (const char *[]){"A"}, (const char *[]){button});
}

static void update_check(App *app, const Input *in)
{
    if (app->worker.busy)
        return;
    Row rows[CHECK_ROWS];
    const Row *first;
    worst(rows, check_rows(app, rows), &first);
    int pressed = navigate(app, in, CHECK_FOCUS, 1);
    if (pressed == 0)
        first ? start_job(app, JOB_CHECK) : go(app, SCREEN_MAIN, 0);
    else if (in->down & KEY_B)
        go(app, SCREEN_MAIN, app->focus[SCREEN_MAIN]);
}

/* ---------------------------------------------------------------- main */

enum { MAIN_ADD, MAIN_UPDATE, MAIN_LOG, MAIN_CHECK, MAIN_REMOVE };
static const Focus MAIN_FOCUS[] = {
    [MAIN_ADD] = {{14, 14, 292, 78}, -1, MAIN_UPDATE, -1, -1},
    [MAIN_UPDATE] = {{14, 100, 92, 56}, MAIN_ADD, MAIN_REMOVE, -1, MAIN_LOG},
    [MAIN_LOG] = {{114, 100, 92, 56}, MAIN_ADD, MAIN_REMOVE, MAIN_UPDATE, MAIN_CHECK},
    [MAIN_CHECK] = {{214, 100, 92, 56}, MAIN_ADD, MAIN_REMOVE, MAIN_LOG, -1},
    [MAIN_REMOVE] = {{14, 164, 292, 32}, MAIN_UPDATE, -1, -1, -1},
};

static void info_row_line(float y)
{
    ui_rect(36, y, 328, 1, COL_LINE);
}

static void draw_main_top(const App *app)
{
    const Model *m = &app->model;
    const Console *c = &m->console;
    char buf[64];

    ui_rrect(22, 18, 48, 48, 12, COL_BLUE);
    ui_icon(ICON_USER_PLUS, 46, 42, 29, COL_WHITE);
    ui_text(82, 16, 22, FONT_HEAVY, COL_INK, "Friend Auto Add Tool");
    ui_icon(ICON_TELEGRAM, 88, 50, 12, COL_TELEGRAM);
    ui_text(99, 43, 11.5f, FONT_BOLD, COL_MUTED, "для t.me/n3ds_ua · v" APP_VERSION);

    ui_card(22, 78, 356, 134, 10, COL_CARD, 1);

    ui_text(36, 87, 12, FONT_BOLD, COL_MUTED, "Ваш код друга");
    if (c->my_code)
        fc_format(c->my_code, buf);
    else
        snprintf(buf, sizeof(buf), "-");
    ui_text_right(364, 86, 14, FONT_HEAVY, COL_INK, buf);
    info_row_line(110);

    const FriendList *l = &m->friends;
    ui_text(36, 114, 12, FONT_BOLD, COL_MUTED, "Друзів");
    snprintf(buf, sizeof(buf), "%d %s · ", l->mutual, plural(l->mutual, "взаємний", "взаємних", "взаємних"));
    float x = 36 + ui_text(36, 131, 10, FONT_BOLD, COL_GREEN_TEXT, buf);
    snprintf(buf, sizeof(buf), "%d %s", l->requests, plural(l->requests, "заявка", "заявки", "заявок"));
    ui_text(x, 131, 10, FONT_BOLD, COL_BLUE, buf);
    snprintf(buf, sizeof(buf), m->friends_ok ? "%d / 100" : "? / 100", l->count);
    ui_text_right(364, 112, 14, FONT_HEAVY, COL_INK, buf);
    ui_rrect(264, 137, 100, 5, 2.5f, COL_TRACK);
    float wm = l->mutual, wr = l->requests;
    if (wm + wr > 0) {
        ui_rrect(264, 137, wm + wr, 5, 2.5f, COL_BLUE);
        if (wm > 0)
            ui_rrect(264, 137, wm, 5, 2.5f, COL_GREEN);
    }
    info_row_line(149);

    ui_text(36, 157, 12, FONT_BOLD, COL_MUTED, "Кодів у базі");
    if (m->db_present && m->db.count) {
        float right = 364;
        if (m->db_updated) {
            struct tm *tm = gmtime(&m->db_updated);
            char when[48] = ""; /* " · оновлено 24.10, 13:42" is 33 bytes in UTF-8 */
            if (tm)
                strftime(when, sizeof(when), " · оновлено %d.%m, %H:%M", tm);
            right -= ui_text_right(364, 157, 12, FONT_BOLD, COL_FAINT, when);
        }
        snprintf(buf, sizeof(buf), "%d", m->db.count);
        ui_text_right(right, 156, 14, FONT_HEAVY, COL_INK, buf);
    } else {
        ui_text_right(364, 157, 12, FONT_HEAVY, COL_BLUE, "оновіть базу");
    }
    info_row_line(180);

    ui_text(36, 188, 12, FONT_BOLD, COL_MUTED, "Інтернет");
    const char *net;
    u32 color;
    if (app->relogin || (app->worker.busy && app->worker.job == JOB_LOGIN)) {
        /* after sleep: Wi-Fi comes back first, then the background login */
        float w = ui_text_right(364, 188, 12, FONT_BOLD, COL_BLUE, c->wifi ? "Відновлюю вхід…" : "Чекаю на Wi-Fi…");
        ui_spinner(364 - w - 10, 196, 4.5f, COL_BLUE);
        return;
    }
    if (!c->wifi) {
        net = "Немає з'єднання";
        color = COL_RED;
    } else if (c->online) {
        net = c->account == 2 ? "Підключено · Pretendo" : "Підключено";
        color = COL_GREEN_TEXT;
    } else {
        net = "Wi-Fi є · вхід не підтверджено";
        color = COL_AMBER;
    }
    float w = ui_text_right(364, 188, 12, FONT_BOLD, color, net);
    ui_circle(364 - w - 9, 196, 3, color);
}

static void draw_main_bottom(const App *app)
{
    char sub[96];
    const char *shortly;
    bool can_add = !add_block(app, &shortly);
    if (can_add) {
        int add = app->plan.add, places = free_places(app);
        snprintf(sub, sizeof(sub), "з бази: %d %s %s · вільно %d %s", add,
                 (add % 10 == 1 && add % 100 != 11) ? "новий" : "нових", plural(add, "код", "коди", "кодів"), places,
                 plural(places, "місце", "місця", "місць"));
    } else {
        snprintf(sub, sizeof(sub), "%s", shortly);
    }
    draw_primary(MAIN_FOCUS[MAIN_ADD].r, ICON_USER_PLUS, "Додати друзів", sub, can_add, focused(app, MAIN_ADD), COL_BLUE,
                 COL_BLUE_DARK);
    draw_tile(MAIN_FOCUS[MAIN_UPDATE].r, ICON_UPDATE, "Оновити базу", !update_block(app), focused(app, MAIN_UPDATE));
    draw_tile(MAIN_FOCUS[MAIN_LOG].r, ICON_LINES, "Журнал", true, focused(app, MAIN_LOG));
    draw_tile(MAIN_FOCUS[MAIN_CHECK].r, ICON_CHECK_CIRCLE, "Перевірка", true, focused(app, MAIN_CHECK));

    Rect r = MAIN_FOCUS[MAIN_REMOVE].r;
    draw_card_button(r, focused(app, MAIN_REMOVE));
    u32 red = remove_block(app) ? COL_FAINT : COL_RED;
    float w = 14 + 6 + ui_text_width(12.5f, FONT_HEAVY, "Видалити друзів");
    float x = r.x + (r.w - w) / 2;
    ui_icon(ICON_TRASH, x + 7, r.y + r.h / 2, 14, red);
    ui_text(x + 20, r.y + r.h / 2 - 9, 12.5f, FONT_HEAVY, red, "Видалити друзів");

    draw_hints(4, (const char *[]){"A", "X", "Y", "START"}, (const char *[]){"Дія", "База", "Журнал", "Вийти"});
    draw_toast(app);
}

static void press_main(App *app, int button)
{
    const char *why;
    switch (button) {
    case MAIN_ADD:
        if ((why = add_block(app, NULL)))
            toast(app, why);
        else
            start_job(app, JOB_ADD);
        break;
    case MAIN_UPDATE:
        if ((why = update_block(app)))
            toast(app, why);
        else
            start_job(app, JOB_UPDATE);
        break;
    case MAIN_LOG:
        app->log_back = SCREEN_MAIN;
        app->log_scroll = 0;
        go(app, SCREEN_LOG, 0);
        break;
    case MAIN_CHECK:
        go(app, SCREEN_CHECK, 0);
        start_job(app, JOB_CHECK);
        break;
    case MAIN_REMOVE:
        if ((why = remove_block(app)))
            toast(app, why);
        else
            go(app, SCREEN_REMOVE, 0);
        break;
    }
}

static void update_main(App *app, const Input *in)
{
    if ((in->held & (KEY_L | KEY_R)) == (KEY_L | KEY_R) && (in->down & KEY_Y)) {
        app->debug_scroll = 0;
        go(app, SCREEN_DEBUG, 0);
        return;
    }
    if (in->down & KEY_X) {
        app->focus[SCREEN_MAIN] = MAIN_UPDATE;
        press_main(app, MAIN_UPDATE);
        return;
    }
    if (in->down & KEY_Y) {
        press_main(app, MAIN_LOG);
        return;
    }
    int pressed = navigate(app, in, MAIN_FOCUS, sizeof(MAIN_FOCUS) / sizeof(*MAIN_FOCUS));
    if (pressed >= 0)
        press_main(app, pressed);
}

/* ---------------------------------------------------------------- log */

static const char *const FILTERS[] = {"Усе", "Взаємні", "Заявки", "Помилки"};
#define FILTER_COUNT 4
static const Rect LOG_UP = {14, 74, 142, 44}, LOG_DOWN = {164, 74, 142, 44};

static bool log_match(const LogLine *l, int filter)
{
    switch (filter) {
    case 1: return l->kind == LOG_MUTUAL || l->kind == LOG_NOW_MUTUAL;
    case 2: return l->kind == LOG_REQUEST;
    case 3: return l->kind == LOG_ERROR;
    }
    return true;
}

/* The `skip`-th line (newest first) that passes the filter; the count of all that pass. */
static int log_filtered(int filter, int skip, LogLine *out, int max)
{
    int total = 0, got = 0;
    LogLine l;
    for (int i = 0; log_get(i, &l); i++) {
        if (!log_match(&l, filter))
            continue;
        if (total >= skip && got < max)
            out[got++] = l;
        total++;
    }
    return total;
}

static float chip_width(int i)
{
    return ui_text_width(12, FONT_HEAVY, FILTERS[i]) + 24;
}

static Rect chip_rect(int i)
{
    float x = 14;
    for (int j = 0; j < i; j++)
        x += chip_width(j) + 6;
    return (Rect){x, 30, chip_width(i), 28};
}

static void draw_log_top(const App *app)
{
    LogLine lines[LOG_ROWS];
    int total = log_filtered(app->log_filter, app->log_scroll, lines, LOG_ROWS);
    int shown = total - app->log_scroll < LOG_ROWS ? total - app->log_scroll : LOG_ROWS;
    char text[96];

    ui_text(16, 8, 16, FONT_HEAVY, COL_INK, "Журнал");
    ui_text_right(384, 13, 10, FONT_BOLD, COL_MUTED, "останні 200 · log.txt на SD");
    if (total == 0)
        ui_text(16, 44, 11, FONT_BOLD, COL_FAINT, "Поки що порожньо.");
    for (int i = 0; i < shown; i++) {
        const LogLine *l = &lines[i];
        float y = 34 + i * 17, mid = y + 8.5f;
        ui_text(16, mid - 7, 10, FONT_BOLD, COL_FAINT, l->time);
        const char *badge = log_badge(l->kind);
        if (l->code) {
            fc_format(l->code, text);
            ui_text(45, mid - 7, 10, FONT_BOLD, COL_INK, text);
            ui_text(132, mid - 7, 10, FONT_BOLD, COL_BODY, l->text);
        } else {
            ui_text(45, mid - 7, 10, l->kind == LOG_DONE ? FONT_HEAVY : FONT_BOLD, l->kind == LOG_DONE ? COL_BLUE : COL_BODY,
                    l->text);
        }
        if (badge)
            draw_badge(384, mid, badge, l->kind);
        if (i < shown - 1)
            ui_rect(16, y + 16.5f, 368, 1, COL_LINE);
    }
    if (total > LOG_ROWS) {
        float track = 186, thumb = track * LOG_ROWS / total;
        ui_rrect(394, 36, 3, track, 1.5f, COL_TRACK);
        ui_rrect(394, 36 + (track - thumb) * app->log_scroll / (total - LOG_ROWS), 3, thumb, 1.5f, COL_MUTED);
    }
}

static void draw_log_bottom(const App *app)
{
    ui_text(14, 10, 11, FONT_BOLD, COL_MUTED, "Показати");
    for (int i = 0; i < FILTER_COUNT; i++) {
        Rect r = chip_rect(i);
        bool on = app->log_filter == i;
        if (on) {
            ui_rrect(r.x, r.y + 1.5f, r.w, r.h, 14, COL_BLUE_DARK);
            ui_rrect(r.x, r.y, r.w, r.h, 14, COL_BLUE);
        } else {
            ui_card(r.x, r.y, r.w, r.h, 14, COL_CARD, 1.5f);
        }
        ui_text_center(r.x + r.w / 2, r.y + 5.5f, 12, FONT_HEAVY, on ? COL_WHITE : COL_INK, FILTERS[i]);
    }
    const Rect *arrows[2] = {&LOG_UP, &LOG_DOWN};
    const char *labels[2] = {"Вгору", "Вниз"};
    for (int i = 0; i < 2; i++) {
        Rect r = *arrows[i];
        ui_card(r.x, r.y, r.w, r.h, 11, COL_CARD, 1.5f);
        float w = 15 + 5 + ui_text_width(12, FONT_HEAVY, labels[i]);
        float x = r.x + (r.w - w) / 2;
        ui_icon(i ? ICON_DOWN : ICON_UP, x + 7.5f, r.y + r.h / 2, 15, COL_INK);
        ui_text(x + 20, r.y + r.h / 2 - 9, 12, FONT_HEAVY, COL_INK, labels[i]);
    }
    draw_hints(3, (const char *[]){"D-Pad", "L/R", "B"}, (const char *[]){"Прокрутка", "Фільтр", "Назад"});
}

static void update_log(App *app, const Input *in)
{
    LogLine dummy;
    int total = log_filtered(app->log_filter, 0, &dummy, 0);
    int max_scroll = total > LOG_ROWS ? total - LOG_ROWS : 0;
    int step = 0;
    if ((in->repeat & (KEY_DDOWN | KEY_CPAD_DOWN)) || hit(in, LOG_DOWN))
        step = in->tapped ? LOG_ROWS - 1 : 1;
    else if ((in->repeat & (KEY_DUP | KEY_CPAD_UP)) || hit(in, LOG_UP))
        step = in->tapped ? -(LOG_ROWS - 1) : -1;
    app->log_scroll += step;

    int filter = app->log_filter;
    if (in->down & (KEY_R | KEY_DRIGHT))
        filter = (filter + 1) % FILTER_COUNT;
    else if (in->down & (KEY_L | KEY_DLEFT))
        filter = (filter + FILTER_COUNT - 1) % FILTER_COUNT;
    for (int i = 0; i < FILTER_COUNT; i++)
        if (hit(in, chip_rect(i)))
            filter = i;
    if (filter != app->log_filter) {
        app->log_filter = filter;
        app->log_scroll = 0;
        total = log_filtered(filter, 0, &dummy, 0);
        max_scroll = total > LOG_ROWS ? total - LOG_ROWS : 0;
    }
    if (app->log_scroll > max_scroll)
        app->log_scroll = max_scroll;
    if (app->log_scroll < 0)
        app->log_scroll = 0;
    if (in->down & KEY_B)
        app->screen = app->log_back;
}

/* ---------------------------------------------------------------- progress */

static const Focus PROGRESS_FOCUS[] = {{{14, 124, 292, 38}, -1, -1, -1, -1}};

static void draw_counter(float cx, int value, const char *label, u32 color)
{
    char buf[12];
    snprintf(buf, sizeof(buf), "%d", value);
    ui_text_center(cx, 124, 18, FONT_HEAVY, color, buf);
    ui_text_center(cx, 150, 9, FONT_BOLD, COL_MUTED, label);
}

static void draw_progress_top(const App *app)
{
    const Worker *w = &app->worker;
    char buf[96];
    float fraction;
    if (w->job == JOB_UPDATE) {
        fraction = (float)(osGetTime() % 1500) / 1500.0f; /* no size known in advance: a sweep */
        snprintf(buf, sizeof(buf), "…");
    } else {
        fraction = w->total ? (float)w->done / w->total : 0;
        snprintf(buf, sizeof(buf), "%d / %d", w->done, w->total);
    }
    ui_ring(80, 120, 60, 49, fraction, COL_BLUE, COL_TRACK);
    ui_text_center(80, 98, buf[0] == '\xE2' ? 30 : 26, FONT_HEAVY, COL_INK, buf);
    const char *caption = w->job == JOB_ADD ? "коди з бази" : w->job == JOB_UPDATE ? "завантаження" : "видалення";
    ui_text_center(80, 131, 11, FONT_BOLD, COL_MUTED, caption);

    ui_card(158, 40, 222, 74, 10, COL_CARD, 1);
    const char *what = w->job == JOB_ADD ? "Зараз додаю" : w->job == JOB_UPDATE ? "Оновлюю базу" : "Зараз видаляю";
    ui_text(169, 47, 9.5f, FONT_BOLD, COL_MUTED, what);
    if (w->job == JOB_UPDATE) {
        ui_text(169, 60, 13, FONT_HEAVY, COL_INK, "GitHub");
        ui_spinner(173, 99, 5, COL_BLUE);
        ui_text(183, 92, 10.5f, FONT_BOLD, COL_BLUE, "завантажую файл…");
    } else if (w->retrying) {
        /* a login can take up to 15 s: show it is still going, not stuck */
        ui_text(169, 60, 15, FONT_HEAVY, w->current[0] ? COL_INK : COL_FAINT, w->current[0] ? w->current : "Pretendo");
        ui_spinner(173, 99, 5, COL_BLUE);
        ui_text(183, 92, 10.5f, FONT_BOLD, COL_BLUE, w->current[0] ? "входжу знову, повторю код…" : "вхід у сервіс друзів…");
    } else if (w->current[0]) {
        ui_text(169, 60, 15, FONT_HEAVY, COL_INK, w->current);
        snprintf(buf, sizeof(buf), "%s%s%s", w->current_name, w->current_name[0] ? " · " : "",
                 w->job == JOB_ADD ? "чекаю на сервер…" : "видаляю…");
        ui_spinner(173, 99, 5, COL_BLUE);
        ui_text(183, 92, 10.5f, FONT_BOLD, COL_BLUE, buf);
    } else {
        ui_text(169, 62, 13, FONT_HEAVY, COL_FAINT, "готуюсь…");
    }

    if (w->job == JOB_ADD) {
        draw_counter(195, w->mutual, "взаємних", COL_GREEN_TEXT);
        draw_counter(269, w->requests, "заявок", COL_BLUE);
        draw_counter(343, w->skipped, "пропущено", COL_MUTED);
        /* the friend list is read before the run and after it: count down from that by what got in */
        int left = free_places(app) - (w->mutual + w->requests);
        snprintf(buf, sizeof(buf), "Вільних місць: %d з 100", left > 0 ? left : 0);
        ui_text(158, 176, 9.5f, FONT_BOLD, COL_FAINT, buf);
    } else if (w->job != JOB_UPDATE) {
        draw_counter(220, w->removed, "видалено", COL_INK);
        draw_counter(318, w->errors, "помилки", w->errors ? COL_RED : COL_MUTED);
    }
}

static void draw_progress_bottom(const App *app)
{
    const Worker *w = &app->worker;
    LogLine lines[4];
    char text[96];
    int n = 0;
    for (int i = 0; n < 4 && log_get(i, &lines[n]); i++)
        n++;

    ui_text(14, 9, 12, FONT_HEAVY, COL_INK, "Журнал");
    ui_card(14, 30, 292, 84, 10, COL_CARD, 1);
    for (int i = 0; i < n; i++) {
        float mid = 30 + 2 + i * 20 + 10;
        log_label(&lines[i], text, sizeof(text));
        ui_text(24, mid - 7, 10, FONT_BOLD, COL_INK, text);
        const char *badge = log_badge(lines[i].kind);
        if (badge)
            draw_badge(296, mid, badge, lines[i].kind);
        if (i < n - 1)
            ui_rect(24, mid + 9.5f, 272, 1, COL_LINE);
    }

    Rect r = PROGRESS_FOCUS[0].r;
    bool can_stop = w->job != JOB_UPDATE && !w->stop;
    draw_card_button(r, false);
    const char *label = w->stop ? "Зупиняю…" : w->job == JOB_UPDATE ? "Зачекайте…" : "Зупинити після цього коду";
    u32 ink = can_stop ? COL_INK : COL_FAINT;
    float tw = 13 + 6 + ui_text_width(13, FONT_HEAVY, label);
    float x = r.x + (r.w - tw) / 2;
    if (can_stop)
        ui_icon(ICON_STOP, x + 6.5f, r.y + r.h / 2, 13, ink);
    ui_text(can_stop ? x + 19 : x + 10, r.y + r.h / 2 - 9.5f, 13, FONT_HEAVY, ink, label);

    if (w->job == JOB_ADD) {
        ui_key_hint(14, 212, "B", "Зупинити");
        ui_text_right(306, 213, 9, FONT_BOLD, COL_FAINT, "~1 с на код");
    } else if (can_stop) {
        ui_key_hint(14, 212, "B", "Зупинити");
    }
}

static void update_progress(App *app, const Input *in)
{
    Worker *w = &app->worker;
    int pressed = navigate(app, in, PROGRESS_FOCUS, 1);
    if ((pressed == 0 || (in->down & KEY_B)) && w->job != JOB_UPDATE && !w->stop) {
        w->stop = true;
        log_add(LOG_INFO, 0, "Зупинка після поточного коду…");
    }
}

/* ---------------------------------------------------------------- done */

static const Focus DONE_FOCUS[] = {{{14, 90, 292, 56}, -1, -1, -1, -1}};

typedef struct {
    const char *label;
    int value;
    u32 color;
} Stat;

static void draw_done_top(const App *app)
{
    const Worker *w = &app->worker;
    char sub[96];
    Stat stats[4];
    int n = 0;
    const char *title;

    if (w->job == JOB_ADD) {
        int added = w->mutual + w->requests;
        if (w->no_room)
            snprintf(sub, sizeof(sub), "Додано %d. Не вмістилось %d: список повний.", added, w->no_room);
        else
            snprintf(sub, sizeof(sub), "Додано %d %s із бази.", added, plural(added, "друга", "друзів", "друзів"));
        stats[n++] = (Stat){"Взаємні друзі", w->mutual, COL_GREEN_TEXT};
        stats[n++] = (Stat){"Заявки - чекають, поки вас додадуть", w->requests, COL_BLUE};
        stats[n++] = (Stat){"Пропущено (ваш код, вже у списку)", w->skipped, COL_INK};
        stats[n++] = (Stat){"Помилки", w->errors, w->errors ? COL_RED : COL_INK};
    } else if (w->job == JOB_UPDATE) {
        snprintf(sub, sizeof(sub), "Базу кодів оновлено.");
        stats[n++] = (Stat){"Кодів у базі", app->model.db.count, COL_INK};
        stats[n++] = (Stat){"Нових кодів", w->new_codes, COL_BLUE};
        stats[n++] = (Stat){"Хибних рядків у файлі", app->model.db.bad, app->model.db.bad ? COL_AMBER : COL_INK};
        stats[n++] = (Stat){"Повторів у файлі", app->model.db.dupes, COL_INK};
    } else {
        snprintf(sub, sizeof(sub), "Видалено %d %s.", w->removed, plural(w->removed, "друга", "друзів", "друзів"));
        stats[n++] = (Stat){"Видалено", w->removed, COL_INK};
        stats[n++] = (Stat){"Помилки", w->errors, w->errors ? COL_RED : COL_INK};
        stats[n++] = (Stat){"Лишилось у списку", app->model.friends.count, COL_INK};
    }

    if (w->ok) {
        title = "Готово!";
        ui_circle(43, 37, 21, COL_GREEN);
        ui_icon(ICON_CHECK, 43, 37, 24, COL_WHITE);
    } else {
        title = "Не вийшло";
        ui_circle(43, 37, 21, COL_YELLOW);
        ui_rect(42, 25, 2.4f, 14, COL_INK);
        ui_circle(43.2f, 46, 1.8f, COL_INK);
    }
    ui_text(74, 14, 22, FONT_HEAVY, COL_INK, title);
    ui_text(74, 43, 12.5f, FONT_BOLD, COL_BODY, w->ok ? sub : "Деталі - на нижньому екрані й у журналі.");

    ui_card(22, 72, 356, n * 26 + 4, 10, COL_CARD, 1);
    for (int i = 0; i < n; i++) {
        float y = 74 + i * 26;
        char v[12];
        snprintf(v, sizeof(v), "%d", stats[i].value);
        ui_text(36, y + 5, 11.5f, FONT_BOLD, COL_MUTED, stats[i].label);
        ui_text_right(364, y + 5, 11.5f, FONT_HEAVY, stats[i].color, v);
        if (i < n - 1)
            ui_rect(36, y + 25, 328, 1, COL_LINE);
    }
    if (w->job == JOB_ADD) {
        ui_icon(ICON_TELEGRAM, 28, 203, 12, COL_TELEGRAM);
        ui_text(39, 196, 10, FONT_BOLD, COL_MUTED, "Додайте свій код у базу - t.me/n3ds_ua");
    }
}

static void draw_done_bottom(const App *app)
{
    const Worker *w = &app->worker;
    const char *text;
    if (!w->ok)
        text = w->message[0] ? w->message : "Щось пішло не так. Подробиці - у журналі.";
    else if (w->job == JOB_ADD)
        text = "Заявки вже на сервері Pretendo. Коли людина додасть ваш код, ви станете друзями - консоль побачить це "
               "при наступному виході в інтернет.";
    else if (w->job == JOB_UPDATE)
        text = "Нові коди додаються кнопкою «Додати друзів» на головному екрані.";
    else
        text = "Щоб знову додати друзів із бази, натисніть «Додати друзів».";
    ui_rrect(14, 14, 292, 66, 11, w->ok ? COL_BLUE_BG : COL_AMBER_BG);
    ui_icon(ICON_INFO, 31, 31, 17, w->ok ? COL_BLUE : COL_AMBER);
    ui_text_wrap(46, 20, 252, 11, FONT_BOLD, COL_INK, text);
    draw_primary(DONE_FOCUS[0].r, ICON_CHECK, "До меню", NULL, true, false, COL_BLUE, COL_BLUE_DARK);
    draw_hints(2, (const char *[]){"A", "Y"}, (const char *[]){"До меню", "Журнал"});
}

static void update_done(App *app, const Input *in)
{
    if (in->down & KEY_Y) {
        app->log_back = SCREEN_DONE;
        app->log_scroll = 0;
        go(app, SCREEN_LOG, 0);
        return;
    }
    if (navigate(app, in, DONE_FOCUS, 1) == 0 || (in->down & KEY_B))
        go(app, SCREEN_MAIN, 0);
}

/* ---------------------------------------------------------------- remove: choice */

static const Focus REMOVE_FOCUS[] = {
    {{14, 14, 292, 75}, -1, 1, -1, -1},
    {{14, 98, 292, 75}, 0, -1, -1, -1},
};

static int db_friends(const App *app)
{
    int n = 0;
    for (int i = 0; i < app->model.friends.count; i++)
        if (db_find(&app->model.db, app->model.friends.items[i].code) >= 0)
            n++;
    return n;
}

static void draw_remove_top(const App *app)
{
    char buf[32];
    ui_text(22, 16, 19, FONT_HEAVY, COL_INK, "Кого видалити?");
    ui_text_wrap(22, 46, 356, 12, FONT_BOLD, COL_BODY,
                 "«Тільки коди з бази» прибирає лише тих, чиї коди є в базі, - ваші справжні друзі лишаться. «Усіх "
                 "друзів» повністю очищає список (для тестів).");
    ui_card(22, 172, 356, 50, 10, COL_CARD, 1);
    ui_text(34, 178, 11, FONT_BOLD, COL_MUTED, "У списку зараз");
    snprintf(buf, sizeof(buf), "%d %s", app->model.friends.count,
             plural(app->model.friends.count, "друг", "друзі", "друзів"));
    ui_text_right(366, 178, 11, FONT_HEAVY, COL_INK, buf);
    ui_text(34, 198, 11, FONT_BOLD, COL_MUTED, "З них із бази");
    snprintf(buf, sizeof(buf), "%d", db_friends(app));
    ui_text_right(366, 198, 11, FONT_HEAVY, COL_INK, buf);
}

static void draw_option(Rect r, bool focus, u32 badge_bg, u32 badge_ink, Icon icon, const char *title, const char *sub)
{
    if (focus) {
        ui_rrect(r.x - 2, r.y - 2, r.w + 4, r.h + 4, 14, COL_BLUE);
        ui_rrect(r.x, r.y, r.w, r.h, 12, COL_CARD);
    } else {
        ui_card(r.x, r.y, r.w, r.h, 12, COL_CARD, 1.5f);
    }
    ui_rrect(r.x + 13, r.y + r.h / 2 - 16, 32, 32, 8, badge_bg);
    ui_icon(icon, r.x + 29, r.y + r.h / 2, 18, badge_ink);
    ui_text(r.x + 58, r.y + r.h / 2 - 19, 15, FONT_HEAVY, COL_INK, title);
    ui_text(r.x + 58, r.y + r.h / 2 + 4, 10, FONT_BOLD, COL_MUTED, sub);
}

static void draw_remove_bottom(const App *app)
{
    char a[96], b[96];
    int in_db = db_friends(app), all = app->model.friends.count;
    snprintf(a, sizeof(a), "%d %s · інші друзі лишаться", in_db, plural(in_db, "запис", "записи", "записів"));
    snprintf(b, sizeof(b), "%d %s · список стане порожнім", all, plural(all, "запис", "записи", "записів"));
    draw_option(REMOVE_FOCUS[0].r, focused(app, 0), COL_BLUE_BG, COL_BLUE, ICON_LINES, "Тільки коди з бази", a);
    draw_option(REMOVE_FOCUS[1].r, focused(app, 1), COL_AMBER_BG, COL_RED, ICON_TRASH, "Усіх друзів", b);
    draw_hints(2, (const char *[]){"B", "A"}, (const char *[]){"Назад", "Вибрати"});
    draw_toast(app);
}

static void update_remove(App *app, const Input *in)
{
    int pressed = navigate(app, in, REMOVE_FOCUS, 2);
    if (pressed == 0 && db_friends(app) == 0) {
        toast(app, "У списку немає друзів із бази.");
    } else if (pressed >= 0) {
        app->remove_all = pressed == 1;
        go(app, SCREEN_CONFIRM, 1); /* "Скасувати": the safe button */
    } else if (in->down & KEY_B) {
        go(app, SCREEN_MAIN, MAIN_REMOVE);
    }
}

/* ---------------------------------------------------------------- remove: confirm */

static const Focus CONFIRM_FOCUS[] = {
    {{14, 14, 292, 60}, -1, 1, -1, -1},
    {{14, 83, 292, 48}, 0, -1, -1, -1},
};

static int remove_count(const App *app)
{
    return app->remove_all ? app->model.friends.count : db_friends(app);
}

static void draw_confirm_top(const App *app)
{
    char title[48];
    int n = remove_count(app);
    ui_circle(42, 40, 20, COL_YELLOW);
    ui_rect(40.8f, 28, 2.4f, 14, COL_INK);
    ui_circle(42, 48.5f, 1.8f, COL_INK);
    snprintf(title, sizeof(title), "Видалити %d %s?", n, plural(n, "друга", "друзів", "друзів"));
    ui_text(72, 27, 20, FONT_HEAVY, COL_INK, title);
    const char *text = app->remove_all
                           ? "Список друзів стане порожнім - разом зі справжніми друзями. Повернути їх можна лише "
                             "повторним додаванням: взаємні знову стануть заявками, поки люди не додадуть вас."
                           : "Буде видалено тих, чиї коди є в базі; інші друзі лишаться. Додати їх знову можна "
                             "кнопкою «Додати друзів»: взаємні стануть заявками, поки люди не додадуть вас знову.";
    float h = ui_text_wrap_height(330, 12, FONT_BOLD, text);
    ui_rrect(22, 82, 356, h + 20, 10, COL_AMBER_BG);
    ui_text_wrap(35, 92, 330, 12, FONT_BOLD, COL_AMBER_TEXT, text);
}

static void draw_confirm_bottom(const App *app)
{
    char label[40];
    int n = remove_count(app);
    if (app->remove_all)
        snprintf(label, sizeof(label), "Так, видалити всіх");
    else
        snprintf(label, sizeof(label), "Так, видалити %d", n);
    draw_primary(CONFIRM_FOCUS[0].r, ICON_TRASH, label, NULL, true, focused(app, 0), COL_RED, COL_RED_DARK);
    Rect r = CONFIRM_FOCUS[1].r;
    draw_card_button(r, focused(app, 1));
    ui_text_center(r.x + r.w / 2, r.y + r.h / 2 - 11, 15, FONT_HEAVY, COL_INK, "Скасувати");
    float w = ui_key_hint(14, 212, "B", "Скасувати");
    (void)w;
    float lw = 17 + 4 + ui_text_width(11, FONT_BOLD, "+") + 4 + 17 + 4 + ui_text_width(11, FONT_BOLD, "Видалити");
    float x = BOTTOM_W - 14 - lw;
    x += ui_key_hint(x, 212, "L", "+") + 4;
    ui_key_hint(x, 212, "A", "Видалити");
}

static void update_confirm(App *app, const Input *in)
{
    int pressed = navigate(app, in, CONFIRM_FOCUS, 2);
    bool shortcut = (in->held & KEY_L) && (in->down & KEY_A);
    if (pressed == 0 || shortcut)
        start_job(app, app->remove_all ? JOB_REMOVE_ALL : JOB_REMOVE_DB);
    else if (pressed == 1 || (in->down & KEY_B))
        go(app, SCREEN_MAIN, MAIN_REMOVE);
}

/* ---------------------------------------------------------------- debug (L+R+Y on the main screen) */

static const char *relationship_name(u8 r)
{
    switch (r) {
    case RELATIONSHIP_INCOMPLETE: return "INCOMPLETE";
    case RELATIONSHIP_COMPLETE: return "COMPLETE";
    case RELATIONSHIP_NOT_FOUND: return "NOT_FOUND";
    case RELATIONSHIP_DELETED: return "DELETED";
    case RELATIONSHIP_LOCAL: return "LOCAL";
    }
    return "?";
}

static void draw_debug_top(const App *app)
{
    const FriendList *l = &app->model.friends;
    char buf[80], code[FC_STR_LEN];
    snprintf(buf, sizeof(buf), "Debug: друзі %d (взаємні %d, заявки %d)", l->count, l->mutual, l->requests);
    ui_text(12, 6, 13, FONT_HEAVY, COL_INK, buf);
    for (int i = 0; i < DEBUG_ROWS && app->debug_scroll + i < l->count; i++) {
        const Friend *f = &l->items[app->debug_scroll + i];
        float y = 30 + i * 18.5f;
        fc_format(f->code, code);
        ui_text(12, y, 10, FONT_BOLD, COL_INK, code);
        snprintf(buf, sizeof(buf), "%s (%u)", relationship_name(f->relationship), f->relationship);
        ui_text(100, y, 10, FONT_HEAVY, f->relationship == RELATIONSHIP_COMPLETE ? COL_GREEN_TEXT : COL_BLUE, buf);
        snprintf(buf, sizeof(buf), "attr %lu", f->attributes);
        ui_text(212, y, 10, FONT_BOLD, COL_MUTED, buf);
        ui_text(262, y, 10, FONT_BOLD, COL_BODY, f->name);
    }
}

static void draw_debug_bottom(const App *app)
{
    const Console *c = &app->model.console;
    char buf[80];
    ui_text(14, 10, 13, FONT_HEAVY, COL_INK, "Стан сервісу друзів");
    snprintf(buf, sizeof(buf), "Обліковий запис: %u (%s)", c->account,
             c->account == 2 ? "Pretendo" : c->account == 1 ? "Nintendo" : "?");
    ui_text(14, 36, 11, FONT_BOLD, COL_BODY, buf);
    snprintf(buf, sizeof(buf), "Онлайн: %s · вхід: 0x%08lX", c->online ? "так" : "ні", c->login_result);
    ui_text(14, 54, 11, FONT_BOLD, COL_BODY, buf);
    snprintf(buf, sizeof(buf), "Nimbus: %s · патчі ігор: %s", c->nimbus ? "є" : "немає", c->game_patching ? "так" : "ні");
    ui_text(14, 72, 11, FONT_BOLD, COL_BODY, buf);
    ui_text_wrap(14, 100, 292, 10.5f, FONT_BOLD, COL_MUTED,
                 "INCOMPLETE - заявка, COMPLETE - взаємний друг, LOCAL - заявка, ще не надіслана на сервер.");
    draw_hints(3, (const char *[]){"D-Pad", "X", "B"}, (const char *[]){"Прокрутка", "Оновити", "Назад"});
}

static void update_debug(App *app, const Input *in)
{
    int max = app->model.friends.count - DEBUG_ROWS;
    if (in->repeat & (KEY_DDOWN | KEY_CPAD_DOWN))
        app->debug_scroll++;
    else if (in->repeat & (KEY_DUP | KEY_CPAD_UP))
        app->debug_scroll--;
    if (app->debug_scroll > max)
        app->debug_scroll = max;
    if (app->debug_scroll < 0)
        app->debug_scroll = 0;
    if (in->down & KEY_X)
        refresh_friends(app);
    if (in->down & KEY_B)
        go(app, SCREEN_MAIN, app->focus[SCREEN_MAIN]);
}

/* ---------------------------------------------------------------- app */

static void job_finished(App *app)
{
    Job job = app->worker.job;
    update_plan(app);
    if (job == JOB_LOGIN)
        return;
    if (job == JOB_CHECK) {
        Console *c = &app->model.console;
        if (c->frd_admin && !app->notify)
            friends_watch(&app->notify);
        if (app->startup) {
            app->startup = false;
            Row rows[CHECK_ROWS];
            const Row *first;
            if (worst(rows, check_rows(app, rows), &first) != LEVEL_FAIL)
                go(app, SCREEN_MAIN, MAIN_ADD);
        }
        return;
    }
    go(app, SCREEN_DONE, 0);
}

void app_init(App *app)
{
    memset(app, 0, sizeof(*app));
    store_writable();
    log_init(STORE_LOG);
    log_add(LOG_INFO, 0, "Запуск Friend Auto Add Tool v" APP_VERSION);
    if (!net_init())
        log_add(LOG_ERROR, 0, "Мережа не запустилась: оновлення бази не працюватиме");
    hidSetRepeatParameters(24, 7); /* frames: 0.4 s before repeating, then ~8 a second */
    app->startup = true;
    app->screen = SCREEN_CHECK;
    start_job(app, JOB_CHECK);
}

void app_update(App *app, const Input *in)
{
    if (worker_poll(&app->worker))
        job_finished(app);

    if (app->notify && svcWaitSynchronization(app->notify, 0) == 0 && friends_drain_notifications())
        app->next_refresh = 0;
    u64 now = osGetTime();
    Console *c = &app->model.console;
    if (!app->worker.busy && c->done && now >= app->next_refresh) {
        app->next_refresh = now + REFRESH_MS;
        bool was_online = c->online;
        checks_refresh_network(c);
        /* sleep mode logs the friends module out; it waits for Wi-Fi to come back */
        if (was_online && !c->online)
            app->relogin = true;
        refresh_friends(app);
    }
    if (app->relogin && !app->worker.thread && c->wifi) {
        app->relogin = false;
        start_job(app, JOB_LOGIN);
    }

    if ((in->down & KEY_START) && app->screen != SCREEN_PROGRESS) {
        app->quit = true;
        return;
    }
    switch (app->screen) {
    case SCREEN_CHECK: update_check(app, in); break;
    case SCREEN_MAIN: update_main(app, in); break;
    case SCREEN_LOG: update_log(app, in); break;
    case SCREEN_PROGRESS: update_progress(app, in); break;
    case SCREEN_DONE: update_done(app, in); break;
    case SCREEN_REMOVE: update_remove(app, in); break;
    case SCREEN_CONFIRM: update_confirm(app, in); break;
    case SCREEN_DEBUG: update_debug(app, in); break;
    case SCREEN_COUNT: break;
    }
}

void app_draw_top(const App *app)
{
    switch (app->screen) {
    case SCREEN_CHECK: draw_check_top(app); break;
    case SCREEN_MAIN: draw_main_top(app); break;
    case SCREEN_LOG: draw_log_top(app); break;
    case SCREEN_PROGRESS: draw_progress_top(app); break;
    case SCREEN_DONE: draw_done_top(app); break;
    case SCREEN_REMOVE: draw_remove_top(app); break;
    case SCREEN_CONFIRM: draw_confirm_top(app); break;
    case SCREEN_DEBUG: draw_debug_top(app); break;
    case SCREEN_COUNT: break;
    }
}

void app_draw_bottom(const App *app)
{
    switch (app->screen) {
    case SCREEN_CHECK: draw_check_bottom(app); break;
    case SCREEN_MAIN: draw_main_bottom(app); break;
    case SCREEN_LOG: draw_log_bottom(app); break;
    case SCREEN_PROGRESS: draw_progress_bottom(app); break;
    case SCREEN_DONE: draw_done_bottom(app); break;
    case SCREEN_REMOVE: draw_remove_bottom(app); break;
    case SCREEN_CONFIRM: draw_confirm_bottom(app); break;
    case SCREEN_DEBUG: draw_debug_bottom(app); break;
    case SCREEN_COUNT: break;
    }
}

void app_exit(App *app)
{
    worker_exit(&app->worker);
    if (app->notify)
        svcCloseHandle(app->notify);
    friends_exit();
    net_exit();
}
