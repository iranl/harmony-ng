#include "bt_status_json.h"

/* A JSON string: quotes, backslashes and control characters escaped (a host name may hold
 * any of them; written raw they made the whole file unparsable) */
static void put_str(FILE *f, const char *s) {
    fputc('"', f);
    for (const unsigned char *p = (const unsigned char *)(s ? s : ""); *p; p++) {
        if (*p == '"' || *p == '\\') {
            fputc('\\', f);
            fputc(*p, f);
        } else if (*p < 0x20) {
            fprintf(f, "\\u%04x", *p);
        } else {
            fputc(*p, f);
        }
    }
    fputc('"', f);
}

void bt_status_write(FILE *f, const bt_status_view_t *v) {
    fprintf(f, "{\n");
    fprintf(f, "  \"backend\": \"btstack\",\n");
    fprintf(f, "  \"pid\": %d,\n", v->pid);
    fprintf(f, "  \"state\": "); put_str(f, v->state); fprintf(f, ",\n");
    fprintf(f, "  \"target\": "); put_str(f, v->target); fprintf(f, ",\n");
    fprintf(f, "  \"last_button\": "); put_str(f, v->last_button); fprintf(f, ",\n");
    fprintf(f, "  \"last_seen\": %ld,\n", v->last_seen);
    fprintf(f, "  \"remote\": {\n");
    fprintf(f, "    \"connected\": %s,\n", v->remote_connected ? "true" : "false");
    fprintf(f, "    \"addr\": "); put_str(f, v->remote_addr); fprintf(f, "\n");
    fprintf(f, "  },\n");
    fprintf(f, "  \"host\": {\n");
    fprintf(f, "    \"connected\": %s,\n", v->host_connected ? "true" : "false");
    fprintf(f, "    \"addr\": "); put_str(f, v->host_addr); fprintf(f, ",\n");
    fprintf(f, "    \"name\": "); put_str(f, v->host_name); fprintf(f, ",\n");
    fprintf(f, "    \"pairing\": %s\n", v->host_pairing ? "true" : "false");
    fprintf(f, "  },\n");
    fprintf(f, "  \"hosts\": [\n");
    for (int i = 0; i < v->nhosts && i < BT_STATUS_MAX_HOSTS; i++) {
        const bt_status_host_t *h = &v->hosts[i];
        if (i) fprintf(f, ",\n");
        fprintf(f, "    {\"addr\": "); put_str(f, h->addr);
        fprintf(f, ", \"name\": "); put_str(f, h->name);
        fprintf(f, ", \"connected\": %s, \"paired\": %s, \"cid\": %u}",
                h->connected ? "true" : "false", h->paired ? "true" : "false", (unsigned)h->cid);
    }
    fprintf(f, "\n  ]\n");
    fprintf(f, "}\n");
}
