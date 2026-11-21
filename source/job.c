#include "job.h"
#include "net.h"
#include "translate.h"

#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { ST_IDLE, ST_RUNNING, ST_DONE };

static Job s_job;
static volatile int s_state = ST_IDLE;
static volatile bool s_cancel, s_quit;
static LightEvent s_wake;
static Thread s_thread;
static NetConn *s_conn;

static void run(Job *j)
{
    j->rc = -1;
    j->err[0] = '\0';
    switch (j->type) {
    case JOB_FEED:
        j->rc = reddit_fetch_feed(j->sub, j->sort, j->after, j->last_created, &j->feed,
                                  j->err, sizeof(j->err));
        break;
    case JOB_COMMENTS: {
        Post p;
        memset(&p, 0, sizeof(p));
        snprintf(p.id, sizeof(p.id), "%s", j->post_id);
        p.sub = j->post_sub;
        p.author = j->post_author;
        j->rc = reddit_fetch_comments(&p, &j->comments, j->err, sizeof(j->err));
        break;
    }
    case JOB_SEARCH:
        j->rc = reddit_search_subs(j->query, &j->subs, j->err, sizeof(j->err));
        break;
    case JOB_TRANSLATE:
        j->rc = translate_texts(s_conn, j->texts, j->ntexts, j->tl, j->out, &j->progress,
                                j->err, sizeof(j->err));
        break;
    default:
        break;
    }
    if (s_cancel) {
        j->rc = -2; /* annulé */
        snprintf(j->err, sizeof(j->err), "Annulé");
    }
}

static void worker(void *arg)
{
    (void)arg;
    while (!s_quit) {
        LightEvent_Wait(&s_wake);
        if (s_quit) break;
        if (s_state != ST_RUNNING) continue;
        run(&s_job);
        s_state = ST_DONE;
    }
}

void job_init(void)
{
    LightEvent_Init(&s_wake, RESET_ONESHOT);
    s_conn = net_conn_new(&s_cancel);
    reddit_attach(s_conn);
    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    /* pile large : TLS (mbedTLS) + parseur JSON récursif */
    s_thread = threadCreate(worker, NULL, 256 * 1024, prio + 1, -2, false);
}

void job_exit(void)
{
    s_cancel = true;
    s_quit = true;
    LightEvent_Signal(&s_wake);
    if (s_thread) {
        threadJoin(s_thread, U64_MAX);
        threadFree(s_thread);
    }
    job_release();
    net_conn_free(s_conn);
}

bool job_busy(void)
{
    return s_state == ST_RUNNING;
}

JobType job_type(void)
{
    return s_state == ST_IDLE ? JOB_NONE : s_job.type;
}

bool job_start(const Job *in)
{
    if (s_state != ST_IDLE || !s_thread) return false;
    s_job = *in;
    memset(&s_job.feed, 0, sizeof(s_job.feed));
    memset(&s_job.comments, 0, sizeof(s_job.comments));
    memset(&s_job.subs, 0, sizeof(s_job.subs));
    s_cancel = false;
    s_state = ST_RUNNING;
    LightEvent_Signal(&s_wake);
    return true;
}

Job *job_poll(void)
{
    return s_state == ST_DONE ? &s_job : NULL;
}

void job_release(void)
{
    if (s_state == ST_RUNNING) return;
    job_free_inputs(&s_job);
    feed_clear(&s_job.feed);
    comments_clear(&s_job.comments);
    sublist_clear(&s_job.subs);
    s_job.type = JOB_NONE;
    s_state = ST_IDLE;
}

void job_free_inputs(Job *j)
{
    for (int i = 0; i < j->ntexts; i++) {
        if (j->texts) free(j->texts[i]);
        if (j->out) free(j->out[i]);
    }
    free(j->texts);
    free(j->out);
    j->texts = j->out = NULL;
    j->ntexts = 0;
}

Job *job_current(void)
{
    return s_state == ST_IDLE ? NULL : &s_job;
}

void job_cancel(void)
{
    if (s_state == ST_RUNNING) s_cancel = true;
}
