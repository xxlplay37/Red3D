#pragma once
#include <stdbool.h>

/* Parseur JSON minimal (DOM), suffisant pour l'API Reddit. */

typedef enum { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ } JType;

typedef struct JVal JVal;
struct JVal {
    JType type;
    char *key;      /* nom du membre si l'élément est dans un objet */
    JVal *child;    /* premier enfant (objet / tableau) */
    JVal *next;     /* frère suivant */
    double num;
    char *str;
    bool b;
};

JVal *json_parse(const char *text);
void json_free(JVal *v);

JVal *json_get(const JVal *obj, const char *key);
JVal *json_idx(const JVal *arr, int i);
const char *json_str(const JVal *obj, const char *key, const char *def);
double json_num(const JVal *obj, const char *key, double def);
bool json_bool(const JVal *obj, const char *key, bool def);
