/* 门户探测与 Dr.COM 认证（新式 :801 API 优先，老式 ACSetting 回退）。
 *
 * 语义与 Windows 版严格对齐：
 *   core/Probe.cpp        —— 在线判定（204 / 3xx 异主机 / 200 正文标识）
 *   auth/DrcomAuth.cpp    —— chkstatus → loadConfig(rcn) → login(AES params)
 * 两处刻意保留的"精进"：
 *   1) 服务器回「终端IP已在线」按成功处理（result=0 + ret_code=2 的真实形态）；
 *   2) 老式通道的 userid error2 只在「新式接口确实不存在」时才当凭据错误，
 *      否则降级为"无法判定"——本部署的老式通道恒回 error2，误判会让守护永久停摆。
 */
#include "cnk.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <stdarg.h>

/* 门户脚本里的公开密钥（a41.js util.aes_en 默认值，非机密） */
static const uint8_t AES_KEY[16] = {
    '5', 'c', '1', 'd', '5', 'a', 'd', '4',
    'd', 'e', 'a', '0', 'e', '8', 'd', 'd'};

/* ---------------- JSON 迷你解析 ---------------- */

static int key_boundary_ok(const char *json, const char *hit) {
    const char *p = hit;
    while (p > json) {
        char c = *(p - 1);
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { --p; continue; }
        return (c == '{' || c == ',');
    }
    return 1;
}

static const char *find_key(const char *json, const char *key) {
    char pat[64];
    size_t kl = strlen(key);
    const char *hit;
    if (kl + 3 > sizeof pat) return NULL;
    pat[0] = '"';
    memcpy(pat + 1, key, kl);
    pat[kl + 1] = '"';
    pat[kl + 2] = '\0';

    hit = json;
    while ((hit = strstr(hit, pat)) != NULL) {
        if (key_boundary_ok(json, hit)) return hit;
        hit += kl + 2;
    }
    return NULL;
}

static int hex4(const char *p, unsigned *out) {
    unsigned v = 0;
    int i;
    for (i = 0; i < 4; ++i) {
        char c = p[i];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
        else return -1;
    }
    *out = v;
    return 0;
}

/* 解 JSON 字符串转义并写入 out（UTF-8）。返回写入长度，失败 -1。 */
static int json_unescape(const char *s, size_t len, char *out, size_t cap) {
    size_t i = 0, o = 0;
    while (i < len) {
        char c = s[i++];
        if (c != '\\') {
            if (o + 1 >= cap) return -1;
            out[o++] = c;
            continue;
        }
        if (i >= len) return -1;
        c = s[i++];
        switch (c) {
            case '"': out[o++] = '"'; break;
            case '\\': out[o++] = '\\'; break;
            case '/': out[o++] = '/'; break;
            case 'b': out[o++] = '\b'; break;
            case 'f': out[o++] = '\f'; break;
            case 'n': out[o++] = '\n'; break;
            case 'r': out[o++] = '\r'; break;
            case 't': out[o++] = '\t'; break;
            case 'u': {
                unsigned cp;
                if (i + 4 > len || hex4(s + i, &cp) != 0) return -1;
                i += 4;
                /* 代理对：\uD8xx\uDCxx → 合并成一个码点 */
                if (cp >= 0xD800 && cp <= 0xDBFF && i + 6 <= len &&
                    s[i] == '\\' && s[i + 1] == 'u') {
                    unsigned lo;
                    if (hex4(s + i + 2, &lo) == 0 && lo >= 0xDC00 && lo <= 0xDFFF) {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        i += 6;
                    }
                }
                if (cp < 0x80) {
                    if (o + 1 >= cap) return -1;
                    out[o++] = (char)cp;
                } else if (cp < 0x800) {
                    if (o + 2 >= cap) return -1;
                    out[o++] = (char)(0xC0 | (cp >> 6));
                    out[o++] = (char)(0x80 | (cp & 0x3F));
                } else if (cp < 0x10000) {
                    if (o + 3 >= cap) return -1;
                    out[o++] = (char)(0xE0 | (cp >> 12));
                    out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                    out[o++] = (char)(0x80 | (cp & 0x3F));
                } else {
                    if (o + 4 >= cap) return -1;
                    out[o++] = (char)(0xF0 | (cp >> 18));
                    out[o++] = (char)(0x80 | ((cp >> 12) & 0x3F));
                    out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                    out[o++] = (char)(0x80 | (cp & 0x3F));
                }
                break;
            }
            default: return -1;
        }
        if (o >= cap) return -1;
    }
    out[o] = '\0';
    return (int)o;
}

int json_find_str(const char *json, const char *key, char *out, size_t cap) {
    const char *hit = find_key(json, key);
    const char *p;
    const char *start;
    size_t len;
    if (!hit) return 0;
    p = hit + strlen(key) + 2;
    while (*p && isspace((unsigned char)*p)) ++p;
    if (*p != ':') return 0;
    ++p;
    while (*p && isspace((unsigned char)*p)) ++p;
    if (*p != '"') return 0;
    start = ++p;
    while (*p && *p != '"') {
        if (*p == '\\' && p[1]) ++p;
        ++p;
    }
    if (*p != '"') return 0;
    len = (size_t)(p - start);
    if (json_unescape(start, len, out, cap) < 0) return 0;
    return 1;
}

int json_find_int(const char *json, const char *key, long *out) {
    const char *hit = find_key(json, key);
    const char *p;
    if (!hit) return 0;
    p = hit + strlen(key) + 2;
    while (*p && isspace((unsigned char)*p)) ++p;
    if (*p != ':') return 0;
    ++p;
    while (*p && isspace((unsigned char)*p)) ++p;
    if (*p == '"') {
        ++p;
        if (!isdigit((unsigned char)*p) && *p != '-') return 0;
    }
    if (!isdigit((unsigned char)*p) && *p != '-') return 0;
    *out = strtol(p, NULL, 10);
    return 1;
}

/* 子串包含（大小写敏感，用于中文提示词判定） */
static int has(const char *s, const char *needle) {
    return s && strstr(s, needle) != NULL;
}

/* 大小写不敏感的包含判断（只处理 ASCII）。
 * 门户的 inuse 提示不只有小写一种写法（InUse / INUSE 都见过），
 * 只按小写匹配等于埋一个"换个写法就漏判"的坑。
 * 非 static：暴露给 selftest 独立复算（见 main.c），避免"只改函数不测函数"。 */
int has_i(const char *s, const char *needle) {
    size_t i, n;
    if (!s || !needle || !needle[0]) return 0;
    n = strlen(needle);
    for (i = 0; s[i]; ++i) {
        size_t j = 0;
        while (j < n && s[i + j] &&
               tolower((unsigned char)s[i + j]) == tolower((unsigned char)needle[j])) {
            ++j;
        }
        if (j == n) return 1;
    }
    return 0;
}

/* 把 s 里出现的 secret 全部抹成 *（等长、原地安全），用于日志脱敏。
 * 账号/密码绝不可落盘：msg 是服务器回包、启动日志里的账号，都不能明文。 */
void redact_secret(char *s, const char *secret) {
    char *p;
    size_t n;
    if (!s || !secret) return;
    n = strlen(secret);
    if (n == 0) return;
    for (p = s; (p = strstr(p, secret)) != NULL; p += n) {
        memset(p, '*', n);
    }
}

/* FNV-1a 32 位稳定哈希：取证里的 olmac/ss1 是设备标识，不该明文落盘
 * （日志目录里还有 WAN IP / 门户地址，文件已 chmod 600）。
 * 哈希后「字段有没有突变」照样能比 —— 这正是取证要的，原值反而用不上。 */
void mask_id(const char *s, char *out, size_t cap) {
    uint32_t h = 2166136261u;
    const unsigned char *p = (const unsigned char *)(s ? s : "");
    if (cap < 9) { if (cap) out[0] = '\0'; return; }
    for (; *p; ++p) {
        h ^= *p;
        h *= 16777619u;
    }
    snprintf(out, cap, "%08x", (unsigned)h);
}

/* ---------------- 探测 ---------------- */

const char *net_state_name(NetState s) {
    switch (s) {
        case NET_ONLINE:  return "online";
        case NET_PORTAL:  return "portal";
        case NET_OFFLINE: return "offline";
        default:          return "unknown";
    }
}

/* ---------------- AC 复核取证（2026-11-05 新增） ----------------
 *
 * 背景：AC 会话被频繁回收，但「回收」只表现为「DNS 解析失败 + 门户连接超时」
 * （AC 把整个 IP 黑洞了，而 DNS 服务器恰是 AC 自己）。要从日志定位根因，
 * 需要在 AC 视角还正常时持续留快照，等离线时回放，看 AC 是哪种死法：
 *   · 会话到期      → result 从 1 突变 0、oltime 复位；
 *   · MAC 绑定冲突   → olmac（当前在线 MAC）与 ss1（历史绑定 MAC）对不上或突变；
 *   · 共享封禁/黑洞   → result 一直 1、字段纹丝不动，但转发面突然全断。
 *
 * 实现：环形缓冲保留最近 N 次「成功解析出 result」的 chkstatus 快照（result 是 0
 * 还是 1 都留——0 正是"会话被回收"的关键指纹）。请求失败/取不到 result 的则不留：
 * 那类回包没有有效字段，塞进历史反而污染「AC 正常时是什么样」的对照基线。
 * 离线现场由 ac_forensics_dump 里再打一次 chkstatus 补全（成败都留痕）。 */
#define AC_FORENSIC_MAX 8

typedef struct {
    time_t ts;
    long   result;
    char   olmac[24];
    char   ss1[24];
    char   v4ip[64];
    long   oltime;
} AcForensicEntry;

static AcForensicEntry g_ac_hist[AC_FORENSIC_MAX];
static int g_ac_hist_n   = 0;   /* 已存条数（0..MAX） */
static int g_ac_hist_pos = 0;   /* 下一条写入位置 */

static void ac_hist_push(long result, const char *body) {
    AcForensicEntry *e = &g_ac_hist[g_ac_hist_pos];
    memset(e, 0, sizeof *e);
    e->ts = time(NULL);
    e->result = result;
    e->oltime = -1;
    if (body) {
        if (!json_find_str(body, "olmac", e->olmac, sizeof e->olmac)) e->olmac[0] = '\0';
        if (!json_find_str(body, "ss1", e->ss1, sizeof e->ss1))       e->ss1[0] = '\0';
        if (!json_find_str(body, "v4ip", e->v4ip, sizeof e->v4ip))    e->v4ip[0] = '\0';
        { long t = -1; if (json_find_int(body, "oltime", &t)) e->oltime = t; }
    }
    g_ac_hist_pos = (g_ac_hist_pos + 1) % AC_FORENSIC_MAX;
    if (g_ac_hist_n < AC_FORENSIC_MAX) ++g_ac_hist_n;
}

static void fmt_hmstime(time_t t, char *out, size_t cap) {
    struct tm tmv;
#ifdef _WIN32
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    strftime(out, cap, "%H:%M:%S", &tmv);
}

/* 前置声明：ac_says_online 定义在本文件更下方（与探测判定放一起） */
static int ac_says_online(const Config *c);

void ac_forensics_dump(const Config *c) {
    int i, n = 0;
    char tbuf[16];

    log_write(LOG_INFO, "【取证】AC 复核历史（最近 %d 次，最旧→最新）：", g_ac_hist_n);
    for (i = 0; i < g_ac_hist_n; ++i) {
        int idx = (g_ac_hist_pos - g_ac_hist_n + i + AC_FORENSIC_MAX) % AC_FORENSIC_MAX;
        const AcForensicEntry *e = &g_ac_hist[idx];
        char mb1[16], mb2[16];
        const char *om = "-", *sm = "-";
        if (e->olmac[0]) { mask_id(e->olmac, mb1, sizeof mb1); om = mb1; }
        if (e->ss1[0])  { mask_id(e->ss1,  mb2, sizeof mb2); sm = mb2; }
        fmt_hmstime(e->ts, tbuf, sizeof tbuf);
        /* olmac/ss1 只以稳定摘要出现：字段突变仍一眼可见，真实 MAC 不落盘。
         * v4ip 保留 —— 运维要靠它确认软拔插是否真的换到了新地址。 */
        log_write(LOG_INFO,
                  "  #%d %s result=%ld olmac=%s ss1=%s v4ip=%s oltime=%ld",
                  i + 1, tbuf, e->result, om, sm,
                  e->v4ip[0] ? e->v4ip : "-",
                  e->oltime);
        ++n;
    }
    if (n == 0) log_write(LOG_INFO, "  （无历史快照）");

    /* 现场再打一次 chkstatus，看「此刻 AC 还认不认」——这是区分
     * 「AC 主动踢（还能连、result=0）」vs「AC 黑洞（连不上）」的关键。
     * 结论用 WARN 打出（不依赖 DBG 级别），保证取证结果一定可见。 */
    {
        int now = ac_says_online(c);
        if (now == 1)
            log_write(LOG_WARN, "【取证】现场 AC 复核：仍认为在线 → 转发面失效（僵尸会话/黑洞），非会话到期");
        else if (now == 0)
            log_write(LOG_WARN, "【取证】现场 AC 复核：明确不在线 → 会话已被 AC 主动回收（到期/被踢）");
        else
            log_write(LOG_WARN, "【取证】现场 AC 复核：查不出 → AC 不可达（连 chkstatus 都不响应，疑似整段黑洞）");
    }
}

/* 「持续在线静默」标志：探测间隔降到 10s 后，在线时每轮都会打
 * 「AC 复核 result=1」「出网验证 204 通」「在线：正文匹配预期标识」三条一模一样的日志，
 * 10 秒一轮 ≈ 26 KB/天 × 纯重复。置 1 后只静默「一切正常」的那几条，异常照打。
 * 由 run_daemon 在进入持续在线时置 1、发生状态变化时清零。 */
static int g_probe_quiet = 0;

void probe_set_quiet(int q) { g_probe_quiet = q ? 1 : 0; }

/* 用认证服务器（AC）的视角复核本机是否在线。
 *
 * 为什么需要：2026-10-30 实测，本网把 captive.apple.com **DNS 劫持**到了
 * *.download.ks-cdn.com（国内 CDN）。AC 会话失效后，那个域名**照样返回
 * 200 + "Success"** —— 探测判定"在线"，守护进程于是不去重连，用户上不了网
 * 而程序认为一切正常（当天实测持续了 9 小时，日志里一条记录都没有）。
 *
 * 返回：1=AC 认为在线，0=AC 明确说不在线，-1=查不出来
 *      （接口不存在/超时/回包异常——此时**不能**据此判离线，否则会把
 *       非 Dr.COM 环境全判成断网）。 */
static int ac_says_online(const Config *c) {
    char scheme[16], host[CNK_HOST_MAX], port[16], path[CNK_URL_MAX];
    char origin[CNK_URL_MAX], url[CNK_URL_MAX * 2], err[128];
    HttpResp resp;
    long code = 0;
    int verdict = -1;

    if (!c->auth_url[0]) return -1;
    if (url_split(c->auth_url, scheme, sizeof scheme, host, sizeof host,
                  port, sizeof port, path, sizeof path) != 0)
        return -1;
    if (port[0]) snprintf(origin, sizeof origin, "%s://%s:%s", scheme, host, port);
    else         snprintf(origin, sizeof origin, "%s://%s", scheme, host);

    snprintf(url, sizeof url, "%s/drcom/chkstatus?callback=cnkwd&jsVersion=4.X&lang=zh", origin);

    memset(&resp, 0, sizeof resp);
    if (http_request("GET", url, NULL, NULL, c->bind_ip, 4000, &resp, err, sizeof err) != 0) {
        log_write(LOG_DBG, "AC 复核：请求失败（%s）", err);
        return -1;
    }
    if (resp.status != 200) {
        log_write(LOG_DBG, "AC 复核：HTTP %d", resp.status);
    } else if (json_find_int(resp.body, "result", &code) == 1) {
        /* json_find_int 成功返回 1、失败返回 0 —— 这里千万别写成 == 0
         * （第一版就写反了，结果永远拿不到结果，一路降级成"复核不可用"） */
        verdict = (code == 1) ? 1 : 0;
        if (!g_probe_quiet)
            log_write(LOG_DBG, "AC 复核：result=%ld → %s", code,
                      verdict == 1 ? "在线" : "不在线");
        ac_hist_push(code, resp.body);   /* 取证快照：无论 0/1 都留（静默也要留） */
    } else {
        log_write(LOG_DBG, "AC 复核：回包里取不到 result 字段");
    }
    http_resp_free(&resp);
    return verdict;
}

/* 第三层验证：真的能出网吗？
 *
 * 前两层（apple 探测 + chkstatus）都可能被"僵尸会话"骗过：
 * AC 记着"这个 IP 已认证"、探测域名又能通，但**转发面已经失效**，
 * 用户实际打不开任何网页（实测持续 9 小时，程序毫无察觉）。
 *
 * 这里用两个与校园网无关的目标确认"真的能出去"：
 *   · MIUI 的 generate_204：正常必回 204 空正文
 *   · 百度 robots.txt：正常必回 200 且含 "User-agent"
 * 被劫持/被拦时这两个都会返回门户页或超时，与"真在线"区分得开。
 *
 * 返回 1 = 能出网，0 = 不能，-1 = 目标全都不可达（判不出来，别据此断网） */
static int can_reach_internet(const Config *c) {
    static const char *urls[] = {
        "http://connect.rom.miui.com/generate_204",
        "http://www.baidu.com/robots.txt",
        NULL
    };
    int i;
    int got_response = 0;   /* 至少有一个目标回了 HTTP —— 用来区分"被劫持"和"够不着" */

    for (i = 0; urls[i]; ++i) {
        HttpResp resp;
        char err[128];

        memset(&resp, 0, sizeof resp);
        if (http_request("GET", urls[i], NULL, NULL, c->bind_ip, 5000,
                         &resp, err, sizeof err) != 0) {
            /* 上游 DNS 就是 AC，短黑洞时「域名解析失败」会每轮刷一条
             * （真机一天 47 条来自 MIUI，占当天 DNS 失败的一半以上）。
             * 这类瞬时抖动限频 60 秒打一条；超时/连接失败是排障依据，照常全打。 */
            if (has_i(err, "域名解析失败")) {
                static time_t t_last_dns = 0;
                time_t now = time(NULL);
                if (now - t_last_dns >= 60) {
                    t_last_dns = now;
                    log_write(LOG_DBG, "出网验证：%s 请求失败（%s）", urls[i], err);
                }
            } else {
                log_write(LOG_DBG, "出网验证：%s 请求失败（%s）", urls[i], err);
            }
            continue;
        }
        got_response = 1;

        if (resp.status == 204 ||
            (resp.status == 200 && resp.body && has(resp.body, "User-agent"))) {
            if (!g_probe_quiet)
                log_write(LOG_DBG, "出网验证：%s → HTTP %d 通", urls[i], resp.status);
            http_resp_free(&resp);
            return 1;
        }
        /* 回了 200/302 但内容不是预期 → 多半被劫持到门户页，明确不通 */
        log_write(LOG_DBG, "出网验证：%s → HTTP %d，内容不符（疑似劫持）",
                  urls[i], resp.status);
        http_resp_free(&resp);
    }

    if (got_response) return 0;   /* 能收到响应但都不对 —— 确定出不去，判僵尸 */
    return -1;                    /* 一个都没搭上（可能目标被墙）—— 判不出来 */
}

ProbeResult probe_once(const Config *c) {
    ProbeResult r;
    HttpResp resp;
    char err[192];
    int secs;

    memset(&r, 0, sizeof r);
    memset(&resp, 0, sizeof resp);

    if (!c->probe_url[0]) {
        r.state = NET_UNKNOWN;
        snprintf(r.message, sizeof r.message, "未配置探测地址");
        return r;
    }

    secs = c->probe_timeout > 0 ? c->probe_timeout : 3;
    if (secs > 60) secs = 60;

    if (http_request("GET", c->probe_url, NULL, NULL, c->bind_ip,
                     secs * 1000, &resp, err, sizeof err) != 0) {
        r.state = NET_OFFLINE;
        snprintf(r.message, sizeof r.message, "探测失败：%s", err);
        return r;
    }

    /* 探测目标「可达」只是第 1 层。204 或 200+预期标识都算可达，但可达 ≠ 真在线：
     * 僵尸会话正是「探测域名能通、AC 还记着在线、但实际打不开网页」。
     * 所以第 1 层通过后统一走第 2 层（问 AC）+ 第 3 层（真的出得去），
     * 两层都过才判在线。 */
    if (resp.status == 204 ||
        (resp.status == 200 && c->probe_expect[0] &&
         has(resp.body, c->probe_expect) && resp.body_len <= 8192)) {
        int ac = 1;
        /* 第 2 层：问 AC 自己（防 DNS 劫持导致的假在线） */
        if (c->verify_ac) ac = ac_says_online(c);

        if (ac == 0) {
            r.state = NET_OFFLINE;
            snprintf(r.message, sizeof r.message,
                     "探测目标可达但认证服务器判定不在线（AC 会话多半已失效）");
            http_resp_free(&resp);
            return r;
        }

        /* 第 3 层：AC 说在线也得"真的出得去"。 */
        {
            int net = can_reach_internet(c);
            if (net == 0) {
                r.state = NET_OFFLINE;
                snprintf(r.message, sizeof r.message,
                         "探测目标可达但实际出不了网（疑似僵尸会话）");
                http_resp_free(&resp);
                return r;
            }
            /* net < 0 且 ac < 0：三层里只有第 1 层给了「可达」，AC 与出网
             * 都查不出来。维持在线是保守策略（不主动断网），但必须留痕 ——
             * 否则又是「程序说在线、用户上不了网」而日志里什么都查不到。
             * 限频 60 秒，避免瞬时抖动刷屏。 */
            if (net < 0 && ac < 0) {
                static time_t t_last_blind = 0;
                time_t now = time(NULL);
                if (now - t_last_blind >= 60) {
                    t_last_blind = now;
                    log_write(LOG_WARN,
                              "三层判定均无法确认（AC 复核与出网验证都不可达），暂时维持在线");
                }
            }
            /* net < 0：两个目标都够不着，判不出来，维持在线（宁可不折腾） */
        }

        r.state = NET_ONLINE;
        if (ac < 0)
            snprintf(r.message, sizeof r.message, "探测目标可达（AC 复核不可用）");
        else
            snprintf(r.message, sizeof r.message, "探测目标可达（%s）",
                     resp.status == 204 ? "HTTP 204" : "正文匹配预期标识");
        http_resp_free(&resp);
        return r;
    }

    if (resp.status >= 300 && resp.status < 400) {
        char abs_url[CNK_URL_MAX];
        char loc_host[CNK_HOST_MAX], probe_host[CNK_HOST_MAX];
        url_resolve(c->probe_url, resp.location, abs_url, sizeof abs_url);
        if (url_host_of(abs_url, loc_host, sizeof loc_host) == 0 &&
            url_host_of(c->probe_url, probe_host, sizeof probe_host) == 0 &&
            strcmp(loc_host, probe_host) != 0) {
            r.state = NET_PORTAL;
            snprintf(r.message, sizeof r.message, "被重定向到 %s", abs_url);
            snprintf(r.portal_url, sizeof r.portal_url, "%s", abs_url);
        } else {
            /* 同主机 3xx 不算劫持，但同样要过 AC 复核 + 出网验证（防僵尸会话） */
            int ac = 1;
            if (c->verify_ac) ac = ac_says_online(c);
            if (ac == 0) {
                r.state = NET_OFFLINE;
                snprintf(r.message, sizeof r.message,
                         "同主机重定向但 AC 判定不在线");
                http_resp_free(&resp);
                return r;
            }
            {
                int net = can_reach_internet(c);
                if (net == 0) {
                    r.state = NET_OFFLINE;
                    snprintf(r.message, sizeof r.message,
                             "同主机重定向但实际出不了网（疑似僵尸会话）");
                    http_resp_free(&resp);
                    return r;
                }
            }
            r.state = NET_ONLINE;
            snprintf(r.message, sizeof r.message, "同主机重定向，判定在线");
        }
        http_resp_free(&resp);
        return r;
    }

    if (resp.status == 200) {
        /* 走到这里说明：200 但正文不含预期标识或过长 → 疑似门户劫持页 */
        r.state = NET_PORTAL;
        snprintf(r.message, sizeof r.message, "%s",
                 resp.body_len > 8192 ? "正文长度异常，疑似门户劫持"
                                      : "正文不含预期标识，疑似门户劫持");
        http_resp_free(&resp);
        return r;
    }

    /* 4xx/5xx：宁可多试一次登录，也不把可疑响应当在线 */
    r.state = NET_PORTAL;
    snprintf(r.message, sizeof r.message, "HTTP %d", resp.status);
    http_resp_free(&resp);
    return r;
}

/* ---------------- 认证 ---------------- */

static int rand_v(void) {
    return 1000 + (int)((unsigned)time(NULL) + (unsigned)rand()) % 9000;
}

/* 拼 URL 并检查截断：snprintf 静默截断过一次 params，代价是"服务器说参数错误" */
static int build_url(char *out, size_t cap, char *err, size_t errcap,
                     const char *fmt, ...) {
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vsnprintf(out, cap, fmt, ap);
    va_end(ap);
    if (n < 0) { snprintf(err, errcap, "URL 格式化失败"); return -1; }
    if ((size_t)n >= cap) { snprintf(err, errcap, "URL 超出缓冲区 %u 字节", (unsigned)cap); return -1; }
    return 0;
}

static int build_payload(const Config *c, const char *account, const char *ip,
                         const char *rcn, char *out, size_t cap) {
    char acct[CNK_USER_MAX + 32];
    char esc_acct[CNK_USER_MAX * 2 + 64];
    char esc_pw[CNK_PASS_MAX * 2 + 8];
    char esc_ip[128];
    char esc_rcn[256];
    int n;

    snprintf(acct, sizeof acct, ",0,%s", account);
    if (!json_escape(acct, esc_acct, sizeof esc_acct)) return -1;
    if (!json_escape(c->password, esc_pw, sizeof esc_pw)) return -1;
    if (!json_escape(ip, esc_ip, sizeof esc_ip)) return -1;
    if (!json_escape(rcn, esc_rcn, sizeof esc_rcn)) return -1;

    n = snprintf(out, cap,
        "{\"login_method\":1"
        ",\"user_account\":\"%s\""
        ",\"user_password\":\"%s\""
        ",\"wlan_user_ip\":\"%s\""
        ",\"wlan_user_ipv6\":\"\",\"wlan_user_mac\":\"000000000000\""
        ",\"wlan_ac_ip\":\"\",\"wlan_ac_name\":\"\""
        ",\"jsVersion\":\"4.2.1\",\"login_t\":\"0\",\"js_status\":\"0\""
        ",\"is_page\":\"1\",\"is_page_new\":558"
        ",\"terminal_type\":1,\"lang\":\"zh-cn\""
        ",\"rcn\":\"%s\"}",
        esc_acct, esc_pw, esc_ip, esc_rcn);
    return (n > 0 && (size_t)n < cap) ? 0 : -1;
}

/* 取信息页里 msga='...' / msga="..." 的值（老式通道的厂商原始判定码，非 JSON 字段） */
static int js_var_string(const char *body, const char *name, char *out, size_t cap) {
    size_t nl = strlen(name);
    const char *p = body;
    while (p && *p) {
        const char *hit = strstr(p, name);
        const char *q;
        if (!hit) break;
        if (hit == body || !(isalnum((unsigned char)hit[-1]) || hit[-1] == '_')) {
            q = hit + nl;
            while (*q == ' ' || *q == '\t') ++q;
            if (*q == '=') {
                ++q;
                while (*q == ' ' || *q == '\t') ++q;
                if (*q == '\'' || *q == '"') {
                    const char quote = *q;
                    const char *start = ++q;
                    const char *end = strchr(start, quote);
                    if (end) {
                        size_t len = (size_t)(end - start);
                        if (len > cap - 1) len = cap - 1;
                        memcpy(out, start, len);
                        out[len] = '\0';
                        return 1;
                    }
                }
            }
        }
        p = hit + nl;
    }
    return 0;
}

/* 老式通道：仅在新式接口确实不存在时采信其判定 */
static LoginResult legacy_login(const Config *c, const char *account,
                                const char *origin) {
    LoginResult lr;
    HttpResp resp;
    char err[192];
    char enc_user[CNK_USER_MAX * 3 + 8];
    char enc_pass[CNK_PASS_MAX * 3 + 8];
    char body[1024];
    char url[CNK_URL_MAX];

    memset(&lr, 0, sizeof lr);
    memset(&resp, 0, sizeof resp);

    url_encode(account, strlen(account), enc_user, sizeof enc_user);
    url_encode(c->password, strlen(c->password), enc_pass, sizeof enc_pass);
    snprintf(body, sizeof body,
             "DDDDD=%s&upass=%s&R1=0&R2=0&R3=0&R6=0&para=00&0MKKey=123456"
             "&buttonClicked=&redirect_url=&err_flag=&username=&password=&user=&cmd=&Login=&v6ip=",
             enc_user, enc_pass);
    snprintf(url, sizeof url, "%s/eportal/?c=ACSetting&a=Login&url=drappall", origin);
    snprintf(lr.request_url, sizeof lr.request_url,
             "%s/eportal/?c=ACSetting&a=Login&url=drappall", origin);

    if (http_request("POST", url, body, "application/x-www-form-urlencoded",
                     c->bind_ip, 8000, &resp, err, sizeof err) != 0) {
        snprintf(lr.message, sizeof lr.message, "老式通道请求失败：%s", err);
        return lr;
    }
    if (resp.status >= 400) {
        snprintf(lr.message, sizeof lr.message, "服务器返回 %d，交探测裁决", resp.status);
        http_resp_free(&resp);
        return lr;
    }

    {
        const int hit3 = has(resp.body, "Dr.COMWebLoginID_3.htm") || has(resp.location, "Dr.COMWebLoginID_3.htm");
        const int hit2 = has(resp.body, "Dr.COMWebLoginID_2.htm") || has(resp.location, "Dr.COMWebLoginID_2.htm");
        if (hit3 && hit2) {
            snprintf(lr.message, sizeof lr.message, "响应同时含成功与失败标志，交探测裁决");
        } else if (hit3) {
            lr.ok = 1;
            snprintf(lr.message, sizeof lr.message, "老式通道登录成功");
        } else if (hit2) {
            char msga[80];
            lr.cred_error = 1;
            snprintf(lr.message, sizeof lr.message, "服务器返回登录失败（账号或密码错误）");
            if (js_var_string(resp.body, "msga", msga, sizeof msga) && msga[0]) {
                size_t l = strlen(lr.message);
                snprintf(lr.message + l, sizeof lr.message - l, "；服务器信息：%s", msga);
            }
        } else {
            snprintf(lr.message, sizeof lr.message, "无明确标志，交探测裁决");
        }
    }
    http_resp_free(&resp);
    return lr;
}

LoginResult portal_login(const Config *c) {
    LoginResult lr;
    char scheme[8], host[CNK_HOST_MAX], port[8];
    char origin[CNK_URL_MAX];
    char api_base[CNK_URL_MAX];
    char api_port[8];
    char url[4096];
    char err[192];
    char ip[64] = "";
    char rcn[192] = "";
    char enc_ip[512];
    char payload[1024];
    char params[2048];
    char enc_params[6144];
    char account[CNK_USER_MAX + 32];
    HttpResp resp;
    int api_present = 0;
    long code = 0;
    char msg[CNK_MSG_MAX] = "";
    char ret_code[32] = "";
    const char *suffix;

    memset(&lr, 0, sizeof lr);
    memset(&resp, 0, sizeof resp);

    if (!c->username[0]) {
        snprintf(lr.message, sizeof lr.message, "账号为空，请填写 /etc/cnkrouter.conf");
        lr.cred_error = 1;
        return lr;
    }
    if (!c->password[0]) {
        snprintf(lr.message, sizeof lr.message, "密码为空，请填写 /etc/cnkrouter.conf");
        lr.cred_error = 1;
        return lr;
    }
    if (url_split(c->auth_url, scheme, sizeof scheme, host, sizeof host,
                  port, sizeof port, NULL, 0) != 0) {
        snprintf(lr.message, sizeof lr.message, "认证网址格式不正确：%s", c->auth_url);
        lr.cred_error = 1;
        return lr;
    }
    if (strcmp(scheme, "http") != 0) {
        snprintf(lr.message, sizeof lr.message,
                 "路由器版只支持 http:// 门户（当前 %s）；门户若是 https 请见 README 说明", scheme);
        lr.cred_error = 1;
        return lr;
    }

    if (port[0]) snprintf(origin, sizeof origin, "%s://%s:%s", scheme, host, port);
    else        snprintf(origin, sizeof origin, "%s://%s", scheme, host);

    /* 新式 API 端口：auth_url 显式端口优先，否则用实测的 801 */
    if (port[0]) snprintf(api_port, sizeof api_port, "%s", port);
    else         snprintf(api_port, sizeof api_port, "801");
    snprintf(api_base, sizeof api_base, "%s://%s:%s", scheme, host, api_port);

    suffix = isp_suffix(c);
    snprintf(account, sizeof account, "%s%s", c->username, suffix);

    /* 1) chkstatus：取本机（= 路由器 WAN 侧）校园网 IP，失败不致命 */
    if (build_url(url, sizeof url, err, sizeof err,
                  "%s/drcom/chkstatus?callback=dr8001&jsVersion=4.X&v=%d&lang=zh",
                  origin, rand_v()) != 0) {
        snprintf(lr.message, sizeof lr.message, "构造 chkstatus 请求失败：%s", err);
        return lr;
    }
    if (http_request("GET", url, NULL, NULL, c->bind_ip, 3000, &resp, err, sizeof err) == 0) {
        if (resp.status == 200) json_find_str(resp.body, "v46ip", ip, sizeof ip);
        http_resp_free(&resp);
    }
    if (ip[0]) {
        char b64ip[64];
        b64_encode((const uint8_t *)ip, strlen(ip), b64ip, sizeof b64ip);
        url_encode(b64ip, strlen(b64ip), enc_ip, sizeof enc_ip);
    } else {
        enc_ip[0] = '\0';
    }

    /* 2) loadConfig：取页面令牌 rcn（缺它服务器报「页面已超时」） */
    if (build_url(url, sizeof url, err, sizeof err,
                  "%s/eportal/portal/page/loadConfig?callback=dr8002&program_index=&wlan_vlan_id=0"
                  "&wlan_user_ip=%s&wlan_user_ipv6=&wlan_user_ssid=&wlan_user_areaid=&wlan_ac_ip="
                  "&wlan_ap_mac=000000000000&gw_id=000000000000&jsVersion=4.X&v=%d&lang=zh",
                  api_base, enc_ip, rand_v()) != 0) {
        snprintf(lr.message, sizeof lr.message, "构造 loadConfig 请求失败：%s", err);
        return lr;
    }
    if (http_request("GET", url, NULL, NULL, c->bind_ip, 3000, &resp, err, sizeof err) != 0) {
        snprintf(lr.message, sizeof lr.message, "loadConfig 请求失败：%s", err);
        return lr;
    }
    if (resp.status == 200) {
        if (json_find_str(resp.body, "rcn", rcn, sizeof rcn)) api_present = 1;
    }
    http_resp_free(&resp);

    if (!rcn[0]) {
        LoginResult legacy = legacy_login(c, account, origin);
        if (legacy.cred_error && api_present) {
            legacy.cred_error = 0;
            snprintf(legacy.message, sizeof legacy.message,
                     "新式接口存在但未下发 rcn；老式通道判定不可信（本部署恒回 error2），交探测裁决");
        }
        return legacy;
    }

    /* 3) AES 加密载荷并提交 */
    if (build_payload(c, account, ip, rcn, payload, sizeof payload) != 0) {
        snprintf(lr.message, sizeof lr.message, "登录载荷构造失败（字段过长）");
        return lr;
    }
    aes128_ecb_pkcs7_b64(AES_KEY, payload, params, sizeof params);
    if (!params[0]) {
        snprintf(lr.message, sizeof lr.message, "登录参数加密失败");
        return lr;
    }
    url_encode(params, strlen(params), enc_params, sizeof enc_params);

    snprintf(lr.request_url, sizeof lr.request_url,
             "%s/eportal/portal/login?callback=dr8003&params=***&jsVersion=4.X&v=***&lang=zh",
             api_base);
    if (build_url(url, sizeof url, err, sizeof err,
                  "%s/eportal/portal/login?callback=dr8003&params=%s&jsVersion=4.X&v=%d&lang=zh",
                  api_base, enc_params, rand_v()) != 0) {
        snprintf(lr.message, sizeof lr.message, "构造 login 请求失败：%s", err);
        return lr;
    }

    if (http_request("GET", url, NULL, NULL, c->bind_ip, 8000, &resp, err, sizeof err) != 0) {
        snprintf(lr.message, sizeof lr.message, "login 请求失败：%s", err);
        return lr;
    }
    if (resp.status != 200) {
        snprintf(lr.message, sizeof lr.message, "login 返回 HTTP %d", resp.status);
        http_resp_free(&resp);
        return lr;
    }

    json_find_int(resp.body, "result", &code);
    json_find_str(resp.body, "msg", msg, sizeof msg);
    json_find_str(resp.body, "ret_code", ret_code, sizeof ret_code);
    http_resp_free(&resp);

    /* 原始字段留证：以前只把解析后的结论写进日志，出现没见过的回包时无从判断。
     * msg 是服务器回包、文案表未采齐，落盘前先抹掉账号/密码（防回显学号）。 */
    {
        char msg_log[CNK_MSG_MAX];
        snprintf(msg_log, sizeof msg_log, "%s", msg);
        redact_secret(msg_log, c->username);
        redact_secret(msg_log, c->password);
        log_write(LOG_DBG, "登录回包：result=%ld ret_code=%s msg=%s",
                  code, ret_code[0] ? ret_code : "-", msg_log[0] ? msg_log : "-");
    }

    if (code == 1) {
        lr.ok = 1;
        snprintf(lr.message, sizeof lr.message, "Portal 协议认证成功");
        return lr;
    }

    /* 「终端IP已在线」= 链路已通、无需重登，按成功处理（必须含「终端」）
     * 但要把 inuse 标出来：这不一定真成功——AC 库里有记录、转发面却已失效时，
     * 重试再多遍也没用（用户实测过：网页卡死、脚本重试全无效，拔插网线才恢复）。
     * 上层据此走注销/换 IP/换 MAC，而不是傻等重试。 */
    if (has(msg, "已在线") && has(msg, "终端")) {
        lr.ok = 1;
        lr.inuse = 1;
        snprintf(lr.message, sizeof lr.message, "服务器报告该终端已在线（inuse）");
        return lr;
    }

    /* 英文变体：2026-11-01 真机实测拿到 "inuse, login again"。
     * 以前只认中文，它就掉进"登录失败"分支 —— 会话明明已经存在，程序却一遍遍重试登录，
     * 也永远进不了僵尸会话处置（用户早就要求过 inuse 必须有专门处理）。
     * 依旧标 inuse 而不是单纯 ok：会话存在 ≠ 转发面可用，上层会复测，
     * 复测不通才走注销/换 IP/换 MAC。
     * 排除含否定词的回包，避免把真正的失败当成已在线放过去——否定词中英文都要查，
     * 否则英文否定（"inuse failed" / "account not exist"）会漏过去。 */
    {
        const int says_fail = has(msg, "错误") || has(msg, "不存在") ||
                              has(msg, "不正确") || has(msg, "无效") ||
                              has_i(msg, "error") || has_i(msg, "fail") ||
                              has_i(msg, "invalid") || has_i(msg, "denied") ||
                              has_i(msg, "not exist");
        if (has_i(msg, "inuse") && !says_fail) {
            lr.ok = 1;
            lr.inuse = 1;
            snprintf(lr.message, sizeof lr.message, "服务器报告该 IP 上已有会话（inuse）");
            return lr;
        }
    }

    if (ret_code[0])
        snprintf(lr.message, sizeof lr.message, "登录失败：%s（ret_code=%s）",
                 msg[0] ? msg : "（无消息）", ret_code);
    else
        snprintf(lr.message, sizeof lr.message, "登录失败：%s",
                 msg[0] ? msg : "（无消息）");

    /* 凭据类失败：必须同时出现「凭据词」与「否定词」，
     * 否则「获取用户信息失败，请稍后重试」会被误判成密码错误而永久停摆 */
    {
        const int mentions_cred = has(msg, "密码") || has(msg, "账号") ||
                                  has(msg, "用户名") || has(msg, "用户");
        const int says_invalid = has(msg, "错误") || has(msg, "不正确") ||
                                 has(msg, "不存在") || has(msg, "无效");
        lr.cred_error = (mentions_cred && says_invalid) ? 1 : 0;
    }
    return lr;
}

/* ---------------- 主动注销 ---------------- */

/* 把服务器侧的会话杀掉，让下一次 login 能真正建立新会话。
 *
 * 为什么需要：AC 的会话是按 **IP** 记的（chkstatus 回包里只有 v4ip/ss5，没有 MAC）。
 * 当它不是"没认证"而是"记着你已认证、转发面却已失效"时，重登只会收到
 * 「终端IP已在线」，客户端判成功、实际照样上不了网 —— 死循环重试永远不会恢复，
 * 用户实测只有拔插网线（换 IP）才好。注销就是软件版的"踢掉这条记录"。
 *
 * 地址不靠猜：门户首页会下发 logoutport / logoutpath / logoutparam，
 * 本网实测是 :801 + /eportal/?c=ACSetting&a=Logout&ver=1.0&url=drappall。
 * 先按 801 试，失败再回落 auth_url 自带端口（兼容别的部署）。
 *
 * 返回 0 = 请求发出去了（不等于服务器真的注销成功，要重新探测才知道）；
 * 严格说这个返回值只代表"动作已尝试"。 */
int portal_logout(const Config *c) {
    char scheme[16], host[CNK_HOST_MAX], port[16], path[CNK_URL_MAX];
    char url[CNK_URL_MAX * 2], err[128], brief[128];
    HttpResp resp;

    if (!c->auth_url[0]) return -1;
    if (url_split(c->auth_url, scheme, sizeof scheme, host, sizeof host,
                  port, sizeof port, path, sizeof path) != 0)
        return -1;

    snprintf(url, sizeof url,
             "http://%s:801/eportal/?c=ACSetting&a=Logout&ver=1.0&url=drappall", host);

    memset(&resp, 0, sizeof resp);
    if (http_request("GET", url, NULL, NULL, c->bind_ip, 5000, &resp, err, sizeof err) != 0) {
        log_write(LOG_DBG, "注销：%s:801 不通（%s），回落默认端口", host, err);
        if (port[0])
            snprintf(url, sizeof url,
                     "http://%s:%s/eportal/?c=ACSetting&a=Logout&ver=1.0&url=drappall",
                     host, port);
        else
            snprintf(url, sizeof url,
                     "http://%s/eportal/?c=ACSetting&a=Logout&ver=1.0&url=drappall", host);
        memset(&resp, 0, sizeof resp);
        if (http_request("GET", url, NULL, NULL, c->bind_ip, 5000, &resp, err, sizeof err) != 0) {
            log_write(LOG_WARN, "注销：两个地址都没发出去（%s）", err);
            return -1;
        }
    }

    snprintf(brief, sizeof brief, "%.*s", 120, resp.body ? resp.body : "");
    log_write(LOG_INFO, "注销：HTTP %d %s", resp.status, brief);
    http_resp_free(&resp);
    return 0;
}
