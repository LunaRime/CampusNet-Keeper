#!/bin/sh
# cnkrouter-harden —— 校园网共享的基础伪装层（TTL 统一 + 可选 TCP 时间戳摘除）
#
# 只做两件事，都是内核态的包处理规则，**不增加任何常驻内存**：
#   1) 让所有出 WAN 的包的 TTL 变成同一个值（默认 128，与 Windows 一致）；
#      同时把路由器自己产生的包（门户登录、DNS、NTP）的默认 TTL 也设成同值，
#      否则 AC 会看到同一个 IP 出现两种 TTL —— 这本身就是"后面有设备"的铁证。
#   2) 可选：摘掉 TCP 时间戳选项（需内核带 xt_TCPOPTSTRIP）。
#
# 用法：
#   ./harden.sh                 应用（TTL=128）
#   ./harden.sh --ttl 64        改成 64（Linux/Android/iOS 的默认值）
#   ./harden.sh --ipv6 off      阻断 IPv6 转发（每台设备各自有 IPv6 地址 = 直接暴露台数）
#   ./harden.sh --strip-timestamp  摘 TCP 时间戳（内核不支持会明确报错，不静默）
#   ./harden.sh --status        看当前生效的规则
#   ./harden.sh --off           全部撤销
#
# 兼容：OpenWrt 22.03+（fw4/nftables）与旧版 iptables 固件（LEDE/Lean）。
# 不做：HTTP UA 改写（那是 UA2F 的事，见 README）。

set -u

NFT_FILE=/etc/nftables.d/10-cnkrouter.nft
SYSCTL_FILE=/etc/sysctl.d/90-cnkrouter-ttl.conf
RC_LOCAL=/etc/rc.local
CHAIN=cnk_post

TTL=128
IPV6=keep          # keep | off | unify
STRIP=0
ACTION=apply

usage() {
    cat <<'EOF'
用法：harden.sh [选项]

  --ttl N            出 WAN 的 IPv4 TTL 统一为 N（默认 128；0 = 不动）
  --ipv6 keep|off|unify   keep=不处理 / off=阻断 IPv6 转发 / unify=统一 hop limit
  --strip-timestamp  摘除出站 TCP 时间戳（需内核 xt_TCPOPTSTRIP）
  --status           查看当前生效的规则
  --off              撤销全部改动（含 sysctl 与 rc.local）
  -h, --help         本帮助
EOF
    exit "${1:-0}"
}

while [ $# -gt 0 ]; do
    case "$1" in
        --ttl)      [ $# -ge 2 ] || { echo "--ttl 缺参数" >&2; exit 64; }
                    TTL="$2"; shift 2 ;;
        --ttl=*)    TTL="${1#*=}"; shift ;;
        --ipv6)     [ $# -ge 2 ] || { echo "--ipv6 缺参数" >&2; exit 64; }
                    IPV6="$2"; shift 2 ;;
        --ipv6=*)   IPV6="${1#*=}"; shift ;;
        --strip-timestamp) STRIP=1; shift ;;
        --off)      ACTION=off; shift ;;
        --status)   ACTION=status; shift ;;
        -h|--help)  usage 0 ;;
        *)          echo "未知参数：$1" >&2; usage 64 ;;
    esac
done

case "$TTL" in ''|*[!0-9]*) echo "--ttl 必须是数字" >&2; exit 64 ;; esac
case "$IPV6" in keep|off|unify) ;; *) echo "--ipv6 只能是 keep/off/unify" >&2; exit 64 ;; esac

log() { echo "[cnkrouter-harden] $*"; }

backend() {
    if command -v nft >/dev/null 2>&1 && nft list table inet fw4 >/dev/null 2>&1; then
        echo nft
    elif command -v iptables >/dev/null 2>&1; then
        echo ipt
    else
        echo none
    fi
}

# WAN 出接口名：不同固件差别很大（wan / pppoe-wan / eth0.2 / wwan0），
# 从 UCI 取，取不到就把常见名字都放进集合里（不存在的名字不影响匹配）
wan_set() {
    devs=""
    u=""; i=""
    if command -v uci >/dev/null 2>&1; then
        u=$(uci -q get network.wan.device 2>/dev/null)
        i=$(uci -q get network.wan.ifname 2>/dev/null)
    fi
    for d in "$u" "$i" pppoe-wan wan wwan0; do
        [ -n "$d" ] || continue
        case ",$devs," in *",\"$d\","*) continue ;; esac
        devs="${devs:+$devs, }\"$d\""
    done
    echo "$devs"
}

# ---------------------------------------------------------------- nftables

nft_write() {
    mkdir -p /etc/nftables.d
    {
        echo "# 由 cnkrouter-harden 生成；改参数请重新运行脚本，不要手改"
        echo "chain $CHAIN {"
        echo "    type filter hook postrouting priority mangle; policy accept;"
        # 只改出 WAN 的包：转发流量与路由器自身发出的流量都会经过 postrouting，
        # 一条规则就够，不会出现"转发 127 / 自产 64"的不一致
        if [ "$TTL" != "0" ]; then
            echo "    oifname { $(wan_set) } ip ttl set $TTL"
        fi
        case "$IPV6" in
            off)   echo "    oifname { $(wan_set) } meta nfproto ipv6 drop" ;;
            unify) # 必须放过 ICMPv6，否则 ND/RA 全废（OpenWrt 官方文档明确要求）
                   echo "    oifname { $(wan_set) } meta nfproto ipv6 meta l4proto != ipv6-icmp ip6 hoplimit set $TTL" ;;
        esac
        echo "}"
    } > "$NFT_FILE"
}

nft_apply() {
    nft_write
    fw4 reload >/dev/null 2>&1 || /etc/init.d/firewall reload >/dev/null 2>&1
    if nft list chain inet fw4 "$CHAIN" >/dev/null 2>&1; then
        if [ "$TTL" != "0" ]; then
            log "已生效：出 WAN 的 IPv4 TTL = $TTL（链 inet fw4/$CHAIN）"
        fi
        case "$IPV6" in
            off)   log "已生效：IPv6 转发已阻断" ;;
            unify) log "已生效：IPv6 hop limit = $TTL（ICMPv6 除外）" ;;
            keep)  log "IPv6 未处理（默认）。注意：每台设备都有独立 IPv6 地址时会直接暴露台数" ;;
        esac
    else
        log "警告：$NFT_FILE 未出现在 inet fw4 中（该固件的 fw4 可能不支持 /etc/nftables.d）"
        log "      已回退为独立表 inet cnkrouter，需配合开机脚本（见 --status 输出）"
        nft delete table inet cnkrouter 2>/dev/null
        {
            echo "table inet cnkrouter {"
            sed -e 's/^/    /' "$NFT_FILE"
            echo "}"
        } > /tmp/cnkrouter.nft
        nft -f /tmp/cnkrouter.nft && log "已用独立表 inet cnkrouter 生效"
        rm -f /tmp/cnkrouter.nft
        # 独立表会被重启清掉，挂到 rc.local
        grep -q 'cnkrouter-harden' "$RC_LOCAL" 2>/dev/null || {
            [ -f "$RC_LOCAL" ] || printf '#!/bin/sh\n\nexit 0\n' > "$RC_LOCAL"
            sed -i '/^exit 0/i /usr/sbin/cnkrouter-harden --ttl '"$TTL"' >/dev/null 2>&1' "$RC_LOCAL" 2>/dev/null
        }
    fi
}

nft_status() {
    echo "后端：nftables (fw4)"
    nft list chain inet fw4 "$CHAIN" 2>/dev/null || nft list table inet cnkrouter 2>/dev/null || echo "（未安装规则）"
}

nft_off() {
    rm -f "$NFT_FILE"
    nft delete table inet cnkrouter 2>/dev/null
    fw4 reload >/dev/null 2>&1 || /etc/init.d/firewall reload >/dev/null 2>&1
    log "已撤销 nftables 规则"
}

# ---------------------------------------------------------------- iptables

ipt_apply() {
    modprobe xt_TTL 2>/dev/null
    iptables -t mangle -N "$CHAIN" 2>/dev/null
    iptables -t mangle -F "$CHAIN" 2>/dev/null
    if [ "$TTL" != "0" ]; then
        for d in $(wan_set | tr -d '"' | tr ',' ' '); do
            iptables -t mangle -A "$CHAIN" -o "$d" -j TTL --ttl-set "$TTL" 2>/dev/null
        done
    fi
    case "$IPV6" in
        off) ip6tables -t filter -N cnk6 2>/dev/null; ip6tables -t filter -F cnk6 2>/dev/null
             ip6tables -t filter -A cnk6 -j DROP 2>/dev/null
             ip6tables -t filter -C FORWARD -j cnk6 2>/dev/null || ip6tables -t filter -A FORWARD -j cnk6 ;;
        unify) modprobe xt_HL 2>/dev/null; ip6tables -t mangle -N cnk6h 2>/dev/null
               ip6tables -t mangle -F cnk6h 2>/dev/null
               ip6tables -t mangle -A cnk6h -p ipv6-icmp -j RETURN 2>/dev/null
               ip6tables -t mangle -A cnk6h -j HL --hl-set "$TTL" 2>/dev/null
               ip6tables -t mangle -C POSTROUTING -j cnk6h 2>/dev/null || ip6tables -t mangle -A POSTROUTING -j cnk6h ;;
    esac
    iptables -t mangle -C POSTROUTING -j "$CHAIN" 2>/dev/null || iptables -t mangle -A POSTROUTING -j "$CHAIN"
    log "已生效：iptables mangle 链 $CHAIN（TTL=$TTL）"

    grep -q 'cnkrouter-harden' "$RC_LOCAL" 2>/dev/null || {
        [ -f "$RC_LOCAL" ] || printf '#!/bin/sh\n\nexit 0\n' > "$RC_LOCAL"
        sed -i '/^exit 0/i /usr/sbin/cnkrouter-harden --ttl '"$TTL"' >/dev/null 2>&1' "$RC_LOCAL" 2>/dev/null
        log "已写入 $RC_LOCAL 开机自启"
    }
}

ipt_status() {
    echo "后端：iptables"
    iptables -t mangle -S "$CHAIN" 2>/dev/null || echo "（未安装规则）"
}

ipt_off() {
    iptables -t mangle -D POSTROUTING -j "$CHAIN" 2>/dev/null
    iptables -t mangle -F "$CHAIN" 2>/dev/null
    iptables -t mangle -X "$CHAIN" 2>/dev/null
    ip6tables -t filter -D FORWARD -j cnk6 2>/dev/null
    ip6tables -t filter -F cnk6 2>/dev/null; ip6tables -t filter -X cnk6 2>/dev/null
    ip6tables -t mangle -D POSTROUTING -j cnk6h 2>/dev/null
    ip6tables -t mangle -F cnk6h 2>/dev/null; ip6tables -t mangle -X cnk6h 2>/dev/null
    log "已撤销 iptables 规则"
}

# ---------------------------------------------------------------- TCP 时间戳

strip_timestamp() {
    # 只有内核带 xt_TCPOPTSTRIP 才做得了；nftables 没有等价能力
    if ! command -v iptables >/dev/null 2>&1; then
        log "没有 iptables（本固件可能只装了 nftables）：跳过。nftables 没有摘 TCP 选项的能力"
        return 1
    fi
    if ! modprobe xt_TCPOPTSTRIP 2>/dev/null && ! iptables -j TCPOPTSTRIP -h >/dev/null 2>&1; then
        log "内核/iptables 不含 TCPOPTSTRIP：跳过（不静默）。需要时再考虑 UA3F（注意 45.6 MB 内存）"
        return 1
    fi
    iptables -t mangle -N cnk_ts 2>/dev/null
    iptables -t mangle -F cnk_ts 2>/dev/null
    iptables -t mangle -A cnk_ts -p tcp -j TCPOPTSTRIP --strip-options timestamp 2>/dev/null || {
        log "TCPOPTSTRIP 规则添加失败（target 不在内核里）"; return 1; }
    iptables -t mangle -C POSTROUTING -j cnk_ts 2>/dev/null || iptables -t mangle -A POSTROUTING -j cnk_ts
    log "已生效：已摘除出站 TCP 时间戳"
}

# ---------------------------------------------------------------- 路由器自身

sysctl_apply() {
    if [ "$TTL" = "0" ]; then
        return 0
    fi
    mkdir -p /etc/sysctl.d
    printf 'net.ipv4.ip_default_ttl = %s\n' "$TTL" > "$SYSCTL_FILE"
    sysctl -w net.ipv4.ip_default_ttl="$TTL" >/dev/null 2>&1
    log "路由器自身出包 TTL = $TTL（$SYSCTL_FILE）"
}

sysctl_off() {
    rm -f "$SYSCTL_FILE"
    sysctl -w net.ipv4.ip_default_ttl=64 >/dev/null 2>&1
}

# ---------------------------------------------------------------- main

BE=$(backend)
[ "$BE" = none ] && { echo "既没有 nft 也没有 iptables，无法处理" >&2; exit 1; }

case "$ACTION" in
    apply)
        if [ "$BE" = nft ]; then nft_apply; else ipt_apply; fi
        sysctl_apply
        if [ "$STRIP" = 1 ]; then
            strip_timestamp || log "时间戳摘除未生效（见上），其余规则已应用"
        fi
        echo
        echo "验证方法（在局域网内任意设备上）："
        echo "  1) TTL：随便找个能看 TTL 的站点/工具，应与上面设置的值一致"
        echo "  2) 规则：$0 --status"
        ;;
    status)
        if [ "$BE" = nft ]; then nft_status; else ipt_status; fi
        echo "ip_default_ttl = $(cat /proc/sys/net/ipv4/ip_default_ttl 2>/dev/null)"
        ;;
    off)
        if [ "$BE" = nft ]; then nft_off; else ipt_off; fi
        sysctl_off
        sed -i '/cnkrouter-harden/d' "$RC_LOCAL" 2>/dev/null
        log "已全部撤销"
        ;;
esac
