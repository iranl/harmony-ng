/*
 * hub_fetch: the Hub's one TLS client (BearSSL, linked dynamically to libbearssl.so.0 in
 * /data/harmony/lib, which plugins share). The core binaries are static and carry no TLS:
 * update.sh, hub_daemon and hub_rf run this program for every https:// download and for the
 * commands of HTTPS and TLS TCP devices (net_action.c).
 *
 * The Hub's ld-uClibc ignores DT_RPATH: run it with LD_LIBRARY_PATH=/data/harmony/lib.
 *
 *   hub_fetch [options] -o out|- URL               HTTP(S) request
 *   hub_fetch --tcp [options] -o out|- HOST PORT   TCP (with --tls: TLS) send, then one read
 *   hub_fetch --probe                              exit 0 (update.sh tells it from nothing)
 *
 * Options: -T sec, -X METHOD, -H "Name: value" (repeatable), --header-file f (one
 * "Name: value" per line), --data-file f (request body / TCP bytes), --max bytes,
 * --no-redirect, --ca file, --insecure, --pin HEX (the server key must have this SHA-256),
 * --pin-any (any key, reported), --tls (TCP), --read bytes (TCP: answer bytes to keep),
 * --status-file f (writes status=, fingerprint=, error= lines; for the caller), --connect-to
 * ADDR[,ADDR...] (numeric addresses to connect to instead of resolving the host, which still
 * names the server for SNI, the certificate and Host; the first request only, not a redirect).
 * Exit 0 on a 2xx answer (TCP: sent), 1 on any other answer or error (one line on stderr, the
 * output file removed unless --status-file is given), 2 on bad arguments.
 */
#include "http_client.h"
#include "tls_client.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAX_FILE_HEADERS 10

static int usage(void)
{
    fprintf(stderr, "usage: hub_fetch [-T sec] [-X method] [-H 'Name: value']... [--header-file f] [--data-file f]\n"
                    "         [--max bytes] [--no-redirect] [--ca file] [--insecure|--pin hex|--pin-any]\n"
                    "         [--status-file f] [--connect-to addr[,addr...]] -o out|- url\n"
                    "       hub_fetch --tcp [-T sec] [--tls] [--insecure|--pin hex|--pin-any] [--data-file f]\n"
                    "         [--read bytes] [--status-file f] [--connect-to addr[,addr...]] -o out|- host port\n");
    return 2;
}

static char *read_file(const char *path, size_t max, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    char *buf = malloc(max + 1);
    size_t n = buf ? fread(buf, 1, max + 1, f) : 0;
    fclose(f);
    if (!buf || n > max) {
        free(buf);
        return NULL;
    }
    buf[n] = '\0';
    *len = n;
    return buf;
}

static void write_status(const char *path, int status, const char *pin, const char *err)
{
    if (!path) return;
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "status=%d\nfingerprint=%s\nerror=%s\n", status, pin ? pin : "", err ? err : "");
    fclose(f);
}

int main(int argc, char **argv)
{
    http_req req;
    memset(&req, 0, sizeof(req));
    req.follow_redirects = 1;
    req.out_fd = -1;
    req.conn.verify = NET_TLS_VERIFY;
    req.conn.timeout_ms = 20000;
    const char *out = NULL, *arg1 = NULL, *arg2 = NULL, *status_path = NULL;
    const char *header_file = NULL, *data_file = NULL, *pin = NULL;
    int nh = 0, tcp = 0, tcp_tls = 0;
    size_t tcp_read = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (strcmp(a, "--probe") == 0) return 0;
        if (strcmp(a, "--insecure") == 0) { req.conn.verify = NET_TLS_INSECURE; continue; }
        if (strcmp(a, "--pin-any") == 0) { req.conn.verify = NET_TLS_PIN; pin = ""; continue; }
        if (strcmp(a, "--no-redirect") == 0) { req.follow_redirects = 0; continue; }
        if (strcmp(a, "--tcp") == 0) { tcp = 1; continue; }
        if (strcmp(a, "--tls") == 0) { tcp_tls = 1; continue; }
        if (a[0] == '-' && a[1] && !v) return usage();
        if (strcmp(a, "-T") == 0) { req.conn.timeout_ms = atoi(v) * 1000; i++; }
        else if (strcmp(a, "-X") == 0) { req.method = v; i++; }
        else if (strcmp(a, "-H") == 0) {
            if (nh >= HTTP_MAX_HEADERS - 1) return usage();
            req.headers[nh++] = v;
            i++;
        }
        else if (strcmp(a, "--header-file") == 0) { header_file = v; i++; }
        else if (strcmp(a, "--data-file") == 0) { data_file = v; i++; }
        else if (strcmp(a, "-o") == 0) { out = v; i++; }
        else if (strcmp(a, "--max") == 0) { req.max_body = (size_t)strtoul(v, NULL, 10); i++; }
        else if (strcmp(a, "--read") == 0) { tcp_read = (size_t)strtoul(v, NULL, 10); i++; }
        else if (strcmp(a, "--ca") == 0) { req.conn.ca_bundle = v; i++; }
        else if (strcmp(a, "--pin") == 0) { req.conn.verify = NET_TLS_PIN; pin = v; i++; }
        else if (strcmp(a, "--status-file") == 0) { status_path = v; i++; }
        else if (strcmp(a, "--connect-to") == 0) { req.conn.connect_addrs = v; i++; }
        else if (a[0] == '-' && a[1]) return usage();
        else if (!arg1) arg1 = a;
        else if (!arg2) arg2 = a;
        else return usage();
    }
    if (!out || !arg1 || (tcp && !arg2) || (!tcp && arg2) || req.conn.timeout_ms <= 0) return usage();
    req.conn.pin = pin;

    /* headers from a file: the caller's credentials stay out of the process list */
    char *hbuf = NULL;
    if (header_file) {
        size_t hlen = 0;
        hbuf = read_file(header_file, 8192, &hlen);
        if (!hbuf) {
            fprintf(stderr, "hub_fetch: cannot read %s\n", header_file);
            return 2;
        }
        int nf = 0;
        for (char *line = strtok(hbuf, "\n"); line; line = strtok(NULL, "\n")) {
            size_t l = strlen(line);
            if (l && line[l - 1] == '\r') line[l - 1] = '\0';
            if (!line[0]) continue;
            if (nh >= HTTP_MAX_HEADERS - 1 || ++nf > MAX_FILE_HEADERS) return usage();
            req.headers[nh++] = line;
        }
    }
    char *body = NULL;
    size_t body_len = 0;
    if (data_file) {
        body = read_file(data_file, 65536, &body_len);
        if (!body) {
            fprintf(stderr, "hub_fetch: cannot read %s\n", data_file);
            return 2;
        }
        req.body = body;
        req.body_len = body_len;
    }

    int to_stdout = strcmp(out, "-") == 0;
    int out_fd = to_stdout ? 1 : open(out, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (out_fd < 0) {
        fprintf(stderr, "hub_fetch: cannot write %s\n", out);
        return 1;
    }

    int rc;
    char err[160] = "";
    char fp[NET_PIN_HEX_LEN + 1] = "";
    int status = 0;
    if (tcp) {
        /* TCP: connect (TLS with --tls), send the data, keep up to --read bytes of the answer */
        req.conn.tls = tcp_tls;
        net_conn *c = net_conn_open(arg1, atoi(arg2), &req.conn, fp, err, sizeof(err));
        rc = c ? 0 : -1;
        if (c && body_len && net_conn_write(c, body, body_len) != 0) {
            snprintf(err, sizeof(err), "%s", net_conn_error(c));
            rc = -1;
        }
        if (c && rc == 0 && tcp_read > 0) {
            char buf[1024];
            size_t want = tcp_read < sizeof(buf) ? tcp_read : sizeof(buf);
            ssize_t n = net_conn_read(c, buf, want);
            if (n > 0 && write(out_fd, buf, (size_t)n) != n) rc = -1;
        }
        net_conn_close(c);
    } else {
        req.out_fd = out_fd;
        http_resp resp;
        rc = http_request(arg1, &req, &resp);
        status = resp.status;
        snprintf(fp, sizeof(fp), "%s", resp.pin);
        snprintf(err, sizeof(err), "%s", resp.err);
        if (rc == 0 && resp.truncated) {
            rc = -1;
            snprintf(err, sizeof(err), "answer larger than %lu bytes", (unsigned long)req.max_body);
        }
        if (rc == 0 && (status < 200 || status > 299)) {
            rc = -1;
            snprintf(err, sizeof(err), "HTTP %d", status);
        }
    }
    if (!to_stdout && close(out_fd) != 0 && rc == 0) {
        rc = -1;
        snprintf(err, sizeof(err), "cannot write %s", out);
    }
    write_status(status_path, status, fp, rc == 0 ? "" : err);
    free(hbuf);
    free(body);
    if (rc != 0) {
        fprintf(stderr, "hub_fetch: %s: %s\n", arg1, err);
        if (!to_stdout && !status_path) unlink(out);
        return 1;
    }
    return 0;
}
