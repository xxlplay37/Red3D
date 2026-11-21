#pragma once
#include <stdbool.h>
#include <stddef.h>

struct NetConn;

/*
 * Traduit in[0..n) vers la langue tl ("fr", "en"...). Les résultats sont écrits dans
 * out[i] (malloc, NULL si texte vide ou non traduit). *progress avance au fur et à mesure
 * (out[0..progress) est définitif), ce qui permet d'afficher les traductions pendant le travail.
 * Retourne 0 si tout a été traduit.
 */
int translate_texts(struct NetConn *c, char *const *in, int n, const char *tl, char **out,
                    volatile int *progress, char *err, size_t errlen);
