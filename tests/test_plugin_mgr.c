#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <sys/stat.h>

#include "../payload/source/plugin_mgr.h"
#include "../payload/source/cJSON.h"
#include "../payload/source/http_util.h"

int main(void) {
    printf("[*] Running Plugin Manager Core Unit Tests...\n");

    /* 1. Invalid archive path error handling */
    char err[256] = {0};
    int rc = plugin_mgr_install_archive("/tmp/non_existent_archive.tar.gz", err, sizeof(err));
    assert(rc == -1);
    assert(err[0] != '\0');
    printf("[+] Non-existent archive rejection verified: %s\n", err);

    /* 2. Corrupt archive rejection */
    const char *corrupt_file = "/tmp/test_corrupt.tar.gz";
    FILE *cf = fopen(corrupt_file, "w");
    assert(cf != NULL);
    fprintf(cf, "This is garbage data, not a gzip archive\n");
    fclose(cf);

    memset(err, 0, sizeof(err));
    rc = plugin_mgr_install_archive(corrupt_file, err, sizeof(err));
    assert(rc == -1);
    unlink(corrupt_file);
    printf("[+] Corrupt archive rejection verified: %s\n", err);

    /* 3. Valid plugin archive packaging & installation */
    system("rm -rf /tmp/test_pkg_src /tmp/test_mock_plug.tar.gz");
    mkdir("/tmp/test_pkg_src", 0755);

    FILE *mf = fopen("/tmp/test_pkg_src/manifest.json", "w");
    assert(mf != NULL);
    fprintf(mf, "{\n"
                "  \"id\": \"test-roku\",\n"
                "  \"name\": \"Test Roku Controller\",\n"
                "  \"version\": \"1.2.3\",\n"
                "  \"description\": \"Unit test driver\",\n"
                "  \"entrypoint\": \"run.sh\",\n"
                "  \"device_definition\": {\n"
                "    \"device_type\": \"roku\",\n"
                "    \"display_name\": \"Roku Test Player\"\n"
                "  }\n"
                "}\n");
    fclose(mf);

    FILE *rf = fopen("/tmp/test_pkg_src/run.sh", "w");
    assert(rf != NULL);
    fprintf(rf, "#!/bin/sh\nexit 0\n");
    fclose(rf);

    system("tar -czf /tmp/test_mock_plug.tar.gz -C /tmp/test_pkg_src manifest.json run.sh");

    memset(err, 0, sizeof(err));
    rc = plugin_mgr_install_archive("/tmp/test_mock_plug.tar.gz", err, sizeof(err));
    assert(rc == 0);
    printf("[+] Plugin archive installation succeeded.\n");

    /* 4. Verify installed JSON lists test-roku with v1.2.3 */
    cJSON *installed = plugin_mgr_get_installed_json();
    assert(installed != NULL && cJSON_IsArray(installed));
    int found = 0;
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, installed) {
        cJSON *j_id = cJSON_GetObjectItemCaseSensitive(item, "id");
        if (j_id && j_id->valuestring && strcmp(j_id->valuestring, "test-roku") == 0) {
            found = 1;
            cJSON *j_ver = cJSON_GetObjectItemCaseSensitive(item, "version");
            assert(j_ver && strcmp(j_ver->valuestring, "1.2.3") == 0);
            cJSON *j_name = cJSON_GetObjectItemCaseSensitive(item, "name");
            assert(j_name && strstr(j_name->valuestring, "Test Roku") != NULL);
        }
    }
    cJSON_Delete(installed);
    assert(found == 1);
    printf("[+] Installed plugin discovery & manifest parsing verified.\n");

    /* 5. Enable / Disable toggle */
    rc = plugin_mgr_set_enabled("test-roku", 0);
    assert(rc == 0);
    assert(access("/data/harmony/plugins/test-roku/disabled", F_OK) == 0);

    rc = plugin_mgr_set_enabled("test-roku", 1);
    assert(access("/data/harmony/plugins/test-roku/disabled", F_OK) != 0);
    printf("[+] Plugin enable/disable toggling verified.\n");

    /* 6. Repositories management */
    cJSON *repos = plugin_mgr_get_repositories();
    assert(repos != NULL && cJSON_IsArray(repos));
    size_t initial_count = cJSON_GetArraySize(repos);
    cJSON_Delete(repos);

    const char *custom_repo = "https://example.com/custom_repo.json";
    rc = plugin_mgr_add_repository(custom_repo);
    assert(rc == 0);

    repos = plugin_mgr_get_repositories();
    assert(cJSON_GetArraySize(repos) == initial_count + 1);
    cJSON_Delete(repos);

    /* Idempotent add */
    assert(plugin_mgr_add_repository(custom_repo) == 0);

    /* Remove repo */
    rc = plugin_mgr_remove_repository(custom_repo);
    assert(rc == 0);
    repos = plugin_mgr_get_repositories();
    assert(cJSON_GetArraySize(repos) == initial_count);
    cJSON_Delete(repos);
    printf("[+] Repository add/remove/deduplication verified.\n");

    /* 7. Uninstall plugin */
    rc = plugin_mgr_uninstall("test-roku");
    assert(rc == 0);
    assert(access("/data/harmony/plugins/test-roku", F_OK) != 0);

    installed = plugin_mgr_get_installed_json();
    found = 0;
    cJSON_ArrayForEach(item, installed) {
        cJSON *j_id = cJSON_GetObjectItemCaseSensitive(item, "id");
        if (j_id && j_id->valuestring && strcmp(j_id->valuestring, "test-roku") == 0) {
            found = 1;
        }
    }
    cJSON_Delete(installed);
    assert(found == 0);
    printf("[+] Plugin uninstallation & cleanup verified.\n");

    /* Clean up temp packaging files */
    system("rm -rf /tmp/test_pkg_src /tmp/test_mock_plug.tar.gz");

    printf("\n===========================================================\n");
    printf("  ALL PLUGIN MANAGER CORE UNIT TESTS PASSED!\n");
    printf("===========================================================\n");
    return 0;
}
