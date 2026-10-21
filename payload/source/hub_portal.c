#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define WPA_CONF "/etc/wpa_supplicant.conf"

static const char *HTML_FORM =
    "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nConnection: close\r\n\r\n"
    "<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Harmony Recovery Setup</title><style>"
    "body{font-family:system-ui,-apple-system,sans-serif;background:#0d1117;color:#c9d1d9;margin:2rem auto;max-width:28rem;padding:0 1rem}"
    "h1{font-size:1.4rem;color:#58a6ff;margin-bottom:0.25rem}.sub{color:#8b949e;font-size:0.9rem;margin-bottom:1.5rem}"
    "label{display:block;margin:1rem 0 0.35rem;font-size:0.85rem;color:#8b949e}"
    "input[type=text],input[type=password]{font-size:1rem;padding:0.6rem;width:100%;box-sizing:border-box;background:#161b22;border:1px solid #30363d;border-radius:6px;color:#fff}"
    "button{margin-top:1.5rem;padding:0.75rem 1.25rem;font-size:1rem;font-weight:600;background:#238636;color:#fff;border:0;border-radius:6px;width:100%;cursor:pointer}"
    "button:hover{background:#2ea043}"
    ".check{display:flex;align-items:center;gap:0.5rem;margin-top:0.75rem;font-size:0.9rem}"
    "</style></head><body>"
    "<h1>Harmony Hub Setup</h1><div class='sub'>Connect hub to your local Wi-Fi</div>"
    "<form method='POST' action='/wifi'>"
    "<label>Wi-Fi Network (SSID)</label><input type='text' name='ssid' required autofocus autocomplete='off'>"
    "<label>Password</label><input type='password' name='psk'>"
    "<div class='check'><input type='checkbox' name='hidden' value='1' id='h'><label for='h' style='margin:0'>Hidden Network</label></div>"
    "<button type='submit'>Save and Connect</button></form></body></html>";

static const char *HTML_SAVED =
    "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nConnection: close\r\n\r\n"
    "<!doctype html><html><body style='font-family:sans-serif;background:#0d1117;color:#58a6ff;text-align:center;padding:3rem'>"
    "<h2>Wi-Fi Saved!</h2><p style='color:#8b949e'>Hub is now connecting to Wi-Fi. LED will turn green when ready.</p></body></html>";

static void url_decode(char *dst, const char *src, size_t dst_len) {
    size_t d = 0;
    while (*src && d < dst_len - 1) {
        if (*src == '+') {
            dst[d++] = ' ';
            src++;
        } else if (*src == '%' && isxdigit((unsigned char)src[1]) && isxdigit((unsigned char)src[2])) {
            char hex[3] = { src[1], src[2], '\0' };
            dst[d++] = (char)strtol(hex, NULL, 16);
            src += 3;
        } else {
            dst[d++] = *src++;
        }
    }
    dst[d] = '\0';
}

static void extract_param(const char *body, const char *name, char *out, size_t outlen) {
    out[0] = '\0';
    if (!body || !name) return;
    size_t nlen = strlen(name);
    const char *p = body;
    while (p && *p) {
        if (strncmp(p, name, nlen) == 0 && p[nlen] == '=') {
            const char *val_start = p + nlen + 1;
            const char *val_end = strchr(val_start, '&');
            size_t vlen = val_end ? (size_t)(val_end - val_start) : strlen(val_start);
            char enc[256] = {0};
            if (vlen >= sizeof(enc)) vlen = sizeof(enc) - 1;
            strncpy(enc, val_start, vlen);
            url_decode(out, enc, outlen);
            return;
        }
        p = strchr(p, '&');
        if (p) p++;
    }
}

static int save_wifi_config(const char *ssid, const char *psk, int hidden) {
    FILE *f = fopen(WPA_CONF, "w");
    if (!f) return -1;

    fprintf(f, "ctrl_interface=/var/run/wpa_supplicant\n");
    fprintf(f, "update_config=1\n\n");
    fprintf(f, "network={\n");
    fprintf(f, "    ssid=\"%s\"\n", ssid);
    if (psk && psk[0]) {
        fprintf(f, "    psk=\"%s\"\n", psk);
    } else {
        fprintf(f, "    key_mgmt=NONE\n");
    }
    if (hidden) {
        fprintf(f, "    scan_ssid=1\n");
    }
    fprintf(f, "}\n");
    fclose(f);
    return 0;
}

int main(int argc, char **argv) {
    int port = (argc > 1) ? atoi(argv[1]) : 80;
    if (port <= 0) port = 80;

    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) { perror("socket"); return 1; }

    int opt = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        perror("bind");
        close(srv);
        return 1;
    }

    if (listen(srv, 4) != 0) {
        perror("listen");
        close(srv);
        return 1;
    }

    printf("[+] Wi-Fi recovery portal listening on port %d...\n", port);

    while (1) {
        struct sockaddr_in client;
        socklen_t clen = sizeof(client);
        int cfd = accept(srv, (struct sockaddr *)&client, &clen);
        if (cfd < 0) continue;

        char buf[2048] = {0};
        ssize_t n = recv(cfd, buf, sizeof(buf) - 1, 0);
        if (n <= 0) { close(cfd); continue; }

        if (strncmp(buf, "GET ", 4) == 0) {
            send(cfd, HTML_FORM, strlen(HTML_FORM), 0);
            close(cfd);
            continue;
        }

        if (strncmp(buf, "POST /wifi", 10) == 0) {
            char *body = strstr(buf, "\r\n\r\n");
            if (body) body += 4;
            char ssid[128] = {0}, psk[128] = {0}, hid_str[16] = {0};
            extract_param(body, "ssid", ssid, sizeof(ssid));
            extract_param(body, "psk", psk, sizeof(psk));
            extract_param(body, "hidden", hid_str, sizeof(hid_str));

            if (ssid[0]) {
                printf("[+] Received Wi-Fi credentials for SSID: '%s'\n", ssid);
                save_wifi_config(ssid, psk, hid_str[0] == '1');
                send(cfd, HTML_SAVED, strlen(HTML_SAVED), 0);
                close(cfd);

                /* Trigger background sync and reboot */
                sync();
                sleep(2);
                system("reboot");
                break;
            }
        }

        /* Default fallback */
        send(cfd, HTML_FORM, strlen(HTML_FORM), 0);
        close(cfd);
    }

    close(srv);
    return 0;
}
