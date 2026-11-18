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

cJSON *config_store_devices_copy(void) {
    size_t len = 0;
    char *txt = read_file_text_alloc("/data/harmony/config/devices.json", 32768, &len);
    if (!txt) return NULL;
    cJSON *obj = cJSON_Parse(txt);
    free(txt);
    return obj;
}

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

    /* 3b. Archives that write outside the unpack directory are refused before the unpack */
    /* tar itself drops '../' when it packs, so Python writes the bad entries */
    system("python3 -c \"import tarfile,io\n"
           "def mk(p, extra):\n"
           "  t = tarfile.open(p, 'w:gz'); t.add('/tmp/test_pkg_src/manifest.json', 'manifest.json'); extra(t); t.close()\n"
           "def dotdot(t):\n"
           "  i = tarfile.TarInfo('../escaped'); i.size = 1; t.addfile(i, io.BytesIO(b'x'))\n"
           "def link(t):\n"
           "  i = tarfile.TarInfo('link'); i.type = tarfile.SYMTYPE; i.linkname = '/etc'; t.addfile(i)\n"
           "mk('/tmp/test_evil_dotdot.tar.gz', dotdot); mk('/tmp/test_evil_link.tar.gz', link)\"");
    rc = plugin_mgr_install_archive("/tmp/test_evil_dotdot.tar.gz", err, sizeof(err));
    assert(rc == -1 && strstr(err, "'..'") != NULL);
    rc = plugin_mgr_install_archive("/tmp/test_evil_link.tar.gz", err, sizeof(err));
    assert(rc == -1 && strstr(err, "link") != NULL);
    assert(access("/tmp/escaped", F_OK) != 0);
    system("rm -f /tmp/test_evil_dotdot.tar.gz /tmp/test_evil_link.tar.gz");
    printf("[+] Archives with '..' or link entries refused.\n");

    /* 3c. URLs that a shell or wget would read as more than a URL are refused, unfetched */
    static const char *const bad_urls[] = {
        "http://x/\"; touch /tmp/pwned; \"", "https://x/$(id).tar.gz", "https://x/`id`", "ftp://x/a.tar.gz",
        "https://x/a b.tar.gz", "-O/etc/passwd"
    };
    for (size_t i = 0; i < sizeof(bad_urls) / sizeof(bad_urls[0]); i++) {
        assert(plugin_mgr_install_from_url(bad_urls[i], NULL, err, sizeof(err)) == -1);
        assert(strstr(err, "Invalid URL") != NULL);
        assert(plugin_mgr_install_dev_raw(bad_urls[i], err, sizeof(err)) == -1);
        assert(plugin_mgr_add_repository(bad_urls[i]) == -1);
    }
    assert(access("/tmp/pwned", F_OK) != 0);
    assert(plugin_mgr_is_safe_id("android-tv") && !plugin_mgr_is_safe_id("../x") && !plugin_mgr_is_safe_id(""));
    printf("[+] Unsafe URLs and plugin ids refused.\n");

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

            /* Verify size calculation */
            cJSON *j_sz = cJSON_GetObjectItemCaseSensitive(item, "size_bytes");
            assert(j_sz != NULL && j_sz->valuedouble > 0);
            cJSON *j_szf = cJSON_GetObjectItemCaseSensitive(item, "size_formatted");
            assert(j_szf != NULL && j_szf->valuestring && j_szf->valuestring[0] != '\0');
            printf("[+] Plugin size telemetry verified: %0.0f bytes (%s)\n", j_sz->valuedouble, j_szf->valuestring);
        }
    }
    cJSON_Delete(installed);
    assert(found == 1);
    printf("[+] Installed plugin discovery & manifest parsing verified.\n");

    /* 5. Storage metrics */
    cJSON *storage = plugin_mgr_get_storage_json();
    assert(storage != NULL);
    cJSON *j_tot = cJSON_GetObjectItemCaseSensitive(storage, "total_bytes");
    cJSON *j_free = cJSON_GetObjectItemCaseSensitive(storage, "free_bytes");
    cJSON *j_tot_fmt = cJSON_GetObjectItemCaseSensitive(storage, "total_formatted");
    cJSON *j_free_fmt = cJSON_GetObjectItemCaseSensitive(storage, "free_formatted");
    assert(j_tot != NULL && j_tot->valuedouble > 0);
    assert(j_free != NULL && j_free->valuedouble > 0);
    assert(j_tot_fmt != NULL && j_tot_fmt->valuestring);
    assert(j_free_fmt != NULL && j_free_fmt->valuestring);
    printf("[+] Storage telemetry verified: %s total, %s free\n", j_tot_fmt->valuestring, j_free_fmt->valuestring);
    cJSON_Delete(storage);

    /* 6. Enable / Disable toggle */
    rc = plugin_mgr_set_enabled("test-roku", 0);
    assert(rc == 0);
    assert(access("/data/harmony/plugins/test-roku/disabled", F_OK) == 0);

    rc = plugin_mgr_set_enabled("test-roku", 1);
    assert(access("/data/harmony/plugins/test-roku/disabled", F_OK) != 0);
    printf("[+] Plugin enable/disable toggling verified.\n");

    /* 6b. A disabled plugin stays disabled when it is installed again */
    plugin_mgr_set_enabled("test-roku", 0);
    rc = plugin_mgr_install_archive("/tmp/test_mock_plug.tar.gz", err, sizeof(err));
    assert(rc == 0);
    assert(access("/data/harmony/plugins/test-roku/disabled", F_OK) == 0);
    plugin_mgr_set_enabled("test-roku", 1);
    assert(access("/data/harmony/plugin-data/test-roku", F_OK) == 0);
    printf("[+] Reinstall keeps the disabled state; data dir created.\n");

    /* 6c. Watchdog: a plugin that does not stay up is restarted with a growing gap, and one
     * stopped by request is left alone (test-roku's run.sh exits at once) */
    #define RESTARTS() ({ int _r = -1; cJSON *_l = plugin_mgr_get_installed_json(); cJSON *_i; \
        cJSON_ArrayForEach(_i, _l) { cJSON *_id = cJSON_GetObjectItemCaseSensitive(_i, "id"); \
            if (strcmp(_id->valuestring, "test-roku") == 0) _r = cJSON_GetObjectItemCaseSensitive(_i, "restarts")->valueint; } \
        cJSON_Delete(_l); _r; })
    unlink("/tmp/plugins/test-roku.stopped");
    plugin_mgr_watchdog_tick();
    assert(RESTARTS() == 1);
    plugin_mgr_watchdog_tick();          /* within the 20 s gap: no new attempt */
    assert(RESTARTS() == 1);
    plugin_mgr_stop("test-roku");
    assert(access("/tmp/plugins/test-roku.stopped", F_OK) == 0);
    printf("[+] Watchdog backoff and stop-by-request verified.\n");

    /* 7. In-use guard test */
    mkdir("/data/harmony/config", 0755);
    FILE *dev_f = fopen("/data/harmony/config/devices.json", "w");
    assert(dev_f != NULL);
    fprintf(dev_f, "{\"devices\":[{\"id\":\"1\",\"name\":\"Living Room TV\",\"type\":\"plugin:test-roku\",\"plugin\":\"test-roku\"}]}\n");
    fclose(dev_f);

    char dev_name[64] = {0};
    int in_use = plugin_mgr_is_in_use_by_device("test-roku", dev_name, sizeof(dev_name));
    assert(in_use == 1);
    assert(strcmp(dev_name, "Living Room TV") == 0);
    printf("[+] In-use check verified: device '%s' uses plugin 'test-roku'\n", dev_name);

    /* Non-existent plugin should not be in use */
    assert(plugin_mgr_is_in_use_by_device("non-existent-plugin", NULL, 0) == 0);

    /* Clear devices so uninstall can proceed */
    dev_f = fopen("/data/harmony/config/devices.json", "w");
    assert(dev_f != NULL);
    fprintf(dev_f, "{\"devices\":[]}\n");
    fclose(dev_f);
    assert(plugin_mgr_is_in_use_by_device("test-roku", NULL, 0) == 0);
    printf("[+] In-use check cleared when device list is empty.\n");

    /* 8. Repositories management */
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

    /* 9. Uninstall plugin */
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

    /* 10. Direct dev download test (using local file/HTTP mock) */
    system("rm -rf /tmp/test_dev_src /tmp/test_dev_dest");
    mkdir("/tmp/test_dev_src", 0755);
    mkdir("/tmp/test_dev_src/sub", 0755);

    FILE *dmf = fopen("/tmp/test_dev_src/manifest.json", "w");
    assert(dmf != NULL);
    fprintf(dmf, "{\n"
                 "  \"id\": \"test-dev-plug\",\n"
                 "  \"name\": \"Dev Test Plugin\",\n"
                 "  \"version\": \"2.0.0\",\n"
                 "  \"files\": [\"manifest.json\", \"run.sh\", \"sub/helper.sh\"]\n"
                 "}\n");
    fclose(dmf);

    FILE *drf = fopen("/tmp/test_dev_src/run.sh", "w");
    assert(drf != NULL);
    fprintf(drf, "#!/bin/sh\necho dev_run\n");
    fclose(drf);

    FILE *dhf = fopen("/tmp/test_dev_src/sub/helper.sh", "w");
    assert(dhf != NULL);
    fprintf(dhf, "#!/bin/sh\necho helper\n");
    fclose(dhf);

    /* Start temporary local mock python HTTP server on localhost port 9988 */
    system("python3 -m http.server 9988 --directory /tmp/test_dev_src >/dev/null 2>&1 & echo $! > /tmp/test_http_pid");
    usleep(300000);

    memset(err, 0, sizeof(err));
    rc = plugin_mgr_install_dev_raw("http://127.0.0.1:9988/manifest.json", err, sizeof(err));
    /* Kill mock python http server */
    system("kill -9 $(cat /tmp/test_http_pid 2>/dev/null) 2>/dev/null; rm -f /tmp/test_http_pid");

    assert(rc == 0);
    assert(access("/data/harmony/plugins/test-dev-plug/manifest.json", R_OK) == 0);
    assert(access("/data/harmony/plugins/test-dev-plug/run.sh", X_OK) == 0);
    assert(access("/data/harmony/plugins/test-dev-plug/sub/helper.sh", X_OK) == 0);
    printf("[+] Direct dev raw GitHub/HTTP download and manifest files verified.\n");

    /* Archive download: a SHA-256 that does not match refuses the install, the right one passes */
    system("rm -rf /tmp/test_sha_srv && mkdir -p /tmp/test_sha_srv && "
           "tar -czf /tmp/test_sha_srv/p.tar.gz -C /tmp/test_pkg_src manifest.json run.sh && "
           "sha256sum /tmp/test_sha_srv/p.tar.gz | cut -c1-64 > /tmp/test_sha_srv/sum");
    char good_sha[65] = {0};
    FILE *sf = fopen("/tmp/test_sha_srv/sum", "r");
    assert(sf && fscanf(sf, "%64s", good_sha) == 1);
    fclose(sf);
    system("python3 -m http.server 9989 --directory /tmp/test_sha_srv >/dev/null 2>&1 & echo $! > /tmp/test_http_pid");
    usleep(300000);
    rc = plugin_mgr_install_from_url("http://127.0.0.1:9989/p.tar.gz",
                                     "0000000000000000000000000000000000000000000000000000000000000000", err, sizeof(err));
    int rc_good = plugin_mgr_install_from_url("http://127.0.0.1:9989/p.tar.gz", good_sha, err, sizeof(err));
    system("kill -9 $(cat /tmp/test_http_pid 2>/dev/null) 2>/dev/null; rm -f /tmp/test_http_pid; rm -rf /tmp/test_sha_srv");
    assert(rc == -1);
    assert(rc_good == 0);
    plugin_mgr_uninstall("test-roku");
    printf("[+] Archive SHA-256 check verified (mismatch refused, match installed).\n");

    /* Clean up dev plugin */
    plugin_mgr_uninstall("test-dev-plug");
    assert(access("/data/harmony/plugins/test-dev-plug", F_OK) != 0);

    /* Clean up temp packaging files */
    system("rm -rf /tmp/test_pkg_src /tmp/test_mock_plug.tar.gz /tmp/test_dev_src");

    printf("\n===========================================================\n");
    printf("  ALL PLUGIN MANAGER CORE UNIT TESTS PASSED!\n");
    printf("===========================================================\n");
    return 0;
}
