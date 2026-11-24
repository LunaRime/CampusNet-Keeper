#!/bin/sh
# 用 OpenWrt SDK 交叉编译 cnkrouter。
#
# 准备 SDK（以 x86_64 Linux 为例，版本要与路由器固件一致）：
#   wget https://downloads.openwrt.org/releases/23.05.5/targets/ramips/mt7621/openwrt-sdk-23.05.5-ramips-mt7621_gcc-12.3.0_musl.Linux-x86_64.tar.xz
#   tar xf openwrt-sdk-*.tar.xz
#
# 然后：
#   sh scripts/build_openwrt.sh /path/to/openwrt-sdk-23.05.5-ramips-mt7621...
#
# 产物：$SDK/bin/packages/<arch>/base/cnkrouter_*.ipk
#
# 宿主依赖（Debian/Ubuntu；SDK 的 prereq 检查会逐个核对，缺一个就整体失败）：
#   build-essential zstd libncurses-dev git rsync gawk unzip python3 python3-distutils file
# 2026-11-24 实测：干净容器里上面这些不全，报的就是一行 "Prerequisite check failed"。
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

# package/ 副本不带 .git，取不到提交哈希 —— 在这里从仓库取好，用环境变量
# 传给 SDK 的 Build/Compile，让产物 `--version` 仍能对上是哪次提交编的。
export CNK_GIT_COMMIT="$(git -C "$HERE" rev-parse --short HEAD 2>/dev/null || echo unknown)"
echo "构建身份：git $CNK_GIT_COMMIT"

cd "$SDK"
# 干净 SDK 没有可用的 .config 时，compile 会掉进交互式 menuconfig；无 TTY 直接失败
# （2026-11-24 实测：Error opening terminal: unknown. → toplevel.mk:224）。
# defconfig 是幂等的，有 .config 时只补新符号的默认值。
make defconfig >/dev/null
# ipk 就在 compile 里产出：Build/Compile → Package/install → ipkg-build 一条链跑完。
# 别再加 `make package/cnkrouter/install` —— OpenWrt 没有这个目标，实测报
# “No rule to make target 'package/cnkrouter/install'”，而 ipk 其实已经躺在
# bin/packages/<arch>/base/ 里了。
make package/cnkrouter/compile V=s

echo
echo "构建完成，产物："
find "$SDK/bin/packages" -name 'cnkrouter_*.ipk' -printf '%p  %s 字节\n'
