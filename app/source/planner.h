/*
 * Which codes of the base to add, in what order: newest first (the bottom of the file
 * first), so when the 100 places run out a newcomer still meets the people who joined
 * last and are adding codes too. Pure C, built into the host tests too.
 */
#ifndef PLANNER_H
#define PLANNER_H

#include <stdint.h>

#include "db.h"

#define FRIENDS_MAX 100

typedef enum {
    PLAN_ADD,
    PLAN_SELF,     /* this console's own code */
    PLAN_EXISTS,   /* already in the friend list, as a friend or a request */
    PLAN_NO_ROOM,  /* the list would pass 100 */
} PlanReason;

typedef struct {
    int entry; /* index into Db.entries */
    PlanReason reason;
} PlanItem;

typedef struct {
    PlanItem items[DB_MAX];
    int count;          /* every entry of the base, newest first */
    int add, self, exists, no_room;
} Plan;

/* `friends` are the codes in the friend list now; `my_code` 0 when unknown. */
void plan_make(Plan *plan, const Db *db, uint64_t my_code, const uint64_t *friends, int friend_count);

#endif
