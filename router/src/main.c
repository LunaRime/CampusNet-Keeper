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
#ifdef _WIN32
#define strncasecmp _strnicmp
#endif
#ifndef _WIN32
#include <unistd.h>   /* unlink：软拔插标记文件（见 replug_flag_*） */
#include <sys/stat.h> /* chmod：日志文件含现场信息，不能是默认 644 */
#endif

static int   g_level = LOG_INFO;
static FILE *g_logf  = NULL;
static char  g_log_dir[CNK_PATH_MAX];   /* 空 = 不落盘 */
static char  g_log_date[16];            /* 当前打开的文件对应哪天 */

/* 这三个原来夹在日志变量和配置段之间，上一次整块替换日志模块时被我一起删掉了
 * （CI 报 g_stop/g_reload/g_config_path undeclared）。它们不属于日志，必须留在这。 */
static volatile sig_atomic_t g_stop = 0;
static volatile sig_atomic_t g_reload = 0;
/* SIGHUP 必须重载**同一个**文件：init 脚本用的是 /var/etc/cnkrouter.conf，
 * 写死 /etc/cnkrouter.conf 会让热重载静默读到另一份（或读不到） */
static char g_config_path[256] = "/etc/cnkrouter.conf";

/* 配置里的目录名直接进 system()，防一手注入（配置文件本身 600，但别留口子） */
static int path_is_safe(const char *p) {
    return p[0] && !strpbrk(p, "'\"`;$|&<>()\n");
}

/* ---------------- 日志出口（R3：唯一的决策点） ----------------
 *
 * 三个出口职责不同，别再在别处判断：
 *   ① 文件（log_dir 可写）：**详细历史**，按天分文件，完整保留 level <= g_level；
 *   ② 系统日志（stderr → procd → logd）：**只写关键状态与告警**（<= LOG_INFO）。
 *      logd 只有 128KB 环形缓冲，把 DEBUG 灌进去等于把历史冲掉；
 *   ③ 回退目录（持久盘不可用）：/tmp/cnkrouter（tmpfs，吃内存不吃 flash），
 *      自动降为 INFO，并限频告警。**绝不往 overlay(/) 上找地方写**。
 *
 * 旧实现是二选一（有文件就不打 stderr、没文件才打），于是：
 *   · 正常运行时 `logread` 看不到任何状态，只能翻数据盘；
 *   · 数据盘挂了就再也看不到"为什么"；
 *   · 打开失败后**每写一行**都重新 fopen + system("mkdir -p")（g_logf 为空不会短路）。
 */
#ifndef _WIN32
#define CNK_FALLBACK_LOG_DIR "/tmp/cnkrouter"
#else
#define CNK_FALLBACK_LOG_DIR ""   /* Windows 端只有 CLI：stderr 就是全部 */
#endif

static time_t g_log_warned_at = 0;   /* 「日志不可用」告警的限频时间戳（秒） */
static int    g_log_fallback = 0;    /* 当前写的是回退目录（tmpfs），不是持久盘 */

static void log_stamp(char *buf, size_t cap) {
    time_t t = time(NULL);
    struct tm tmv;
#ifdef _WIN32
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    strftime(buf, cap, "%Y-%m-%d %H:%M:%S", &tmv);
}

/* 打开 dir/YYYY-MM-DD.log；目录不存在时建一次。失败返回 NULL（不打印）。 */
static FILE *log_open_in(const char *dir, const char *date) {
    char path[CNK_PATH_MAX + 32];
    FILE *f;

    snprintf(path, sizeof path, "%s/%s.log", dir, date);
    f = fopen(path, "a");
    if (!f && path_is_safe(dir)) {
        char cmd[CNK_PATH_MAX + 32];
        snprintf(cmd, sizeof cmd, "mkdir -p '%s' 2>/dev/null", dir);
        if (system(cmd) == 0) f = fopen(path, "a");
    }
#ifndef _WIN32
    /* 日志里有 WAN IP / AC 字段 / 门户地址，不能是默认的 644。
     * 只改自己刚打开的这一份，不动进程 umask（避免影响其他文件操作）。 */
    if (f) chmod(path, 0600);
#endif
    return f;
}

/* 跨天就切文件。每次写日志都调它，但命中"今天已处理"后只是 strftime + strcmp。 */
static void log_open_today(void) {
    char date[16], ts[32];
    time_t t = time(NULL);
    struct tm tmv;

    if (!g_log_dir[0]) return;
#ifdef _WIN32
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    strftime(date, sizeof date, "%Y-%m-%d", &tmv);

    /* 今天已经处理过（成功或失败都算）：失败后不能每行重试。
     * g_log_date 在下面先行写入，正是为了给失败路径也留下"今天试过了"。 */
    if (g_log_date[0] && strcmp(date, g_log_date) == 0) return;

    if (g_logf) { fclose(g_logf); g_logf = NULL; }
    snprintf(g_log_date, sizeof g_log_date, "%s", date);
    g_log_fallback = 0;

    g_logf = log_open_in(g_log_dir, date);
    if (g_logf) return;

    /* 持久盘不可用 → 回退 tmpfs。与配置同一个路径时不重复试。 */
    if (CNK_FALLBACK_LOG_DIR[0] && strcmp(g_log_dir, CNK_FALLBACK_LOG_DIR) != 0) {
        g_logf = log_open_in(CNK_FALLBACK_LOG_DIR, date);
        if (g_logf) g_log_fallback = 1;
    }

    /* 拔盘/挂载失败时会反复走到这里：一小时最多告警一次 */
    if (t - g_log_warned_at < 3600) return;
    g_log_warned_at = t;
    log_stamp(ts, sizeof ts);
    if (g_logf)
        fprintf(stderr, "%s [WARN] 日志目录 %s 不可用，已回退 %s（tmpfs，重启即失）\n",
                ts, g_log_dir, CNK_FALLBACK_LOG_DIR);
    else
        fprintf(stderr, "%s [WARN] 日志目录 %s 与回退目录都不可用，只写系统日志\n",
                ts, g_log_dir);
}

void log_init(int level, const char *dir) {
    g_level = level;
    if (g_logf) { fclose(g_logf); g_logf = NULL; }
    g_log_date[0] = '\0';
    g_log_fallback = 0;
    g_log_warned_at = 0;          /* 换配置后允许立刻告警一次 */
    if (dir && dir[0]) snprintf(g_log_dir, sizeof g_log_dir, "%s", dir);
    else g_log_dir[0] = '\0';
    log_open_today();
}

void log_write(int level, const char *fmt, ...) {
    char ts[32];
    char line[768];
    int n;
    va_list ap;

    if (level > g_level) return;
    log_open_today();               /* 跨天自动换文件 */

    log_stamp(ts, sizeof ts);

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

    /* ① 文件：详细历史。回退目录在 tmpfs（内存）上，只写 INFO 及以上 ——
     *    持久盘真挂了的时候，别让 DEBUG 把 512MB 的内存吃掉。 */
    if (g_logf && (!g_log_fallback || level <= LOG_INFO)) {
        fputs(line, g_logf);
        fputc('\n', g_logf);
        fflush(g_logf);
    }

    /* ② 系统日志：只留关键状态与告警（DEBUG 不进 logd 的 128KB 环形缓冲）。
     *    没有文件可写时（CLI / 两个目录都不可用）stderr 是唯一出口，按 g_level 全量输出。 */
    if (!g_logf || level <= LOG_INFO) {
        fputs(line, stderr);
        fputc('\n', stderr);
        fflush(stderr);
    }
}

/* ---------------- 配置 ---------------- */

void config_defaults(Config *c) {
    memset(c, 0, sizeof *c);
    /* 探测地址：generate_204 直连返回 204（无重定向、无劫持），比 captive.apple.com
     * 更稳——后者在本网被 DNS 劫持到 CDN（多一跳，抖动就报"域名解析失败"）。
     * 2026-11-09 实测 hicloud 38ms / miui 84ms，均直连 204。probe_expect 留空：
     * 204 分支不看 expect；若换回 200 型探测页再填。 */
    snprintf(c->auth_url, sizeof c->auth_url, "http://10.0.0.1/");
    snprintf(c->isp, sizeof c->isp, "campus");
    snprintf(c->probe_url, sizeof c->probe_url, "http://connectivitycheck.platform.hicloud.com/generate_204");
    snprintf(c->probe_expect, sizeof c->probe_expect, "");
    c->probe_timeout = 6;
    c->recheck_timeout = 2;   /* 登录后复测：失败探测最多等 2 秒，不再拖满 6 秒 */
    c->probe_interval = 30;
    /* 默认开 DEBUG：2026-11-01 排查时才发现 log_level=2 把 AC 复核、出网验证这些
     * 关键判据全滤掉了，日志里只剩一句"域名解析失败"，根本定位不到故障层次。
     * 一天也就一两 MB，而 /mnt/data 有 54 GB —— 证据比省空间重要。 */
    c->log_level = LOG_DBG;
    /* 日志落到数据盘、按天分文件：/mnt/data 有 54 GB，一天一百多 KB 根本不用省；
     * 排查故障时"哪一天"比"文件多大"重要。旧文件由 cnk-cleanup 清理。 */
    snprintf(c->log_dir, sizeof c->log_dir, "/mnt/data/log/cnkrouter");
    c->log_keep_days = 7;
    c->verify_ac = 1;   /* 探测成功后用 chkstatus 复核，防 DNS 劫持导致的"假在线" */
    /* 默认只记录：处置动作（尤其换 IP/换 MAC）会主动断网，误报的代价比漏报高得多 */
    c->inuse_action = 0;

    /* 链路层自愈：默认开。软拔插会断 5~10 秒，但它是实测唯一有效的手段，
     * 且只在「门户 + 网关都不可达」这个明确条件下才动手。 */
    snprintf(c->wan_iface, sizeof c->wan_iface, "wan");
    c->link_recovery = 1;
    c->link_after = 2;
    c->link_cooldown = 900;
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

/* 探测间隔下限：小于 10 秒一律回到默认值 30 秒。
 * 真机 10 秒间隔时一天刷出 88 条「域名解析失败」，而黑洞发现并不更快 ——
 * 链路层判定本来就要连续 2 次离线才动手。三处默认值（代码 / UCI 模板 /
 * init.d）必须一致，否则渲染出来的配置和文档说的不是一回事。
 * 抽成纯函数是为了让 --selftest 独立断言，不依赖配置文件。 */
static int clamp_interval(int v) { return v < 10 ? 30 : v; }

/* 网卡名会拼进 system() 执行的命令行里。配置文件是 600，但仍然只放行
 * 字母数字和 . _ - ：既防注入，也顺便挡掉拼错的怪名字。 */
static int iface_name_ok(const char *s) {
    size_t i;
    if (!s || !s[0] || strlen(s) >= 16) return 0;
    for (i = 0; s[i]; ++i) {
        unsigned char ch = (unsigned char)s[i];
        if (!(isalnum(ch) || ch == '_' || ch == '.' || ch == '-')) return 0;
    }
    return 1;
}

/* 解析进 *c。**不做事务保护**：失败时 *c 可能已被写了一半，
 * 只允许 config_load 内部的临时对象调用（见下方包装）。 */
static int config_parse(const char *path, Config *c, char *err, size_t cap) {
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
        else if (strcmp(key, "recheck_timeout") == 0) c->recheck_timeout = atoi(val);
        else if (strcmp(key, "probe_interval") == 0) c->probe_interval = atoi(val);
        else if (strcmp(key, "bind_ip") == 0)    copy_field(c->bind_ip, sizeof c->bind_ip, val);
        else if (strcmp(key, "log_level") == 0)  c->log_level = atoi(val);
        else if (strcmp(key, "log_dir") == 0)        copy_field(c->log_dir, sizeof c->log_dir, val);
        else if (strcmp(key, "log_keep_days") == 0)  c->log_keep_days = atoi(val);
        else if (strcmp(key, "verify_ac") == 0)  c->verify_ac = atoi(val) ? 1 : 0;
        else if (strcmp(key, "inuse_action") == 0) c->inuse_action = atoi(val);
        else if (strcmp(key, "wan_iface") == 0) {
            copy_field(c->wan_iface, sizeof c->wan_iface, val);
            /* 网卡名会拼进 system() 执行的命令里，只放行安全字符 */
            if (!iface_name_ok(c->wan_iface)) {
                log_write(LOG_WARN, "配置里的 wan_iface 不合法，回退为 wan");
                snprintf(c->wan_iface, sizeof c->wan_iface, "wan");
            }
        }
        else if (strcmp(key, "link_recovery") == 0) c->link_recovery = atoi(val) ? 1 : 0;
        else if (strcmp(key, "link_after") == 0)    c->link_after = atoi(val);
        else if (strcmp(key, "link_cooldown") == 0) c->link_cooldown = atoi(val);
        else log_write(LOG_WARN, "配置第 %d 行：未知键 '%s'（已忽略）", lineno, key);
    }
    fclose(f);

    if (c->probe_timeout <= 0 || c->probe_timeout > 60) c->probe_timeout = 6;
    if (c->recheck_timeout <= 0 || c->recheck_timeout > 60) c->recheck_timeout = 2;
    c->probe_interval = clamp_interval(c->probe_interval);
    if (c->link_after < 1) c->link_after = 2;
    if (c->link_cooldown < 60) c->link_cooldown = 900;
    return 0;
}

/* 事务化装载：先在副本上解析，全部成功才整体替换。
 *
 * 旧实现直接解析进调用方的 Config：坏行出现在文件中段时，前面几行**已经生效**了，
 * 日志却写着「继续用旧配置」—— 实际配置是半新半旧，而且调用方还把退避状态重置了
 * （2026-11-24 R2）。现在失败路径保证 *c 一个字节都不变。 */
int config_load(const char *path, Config *c, char *err, size_t cap) {
    Config tmp = *c;
    if (config_parse(path, &tmp, err, cap) != 0) return -1;
    *c = tmp;
    return 0;
}

const char *isp_suffix(const Config *c) {
    if (strncmp(c->isp, "custom:", 7) == 0) return c->isp + 7;
    if (strcmp(c->isp, "cmcc") == 0)    return "@cmcc";
    if (strcmp(c->isp, "telecom") == 0) return "@dx";
    if (strcmp(c->isp, "unicom") == 0)  return "@lt";
    return "";
}

/* 启动日志用的脱敏账号：学号抹成 *，只留运营商后缀。
 * 排查"配置对不对"主要看后缀（@cmcc/@dx/@lt），账号本身用户自己知道，不必明文落盘。 */
static const char *masked_account(const Config *c) {
    static char buf[CNK_USER_MAX + 16];
    snprintf(buf, sizeof buf, "%s%s", c->username, isp_suffix(c));
    redact_secret(buf, c->username);
    return buf;
}

/* ---------------- 复测 ---------------- */

static int wait_online(const Config *c, int tries, int interval_ms) {
    int i;
    /* 复测用更短的探测超时：常态 6 秒是为容忍链路抖动，但登录后复测时
     * 一次失败探测就要等满 6 秒，把会话回收的恢复拖到 10 秒以上
     * （2026-11-12 13:07 真机：13:07:32 登录成功 → 13:07:44 才复测通过）。
     * 这里只改 Config 副本，常态 probe_once 仍用原配置，判定口径不变。 */
    Config fast = *c;
    if (fast.recheck_timeout > 0 && fast.recheck_timeout < fast.probe_timeout)
        fast.probe_timeout = fast.recheck_timeout;
    for (i = 0; i < tries; ++i) {
        ProbeResult pr;
        if (i) sleep_ms(interval_ms);
        if (g_stop) return 0;
        pr = probe_once(&fast);
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

/* ---------------- 僵尸会话处置 ---------------- */

/* AC 的会话是按 **IP** 记的（chkstatus 回包里只有 v4ip/ss5，没有 MAC 字段）。
 * 当它记着"这个 IP 已认证"、转发面却已失效时，重登只会收到「终端IP已在线」——
 * 客户端判成功、实际照样不通，**重试再多次也没用**。用户实测的唯一解法是
 * 拔插网线（= 换 IP，让 AC 眼里变成另一台设备）。
 *
 * 这里按代价从低到高复刻人工做的三件事，每级都限流，避免反复断网：
 *   ① 注销会话            不中断连接
 *   ② 换 IP              已废弃：统一由 link_recovery 的软拔插负责
 *   ③ 改 MAC 换身份       最重，30 分钟最多一次
 *
 * ⚠ 默认 inuse_action=0 时只记录、几乎立即返回（2026-11-10 修）：
 *   原来是 sleep 60 秒，判出僵尸会话后整整一分钟不探测；真机 09:52:44 判僵尸
 *   → 09:53:49 才复探 → 09:55:11 才恢复，端到端 3m28s。故障会自己从「认证侧」
 *   恶化到「二层」，守护进程必须持续采样，不能在最需要观察的时候失明。
 */
#ifndef _WIN32
/* 把 wan MAC 的最后一个字节 +1：保持 OUI 前缀不变，只让服务器认为是"新设备" */
static int bump_wan_mac(void) {
    char mac[32] = "";
    char line[64];
    unsigned last;
    size_t n;
    FILE *f = fopen("/sys/class/net/wan/address", "r");

    if (!f) return -1;
    if (fgets(line, sizeof line, f)) snprintf(mac, sizeof mac, "%s", line);
    fclose(f);
    n = strlen(mac);
    while (n && (mac[n - 1] == '\n' || mac[n - 1] == '\r')) mac[--n] = '\0';
    if (n != 17) return -1;

    last = (unsigned)strtoul(mac + 15, NULL, 16);
    last = (last + 1) & 0xFF;
    mac[15] = "0123456789ABCDEF"[(last >> 4) & 0xF];
    mac[16] = "0123456789ABCDEF"[last & 0xF];

    {
        char cmd[320];
        snprintf(cmd, sizeof cmd,
                 "uci set network.wan.macaddr='%s'; uci commit network; "
                 "ubus call network.interface.wan down >/dev/null 2>&1; "
                 "sleep 2; ubus call network.interface.wan up >/dev/null 2>&1", mac);
        log_write(LOG_WARN, "僵尸会话处置③：改 MAC 为 %s 并重连 WAN", mac);
        if (system(cmd) != 0) log_write(LOG_WARN, "改 MAC 的命令执行失败");
    }
    return 0;
}
#endif

static void handle_inuse(Config *c) {
    static time_t t_logout = 0;
#ifndef _WIN32
    static time_t t_mac = 0;
#endif
    time_t now = time(NULL);

    /* 默认只记录。处置里含"换 IP / 换 MAC"这类主动断网动作，
     * 判定一旦误报就会亲手制造故障 —— 在拿到真实故障的日志证据之前，
     * 程序只负责把现场记清楚，不替用户做决定。需要它动手时把
     * /etc/config/cnkrouter 的 inuse_action 调到 1/3。 */
    if (c->inuse_action <= 0) {
        log_write(LOG_WARN,
                  "疑似僵尸会话：服务器说已在线但实际不通。"
                  "inuse_action=0，只记录并立即进入下一轮故障分层");
        /* 2026-11-10 修：原来是 sleep_slices(60000)。留 2 秒把日志刷完即可，
         * 之后由 run_daemon 继续探测；链路层若恶化，下一轮就能触发软拔插。 */
        sleep_slices(2000);
        return;
    }

    /* ① 注销：不中断连接，先试它 */
    if (c->inuse_action >= 1 && now - t_logout >= 300) {
        t_logout = now;
        log_write(LOG_INFO, "僵尸会话处置①：发注销，3 秒后重探");
        if (portal_logout(c) == 0) {
            sleep_slices(3000);
            return;
        }
        log_write(LOG_WARN, "注销发不出去，直接升级到下一级");
    }

#ifndef _WIN32
    /* ② 换 IP：不再自己跑 ubus down/up（2026-11-10 收敛）。
     * 它和 handle_link_down 的软拔插是两套并存的换 IP 实现：命令不同
     * （ubus vs ip link）、冷却不同（1800s vs link_cooldown）、日志不同、
     * replug_flag 也不同，而真机上从未启用过 inuse_action>=2。
     * 现在统一交给链路层（默认 link_recovery=1）：它有故障分层判定、冷却、
     * 结构化「链路自愈」日志，还会抑制自家 ifup 触发的服务重启。 */
    if (c->inuse_action >= 2)
        log_write(LOG_INFO, "僵尸会话处置②：换 IP 由链路层自愈统一处理，此处不重复动作");

    /* ③ 换 MAC */
    if (c->inuse_action >= 3 && now - t_mac >= 1800) {
        t_mac = now;
        if (bump_wan_mac() == 0) {
            sleep_slices(15000);
            return;
        }
        log_write(LOG_WARN, "读不到 wan 的 MAC，跳过换 MAC");
    }
#endif

    /* 手段都在冷却中：同样不能长睡，否则链路层自愈会被一起拖住。 */
    log_write(LOG_WARN, "僵尸会话处置手段均在冷却中，10 秒后重新判层");
    sleep_slices(10000);
}

/* ---------------- 链路层诊断与自愈 ----------------
 *
 * 起因是 2026-11-01 那次**有人在场、没拔网线**的完整故障现场（用户特意保留了现场）。
 * 当时日志只有两种句子循环出现：
 *     离线：探测失败：域名解析失败：captive.apple.com
 *     登录：loadConfig 请求失败：连接失败（拒绝/不可达/超时）
 * 循环 6 次，程序毫无办法；用户拔插网线后 1 秒内恢复。
 *
 * 现场实测（以下地址均为**脱敏占位符**，真实值不入库 —— 见 docs/evidence/audit_redaction.py）：
 *   · WAN 有 IP（10.0.0.100/24）、链路 LOWER_UP、零错包零丢包
 *   · DHCP 租约当天早些时候刚续过，还剩 3 天      → 不是 DHCP 到期
 *   · ping 网关 10.0.0.1 = 100% 丢包
 *   · ip neigh：网关 = INCOMPLETE（ARP 解析不出）
 *   · RX 速率从历史 272 包/秒掉到 0.1 包/秒  → 上游端口基本不再转发
 *   · 门户 10.0.0.9（同时也是 DNS 服务器）不可达
 *
 * 结论：故障在**二层**。登录、探测、重试全在三层以上，对一个被隔离的端口毫无作用，
 * 所以「死循环重试」必然无效。而拔插网线会让交换机看到 link down/up 并重新放行。
 *
 * 软件版拔插网线 = ip link set dev wan down/up。本平台内核会真的把 PHY 拉低
 * （dmesg 打出 "PHY Link is down" / "PHY Link up speed: 1000"），实测一次即恢复，
 * 而且 DHCP 拿到了**新地址**（10.0.0.100 → 10.0.0.5）—— 续租拿的是同一个旧地址，
 * 救不回来，这正是必须重新 DHCP 的原因。
 */

#ifndef _WIN32

/* 从 /proc/net/route 取默认网关（Destination 为 00000000 的那行）。
 * 注意 Gateway 列是**字节反序**的十六进制：192.168.1.1 显示成 0101A8C0。 */
static int default_gateway(char *gw, size_t gwcap, char *iface, size_t icap) {
    FILE *f;
    char line[256];

    f = fopen("/proc/net/route", "r");
    if (!f) return -1;
    while (fgets(line, sizeof line, f)) {
        char ifc[32], dest[32], gwh[32], flg[32];
        unsigned b[4];
        if (sscanf(line, "%31s %31s %31s %31s", ifc, dest, gwh, flg) != 4) continue;
        if (strcmp(dest, "00000000") != 0) continue;      /* 只要默认路由 */
        if (strlen(gwh) != 8) continue;
        if (sscanf(gwh, "%2x%2x%2x%2x", &b[0], &b[1], &b[2], &b[3]) != 4) continue;
        snprintf(gw, gwcap, "%u.%u.%u.%u", b[3], b[2], b[1], b[0]);
        if (iface && icap) snprintf(iface, icap, "%s", ifc);
        fclose(f);
        return 0;
    }
    fclose(f);
    return -1;
}

/* 查某个 IP 的 ARP 状态：1 = 已解析出 MAC，0 = 未解析，-1 = 表里没有 */
static int arp_state(const char *ip, const char *iface, char *mac, size_t cap) {
    FILE *f;
    char line[256];

    f = fopen("/proc/net/arp", "r");
    if (!f) return -1;
    while (fgets(line, sizeof line, f)) {
        char aip[64], hwtype[32], flg[32], amac[32], mask[32], dev[32];
        unsigned flags;
        if (sscanf(line, "%63s %31s %31s %31s %31s %31s",
                   aip, hwtype, flg, amac, mask, dev) != 6) continue;
        if (strcmp(aip, ip) != 0) continue;
        if (iface && iface[0] && strcmp(dev, iface) != 0) continue;
        flags = (unsigned)strtoul(flg, NULL, 16);
        if (cap) snprintf(mac, cap, "%s", amac);
        fclose(f);
        /* ATF_COM = 0x2 表示条目完整；没解析出来时 flags 为 0 且 MAC 是全零 */
        return (flags & 0x2) && strcmp(amac, "00:00:00:00:00:00") != 0 ? 1 : 0;
    }
    fclose(f);
    return -1;
}

/* 网关的 IP + ARP 状态。1 = 通，0 = 不通（二层断了），-1 = 读不到（无从判断） */
static int link_gw_status(char *gw, size_t gwcap, char *mac, size_t maccap) {
    char dev[32] = "";
    if (mac && maccap) mac[0] = '\0';
    if (default_gateway(gw, gwcap, dev, sizeof dev) != 0) return -1;
    return arp_state(gw, dev[0] ? dev : NULL, mac, maccap);
}

/* 本机实际用的上游 DNS。dnsmasq 自己监听 127.0.0.1，光看 /etc/resolv.conf
 * 只能看到 127.0.0.1，真正的服务器在 DHCP 下发的那份里 —— 排查 DNS 故障时
 * 这才是要写进日志的那个值。 */
static int dns_server(char *out, size_t cap) {
    static const char *files[] = {
        "/tmp/resolv.conf.d/resolv.conf.auto", "/etc/resolv.conf", NULL
    };
    int i;

    for (i = 0; files[i]; ++i) {
        FILE *f = fopen(files[i], "r");
        char line[256];
        if (!f) continue;
        while (fgets(line, sizeof line, f)) {
            char ns[64];
            if (sscanf(line, "nameserver %63s", ns) != 1) continue;
            if (strncmp(ns, "127.", 4) == 0 || strcmp(ns, "::1") == 0) continue;
            snprintf(out, cap, "%s", ns);
            fclose(f);
            return 0;
        }
        fclose(f);
    }
    return -1;
}

static int wan_ip_of(const char *iface, char *out, size_t cap) {
    char cmd[160], line[256];
    FILE *p;

    if (!iface_name_ok(iface)) return -1;
    snprintf(cmd, sizeof cmd, "ip -4 -o addr show dev %s 2>/dev/null", iface);
    p = popen(cmd, "r");
    if (!p) return -1;
    if (fgets(line, sizeof line, p)) {
        char *s = strstr(line, "inet ");
        if (s) {
            char *slash = strchr(s + 5, '/');
            size_t n = slash ? (size_t)(slash - (s + 5)) : strlen(s + 5);
            if (n && n < cap) {
                memcpy(out, s + 5, n);
                out[n] = '\0';
                pclose(p);
                return 0;
            }
        }
    }
    pclose(p);
    return -1;
}

/* 门户还有反应吗？门户在**未认证**时也会回首页或 302 —— 它都不可达，
 * 就说明故障在认证之下，登录再多次也没用。 */
static int link_portal_reachable(const Config *c) {
    HttpResp resp;
    char err[128];
    int rc;

    memset(&resp, 0, sizeof resp);
    rc = http_request("GET", c->auth_url, NULL, NULL, c->bind_ip, 3000,
                      &resp, err, sizeof err);
    if (rc != 0) {
        log_write(LOG_DBG, "门户可达性：请求失败（%s）", err);
        return 0;
    }
    log_write(LOG_DBG, "门户可达性：HTTP %d", resp.status);
    http_resp_free(&resp);
    return 1;
}

/* 软拔插标记：告诉 hotplug 钩子"这次 ifup 是我们自己拉起来的"。
 *
 * 为什么需要：原来自家软拔插一 up，netifd 就发 ifup → hotplug 重启服务 →
 * 正在 link_wait_ready() 里等的进程被 SIGTERM 打断，日志却写成
 * 「40 秒内 WAN 仍未就绪」（实际只等了 10 秒，2026-11-02 00:01 实测）。
 * 更糟的是这次重启会把"重试"的机会一起掐掉：WAN 没起来时重启进程毫无意义，
 * 再拉一次 PHY 才有用。所以软拔插期间抑制重启，让恢复逻辑在本进程里跑完。 */
#define REPLUG_FLAG_PATH "/tmp/cnkrouter.replug"

static void replug_flag_set(void) {
    FILE *f = fopen(REPLUG_FLAG_PATH, "w");
    if (f) fclose(f);
}

static void replug_flag_clear(void) { unlink(REPLUG_FLAG_PATH); }

/* 软件版拔插网线：把 PHY 拉低再拉高。
 * 本平台（nss-dp）对 ip link down 会真的断 PHY，dmesg 里有 "PHY Link is down"，
 * 上游交换机看到链路变化后重新协商放行。 */
static int link_wan_cycle(const char *iface) {
    char cmd[160];

    snprintf(cmd, sizeof cmd, "ip link set dev %s down 2>/dev/null", iface);
    if (system(cmd) != 0) {
        log_write(LOG_WARN, "软拔插失败：无法 down %s", iface);
        return -1;
    }
    sleep_slices(3000);                 /* 让交换机明确看到链路断开 */
    snprintf(cmd, sizeof cmd, "ip link set dev %s up 2>/dev/null", iface);
    if (system(cmd) != 0) {
        log_write(LOG_WARN, "软拔插失败：无法 up %s（接口可能停在 down 状态）", iface);
        return -1;
    }
    return 0;
}

/* 等 WAN 重新拿到地址、网关 ARP 解析成功。最多等 wait_secs 秒 */
static int link_wait_ready(int wait_secs) {
    int i;
    for (i = 0; i < wait_secs * 2; ++i) {
        char gw[64], mac[32];
        if (g_stop) return 0;
        if (link_gw_status(gw, sizeof gw, mac, sizeof mac) == 1) return 1;
        sleep_ms(500);
    }
    return 0;
}

/* 连续离线到门槛后调用：先分清故障层次，确认是二层问题才软拔插。
 * 返回 1 表示已执行拔插（调用方应跳过本轮登录，直接重新探测）。 */
static int handle_link_down(const Config *c) {
    /* 冷却用单调时钟（now_ms 走 CLOCK_MONOTONIC）：NTP 校时把墙上时间回拨时，
     * time(NULL) 会让"距上次软拔插不足 N 秒"的判断失效、进而反复断网。 */
    static long long last_ms = 0;
    long long now = now_ms() / 1000;
    char gw[64], mac[32], dns[64], iface[32];
    int gws;

    gws = link_gw_status(gw, sizeof gw, mac, sizeof mac);
    if (gws < 0) {
        /* 读不到默认网关 = WAN 没拿到 IP / 没路由。此时软拔插无的放矢：
         * 接口很可能还没起来（开机 / DHCP 未完成），down/up 反而可能打断 DHCP 协商；
         * 且软拔插的触发前提「门户 + 网关都不可达」在此无从判断，故只记录、交上层继续探测。 */
        log_write(LOG_WARN, "链路诊断：读不到默认网关（WAN 还没起来），不做软拔插");
        return 0;
    }
    if (dns_server(dns, sizeof dns) == 0)
        log_write(LOG_WARN, "链路诊断：网关 %s（ARP %s），DNS 服务器 %s",
                  gw, gws ? (mac[0] ? mac : "已解析") : "未解析", dns);
    else
        log_write(LOG_WARN, "链路诊断：网关 %s（ARP %s），取不到 DNS 服务器",
                  gw, gws ? (mac[0] ? mac : "已解析") : "未解析");

    if (gws == 0)
        log_write(LOG_WARN, "链路诊断：网关不代答 ARP —— 故障在二层，登录重试必然无效");
    else
        log_write(LOG_WARN, "链路诊断：网关可达，可能是认证侧或门户侧的问题");

    if (link_portal_reachable(c)) {
        log_write(LOG_WARN, "门户 %s 可达 → 按认证问题处理，不做软拔插", c->auth_url);
        return 0;
    }
    log_write(LOG_WARN, "门户 %s 不可达 → 故障在认证之下（登录再多次也没用）", c->auth_url);

    if (c->link_recovery <= 0) {
        log_write(LOG_WARN, "link_recovery=0，只记录不动作（要自动软拔插请设为 1）");
        return 0;
    }
    if (now - last_ms < (long long)c->link_cooldown) {
        log_write(LOG_WARN, "距上次软拔插不足 %d 秒，冷却中（防反复断网）", c->link_cooldown);
        return 0;
    }

    /* 只动**真正承载默认路由**的那个口：配置里名字写错时不至于去 down 别人的接口，
     * 也不会在开发环境（没有 wan 口）上误伤宿主网络。 */
    {
        char dev[32] = "";
        if (default_gateway(gw, sizeof gw, dev, sizeof dev) != 0 || !dev[0]) {
            log_write(LOG_WARN, "推断不出默认路由所在的网卡，跳过软拔插");
            return 0;
        }
        if (c->wan_iface[0] && strcmp(c->wan_iface, dev) != 0)
            log_write(LOG_WARN, "配置的 wan_iface=%s 与默认路由所在口 %s 不一致，以 %s 为准",
                      c->wan_iface, dev, dev);
        if (!iface_name_ok(dev)) {
            log_write(LOG_WARN, "网卡名 %s 不合法，跳过软拔插", dev);
            return 0;
        }
        snprintf(iface, sizeof iface, "%s", dev);
    }

    {
        char old_ip[64] = "?", new_ip[64] = "?";
        long long t0;
        int attempt = 0, done = 0, interrupted = 0;
        wan_ip_of(iface, old_ip, sizeof old_ip);
        last_ms = now;
        t0 = now_ms();

        /* 第一次不成就再拉一次 PHY：二层故障常常一次不放过（DHCP 还没谈完、
         * 上游端口还在重新协商），盲等 40 秒不如再断一次链路。最多两次，
         * 再多就成"替用户反复断网"了。 */
        for (attempt = 1; attempt <= 2; ++attempt) {
            log_write(LOG_WARN, "执行软拔插（第 %d/2 次）：ip link set dev %s down/up"
                                "（等价于拔插网线，会断 5~10 秒）", attempt, iface);
            replug_flag_set();
            if (link_wan_cycle(iface) != 0) break;
            if (link_wait_ready(40)) { done = 1; break; }
            if (g_stop) {
                /* 被 procd 重启打断 ≠ WAN 未就绪。前者是外部动作（init 重启/关机），
                 * 后者才需要再拉一次；混在一起会让日志骗人。 */
                interrupted = 1;
                log_write(LOG_WARN, "软拔插等待被服务重启打断（非 WAN 未就绪）");
                break;
            }
            wan_ip_of(iface, new_ip, sizeof new_ip);
            log_write(LOG_WARN, "第 %d 次软拔插后 40 秒 WAN 仍未就绪（当前地址 %s）",
                      attempt, new_ip);
        }
        wan_ip_of(iface, new_ip, sizeof new_ip);
        /* 一条结构化记录，`grep 链路自愈` 就是完整的"链路健康"时间线：
         * 什么时候、断在哪一层、动了几次、换没换地址、花了多久。 */
        if (done)
            log_write(LOG_INFO,
                      "链路自愈：动作=软拔插 原因=网关ARP未解析 尝试=%d/2 结果=成功 "
                      "地址=%s→%s 耗时=%llds",
                      attempt, old_ip, new_ip, (now_ms() - t0) / 1000);
        else if (interrupted)
            log_write(LOG_WARN,
                      "链路自愈：动作=软拔插 原因=网关ARP未解析 尝试=%d/2 结果=中断(服务重启) "
                      "地址=%s 耗时=%llds",
                      attempt, new_ip, (now_ms() - t0) / 1000);
        else
            log_write(LOG_WARN,
                      "链路自愈：动作=软拔插 原因=网关ARP未解析 尝试=%d/2 结果=失败(WAN未就绪) "
                      "地址=%s 耗时=%llds",
                      attempt > 2 ? 2 : attempt, new_ip, (now_ms() - t0) / 1000);
        replug_flag_clear();
    }
    return 1;
}

#else  /* _WIN32：这些能力只在路由器上有，Windows 上全部降级成"查不出来 / 不动作" */

static int link_gw_status(char *gw, size_t gwcap, char *mac, size_t maccap) {
    (void)gw; (void)gwcap; (void)mac; (void)maccap;
    return -1;
}
static int handle_link_down(const Config *c) {
    (void)c;
    return 0;
}

#endif /* _WIN32 */

/* 应用一次 SIGHUP 重载。返回 1 = 已切换，0 = 保持原样。
 *
 * 三件事必须同时成立，否则就是「日志说重载成功了，实际没生效」：
 *   ① 配置整体替换（config_load 已事务化，失败不碰 *c）；
 *   ② 日志设置真的应用 —— 只改 Config 里的 log_level/log_dir 没用，
 *      写日志读的是 g_level / g_log_dir（R2 前改了不生效，还继续写旧目录）；
 *   ③ 退避与停止态归零，且**只在成功时** —— 坏配置若把「凭据错误已停止」
 *      顺手解除，等于拿坏配置反复重试（R2 前每次 reload 都无条件重置）。 */
static int apply_reload(Config *c, int *bidx, int *total, int *stopped) {
    char err[128];

    if (config_load(g_config_path, c, err, sizeof err) != 0) {
        log_write(LOG_ERR, "配置重载失败：%s（继续用旧配置，退避与停止状态不动）", err);
        return 0;
    }
    log_init(c->log_level, c->log_dir);
    log_write(LOG_INFO, "配置已重新加载（log_level=%d log_dir=%s）",
              c->log_level, c->log_dir[0] ? c->log_dir : "(不落盘)");
    *bidx = 0;
    *total = 0;
    *stopped = 0;
    return 1;
}

static int run_daemon(Config *c) {
    static const int backoff[] = {15, 30, 60, 120, 300};
    int bidx = 0, total = 0, ever_ok = 0, stopped = 0, offline_streak = 0;
    int dns_streak = 0;   /* 连续「DNS 抖动但旁证通」的轮数，超过阈值按真实离线处理 */
    int was_online = 0;   /* 上一轮是否在线：用于只在状态切换时打日志 */

    log_write(LOG_INFO, "cnkrouter %s 启动：账号=%s 认证地址=%s 探测间隔=%ds",
              CNK_VERSION, masked_account(c), c->auth_url, c->probe_interval);

    while (!g_stop) {
        ProbeResult pr;

        if (g_reload) {
            g_reload = 0;
            /* 成功才重置退避；失败时 apply_reload 内部保持原样 */
            apply_reload(c, &bidx, &total, &stopped);
        }

        pr = probe_once(c);
        if (g_stop) break;

        if (pr.state == NET_ONLINE) {
            if (stopped) log_write(LOG_INFO, "网络已恢复，解除停止状态");
            /* 只在状态切换时打一条：首次在线 / 从离线恢复在线各一条。
             * 持续在线（was_online==1）静默 —— 探测间隔降到 10s 后，
             * 若每轮都打「在线」会是 26 KB/天的纯重复。 */
            if (!ever_ok)
                log_write(LOG_INFO, "网络在线：%s", pr.message);
            else if (!was_online)
                log_write(LOG_INFO, "已恢复在线：%s", pr.message);
            was_online = 1;
            probe_set_quiet(1);   /* 下一轮若仍在线，AC 复核/出网验证的 DEBUG 也静默 */
            ever_ok = 1;
            stopped = 0;
            total = 0;
            bidx = 0;
            offline_streak = 0;
            dns_streak = 0;
            sleep_slices(c->probe_interval * 1000);
            continue;
        }

        /* NET_DNS 是观察态，不是离线：IP 直连旁证还在，链路大概率没断。
         * 快重探 1.5 秒、最多 3 轮（合计约 4.5 秒，与旧版离线判定 3 秒同量级）；
         * 连续 3 轮仍解析不了就说明不是瞬时抖动，降级按真实离线分层处理。
         * 观察期间不动 was_online / quiet，避免恢复时刷一条「已恢复在线」。 */
        if (pr.state == NET_DNS) {
            if (stopped) { sleep_slices(60000); continue; }
            ++dns_streak;
            log_write(LOG_WARN, "DNS 解析失败，IP 直连旁证仍通（观察 %d/3）：%s",
                      dns_streak, pr.message);
            if (dns_streak < 3) {
                sleep_slices(1500);
                continue;
            }
            log_write(LOG_WARN, "连续 3 次 DNS 仍无法解析，不再当抖动，按真实离线处理");
            dns_streak = 0;
            offline_streak = 2;   /* 直接进分层，不再叠加 1.5s×2 的等待 */
            snprintf(pr.message, sizeof pr.message, "连续 3 次 DNS 解析失败（IP 直连旁证已失效）");
            /* 落到下面的离线分层逻辑 */
        }

        /* 不在线：关闭静默，恢复详细日志，便于观察后续处置 */
        was_online = 0;
        probe_set_quiet(0);

        if (stopped) {                      /* 终止态：低频巡检 */
            sleep_slices(60000);
            continue;
        }
        if (pr.state == NET_UNKNOWN) {
            log_write(LOG_WARN, "配置无效：%s", pr.message);
            sleep_slices(10000);
            continue;
        }
        if (pr.state == NET_OFFLINE || pr.state == NET_DNS) {
            char gw[64], mac[32];
            int gws = link_gw_status(gw, sizeof gw, mac, sizeof mac);
            int thr = c->link_after > 0 ? c->link_after : 2;

            ++offline_streak;
            /* 每次离线都把网关状态带上。以前只写"域名解析失败"，
             * 三种完全不同的故障（没认证 / 僵尸会话 / 二层断了）塌缩成同一句话，
             * 排查时根本分不出来 —— 这是 2026-11-01 定位失败的直接原因。 */
            if (gws == 0)
                log_write(LOG_INFO, "离线：%s（连续 %d 次）｜网关 %s ARP 未解析",
                          pr.message, offline_streak, gw);
            else if (gws == 1)
                log_write(LOG_INFO, "离线：%s（连续 %d 次）｜网关 %s 已解析到 %s",
                          pr.message, offline_streak, gw, mac[0] ? mac : "?");
            else
                log_write(LOG_INFO, "离线：%s（连续 %d 次）｜无默认网关",
                          pr.message, offline_streak);

            if (offline_streak < 2) {
                sleep_slices(1500);         /* 快速重探：先排除瞬时抖动 */
                continue;
            }
            if (offline_streak < thr) {
                sleep_slices(1500);         /* 还没到动手门槛，继续数 */
                continue;
            }
            offline_streak = 0;
            /* 关键一步：先分清是"没认证"还是"链路断了"，再决定要不要登录。
             * 后者在二层，登录再多次也无效（2026-11-01 实测循环 6 次全败），
             * 只有软拔插能恢复 —— 只重探不分层，就是用户遇到的那种假死。 */
            log_write(LOG_WARN, "连续 %d 次离线，先判故障层次（不再干等）", thr);
            /* 取证：动手前回放 AC 复核历史 + 现场再问一次，定位「会话被回收」根因 */
            ac_forensics_dump(c);
            if (handle_link_down(c)) continue;   /* 已软拔插，直接进下一轮探测 */
            log_write(LOG_WARN, "层次判定：属认证问题，主动尝试登录");
        }

        /* 只有 NET_PORTAL 才是真的被劫持。NET_OFFLINE 走到这里说明故障分层判定
         * 认为属认证侧问题，pr.message 是「探测失败：…」；以前两种情况都打
         * 「检测到门户劫持」，真机 15:16 出现「检测到门户劫持：探测失败：域名解析失败」
         * 这种自相矛盾的日志。按状态分开写。 */
        log_write(LOG_INFO, "%s：%s",
                  pr.state == NET_PORTAL ? "检测到门户劫持" : "层次判定后尝试登录",
                  pr.message);
        {
            LoginResult lr = portal_login(c);
            ++total;
            log_write(lr.ok ? LOG_INFO : LOG_WARN, "登录：%s", lr.message);

            if (lr.ok && lr.inuse) {
                /* 服务器回「终端IP已在线」：这时**不能**当成功就走人。
                 * 复测通了才是真在线；复测不通就是僵尸会话（AC 记着已认证、
                 * 转发面却失效），必须走注销/换 IP/换 MAC，而不是傻等重试。 */
                if (wait_online(c, 2, 1000)) {
                    log_write(LOG_INFO, "服务器报已在线，复测也通，正常");
                    ever_ok = 1;
                    /* 复测已确认在线：同步状态位，否则下一轮 probe 又按「从离线恢复」
                     * 再打一条「已恢复在线」，同一秒出现两条（2026-11-10 实测）。 */
                    was_online = 1;
                    probe_set_quiet(1);
                    total = 0;
                    bidx = 0;
                } else {
                    log_write(LOG_WARN, "服务器报「已在线」但复测不通 —— 按僵尸会话处置");
                    handle_inuse(c);
                }
                continue;
            }

            if (lr.ok) {
                if (wait_online(c, 4, 1000)) {
                    log_write(LOG_INFO, "已恢复在线");
                    ever_ok = 1;
                    was_online = 1;
                    probe_set_quiet(1);
                    total = 0;
                    bidx = 0;
                } else {
                    log_write(LOG_WARN, "接口报成功但复测不在线，8 秒后重试");
                    sleep_slices(8000);
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

    /* 复测短超时的默认值：改坏了不会报错，只会让会话回收后的恢复悄悄变慢 */
    {
        Config c;
        config_defaults(&c);
        if (c.recheck_timeout != 2 || c.probe_timeout != 6) {
            log_write(LOG_ERR, "自检失败：复测超时默认值不对（recheck=%d probe=%d）",
                      c.recheck_timeout, c.probe_timeout);
            ++fails;
        } else {
            log_write(LOG_INFO, "自检通过：复测超时默认值（recheck=2s / probe=6s）");
        }
    }

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

    /* has_i：ASCII 大小写不敏感包含（inuse 分类的核心，2026-11-01 真机踩过 InUse 写法） */
    {
        int ok = 1;
        ok &= has_i("inuse, login again", "inuse");
        ok &= has_i("InUse", "inuse");
        ok &= has_i("INUSE", "inuse");
        ok &= has_i("xxInUseyy", "inuse");
        ok &= !has_i("login again", "inuse");  /* 不应命中 */
        ok &= !has_i("", "inuse");             /* 空串 */
        ok &= !has_i("inuse", "");             /* 空 needle */
        if (!ok) { log_write(LOG_ERR, "自检失败：has_i 大小写不敏感匹配"); ++fails; }
        else log_write(LOG_INFO, "自检通过：has_i（大小写不敏感包含）");
    }

    /* iface_name_ok：网卡名白名单（会拼进 system() 命令，防注入） */
    {
        int ok = 1;
        ok &= iface_name_ok("wan");
        ok &= iface_name_ok("eth0.1");
        ok &= iface_name_ok("br-lan");
        ok &= iface_name_ok("veth_a-1");
        ok &= !iface_name_ok("");                     /* 空 */
        ok &= !iface_name_ok("wan; rm -rf /");        /* 注入 */
        ok &= !iface_name_ok("wan`reboot`");          /* 反引号 */
        ok &= !iface_name_ok("a very long interface name");  /* 超长 */
        if (!ok) { log_write(LOG_ERR, "自检失败：iface_name_ok 白名单"); ++fails; }
        else log_write(LOG_INFO, "自检通过：iface_name_ok（网卡名白名单）");
    }

    /* 探测间隔下限（纯函数，不依赖配置文件） */
    {
        int ok = 1;
        ok &= clamp_interval(1) == 30;
        ok &= clamp_interval(9) == 30;
        ok &= clamp_interval(10) == 10;
        ok &= clamp_interval(30) == 30;
        ok &= clamp_interval(60) == 60;
        if (!ok) { log_write(LOG_ERR, "自检失败：clamp_interval 下限"); ++fails; }
        else log_write(LOG_INFO, "自检通过：探测间隔钳制（<10 秒回到 30 秒）");
    }

    /* 取证掩码：同一输入恒定、不同输入不同、且不泄露原值 */
    {
        char a[16], b[16], c[16];
        mask_id("1004c101ed75", a, sizeof a);
        mask_id("1004c101ed75", b, sizeof b);
        mask_id("0010f371831f", c, sizeof c);
        if (strcmp(a, b) != 0 || strcmp(a, c) == 0 ||
            strstr(a, "1004c101ed75") != NULL || strlen(a) != 8) {
            log_write(LOG_ERR, "自检失败：mask_id 稳定哈希脱敏"); ++fails;
        } else log_write(LOG_INFO, "自检通过：mask_id（取证字段稳定脱敏）");
    }

    /* DNS 失败原因码分类（阶段 A 的入口，错误串由 net.c 生成） */
    {
        int ok = 1;
        ok &= net_is_dns_error("域名解析失败：connectivitycheck.platform.hicloud.com");
        ok &= !net_is_dns_error("连接超时（目标无响应，疑似被丢包/黑洞）");
        ok &= !net_is_dns_error("主机/网络不可达（网关不通或路由不可达）");
        ok &= !net_is_dns_error("");
        ok &= !net_is_dns_error(NULL);
        if (!ok) { log_write(LOG_ERR, "自检失败：net_is_dns_error 分类"); ++fails; }
        else log_write(LOG_INFO, "自检通过：net_is_dns_error（DNS 失败原因码）");
    }

    /* DNS 结果缓存（阶段 B）：命中 / 覆盖 / 未命中 / 过期 / 清空 */
    {
        char ip[64];
        int ok = 1;
        dns_cache_clear();
        ok &= dns_cache_get("a.example", ip, sizeof ip) == 0;
        dns_cache_put("a.example", "192.0.2.1");
        ok &= dns_cache_get("a.example", ip, sizeof ip) == 1 && strcmp(ip, "192.0.2.1") == 0;
        dns_cache_put("a.example", "192.0.2.9");   /* 同 host 覆盖 */
        ok &= dns_cache_get("a.example", ip, sizeof ip) == 1 && strcmp(ip, "192.0.2.9") == 0;
        ok &= dns_cache_get("b.example", ip, sizeof ip) == 0;
        dns_cache_put_at("exp.example", "192.0.2.7",
                         now_ms() - (long long)CNK_DNS_CACHE_TTL * 1000 - 1);
        ok &= dns_cache_get("exp.example", ip, sizeof ip) == 0;   /* 过期不认 */
        dns_cache_clear();
        ok &= dns_cache_get("a.example", ip, sizeof ip) == 0;
        if (!ok) { log_write(LOG_ERR, "自检失败：dns_cache 缓存/过期"); ++fails; }
        else log_write(LOG_INFO, "自检通过：dns_cache（命中/覆盖/过期/清空）");
    }

    /* 配置重载：合法生效 / 坏配置一个字节都不许改 / 失败不重置退避 */
    {
        const char *path = "cnk-selftest-reload.conf";
        char saved_path[256];
        Config base, cur;
        char err[160];
        FILE *f;
        int ok = 1, bidx, total, stopped;

        config_defaults(&base);
        snprintf(base.username, sizeof base.username, "before");
        snprintf(base.auth_url, sizeof base.auth_url, "http://10.0.0.1/");
        base.probe_interval = 30;
        base.log_level = LOG_INFO;
        base.log_dir[0] = '\0';              /* 自检不落盘 */

        /* ① 合法：只改两个键，文件里没写的键必须保留旧值 */
        cur = base;
        f = fopen(path, "w");
        if (!f) { log_write(LOG_ERR, "自检失败：无法创建临时配置"); ++fails; }
        else {
            fprintf(f, "username=after\nprobe_interval=45\n");
            fclose(f);
            if (config_load(path, &cur, err, sizeof err) != 0) ok = 0;
            if (strcmp(cur.username, "after") != 0) ok = 0;
            if (cur.probe_interval != 45) ok = 0;
            if (strcmp(cur.auth_url, "http://10.0.0.1/") != 0) ok = 0;   /* 未写 → 保留 */
        }

        /* ② 坏配置：前两行合法、第三行缺 '='。解析必须失败，且已写进去的值全部回滚 */
        {
            Config before_bad = cur;
            f = fopen(path, "w");
            if (!f) { log_write(LOG_ERR, "自检失败：无法写临时配置"); ++fails; }
            else {
                fprintf(f, "username=leaked\nprobe_interval=99\nno-equals-here\n");
                fclose(f);
                if (config_load(path, &cur, err, sizeof err) == 0) ok = 0;   /* 必须失败 */
                if (memcmp(&cur, &before_bad, sizeof cur) != 0) ok = 0;      /* 一字节不许变 */
            }
        }

        /* ③ apply_reload：失败不动退避/停止态，成功才归零并把 log_level 应用下去 */
        snprintf(saved_path, sizeof saved_path, "%s", g_config_path);
        snprintf(g_config_path, sizeof g_config_path, "%s", path);

        log_write(LOG_INFO, "自检：下一行 ERROR 是故意用坏配置触发的，属正常");
        bidx = 3; total = 7; stopped = 1;
        if (apply_reload(&cur, &bidx, &total, &stopped) != 0) ok = 0;        /* 坏配置还在盘上 */
        if (bidx != 3 || total != 7 || stopped != 1) ok = 0;

        f = fopen(path, "w");
        if (!f) { log_write(LOG_ERR, "自检失败：无法写临时配置"); ++fails; }
        else {
            fprintf(f, "username=reloaded\nprobe_interval=60\nlog_level=1\n");
            fclose(f);
            if (apply_reload(&cur, &bidx, &total, &stopped) != 1) ok = 0;
            if (bidx != 0 || total != 0 || stopped != 0) ok = 0;
            if (strcmp(cur.username, "reloaded") != 0) ok = 0;
            if (cur.probe_interval != 60) ok = 0;
            if (g_level != LOG_WARN) ok = 0;     /* log_level 真的落到全局了 */
        }
        snprintf(g_config_path, sizeof g_config_path, "%s", saved_path);
        log_init(LOG_INFO, NULL);                /* 后续自检日志仍要可见 */

        remove(path);
        if (!ok) { log_write(LOG_ERR, "自检失败：配置重载（事务/日志应用/退避）"); ++fails; }
        else log_write(LOG_INFO,
                       "自检通过：配置重载（事务替换 / log_level 生效 / 失败不重置退避）");
    }

    /* 构建身份：CI 用 strings 校验它确实烧进了交叉产物 */
    {
        const char *id = CNK_BUILD_ID;
        if (!strstr(id, "git ") || !strstr(id, "built ")) {
            log_write(LOG_ERR, "自检失败：构建身份缺失"); ++fails;
        } else log_write(LOG_INFO, "自检通过：构建身份 %s", id);
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


/* ---------------- 诊断日志导出（D1） ----------------
 *
 * 只读采集：只打开**自己的**日志文件（YYYY-MM-DD.log / blips-YYYY-MM-DD.log），
 * 不改配置、不写别的路径。输出前脱敏，先脱敏再落盘。
 *
 * 与 Windows 端 core/DiagExport.cpp 同一套语义：
 *   · 默认最近 15 分钟；--around "YYYY-MM-DD HH:MM:SS" 取前后各 2 分钟；
 *   · 跨午夜（窗口跨两天就都读）、tmpfs 回退目录（/tmp/cnkrouter）都覆盖；
 *   · 来源缺失、输出截断都写在头部，不假装完整。
 */
#define DIAG_MAX_LINES 5000
#define DIAG_MAX_BYTES (2 * 1024 * 1024)
#define DIAG_NEAR_SECONDS 120

static void diag_stamp(time_t t, char *buf, size_t cap) {
    struct tm tmv;
#ifdef _WIN32
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    strftime(buf, cap, "%Y-%m-%d %H:%M:%S", &tmv);
}

static void diag_ymd(time_t t, char *buf, size_t cap) {
    struct tm tmv;
#ifdef _WIN32
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    strftime(buf, cap, "%Y-%m-%d", &tmv);
}

static int diag_parse_ts(const char *s, time_t *out) {
    int Y, M, D, h, m, sec;
    struct tm tmv;
    if (sscanf(s, "%4d-%2d-%2d %2d:%2d:%2d", &Y, &M, &D, &h, &m, &sec) != 6) return 0;
    if (Y < 1970 || M < 1 || M > 12 || D < 1 || D > 31) return 0;
    memset(&tmv, 0, sizeof tmv);
    tmv.tm_year = Y - 1900;
    tmv.tm_mon = M - 1;
    tmv.tm_mday = D;
    tmv.tm_hour = h;
    tmv.tm_min = m;
    tmv.tm_sec = sec;
    tmv.tm_isdst = -1;
    *out = mktime(&tmv);
    return *out != (time_t)-1;
}

static int diag_name_ok(const char *name, char *ymd, size_t cap) {
    const char *p = name;
    size_t n, k;
    if (strncmp(p, "blips-", 6) == 0) p += 6;
    n = strlen(p);
    if (n != 14 || strcmp(p + 10, ".log") != 0) return 0;
    for (k = 0; k < 10; ++k) {
        if (k == 4 || k == 7) {
            if (p[k] != '-') return 0;
        } else if (p[k] < '0' || p[k] > '9') {
            return 0;
        }
    }
    snprintf(ymd, cap, "%.10s", p);
    return 1;
}

static int diag_key_before(const char *s, size_t i) {
    return i == 0 || !(isalnum((unsigned char)s[i - 1]) || s[i - 1] == '_');
}

/* 键值对：password=/userid=/username=... 的值整体打码 */
static size_t diag_key_len(const char *s, size_t i) {
    static const char *keys[] = {"password", "passwd", "pwd", "token", "secret",
                                 "userid", "username", "user", "account"};
    size_t k;
    for (k = 0; k < sizeof keys / sizeof keys[0]; ++k) {
        size_t n = strlen(keys[k]);
        if (strncasecmp(s + i, keys[k], n) == 0 && s[i + n] == '=') return n;
    }
    return 0;
}

/* 单行脱敏。中文原样保留（按字节扫描，只替换 ASCII 片段）。 */
static void diag_redact(const char *in, char *out, size_t cap) {
    size_t i = 0, o = 0;
    while (in[i] && o + 8 < cap) {
        size_t kl = 0;
        if (diag_key_before(in, i) && (kl = diag_key_len(in, i)) != 0) {
            memcpy(out + o, in + i, kl + 1);   /* key= */
            o += kl + 1;
            memcpy(out + o, "***", 3);
            o += 3;
            i += kl + 1;
            while (in[i] && !isspace((unsigned char)in[i]) &&
                   in[i] != '&' && in[i] != ',' && in[i] != ';') ++i;
            continue;
        }
        if (in[i] == '?') {                     /* 门户查询参数整体打码 */
            memcpy(out + o, "?<REDACTED>", 11);
            o += 11;
            ++i;
            while (in[i] && !isspace((unsigned char)in[i])) ++i;
            continue;
        }
        if (isdigit((unsigned char)in[i])) {
            int a = 0, b = 0, c = 0, d = 0, used = 0;
            if (sscanf(in + i, "%3d.%3d.%3d.%3d%n", &a, &b, &c, &d, &used) == 4 &&
                used > 0 && diag_key_before(in, i)) {
                o += (size_t)snprintf(out + o, cap - o, "%d.%d.x.x", a, b);
                i += (size_t)used;
                continue;
            }
            {
                size_t j = i;
                while (isdigit((unsigned char)in[j])) ++j;
                if (j - i == 12 && diag_key_before(in, i)) {
                    memcpy(out + o, "***", 3);
                    o += 3;
                    i = j;
                    continue;
                }
            }
        }
        out[o++] = in[i++];
    }
    out[o] = '\0';
}

static int diag_cmp_line(const void *a, const void *b) {
    return strncmp(*(const char *const *)a, *(const char *const *)b, 19);
}

static int run_diagnose(const Config *cfg, int minutes, const char *around, const char *out_path) {
    const time_t now = time(NULL);
    time_t start, end;
    char tsbuf[32], s1[32], s2[32];
    const char *dirs[2];
    int ndirs = 0, d, n = 0, cap_lines = 0, truncated = 0;
    char **lines = NULL;
    size_t bytes = 0;
    char missing[4][16];
    int nmissing = 0;
    char sources[8][160];
    int nsources = 0;
    FILE *out = stdout;

    if (around && around[0]) {
        if (!diag_parse_ts(around, &start)) {
            fprintf(stderr, "--around 的时间格式应为 \"YYYY-MM-DD HH:MM:SS\"\n");
            return 64;
        }
        start -= DIAG_NEAR_SECONDS;
        end = start + 2 * DIAG_NEAR_SECONDS;
    } else {
        if (minutes <= 0) minutes = 15;
        if (minutes > 24 * 60) minutes = 24 * 60;
        end = now;
        start = end - (time_t)minutes * 60;
    }

    if (out_path && out_path[0]) {
        out = fopen(out_path, "w");
        if (!out) {
            fprintf(stderr, "无法写入 %s\n", out_path);
            return 4;
        }
#ifndef _WIN32
        chmod(out_path, 0600);
#endif
    }

    /* 目录：生效的 log_dir + tmpfs 回退（去重） */
    dirs[ndirs++] = cfg->log_dir;
    if (strcmp(cfg->log_dir, "/tmp/cnkrouter") != 0) dirs[ndirs++] = "/tmp/cnkrouter";

    {
        char day[4][16];
        int nday = 0;
        time_t t;
        for (t = start; t <= end && nday < 4; t += 43200) {
            char dd[16];
            diag_ymd(t, dd, sizeof dd);
            if (nday == 0 || strcmp(day[nday - 1], dd) != 0) snprintf(day[nday++], 16, "%s", dd);
        }
        {
            char dd[16];
            diag_ymd(end, dd, sizeof dd);
            if (nday == 0 || strcmp(day[nday - 1], dd) != 0) snprintf(day[nday++], 16, "%s", dd);
        }

        for (d = 0; d < nday; ++d) {
            int found = 0;
            int k;
            for (k = 0; k < ndirs; ++k) {
                static const char *prefixes[2] = {"", "blips-"};
                int pi;
                for (pi = 0; pi < 2; ++pi) {
                    char path[CNK_PATH_MAX + 32];
                    FILE *fp;
                    char line[1024];
                    char ymd[16];
                    snprintf(path, sizeof path, "%s/%s%s.log", dirs[k], prefixes[pi], day[d]);
                    if (!diag_name_ok(strrchr(path, '/') + 1, ymd, sizeof ymd)) continue;
                    fp = fopen(path, "r");
                    if (!fp) continue;
                    found = 1;
                    if (nsources < 8) snprintf(sources[nsources++], 160, "%s", path);
                    while (!truncated && fgets(line, sizeof line, fp)) {
                        time_t t2 = 0;
                        size_t len = strlen(line);
                        while (len && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = '\0';
                        if (len < 19 || !diag_parse_ts(line, &t2)) continue;
                        if (t2 < start || t2 > end) continue;
                        if (n >= DIAG_MAX_LINES) { truncated = 1; break; }
                        {
                            char red[1024];
                            char *dup;
                            diag_redact(line, red, sizeof red);
                            if (bytes + strlen(red) > DIAG_MAX_BYTES) { truncated = 1; break; }
                            dup = (char *)malloc(strlen(red) + 1);
                            if (!dup) { truncated = 1; break; }
                            strcpy(dup, red);
                            if (n == cap_lines) {
                                int ncap = cap_lines ? cap_lines * 2 : 256;
                                char **grown = (char **)realloc(lines, (size_t)ncap * sizeof(char *));
                                if (!grown) { free(dup); truncated = 1; break; }
                                lines = grown;
                                cap_lines = ncap;
                            }
                            lines[n++] = dup;
                            bytes += strlen(red) + 1;
                        }
                    }
                    fclose(fp);
                }
            }
            if (!found && nmissing < 4) snprintf(missing[nmissing++], 16, "%s", day[d]);
        }
    }

    if (n > 1) qsort(lines, (size_t)n, sizeof(char *), diag_cmp_line);

    diag_stamp(now, tsbuf, sizeof tsbuf);
    diag_stamp(start, s1, sizeof s1);
    diag_stamp(end, s2, sizeof s2);
    fprintf(out, "# cnkrouter 诊断日志\n");
    fprintf(out, "# 生成时间: %s\n", tsbuf);
    fprintf(out, "# 版本: %s\n", CNK_BUILD_ID);
    if (around && around[0])
        fprintf(out, "# 时间窗: %s ~ %s（故障时刻前后各 2 分钟）\n", s1, s2);
    else
        fprintf(out, "# 时间窗: %s ~ %s（最近 %d 分钟）\n", s1, s2, minutes);
    fprintf(out, "# 来源: ");
    if (nsources == 0) {
        fprintf(out, "(无)");
    } else {
        int k;
        for (k = 0; k < nsources; ++k) fprintf(out, "%s%s", k ? ", " : "", sources[k]);
    }
    fprintf(out, "\n");
    if (nmissing) {
        int k;
        fprintf(out, "# 缺失: ");
        for (k = 0; k < nmissing; ++k) fprintf(out, "%s%s", k ? ", " : "", missing[k]);
        fprintf(out, "（这些日期在两个目录里都没有日志文件）\n");
    }
    if (truncated)
        fprintf(out, "# 截断: 是（已达 %d 行 / %d 字节上限）\n", DIAG_MAX_LINES, DIAG_MAX_BYTES);
    fprintf(out, "# 脱敏: 账号与凭据 -> ***；门户查询参数 -> ?<REDACTED>；"
                 "IP -> a.b.x.x；12 位数字 -> ***\n");
    fprintf(out, "# --------------------------------------------------------\n");

    {
        int k;
        for (k = 0; k < n; ++k) {
            fprintf(out, "%s\n", lines[k]);
            free(lines[k]);
        }
    }
    free(lines);
    if (truncated) fprintf(out, "# （输出已截断，仅保留前 %d 行）\n", n);

    if (out != stdout) fclose(out);
    return 0;
}

static void usage(void) {
    printf(
        "cnkrouter %s —— 校园网自动认证（路由器端）\n\n"
        "用法：cnkrouter [选项]\n\n"
        "  --config=<路径>   配置文件（默认 /etc/cnkrouter.conf）\n"
        "  --daemon          常驻运行（由 procd 托管）\n"
        "  --once            探测 + 必要时登录 + 复测，退出码 0=在线\n"
        "  --login           直接发起一次登录（不探测，调试用）\n"
        "  --probe           只探测一次，退出码 0=在线 3=离线 2=门户\n"
        "  --diagnose        只读采集日志并脱敏输出（默认最近 15 分钟）\n"
        "  --minutes=N       --diagnose 的时间窗（分钟，默认 15）\n"
        "  --around=时间     --diagnose 取该时刻前后各 2 分钟（\"YYYY-MM-DD HH:MM:SS\"）\n"
        "  --out=文件        --diagnose 写入文件（默认 stdout，权限 600）\n"
        "  --user=<账号>      覆盖配置里的账号\n"
        "  --pass=<密码>      覆盖配置里的密码\n"
        "  --verbose         输出调试日志\n"
        "  --quiet           只输出错误\n"
        "  --selftest        本地自检（AES/Base64/URL/JSON，不联网）\n"
        "  --version         显示版本（含 git 提交与构建时间）\n"
        "  --help            显示本帮助\n",
        CNK_VERSION);
}

int main(int argc, char **argv) {
    Config cfg;
    const char *config_path = g_config_path;
    const char *user = NULL, *pass = NULL;
    int mode = 0;   /* 0=help 1=daemon 2=once 3=login 4=probe 5=selftest 6=diagnose */
    int diag_minutes = 15;
    const char *diag_around = "";
    const char *diag_out = "";
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
        else if (strcmp(a, "--diagnose") == 0) mode = 6;
        else if (strncmp(a, "--minutes=", 10) == 0) diag_minutes = atoi(a + 10);
        else if (strncmp(a, "--around=", 9) == 0) diag_around = a + 9;
        else if (strncmp(a, "--out=", 6) == 0) diag_out = a + 6;
        else if (strncmp(a, "--user=", 7) == 0) user = a + 7;
        else if (strncmp(a, "--pass=", 7) == 0) pass = a + 7;
        else if (strcmp(a, "--verbose") == 0) cfg.log_level = LOG_DBG;
        else if (strcmp(a, "--quiet") == 0) cfg.log_level = LOG_ERR;
        else if (strcmp(a, "--version") == 0) { printf("cnkrouter %s\n", CNK_BUILD_ID); return 0; }
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
    if (mode == 1) log_init(cfg.log_level, cfg.log_dir);
    else           log_init(cfg.log_level, NULL);

    if (mode == 6) return run_diagnose(&cfg, diag_minutes, diag_around, diag_out);

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
        /* NET_DNS 按在线处理（守护处于观察态，不动作），输出 dns_degraded 便于识别 */
        return (pr.state == NET_ONLINE || pr.state == NET_DNS) ? 0 :
               (pr.state == NET_PORTAL ? 2 : 3);
    }
    if (mode == 3) {
        LoginResult lr = portal_login(&cfg);
        printf("%s\t%s\n", lr.ok ? "ok" : "fail", lr.message);
        return lr.ok ? 0 : 2;
    }
    if (mode == 2) return run_once(&cfg);
    return run_daemon(&cfg);
}
