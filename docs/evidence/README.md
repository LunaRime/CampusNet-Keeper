# 实测证据

本目录存放 2026-10-08 对目标校园网 Portal 的实际抓取结果，作为设计文档
`../superpowers/specs/2026-10-08-campusnet-keeper-design.md` §7.1 / §7.2 的实证依据，
以及实现阶段的测试夹具来源。

所有文件已脱敏：真实账号替换为 `202400000000`，本机 IP 替换为 `10.0.0.100`。

| 文件 | 来源 | 内容 |
|---|---|---|
| `loadconfig_185_801.json` | `GET :801/eportal/portal/page/loadConfig` | 服务器下发的门户配置：`carrier` 运营商列表、`visit_blacklist` 免劫持地址、`en_md5` 加密开关、端口、`program_index` |
| `portal_home_gbk.htm` | `GET http://<portal>/` | GBK 编码的门户首页，含登录路径 / 字段名 / 端口配置、运营商列表 |

## 由此确定的实现要点

1. **厂商是 Dr.COM 哆点 v4.0**，不是锐捷 —— 尽管两者都用 `/eportal/` 路径。
2. **`en_md5 = 0`**，密码明文提交，无需实现任何加密。
3. **登录用 POST**，字段名 `DDDDD`（账号）/ `upass`（密码），端口 **801**。
4. **运营商是账号后缀制**（`@dx`/`@lt`/`@cmcc`/空），且列表由服务器下发，不可硬编码。
5. **探测地址必须取自 `visit_blacklist`**，否则会被 Portal 劫持导致离线/在线误判。
6. **编码按 Content-Type 动态判定**：首页 GBK、eportal 路径 UTF-8。
