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
#ifndef _WIN32
#include <unistd.h>   /* unlink：软拔插标记文件（见 replug_flag_*） */
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

/* 跨天就切文件；目录不存在就建一次。
 * 每次写日志都会调它，但只是 strftime + strcmp，开销可忽略。 */
static void log_open_today(void) {
    char date[16], path[CNK_PATH_MAX + 32];
    time_t t = time(NULL);
    struct tm tmv;

    if (!g_log_dir[0]) return;
#ifdef _WIN32
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    strftime(date, sizeof date, "%Y-%m-%d", &tmv);
    if (g_logf && strcmp(date, g_log_date) == 0) return;

    if (g_logf) { fclose(g_logf); g_logf = NULL; }
    snprintf(g_log_date, sizeof g_log_date, "%s", date);
    snprintf(path, sizeof path, "%s/%s.log", g_log_dir, date);

    g_logf = fopen(path, "a");
    if (!g_logf && path_is_safe(g_log_dir)) {
        char cmd[CNK_PATH_MAX + 32];
        snprintf(cmd, sizeof cmd, "mkdir -p '%s' 2>/dev/null", g_log_dir);
        if (system(cmd) == 0) g_logf = fopen(path, "a");
    }
    if (!g_logf) fprintf(stderr, "[warn] 无法打开日志 %s\n", path);
}

void log_init(int level, const char *dir) {
    g_level = level;
    if (g_logf) { fclose(g_logf); g_logf = NULL; }
    g_log_date[0] = '\0';
    if (dir && dir[0]) snprintf(g_log_dir, sizeof g_log_dir, "%s", dir);
    else g_log_dir[0] = '\0';
    log_open_today();
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
    log_open_today();               /* 跨天自动换文件 */
#ifdef _WIN32
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", &tmv);

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
    if (c->probe_interval < 10) c->probe_interval = 60;
    if (c->link_after < 1) c->link_after = 2;
    if (c->link_cooldown < 60) c->link_cooldown = 900;
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

/* ---------------- 僵尸会话处置 ---------------- */

/* AC 的会话是按 **IP** 记的（chkstatus 回包里只有 v4ip/ss5，没有 MAC 字段）。
 * 当它记着"这个 IP 已认证"、转发面却已失效时，重登只会收到「终端IP已在线」——
 * 客户端判成功、实际照样不通，**重试再多次也没用**。用户实测的唯一解法是
 * 拔插网线（= 换 IP，让 AC 眼里变成另一台设备）。
 *
 * 这里按代价从低到高复刻人工做的三件事，每级都限流，避免反复断网：
 *   ① 注销会话            不中断连接
 *   ② 重启 WAN 换 IP      断 3~5 秒，≈ 拔插网线
 *   ③ 改 MAC 换身份       最重，30 分钟最多一次
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
    static time_t t_logout = 0, t_ifup = 0, t_mac = 0;
    time_t now = time(NULL);

    /* 默认只记录。处置里含"换 IP / 换 MAC"这类主动断网动作，
     * 判定一旦误报就会亲手制造故障 —— 在拿到真实故障的日志证据之前，
     * 程序只负责把现场记清楚，不替用户做决定。需要它动手时把
     * /etc/config/cnkrouter 的 inuse_action 调到 1/2/3。 */
    if (c->inuse_action <= 0) {
        log_write(LOG_WARN,
                  "疑似僵尸会话：服务器说已在线但实际不通。"
                  "inuse_action=0，只记录不动作（要自动处置请调高该项）");
        sleep_slices(60000);
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
    /* ② 换 IP（≈ 拔插网线）。Windows 上没有 ubus，编译期就排除 */
    if (c->inuse_action >= 2 && now - t_ifup >= 1800) {
        t_ifup = now;
        log_write(LOG_WARN, "僵尸会话处置②：重启 WAN 接口换 IP（会断 3~5 秒）");
        if (system("ubus call network.interface.wan down >/dev/null 2>&1; sleep 2; "
                   "ubus call network.interface.wan up >/dev/null 2>&1") != 0)
            log_write(LOG_WARN, "重启 WAN 的命令执行失败");
        sleep_slices(12000);
        return;
    }

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

    log_write(LOG_WARN, "僵尸会话处置手段已用尽，5 分钟后再试一轮");
    sleep_slices(300000);
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

static int run_daemon(Config *c) {
    static const int backoff[] = {15, 30, 60, 120, 300};
    int bidx = 0, total = 0, ever_ok = 0, stopped = 0, offline_streak = 0;
    int was_online = 0;   /* 上一轮是否在线：用于只在状态切换时打日志 */

    log_write(LOG_INFO, "cnkrouter %s 启动：账号=%s 认证地址=%s 探测间隔=%ds",
              CNK_VERSION, masked_account(c), c->auth_url, c->probe_interval);

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
            sleep_slices(c->probe_interval * 1000);
            continue;
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
        if (pr.state == NET_OFFLINE) {
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

        log_write(LOG_INFO, "检测到门户劫持：%s", pr.message);
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
    if (mode == 1) log_init(cfg.log_level, cfg.log_dir);
    else           log_init(cfg.log_level, NULL);

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
