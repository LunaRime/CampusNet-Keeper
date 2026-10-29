/*
 * cnkrouter —— CampusNet-Keeper 的路由器端精简实现
 *
 * 目标平台：OpenWrt / Padavan / 任意 Linux（静态 musl 或 glibc 均可）。
 * 设计约束：
 *   - 只用 libc，不依赖 OpenSSL/curl/Python（路由器 Flash 与 RAM 都紧张）；
 *   - 常驻内存固定分配，不在循环里做无界 malloc；
 *   - 单进程，无守护框架依赖，由 procd 托管。
 *
 * 与 Windows 版（src/native）的对应关系：
 *   auth/DrcomAuth.cpp      -> portal.c   （新式 API + 老式回退）
 *   core/Probe.cpp          -> probe 部分 （同判定语义）
 *   core/Scheduler.cpp      -> main.c     （同退避/终止策略）
 *   core/Crypto.cpp         -> crypto.c   （AES-128-ECB + PKCS7 + Base64）
 */
#ifndef CNK_ROUTER_H
#define CNK_ROUTER_H

/* Linux / musl 上必须**显式声明 POSIX 特性宏**：`-std=c99 -pedantic` 会隐藏
 * clock_gettime / CLOCK_MONOTONIC / struct addrinfo / strdup / usleep 等接口，
 * 症状是 "storage size of 'ts' isn't known" 这类看不出根因的编译错误。
 * 必须放在任何系统头之前 —— 本文件是所有 .c 的第一个 include，放这里最稳。
 * （2026-10-29 由 CI 首次在 Linux 上编译时暴露；Windows/mingw 下无此问题。） */
#if !defined(_WIN32)
#  ifndef _POSIX_C_SOURCE
#    define _POSIX_C_SOURCE 200809L
#  endif
#  ifndef _DEFAULT_SOURCE
#    define _DEFAULT_SOURCE 1
#  endif
#endif

#if defined(_MSC_VER)
#define _CRT_SECURE_NO_WARNINGS 1
#endif

#include <stddef.h>
#include <stdint.h>

#define CNK_VERSION "0.1.0"

/* 固定上限：配置与报文的最大尺寸（路由器上避免堆碎片） */
#define CNK_URL_MAX   256
#define CNK_HOST_MAX  96
/* 登录 URL 里 params 是 400+ 字符的 Base64（URL 编码后可能三倍膨胀），
 * 路径缓冲区必须留足——原值 640 会把 params 静默截断（由 e2e 抓出） */
#define CNK_PATH_MAX  3072
#define CNK_USER_MAX  64
#define CNK_PASS_MAX  128
#define CNK_MSG_MAX   256
#define CNK_RESP_MAX  512u * 1024u

/* ---------------- 日志 ---------------- */
#define LOG_ERR  0
#define LOG_WARN 1
#define LOG_INFO 2
#define LOG_DBG  3

void log_init(int level, const char *path);
void log_write(int level, const char *fmt, ...);

/* ---------------- 加密 / 编码 ---------------- */
/* AES-128-ECB + PKCS7 填充 + 标准 Base64（与 Windows 版 Crypto.cpp 逐字节等价） */
void aes128_ecb_pkcs7_b64(const uint8_t key[16], const char *plain,
                          char *out, size_t cap);
/* 裸块加密（长度必须是 16 的倍数），仅用于自检与交叉校验 */
void aes128_ecb_encrypt_raw(const uint8_t key[16], const uint8_t *in, size_t len,
                            uint8_t *out);
size_t b64_encode(const uint8_t *in, size_t len, char *out, size_t cap);
int b64_decode(const char *in, size_t inlen, uint8_t *out, size_t cap);
/* 百分号编码：除 RFC 3986 unreserved 外全部转义（Base64 的 + / = 必须被转义） */
size_t url_encode(const char *in, size_t len, char *out, size_t cap);
size_t json_escape(const char *in, char *out, size_t cap);

/* ---------------- HTTP（仅 http://，门户都在内网明文） ---------------- */
typedef struct {
    int    status;              /* HTTP 状态码，0 表示未拿到响应 */
    char   location[512];
    char  *body;                /* 堆分配，需 http_resp_free */
    size_t body_len;
} HttpResp;

void http_resp_free(HttpResp *r);

/* 返回 0 成功；失败时 err 带原因。bind_ip 非空时绑定源地址（多 WAN 时指定出口）。 */
int http_request(const char *method, const char *url, const char *body,
                 const char *content_type, const char *bind_ip,
                 int timeout_ms, HttpResp *out, char *err, size_t errcap);
/* Windows 下做 WSAStartup；POSIX 下为空操作 */
void net_init(void);

/* 拆分 URL；任一输出可为 NULL。返回 0 成功，仅支持 http:// */
int url_split(const char *url, char *scheme, size_t scheme_cap,
              char *host, size_t host_cap, char *port, size_t port_cap,
              char *path, size_t path_cap);
/* 取「异主机」判定用的主机键（去 userinfo、去默认端口、小写） */
int url_host_of(const char *url, char *out, size_t cap);
/* 把 ref 解析成绝对地址（相对路径按 base 的 origin 拼接） */
void url_resolve(const char *base, const char *ref, char *out, size_t cap);

/* ---------------- JSON 迷你解析（只取标量，容忍 JSONP 包裹） ---------------- */
int json_find_str(const char *json, const char *key, char *out, size_t cap);
int json_find_int(const char *json, const char *key, long *out);

/* ---------------- 配置 ---------------- */
typedef struct {
    char auth_url[CNK_URL_MAX];
    char username[CNK_USER_MAX];
    char password[CNK_PASS_MAX];
    char isp[16];              /* campus | cmcc | telecom | unicom | custom:<后缀> */
    char portal_host[CNK_HOST_MAX];  /* 可选：强制认证端口/主机（留空按 auth_url） */
    char probe_url[CNK_URL_MAX];
    char probe_expect[64];
    int  probe_timeout;        /* 秒 */
    int  probe_interval;       /* 秒 */
    char bind_ip[64];
    int  log_level;
} Config;

void        config_defaults(Config *c);
int         config_load(const char *path, Config *c, char *err, size_t cap);
const char *isp_suffix(const Config *c);

/* ---------------- 探测 ---------------- */
typedef enum {
    NET_UNKNOWN = 0,   /* 未配置探测地址 */
    NET_ONLINE  = 1,
    NET_PORTAL  = 2,
    NET_OFFLINE = 3
} NetState;

typedef struct {
    NetState state;
    char message[CNK_MSG_MAX];
    char portal_url[CNK_URL_MAX];   /* 3xx 劫持时的跳转目标 */
} ProbeResult;

ProbeResult probe_once(const Config *c);
const char *net_state_name(NetState s);

/* ---------------- 登录 ---------------- */
typedef struct {
    int  ok;
    int  cred_error;    /* 1 = 服务器明确报凭据错误 → 停止自动重试 */
    char message[CNK_MSG_MAX];
    char request_url[CNK_URL_MAX * 2];
} LoginResult;

LoginResult portal_login(const Config *c);

/* ---------------- 运行 ---------------- */
long long now_ms(void);
void      sleep_ms(int ms);
int       run_once(Config *c);      /* 探测+必要时登录，0=最终在线 */
int       run_daemon(Config *c);

#endif /* CNK_ROUTER_H */
