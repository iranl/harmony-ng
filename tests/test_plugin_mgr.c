#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include "../payload/source/plugin_mgr.h"
#include "../payload/source/cJSON.h"
#include "../payload/source/http_util.h"

/* Plugin manager on the host: install checks, the fork/exec lifecycle, binding of devices.
 * Built with -DHARMONY_PLUGIN_ROOT under the build folder (tests/suite.d/27-plugins.sh). */

#define ROOT HARMONY_PLUGIN_ROOT
#define SRC  "/tmp/test_pkg_src"

cJSON *config_store_devices_copy(void) {
    size_t len = 0;
    char *txt = read_file_text_alloc(ROOT "/config/devices.json", 32768, &len);
    if (!txt) return NULL;
    cJSON *obj = cJSON_Parse(txt);
    free(txt);
    return obj;
}

static void write_file(const char *path, const char *text, int exec) {
    FILE *f = fopen(path, "w");
    assert(f != NULL);
    fputs(text, f);
    fclose(f);
    if (exec) chmod(path, 0755);
}

static void sh(const char *cmd) {
    int rc = system(cmd);
    (void)rc;
}

/* A plugin source folder: manifest with the given extra JSON, bin/drv running body */
static void make_src(const char *id, const char *version, const char *body) {
    sh("rm -rf " SRC " && mkdir -p " SRC "/bin");
    char m[1024];
    snprintf(m, sizeof(m),
             "{\"id\": \"%s\", \"name\": \"Test %s\", \"version\": \"%s\", \"entrypoint\": \"bin/drv\","
             " \"device_definition\": {\"device_type\": \"roku\", \"display_name\": \"Television\","
             " \"config_fields\": [{\"key\": \"host\", \"type\": \"string\"}]}}\n", id, id, version);
    write_file(SRC "/manifest.json", m, 0);
    char drv[512];
    snprintf(drv, sizeof(drv), "#!/bin/sh\n%s\n", body);
    write_file(SRC "/bin/drv", drv, 1);
}

static void pack(const char *out) {
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "tar -czf %s -C " SRC " manifest.json bin", out);
    sh(cmd);
}

static int installed_field(const char *id, const char *field) {
    int v = -1;
    cJSON *l = plugin_mgr_get_installed_json(), *i;
    cJSON_ArrayForEach(i, l) {
        cJSON *j = cJSON_GetObjectItemCaseSensitive(i, "id");
        if (strcmp(j->valuestring, id) != 0) continue;
        cJSON *f = cJSON_GetObjectItemCaseSensitive(i, field);
        v = f ? (cJSON_IsBool(f) ? cJSON_IsTrue(f) : f->valueint) : -2;
    }
    cJSON_Delete(l);
    return v;
}

/* gone, or a zombie of another parent */
static int pid_gone(pid_t pid) {
    if (kill(pid, 0) != 0) return 1;
    char path[64], st[256];
    snprintf(path, sizeof(path), "/proc/%d/stat", (int)pid);
    FILE *f = fopen(path, "r");
    if (!f) return 1;
    int ok = fgets(st, sizeof(st), f) != NULL;
    fclose(f);
    const char *p = ok ? strrchr(st, ')') : NULL;
    return p && p[1] == ' ' && p[2] == 'Z';
}

static pid_t read_pid(const char *path) {
    for (int i = 0; i < 50; i++) {
        FILE *f = fopen(path, "r");
        long pid = 0;
        if (f) {
            if (fscanf(f, "%ld", &pid) != 1) pid = 0;
            fclose(f);
        }
        if (pid > 1) return (pid_t)pid;
        usleep(20000);
    }
    return 0;
}

static char *file_sha(const char *path, char out[65]) {
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "sha256sum %s | cut -c1-64 > /tmp/test_sum", path);
    sh(cmd);
    FILE *f = fopen("/tmp/test_sum", "r");
    assert(f && fscanf(f, "%64s", out) == 1);
    fclose(f);
    unlink("/tmp/test_sum");
    return out;
}

int main(void) {
    char err[256] = {0};
    int rc;
    printf("[*] Running Plugin Manager Core Unit Tests...\n");
    sh("rm -rf " ROOT " && mkdir -p " ROOT "/config");
    /* no repository: the catalog is empty and no test goes to the network */
    write_file(ROOT "/config/repositories.json", "[]\n", 0);
    unlink("/tmp/plugins/marketplace_cache.json");

    /* 1. Unreadable and corrupt archives */
    assert(plugin_mgr_install_archive("/tmp/non_existent_archive.tar.gz", NULL, err, sizeof(err)) == -1 && err[0]);
    write_file("/tmp/test_corrupt.tar.gz", "This is garbage data, not a gzip archive\n", 0);
    assert(plugin_mgr_install_archive("/tmp/test_corrupt.tar.gz", NULL, err, sizeof(err)) == -1);
    unlink("/tmp/test_corrupt.tar.gz");
    printf("[+] Unreadable and corrupt archives refused.\n");

    /* 2. Manifest validation runs in the firmware: each bad manifest is refused */
    static const char *const bad_manifests[][2] = {
        { "{\"id\": \"../x\", \"version\": \"1\", \"entrypoint\": \"bin/drv\"}", "plugin id" },
        { "{\"id\": \"ok\", \"entrypoint\": \"bin/drv\"}", "version" },
        { "{\"id\": \"ok\", \"version\": \"1\"}", "entrypoint" },
        { "{\"id\": \"ok\", \"version\": \"1\", \"entrypoint\": \"/bin/sh\"}", "entrypoint" },
        { "{\"id\": \"ok\", \"version\": \"1\", \"entrypoint\": \"bin/missing\"}", "not in the plugin" },
        { "{\"id\": \"ok\", \"version\": \"1\", \"entrypoint\": \"bin/drv\", \"device_definition\": {\"config_fields\": 5}}", "array" },
        { "{\"id\": \"ok\", \"version\": \"1\", \"entrypoint\": \"bin/drv\", \"device_definition\": {\"config_fields\": [null]}}", "key" },
    };
    for (size_t i = 0; i < sizeof(bad_manifests) / sizeof(bad_manifests[0]); i++) {
        make_src("ok", "1", "exec sleep 300");
        write_file(SRC "/manifest.json", bad_manifests[i][0], 0);
        pack("/tmp/test_bad.tar.gz");
        rc = plugin_mgr_install_archive("/tmp/test_bad.tar.gz", NULL, err, sizeof(err));
        if (rc != -1 || !strstr(err, bad_manifests[i][1])) {
            fprintf(stderr, "manifest %zu: rc=%d err=%s\n", i, rc, err);
            assert(0);
        }
    }
    assert(access(ROOT "/plugins/ok", F_OK) != 0);
    unlink("/tmp/test_bad.tar.gz");
    printf("[+] Bad manifests refused (id, version, entrypoint, config_fields).\n");

    /* 3. Archives that write outside the unpack directory, or link, are refused before the
     * unpack. Python writes the entries: tar drops '../' when it packs. A hard link is
     * listed as a plain file by BusyBox tar ("-rw-r--r-- ... name -> target"). */
    make_src("evil", "1", "exec sleep 300");
    sh("python3 -c \"import tarfile,io\n"
       "def mk(p, extra):\n"
       "  t = tarfile.open(p, 'w:gz'); t.add('" SRC "/manifest.json', 'manifest.json'); t.add('" SRC "/bin/drv', 'bin/drv'); extra(t); t.close()\n"
       "def dotdot(t):\n"
       "  i = tarfile.TarInfo('../escaped'); i.size = 1; t.addfile(i, io.BytesIO(b'x'))\n"
       "def link(t):\n"
       "  i = tarfile.TarInfo('link'); i.type = tarfile.SYMTYPE; i.linkname = '/etc'; t.addfile(i)\n"
       "def hard(t):\n"
       "  i = tarfile.TarInfo('outside_link'); i.type = tarfile.LNKTYPE; i.linkname = '../secret'; t.addfile(i)\n"
       "mk('/tmp/test_evil_dotdot.tar.gz', dotdot); mk('/tmp/test_evil_link.tar.gz', link); mk('/tmp/test_evil_hard.tar.gz', hard)\"");
    assert(plugin_mgr_install_archive("/tmp/test_evil_dotdot.tar.gz", NULL, err, sizeof(err)) == -1 && strstr(err, "'..'"));
    assert(plugin_mgr_install_archive("/tmp/test_evil_link.tar.gz", NULL, err, sizeof(err)) == -1 && strstr(err, "link"));
    assert(plugin_mgr_install_archive("/tmp/test_evil_hard.tar.gz", NULL, err, sizeof(err)) == -1 && strstr(err, "link"));
    assert(access("/tmp/escaped", F_OK) != 0);
    assert(access(ROOT "/plugins/evil", F_OK) != 0);
    sh("rm -f /tmp/test_evil_dotdot.tar.gz /tmp/test_evil_link.tar.gz /tmp/test_evil_hard.tar.gz");
    printf("[+] Archives with '..', symlink or hard-link entries refused.\n");

    /* 4. URLs a shell or wget would read as more than a URL are refused, unfetched */
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
    printf("[+] Unsafe URLs refused.\n");

    /* 5. A URL install needs a SHA-256: none given and no catalog entry is refused unfetched */
    assert(plugin_mgr_install_from_url("http://127.0.0.1:9/p.tar.gz", NULL, err, sizeof(err)) == -1);
    assert(strstr(err, "No SHA-256") != NULL);
    printf("[+] URL install without a SHA-256 refused.\n");

    /* 6. Device binding: only the plugin field or a plugin:<id> type, with a safe id */
    {
        char id[64];
        cJSON *d = cJSON_Parse("{\"plugin\": \"roku-ecp\", \"type\": \"Roku\"}");
        assert(plugin_mgr_device_plugin_id(d, id, sizeof(id)) == 1 && strcmp(id, "roku-ecp") == 0);
        cJSON_Delete(d);
        d = cJSON_Parse("{\"type\": \"plugin:android-tv\"}");
        assert(plugin_mgr_device_plugin_id(d, id, sizeof(id)) == 1 && strcmp(id, "android-tv") == 0);
        cJSON_Delete(d);
        d = cJSON_Parse("{\"type\": \"Television\"}"); /* free text, even if a plugin is named so */
        assert(plugin_mgr_device_plugin_id(d, id, sizeof(id)) == 0 && id[0] == '\0');
        cJSON_Delete(d);
        d = cJSON_Parse("{\"plugin\": \"../../tmp/evil\"}");
        assert(plugin_mgr_device_plugin_id(d, id, sizeof(id)) == 0);
        cJSON_Delete(d);
        assert(plugin_mgr_is_safe_id("android-tv") && !plugin_mgr_is_safe_id("../x") && !plugin_mgr_is_safe_id(""));
    }
    printf("[+] Device binding by plugin field / plugin:<id> type only, safe ids only.\n");

    /* 7. Install and start: the Hub forks and execs the entrypoint, which stays in the
     * foreground with the socket as argv[1] and the HARMONY_* environment. Its own child
     * (a helper) is in the same process group. */
    make_src("test-roku", "1.2.3",
             "echo \"$1|$HARMONY_PLUGIN_ID|$HARMONY_SOCKET|$HARMONY_PLUGIN_DATA\" > /tmp/test_plug_env\n"
             "sleep 300 &\necho $! > /tmp/test_plug_child\nexec sleep 300");
    pack("/tmp/test_mock_plug.tar.gz");
    unlink("/tmp/test_plug_env");
    unlink("/tmp/test_plug_child");
    rc = plugin_mgr_install_archive("/tmp/test_mock_plug.tar.gz", NULL, err, sizeof(err));
    if (rc != 0) fprintf(stderr, "install: %s\n", err);
    assert(rc == 0);
    assert(plugin_mgr_is_running("test-roku"));
    pid_t child = read_pid("/tmp/test_plug_child");
    assert(child > 1);
    int pid = installed_field("test-roku", "pid");
    assert(pid > 1);
    {
        char env[512] = {0};
        FILE *f = fopen("/tmp/test_plug_env", "r");
        assert(f && fgets(env, sizeof(env), f));
        fclose(f);
        assert(strstr(env, "/tmp/plugins/test-roku.sock|test-roku|/tmp/plugins/test-roku.sock|" ROOT "/plugin-data/test-roku"));
    }
    assert(access(ROOT "/plugins/test-roku/.source.json", F_OK) != 0); /* local archive: no source */
    printf("[+] Install starts the entrypoint (pid %d, argv and environment checked).\n", pid);

    /* 8. Listing and storage */
    assert(installed_field("test-roku", "size_bytes") > 0);
    cJSON *storage = plugin_mgr_get_storage_json();
    assert(storage && cJSON_GetObjectItemCaseSensitive(storage, "total_bytes")->valuedouble > 0);
    cJSON_Delete(storage);

    /* 9. Stop: SIGTERM to the process group, the helper child goes too */
    assert(plugin_mgr_stop("test-roku") == 0);
    assert(!plugin_mgr_is_running("test-roku"));
    assert(pid_gone((pid_t)pid));
    for (int i = 0; i < 50 && !pid_gone(child); i++) usleep(20000);
    assert(pid_gone(child));
    assert(access("/tmp/plugins/test-roku.stopped", F_OK) == 0);
    printf("[+] Stop ends the plugin and its process group.\n");

    /* 10. A stale pid of another process no longer decides anything: the Hub knows its child */
    write_file("/tmp/plugins/test-roku.pid", "1\n", 0);
    assert(!plugin_mgr_is_running("test-roku"));
    unlink("/tmp/plugins/test-roku.pid");

    /* 11. Enable / disable; a reinstall keeps a disabled plugin disabled and stopped */
    assert(plugin_mgr_set_enabled("test-roku", 0) == 0);
    assert(access(ROOT "/plugins/test-roku/disabled", F_OK) == 0);
    assert(plugin_mgr_install_archive("/tmp/test_mock_plug.tar.gz", NULL, err, sizeof(err)) == 0);
    assert(access(ROOT "/plugins/test-roku/disabled", F_OK) == 0);
    assert(!plugin_mgr_is_running("test-roku"));
    assert(plugin_mgr_set_enabled("test-roku", 1) == 0);
    assert(plugin_mgr_is_running("test-roku"));
    assert(access(ROOT "/plugin-data/test-roku", F_OK) == 0);
    printf("[+] Enable/disable, reinstall keeps the disabled state.\n");

    /* 12. An install that cannot be written keeps the installed version, running.
     * A read-only plugins folder stands in for a full flash (root ignores the mode). */
    if (geteuid() != 0) {
        int pid_before = installed_field("test-roku", "pid");
        make_src("test-roku", "9.9.9", "exec sleep 300");
        pack("/tmp/test_v2.tar.gz");
        chmod(ROOT "/plugins", 0555);
        rc = plugin_mgr_install_archive("/tmp/test_v2.tar.gz", NULL, err, sizeof(err));
        chmod(ROOT "/plugins", 0755);
        assert(rc == -1 && strstr(err, "unchanged"));
        cJSON *l = plugin_mgr_get_installed_json(), *i;
        int v1 = 0;
        cJSON_ArrayForEach(i, l) {
            if (strcmp(cJSON_GetObjectItemCaseSensitive(i, "id")->valuestring, "test-roku") == 0)
                v1 = strcmp(cJSON_GetObjectItemCaseSensitive(i, "version")->valuestring, "1.2.3") == 0;
        }
        cJSON_Delete(l);
        assert(v1);
        assert(installed_field("test-roku", "pid") == pid_before);
        unlink("/tmp/test_v2.tar.gz");
        printf("[+] A failed write keeps v1 installed and running.\n");
    } else {
        printf("[~] Failed-write check skipped (running as root).\n");
    }

    /* 13. Archive SHA-256: a wrong one refuses, the right one installs */
    {
        char sha[65];
        file_sha("/tmp/test_mock_plug.tar.gz", sha);
        assert(plugin_mgr_install_archive("/tmp/test_mock_plug.tar.gz",
               "0000000000000000000000000000000000000000000000000000000000000000", err, sizeof(err)) == -1);
        assert(strstr(err, "SHA-256"));
        assert(plugin_mgr_install_archive("/tmp/test_mock_plug.tar.gz", sha, err, sizeof(err)) == 0);
    }
    printf("[+] Archive SHA-256 check (mismatch refused, match installed).\n");

    /* 14. Crash: the exit is reaped, the watchdog restarts with a growing gap, and a plugin
     * stopped by request is left alone */
    make_src("crashy", "1", "exit 3");
    pack("/tmp/test_crashy.tar.gz");
    assert(plugin_mgr_install_archive("/tmp/test_crashy.tar.gz", NULL, err, sizeof(err)) == 0);
    for (int i = 0; i < 100 && plugin_mgr_is_running("crashy"); i++) usleep(20000);
    assert(!plugin_mgr_is_running("crashy"));
    plugin_mgr_watchdog_tick();
    assert(installed_field("crashy", "restarts") == 1);
    for (int i = 0; i < 100 && plugin_mgr_is_running("crashy"); i++) usleep(20000);
    plugin_mgr_watchdog_tick(); /* within the 20 s gap: no new attempt */
    assert(installed_field("crashy", "restarts") == 1);
    plugin_mgr_stop("crashy");
    plugin_mgr_watchdog_tick();
    assert(installed_field("crashy", "restarts") == 1);
    assert(plugin_mgr_uninstall("crashy") == 0);
    unlink("/tmp/test_crashy.tar.gz");
    printf("[+] Crash reaped, watchdog backoff, stop-by-request respected.\n");

    /* 15. A plugin left by a killed hub_daemon (not our child) is killed at init */
    plugin_mgr_stop("test-roku");
    pid_t orphan = fork();
    if (orphan == 0) {
        if (chdir(ROOT "/plugins/test-roku") != 0) _exit(1);
        execl("/bin/sleep", "sleep", "300", (char *)NULL);
        _exit(1);
    }
    usleep(100000);
    plugin_mgr_init();
    int st;
    for (int i = 0; i < 50 && waitpid(orphan, &st, WNOHANG) == 0; i++) usleep(20000);
    assert(kill(orphan, 0) != 0);
    assert(plugin_mgr_is_running("test-roku")); /* enabled: started again by init */
    printf("[+] Orphan plugin process killed at init, plugin started fresh.\n");

    /* 16. In-use guard */
    write_file(ROOT "/config/devices.json",
               "{\"devices\":[{\"id\":\"1\",\"name\":\"Living Room TV\",\"type\":\"plugin:test-roku\",\"plugin\":\"test-roku\"},"
               "{\"id\":\"2\",\"name\":\"IR TV\",\"type\":\"Television\"}]}\n", 0);
    char dev_name[64] = {0};
    assert(plugin_mgr_is_in_use_by_device("test-roku", dev_name, sizeof(dev_name)) == 1);
    assert(strcmp(dev_name, "Living Room TV") == 0);
    assert(plugin_mgr_is_in_use_by_device("non-existent-plugin", NULL, 0) == 0);
    write_file(ROOT "/config/devices.json", "{\"devices\":[]}\n", 0);
    assert(plugin_mgr_is_in_use_by_device("test-roku", NULL, 0) == 0);
    printf("[+] In-use check.\n");

    /* 17. Repositories */
    const char *custom_repo = "https://example.com/custom_repo.json";
    assert(plugin_mgr_add_repository(custom_repo) == 0);
    assert(plugin_mgr_add_repository(custom_repo) == 0);
    cJSON *repos = plugin_mgr_get_repositories();
    assert(cJSON_GetArraySize(repos) == 1);
    cJSON_Delete(repos);
    assert(plugin_mgr_remove_repository(custom_repo) == 0);
    repos = plugin_mgr_get_repositories();
    assert(cJSON_GetArraySize(repos) == 0);
    cJSON_Delete(repos);
    printf("[+] Repository add/remove/deduplication.\n");

    /* 18. Uninstall: an unknown id is an error; the plugin's data and log go with it */
    assert(plugin_mgr_uninstall("not-installed") == PLUGIN_ERR_NOT_FOUND);
    write_file(ROOT "/plugin-data/test-roku/key.der", "k", 0);
    assert(plugin_mgr_uninstall("test-roku") == 0);
    assert(access(ROOT "/plugins/test-roku", F_OK) != 0);
    assert(access(ROOT "/plugin-data/test-roku", F_OK) != 0);
    assert(access("/tmp/plugins/test-roku.log", F_OK) != 0);
    assert(installed_field("test-roku", "pid") == -1);
    printf("[+] Uninstall removes plugin, data and log; unknown id refused.\n");

    /* 19. Dev install from raw files over HTTP; pull installs it again from the same URL */
    sh("rm -rf /tmp/test_dev_src && mkdir -p /tmp/test_dev_src/bin /tmp/test_dev_src/sub");
    write_file("/tmp/test_dev_src/manifest.json",
               "{\"id\": \"test-dev-plug\", \"name\": \"Dev Test Plugin\", \"version\": \"2.0.0\","
               " \"entrypoint\": \"bin/drv\", \"files\": [\"manifest.json\", \"bin/drv\", \"sub/helper.sh\"]}\n", 0);
    write_file("/tmp/test_dev_src/bin/drv", "#!/bin/sh\nexec sleep 300\n", 1);
    write_file("/tmp/test_dev_src/sub/helper.sh", "#!/bin/sh\necho helper\n", 1);
    sh("python3 -m http.server 9988 --directory /tmp/test_dev_src >/dev/null 2>&1 & echo $! > /tmp/test_http_pid");
    usleep(500000);
    rc = plugin_mgr_install_dev_raw("http://127.0.0.1:9988/manifest.json", err, sizeof(err));
    int rc_pull = plugin_mgr_pull("test-dev-plug", err, sizeof(err));
    sh("kill -9 $(cat /tmp/test_http_pid 2>/dev/null) 2>/dev/null; rm -f /tmp/test_http_pid");
    if (rc != 0 || rc_pull != 0) fprintf(stderr, "dev: rc=%d pull=%d err=%s\n", rc, rc_pull, err);
    assert(rc == 0 && rc_pull == 0);
    assert(access(ROOT "/plugins/test-dev-plug/bin/drv", X_OK) == 0);
    assert(access(ROOT "/plugins/test-dev-plug/sub/helper.sh", X_OK) == 0);
    assert(access(ROOT "/plugins/test-dev-plug/.source.json", R_OK) == 0);
    assert(plugin_mgr_is_running("test-dev-plug"));
    /* a release install with no repository listing it cannot be pulled */
    assert(plugin_mgr_pull("not-installed", err, sizeof(err)) == -1);
    assert(plugin_mgr_uninstall("test-dev-plug") == 0);
    printf("[+] Dev raw install, pull from its source.\n");

    /* 20. URL install with the right SHA-256, then the shutdown stops every child */
    sh("rm -rf /tmp/test_sha_srv && mkdir -p /tmp/test_sha_srv && cp /tmp/test_mock_plug.tar.gz /tmp/test_sha_srv/p.tar.gz");
    char good_sha[65];
    file_sha("/tmp/test_sha_srv/p.tar.gz", good_sha);
    sh("python3 -m http.server 9989 --directory /tmp/test_sha_srv >/dev/null 2>&1 & echo $! > /tmp/test_http_pid");
    usleep(500000);
    rc = plugin_mgr_install_from_url("http://127.0.0.1:9989/p.tar.gz",
                                     "0000000000000000000000000000000000000000000000000000000000000000", err, sizeof(err));
    int rc_good = plugin_mgr_install_from_url("http://127.0.0.1:9989/p.tar.gz", good_sha, err, sizeof(err));
    int rc_down = plugin_mgr_install_from_url("http://127.0.0.1:9/none.tar.gz", good_sha, err, sizeof(err));
    sh("kill -9 $(cat /tmp/test_http_pid 2>/dev/null) 2>/dev/null; rm -f /tmp/test_http_pid; rm -rf /tmp/test_sha_srv");
    assert(rc == -1);
    assert(rc_good == 0);
    assert(rc_down == PLUGIN_ERR_DOWNLOAD);
    assert(access(ROOT "/plugins/test-roku/.source.json", R_OK) == 0);
    int last_pid = installed_field("test-roku", "pid");
    assert(last_pid > 1);
    plugin_mgr_shutdown();
    assert(pid_gone((pid_t)last_pid));
    assert(plugin_mgr_uninstall("test-roku") == 0);
    printf("[+] URL install SHA-256 checked, download failure reported, shutdown stops plugins.\n");

    sh("rm -rf " SRC " /tmp/test_mock_plug.tar.gz /tmp/test_dev_src /tmp/test_plug_env /tmp/test_plug_child");

    printf("\n===========================================================\n");
    printf("  ALL PLUGIN MANAGER CORE UNIT TESTS PASSED!\n");
    printf("===========================================================\n");
    return 0;
}
