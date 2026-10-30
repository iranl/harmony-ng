#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdbool.h>

#include "elite/elite_i18n.h"

int main(void) {
    printf("[*] Running Elite i18n tests...\n");

    /* 1. Verify language code mapping */
    assert(strcmp(elite_lang_code(ELITE_LANG_EN), "en") == 0);
    assert(strcmp(elite_lang_code(ELITE_LANG_DE), "de") == 0);
    assert(strcmp(elite_lang_code(ELITE_LANG_FR), "fr") == 0);
    assert(strcmp(elite_lang_code(ELITE_LANG_NL), "nl") == 0);
    assert(strcmp(elite_lang_code(ELITE_LANG_ES), "es") == 0);
    assert(strcmp(elite_lang_code(ELITE_LANG_IT), "it") == 0);

    /* 2. Verify language from code parser */
    assert(elite_lang_from_code("en") == ELITE_LANG_EN);
    assert(elite_lang_from_code("de") == ELITE_LANG_DE);
    assert(elite_lang_from_code("DE") == ELITE_LANG_DE);
    assert(elite_lang_from_code("fr") == ELITE_LANG_FR);
    assert(elite_lang_from_code("nl") == ELITE_LANG_NL);
    assert(elite_lang_from_code("es") == ELITE_LANG_ES);
    assert(elite_lang_from_code("it") == ELITE_LANG_IT);
    assert(elite_lang_from_code("invalid") == ELITE_LANG_EN);
    assert(elite_lang_from_code(NULL) == ELITE_LANG_EN);

    /* 3. Verify cycling through languages */
    g_language = ELITE_LANG_EN;
    elite_lang_cycle();
    assert(g_language == ELITE_LANG_DE);
    elite_lang_cycle();
    assert(g_language == ELITE_LANG_FR);
    elite_lang_cycle();
    assert(g_language == ELITE_LANG_NL);
    elite_lang_cycle();
    assert(g_language == ELITE_LANG_ES);
    elite_lang_cycle();
    assert(g_language == ELITE_LANG_IT);
    elite_lang_cycle();
    assert(g_language == ELITE_LANG_EN);

    /* 4. Verify all strings across all languages are non-empty and strictly ASCII printable (32..126) */
    for (int l = 0; l < ELITE_LANG_COUNT; l++) {
        elite_lang_t lang = (elite_lang_t)l;
        for (int s = 0; s < STR_COUNT; s++) {
            elite_str_id_t id = (elite_str_id_t)s;
            const char *str = tr_lang(id, lang);
            assert(str != NULL);
            assert(strlen(str) > 0);

            for (size_t i = 0; i < strlen(str); i++) {
                unsigned char c = (unsigned char)str[i];
                if (c < 32 || c > 126) {
                    fprintf(stderr, "[-] Non-ASCII character 0x%02X at pos %zu in lang %s (str_id %d): '%s'\n",
                            c, i, elite_lang_code(lang), s, str);
                    assert(false);
                }
            }
        }
    }

    /* 5. Verify basic translations */
    g_language = ELITE_LANG_EN;
    assert(strcmp(tr(STR_ACTIVITIES), "ACTIVITIES") == 0);
    assert(strcmp(tr(STR_LANGUAGE), "Language") == 0);

    g_language = ELITE_LANG_DE;
    assert(strcmp(tr(STR_ACTIVITIES), "AKTIVITAETEN") == 0);
    assert(strcmp(tr(STR_LANGUAGE), "Sprache") == 0);

    g_language = ELITE_LANG_FR;
    assert(strcmp(tr(STR_ACTIVITIES), "ACTIVITES") == 0);
    assert(strcmp(tr(STR_LANGUAGE), "Langue") == 0);

    g_language = ELITE_LANG_NL;
    assert(strcmp(tr(STR_ACTIVITIES), "ACTIVITEITEN") == 0);
    assert(strcmp(tr(STR_LANGUAGE), "Taal") == 0);

    g_language = ELITE_LANG_ES;
    assert(strcmp(tr(STR_ACTIVITIES), "ACTIVIDADES") == 0);
    assert(strcmp(tr(STR_LANGUAGE), "Idioma") == 0);

    g_language = ELITE_LANG_IT;
    assert(strcmp(tr(STR_ACTIVITIES), "ATTIVITA") == 0);
    assert(strcmp(tr(STR_LANGUAGE), "Lingua") == 0);

    printf("[+] All Elite i18n tests passed successfully!\n");
    return 0;
}
