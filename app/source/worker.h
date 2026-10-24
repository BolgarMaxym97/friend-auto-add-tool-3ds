/*
 * Everything slow runs here, on one background thread, so the screens keep drawing: the
 * console check (the friends login can take seconds), the base download, adding and
 * removing friends. While `busy`, the UI thread reads only the progress fields and the
 * log; the model belongs to the worker until the job is collected with worker_poll().
 */
#ifndef WORKER_H
#define WORKER_H

#include <3ds.h>
#include <stdbool.h>
#include <time.h>

#include "checks.h"
#include "db.h"
#include "fc.h"
#include "friends.h"
#include "planner.h"

typedef struct {
    Console console;
    Db db;
    bool db_present;
    time_t db_updated;
    FriendList friends;
    bool friends_ok;
} Model;

typedef enum { JOB_NONE, JOB_CHECK, JOB_UPDATE, JOB_ADD, JOB_REMOVE_DB, JOB_REMOVE_ALL } Job;

typedef struct {
    Thread thread;
    Model *model;
    Job job;
    volatile bool busy;
    volatile bool stop;   /* asked to stop after the code in hand */
    /* progress, written by the worker */
    volatile int done, total;
    volatile int mutual, requests, skipped, errors, removed, no_room, new_codes;
    char current[FC_STR_LEN];
    char current_name[DB_NAME_BYTES];
    /* the outcome, valid once the job is collected */
    bool ok;
    char message[200];
} Worker;

void worker_start(Worker *w, Job job, Model *model);
/* True once, when a started job has finished; joins the thread. */
bool worker_poll(Worker *w);
/* Asks the job to stop and waits for it. */
void worker_exit(Worker *w);

#endif
