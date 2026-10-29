/* 极简 HTTP 客户端：只用 socket + select，不依赖 curl/OpenSSL。
 *
 * 只支持 http://（校园网门户都在内网明文：实测 http://10.0.0.1/ + :801）。
 * 支持 Content-Length 与 chunked 两种长度形式；连接失败/超时都有明确错误串。
 */
#include "cnk.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
typedef SOCKET sock_t;
#define SOCK_INVALID INVALID_SOCKET
#define sock_close closesocket
#else
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
typedef int sock_t;
#define SOCK_INVALID (-1)
#define sock_close close
#endif

/* ---------------- 时间 ---------------- */

void net_init(void) {
#ifdef _WIN32
    static int done = 0;
    WSADATA wsa;
    if (!done) {
        WSAStartup(0x0202, &wsa);
        done = 1;
    }
#else
    /* 对端提前关闭时 send() 会触发 SIGPIPE，默认动作是**杀掉进程**。
     * 守护进程要长期运行，绝不能因为一次对端 RST 就死掉。 */
    signal(SIGPIPE, SIG_IGN);
#endif
}

long long now_ms(void) {
#ifdef _WIN32
    return (long long)GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
#endif
}

void sleep_ms(int ms) {
    if (ms <= 0) return;
#ifdef _WIN32
    Sleep((DWORD)ms);
#else
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
#endif
}

/* ---------------- 字符串小工具 ---------------- */

static void set_err(char *err, size_t cap, const char *msg) {
    if (err && cap) {
        strncpy(err, msg, cap - 1);
        err[cap - 1] = '\0';
    }
}

static const char *ci_find(const char *hay, const char *needle) {
    size_t nl = strlen(needle);
    const char *p;
    if (!nl) return hay;
    for (p = hay; *p; ++p) {
        size_t i;
        for (i = 0; i < nl; ++i) {
            if (!p[i]) return NULL;
            if (tolower((unsigned char)p[i]) != tolower((unsigned char)needle[i])) break;
        }
        if (i == nl) return p;
    }
    return NULL;
}

/* ---------------- URL 解析 ---------------- */

int url_split(const char *url, char *scheme, size_t scheme_cap,
              char *host, size_t host_cap, char *port, size_t port_cap,
              char *path, size_t path_cap) {
    const char *sep, *h, *hend, *slash, *cport = NULL;
    size_t sl;

    if (!url) return -1;
    sep = strstr(url, "://");
    if (!sep) return -1;
    sl = (size_t)(sep - url);
    if (sl == 0 || sl + 1 > scheme_cap) return -1;
    if (scheme) {
        memcpy(scheme, url, sl);
        scheme[sl] = '\0';
    }
    h = sep + 3;
    slash = strchr(h, '/');
    hend = slash ? slash : h + strlen(h);

    /* userinfo：取最后一个 '@'（RFC 3986 中 '@' 不属于 userinfo 字符集） */
    {
        const char *at = NULL, *q;
        for (q = h; q < hend; ++q)
            if (*q == '@') at = q;
        if (at) h = at + 1;
    }

    /* 端口：方括号 IPv6 之后才找冒号 */
    if (h < hend && *h == '[') {
        const char *rb = memchr(h, ']', (size_t)(hend - h));
        if (!rb) return -1;
        if (rb + 1 < hend && rb[1] == ':') cport = rb + 1;
    } else {
        const char *colon = memchr(h, ':', (size_t)(hend - h));
        if (colon) cport = colon;
    }

    {
        const char *host_end = cport ? cport : hend;
        size_t hl = (size_t)(host_end - h);
        if (hl == 0 || hl + 1 > host_cap) return -1;
        if (host) {
            memcpy(host, h, hl);
            host[hl] = '\0';
        }
    }
    if (port) port[0] = '\0';
    if (cport) {
        size_t pl = (size_t)(hend - (cport + 1));
        if (pl + 1 > port_cap) return -1;
        if (port) {
            memcpy(port, cport + 1, pl);
            port[pl] = '\0';
        }
    }
    if (path) {
        const char *pp = slash ? slash : "/";
        size_t pl = strlen(pp);
        if (pl + 1 > path_cap) return -1;
        memcpy(path, pp, pl + 1);
    }
    return 0;
}

static void origin_of(const char *url, char *out, size_t cap) {
    char scheme[8], host[CNK_HOST_MAX], port[8];
    if (url_split(url, scheme, sizeof scheme, host, sizeof host, port, sizeof port, NULL, 0) != 0) {
        if (cap) out[0] = '\0';
        return;
    }
    if (port[0])
        snprintf(out, cap, "%s://%s:%s", scheme, host, port);
    else
        snprintf(out, cap, "%s://%s", scheme, host);
}

int url_host_of(const char *url, char *out, size_t cap) {
    char scheme[8], host[CNK_HOST_MAX], port[8];
    size_t i;
    int is_default;

    if (url_split(url, scheme, sizeof scheme, host, sizeof host, port, sizeof port, NULL, 0) != 0)
        return -1;
    is_default = (port[0] == '\0') ||
                 (strcmp(scheme, "http") == 0 && strcmp(port, "80") == 0) ||
                 (strcmp(scheme, "https") == 0 && strcmp(port, "443") == 0);
    if (is_default) {
        snprintf(out, cap, "%s", host);
    } else {
        snprintf(out, cap, "%s:%s", host, port);
    }
    for (i = 0; out[i]; ++i) out[i] = (char)tolower((unsigned char)out[i]);
    return 0;
}

void url_resolve(const char *base, const char *ref, char *out, size_t cap) {
    char origin[CNK_URL_MAX];
    if (!ref || !ref[0]) { if (cap) out[0] = '\0'; return; }

    if (strncmp(ref, "http://", 7) == 0 || strncmp(ref, "https://", 8) == 0) {
        snprintf(out, cap, "%s", ref);
        return;
    }
    if (ref[0] == '/' && ref[1] == '/') {
        char scheme[8], host[CNK_HOST_MAX], port[8];
        if (url_split(base, scheme, sizeof scheme, host, sizeof host, port, sizeof port, NULL, 0) == 0)
            snprintf(out, cap, "%s:%s", scheme, ref);
        else if (cap) out[0] = '\0';
        return;
    }
    origin_of(base, origin, sizeof origin);
    if (origin[0] == '\0') { if (cap) out[0] = '\0'; return; }
    if (ref[0] == '/')
        snprintf(out, cap, "%s%s", origin, ref);
    else
        snprintf(out, cap, "%s/%s", origin, ref);   /* 与 Windows 版 ResolveUrl 同口径 */
}

/* ---------------- socket 基础 ---------------- */

static int wait_fd(sock_t fd, int for_write, int timeout_ms) {
    fd_set fs;
    struct timeval tv;
    int r;
    if (timeout_ms < 0) timeout_ms = 0;
    FD_ZERO(&fs);
    FD_SET(fd, &fs);
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
#ifdef _WIN32
    r = select(0, for_write ? NULL : &fs, for_write ? &fs : NULL, NULL, &tv);
    return r > 0 ? 0 : -1;
#else
    r = select((int)(fd + 1), for_write ? NULL : &fs, for_write ? &fs : NULL, NULL, &tv);
    if (r > 0) return 0;
    if (r == 0) return -1;
    return (errno == EINTR) ? -2 : -1;
#endif
}

static void set_nonblock(sock_t fd) {
#ifdef _WIN32
    u_long mode = 1;
    ioctlsocket(fd, FIONBIO, &mode);
#else
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl >= 0) fcntl(fd, F_SETFL, fl | O_NONBLOCK);
#endif
}

static int sock_connect(const char *host, const char *port, const char *bind_ip,
                        int timeout_ms, sock_t *out, char *err, size_t errcap) {
    struct addrinfo hints, *res = NULL, *it;
    int rc;

    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    rc = getaddrinfo(host, port, &hints, &res);
    if (rc != 0 || !res) {
        char buf[160];
        snprintf(buf, sizeof buf, "域名解析失败：%s", host);
        set_err(err, errcap, buf);
        return -1;
    }

    for (it = res; it; it = it->ai_next) {
        sock_t fd = socket(it->ai_family, it->ai_socktype, it->ai_protocol);
        int ok = 0;
        if (fd == SOCK_INVALID) continue;

        if (bind_ip && bind_ip[0]) {
            struct addrinfo bh, *br = NULL;
            memset(&bh, 0, sizeof bh);
            bh.ai_family = it->ai_family;
            bh.ai_socktype = SOCK_STREAM;
            if (getaddrinfo(bind_ip, NULL, &bh, &br) == 0 && br) {
                bind(fd, br->ai_addr, (int)br->ai_addrlen);
                freeaddrinfo(br);
            }
        }

        set_nonblock(fd);
        rc = connect(fd, it->ai_addr, (int)it->ai_addrlen);
        if (rc == 0) {
            ok = 1;
        } else {
#ifdef _WIN32
            if (WSAGetLastError() == WSAEWOULDBLOCK) {
#else
            if (errno == EINPROGRESS || errno == EWOULDBLOCK) {
#endif
                if (wait_fd(fd, 1, timeout_ms) == 0) {
                    int soerr = 0;
#ifdef _WIN32
                    int sl = (int)sizeof soerr;
#else
                    socklen_t sl = sizeof soerr;
#endif
                    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, (char *)&soerr, &sl) == 0 && soerr == 0)
                        ok = 1;
                }
            }
        }
        if (ok) {
            *out = fd;
            freeaddrinfo(res);
            return 0;
        }
        sock_close(fd);
    }
    freeaddrinfo(res);
    set_err(err, errcap, "连接失败（拒绝/不可达/超时）");
    return -1;
}

static int send_all(sock_t fd, const char *buf, size_t len, long long deadline_ms, char *err, size_t errcap) {
    size_t sent = 0;
    while (sent < len) {
        int r;
        /* deadline 必须是 long long：路由器连续运行超过 24 天后，
         * 毫秒计数用 int 会溢出成负数，超时判断立刻失效 */
        long long remain = deadline_ms - now_ms();
        if (remain <= 0) { set_err(err, errcap, "发送超时"); return -1; }
        r = (int)send(fd, buf + sent, (int)(len - sent), 0);
        if (r > 0) { sent += (size_t)r; continue; }
#ifdef _WIN32
        if (WSAGetLastError() == WSAEWOULDBLOCK) {
#else
        if (errno == EINTR) continue;
        if (errno == EWOULDBLOCK || errno == EAGAIN) {
#endif
            wait_fd(fd, 1, (int)remain);
            continue;
        }
        set_err(err, errcap, "发送失败");
        return -1;
    }
    return 0;
}

/* 解析头部：返回头结束位置（body 起点），失败 -1 */
static int parse_head(char *raw, size_t len, HttpResp *resp, int *content_length, int *chunked) {
    char *head_end = strstr(raw, "\r\n\r\n");
    size_t hdr_len;
    char *line, *next;

    *content_length = -1;
    *chunked = 0;
    if (!head_end) return -1;
    hdr_len = (size_t)(head_end - raw) + 4;

    if (strncmp(raw, "HTTP/", 5) != 0) return -1;
    {
        char *sp = strchr(raw, ' ');
        resp->status = sp ? atoi(sp + 1) : 0;
    }

    line = strstr(raw, "\r\n");
    while (line && line < head_end) {
        line += 2;
        next = strstr(line, "\r\n");
        if (!next || next > head_end) break;
        {
            size_t ll = (size_t)(next - line);
            if (ll > 5 && tolower((unsigned char)line[0]) == 'l' &&
                ci_find(line, "location:") == line) {
                const char *v = line + 9;
                while (v < next && (*v == ' ' || *v == '\t')) ++v;
                {
                    size_t vl = (size_t)(next - v);
                    if (vl > sizeof resp->location - 1) vl = sizeof resp->location - 1;
                    memcpy(resp->location, v, vl);
                    resp->location[vl] = '\0';
                }
            } else if (ll > 15 && ci_find(line, "content-length:") == line) {
                *content_length = atoi(line + 15);
            } else if (ll > 17 && ci_find(line, "transfer-encoding:") == line) {
                if (ci_find(line, "chunked")) *chunked = 1;
            }
        }
        line = next;
    }
    return (int)hdr_len;
}

static size_t decode_chunked(char *buf, size_t len) {
    size_t r = 0, w = 0;
    while (r < len) {
        char *endp = NULL;
        unsigned long sz = strtoul(buf + r, &endp, 16);
        if (!endp) break;
        while (endp < buf + len && *endp != '\n') ++endp;
        if (endp >= buf + len) break;
        r = (size_t)(endp - buf) + 1;
        if (sz == 0) break;
        if (r + sz > len) sz = len - r;
        memmove(buf + w, buf + r, sz);
        w += sz;
        r += sz;
        while (r + 1 < len && buf[r] == '\r' && buf[r + 1] == '\n') r += 2;
    }
    return w;
}

void http_resp_free(HttpResp *r) {
    if (!r) return;
    free(r->body);
    r->body = NULL;
    r->body_len = 0;
}

int http_request(const char *method, const char *url, const char *body,
                 const char *content_type, const char *bind_ip,
                 int timeout_ms, HttpResp *out, char *err, size_t errcap) {
    char scheme[8], host[CNK_HOST_MAX], port[8], path[CNK_PATH_MAX];
    char *req = NULL;
    size_t req_len;
    sock_t fd = SOCK_INVALID;
    char *raw = NULL;
    size_t raw_len = 0, raw_cap = 0;
    int hdr_len = -1, content_length = -1, chunked = 0;
    long long deadline = now_ms() + timeout_ms;
    int rc = -1;

    memset(out, 0, sizeof *out);
    if (err && errcap) err[0] = '\0';

    if (url_split(url, scheme, sizeof scheme, host, sizeof host, port, sizeof port, path, sizeof path) != 0) {
        set_err(err, errcap, "URL 格式不正确");
        return -1;
    }
    if (strcmp(scheme, "http") != 0) {
        set_err(err, errcap, "只支持 http://（门户在校园内网）");
        return -1;
    }
    if (!port[0]) snprintf(port, sizeof port, "80");

    if (sock_connect(host, port, bind_ip, timeout_ms, &fd, err, errcap) != 0) return -1;

    req = (char *)malloc(strlen(host) + strlen(path) + 512 + (body ? strlen(body) : 0));
    if (!req) { set_err(err, errcap, "内存不足"); goto done; }

    if (body) {
        req_len = (size_t)sprintf(req,
            "%s %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: cnkrouter/%s\r\nAccept: */*\r\n"
            "Content-Type: %s\r\nContent-Length: %u\r\nConnection: close\r\n\r\n",
            method, path, host, CNK_VERSION,
            content_type ? content_type : "application/x-www-form-urlencoded",
            (unsigned)strlen(body));
        memcpy(req + req_len, body, strlen(body));
        req_len += strlen(body);
    } else {
        req_len = (size_t)sprintf(req,
            "%s %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: cnkrouter/%s\r\nAccept: */*\r\n"
            "Connection: close\r\n\r\n",
            method, path, host, CNK_VERSION);
    }

    if (send_all(fd, req, req_len, deadline, err, errcap) != 0) goto done;

    for (;;) {
        long long remain = deadline - now_ms();
        int r, w2;
        if (remain <= 0) { set_err(err, errcap, "读取响应超时"); goto done; }
        if (wait_fd(fd, 0, (int)remain) != 0) { set_err(err, errcap, "读取响应超时"); goto done; }

        if (raw_len + 8192 + 1 > raw_cap) {
            size_t ncap = raw_cap ? raw_cap * 2 : 16384;
            char *nb;
            if (ncap > CNK_RESP_MAX + 8192) ncap = CNK_RESP_MAX + 8192;
            if (ncap <= raw_len + 8192) { set_err(err, errcap, "响应过大"); goto done; }
            nb = (char *)realloc(raw, ncap);
            if (!nb) { set_err(err, errcap, "内存不足"); goto done; }
            raw = nb;
            raw_cap = ncap;
        }

        r = (int)recv(fd, raw + raw_len, (int)(raw_cap - raw_len - 1), 0);
        if (r == 0) { w2 = 1; }
        else if (r > 0) {
            raw_len += (size_t)r;
            raw[raw_len] = '\0';   /* decode_chunked/strstr 需要 NUL 终止 */
            w2 = 0;
        }
        else {
#ifdef _WIN32
            if (WSAGetLastError() == WSAEWOULDBLOCK) continue;
#else
            if (errno == EINTR || errno == EWOULDBLOCK || errno == EAGAIN) continue;
#endif
            set_err(err, errcap, "读取失败");
            goto done;
        }

        if (hdr_len < 0) hdr_len = parse_head(raw, raw_len, out, &content_length, &chunked);

        if (hdr_len >= 0) {
            size_t have = raw_len - (size_t)hdr_len;
            if (content_length >= 0 && have >= (size_t)content_length) break;
            if (have > CNK_RESP_MAX) { set_err(err, errcap, "响应过大"); goto done; }
        }
        if (w2) break;   /* 对端关闭 */
    }

    if (hdr_len < 0) { set_err(err, errcap, "响应不是合法 HTTP"); goto done; }

    {
        size_t blen = raw_len - (size_t)hdr_len;
        char *bstart = raw + hdr_len;
        if (chunked) blen = decode_chunked(bstart, blen);
        if (content_length >= 0 && blen > (size_t)content_length) blen = (size_t)content_length;
        out->body = (char *)malloc(blen + 1);
        if (!out->body) { set_err(err, errcap, "内存不足"); goto done; }
        memcpy(out->body, bstart, blen);
        out->body[blen] = '\0';
        out->body_len = blen;
    }
    rc = 0;

done:
    if (rc != 0) http_resp_free(out);
    if (fd != SOCK_INVALID) sock_close(fd);
    free(req);
    free(raw);
    return rc;
}
