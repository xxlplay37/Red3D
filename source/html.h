#pragma once
#include <stddef.h>

/* Petits outils pour extraire des données du HTML de Redlib. */

/* strstr borné à [s, end) */
const char *html_find(const char *s, const char *end, const char *needle);

/* Cherche `marker` dans [s, end) puis copie jusqu'au prochain `stop` (malloc). */
char *html_copy_after(const char *s, const char *end, const char *marker, char stop);

/* Convertit un fragment HTML en texte brut UTF-8 (balises retirées, entités décodées,
 * paragraphes / retours à la ligne / listes conservés). Résultat alloué (malloc). */
char *html_to_text(const char *s, size_t n);

/* "Oct 14 2026, 08:38:14 UTC" -> secondes depuis 1970 (0 si invalide) */
double html_parse_date(const char *s);
