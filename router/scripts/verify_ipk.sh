#!/bin/sh
# OpenWrt 包的安装清单 / 权限校验（本地与 CI 共用）。
#
#   无参数              静态检查：packaging Makefile 引用的 files/* 都在，且没有未安装的孤儿
#   <cnkrouter.ipk>     读包检查：清单、权限、默认不装 crontab
#
# 为什么有它（2026-11-24 R1）：CI 之前只交叉编裸二进制，从没验证 .ipk 里到底装了什么；
# 而 cnk-blip 因 .gitignore 的 [Bb]in/ 一直缺席仓库，干净检出构建 .ipk 会直接失败。
# 清单在这里**写死**、不去解析 Makefile —— 两处独立表述才能发现漏装/多装。
#
# 权限为什么读 data.tar.gz 的条目头、而不是解包后 stat：宿主文件系统的 chmod 语义不同
# （Windows/Git Bash 会把 0600 落成 0644），stat 出来的是宿主副作用，不是包内事实；
# 条目头里的 mode 才是 opkg 解包时真正用的值。提取完整性另外单独验一次。
set -u

HERE=$(cd "$(dirname "$0")/.." && pwd)
PKG="$HERE/packaging/openwrt"

EXPECT='usr/bin/cnkrouter|rwxr-xr-x
usr/bin/cnk-cleanup|rwxr-xr-x
usr/bin/cnk-traffic|rwxr-xr-x
usr/bin/cnk-blip|rwxr-xr-x
etc/init.d/cnkrouter|rwxr-xr-x
etc/config/cnkrouter|rw-------
etc/hotplug.d/iface/90-cnkrouter|rwxr-xr-x'

static_check() {
    fail=0
    # 正向：Makefile 每一处 ./files/... 都必须在工作区存在且非空
    for f in $(sed -n 's|.*\./files/\([^ ]*\).*|\1|p' "$PKG/Makefile"); do
        if [ ! -s "$PKG/files/$f" ]; then
            echo "缺失或为空: files/$f （Makefile 有引用）"
            fail=1
        fi
    done
    # 反向：files/ 下每个文件都应被 Makefile 安装（防止脚本入库却装不进包）
    for f in $(cd "$PKG/files" && find . -type f | sed 's|^\./||'); do
        grep -Fq "./files/$f" "$PKG/Makefile" || {
            echo "未被 Makefile 安装: files/$f"
            fail=1
        }
    done
    [ "$fail" -eq 0 ] && echo "静态检查通过：Makefile 引用与 files/ 内容一一对应"
    return "$fail"
}

ipk_check() {
    ipk=$1
    [ -f "$ipk" ] || { echo "!! 找不到 $ipk"; return 1; }

    if ! tar -tzf "$ipk" >/dev/null 2>&1; then
        echo "!! 不是 gzip tar：opkg 的 .ipk 应含 ./data.tar.gz（apk 是 ar 归档）"
        return 1
    fi

    # 先证明 data.tar.gz 真的能完整解出来（不落盘，避免宿主 chmod 失真）
    if ! tar -xzOf "$ipk" ./data.tar.gz 2>/dev/null | tar -tzf - >/dev/null 2>&1; then
        echo "!! data.tar.gz 无法完整读出"
        return 1
    fi

    actual=$(tar -xzOf "$ipk" ./data.tar.gz 2>/dev/null \
        | tar -tvzf - 2>/dev/null \
        | awk '$1 ~ /^d/ { next } { sub(/^\.\//, "", $6); printf "%s|%s\n", $6, substr($1, 2) }' \
        | sort)
    want=$(printf '%s\n' "$EXPECT" | sort)

    fail=0
    if [ "$actual" != "$want" ]; then
        echo "!! 安装清单或权限与期望不一致"
        echo "--- 期望 ---"
        printf '%s\n' "$want"
        echo "--- 实际 ---"
        printf '%s\n' "$actual"
        fail=1
    fi

    # 短中断探针必须默认不启动：包里出现任何 crontab 都算越界
    if printf '%s\n' "$actual" | grep -q 'crontabs'; then
        echo "!! 包内出现 crontabs —— cnk-blip 必须默认不启动"
        fail=1
    fi

    echo "包内清单（$ipk）："
    printf '%s\n' "$actual" | sed 's/|/  /; s|^|  /|'

    [ "$fail" -eq 0 ] && echo "ipk 校验通过：清单与权限符合预期，未安装 crontab"
    return "$fail"
}

if [ $# -eq 0 ]; then
    static_check
else
    ipk_check "$1"
fi
