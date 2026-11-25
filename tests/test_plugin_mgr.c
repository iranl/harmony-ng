#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <pthread.h>
#include <time.h>

#include "../payload/source/plugin_mgr.h"
#include "../payload/source/cJSON.h"
#include "../payload/source/http_util.h"

/* Plugin manager on the host: install checks, the fork/exec lifecycle, binding of devices.
 * Built with -DHARMONY_PLUGIN_ROOT under the build folder (tests/suite.d/27-plugins.sh). */

#define ROOT HARMONY_PLUGIN_ROOT
#define RUN  HARMONY_PLUGINS_RUN_DIR
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

static const char *const MEMINFO_PLENTY = "MemTotal: 61440 kB\nMemFree: 20000 kB\nCached: 8000 kB\n";

static int s_restart_rc[2];
static void *restart_thread(void *arg) {
    *(int *)arg = plugin_mgr_restart("socky");
    return NULL;
}

static void *restart_slowstop_thread(void *arg) {
    *(int *)arg = plugin_mgr_restart("slowstop");
    return NULL;
}

/* 1 when the installed manifest of id holds text */
static int manifest_has(const char *id, const char *text) {
    char path[300];
    snprintf(path, sizeof(path), ROOT "/plugins/%s/manifest.json", id);
    size_t len = 0;
    char *m = read_file_text_alloc(path, 32768, &len);
    int hit = m && strstr(m, text) != NULL;
    free(m);
    return hit;
}

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0); /* progress survives an abort in CI logs */
    char err[256] = {0};
    int rc;
    printf("[*] Running Plugin Manager Core Unit Tests...\n");
    sh("rm -rf " ROOT " && mkdir -p " ROOT "/config");
    /* no repository: the catalog is empty and no test goes to the network */
    write_file(ROOT "/config/repositories.json", "[]\n", 0);
    unlink(RUN "/marketplace_cache.json");
    /* /tmp has no size here (statfs of /proc, as ramfs on the Hub): free RAM is read from this */
    write_file(PLUGIN_MEMINFO_PATH, MEMINFO_PLENTY, 0);

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
        { "{\"id\": \"ok\", \"version\": \"1\", \"entrypoint\": \"bin/drv\", \"device_definition\": {\"default_commands\": [{\"name\": \"L\", \"range\": [100, 1]}]}}", "range" },
        { "{\"id\": \"ok\", \"version\": \"1\", \"entrypoint\": \"bin/drv\", \"device_definition\": {\"default_commands\": [{\"name\": \"L\", \"range\": [1.5, 9]}]}}", "range" },
        { "{\"id\": \"ok\", \"version\": \"1\", \"entrypoint\": \"bin/drv\", \"device_definition\": {\"default_commands\": [{\"name\": \"L\", \"range\": [1]}]}}", "range" },
        { "{\"id\": \"ok\", \"version\": \"1\", \"entrypoint\": \"bin/drv\", \"device_definition\": {\"default_commands\": [{\"name\": \"H\", \"irKeyCode\": 5}]}}", "irKeyCode" },
        { "{\"id\": \"ok\", \"version\": \"1\", \"entrypoint\": \"bin/drv\", \"device_definition\": {\"default_commands\": [{\"name\": \"H\", \"irKeyCode\": \"\"}]}}", "irKeyCode" },
        { "{\"id\": \"ok\", \"version\": \"1\", \"entrypoint\": \"bin/drv\", \"device_definition\": {\"default_commands\": [{\"name\": \"H\", \"irWhen\": \"a b\"}]}}", "irWhen" },
        { "{\"id\": \"ok\", \"version\": \"1\", \"entrypoint\": \"bin/drv\", \"device_definition\": {\"default_commands\": [{\"name\": \"H\", \"irWhen\": true}]}}", "irWhen" },
        { "{\"id\": \"ok\", \"version\": \"1\", \"entrypoint\": \"bin/drv\", \"requires_libs\": \"libbearssl.so.0\"}", "must be an array" },
        { "{\"id\": \"ok\", \"version\": \"1\", \"entrypoint\": \"bin/drv\", \"requires_libs\": [\"../lib/x.so\"]}", "bad library name" },
        { "{\"id\": \"ok\", \"version\": \"1\", \"entrypoint\": \"bin/drv\", \"requires_libs\": [\"libnope.so.0\"]}", "does not have" },
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
    printf("[+] Bad manifests refused (id, version, entrypoint, config_fields, command range, irKeyCode, irWhen, requires_libs).\n");

    /* A command with an infrared alternative (irKeyCode, irWhen) is accepted */
    make_src("iralt", "1", "exec sleep 300");
    write_file(SRC "/manifest.json",
               "{\"id\": \"iralt\", \"version\": \"1\", \"entrypoint\": \"bin/drv\", \"device_definition\": {"
               "\"config_fields\": [{\"key\": \"hdmi_via_ir\", \"type\": \"boolean\"}], \"default_commands\": ["
               "{\"name\": \"HDMI 2\", \"keyCode\": \"PLUGIN:iralt:KEY_HDMI2\", \"irKeyCode\": \"G:GoVideoO1 32 Bit:()(0xE0E07D82)():3\","
               " \"irWhen\": \"hdmi_via_ir\"}]}}", 0);
    pack("/tmp/test_iralt.tar.gz");
    rc = plugin_mgr_install_archive("/tmp/test_iralt.tar.gz", NULL, err, sizeof(err));
    if (rc != 0) fprintf(stderr, "irKeyCode install: rc=%d err=%s\n", rc, err);
    assert(rc == 0);
    plugin_mgr_uninstall("iralt");
    unlink("/tmp/test_iralt.tar.gz");
    printf("[+] default_commands with irKeyCode and irWhen: installed.\n");

    /* A library the Hub has (HARMONY_PLUGIN_LIB_DIR) is accepted */
    sh("mkdir -p " ROOT "/lib && : > " ROOT "/lib/libok.so.0");
    make_src("libuser", "1", "exec sleep 300");
    write_file(SRC "/manifest.json",
               "{\"id\": \"libuser\", \"version\": \"1\", \"entrypoint\": \"bin/drv\", \"requires_libs\": [\"libok.so.0\"]}", 0);
    pack("/tmp/test_libuser.tar.gz");
    rc = plugin_mgr_install_archive("/tmp/test_libuser.tar.gz", NULL, err, sizeof(err));
    if (rc != 0) fprintf(stderr, "requires_libs install: rc=%d err=%s\n", rc, err);
    assert(rc == 0);
    plugin_mgr_uninstall("libuser");
    unlink("/tmp/test_libuser.tar.gz");
    sh("rm -rf " ROOT "/lib");
    printf("[+] requires_libs with a library the Hub has: installed.\n");

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
             "echo \"$1|$HARMONY_PLUGIN_ID|$HARMONY_SOCKET|$HARMONY_PLUGIN_DATA|$LD_LIBRARY_PATH\" > /tmp/test_plug_env\n"
             "sleep 300 &\necho $! > /tmp/test_plug_child\nexec sleep 300");
    pack("/tmp/test_mock_plug.tar.gz");
    unlink("/tmp/test_plug_env");
    unlink("/tmp/test_plug_child");
    /* A long inherited PATH (CI runners have one) reaches the plugin whole: a fixed buffer cut
     * it before /usr/bin, and the plugin's commands were not found */
    {
        const char *p = getenv("PATH");
        size_t n = 1200 + strlen(p ? p : "");
        char *longp = malloc(n);
        assert(longp);
        memset(longp, 0, n);
        for (int i = 0; i < 40; i++) strcat(longp, "/nonexistent/long/path/entry:");
        strcat(longp, p ? p : "/usr/bin:/bin");
        setenv("PATH", longp, 1);
        free(longp);
    }
    rc = plugin_mgr_install_archive("/tmp/test_mock_plug.tar.gz", NULL, err, sizeof(err));
    if (rc != 0) fprintf(stderr, "install: %s\n", err);
    assert(rc == 0);
    assert(plugin_mgr_is_running("test-roku"));
    pid_t child = read_pid("/tmp/test_plug_child");
    assert(child > 1);
    int pid = installed_field("test-roku", "pid");
    if (pid <= 1) sh("cat " RUN "/test-roku.log >&2");
    assert(pid > 1);
    {
        char env[512] = {0};
        FILE *f = fopen("/tmp/test_plug_env", "r");
        assert(f && fgets(env, sizeof(env), f));
        fclose(f);
        assert(strstr(env, RUN "/test-roku.sock|test-roku|" RUN "/test-roku.sock|" ROOT "/plugin-data/test-roku"));
        /* the shared libraries for plugins: the stock ld-uClibc ignores a binary's rpath */
        assert(strstr(env, "|" ROOT "/lib"));
    }
    assert(access(ROOT "/plugins/test-roku/.source.json", F_OK) != 0); /* local archive: no source */
    /* no source: a pull would take the first catalog entry with this id, from any repository */
    assert(plugin_mgr_pull("test-roku", err, sizeof(err)) == -1 && strstr(err, "not known"));
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
    assert(access(RUN "/test-roku.stopped", F_OK) == 0);
    printf("[+] Stop ends the plugin and its process group.\n");

    /* 10. A stale pid of another process no longer decides anything: the Hub knows its child */
    write_file(RUN "/test-roku.pid", "1\n", 0);
    assert(!plugin_mgr_is_running("test-roku"));
    unlink(RUN "/test-roku.pid");

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

    /* 15. A plugin left by a killed hub_daemon (not our child, HARMONY_PLUGIN_ID in its
     * environment) is stopped at init. A program started by hand in the plugin folder is no
     * plugin: the sweep by working directory killed it, and the shell that ran init.sh. */
    plugin_mgr_stop("test-roku");
    /* a shell that exported only the id, and a plugin of another run folder (a second test
     * daemon on the same machine), are no plugin of this hub_daemon either */
    char *const env_orphan[] = { "HARMONY_PLUGIN_ID=test-roku", "HARMONY_SOCKET=" RUN "/test-roku.sock", "PATH=/usr/bin:/bin", NULL };
    char *const env_by_hand[] = { "PATH=/usr/bin:/bin", NULL };
    char *const env_id_only[] = { "HARMONY_PLUGIN_ID=test-roku", "PATH=/usr/bin:/bin", NULL };
    char *const env_other[] = { "HARMONY_PLUGIN_ID=test-roku", "HARMONY_SOCKET=/tmp/elsewhere/test-roku.sock", "PATH=/usr/bin:/bin", NULL };
    char *const *envs[] = { env_orphan, env_by_hand, env_id_only, env_other };
    pid_t procs[4];
    for (int i = 0; i < 4; i++) {
        procs[i] = fork();
        if (procs[i] == 0) {
            if (chdir(ROOT "/plugins/test-roku") != 0) _exit(1);
            execle("/bin/sleep", "sleep", "300", (char *)NULL, envs[i]);
            _exit(1);
        }
    }
    usleep(100000);
    plugin_mgr_init();
    int st;
    for (int i = 0; i < 100 && waitpid(procs[0], &st, WNOHANG) == 0; i++) usleep(20000);
    assert(kill(procs[0], 0) != 0);
    for (int i = 1; i < 4; i++) {
        assert(waitpid(procs[i], &st, WNOHANG) == 0); /* still running */
        kill(procs[i], SIGKILL);
        waitpid(procs[i], &st, 0);
    }
    assert(plugin_mgr_is_running("test-roku")); /* enabled: started again by init */
    printf("[+] Orphan plugin process stopped at init; by hand, id only and another run folder kept; plugin started fresh.\n");

    /* 15b. The helper of a leader that died goes before the next start (each restart of a
     * plugin that forks added a copy); a copy that daemonized goes with the uninstall */
    make_src("forky", "1", "sleep 301 &\necho $! > /tmp/test_forky_child\nexec sleep 302");
    pack("/tmp/test_forky.tar.gz");
    unlink("/tmp/test_forky_child");
    assert(plugin_mgr_install_archive("/tmp/test_forky.tar.gz", NULL, err, sizeof(err)) == 0);
    pid_t helper = read_pid("/tmp/test_forky_child");
    int leader = installed_field("forky", "pid");
    assert(helper > 1 && leader > 1);
    kill(leader, SIGKILL);
    for (int i = 0; i < 100 && plugin_mgr_is_running("forky"); i++) usleep(20000);
    assert(!plugin_mgr_is_running("forky"));
    assert(!pid_gone(helper)); /* the leader alone died */
    assert(plugin_mgr_start("forky") == 0);
    for (int i = 0; i < 100 && !pid_gone(helper); i++) usleep(20000);
    assert(pid_gone(helper));
    assert(plugin_mgr_is_running("forky"));
    /* 15c. /tmp without a size (ramfs on the Hub, 0 blocks): free RAM from meminfo, which
     * refuses an unpack that would leave less than 2 MB */
    write_file(PLUGIN_MEMINFO_PATH, "MemTotal: 61440 kB\nMemFree: 100 kB\nCached: 200 kB\n", 0);
    rc = plugin_mgr_install_archive("/tmp/test_forky.tar.gz", NULL, err, sizeof(err));
    assert(rc != 0 && strstr(err, "RAM"));
    write_file(PLUGIN_MEMINFO_PATH, MEMINFO_PLENTY, 0);
    make_src("forky", "2", "setsid sleep 303 </dev/null >/dev/null 2>&1 &\necho $! > /tmp/test_forky_child\nexit 0");
    pack("/tmp/test_forky.tar.gz");
    unlink("/tmp/test_forky_child");
    assert(plugin_mgr_install_archive("/tmp/test_forky.tar.gz", NULL, err, sizeof(err)) == 0);
    pid_t daemonized = read_pid("/tmp/test_forky_child");
    assert(daemonized > 1);
    for (int i = 0; i < 100 && plugin_mgr_is_running("forky"); i++) usleep(20000);
    assert(!pid_gone(daemonized));
    assert(plugin_mgr_uninstall("forky") == 0);
    for (int i = 0; i < 100 && !pid_gone(daemonized); i++) usleep(20000);
    assert(pid_gone(daemonized));
    unlink("/tmp/test_forky.tar.gz");
    unlink("/tmp/test_forky_child");
    printf("[+] Helpers of a dead leader and a daemonized copy are stopped; ramfs /tmp read from meminfo.\n");

    /* 15d. An entrypoint with a dot name (the copy skips those) is refused; v1 stays */
    make_src("dotty", "1", "exec sleep 304");
    pack("/tmp/test_dotty.tar.gz");
    assert(plugin_mgr_install_archive("/tmp/test_dotty.tar.gz", NULL, err, sizeof(err)) == 0);
    make_src("dotty", "2", "exec sleep 305");
    sh("mv " SRC "/bin/drv " SRC "/bin/.drv && sed -i 's#bin/drv#bin/.drv#' " SRC "/manifest.json");
    pack("/tmp/test_dotty.tar.gz");
    assert(plugin_mgr_install_archive("/tmp/test_dotty.tar.gz", NULL, err, sizeof(err)) != 0);
    assert(plugin_mgr_is_running("dotty") && manifest_has("dotty", "\"version\": \"1\""));
    assert(plugin_mgr_uninstall("dotty") == 0);
    unlink("/tmp/test_dotty.tar.gz");

    /* 15e. Two restarts at once: one waits its turn as busy, the socket the new instance
     * created stays (the older stop removed it) */
    make_src("socky", "1", "touch \"$1\"\nexec sleep 306");
    pack("/tmp/test_socky.tar.gz");
    assert(plugin_mgr_install_archive("/tmp/test_socky.tar.gz", NULL, err, sizeof(err)) == 0);
    pthread_t th[2];
    for (int i = 0; i < 2; i++) pthread_create(&th[i], NULL, restart_thread, &s_restart_rc[i]);
    for (int i = 0; i < 2; i++) pthread_join(th[i], NULL);
    assert(s_restart_rc[0] == 0 || s_restart_rc[1] == 0);
    assert((s_restart_rc[0] == 0 || s_restart_rc[0] == PLUGIN_ERR_BUSY) && (s_restart_rc[1] == 0 || s_restart_rc[1] == PLUGIN_ERR_BUSY));
    for (int i = 0; i < 100 && access(RUN "/socky.sock", F_OK) != 0; i++) usleep(20000);
    usleep(200000);
    assert(access(RUN "/socky.sock", F_OK) == 0 && plugin_mgr_is_running("socky"));
    assert(plugin_mgr_uninstall("socky") == 0);
    unlink("/tmp/test_socky.tar.gz");

    /* 15f. stop or disable of an id that is not installed: not found, no marker in /tmp */
    assert(plugin_mgr_stop("never-installed") == PLUGIN_ERR_NOT_FOUND);
    assert(plugin_mgr_set_enabled("never-installed", 0) == PLUGIN_ERR_NOT_FOUND);
    assert(access(RUN "/never-installed.stopped", F_OK) != 0);
    printf("[+] Dot-named entrypoint refused, overlapping restarts keep the socket, unknown stop is 404.\n");

    /* 15g. A helper's EXIT trap (2 s here, about 5 s for ps5-wake) is not cut: one SIGTERM,
     * the SIGKILL only after the grace (3 s in this build). A stop whose leader exits on
     * SIGTERM sends its group no second one; a start after the leader died does not wait. */
    make_src("trappy", "1",
             "( trap 'sleep 2; touch /tmp/test_trap_done; exit 0' TERM; while :; do sleep 1 & wait $!; done ) &\n"
             "echo $! > /tmp/test_trap_child\nexec sleep 310");
    pack("/tmp/test_trappy.tar.gz");
    unlink("/tmp/test_trap_done");
    unlink("/tmp/test_trap_child");
    assert(plugin_mgr_install_archive("/tmp/test_trappy.tar.gz", NULL, err, sizeof(err)) == 0);
    pid_t trap_child = read_pid("/tmp/test_trap_child");
    assert(trap_child > 1);
    usleep(200000);
    assert(plugin_mgr_stop("trappy") == 0);
    for (int i = 0; i < 150 && access("/tmp/test_trap_done", F_OK) != 0; i++) {
        plugin_mgr_tick();
        usleep(20000);
    }
    assert(access("/tmp/test_trap_done", F_OK) == 0);
    unlink("/tmp/test_trap_done");
    unlink("/tmp/test_trap_child");
    assert(plugin_mgr_start("trappy") == 0);
    trap_child = read_pid("/tmp/test_trap_child");
    assert(trap_child > 1);
    usleep(200000);
    kill(installed_field("trappy", "pid"), SIGKILL);
    for (int i = 0; i < 100 && plugin_mgr_is_running("trappy"); i++) usleep(20000);
    {
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        assert(plugin_mgr_start("trappy") == 0);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        long ms = (t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000;
        assert(ms < 500); /* the main loop (watchdog restart) is not held */
    }
    for (int i = 0; i < 150 && access("/tmp/test_trap_done", F_OK) != 0; i++) {
        plugin_mgr_tick();
        usleep(20000);
    }
    assert(access("/tmp/test_trap_done", F_OK) == 0);
    assert(plugin_mgr_uninstall("trappy") == 0);
    /* a helper that ignores SIGTERM gets the SIGKILL from the tick once the grace has passed */
    make_src("stubborn", "1", "( trap '' TERM; while :; do sleep 1; done ) &\necho $! > /tmp/test_trap_child\nexec sleep 311");
    pack("/tmp/test_trappy.tar.gz");
    unlink("/tmp/test_trap_child");
    assert(plugin_mgr_install_archive("/tmp/test_trappy.tar.gz", NULL, err, sizeof(err)) == 0);
    pid_t stubborn = read_pid("/tmp/test_trap_child");
    assert(stubborn > 1);
    usleep(200000);
    kill(installed_field("stubborn", "pid"), SIGKILL);
    for (int i = 0; i < 100 && plugin_mgr_is_running("stubborn"); i++) usleep(20000);
    assert(plugin_mgr_start("stubborn") == 0);
    plugin_mgr_tick();
    usleep(500000);
    assert(!pid_gone(stubborn)); /* within the grace */
    for (int i = 0; i < 250 && !pid_gone(stubborn); i++) {
        plugin_mgr_tick();
        usleep(20000);
    }
    assert(pid_gone(stubborn));
    stubborn = read_pid("/tmp/test_trap_child"); /* the helper of the new instance */
    assert(plugin_mgr_uninstall("stubborn") == 0);
    for (int i = 0; i < 250 && !pid_gone(stubborn); i++) {
        plugin_mgr_tick();
        usleep(20000);
    }
    assert(pid_gone(stubborn));
    unlink("/tmp/test_trappy.tar.gz");
    unlink("/tmp/test_trap_done");
    unlink("/tmp/test_trap_child");

    /* 15h. A disable while a restart runs answers busy and changes nothing */
    make_src("slowstop", "1", "trap '' TERM\nwhile :; do sleep 1; done");
    pack("/tmp/test_slowstop.tar.gz");
    assert(plugin_mgr_install_archive("/tmp/test_slowstop.tar.gz", NULL, err, sizeof(err)) == 0);
    usleep(200000);
    {
        pthread_t t;
        int t_rc = -99;
        pthread_create(&t, NULL, restart_slowstop_thread, &t_rc);
        usleep(300000); /* its stop waits 1 s for a leader that ignores SIGTERM */
        assert(plugin_mgr_set_enabled("slowstop", 0) == PLUGIN_ERR_BUSY);
        assert(access(ROOT "/plugins/slowstop/disabled", F_OK) != 0);
        pthread_join(t, NULL);
        assert(t_rc == 0 && plugin_mgr_is_running("slowstop"));
    }
    assert(plugin_mgr_uninstall("slowstop") == 0);
    unlink("/tmp/test_slowstop.tar.gz");

    /* 15i. The log cap acts at the 1 s tick */
    make_src("loud", "1", "head -c 400000 /dev/zero | tr '\\000' x\nexec sleep 312");
    pack("/tmp/test_loud.tar.gz");
    assert(plugin_mgr_install_archive("/tmp/test_loud.tar.gz", NULL, err, sizeof(err)) == 0);
    {
        struct stat lst;
        for (int i = 0; i < 100 && (stat(RUN "/loud.log", &lst) != 0 || lst.st_size < 400000); i++) usleep(20000);
        assert(lst.st_size >= 400000);
        plugin_mgr_tick();
        assert(stat(RUN "/loud.log", &lst) == 0 && lst.st_size < 256 * 1024);
    }
    assert(plugin_mgr_uninstall("loud") == 0);
    unlink("/tmp/test_loud.tar.gz");
    /* a log no running slot writes (a helper writing on while its leader is dead) is capped too */
    {
        char big[1024];
        memset(big, 'x', sizeof(big));
        FILE *f = fopen(RUN "/ghost.log", "w");
        assert(f);
        for (int i = 0; i < 400; i++) fwrite(big, 1, sizeof(big), f);
        fclose(f);
        struct stat lst;
        plugin_mgr_tick();
        assert(stat(RUN "/ghost.log", &lst) == 0 && lst.st_size == 0);
        unlink(RUN "/ghost.log");
    }

    /* 15j. A leader that ignores SIGTERM gets the SIGKILL at 1 s, its helper does not: the
     * helper's EXIT trap (ps5-wake: signal() keeps recv() blocking) still completes */
    make_src("deaf", "1",
             "( trap 'sleep 2; touch /tmp/test_trap_done; exit 0' TERM; while :; do sleep 1 & wait $!; done ) &\n"
             "echo $! > /tmp/test_trap_child\ntrap '' TERM\nwhile :; do sleep 1; done");
    pack("/tmp/test_deaf.tar.gz");
    unlink("/tmp/test_trap_done");
    unlink("/tmp/test_trap_child");
    assert(plugin_mgr_install_archive("/tmp/test_deaf.tar.gz", NULL, err, sizeof(err)) == 0);
    trap_child = read_pid("/tmp/test_trap_child");
    assert(trap_child > 1);
    usleep(200000);
    int deaf_leader = installed_field("deaf", "pid");
    assert(plugin_mgr_stop("deaf") == 0);
    assert(pid_gone(deaf_leader));
    for (int i = 0; i < 200 && access("/tmp/test_trap_done", F_OK) != 0; i++) {
        plugin_mgr_tick();
        usleep(20000);
    }
    assert(access("/tmp/test_trap_done", F_OK) == 0);
    unlink("/tmp/test_trap_done");

    /* 15k. hub_daemon start does not wait for a leftover that ignores SIGTERM (the HTTP
     * sockets already listen): the tick sends the SIGKILL after the grace */
    {
        char *const env_deaf[] = { "HARMONY_PLUGIN_ID=deaf", "HARMONY_SOCKET=" RUN "/deaf.sock", "PATH=/usr/bin:/bin", NULL };
        pid_t deaf = fork();
        if (deaf == 0) {
            execle("/bin/sh", "sh", "-c", "trap '' TERM; while :; do sleep 1; done", (char *)NULL, env_deaf);
            _exit(1);
        }
        usleep(200000);
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        plugin_mgr_init();
        clock_gettime(CLOCK_MONOTONIC, &t1);
        long ms = (t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000;
        assert(ms < 500);
        int dst;
        assert(waitpid(deaf, &dst, WNOHANG) == 0); /* within the grace */
        int gone = 0;
        for (int i = 0; i < 250 && !gone; i++) {
            plugin_mgr_tick();
            usleep(20000);
            gone = waitpid(deaf, &dst, WNOHANG) == deaf;
        }
        assert(gone && WIFSIGNALED(dst) && WTERMSIG(dst) == SIGKILL);
    }
    assert(plugin_mgr_uninstall("deaf") == 0);
    unlink("/tmp/test_deaf.tar.gz");
    unlink("/tmp/test_trap_child");
    printf("[+] Helper traps not cut (also with a leader that ignores SIGTERM), non-blocking restart and init sweeps, SIGKILL after grace, busy disable unchanged, log cap at the tick for every log.\n");

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
    assert(access(RUN "/test-roku.log", F_OK) != 0);
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

    /* 19b. Two repositories list the same id: a pull takes the entry of the repository the
     * plugin was installed from, not the first one */
    sh("rm -rf /tmp/test_repo_srv && mkdir -p /tmp/test_repo_srv");
    make_src("twin", "1.0.0", "exec sleep 307");
    pack("/tmp/test_repo_srv/b.tar.gz");
    make_src("twin", "9.9.9", "exec sleep 308");
    pack("/tmp/test_repo_srv/a.tar.gz");
    {
        char sha[65], json[512];
        const char *names[] = { "a", "b" }, *vers[] = { "9.9.9", "1.0.0" };
        for (int i = 0; i < 2; i++) {
            char path[128];
            snprintf(path, sizeof(path), "/tmp/test_repo_srv/%s.tar.gz", names[i]);
            file_sha(path, sha);
            snprintf(json, sizeof(json), "{\"plugins\": [{\"id\": \"twin\", \"version\": \"%s\","
                     " \"download_url\": \"http://127.0.0.1:9990/%s.tar.gz\", \"sha256\": \"%s\"}]}\n", vers[i], names[i], sha);
            snprintf(path, sizeof(path), "/tmp/test_repo_srv/%s.json", names[i]);
            write_file(path, json, 0);
        }
    }
    write_file(ROOT "/config/repositories.json",
               "[\"http://127.0.0.1:9990/a.json\", \"http://127.0.0.1:9990/b.json\"]\n", 0);
    unlink(RUN "/marketplace_cache.json");
    sh("python3 -m http.server 9990 --directory /tmp/test_repo_srv >/dev/null 2>&1 & echo $! > /tmp/test_http_pid");
    usleep(500000);
    int rc_twin = plugin_mgr_install_from_url("http://127.0.0.1:9990/b.tar.gz", NULL, err, sizeof(err));
    int had_b = manifest_has("twin", "1.0.0");
    int rc_twin_pull = plugin_mgr_pull("twin", err, sizeof(err));
    sh("kill -9 $(cat /tmp/test_http_pid 2>/dev/null) 2>/dev/null; rm -f /tmp/test_http_pid; rm -rf /tmp/test_repo_srv");
    if (rc_twin != 0 || rc_twin_pull != 0) fprintf(stderr, "twin: %d %d %s\n", rc_twin, rc_twin_pull, err);
    assert(rc_twin == 0 && had_b && rc_twin_pull == 0);
    assert(manifest_has("twin", "1.0.0") && !manifest_has("twin", "9.9.9"));
    write_file(ROOT "/config/repositories.json", "[]\n", 0);
    unlink(RUN "/marketplace_cache.json");
    assert(plugin_mgr_uninstall("twin") == 0);
    printf("[+] Pull uses the repository the plugin was installed from.\n");

    /* 19c. A power cut between the two renames of an update leaves .<id>.new (complete) and
     * no <id>: init puts it in place and removes what is left */
    sh("mkdir -p " ROOT "/plugins/.phoenix.new/bin " ROOT "/plugins/.phoenix.old");
    write_file(ROOT "/plugins/.phoenix.new/manifest.json", "{\"id\": \"phoenix\", \"version\": \"2\", \"entrypoint\": \"bin/drv\"}\n", 0);
    write_file(ROOT "/plugins/.phoenix.new/bin/drv", "#!/bin/sh\nexec sleep 309\n", 1);
    /* a first install cut during its copy (no .old): half the files, never put in place */
    sh("mkdir -p " ROOT "/plugins/.halfcopy.new/bin");
    write_file(ROOT "/plugins/.halfcopy.new/manifest.json", "{\"id\": \"halfcopy\", \"version\": \"1\", \"entrypoint\": \"bin/drv\"}\n", 0);
    write_file(ROOT "/plugins/.halfcopy.new/bin/drv", "#!/bin/sh\ntouch /tmp/test_halfcopy_ran\nexec sl", 1);
    unlink("/tmp/test_halfcopy_ran");
    plugin_mgr_init();
    assert(access(ROOT "/plugins/phoenix/bin/drv", F_OK) == 0);
    assert(access(ROOT "/plugins/.phoenix.new", F_OK) != 0 && access(ROOT "/plugins/.phoenix.old", F_OK) != 0);
    assert(plugin_mgr_is_running("phoenix"));
    assert(plugin_mgr_uninstall("phoenix") == 0);
    assert(access(ROOT "/plugins/halfcopy", F_OK) != 0 && access(ROOT "/plugins/.halfcopy.new", F_OK) != 0);
    usleep(100000);
    assert(access("/tmp/test_halfcopy_ran", F_OK) != 0);
    printf("[+] An update cut between its renames is put back in place at init; a cut first install is removed.\n");

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
