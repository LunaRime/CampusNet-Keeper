#!/bin/sh
# init.d/cnkrouter 的 render_config 行为回归（不需要 OpenWrt 环境）。
#
# 2026-11-24 R2：旧实现 `{ ... } > $CONF_FILE` 会先把**正在生效**的配置截断，
# 中途失败就留下半截文件 —— 守护进程下一次 SIGHUP 读到的就是坏的。
# 这种事故只在路由器上才看得到，用 stub 把 UCI/procd 换掉就能在本地复现。
#
# 用法：sh tests/test_init_render.sh      （CI 与本地都跑）
set -u

HERE=$(cd "$(dirname "$0")/.." && pwd)
INITD="$HERE/packaging/openwrt/files/etc/init.d/cnkrouter"
TMP=$(mktemp -d) || exit 1
command -v cygpath >/dev/null 2>&1 && TMP=$(cygpath -u "$TMP")   # Git Bash 会给 C:\...
trap 'rm -rf "$TMP" 2>/dev/null || true' EXIT INT TERM

# ---------------- OpenWrt 侧 stub ----------------
config_load() { :; }
config_get() {
	# config_get <变量名> <section> <option> <默认值>
	eval "v=\${STUB_$3:-\$4}"
	eval "$1=\$v"
}
config_get_bool() { config_get "$@"; }
logger() { :; }
pidof() { return 1; }
procd_open_instance() { :; }
procd_set_param() { :; }
procd_close_instance() { :; }
procd_add_reload_trigger() { :; }

CNK_CONF_DIR="$TMP"
export CNK_CONF_DIR
. "$INITD"

CONF="$TMP/cnkrouter.conf"
fail=0
ok()  { echo "ok   $1"; }
bad() { echo "FAIL $1"; fail=1; }
keys() { grep -c '^[A-Za-z_][A-Za-z0-9_]*=' "$CONF"; }

# 1) 合法配置：渲染成功、权限 600、键齐全、值正确
STUB_username=202400000000
STUB_password=secret
STUB_log_level=3
if render_config; then ok "合法配置渲染成功"; else bad "合法配置渲染失败"; fi
[ -f "$CONF" ] && ok "配置文件已生成" || bad "配置文件缺失"
case "$(uname -s)" in
	MINGW*|MSYS*|CYGWIN*) PERM_CHECK=0 ;;   # Windows 文件系统不保留 POSIX 权限位
	*) PERM_CHECK=1 ;;
esac
if [ "$PERM_CHECK" = "1" ]; then
	[ "$(stat -c '%a' "$CONF")" = "600" ] && ok "权限 600" || bad "权限不是 600：$(stat -c '%a' "$CONF" 2>/dev/null)"
else
	echo "skip 权限 600（Git Bash 不保留 POSIX 权限位，CI 上会真跑）"
fi
[ "$(keys)" = "19" ] && ok "19 个键齐全" || bad "键数不对：$(keys)"
grep -qx 'username=202400000000' "$CONF" && ok "username 正确" || bad "username 错误"
grep -qx 'log_level=3' "$CONF" && ok "log_level 正确" || bad "log_level 错误"
grep -qx 'log_dir=/mnt/data/log/cnkrouter' "$CONF" && ok "默认 log_dir 正确" || bad "默认 log_dir 错误"

BEFORE=$(cat "$CONF")

# 2) 值里含换行 → 拒绝渲染，旧配置一字节不变，临时文件清理干净
STUB_password=$(printf 'x\nlog_level=0')
if render_config; then bad "含换行的值应被拒绝"; else ok "含换行的值被拒绝"; fi
[ "$(cat "$CONF")" = "$BEFORE" ] && ok "旧配置未被改动" || bad "旧配置被破坏"
[ -z "$(ls "$TMP"/cnkrouter.conf.* 2>/dev/null)" ] && ok "临时文件已清理" || bad "残留临时文件"

# 3) 缺 password → 拒绝渲染，旧配置不变
STUB_password=
if render_config; then bad "缺 password 应被拒绝"; else ok "缺 password 被拒绝"; fi
[ "$(cat "$CONF")" = "$BEFORE" ] && ok "旧配置仍未被改动" || bad "旧配置被破坏"

# 4) enabled=0 → 直接跳过，不碰现有文件
STUB_password=secret
STUB_enabled=0
if render_config; then bad "enabled=0 时不该渲染"; else ok "enabled=0 时跳过"; fi
[ "$(cat "$CONF")" = "$BEFORE" ] && ok "enabled=0 未动旧配置" || bad "enabled=0 改坏了旧配置"

echo
if [ "$fail" -eq 0 ]; then
	echo "全部通过"
	exit 0
fi
echo "有失败项"
exit 1
