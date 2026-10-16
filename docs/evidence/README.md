# 实测证据

本目录存放对目标校园网 Portal 的实际抓取结果，作为设计文档
`../superpowers/specs/2026-10-08-campusnet-keeper-design.md` §7.1 / §7.2 的实证依据，
以及实现阶段的测试夹具来源。

- **2026-10-08**：首个抓取批次（首页 / loadConfig）。
- **2026-10-16 下午**：补采劫持跳转目标页 `a79.htm`（M1 实测回修的直接证据，见下文）。
- **2026-10-16 晚**：劫持 302 原始响应头（用户实测）、真实的 `a=Login` 失败响应样本
  （首次采到），并据此**修订登录端点端口结论**（见下文「登录端点端口修订」）。

所有文件已脱敏：真实账号替换为 `202400000000`，本机 IP 替换为 `10.0.0.100`，
设备相关标识（`ss1` / `mip` / `mac1`）清零。

> **脱敏必须做「十六进制级」审计**（2026-10-16 审阅漏检教训）：本机 IP 与设备标识在页面里
> 同时存在 hex 形态（如 `0a10217d` = 192.0.2.5、`0010f371831f`、`mip` 的编码值），
> **裸 grep 点分 IP 抓不到**。新增/修改证据文件后，除字符串扫描外还要对全体做一次
> 「hex 解码后比对已知敏感值」的全量校验，再提交。

| 文件 | 来源 | 编码 | 内容 |
|---|---|---|---|
| `loadconfig_185_801.json` | `GET :801/eportal/portal/page/loadConfig` | UTF-8 | 服务器下发的门户配置：`visit_blacklist` 免劫持地址清单（15 项）、`en_md5` `ep_http_port` `program_index` `page_index` 等 |
| `portal_home_gbk_raw.htm` | `GET http://<portal>/` | **GBK 原始字节** | 首页（实为**注销页**形态，`Dr.COMWebLoginID_1.htm`，登录态下请求所得），含登录路径 / 字段名 / 端口配置 / `carrier` 运营商列表 |
| `portal_home_utf8.htm` | 同上，转码件 | UTF-8 | 同一页面的可读版本，仅供人工阅读 |
| `portal_a79_login_gbk_raw.htm` | `GET http://<portal>/a79.htm` | **GBK 原始字节** | **登录页**（"上网登录页"，`Dr.COMWebLoginID_0.htm`）——真实劫持 302 的跳转目标；`authlog*` / `carrier` 等字段与首页同源同值 |
| `portal_a79_login_utf8.htm` | 同上，转码件 | UTF-8 | 同一页面的可读版本 |
| `hijack302_headers_20261016.txt` | 注销态 `curl -D -` 抓取 | ASCII | 真实劫持 302 的**原始响应头**：`Server: NetEngine Server 1.0`（拦截器特征，非 Dr.COM 指纹）、空正文、`Location: /a79.htm` |
| `portal_login_fail_wronguser_gbk_raw.htm` | `POST :80 /eportal/?c=ACSetting&a=Login`（错误账号） | **GBK 原始字节** | 真实 `a=Login` **失败**响应样本：`信息页` 模板、`<!--Dr.COMWebLoginID_2.htm-->`、`msga='userid error1'`、`page.run(2)`；响应头 `Server: DrcomServer1.0` / `charset=gbk` / 200 |
| `portal_login_fail_wronguser_utf8.htm` | 同上，转码件 | UTF-8 | 可读版本 |
| `portal_login_fail_alreadyonline_gbk_raw.htm` | 同上（正确账号，在线态复登） | **GBK 原始字节** | 真实 `a=Login` 失败样本（另一种成因）：`msga='userid error2'`、含 `mac1`（已绑 MAC）字段；已脱敏 |
| `portal_login_fail_alreadyonline_utf8.htm` | 同上，转码件 | UTF-8 | 可读版本 |

## 两份首页文件的关系（易踩坑，务必注意）

`portal_home_gbk_raw.htm` 是**线上原始字节**（GBK），`portal_home_utf8.htm` 是它的
UTF-8 转写件。**二者内容相同、编码不同。**

- 写测试夹具、断言编码行为 → 用 **`_gbk_raw`**（只有它能真实检验 GBK 解码路径）
- 人工翻阅 → 用 `_utf8`

用错会出现"按 GBK 解码却得到乱码"的假失败。

## 各字段的真实来源（写客户端时必须照这个来）

Portal 把配置**分散在两个响应里**，不要假设它们都在同一处：

| 字段 | 真实所在 |
|---|---|
| `carrier`（运营商列表） | **首页** JS 变量 |
| `authloginpath` / `authloginport` / `authuserfield` / `authpassfield` / `authsuccess` / `authfail` / `epHTTPPort` | **首页** JS 变量 |
| `visit_blacklist`（免劫持地址清单） | **仅在 `loadConfig` 响应**；首页**没有**这个变量（已做字节级核验，0 次命中） |
| `en_md5` / `ep_http_port` / `program_index` / `page_index` | **仅 `loadConfig` 响应** |

## 由此确定的实现要点

1. **厂商是 Dr.COM 哆点 v4.0**，不是锐捷 —— 尽管两者都用 `/eportal/` 路径。
2. **`en_md5 = 0`**，密码明文提交，无需实现任何加密。
3. **登录用 POST**，字段名 `DDDDD`（账号）/ `upass`（密码）。**登录端点与门户页
   同源**（2026-10-16 晚实测修订，见下节；原「端口 801」结论作废）。
4. **运营商是账号后缀制**（`@dx`/`@lt`/`@cmcc`/空），列表由服务器下发，不可硬编码。
5. **探测地址优先取自 `visit_blacklist`**，但它只在 `loadConfig` 响应里 —— 客户端必须
   容忍首页取不到的情形，回退到内置默认值（`www.msftncsi.com/ncsi.txt`，该地址本身就在
   真实的 15 项清单内，故回退路径可用）。
6. **编码按 Content-Type 动态判定**：首页声明 `charset=gbk`（meta 写 `gb2312`），
   `/eportal/` 路径声明 `charset=utf-8`。硬编码任一种都会让另一条路径乱码。
7. **劫持指纹只在跳转目标页上**（2026-10-16 实测回修）：真实环境的劫持是
   「302 → `http://<portal>/a79.htm`」，**302 的 `Server` 是拦截器的
   `NetEngine Server 1.0`（不是 Dr.COM 指纹）、正文为空**；
   `Server: DrcomServer1.0` 与 `Dr.COMWebLoginID_*`、`carrier` 等全部存在于目标页。
   因此识别的输入必须是**跳转目标页的响应头与 HTML**（spec §7），客户端须先抓取
   目标页再识别（实现：`core/PortalPage.h` 的 `ResolvePortalPage`）。

## 登录端点端口修订（2026-10-16 晚，重要）

设计期依据页面声明的 `authloginport=801` 实现登录，实测证伪：

| 请求 | 结果 |
|---|---|
| `POST :801/eportal/?c=ACSetting&a=Login&url=drappall`（无论凭据对错） | **HTTP 200 + SPA 外壳**（`<title>EPortal</title>`，11561 字节，Webpack 空壳；不处理登录） |
| `POST :80/eportal/?c=ACSetting&a=Login&url=drappall`（老协议与门户同源） | **真正处理登录**：返回 `信息页`（`<!--Dr.COMWebLoginID_2.htm-->`、`msga='userid error1/2'`、`page.run(2)`），响应头 `Server: DrcomServer1.0`、`charset=gbk` |
| `GET :80/eportal/?c=ACSetting...` | HTTP 302（`Location` 指回门户根、`Server: DrcomServer1.5`），正文为空——该端点**只认 POST** |

- :801 是老 SPA/新式 REST API（`/eportal/portal/...`，`jsonpReturn(...)` 包裹）的端口；
  老式 c=ACSetting 协议在其上不被处理。**登录 URL 应取 `auth_url` 自身端口**
  （实现：`auth/DrcomAuth.cpp`，未显式端口时按 scheme 缺省）。
- 失败页的 `msga` 是厂商原始判定码（已采 `userid error1`＝账号错误、
  `userid error2`＝在线态复登受阻），客户端已把它附带进失败信息便于诊断。
- 成功响应（`Dr.COMWebLoginID_3.htm` / `page.run(3)`）尚未采集到真实样本——需在
  **注销态**用正确凭据触发；现有成功判定沿用 `_3.htm`/`3.htm` 标记推断（失败页模板
  已印证该命名体系为真）。

## 新式 API 现状（供后续任务参考）

- `/eportal/portal/page/loadConfig` 仍存活（包裹函数已从 `dr1003` 演进为 `jsonpReturn`）。
- 门户页面 JS（`a41.js`）与 SPA 走的是新式 API：`visitor/checkUserStateByIP`
  （JSONP；参数 `program_name/page_index/login_method/wlan_user_ip/wlan_user_mac/
  wlan_ac_ip/wlan_ac_name`；`result=1` 且 `login_result∈{1,2}` 视为在线）、
  `perceive`、`online_list`；且 `login_method != 0` 时请求体经 **`util.aes_en`
  加密为单一 `params` 字段**。本客户端 M1 未使用该通道（老式协议已足够且更易验证）。

## a79.htm 的补充事实（2026-10-16）

- 标题「上网登录页」，`<!--Dr.COMWebLoginID_0.htm-->`；与首页（注销页形态，
  `_1`）同属门户页面家族，`authlogin*` / `authuserfield` / `authpassfield` /
  `authsuccess` / `authfail` / `carrier` 字段同源同值。
- 页面按请求**动态填充**客户端相关字段（`v46ip` / `ss3` / `ss5` / `timet` 等），
  本目录的脱敏件将账号与客户端 IP 归一化、`mip`/`ss1` 清零，其余字段保持原样。
- 该页在登录态与未登录态下请求结果一致（页面内容由请求侧信息填充，与已否认证无关）。

## 尚未采集的证据（下次接入校园网时应补）

- **真实的 `a=Login` 成功响应**（注销态 + 正确凭据，含状态码与响应头）。现有成功判定
  基于「响应体含 `Dr.COMWebLoginID_3.htm`/`3.htm`」推断；失败页模板（`_2` 体系）已实证，
  成功页（`_3`）待一次注销态实登即可补齐。
- **错误密码的真实失败样本**（在线态被状态检查遮蔽：正确/错误密码都先被 `userid error2`
  拦下，见 `portal_login_fail_alreadyonline_*`）。
- **`/eportal/` 端点的完整响应头**（首页与登录响应的头已齐；`/eportal/` 老协议缺一组）。
