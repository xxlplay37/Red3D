#pragma once
#include <stdbool.h>
#include "reddit.h"

/*
 * Requêtes Reddit exécutées dans un thread de travail : l'interface reste fluide
 * (animations, défilement, annulation avec B) pendant les chargements.
 */

typedef enum { JOB_NONE, JOB_FEED, JOB_COMMENTS, JOB_SEARCH, JOB_TRANSLATE } JobType;

typedef struct {
    JobType type;
    /* Entrées */
    char sub[256], sort[16], after[32], query[96];
    double last_created;
    bool more;                 /* JOB_FEED : ajout à la liste existante */
    char post_id[16], post_sub[64], post_author[64];
    char tl[8];                /* JOB_TRANSLATE : langue cible */
    char **texts;              /* JOB_TRANSLATE : textes à traduire (appartiennent au job) */
    int ntexts;
    /* Sorties */
    char **out;                /* JOB_TRANSLATE : traductions (appartiennent au job) */
    volatile int progress;     /* JOB_TRANSLATE : out[0..progress) est prêt */
    int rc;
    char err[256];
    Feed feed;
    Comments comments;
    SubList subs;
} Job;

void job_init(void);
void job_exit(void);
bool job_busy(void);
JobType job_type(void);
/* Lance une requête (les sorties de `in` sont ignorées). Faux si une requête est déjà en cours. */
bool job_start(const Job *in);
/* Requête terminée ? Renvoie le Job (l'appelant prend ses sorties puis appelle job_release). */
Job *job_poll(void);
void job_release(void);
void job_cancel(void);
/* Requête en cours ou terminée (lecture de la progression), sinon NULL */
Job *job_current(void);
/* Libère les textes / traductions d'un Job qui n'a pas été lancé */
void job_free_inputs(Job *j);
