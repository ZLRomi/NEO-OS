#include "http.h"
#include "net.h"
#include "tls.h"
#include "printk.h"
#include "serial.h"
#include <stdint.h>
#include <stddef.h>

#define HTTP_TIMEOUT 300
#define HTTP_PORT    80
#define HTTPS_PORT   443

static void int_to_str_local(int v, char* out) {
    if (v == 0) { out[0] = '0'; out[1] = '\0'; return; }
    int neg = 0;
    if (v < 0) { neg = 1; v = -v; }
    char buf[12];
    int i = 0;
    while (v > 0) { buf[i++] = '0' + (v % 10); v /= 10; }
    int j = 0;
    if (neg) out[j++] = '-';
    while (i > 0) out[j++] = buf[--i];
    out[j] = '\0';
}

static void mem_copy(void* d, const void* s, int n) {
    uint8_t* dst = (uint8_t*)d;
    const uint8_t* src = (const uint8_t*)s;
    for (int i = 0; i < n; i++) dst[i] = src[i];
}

static int str_len(const char* s) {
    int n = 0;
    while (s[n]) n++;
    return n;
}

static char* find_str(const char* s, const char* sub) {
    int sl = str_len(sub);
    for (int i = 0; s[i]; i++) {
        int j;
        for (j = 0; j < sl && s[i + j] == sub[j]; j++);
        if (j == sl) return (char*)&s[i];
    }
    return 0;
}

static int html_tag_is(const char* tag, const char* name) {
    int i = 0;
    while (name[i]) {
        char a = tag[i];
        char b = name[i];
        if (a == '\0') return 0;
        if (a != b && a != b - 32) return 0;
        i++;
    }
    return tag[i] == '\0' || tag[i] == ' ' || tag[i] == '>' || tag[i] == '/';
}

void http_html_to_text(const char* html, char* out, int max) {
    int in_tag = 0;
    int in_script = 0;
    int in_style = 0;
    int tag_i = 0;
    char tag[32];
    int p = 0;
    int prev_space = 1;
    int cur_style = 0;
    (void)cur_style;

    for (int i = 0; html[i] && p < max - 2; i++) {
        char c = html[i];

        if (in_script) {
            if (c == '<' && html_tag_is(html + i + 1, "/script")) {
                in_script = 0;
                while (html[i] && html[i] != '>') i++;
            }
            continue;
        }
        if (in_style) {
            if (c == '<' && html_tag_is(html + i + 1, "/style")) {
                in_style = 0;
                while (html[i] && html[i] != '>') i++;
            }
            continue;
        }

        if (c == '<') {
            in_tag = 1;
            tag_i = 0;
            tag[0] = '\0';
            continue;
        }
        if (in_tag) {
            if (c == '>') {
                in_tag = 0;
                tag[tag_i] = '\0';

                if (html_tag_is(tag, "script")) {
                    in_script = 1;
                    continue;
                }
                if (html_tag_is(tag, "style")) {
                    in_style = 1;
                    continue;
                }
                if (html_tag_is(tag, "br")) {
                    out[p++] = '\n';
                    prev_space = 1;
                    continue;
                }
                if (html_tag_is(tag, "h1")) {
                    if (p > 0 && out[p - 1] != '\n') out[p++] = '\n';
                    if (p < max - 2) { out[p++] = '\x01'; out[p++] = 1; }
                    prev_space = 1; continue;
                }
                if (html_tag_is(tag, "h2")) {
                    if (p > 0 && out[p - 1] != '\n') out[p++] = '\n';
                    if (p < max - 2) { out[p++] = '\x01'; out[p++] = 2; }
                    prev_space = 1; continue;
                }
                if (html_tag_is(tag, "h3") || html_tag_is(tag, "h4") ||
                    html_tag_is(tag, "h5") || html_tag_is(tag, "h6")) {
                    if (p > 0 && out[p - 1] != '\n') out[p++] = '\n';
                    if (p < max - 2) { out[p++] = '\x01'; out[p++] = 3; }
                    prev_space = 1; continue;
                }
                if (html_tag_is(tag, "p") || html_tag_is(tag, "div") ||
                    html_tag_is(tag, "tr")) {
                    if (p > 0 && out[p - 1] != '\n') {
                        out[p++] = '\n';
                        if (p < max - 2 && out[p - 1] != '\n') out[p++] = '\n';
                    }
                    if (p < max - 2) { out[p++] = '\x01'; out[p++] = 0x10; }
                    cur_style = 0;
                    prev_space = 1;
                    continue;
                }
                if (html_tag_is(tag, "b") || html_tag_is(tag, "strong")) {
                    if (p < max - 2) { out[p++] = '\x01'; out[p++] = 5; }
                    continue;
                }
                if (html_tag_is(tag, "i") || html_tag_is(tag, "em")) {
                    if (p < max - 2) { out[p++] = '\x01'; out[p++] = 6; }
                    continue;
                }
                if (html_tag_is(tag, "blockquote") || html_tag_is(tag, "quote")) {
                    if (p > 0 && out[p - 1] != '\n') out[p++] = '\n';
                    if (p < max - 2) { out[p++] = '\x01'; out[p++] = 8; }
                    prev_space = 1; continue;
                }
                if (html_tag_is(tag, "code") || html_tag_is(tag, "pre")) {
                    if (p < max - 2) { out[p++] = '\x01'; out[p++] = 7; }
                    continue;
                }
                if (html_tag_is(tag, "li")) {
                    if (p > 0 && out[p - 1] != '\n') out[p++] = '\n';
                    out[p++] = '\x01'; out[p++] = 9;
                    out[p++] = ' '; out[p++] = ' ';
                    prev_space = 0;
                    continue;
                }
                if (tag[0] == '/') {
                    if (html_tag_is(tag + 1, "h1") || html_tag_is(tag + 1, "h2") ||
                        html_tag_is(tag + 1, "h3") || html_tag_is(tag + 1, "h4") ||
                        html_tag_is(tag + 1, "h5") || html_tag_is(tag + 1, "h6") ||
                        html_tag_is(tag + 1, "p") || html_tag_is(tag + 1, "div") ||
                        html_tag_is(tag + 1, "tr") || html_tag_is(tag + 1, "blockquote")) {
                        if (p > 0 && out[p - 1] != '\n') out[p++] = '\n';
                        if (p < max - 2) { out[p++] = '\x01'; out[p++] = 0x10; }
                        cur_style = 0;
                        prev_space = 1;
                        continue;
                    }
                    if (html_tag_is(tag + 1, "b") || html_tag_is(tag + 1, "strong") ||
                        html_tag_is(tag + 1, "i") || html_tag_is(tag + 1, "em") ||
                        html_tag_is(tag + 1, "code") || html_tag_is(tag + 1, "pre")) {
                        if (p < max - 2) { out[p++] = '\x01'; out[p++] = 0x10; }
                        continue;
                    }
                }
                continue;
            } else {
                if (tag_i < 31) {
                    tag[tag_i++] = c;
                    tag[tag_i] = '\0';
                }
            }
            continue;
        }

        if (c == '\r') continue;
        if (c == '\n' || c == ' ' || c == '\t') {
            if (!prev_space) {
                out[p++] = ' ';
                prev_space = 1;
            }
            continue;
        }
        out[p++] = c;
        prev_space = 0;
    }
    out[p] = '\0';
}

static int __attribute__((unused)) str_cmp_n(const char* a, const char* b, int n) {
    for (int i = 0; i < n; i++) {
        if (a[i] != b[i]) return a[i] - b[i];
        if (a[i] == '\0') return 0;
    }
    return 0;
}

static int str_prefix(const char* s, const char* pre) {
    while (*pre) {
        if (*s != *pre) return 0;
        s++;
        pre++;
    }
    return 1;
}

static int str_case_prefix(const char* s, const char* pre) {
    while (*pre) {
        char a = *s;
        char b = *pre;
        if (a >= 'A' && a <= 'Z') a += 32;
        if (b >= 'A' && b <= 'Z') b += 32;
        if (a != b) return 0;
        s++;
        pre++;
    }
    return 1;
}

static char* find_header(char* raw, const char* name) {
    int n = str_len(name);
    for (char* p = raw; *p; p++) {
        int at_line_start = (p == raw) || (p[-1] == '\n');
        if (at_line_start && str_case_prefix(p, name) && p[n] == ':') {
            p += n + 1;
            while (*p == ' ' || *p == '\t') p++;
            return p;
        }
    }
    return 0;
}

static void parse_url(const char* url, char* host, int host_max,
                      char* path, int path_max) {
    host[0] = '\0';
    path[0] = '\0';
    const char* p = url;
    if (str_prefix(p, "http://")) p += 7;
    else if (str_prefix(p, "https://")) p += 8;
    if (p[0] == '/' && p[1] == '/') p += 2;

    int i = 0;
    while (p[i] && p[i] != '/' && p[i] != '?' && i < host_max - 1) {
        host[i] = p[i];
        i++;
    }
    host[i] = '\0';

    if (p[i]) {
        int j = 0;
        while (p[i] && j < path_max - 1) {
            path[j++] = p[i++];
        }
        path[j] = '\0';
    } else {
        path[0] = '/';
        path[1] = '\0';
    }
}

static void str_copy_n(char* d, const char* s, int max) {
    int i = 0;
    while (s[i] && i < max - 1) { d[i] = s[i]; i++; }
    d[i] = '\0';
}

static void str_cat_n(char* d, const char* s, int max) {
    int i = 0;
    while (d[i] && i < max - 1) i++;
    int j = 0;
    while (s[j] && i < max - 1) { d[i++] = s[j++]; }
    d[i] = '\0';
}

static void http_resolve_url2(const char* scheme, const char* cur_host, const char* cur_path,
                              const char* loc, char* out, int out_max) {
    out[0] = '\0';
    if (!loc || !*loc) return;

    if (str_prefix(loc, "http://") || str_prefix(loc, "https://")) {
        str_copy_n(out, loc, out_max);
        return;
    }
    if (loc[0] == '/' && loc[1] == '/') {
        str_cat_n(out, scheme, out_max);
        str_cat_n(out, ":", out_max);
        str_cat_n(out, loc, out_max);
        return;
    }

    char base[256];
    str_copy_n(base, cur_path, sizeof(base));
    int dir_end = 0;
    for (int i = 0; base[i]; i++) {
        if (base[i] == '/') dir_end = i + 1;
    }
    base[dir_end] = '\0';

    if (loc[0] == '/') {
        str_cat_n(out, scheme, out_max);
        str_cat_n(out, "://", out_max);
        str_cat_n(out, cur_host, out_max);
        str_cat_n(out, loc, out_max);
    } else {
        str_cat_n(out, scheme, out_max);
        str_cat_n(out, "://", out_max);
        str_cat_n(out, cur_host, out_max);
        str_cat_n(out, base, out_max);
        str_cat_n(out, loc, out_max);
    }
}

void http_resolve_url(const char* cur_host, const char* cur_path,
                      const char* loc, char* out, int out_max) {
    http_resolve_url2("http", cur_host, cur_path, loc, out, out_max);
}

void http_resolve_url_full(const char* cur_url, const char* loc, char* out, int out_max) {
    const char* scheme = "http";
    if (str_prefix(cur_url, "https://") || str_prefix(cur_url, "HTTPS://")) {
        scheme = "https";
    }
    char host[128], path[256];
    parse_url(cur_url, host, sizeof(host), path, sizeof(path));
    http_resolve_url2(scheme, host, path, loc, out, out_max);
}

static int parse_status_code(const char* raw) {
    const char* p = raw;
    while (*p && *p != ' ') p++;
    if (!*p) return 0;
    p++;
    int code = 0;
    for (int i = 0; i < 3 && p[i] >= '0' && p[i] <= '9'; i++) {
        code = code * 10 + (p[i] - '0');
    }
    return code;
}

static int url_is_https(const char* url) {
    return str_prefix(url, "https://");
}

int http_fetch(const char* initial_url, char* raw_out, int raw_max,
               char* text_out, int text_max) {
    static char url[1024];
    int ui = 0;
    while (initial_url[ui] && ui < 1023) { url[ui] = initial_url[ui]; ui++; }
    url[ui] = '\0';

    static char seen[6][1024];
    int seen_count = 0;

    for (int depth = 0; depth < 6; depth++) {
        int using_tls = url_is_https(url);

        for (int s = 0; s < seen_count; s++) {
            int same = 1;
            for (int k = 0; url[k] || seen[s][k]; k++) {
                if (url[k] != seen[s][k]) { same = 0; break; }
            }
            if (same) {
                serial_write("HTTP: redirect loop\n");
                return -1;
            }
        }
        if (seen_count < 6) {
            int k = 0;
            while (url[k] && k < 1023) { seen[seen_count][k] = url[k]; k++; }
            seen[seen_count][k] = '\0';
            seen_count++;
        }

        char host[128], path[256];
        parse_url(url, host, sizeof(host), path, sizeof(path));

        uint32_t ip = net_parse_ip(host);
        if (!ip) {
            ip = net_dns_resolve(host, HTTP_TIMEOUT);
            if (!ip) {
                serial_write("HTTP: DNS failed\n");
                return -1;
            }
        }

        char ipbuf[16];
        net_ip_to_str(ip, ipbuf);
        serial_write("HTTP: connect ");
        serial_write(ipbuf);
        serial_write("\n");

        int tls_sid = 0;
        uint16_t port = 0;
        if (using_tls) {
            tls_sid = tls_connect(ip, HTTPS_PORT, host, HTTP_TIMEOUT);
            if (!tls_sid) {
                serial_write("HTTP: TLS connect failed\n");
                return -1;
            }
        } else {
            port = tcp_connect(ip, HTTP_PORT, HTTP_TIMEOUT);
            if (!port) {
                serial_write("HTTP: TCP connect failed\n");
                return -1;
            }
        }

        char req[1024];
        char* p = req;
        const char* m = "GET ";
        while (*m) *p++ = *m++;
        m = path;
        while (*m) *p++ = *m++;
        m = " HTTP/1.0\r\nHost: ";
        while (*m) *p++ = *m++;
        m = host;
        while (*m) *p++ = *m++;
        m = "\r\nUser-Agent: NEO-OS-Browser/1.0\r\nConnection: close\r\n\r\n";
        while (*m) *p++ = *m++;
        *p = '\0';

        int req_len = p - req;
        if (using_tls) {
            if (tls_send(tls_sid, (const uint8_t*)req, (uint16_t)req_len) != req_len) {
                tls_close(tls_sid);
                return -1;
            }
        } else {
            if (tcp_send(port, req, (uint16_t)req_len) != req_len) {
                tcp_close(port);
                return -1;
            }
        }

        static char raw[65536];
        int total = 0;
        while (total < (int)sizeof(raw) - 1) {
            int n;
            int recv_timeout = (total == 0) ? HTTP_TIMEOUT : 80;
            if (using_tls) {
                n = tls_recv(tls_sid, (uint8_t*)(raw + total), (uint16_t)(sizeof(raw) - 1 - total), recv_timeout);
            } else {
                n = tcp_recv(port, raw + total, (uint16_t)(sizeof(raw) - 1 - total), recv_timeout);
            }
            if (n <= 0) break;
            total += n;
        }
        raw[total] = '\0';
        if (using_tls) tls_close(tls_sid);
        else tcp_close(port);

        int status = parse_status_code(raw);
        if (status >= 300 && status < 400) {
            char* loc = find_header(raw, "Location");
            if (loc) {
                char loc_buf[512];
                int i = 0;
                while (loc[i] && loc[i] != '\r' && loc[i] != '\n' && i < 511) {
                    loc_buf[i] = loc[i];
                    i++;
                }
                loc_buf[i] = '\0';
                char new_url[1024];
                http_resolve_url2(using_tls ? "https" : "http", host, path, loc_buf, new_url, sizeof(new_url));
                if (new_url[0]) {
                    serial_write("HTTP: redirect to ");
                    serial_write(new_url);
                    serial_write("\n");
                    int j = 0;
                    while (new_url[j] && j < 1023) { url[j] = new_url[j]; j++; }
                    url[j] = '\0';
                    continue;
                }
            }
            serial_write("HTTP: redirect without Location\n");
            return -1;
        }

        if (status < 200 || status >= 400) {
            serial_write("HTTP: status ");
            char sbuf[8];
            int_to_str_local(status, sbuf);
            serial_write(sbuf);
            serial_write("\n");
            return -1;
        }

        char* body = find_str(raw, "\r\n\r\n");
        if (!body) {
            serial_write("HTTP: no body\n");
            return -1;
        }
        body += 4;

        int result = 0;
        if (raw_out && raw_max > 0) {
            int len = str_len(body);
            if (len >= raw_max) len = raw_max - 1;
            mem_copy(raw_out, body, len);
            raw_out[len] = '\0';
            result = len;
        }
        if (text_out && text_max > 0) {
            http_html_to_text(body, text_out, text_max);
            result = str_len(text_out);
        }
        return result;
    }

    serial_write("HTTP: too many redirects\n");
    return -1;
}

int http_get(const char* host, const char* path, char* out, int max_len, int text_only) {
    if (!host || !path || !out || max_len <= 0) return -1;
    char url[1024];
    char* p = url;
    const char* m = "http://";
    while (*m) *p++ = *m++;
    m = host;
    while (*m) *p++ = *m++;
    m = path;
    while (*m) *p++ = *m++;
    *p = '\0';
    if (text_only) {
        return http_fetch(url, NULL, 0, out, max_len);
    } else {
        return http_fetch(url, out, max_len, NULL, 0);
    }
}

int http_get_html(const char* host, const char* path, char* raw_out, int raw_max,
                  char* text_out, int text_max) {
    if (!host || !path) return -1;
    char url[1024];
    char* p = url;
    const char* m = "http://";
    while (*m) *p++ = *m++;
    m = host;
    while (*m) *p++ = *m++;
    m = path;
    while (*m) *p++ = *m++;
    *p = '\0';
    return http_fetch(url, raw_out, raw_max, text_out, text_max);
}

int http_download_file(const char* url, const char* save_path,
                       int (*progress_cb)(int received, int total, void* ctx),
                       void* ctx) {
    if (!url || !save_path) return -1;

    extern int fat32_create(const char* path);
    extern int fat32_open(const char* path, int flags);
    extern int fat32_write(int fd, const void* buf, int count);
    extern void fat32_close(int fd);

    static char cur_url[1024];
    int ui = 0;
    while (url[ui] && ui < 1023) { cur_url[ui] = url[ui]; ui++; }
    cur_url[ui] = '\0';

    for (int depth = 0; depth < 6; depth++) {
        int using_tls = url_is_https(cur_url);
        char host[128], path[256];
        parse_url(cur_url, host, sizeof(host), path, sizeof(path));

        uint32_t ip = net_parse_ip(host);
        if (!ip) {
            ip = net_dns_resolve(host, HTTP_TIMEOUT);
            if (!ip) {
                serial_write("DL: DNS failed\n");
                return -1;
            }
        }

        int tls_sid = 0;
        uint16_t port = 0;
        if (using_tls) {
            tls_sid = tls_connect(ip, HTTPS_PORT, host, HTTP_TIMEOUT);
            if (!tls_sid) {
                serial_write("DL: TLS connect failed\n");
                return -1;
            }
        } else {
            port = tcp_connect(ip, HTTP_PORT, HTTP_TIMEOUT);
            if (!port) {
                serial_write("DL: TCP connect failed\n");
                return -1;
            }
        }

        char req[1024];
        char* p = req;
        const char* m = "GET ";
        while (*m) *p++ = *m++;
        m = path;
        while (*m) *p++ = *m++;
        m = " HTTP/1.0\r\nHost: ";
        while (*m) *p++ = *m++;
        m = host;
        while (*m) *p++ = *m++;
        m = "\r\nUser-Agent: NEO-OS-Downloader/1.0\r\nConnection: close\r\n\r\n";
        while (*m) *p++ = *m++;
        *p = '\0';

        int req_len = p - req;
        if (using_tls) {
            if (tls_send(tls_sid, (const uint8_t*)req, (uint16_t)req_len) != req_len) {
                tls_close(tls_sid);
                return -1;
            }
        } else {
            if (tcp_send(port, req, (uint16_t)req_len) != req_len) {
                tcp_close(port);
                return -1;
            }
        }

        static uint8_t header_buf[4096];
        int htotal = 0;
        int header_end = -1;
        int content_total = -1;
        int received = 0;
        int fd = -1;
        int got_header_status = 0;
        int redirect = 0;
        char new_url[1024];
        new_url[0] = '\0';

        while (header_end < 0 && htotal < (int)sizeof(header_buf) - 1) {
            int n;
            int recv_timeout = (htotal == 0) ? HTTP_TIMEOUT : 80;
            if (using_tls) {
                n = tls_recv(tls_sid, header_buf + htotal,
                             (uint16_t)(sizeof(header_buf) - 1 - htotal), recv_timeout);
            } else {
                n = tcp_recv(port, (char*)(header_buf + htotal),
                             (uint16_t)(sizeof(header_buf) - 1 - htotal), recv_timeout);
            }
            if (n <= 0) break;
            htotal += n;
            for (int i = 0; i + 3 <= htotal; i++) {
                if (header_buf[i] == '\r' && header_buf[i+1] == '\n' &&
                    header_buf[i+2] == '\r' && header_buf[i+3] == '\n') {
                    header_end = i + 4;
                    break;
                }
            }
        }

        if (header_end < 0) {
            if (using_tls) tls_close(tls_sid);
            else tcp_close(port);
            return -1;
        }

        header_buf[htotal] = '\0';
        {
            int status = parse_status_code((const char*)header_buf);
            if (status >= 300 && status < 400) {
                char* loc = find_header((char*)header_buf, "Location");
                if (loc) {
                    char loc_buf[512];
                    int i = 0;
                    while (loc[i] && loc[i] != '\r' && loc[i] != '\n' && i < 511) {
                        loc_buf[i] = loc[i];
                        i++;
                    }
                    loc_buf[i] = '\0';
                    http_resolve_url2(using_tls ? "https" : "http", host, path, loc_buf, new_url, sizeof(new_url));
                    redirect = 1;
                }
            }
            if (!redirect && (status < 200 || status >= 400)) {
                if (using_tls) tls_close(tls_sid);
                else tcp_close(port);
                return -1;
            }
            if (!redirect) {
                char* clen = find_header((char*)header_buf, "Content-Length");
                if (clen) {
                    int v = 0;
                    int i = 0;
                    while (clen[i] >= '0' && clen[i] <= '9') {
                        v = v * 10 + (clen[i] - '0');
                        i++;
                    }
                    content_total = v;
                }
            }
            got_header_status = 1;
        }

        if (redirect) {
            if (using_tls) tls_close(tls_sid);
            else tcp_close(port);
            if (!new_url[0]) return -1;
            int j = 0;
            while (new_url[j] && j < 1023) { cur_url[j] = new_url[j]; j++; }
            cur_url[j] = '\0';
            continue;
        }

        (void)got_header_status;
        fd = fat32_create(save_path);
        if (fd < 0) {
            fd = fat32_open(save_path, 2);
        }
        if (fd < 0) {
            if (using_tls) tls_close(tls_sid);
            else tcp_close(port);
            return -1;
        }

        int body_in_buf = htotal - header_end;
        if (body_in_buf > 0) {
            int w = fat32_write(fd, header_buf + header_end, body_in_buf);
            if (w < 0) w = 0;
            received += w;
            if (progress_cb) {
                if (progress_cb(received, content_total, ctx) != 0) {
                    fat32_close(fd);
                    if (using_tls) tls_close(tls_sid);
                    else tcp_close(port);
                    return -2;
                }
            }
        }

        static uint8_t body_buf[4096];
        int done = 0;
        while (!done) {
            int n;
            int recv_timeout = 80;
            if (using_tls) {
                n = tls_recv(tls_sid, body_buf, (uint16_t)sizeof(body_buf), recv_timeout);
            } else {
                n = tcp_recv(port, (char*)body_buf, (uint16_t)sizeof(body_buf), recv_timeout);
            }
            if (n <= 0) break;
            int w = fat32_write(fd, body_buf, n);
            if (w < 0) w = 0;
            received += w;
            if (progress_cb) {
                if (progress_cb(received, content_total, ctx) != 0) {
                    fat32_close(fd);
                    if (using_tls) tls_close(tls_sid);
                    else tcp_close(port);
                    return -2;
                }
            }
            if (content_total > 0 && received >= content_total) done = 1;
        }

        fat32_close(fd);
        if (using_tls) tls_close(tls_sid);
        else tcp_close(port);
        return received;
    }

    serial_write("DL: too many redirects\n");
    return -1;
}
