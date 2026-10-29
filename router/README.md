# cnkrouter —— 校园网认证客户端（路由器端）

把 CampusNet-Keeper 里**已经被真机验证过的那套协议逻辑**改写成一个能在路由器上跑的
小客户端：路由器自己完成门户认证，家里所有设备通过 NAT 共享这一个会话。

> 本目录是**独立子工程**，不改动仓库根目录里已完成的 Windows 程序。

---

## 一、路由器内存要多大（结论）

**128 MB 内存 / 16 MB Flash 是推荐配置；64 MB 能跑但紧。**

| 方案 | 客户端常驻内存 | 建议整机 RAM | 说明 |
|---|---|---|---|
| **本 C 客户端（推荐）** | **2 – 4 MB** | **128 MB** | 无 Python / 无 OpenSSL / 无 curl 依赖；musl 静态约 40–80 KB（估算，Windows 构建实测 240 KB） |
| shell 版（curl + openssl-util） | 每次调用 4 – 6 MB（瞬时） | 128 MB | 省事但 Flash 多 ~1 MB，且每次认证 fork 两个进程 |
| Python 版 | 12 – 20 MB | 256 MB | 解释器本身就吃掉小路由器的余量 |
| 原 Windows 程序 | — | 不适用 | Win32 + .NET(Avalonia)，架构上不可能跑在路由器上 |

整机内存去向（OpenWrt 23.05 实测口径）：

| 组成 | 典型占用 |
|---|---|
| 内核 + 基础系统（无 LuCI） | 22 – 30 MB |
| dnsmasq + firewall + hostapd（WiFi） | 6 – 10 MB |
| cnkrouter 客户端 | 2 – 4 MB |
| NAT 连接跟踪（8 台设备，500–2000 条） | 0.5 – 2 MB |
| LuCI 页面（只在打开时） | 4 – 8 MB |

因此：**64 MB = 能跑（别装 LuCI 或只用精简固件）；128 MB = 舒服（够跑基础伪装）；
256 MB = 想上 UA3F 就往这走**（UA3F 单项常驻 45.6 MB，见第五节）。

> 内存数字的来路：客户端全部用固定缓冲区，循环里不做堆分配，静态数据只有几张
> AES 表（约 1 KB）；Linux/musl 下 RSS 通常 2–4 MB（大头是 libc 的共享页）。
> 本机 Windows 构建实测工作集 10 MB，其中大部分是 ws2_32/dnsapi 等系统 DLL 的共享页，
> 不代表路由器上的占用。若你有更精确的 `busybox top` 数据，以实测为准。
Flash 才是硬门槛：**8 MB 的机器装不下 LuCI + 额外包，务必选 ≥16 MB 的型号。**

CPU 不用考虑：每 60 秒 3 个 HTTP GET + 一次 320 字节 AES，MIPS 580 MHz 上耗时 < 5 ms。

### 挑机器只看三行

1. **OpenWrt 官方支持**（[Table of Hardware](https://openwrt.org/toh/start) 能查到型号）；
   或能刷 Padavan/ImmortalWrt。
2. **Flash ≥ 16 MB**（8 MB 的机器连 LuCI 都装不下，别买）。
3. **RAM ≥ 128 MB**（64 MB 能跑，但一次 `opkg install` 就可能 OOM）。

常见坑：运营商定制机/百元"1200M"多是 8 MB + 64 MB，刷完 OpenWrt 装不了几个包。

> ⚠ 内存大小和"能不能多设备"**没有关系**。限制是账号的**在线会话数**，不是路由器资源。
> 路由器要做的只有两件事：① 自己登录一次（占住那唯一会话）；② NAT 让所有设备共用这个出口。

---

## 二、为什么这样能多设备共享

校园网"限一台设备"通常是**按账号会话**计数：谁登录，AC 就记住那个 IP/MAC，之后只放行它。
路由器做网关时：

```
手机 ─┐
电脑 ─┼─ WiFi ─→ 路由器(NAT) ─→ 校园网 ─→ 只看到 1 个 IP / 1 个 MAC
平板 ─┘                 ↑
                  本客户端在这里登录
```

只要**登录是路由器自己发起的**，AC 记录的就是路由器的 WAN 口 IP/MAC，所有终端共享它。
这也是为什么必须把认证逻辑搬进路由器——Windows 程序登录占住的是电脑那台设备。

登录报文里 `wlan_user_ip` 取自 `chkstatus`（即 AC 看到的源地址），路由器上就是 WAN IP，
与原实现完全一致，不需要额外改动。

---

## 三、从 Windows 版搬过来的东西（以及为什么不能直接编译）

原程序是 Win32（Winsock / 命名管道 / 托盘 / 注册表）+ Avalonia 界面，**一行都不能直接
编译到 MIPS/ARM 上**。真正可移植的是协议与策略，本目录是它们的精简重写：

| Windows 版 | 路由器版 | 搬过来的关键点 |
|---|---|---|
| `auth/DrcomAuth.cpp` | `src/portal.c` | `chkstatus` → `loadConfig(rcn)` → `login(AES-128-ECB params)`；老式 `c=ACSetting` 回退 |
| `core/Crypto.cpp` | `src/crypto.c` | AES-128-ECB + PKCS7 + Base64，**516 组随机向量与 BCrypt 实现逐字节相同** |
| `core/Probe.cpp` | `src/portal.c` | 204 / 3xx 异主机 / 200 正文标识；`msftncsi` 在本校园网不可达，默认用 apple 探测页 |
| `core/Scheduler.cpp` | `src/main.c` | 离线 3 秒快重探、失败退避 15/30/60/120/300、凭据错误停机、连续 3 次失败判非校园网 |
| `main.cpp` 登录后复测 | `wait_online()` | 登录成功后再轮询 5×1.5 秒——AC 放行有秒级延迟，探一次会假失败 |

两个容易踩的语义坑也一并搬了：

1. **「终端IP已在线」要按成功处理**。真实响应是 `result:0` + `ret_code:2` +
   `msg:"终端IP已在线！"`；当失败处理会让守护进程以为密码错而永久停摆。
2. **老式通道的 `userid error2` 不能当凭据错误**。本部署的老式通道无论密码对错都回 error2，
   只有确认新式接口不存在时才采信它。

---

## 四、部署（OpenWrt）

### 1. 编译

```sh
# 用 OpenWrt SDK（版本要和路由器固件一致）
./scripts/build_openwrt.sh /path/to/openwrt-sdk
# 产物：$SDK/bin/packages/<arch>/base/cnkrouter_0.1.0-1_*.ipk
```

没有 SDK 也可以直接交叉编译：

```sh
make CROSS=mipsel-openwrt-linux-musl- STATIC=1
# 或 aarch64-openwrt-linux-musl- / arm-openwrt-linux-musl-
```

### 2. 安装

```sh
# A. ipk
opkg install /tmp/cnkrouter_0.1.0-1_mipsel_24kc.ipk

# B. 或手工推（见 scripts/install_openwrt.sh）
```

### 3. 配置

```sh
vi /etc/config/cnkrouter     # 填 username / password / auth_url
chmod 600 /etc/config/cnkrouter
/etc/init.d/cnkrouter enable
/etc/init.d/cnkrouter start
logread -e cnkrouter -f
```

`auth_url` 是浏览器被劫持后跳到的那个地址（示例 `http://10.0.0.1/`，填你实际看到的那个；
仓库里一律用这个占位地址，真实门户地址不入库）。
`isp` 决定账号后缀：`campus`（不带）、`cmcc`（@cmcc）、`telecom`（@dx）、`unicom`（@lt）。

### 4. 验证

```sh
cnkrouter --config=/var/etc/cnkrouter.conf --probe   # 0=在线 2=门户 3=离线
cnkrouter --config=/var/etc/cnkrouter.conf --once    # 完整走一遍并复测
cnkrouter --selftest                                  # 不联网，验 AES/Base64/URL/JSON
```

---

### 非 OpenWrt 固件

本客户端是纯 libc 的静态二进制，Padavan（老毛子）、ImmortalWrt、普通 Linux 都能跑：

```sh
# 例：Padavan 把二进制放 /etc/storage（重启保留），在「自定义脚本」里加
/etc/storage/cnkrouter --daemon --config=/etc/storage/cnkrouter.conf &
```

---

## 五、真机实录（2026-10-29，京东云亚瑟 AX1800 Pro）

固件 **Kwrt 24.10.4**（OpenWrt 系）、`aarch64`、target `qualcommax/ipq60xx`、
RAM 414 MB、overlay 1.9 G。WAN 为 DHCP（校园网下发 `10.16.x.x/17`），LAN `192.168.100.1`。

### 交叉编译（本机无 gcc，走 CI）

`.github/workflows/ci.yml` 的 `router` job 会自动产出 **aarch64-musl 纯静态**二进制
（`733 KB`，`readelf -d` 无 NEEDED）。工具链来自 **OpenWrt 官方 SDK**：

```sh
# musl.cc 从 GitHub runner 连不上（curl 28 超时），zig 的 Releases 只有源码包，
# 所以用官方 SDK 里的 aarch64-musl 工具链（与路由器 libc 同源）
URL=https://downloads.openwrt.org/releases/24.10.4/targets/qualcommax/ipq807x/openwrt-sdk-24.10.4-qualcommax-ipq807x_gcc-13.3.0_musl.Linux-x86_64.tar.zst
curl -fsSL -o /tmp/sdk.tar.zst "$URL" && sudo tar --zstd -xf /tmp/sdk.tar.zst -C /opt/sdk --strip-components=1
GCC=$(ls /opt/sdk/staging_dir/toolchain-*/bin/aarch64-*-gcc | head -1)
make OUT=build-aarch64 CC="$GCC" STATIC=1
```

### 安装（手工推，路由器无 sftp-server）

```sh
# 二进制与脚本（脚本必须是 LF 行尾！见下）
scp 不适用时用 exec + base64 分块：见仓库 build/.rput.py（本地工具，不入库）
/usr/bin/cnkrouter            755
/etc/init.d/cnkrouter         755
/etc/hotplug.d/iface/90-cnkrouter 755
/etc/config/cnkrouter         600   ← 含明文密码，必须 600
/etc/init.d/cnkrouter enable && /etc/init.d/cnkrouter start
```

### 实测结果

| 项 | 结果 |
|---|---|
| `--selftest`（不联网） | AES FIPS-197 / Base64 / URL 全过 |
| `--probe` | `online 正文匹配预期标识`，退出码 0 |
| 常驻内存 | **VSZ 284 KB**（ps 的 VSZ 列；RSS 更低） |
| 启动到稳定 | 秒级；日志只有两行（启动参数 + 网络在线） |
| 在线时行为 | **只巡检、不登录**（60 s 一次），对现有会话零干扰 |
| 断网恢复 | 由 `hotplug` 在 `wan ifup` 后 restart，恢复时间从"最多 5 分钟"压到秒级 |

### 三个真机才暴露的坑

1. **CRLF 会让 init 脚本彻底失效**：`/etc/init.d/cnkrouter` 若带 `\r`，报
   `can't open '/etc/rc.common'`（路径末尾多了 `\r`）和 `line 7: \: not found`。
   仓库已加 `.gitattributes` 钉死 `router/**` 为 LF、`*.bat` 为 CRLF。
2. **`captive.apple.com` 在本网被 DNS 劫持**（解析到 `*.download.ks-cdn.com`），
   它的 200 + `Success` 不能单独当"已认证"的证据；验证真出网要用 TLS
   （如 `https://api.github.com/zen` 返回真实内容）。
3. **交接顺序**：如果电脑上还跑着 Windows 版守护进程，它经 NAT 出去的源 IP 就是路由器
   WAN IP —— 它才是"当前认证者"。正确顺序是**先启路由器客户端（在线时不登录）→ 观察
   → 再停电脑端**，两头不会抢会话。

只需要保证开机时 WAN 已就绪，或加一个 `sleep 10` 之类的等待。

## 六、防检测（可选，按需升级）

> 先摆正位置：本客户端 + NAT 已经解决了最主要的检测手段——**账号在线会话数**。
> AC 那边只有一个 IP / 一个 MAC。下面这些是"别让它通过指纹算出后面有几台设备"的加固，
> 属于锦上添花，**不是必需**。加得越多越容易被别的东西绊倒，够用就停。

### 第 0 层：IPv6（最容易漏，也最致命）

很多学校同时下发 IPv6，而 IPv6 **不做 NAT**：每台手机/电脑拿到的都是自己的全球地址。
AC 的流量表里一眼就是 N 个地址在同时跑——TTL 伪装得再好也没用。

```sh
./scripts/harden.sh --ipv6 off      # 阻断 IPv6 转发（最彻底，代价是家里没有 v6）
./scripts/harden.sh --ipv6 unify    # 保留 v6，只统一 hop limit（挡不住"数地址"）
```

若学校是 IPv6 单栈，`off` 会直接断网——先确认你能只用 v4 上网。

### 第 1 层：TTL 统一（基础，必做）

```sh
scp scripts/harden.sh root@路由器:/usr/sbin/cnkrouter-harden
ssh root@路由器 'chmod +x /usr/sbin/cnkrouter-harden && /usr/sbin/cnkrouter-harden --ttl 128'
```

这个脚本比常见教程多做一件事，而那正是最容易露馅的地方：

1. `postrouting` 把**出 WAN 的包**TTL 统一成 128（转发流量 + 路由器自身流量一起管）；
2. `net.ipv4.ip_default_ttl=128` —— **路由器自己发的包**（门户登录、DNS、NTP）也是 128。

只做 1 不做 2，AC 会看到同一个 IP 同时出现两种 TTL（转发 127、自产 64）。**一个 IP 出现两种
TTL，比不做伪装更像是"后面挂了设备"**。

选值原则：家里哪种系统多用就选哪个（Windows = 128，Android / iOS / Linux = 64）。
关键是**全屋一致**，具体数值次要。

- OpenWrt 22.03+（fw4/nftables）走 `/etc/nftables.d/10-cnkrouter.nft`，`fw4 reload` 即生效；
- 旧固件（LEDE/Lean，iptables）走 `mangle` 链 + `/etc/rc.local` 开机自启；
- 两者都做了**生效校验**，没装上会明说，不会假装成功。

验证：`/usr/sbin/cnkrouter-harden --status`，再从手机访问任意显示 TTL 的站点对照。

### 第 2 层：UA 统一（UA2F）

Dr.COM 的多终端判定常看 HTTP 请求里的 UA（`Windows NT 10.0` / `Android 14` / `iPhone`
混着来，一目了然）。UA2F 是 C 写的 netfilter 队列程序，把 HTTP 的 UA 就地改写成固定值
（长度不变，用空格补齐/截断）：

```sh
opkg update && opkg install ua2f luci-app-ua2f
uci set ua2f.enabled.enabled=1
uci set ua2f.main.custom_ua='Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36'
uci commit ua2f && service ua2f enable && service ua2f restart
logread | grep UA2F
```

**常驻内存 2.5 MB**（官方 benchmark，REDIRECT 模式）。128 MB 的机器毫无压力。

一个常见误解要澄清：UA2F **只对明文 HTTP 有效**。HTTPS 的 UA 在 TLS 里，AC 根本看不到——
所以"HTTPS UA 检测"这个说法本身不成立。AC 在 HTTPS 上能看到的是 **TLS ClientHello 指纹
（JA3/JA4）和 SNI**，那不是 UA，UA2F 也改不了。

### 第 3 层：TCP 时间戳（进阶，先试便宜的）

不同系统的 TCP 时间戳行为（频率/偏移）能被 p0f 一类工具识别。两条路，代价差 20 倍：

| 做法 | 常驻内存 | 说明 |
|---|---|---|
| `TCPOPTSTRIP`（本脚本 `--strip-timestamp`） | **0**（纯内核规则） | 需装 `iptables` 且内核带 `xt_TCPOPTSTRIP`；没有会明确报错，不静默 |
| UA3F 的 L3 重写 | **45.6 MB** | Go 写的完整重写代理，L3 可改 TTL / IPID / 时间戳 / TCP 窗口，另有 desync 抗 DPI |

官方 benchmark（1 KiB 响应、并发 128）：UA2F **2.5 MB** vs UA3F **45.6 MB**。
在 128 MB 的机器上 UA3F 要吃掉近四成内存，**要上 UA3F 建议整机 256 MB**——
这也是前面内存表里"128 MB 够用"和"想折腾就 256 MB"的分界线。

UA3F 能改 HTTPS，靠的是**自己做 MITM**：生成一张 CA，**每台设备都要装这张证书**，
否则一堆站点报证书错误。这是它"能改 HTTPS"的真实代价，不是装个插件就完事。

### 什么时候才该升级

只上第 0 + 1 层，跑 3–5 天，看这些信号再决定：

| 现象 | 含义 | 下一步 |
|---|---|---|
| `logread -e cnkrouter` 频繁"检测到门户劫持" | 会话被 AC 主动踢 | 加 UA2F |
| 能上网但速度被压得很低 | 限速而非踢线 | 很可能命中共享判定，加 UA2F |
| 每隔固定时间必须重新认证 | AC 有定时巡检 | 同上 |
| 一切正常 | — | **别加东西**，越少越稳 |

### 必须承认的边界

以上都是"对抗检测"，不是"消除共享行为"。如果学校做**应用层并发特征分析**（同账号在不同
设备上的登录/心跳时序、DNS 查询模式、连接数与终端数不符），路由器层面的伪装挡不住；
那时唯一有效的手段是把所有流量塞进一条加密隧道（VPN / 代理），让 AC 只看到一条连接——
但校园网通常也会限制或审计这类隧道，延迟也会变差。到这一步就不是"配置问题"，
而是"要不要继续"的问题了。

**合规提醒**：多数学校的"限一台设备"是计费/授权条款，用 NAT 共享属于绕过，被检测到通常
是强制下线、限速或约谈，极端情况停用账号。学校若提供"多设备套餐"，走正规途径最省心。

**别开二级 NAT**（路由器接路由器、WiFi 中继）：多一层地址转换只会多一个破绽。

---

## 七、已知限制

| 限制 | 说明 |
|---|---|
| 只实现 Dr.COM | 锐捷 / 深澜 / 通用表单在 Windows 版里有，本目录未移植 |
| 只支持 `http://` 门户 | 本实现不带 TLS；https 门户需另加 mbedTLS（Flash +~200 KB，RAM +~1 MB） |
| 无 UDP 心跳 | 本部署实测不需要（会话由 AC 按活跃度维持，每 60 秒的探测就是保活）；若某部署要求 20 秒 UDP 心跳，需另行移植 |
| 验证码 / 二次验证 | 无法自动通过 |
| 门户改版 | 端点或字段变了就要跟着改（改 `src/portal.c`） |

---

## 八、目录

```
router/
├── src/                 客户端源码（4 个 .c，无第三方依赖）
│   ├── crypto.c         AES-128-ECB / PKCS7 / Base64 / URL 编码 / JSON 转义
│   ├── net.c            精简 HTTP 客户端（socket + select，支持 Content-Length 与 chunked）
│   ├── portal.c         探测判定 + Dr.COM 认证（新式 API + 老式回退）
│   └── main.c           配置解析 / CLI / 守护状态机
├── tests/
│   ├── aes_crosscheck.cpp  与主项目 BCrypt 实现逐字节比对（Windows 上跑）
│   └── e2e_test.py         本地模拟门户，**真的解密 params 并逐字段断言**
├── packaging/openwrt/   ipk 打包（procd 服务 + UCI 配置 + hotplug）
├── scripts/
│   ├── build_win.bat    Windows 上构建（开发验证用）
│   ├── build_openwrt.sh SDK 交叉编译
│   ├── install_openwrt.sh 推到路由器
│   └── harden.sh        TTL 统一 / IPv6 / TCP 时间戳（防检测，见第五节）
├── CMakeLists.txt       Windows 开发构建
└── Makefile             Linux / musl 构建
```

### 自测

```sh
make test            # --selftest + e2e（e2e 需要 python3，不联网）
```

Windows 上：

```bat
scripts\build_win.bat
build\cnkrouter.exe --selftest
python tests\e2e_test.py build\cnkrouter.exe
build\aes_crosscheck.exe
```

`e2e_test.py` 里的 AES 解密是**纯 Python 独立实现**，启动时先跑 FIPS-197 向量自校验；
它会把客户端发来的 `params` 解密并逐字段断言 `user_account`（含 `,0,` 前缀与 `@cmcc` 后缀）、
`wlan_user_ip`、`rcn`、`login_method` —— 不是"客户端自己说自己成功"。
