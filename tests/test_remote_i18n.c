#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdbool.h>

#include "remote/remote_i18n.h"

int main(void) {
    printf("[*] Running remote i18n tests...\n");

    /* 1. Verify language code mapping */
    assert(strcmp(remote_lang_code(REMOTE_LANG_EN), "en") == 0);
    assert(strcmp(remote_lang_code(REMOTE_LANG_DE), "de") == 0);
    assert(strcmp(remote_lang_code(REMOTE_LANG_FR), "fr") == 0);
    assert(strcmp(remote_lang_code(REMOTE_LANG_NL), "nl") == 0);
    assert(strcmp(remote_lang_code(REMOTE_LANG_ES), "es") == 0);
    assert(strcmp(remote_lang_code(REMOTE_LANG_IT), "it") == 0);

    /* 2. Verify language from code parser */
    assert(remote_lang_from_code("en") == REMOTE_LANG_EN);
    assert(remote_lang_from_code("de") == REMOTE_LANG_DE);
    assert(remote_lang_from_code("DE") == REMOTE_LANG_DE);
    assert(remote_lang_from_code("fr") == REMOTE_LANG_FR);
    assert(remote_lang_from_code("nl") == REMOTE_LANG_NL);
    assert(remote_lang_from_code("es") == REMOTE_LANG_ES);
    assert(remote_lang_from_code("it") == REMOTE_LANG_IT);
    assert(remote_lang_from_code("invalid") == REMOTE_LANG_EN);
    assert(remote_lang_from_code(NULL) == REMOTE_LANG_EN);

    /* 3. Verify cycling through languages */
    g_language = REMOTE_LANG_EN;
    remote_lang_cycle();
    assert(g_language == REMOTE_LANG_DE);
    remote_lang_cycle();
    assert(g_language == REMOTE_LANG_FR);
    remote_lang_cycle();
    assert(g_language == REMOTE_LANG_NL);
    remote_lang_cycle();
    assert(g_language == REMOTE_LANG_ES);
    remote_lang_cycle();
    assert(g_language == REMOTE_LANG_IT);
    remote_lang_cycle();
    assert(g_language == REMOTE_LANG_EN);

    /* 4. Verify all strings across all languages are non-empty and strictly ASCII printable (32..126) */
    for (int l = 0; l < REMOTE_LANG_COUNT; l++) {
        remote_lang_t lang = (remote_lang_t)l;
        for (int s = 0; s < STR_COUNT; s++) {
            remote_str_id_t id = (remote_str_id_t)s;
            const char *str = tr_lang(id, lang);
            assert(str != NULL);
            assert(strlen(str) > 0);

            for (size_t i = 0; i < strlen(str); i++) {
                unsigned char c = (unsigned char)str[i];
                if (c < 32 || c > 126) {
                    fprintf(stderr, "[-] Non-ASCII character 0x%02X at pos %zu in lang %s (str_id %d): '%s'\n",
                            c, i, remote_lang_code(lang), s, str);
                    assert(false);
                }
            }
        }
    }

    /* 5. Verify basic translations */
    g_language = REMOTE_LANG_EN;
    assert(strcmp(tr(STR_ACTIVITIES), "ACTIVITIES") == 0);
    assert(strcmp(tr(STR_LANGUAGE), "Language") == 0);

    g_language = REMOTE_LANG_DE;
    assert(strcmp(tr(STR_ACTIVITIES), "AKTIVITAETEN") == 0);
    assert(strcmp(tr(STR_LANGUAGE), "Sprache") == 0);

    g_language = REMOTE_LANG_FR;
    assert(strcmp(tr(STR_ACTIVITIES), "ACTIVITES") == 0);
    assert(strcmp(tr(STR_LANGUAGE), "Langue") == 0);

    g_language = REMOTE_LANG_NL;
    assert(strcmp(tr(STR_ACTIVITIES), "ACTIVITEITEN") == 0);
    assert(strcmp(tr(STR_LANGUAGE), "Taal") == 0);

    g_language = REMOTE_LANG_ES;
    assert(strcmp(tr(STR_ACTIVITIES), "ACTIVIDADES") == 0);
    assert(strcmp(tr(STR_LANGUAGE), "Idioma") == 0);

    g_language = REMOTE_LANG_IT;
    assert(strcmp(tr(STR_ACTIVITIES), "ATTIVITA") == 0);
    assert(strcmp(tr(STR_LANGUAGE), "Lingua") == 0);

    printf("[+] All remote i18n tests passed successfully!\n");
    return 0;
}
