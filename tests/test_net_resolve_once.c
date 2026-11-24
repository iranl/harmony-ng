/*
 * net_action.c (product build, no TEST_HARNESS): a device's host is resolved once per send, and
 * the connection goes to the address that was checked.
 *
 * getaddrinfo() and connect() are replaced here, so no packet leaves the machine. The name
 * "rebind.test" answers 192.0.2.7 (not the Hub) at the first lookup and 127.0.0.1 (the Hub
 * itself) at every later one, as a DNS server under someone else's control could:
 * - plain TCP: one lookup of the name, and no connection to 127.0.0.1;
 * - TLS (hub_fetch): one lookup, and hub_fetch gets the checked address with --connect-to;
 * - a name that answers 127.0.0.1 at once is refused, nothing is sent.
 * Before the fix the send resolved the name a second time and connected to 127.0.0.1.
 */
#include "../payload/source/net_action.c"

int g_dry_run = 0;

static int s_lookups;           /* lookups of "rebind.test" / "local.test" */
static int s_loopback_connects; /* connect() calls to 127.x */
static int s_connects;
static char s_fetch_args[1024]; /* hub_fetch's argument list, joined */

static struct addrinfo *make_ai(const char *ip, const char *service)
{
    struct addrinfo *ai = calloc(1, sizeof *ai);
    struct sockaddr_in *sin = calloc(1, sizeof *sin);
    sin->sin_family = AF_INET;
    sin->sin_port = htons(service ? (uint16_t)atoi(service) : 0);
    inet_pton(AF_INET, ip, &sin->sin_addr);
    ai->ai_family = AF_INET;
    ai->ai_socktype = SOCK_STREAM;
    ai->ai_protocol = IPPROTO_TCP;
    ai->ai_addr = (struct sockaddr *)sin;
    ai->ai_addrlen = sizeof *sin;
    return ai;
}

int getaddrinfo(const char *node, const char *service, const struct addrinfo *hints, struct addrinfo **res)
{
    struct in_addr tmp;
    *res = NULL;
    if (!node) return EAI_NONAME;
    if (strcmp(node, "rebind.test") == 0) {
        if (hints && (hints->ai_flags & AI_NUMERICHOST)) return EAI_NONAME;
        *res = make_ai(s_lookups++ == 0 ? "192.0.2.7" : "127.0.0.1", service);
        return 0;
    }
    if (strcmp(node, "local.test") == 0) {
        if (hints && (hints->ai_flags & AI_NUMERICHOST)) return EAI_NONAME;
        s_lookups++;
        *res = make_ai("127.0.0.1", service);
        return 0;
    }
    if (inet_pton(AF_INET, node, &tmp) == 1) {
        *res = make_ai(node, service);
        return 0;
    }
    return EAI_NONAME;
}

void freeaddrinfo(struct addrinfo *res)
{
    while (res) {
        struct addrinfo *n = res->ai_next;
        free(res->ai_addr);
        free(res);
        res = n;
    }
}

int connect(int fd, const struct sockaddr *sa, socklen_t len)
{
    (void)fd;
    (void)len;
    s_connects++;
    if (sa->sa_family == AF_INET &&
            (ntohl(((const struct sockaddr_in *)sa)->sin_addr.s_addr) >> 24) == 127)
        s_loopback_connects++;
    errno = ECONNREFUSED;
    return -1;
}

/* the rest of net_action.c's world */
const char *config_store_path(const char *rel)
{
    static char p[256];
    snprintf(p, sizeof p, "/nonexistent/%s", rel);
    return p;
}
int hub_fetch_available(void) { return 1; }
const char *hub_fetch_path(void) { return "/nonexistent/hub_fetch"; }
int run_argv_limited(char *const argv[], const char *watch_path, long max_bytes, unsigned int max_ms)
{
    (void)watch_path;
    (void)max_bytes;
    (void)max_ms;
    s_fetch_args[0] = '\0';
    for (int i = 0; argv[i]; i++) {
        strncat(s_fetch_args, argv[i], sizeof s_fetch_args - strlen(s_fetch_args) - 2);
        strcat(s_fetch_args, " ");
    }
    return 1; /* hub_fetch "ran" without a status file: an error for the caller */
}

static int failures;

static void check(int ok, const char *what)
{
    printf("  %s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) failures++;
}

static net_job *tcp_job(const char *host, int tls)
{
    net_job *j = calloc(1, sizeof *j);
    j->kind = NET_KIND_TCP;
    snprintf(j->host, sizeof j->host, "%s", host);
    j->port = 7;
    j->tls = tls;
    j->verify = NET_TLS_INSECURE;
    j->timeout_ms = 1000;
    j->sync = 1;
    j->payload = strdup("x");
    j->payload_len = 1;
    snprintf(j->label, sizeof j->label, "Test (99) X");
    return j;
}

int main(void)
{
    net_job *j = tcp_job("rebind.test", 0);
    s_lookups = s_connects = s_loopback_connects = 0;
    job_run(j);
    check(s_lookups == 1, "plain TCP: the name is resolved once");
    check(s_connects >= 1 && s_loopback_connects == 0, "plain TCP: the connection goes to the checked address, not 127.0.0.1");
    job_free(j);

    j = tcp_job("rebind.test", 1);
    s_lookups = s_connects = s_loopback_connects = 0;
    job_run(j);
    check(s_lookups == 1, "TLS: the name is resolved once");
    check(strstr(s_fetch_args, " --connect-to 192.0.2.7 ") != NULL, "TLS: hub_fetch gets the checked address");
    check(strstr(s_fetch_args, " rebind.test 7 ") != NULL, "TLS: hub_fetch keeps the name (SNI)");
    job_free(j);

    j = tcp_job("local.test", 0);
    s_lookups = s_connects = s_loopback_connects = 0;
    job_run(j);
    const char *err = jstr(j->result, "error");
    check(strstr(err, "Hub itself") != NULL, "a name for 127.0.0.1 is refused");
    check(s_connects == 0, "nothing is sent to it");
    job_free(j);

    if (failures) {
        printf("net resolve once: %d FAILED\n", failures);
        return 1;
    }
    printf("all net resolve once checks passed\n");
    return 0;
}
