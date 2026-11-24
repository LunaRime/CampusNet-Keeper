#!/bin/sh
# cnk-cleanup 的行为回归：目录/天数取自生效配置、只删 CNK 命名、dry-run 不动手。
#
# 2026-11-24 R3：旧版硬编码目录和天数，还会清 /tmp 顶层别人家的文件。
# 这里全在临时目录里做，不碰本机任何真实日志；也刻意用 busybox 兼容的写法，
# 以便在真机同版 busybox 里跑同样的断言。

set -u
HERE=$(cd "$(dirname "$0")/.." && pwd)
SCRIPT="$HERE/packaging/openwrt/files/usr/bin/cnk-cleanup"
TMP=$(mktemp -d) || exit 1
command -v cygpath >/dev/null 2>&1 && TMP=$(cygpath -u "$TMP")
trap 'rm -rf "$TMP" 2>/dev/null || true' EXIT INT TERM

LOG="$TMP/data"
FALLBACK="$TMP/fb"
CONF="$TMP/cnkrouter.conf"

fail=0
ok()  { echo "ok   $1"; }
bad() { echo "FAIL $1"; fail=1; }

# 固定一个很老的 mtime：touch -d 在 busybox / GNU 上都认这个格式，
# 不依赖相对时间写法，也不用 12 位纯数字（那会撞上脱敏审计的"学号样数字串"）。
mk_old() { printf 'x\n' > "$1/$2"; touch -d "2020-01-01 00:00:00" "$1/$2"; }
mk_new() { printf 'x\n' > "$1/$2"; }

mkdir -p "$LOG/traffic" "$FALLBACK"

mk_old "$LOG" "2026-01-01.log"          # CNK 主日志，该删（>3 天）
mk_old "$LOG" "blips-2026-01-01.log"    # 探针日志，该删
mk_old "$LOG" "other.log"               # 别的程序的日志：不许动
mk_new "$LOG" "$(date +%F).log"         # 当天：留
mk_old "$LOG/traffic" "2026-01-01.log"  # 子目录：归 cnk-traffic，不递归
mk_old "$FALLBACK" "2026-01-01.log"     # 回退目录只留 1 天：该删
mk_new "$FALLBACK" "$(date +%F).log"    # 当天：留
mk_old "$TMP" "unrelated.tmp"           # 模拟 /tmp 顶层别人家的文件：不许动

printf 'log_dir=%s\nlog_keep_days=3\n' "$LOG" > "$CONF"

echo "== dry-run（先验收，不许真删）=="
OUT=$(CNK_CONF="$CONF" CNK_FALLBACK_DIR="$FALLBACK" sh "$SCRIPT" --dry-run 2>&1)
[ -f "$LOG/2026-01-01.log" ] && ok "dry-run 未删除主日志" || bad "dry-run 删了主日志"
[ -f "$FALLBACK/2026-01-01.log" ] && ok "dry-run 未删除回退日志" || bad "dry-run 删了回退日志"
case "$OUT" in
    *2026-01-01.log*) ok "dry-run 列出了将删文件" ;;
    *) bad "dry-run 没有列出任何文件：$OUT" ;;
esac

echo "== 实删（天数取自配置：3）=="
CNK_CONF="$CONF" CNK_FALLBACK_DIR="$FALLBACK" sh "$SCRIPT" >/dev/null 2>&1
[ -f "$LOG/2026-01-01.log" ] && bad "超过 3 天的主日志没删" || ok "删除超过 3 天的主日志"
[ -f "$LOG/blips-2026-01-01.log" ] && bad "探针日志没删" || ok "删除超过 3 天的探针日志"
[ -f "$LOG/other.log" ] && ok "非 CNK 命名的日志保留" || bad "误删了别的程序的日志"
[ -f "$LOG/$(date +%F).log" ] && ok "当天日志保留" || bad "误删当天日志"
[ -f "$LOG/traffic/2026-01-01.log" ] && ok "子目录不递归（归 cnk-traffic）" || bad "递归删进了 traffic/"
[ -f "$FALLBACK/2026-01-01.log" ] && bad "回退目录旧日志没删" || ok "回退目录按 1 天清理"
[ -f "$FALLBACK/$(date +%F).log" ] && ok "回退目录当天保留" || bad "误删回退目录当天日志"
[ -f "$TMP/unrelated.tmp" ] && ok "不碰不属于它的文件" || bad "删了 /tmp 顶层别人家的文件"

echo "== 配置缺失：回退默认 7 天，不崩 =="
mk_old "$LOG" "2026-01-02.log"
CNK_CONF="$TMP/no-such.conf" CNK_LOG_DIR="$LOG" CNK_FALLBACK_DIR="$FALLBACK" \
    sh "$SCRIPT" >/dev/null 2>&1
rc=$?
[ "$rc" -eq 0 ] && ok "配置缺失仍正常退出" || bad "配置缺失时退出码 $rc"
[ -f "$LOG/2026-01-02.log" ] && bad "默认 7 天没生效" || ok "配置缺失时用默认 7 天"

echo
if [ "$fail" -eq 0 ]; then
    echo "全部通过"
    exit 0
fi
echo "有失败项"
exit 1
