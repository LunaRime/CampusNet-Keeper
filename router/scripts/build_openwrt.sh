#!/bin/sh
# 用 OpenWrt SDK 交叉编译 cnkrouter。
#
# 准备 SDK（以 x86_64 Linux 为例，版本要与路由器固件一致）：
#   wget https://downloads.openwrt.org/releases/23.05.5/targets/ramips/mt7621/openwrt-sdk-23.05.5-ramips-mt7621_gcc-12.3.0_musl.Linux-x86_64.tar.xz
#   tar xf openwrt-sdk-*.tar.xz
#
# 然后：
#   ./scripts/build_openwrt.sh /path/to/openwrt-sdk-23.05.5-ramips-mt7621...
#
# 产物：$SDK/bin/packages/<arch>/base/cnkrouter_*.ipk
set -e

SDK="$1"
if [ -z "$SDK" ] || [ ! -f "$SDK/rules.mk" ]; then
    echo "用法：$0 /path/to/openwrt-sdk"
    exit 1
fi

HERE=$(cd "$(dirname "$0")/.." && pwd)
PKG="$HERE/packaging/openwrt"

# SDK 的 package 目录只接受 package/<name>/，这里做一份带源码的副本
mkdir -p "$PKG/src"
cp "$HERE"/src/*.c "$HERE"/src/*.h "$PKG/src/"

rm -rf "$SDK/package/cnkrouter"
cp -r "$PKG" "$SDK/package/cnkrouter"

cd "$SDK"
make package/cnkrouter/compile V=s
make package/cnkrouter/install V=s

echo
echo "构建完成，产物："
find "$SDK/bin/packages" -name 'cnkrouter_*.ipk' -printf '%p  %s 字节\n'
