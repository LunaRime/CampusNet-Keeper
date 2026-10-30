/* cnkrouter 主程序：配置、CLI、守护循环（对应 Windows 版 core/Scheduler.cpp）。
 *
 * 状态机与 Windows 版一致：
 *   探测 → 在线：等 probe_interval
 *        → 离线：3 秒快速重探（改善恢复延迟）
 *        → 门户：登录 → 复测轮询 5×1.5s → 在线
 *   登录失败：指数退避 15/30/60/120/300 秒
 *   凭据错误：停止重试，转 60 秒低频巡检（改配置后重启服务即恢复）
 *   连续 3 次失败且从未成功：判为非校园网，同样转低频巡检
 */
#include "cnk.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdarg.h>
#include <signal.h>
#include <time.h>

static int   g_level = LOG_INFO;
static FILE *g_logf  = NULL;
static char  g_log_path[CNK_PATH_MAX];   /* 空 = 不落盘 */
static long  g_log_bytes = 0;            /* 当前文件已写字节 */
static long  g_log_max_kb = 0;           /* <=0 不轮转 */
static int   g_log_keep = 0;             /* <=0 不轮转 */
static volatile sig_atomic_t g_stop = 0;
static volatile sig_atomic_t g_reload = 0;
/* SIGHUP 必须重载**同一个**文件：init 脚本用的是 /var/etc/cnkrouter.conf，
 * 写死 /etc/cnkrouter.conf 会让热重载静默读到另一份（或读不到） */
static char g_config_path[256] = "/etc/cnkrouter.conf";

/* ---------------- 日志 ---------------- */

void log_set_rotate(long max_kb, int keep) {
    g_log_max_kb = max_kb > 0 ? max_kb : 0;
    g_log_keep   = keep > 0 ? keep : 0;
}

/* 轮转：<path> → <path>.1 → <path>.2 …，最老的删掉，再重开一个空文件。
 * 只在写满时调用；256 KB、每 60 秒一行的话大概几天到一周触发一次，开销可忽略。 */
static void log_rotate(void) {
    char src[CNK_PATH_MAX + 16], dst[CNK_PATH_MAX + 16];
    int i;

    if (!g_log_path[0] || g_log_keep <= 0) return;
    if (g_logf) { fclose(g_logf); g_logf = NULL; }

    snprintf(src, sizeof src, "%s.%d", g_log_path, g_log_keep);
    remove(src);
    for (i = g_log_keep - 1; i >= 1; --i) {
        snprintf(src, sizeof src, "%s.%d", g_log_path, i);
        snprintf(dst, sizeof dst, "%s.%d", g_log_path, i + 1);
        rename(src, dst);
    }
    snprintf(dst, sizeof dst, "%s.1", g_log_path);
    rename(g_log_path, dst);

    g_logf = fopen(g_log_path, "a");
    g_log_bytes = 0;
    if (!g_logf) fprintf(stderr, "[warn] 轮转后无法重开日志 %s\n", g_log_path);
}

void log_init(int level, const char *path) {
    g_level = level;
    if (path && path[0]) {
        snprintf(g_log_path, sizeof g_log_path, "%s", path);
        g_logf = fopen(path, "a");
        if (g_logf) {
            /* 从现有大小接着算：否则每次重启都把计数清零，长期看还是会涨 */
            if (fseek(g_logf, 0, SEEK_END) == 0) {
                long sz = ftell(g_logf);
                g_log_bytes = sz > 0 ? sz : 0;
            }
        } else {
            fprintf(stderr, "[warn] 无法打开日志文件 %s，改用 stderr\n", path);
        }
    }
}

void log_write(int level, const char *fmt, ...) {
    char ts[32];
    char line[768];
    int n;
    time_t t = time(NULL);
    struct tm tmv;
    FILE *out;
    va_list ap;

    if (level > g_level) return;
#ifdef _WIN32
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", &tmv);

    /* 先格式化到缓冲：一个是为了算清写了多少字节（轮转要用），
     * 另一个是避免 stderr 与文件各写一遍导致两边内容不一致 */
    n = snprintf(line, sizeof line, "%s [%s] ", ts,
                 level == LOG_ERR ? "ERROR" : level == LOG_WARN ? "WARN" :
                 level == LOG_INFO ? "INFO" : "DEBUG");
    if (n < 0) return;
    if (n > (int)sizeof line - 2) n = (int)sizeof line - 2;
    va_start(ap, fmt);
    {
        int m = vsnprintf(line + n, sizeof line - (size_t)n, fmt, ap);
        if (m > 0) {
            n += m;
            if (n > (int)sizeof line - 2) n = (int)sizeof line - 2;
        }
    }
    va_end(ap);

    out = g_logf ? g_logf : stderr;
    fputs(line, out);
    fputc('\n', out);
    fflush(out);

    /* 只有落盘才累加；stderr 交给 logd 自己的 ring buffer 滚，不会撑爆存储 */
    if (g_logf && g_log_max_kb > 0 && g_log_keep > 0) {
        g_log_bytes += (long)n + 1;
        if (g_log_bytes > g_log_max_kb * 1024) log_rotate();
    }
}

/* ---------------- 配置 ---------------- */

void config_defaults(Config *c) {
    memset(c, 0, sizeof *c);
    /* 默认值取自 Windows 版真机实测：msftncsi 在本校园网 0/4 可达，
     * 用 apple 探测页 5/5、平均 86ms，故默认值就是它 */
    snprintf(c->auth_url, sizeof c->auth_url, "http://10.0.0.1/");
    snprintf(c->isp, sizeof c->isp, "campus");
    snprintf(c->probe_url, sizeof c->probe_url, "http://captive.apple.com/hotspot-detect.html");
    snprintf(c->probe_expect, sizeof c->probe_expect, "Success");
    c->probe_timeout = 6;
    c->probe_interval = 60;
    c->log_level = LOG_INFO;
    /* 日志落盘默认开：logd 的 ring buffer 只有 128 KB，排查"某时刻为何没重连"时
     * 历史太短。256 KB × 3 份 = 最多 1 MB，对 1.7 GB 的 overlay 无压力，且不会涨。 */
    snprintf(c->log_file, sizeof c->log_file, "/var/log/cnkrouter.log");
    c->log_max_kb = 256;
    c->log_keep = 3;
    c->verify_ac = 1;   /* 探测成功后用 chkstatus 复核，防 DNS 劫持导致的"假在线" */
}

static void trim_inplace(char *s) {
    char *p = s;
    size_t n;
    while (*p == ' ' || *p == '\t') ++p;
    if (p != s) memmove(s, p, strlen(p) + 1);
    n = strlen(s);
    while (n && (s[n - 1] == '\r' || s[n - 1] == '\n' || s[n - 1] == ' ' || s[n - 1] == '\t'))
        s[--n] = '\0';
    if (n >= 2 && ((s[0] == '"' && s[n - 1] == '"') || (s[0] == '\'' && s[n - 1] == '\''))) {
        memmove(s, s + 1, n - 2);
        s[n - 2] = '\0';
    }
}

static void copy_field(char *dst, size_t cap, const char *v) {
    snprintf(dst, cap, "%s", v);
}

int config_load(const char *path, Config *c, char *err, size_t cap) {
    FILE *f;
    char line[512];
    int lineno = 0;

    f = fopen(path, "r");
    if (!f) {
        snprintf(err, cap, "无法打开配置文件 %s", path);
        return -1;
    }
    while (fgets(line, sizeof line, f)) {
        char *eq = NULL;
        char *key = line;
        char *val;
        ++lineno;
        trim_inplace(line);
        if (!line[0] || line[0] == '#') continue;
        eq = strchr(line, '=');
        if (!eq) {
            snprintf(err, cap, "第 %d 行缺少 '='（格式：key=value）", lineno);
            fclose(f);
            return -1;
        }
        *eq = '\0';
        key = line;
        val = eq + 1;
        trim_inplace(key);
        trim_inplace(val);

        if (strcmp(key, "auth_url") == 0)        copy_field(c->auth_url, sizeof c->auth_url, val);
        else if (strcmp(key, "username") == 0)   copy_field(c->username, sizeof c->username, val);
        else if (strcmp(key, "password") == 0)   copy_field(c->password, sizeof c->password, val);
        else if (strcmp(key, "isp") == 0)        copy_field(c->isp, sizeof c->isp, val);
        else if (strcmp(key, "probe_url") == 0)  copy_field(c->probe_url, sizeof c->probe_url, val);
        else if (strcmp(key, "probe_expect") == 0) copy_field(c->probe_expect, sizeof c->probe_expect, val);
        else if (strcmp(key, "probe_timeout") == 0)  c->probe_timeout = atoi(val);
        else if (strcmp(key, "probe_interval") == 0) c->probe_interval = atoi(val);
        else if (strcmp(key, "bind_ip") == 0)    copy_field(c->bind_ip, sizeof c->bind_ip, val);
        else if (strcmp(key, "log_level") == 0)  c->log_level = atoi(val);
        else if (strcmp(key, "log_file") == 0)   copy_field(c->log_file, sizeof c->log_file, val);
        else if (strcmp(key, "log_max_kb") == 0) c->log_max_kb = atoi(val);
        else if (strcmp(key, "log_keep") == 0)   c->log_keep = atoi(val);
        else if (strcmp(key, "verify_ac") == 0)  c->verify_ac = atoi(val) ? 1 : 0;
        else log_write(LOG_WARN, "配置第 %d 行：未知键 '%s'（已忽略）", lineno, key);
    }
    fclose(f);

    if (c->probe_timeout <= 0 || c->probe_timeout > 60) c->probe_timeout = 6;
    if (c->probe_interval < 10) c->probe_interval = 60;
    return 0;
}

const char *isp_suffix(const Config *c) {
    if (strncmp(c->isp, "custom:", 7) == 0) return c->isp + 7;
    if (strcmp(c->isp, "cmcc") == 0)    return "@cmcc";
    if (strcmp(c->isp, "telecom") == 0) return "@dx";
    if (strcmp(c->isp, "unicom") == 0)  return "@lt";
    return "";
}

/* ---------------- 复测 ---------------- */

static int wait_online(const Config *c, int tries, int interval_ms) {
    int i;
    for (i = 0; i < tries; ++i) {
        ProbeResult pr;
        if (i) sleep_ms(interval_ms);
        if (g_stop) return 0;
        pr = probe_once(c);
        if (pr.state == NET_ONLINE) {
            log_write(LOG_INFO, "复测结果：已在线 OK（第 %d 次探测）", i + 1);
            return 1;
        }
        log_write(LOG_DBG, "复测第 %d 次：%s（%s）", i + 1,
                  net_state_name(pr.state), pr.message);
    }
    log_write(LOG_WARN, "登录后复测 %d 次仍未在线", tries);
    return 0;
}

static void sleep_slices(int ms) {
    while (ms > 0 && !g_stop) {
        int step = ms > 200 ? 200 : ms;
        sleep_ms(step);
        ms -= step;
    }
}

/* ---------------- 运行模式 ---------------- */

static int run_once(Config *c) {
    ProbeResult pr = probe_once(c);
    log_write(LOG_INFO, "探测：%s（%s）", net_state_name(pr.state), pr.message);

    if (pr.state == NET_ONLINE) return 0;
    if (pr.state == NET_UNKNOWN) return 4;
    if (pr.state == NET_OFFLINE) return 3;

    {
        LoginResult lr = portal_login(c);
        log_write(lr.ok ? LOG_INFO : LOG_WARN, "登录：%s", lr.message);
        log_write(LOG_DBG, "请求地址：%s", lr.request_url);
        if (!lr.ok) return 2;
    }
    return wait_online(c, 5, 1500) ? 0 : 2;
}

static int run_daemon(Config *c) {
    static const int backoff[] = {15, 30, 60, 120, 300};
    int bidx = 0, total = 0, ever_ok = 0, stopped = 0;

    log_write(LOG_INFO, "cnkrouter %s 启动：账号=%s%s 认证地址=%s 探测间隔=%ds",
              CNK_VERSION, c->username, isp_suffix(c), c->auth_url, c->probe_interval);

    while (!g_stop) {
        ProbeResult pr;

        if (g_reload) {
            char err[128];
            g_reload = 0;
            if (config_load(g_config_path, c, err, sizeof err) == 0) {
                log_write(LOG_INFO, "配置已重新加载");
            } else {
                log_write(LOG_ERR, "配置重载失败：%s（继续用旧配置）", err);
            }
            stopped = 0;
            total = 0;
            bidx = 0;
        }

        pr = probe_once(c);
        if (g_stop) break;

        if (pr.state == NET_ONLINE) {
            if (stopped) log_write(LOG_INFO, "网络已恢复，解除停止状态");
            if (!ever_ok) log_write(LOG_INFO, "网络在线：%s", pr.message);
            /* 提到 INFO：默认级别下也能看到每次探测（每行 ~60 字节，
             * 60 秒一次 ≈ 86 KB/天，256 KB 轮转足够撑 3 天） */
            else log_write(LOG_INFO, "在线：%s", pr.message);
            ever_ok = 1;
            stopped = 0;
            total = 0;
            bidx = 0;
            sleep_slices(c->probe_interval * 1000);
            continue;
        }

        if (stopped) {                      /* 终止态：低频巡检 */
            sleep_slices(60000);
            continue;
        }
        if (pr.state == NET_UNKNOWN) {
            log_write(LOG_WARN, "配置无效：%s", pr.message);
            sleep_slices(10000);
            continue;
        }
        if (pr.state == NET_OFFLINE) {
            log_write(LOG_INFO, "离线：%s", pr.message);
            sleep_slices(3000);             /* 快速重探 */
            continue;
        }

        log_write(LOG_INFO, "检测到门户劫持：%s", pr.message);
        {
            LoginResult lr = portal_login(c);
            ++total;
            log_write(lr.ok ? LOG_INFO : LOG_WARN, "登录：%s", lr.message);

            if (lr.ok) {
                if (wait_online(c, 5, 1500)) {
                    log_write(LOG_INFO, "已恢复在线");
                    ever_ok = 1;
                    total = 0;
                    bidx = 0;
                } else {
                    log_write(LOG_WARN, "接口报成功但复测不在线，15 秒后重试");
                    sleep_slices(15000);
                }
                continue;
            }

            if (lr.cred_error) {
                stopped = 1;
                log_write(LOG_ERR, "确认凭据错误，停止自动重试（改配置后重启服务）");
                sleep_slices(60000);
                continue;
            }
            if (!ever_ok && total >= 3) {
                stopped = 1;
                log_write(LOG_ERR, "连续 %d 次失败且从未成功，疑似非校园网，停止自动重试", total);
                sleep_slices(60000);
                continue;
            }
            {
                int w = backoff[bidx];
                if (bidx < (int)(sizeof backoff / sizeof backoff[0]) - 1) ++bidx;
                log_write(LOG_INFO, "将在 %d 秒后重试", w);
                sleep_slices(w * 1000);
            }
        }
    }
    log_write(LOG_INFO, "收到退出信号，cnkrouter 结束");
    return 0;
}

/* ---------------- 自检 ---------------- */

static int hex_to_bytes(const char *hex, uint8_t *out, size_t cap) {
    size_t i, n = strlen(hex) / 2;
    if (n > cap) return -1;
    for (i = 0; i < n; ++i) {
        unsigned v;
        if (sscanf(hex + i * 2, "%2x", &v) != 1) return -1;
        out[i] = (uint8_t)v;
    }
    return (int)n;
}

static int selftest(void) {
    int fails = 0;
    uint8_t key[16], pt[16], ct[16], exp[16];
    char buf[128];

    /* FIPS-197 C.1：AES-128 单块向量 */
    {
        int i;
        for (i = 0; i < 16; ++i) { key[i] = (uint8_t)i; pt[i] = (uint8_t)(i * 0x11); }
        hex_to_bytes("69c4e0d86a7b0430d8cdb78070b4c55a", exp, sizeof exp);
        aes128_ecb_encrypt_raw(key, pt, 16, ct);
        if (memcmp(ct, exp, 16) != 0) {
            log_write(LOG_ERR, "自检失败：AES-128 FIPS-197 向量不匹配");
            ++fails;
        } else {
            log_write(LOG_INFO, "自检通过：AES-128 FIPS-197 向量");
        }
    }

    /* Base64 与 URL 编码 */
    b64_encode((const uint8_t *)"abc", 3, buf, sizeof buf);
    if (strcmp(buf, "YWJj") != 0) { log_write(LOG_ERR, "自检失败：Base64(abc)=%s", buf); ++fails; }
    else log_write(LOG_INFO, "自检通过：Base64 编码");

    {
        uint8_t dec[8];
        int n = b64_decode("YWJj", 4, dec, sizeof dec);
        if (n != 3 || memcmp(dec, "abc", 3) != 0) {
            log_write(LOG_ERR, "自检失败：Base64 解码");
            ++fails;
        }
    }

    url_encode("a+b/c=", 6, buf, sizeof buf);
    if (strcmp(buf, "a%2Bb%2Fc%3D") != 0) {
        log_write(LOG_ERR, "自检失败：UrlEncode=%s", buf);
        ++fails;
    } else {
        log_write(LOG_INFO, "自检通过：URL 编码（Base64 的 + / = 已转义）");
    }

    /* JSON 取值（含中文与转义） */
    {
        const char *j = "dr8002({\"result\":0,\"msg\":\"\\u7ec8\\u7aefIP\\u5df2\\u5728\\u7ebf\\uff01\",\"data\":{\"rcn\":\"abc123\"}})";
        char v[64];
        if (!json_find_str(j, "rcn", v, sizeof v) || strcmp(v, "abc123") != 0) {
            log_write(LOG_ERR, "自检失败：rcn 提取");
            ++fails;
        }
        if (!json_find_str(j, "msg", v, sizeof v) || strcmp(v, "终端IP已在线！") != 0) {
            log_write(LOG_ERR, "自检失败：\\uXXXX 解码（得到 '%s'）", v);
            ++fails;
        }
    }

    if (fails) log_write(LOG_ERR, "自检未通过：%d 项失败", fails);
    else       log_write(LOG_INFO, "自检全部通过");
    return fails ? 1 : 0;
}

/* ---------------- main ---------------- */

static void on_signal(int sig) {
    (void)sig;
    g_stop = 1;
}

#ifndef _WIN32
static void on_hup(int sig) {
    (void)sig;
    g_reload = 1;
}
#endif

static void usage(void) {
    printf(
        "cnkrouter %s —— 校园网自动认证（路由器端）\n\n"
        "用法：cnkrouter [选项]\n\n"
        "  --config=<路径>   配置文件（默认 /etc/cnkrouter.conf）\n"
        "  --daemon          常驻运行（由 procd 托管）\n"
        "  --once            探测 + 必要时登录 + 复测，退出码 0=在线\n"
        "  --login           直接发起一次登录（不探测，调试用）\n"
        "  --probe           只探测一次，退出码 0=在线 3=离线 2=门户\n"
        "  --user=<账号>      覆盖配置里的账号\n"
        "  --pass=<密码>      覆盖配置里的密码\n"
        "  --verbose         输出调试日志\n"
        "  --quiet           只输出错误\n"
        "  --selftest        本地自检（AES/Base64/URL/JSON，不联网）\n"
        "  --version         显示版本\n"
        "  --help            显示本帮助\n",
        CNK_VERSION);
}

int main(int argc, char **argv) {
    Config cfg;
    const char *config_path = g_config_path;
    const char *user = NULL, *pass = NULL;
    int mode = 0;   /* 0=help 1=daemon 2=once 3=login 4=probe 5=selftest */
    int i, rc;
    char err[160];

    config_defaults(&cfg);

    for (i = 1; i < argc; ++i) {
        const char *a = argv[i];
        if (strncmp(a, "--config=", 9) == 0) {
            snprintf(g_config_path, sizeof g_config_path, "%s", a + 9);
            config_path = g_config_path;
        }
        else if (strcmp(a, "--daemon") == 0) mode = 1;
        else if (strcmp(a, "--once") == 0) mode = 2;
        else if (strcmp(a, "--login") == 0) mode = 3;
        else if (strcmp(a, "--probe") == 0) mode = 4;
        else if (strcmp(a, "--selftest") == 0) mode = 5;
        else if (strncmp(a, "--user=", 7) == 0) user = a + 7;
        else if (strncmp(a, "--pass=", 7) == 0) pass = a + 7;
        else if (strcmp(a, "--verbose") == 0) cfg.log_level = LOG_DBG;
        else if (strcmp(a, "--quiet") == 0) cfg.log_level = LOG_ERR;
        else if (strcmp(a, "--version") == 0) { printf("cnkrouter %s\n", CNK_VERSION); return 0; }
        else if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) { usage(); return 0; }
        else { fprintf(stderr, "无法识别的参数：%s\n\n", a); usage(); return 64; }
    }

    if (mode == 0) { usage(); return 0; }

    net_init();
    /* 路由器上不落盘写日志（/tmp 是 tmpfs，写文件等于吃内存）：
     * stderr 由 procd 收进 logd，`logread -e cnkrouter` 即可查看 */
    log_init(cfg.log_level, NULL);

    if (mode == 5) return selftest();

    rc = config_load(config_path, &cfg, err, sizeof err);
    if (rc != 0) {
        log_write(LOG_ERR, "%s", err);
        return 4;
    }
    if (user) snprintf(cfg.username, sizeof cfg.username, "%s", user);
    if (pass) snprintf(cfg.password, sizeof cfg.password, "%s", pass);

    /* 配置读到了再开文件日志：daemon 才落盘（一次性命令打到终端就够了，
     * 避免 --probe 之类的临时调用污染服务日志）。 */
    if (mode == 1) {
        log_init(cfg.log_level, cfg.log_file);
        log_set_rotate(cfg.log_max_kb, cfg.log_keep);
    } else {
        log_init(cfg.log_level, NULL);
    }

    if (!cfg.username[0]) {
        log_write(LOG_ERR, "配置里没有账号（username=），请编辑 %s", config_path);
        return 4;
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
#ifndef _WIN32
    signal(SIGHUP, on_hup);
#endif

    if (mode == 4) {
        ProbeResult pr = probe_once(&cfg);
        printf("%s\t%s\n", net_state_name(pr.state), pr.message);
        return pr.state == NET_ONLINE ? 0 : (pr.state == NET_PORTAL ? 2 : 3);
    }
    if (mode == 3) {
        LoginResult lr = portal_login(&cfg);
        printf("%s\t%s\n", lr.ok ? "ok" : "fail", lr.message);
        return lr.ok ? 0 : 2;
    }
    if (mode == 2) return run_once(&cfg);
    return run_daemon(&cfg);
}
