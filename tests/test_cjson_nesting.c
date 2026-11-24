/* cJSON must reject deep nesting before it overflows a 64 KB thread stack (hub_rf parses
 * request bodies and maps on such stacks). The limit is set in payload/source/cJSON.h. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"

static cJSON *parse_depth(int depth) {
    char *s = malloc((size_t)depth * 2 + 1);
    memset(s, '[', (size_t)depth);
    memset(s + depth, ']', (size_t)depth);
    s[depth * 2] = '\0';
    cJSON *j = cJSON_Parse(s);
    free(s);
    return j;
}

int main(void) {
    assert(CJSON_NESTING_LIMIT <= 64);
    cJSON *ok = parse_depth(CJSON_NESTING_LIMIT);
    assert(ok != NULL);
    cJSON_Delete(ok);
    assert(parse_depth(CJSON_NESTING_LIMIT + 1) == NULL);
    assert(parse_depth(600) == NULL);
    printf("test_cjson_nesting: OK (limit %d)\n", CJSON_NESTING_LIMIT);
    return 0;
}
