#include "worker.h"

#include <stdio.h>
#include <string.h>

#include "log.h"
#include "net.h"
#include "store.h"

#define STACK_SIZE (128 * 1024) /* mbedTLS' handshake wants a deep stack */
#define ADD_PAUSE_NS (1000ull * 1000 * 1000) /* between two codes: the server is shared */
#define ERRORS_IN_ROW 3 /* the server refusing this many codes running: stop, it is not the codes */

static const char *name_or_code(const char *name, u64 code, char buf[FC_STR_LEN])
{
    if (name && name[0])
        return name;
    fc_format(code, buf);
    return buf;
}

static void job_check(Worker *w)
{
    Model *m = w->model;
    checks_run(&m->console, true);
    m->friends_ok = m->console.frd_admin && R_SUCCEEDED(friends_list(&m->friends));
    m->db_present = store_load(&m->db, &m->db_updated);
    const Console *c = &m->console;
    log_add(LOG_INFO, 0, "Перевірка: frd:a %s, акаунт %u, Wi-Fi %s, онлайн %s, вхід 0x%08lX, друзів %d, кодів %d",
            c->frd_admin ? "так" : "ні", c->account, c->wifi ? "так" : "ні", c->online ? "так" : "ні", c->login_result,
            m->friends.count, m->db.count);
    w->ok = true;
}

/* Logs in again once the friends module has dropped the server (sleep mode does that) without
 * the full check, which clears the console state the main screen is drawing from. */
static void job_login(Worker *w)
{
    Console *c = &w->model->console;
    Result res = friends_login(LOGIN_TIMEOUT_MS);
    c->online = R_SUCCEEDED(res);
    c->login_result = res;
    if (c->online)
        log_add(LOG_INFO, 0, "Повторний вхід у сервіс друзів: виконано");
    else
        log_add(LOG_INFO, 0, "Повторний вхід у сервіс друзів не вдався (0x%08lX)", res);
    w->ok = true;
}

static void job_update(Worker *w)
{
    static char text[DB_FILE_MAX];
    static Db fresh;
    Model *m = w->model;
    char err[128];
    size_t len;

    w->total = 1;
    log_add(LOG_INFO, 0, "Завантажую базу з GitHub…");
    if (!net_get(DB_URL, text, sizeof(text), &len, err, sizeof(err))) {
        snprintf(w->message, sizeof(w->message), "Не вдалося завантажити базу: %s.", err);
        log_add(LOG_ERROR, 0, "Базу не оновлено: %s", err);
        return;
    }
    db_parse(&fresh, text, len);
    if (fresh.count == 0) {
        snprintf(w->message, sizeof(w->message), "У завантаженому файлі немає жодного коду - лишаю стару базу.");
        log_add(LOG_ERROR, 0, "Базу не оновлено: у файлі немає кодів");
        return;
    }
    if (!store_save(text, len)) {
        snprintf(w->message, sizeof(w->message), "Не вдалося записати базу на SD-карту.");
        log_add(LOG_ERROR, 0, "Базу не оновлено: помилка запису на SD");
        return;
    }
    for (int i = 0; i < fresh.count; i++)
        if (!m->db_present || db_find(&m->db, fresh.entries[i].code) < 0)
            w->new_codes++;
    memcpy(&m->db, &fresh, sizeof(fresh));
    m->db_present = true;
    m->db_updated = time(NULL);
    w->done = 1;
    w->ok = true;
    log_add(LOG_DONE, 0, "Базу оновлено: %d кодів (+%d нових)%s", fresh.count, w->new_codes,
            fresh.bad ? ", є хибні рядки" : "");
}

/* Why the friends server would not take us, for the user: the run cannot go on without it. */
static void no_login(Worker *w, Result res)
{
    if (res == (Result)0xC960C501)
        snprintf(w->message, sizeof(w->message),
                 "Pretendo відхилив вхід (0x%08lX): оновіть Nimbus до 2.2.0 і запустіть його, тоді спробуйте знову.",
                 res);
    else
        snprintf(w->message, sizeof(w->message),
                 "Сервіс друзів Pretendo зараз недоступний (0x%08lX). Спробуйте пізніше: «Список друзів» у меню HOME "
                 "покаже те саме.",
                 res);
    log_add(LOG_ERROR, 0, "Немає входу в сервіс друзів (0x%08lX) - зупинено", res);
}

/* False when the friends server dropped us and would not take us back: stop the run. */
static bool add_one(Worker *w, const DbEntry *e)
{
    char buf[FC_STR_LEN];
    u8 relationship = 0;
    Result res = friends_add(e->code, e->name, &relationship);
    const char *who = name_or_code(e->name, e->code, buf);
    if (R_FAILED(res)) {
        /* The server slowing down ends in an error (0xC960C425) and the friends module logged out,
         * so every next add fails at once with 0xD8A0C4E4 (New 3DS log, 2026-10-24); right after
         * start-up or sleep it is not on the server yet. Log in again if needed and give the code
         * one more go: friends_add finds it if the first try did reach the server. */
        w->retrying = true;
        svcSleepThread(ADD_PAUSE_NS);
        if (!friends_online()) {
            log_add(LOG_INFO, 0, "%s: 0x%08lX, немає входу - входжу знову", who, res);
            Result login = friends_login(LOGIN_TIMEOUT_MS);
            w->model->console.online = R_SUCCEEDED(login);
            w->model->console.login_result = login;
            if (R_FAILED(login)) {
                w->retrying = false;
                no_login(w, login);
                return false;
            }
        } else {
            log_add(LOG_INFO, 0, "%s: 0x%08lX - пробую ще раз", who, res);
        }
        res = friends_add(e->code, e->name, &relationship);
        w->retrying = false;
    }
    if (R_FAILED(res)) {
        w->errors++;
        log_add(LOG_ERROR, e->code, "%s: 0x%08lX", who, res);
    } else if (relationship == RELATIONSHIP_COMPLETE) {
        w->mutual++;
        log_add(LOG_MUTUAL, e->code, "%s", who);
    } else {
        w->requests++;
        log_add(LOG_REQUEST, e->code, "%s%s", who, relationship == RELATIONSHIP_LOCAL ? " (ще не надіслано)" : "");
    }
    return true;
}

static void job_add(Worker *w)
{
    static Plan plan;
    static u64 codes[FRIENDS_MAX];
    Model *m = w->model;

    if (!friends_online()) {
        log_add(LOG_INFO, 0, "Вхід у сервіс друзів…");
        w->retrying = true;
        Result res = friends_login(LOGIN_TIMEOUT_MS);
        w->retrying = false;
        m->console.online = R_SUCCEEDED(res);
        m->console.login_result = res;
        /* Every add without the login fails with 0xD8A0C4E4 (logs of 2026-10-24): when the server
         * is down (0xC960C4A2, HOME's 001-0502) say so instead of failing code after code. */
        if (R_FAILED(res)) {
            no_login(w, res);
            friends_list(&m->friends);
            return;
        }
    }
    if (R_FAILED(friends_list(&m->friends))) {
        snprintf(w->message, sizeof(w->message), "Не вдалося прочитати список друзів.");
        log_add(LOG_ERROR, 0, "Не вдалося прочитати список друзів");
        return;
    }
    for (int i = 0; i < m->friends.count; i++)
        codes[i] = m->friends.items[i].code;
    plan_make(&plan, &m->db, m->console.my_code, codes, m->friends.count);
    w->total = plan.add;
    w->no_room = plan.no_room;
    log_add(LOG_INFO, 0, "Додаю %d з %d кодів бази", plan.add, m->db.count);

    char buf[FC_STR_LEN];
    int failed_in_row = 0;
    for (int i = 0; i < plan.count; i++) {
        const PlanItem *item = &plan.items[i];
        const DbEntry *e = &m->db.entries[item->entry];
        if (item->reason == PLAN_SELF) {
            w->skipped++;
            log_add(LOG_SKIP, e->code, "це ваш код");
            continue;
        }
        if (item->reason == PLAN_EXISTS) {
            w->skipped++;
            log_add(LOG_SKIP, e->code, "%s: вже у списку", name_or_code(e->name, e->code, buf));
            continue;
        }
        if (item->reason != PLAN_ADD)
            continue;
        if (w->stop) {
            log_add(LOG_INFO, 0, "Зупинено на %d з %d", w->done, w->total);
            break;
        }
        fc_format(e->code, w->current);
        snprintf(w->current_name, sizeof(w->current_name), "%s", e->name);
        int errors = w->errors;
        if (!add_one(w, e))
            break;
        w->done++;
        failed_in_row = w->errors > errors ? failed_in_row + 1 : 0;
        if (failed_in_row >= ERRORS_IN_ROW) {
            snprintf(w->message, sizeof(w->message),
                     "Сервер %d рази поспіль відмовив. Перевірте Wi-Fi і Nimbus (Pretendo), тоді спробуйте знову.",
                     ERRORS_IN_ROW);
            log_add(LOG_ERROR, 0, "Зупинено: %d помилки поспіль", ERRORS_IN_ROW);
            break;
        }
        if (w->done < w->total && !w->stop)
            svcSleepThread(ADD_PAUSE_NS);
    }
    w->current[0] = 0;
    if (plan.no_room)
        log_add(LOG_INFO, 0, "Не вмістилось %d кодів: у списку 100 друзів", plan.no_room);
    friends_list(&m->friends);
    w->ok = !w->message[0];
    log_add(LOG_DONE, 0, "Готово. Додано %d: взаємних %d, заявок %d. Пропущено %d.", w->mutual + w->requests, w->mutual,
            w->requests, w->skipped);
}

static void job_remove(Worker *w, bool all)
{
    static FriendList list;
    Model *m = w->model;
    char buf[FC_STR_LEN];

    if (R_FAILED(friends_list(&list))) {
        snprintf(w->message, sizeof(w->message), "Не вдалося прочитати список друзів.");
        log_add(LOG_ERROR, 0, "Не вдалося прочитати список друзів");
        return;
    }
    for (int i = 0; i < list.count; i++)
        if (all || db_find(&m->db, list.items[i].code) >= 0)
            w->total++;
    log_add(LOG_INFO, 0, all ? "Видаляю всіх друзів: %d" : "Видаляю друзів із бази: %d", w->total);

    for (int i = 0; i < list.count && !w->stop; i++) {
        const Friend *f = &list.items[i];
        if (!all && db_find(&m->db, f->code) < 0)
            continue;
        fc_format(f->code, w->current);
        snprintf(w->current_name, sizeof(w->current_name), "%s", f->name);
        Result res = friends_remove(&f->key);
        if (R_SUCCEEDED(res)) {
            w->removed++;
            log_add(LOG_REMOVED, f->code, "%s", name_or_code(f->name, f->code, buf));
        } else {
            w->errors++;
            log_add(LOG_ERROR, f->code, "не видалено: 0x%08lX", res);
        }
        w->done++;
    }
    w->current[0] = 0;
    friends_save();
    friends_list(&m->friends);
    w->ok = true;
    log_add(LOG_DONE, 0, "Видалено %d друзів%s", w->removed, w->errors ? ", є помилки" : "");
}

static void run(void *arg)
{
    Worker *w = arg;
    switch (w->job) {
    case JOB_CHECK: job_check(w); break;
    case JOB_LOGIN: job_login(w); break;
    case JOB_UPDATE: job_update(w); break;
    case JOB_ADD: job_add(w); break;
    case JOB_REMOVE_DB: job_remove(w, false); break;
    case JOB_REMOVE_ALL: job_remove(w, true); break;
    case JOB_NONE: break;
    }
    w->busy = false;
}

void worker_start(Worker *w, Job job, Model *model)
{
    if (w->thread)
        return;
    memset(w, 0, sizeof(*w));
    w->model = model;
    w->job = job;
    w->busy = true;

    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    /* one step below the UI, on the same core: it runs whenever the UI waits for VBlank */
    w->thread = threadCreate(run, w, STACK_SIZE, prio + 1, -2, false);
    if (!w->thread) {
        w->busy = false;
        snprintf(w->message, sizeof(w->message), "Не вдалося запустити фоновий потік.");
    }
}

bool worker_poll(Worker *w)
{
    if (w->busy || !w->thread)
        return false;
    threadJoin(w->thread, U64_MAX);
    threadFree(w->thread);
    w->thread = NULL;
    return true;
}

void worker_exit(Worker *w)
{
    if (!w->thread)
        return;
    w->stop = true;
    threadJoin(w->thread, U64_MAX);
    threadFree(w->thread);
    w->thread = NULL;
}
