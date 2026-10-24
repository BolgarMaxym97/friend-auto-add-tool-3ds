/*
 * Host tests of the pure parts of the app: friend codes, the code base parser and the
 * planner. `make test`. The codes are real ones from the author's base, checked against
 * Python's hashlib.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../source/db.h"
#include "../source/fc.h"
#include "../source/planner.h"

static int failures;

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #cond); \
            failures++;                                                   \
        }                                                                 \
    } while (0)

static Db db;
static Plan plan;

static void parse(const char *text)
{
    db_parse(&db, text, strlen(text));
}

static void test_fc(void)
{
    uint64_t v = 0;
    char s[FC_STR_LEN];

    CHECK(fc_parse("2204-1225-1663", 14, &v) && v == 220412251663ull);
    CHECK(fc_parse(" 2204 1225 1663\t", 16, &v) && v == 220412251663ull);
    CHECK(fc_parse("220412251663", 12, &v) && v == 220412251663ull);
    CHECK(!fc_parse("2204-1225-166", 13, &v));      /* 11 digits */
    CHECK(!fc_parse("2204-1225-16634", 15, &v));    /* 13 digits */
    CHECK(!fc_parse("2204-1225-166a", 14, &v));     /* letter */
    CHECK(!fc_parse("", 0, &v));

    CHECK(fc_valid(220412251663ull));
    CHECK(fc_valid(130556282027ull));
    CHECK(fc_valid(9624444374ull)); /* 0096-2444-4374: leading zeros */
    CHECK(fc_valid(538623090983ull)); /* the test console */
    CHECK(!fc_valid(220412251664ull));              /* checksum off by one */
    CHECK(!fc_valid(0));
    CHECK(!fc_valid(999999999999ull));

    CHECK(fc_from_pid(123456789) == 451095022869ull);
    CHECK(fc_pid(451095022869ull) == 123456789);
    CHECK(fc_from_pid(fc_pid(220412251663ull)) == 220412251663ull);

    fc_format(9624444374ull, s);
    CHECK(!strcmp(s, "0096-2444-4374"));
    fc_format(220412251663ull, s);
    CHECK(!strcmp(s, "2204-1225-1663"));
}

static void test_db(void)
{
    parse("\xEF\xBB\xBF# comment\r\n"
          "\r\n"
          "1305-5628-2027|liza\r\n"
          "  2204 1225 1663 | vaflik  \n"
          "0096-2444-4374\n"
          "not a code\n"
          "2204-1225-1664|bad checksum\n"
          "1305-5628-2027|dupe\n"
          "3274-3533-0296|Олександрович-Довге\n"
          "3274-3533-029");
    CHECK(db.count == 4);
    CHECK(db.bad == 3);
    CHECK(db.dupes == 1);
    CHECK(db.entries[0].code == 130556282027ull && !strcmp(db.entries[0].name, "liza"));
    CHECK(db.entries[1].code == 220412251663ull && !strcmp(db.entries[1].name, "vaflik"));
    CHECK(db.entries[2].code == 9624444374ull && db.entries[2].name[0] == 0);
    /* cut at 10 characters, never inside a two-byte Cyrillic letter */
    CHECK(!strcmp(db.entries[3].name, "Олександро"));
    CHECK(db_find(&db, 220412251663ull) == 1);
    CHECK(db_find(&db, 1) == -1);

    parse("");
    CHECK(db.count == 0 && db.bad == 0);

    /* more codes than the base holds */
    char *big = malloc((DB_MAX + 5) * 16 + 1);
    size_t n = 0;
    for (uint32_t pid = 1; pid <= DB_MAX + 5; pid++)
        n += (size_t)sprintf(big + n, "%012llu\n", (unsigned long long)fc_from_pid(pid * 7919u));
    db_parse(&db, big, n);
    CHECK(db.count == DB_MAX);
    CHECK(db.overflow == 5);
    free(big);

    /* a line far longer than any buffer: the name is still cut cleanly */
    char longline[600];
    memset(longline, 'x', sizeof(longline));
    memcpy(longline, "1305-5628-2027|", 15);
    db_parse(&db, longline, sizeof(longline));
    CHECK(db.count == 1 && strlen(db.entries[0].name) == DB_NAME_CHARS);
}

static void test_planner(void)
{
    parse("1305-5628-2027|a\n"   /* 0, oldest */
          "2204-1225-1663|me\n"  /* 1 */
          "0096-2444-4374|c\n"   /* 2 */
          "3274-3533-0296|d\n"); /* 3, newest */
    uint64_t friends[FRIENDS_MAX] = {9624444374ull};

    plan_make(&plan, &db, 220412251663ull, friends, 1);
    CHECK(plan.count == 4);
    CHECK(plan.items[0].entry == 3 && plan.items[0].reason == PLAN_ADD);   /* newest first */
    CHECK(plan.items[1].entry == 2 && plan.items[1].reason == PLAN_EXISTS);
    CHECK(plan.items[2].entry == 1 && plan.items[2].reason == PLAN_SELF);
    CHECK(plan.items[3].entry == 0 && plan.items[3].reason == PLAN_ADD);
    CHECK(plan.add == 2 && plan.self == 1 && plan.exists == 1 && plan.no_room == 0);

    /* one place left: only the newest gets it */
    for (int i = 1; i < 99; i++)
        friends[i] = fc_from_pid((uint32_t)i * 104729u);
    plan_make(&plan, &db, 220412251663ull, friends, 99);
    CHECK(plan.add == 1 && plan.no_room == 1);
    CHECK(plan.items[0].reason == PLAN_ADD && plan.items[3].reason == PLAN_NO_ROOM);

    /* full list */
    friends[99] = fc_from_pid(42);
    plan_make(&plan, &db, 0, friends, 100);
    CHECK(plan.add == 0 && plan.no_room == 3 && plan.exists == 1 && plan.self == 0);
}

int main(void)
{
    test_fc();
    test_db();
    test_planner();
    if (failures) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    puts("core_test: all checks passed");
    return 0;
}
