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

/* ---------------- 探测 ---------------- */

const char *net_state_name(NetState s) {
    switch (s) {
        case NET_ONLINE:  return "online";
        case NET_PORTAL:  return "portal";
        case NET_OFFLINE: return "offline";
        default:          return "unknown";
    }
}

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
    if (http_request("GET", url, NULL, NULL, c->bind_ip, 4000, &resp, err, sizeof err) != 0)
        return -1;
    if (resp.status == 200 && json_find_int(resp.body, "result", &code) == 0)
        verdict = (code == 1) ? 1 : 0;
    http_resp_free(&resp);
    return verdict;
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

    if (resp.status == 204) {
        r.state = NET_ONLINE;
        snprintf(r.message, sizeof r.message, "HTTP 204");
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
            r.state = NET_ONLINE;
            snprintf(r.message, sizeof r.message, "同主机重定向，判定在线");
        }
        http_resp_free(&resp);
        return r;
    }

    if (resp.status == 200) {
        const int has_expect = c->probe_expect[0] && has(resp.body, c->probe_expect);
        const int too_long = resp.body_len > 8192;
        if (has_expect && !too_long) {
            /* 正文匹配成功也不能全信：本网的探测域名被 DNS 劫持到国内 CDN，
             * AC 会话失效后它仍然回 Success（见 ac_says_online 的注释）。
             * 让认证服务器自己说一句，只有它也认为在线才算在线。 */
            if (c->verify_ac) {
                int ac = ac_says_online(c);
                if (ac == 0) {
                    r.state = NET_OFFLINE;
                    snprintf(r.message, sizeof r.message,
                             "正文匹配但认证服务器判定不在线（AC 会话多半已失效）");
                    http_resp_free(&resp);
                    return r;
                }
                if (ac < 0) {
                    /* 查不到就不改判，但要留痕：连续出现说明这台 AC 没有 chkstatus 接口 */
                    snprintf(r.message, sizeof r.message,
                             "正文匹配（AC 复核不可用，按在线处理）");
                    r.state = NET_ONLINE;
                    http_resp_free(&resp);
                    return r;
                }
            }
            r.state = NET_ONLINE;
            snprintf(r.message, sizeof r.message, "正文匹配预期标识");
        } else {
            r.state = NET_PORTAL;
            snprintf(r.message, sizeof r.message, "%s",
                     too_long ? "正文长度异常，疑似门户劫持" : "正文不含预期标识，疑似门户劫持");
        }
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

    if (code == 1) {
        lr.ok = 1;
        snprintf(lr.message, sizeof lr.message, "Portal 协议认证成功");
        return lr;
    }

    /* 「终端IP已在线」= 链路已通、无需重登，按成功处理（必须含「终端」） */
    if (has(msg, "已在线") && has(msg, "终端")) {
        lr.ok = 1;
        snprintf(lr.message, sizeof lr.message, "服务器报告该终端已在线（无需重新登录）");
        return lr;
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
