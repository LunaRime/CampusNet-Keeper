#!/usr/bin/env sh
# 把「当前会话的代理」显式传给 git，并带有限重试；端口每轮变化也无需手工修改。
#
# 背景：本机 ~/.gitconfig 与本仓库 .git/config 曾写死 http.proxy=127.0.0.1:7897，
# 而 WorkBuddy 每轮会话注入的 HTTPS_PROXY 端口都不同（58112 / 49279 / 62281 ...）。
# 两处写死配置已清除，git 会跟随环境变量。注意：代理软件没开启时环境变量指向
# 未监听端口，会交替报 502 / Empty reply / HTTP2 framing / TLS EOF —— 这不是配置问题，
# 脚本也救不了，必须先把代理开起来。代理刚启动尚未就绪的短暂失败由下面最多 4 次
# 重试兜住。git push 幂等，重试安全。
#
# 用法：
#   sh scripts/git-proxy.sh push origin master:main
#   sh scripts/git-proxy.sh push origin v0.2.0
#   sh scripts/git-proxy.sh ls-remote origin HEAD
# 或（已配仓库级 alias，推荐）：
#   git pp push origin master:main
set -u

P="${HTTPS_PROXY:-${https_proxy:-${HTTP_PROXY:-${http_proxy:-}}}}"
if [ -z "$P" ]; then
    echo "git-proxy: 未找到 HTTPS_PROXY / https_proxy 环境变量；" >&2
    echo "           请在代理会话里执行，或手工设置后再试。" >&2
    exit 2
fi

max=4
i=0
while [ "$i" -lt "$max" ]; do
    i=$((i + 1))
    if git -c "http.proxy=$P" -c "https.proxy=$P" "$@"; then
        exit 0
    fi
    if [ "$i" -lt "$max" ]; then
        echo "git-proxy: 第 $i 次失败（代理可能在切换/抖动），1 秒后重试" >&2
        sleep 1
    fi
done
echo "git-proxy: 连续 $max 次失败，请确认代理会话是否正常（当前 $P）" >&2
exit 1
