# 实测证据

本目录存放 2026-10-08 对目标校园网 Portal 的实际抓取结果，作为设计文档
`../superpowers/specs/2026-10-08-campusnet-keeper-design.md` §7.1 / §7.2 的实证依据，
以及实现阶段的测试夹具来源。

所有文件已脱敏：真实账号替换为 `202400000000`，本机 IP 替换为 `10.0.0.100`。

| 文件 | 来源 | 编码 | 内容 |
|---|---|---|---|
| `loadconfig_185_801.json` | `GET :801/eportal/portal/page/loadConfig` | UTF-8 | 服务器下发的门户配置：`visit_blacklist` 免劫持地址清单（15 项）、`en_md5` `ep_http_port` `program_index` `page_index` 等 |
| `portal_home_gbk_raw.htm` | `GET http://<portal>/` | **GBK 原始字节** | 门户首页原样字节，含登录路径 / 字段名 / 端口配置 / `carrier` 运营商列表 |
| `portal_home_utf8.htm` | 同上，转码件 | UTF-8 | 同一页面的可读版本，仅供人工阅读 |

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
3. **登录用 POST**，字段名 `DDDDD`（账号）/ `upass`（密码），端口 **801**。
4. **运营商是账号后缀制**（`@dx`/`@lt`/`@cmcc`/空），列表由服务器下发，不可硬编码。
5. **探测地址优先取自 `visit_blacklist`**，但它只在 `loadConfig` 响应里 —— 客户端必须
   容忍首页取不到的情形，回退到内置默认值（`www.msftncsi.com/ncsi.txt`，该地址本身就在
   真实的 15 项清单内，故回退路径可用）。
6. **编码按 Content-Type 动态判定**：首页声明 `charset=gbk`（meta 写 `gb2312`），
   `/eportal/` 路径声明 `charset=utf-8`。硬编码任一种都会让另一条路径乱码。

## 尚未采集的证据（下次接入校园网时应补）

- **真实的 `a=Login` 响应**（成功与错误密码各一次，含状态码与响应头）。目前任务 11 的
  成功判定完全依赖"响应体含 `Dr.COMWebLoginID_3.htm`/`3.htm`"这一**推断**，它与首页自身的
  `<!--Dr.COMWebLoginID_1.htm-->` 注释模式同构，但未经证实。若推断有误，认证会在模拟器上
  全绿、在真实网络里永远判失败。
- **`http://<portal>/` 与 `/eportal/` 的完整响应头**（本目录只保存了正文）。
