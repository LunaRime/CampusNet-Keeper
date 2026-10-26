#!/bin/sh
# 把已编译好的 cnkrouter 推到路由器并启用（在你自己的电脑上执行）。
#
# 用法：
#   ./scripts/install_openwrt.sh root@192.168.1.1 ./build/cnkrouter
#
# 不会覆盖已有配置：/etc/config/cnkrouter 若已存在则保留。
set -e

TARGET="$1"
BIN="$2"

if [ -z "$TARGET" ] || [ -z "$BIN" ] || [ ! -f "$BIN" ]; then
    echo "用法：$0 <user@router> <cnkrouter 二进制>"
    exit 1
fi

HERE=$(cd "$(dirname "$0")/.." && pwd)

echo "==> 上传二进制"
scp "$BIN" "$TARGET:/tmp/cnkrouter"
ssh "$TARGET" 'chmod +x /tmp/cnkrouter && mv /tmp/cnkrouter /usr/bin/cnkrouter && /usr/bin/cnkrouter --selftest'

echo "==> 上传启动脚本"
scp "$HERE/packaging/openwrt/files/etc/init.d/cnkrouter" "$TARGET:/etc/init.d/cnkrouter"
ssh "$TARGET" 'chmod +x /etc/init.d/cnkrouter'

echo "==> 上传 UCI 配置（已存在则保留）"
ssh "$TARGET" 'test -f /etc/config/cnkrouter' || \
    scp "$HERE/packaging/openwrt/files/etc/config/cnkrouter" "$TARGET:/etc/config/cnkrouter"

echo "==> 上传 hotplug 钩子"
scp "$HERE/packaging/openwrt/files/etc/hotplug.d/iface/90-cnkrouter" "$TARGET:/etc/hotplug.d/iface/90-cnkrouter"
ssh "$TARGET" 'chmod +x /etc/hotplug.d/iface/90-cnkrouter'

echo
echo "下一步（在路由器上）："
echo "  vi /etc/config/cnkrouter        # 填账号密码、确认 auth_url"
echo "  chmod 600 /etc/config/cnkrouter"
echo "  /etc/init.d/cnkrouter enable && /etc/init.d/cnkrouter start"
echo "  logread -e cnkrouter -f"
