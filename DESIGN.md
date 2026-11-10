# CampusNet-Keeper 设计系统 v2 — 现代桌面原生

**状态**：唯一设计真源（取代原 shadcn/ui 网页设计系统）。
**日期**：2026-11-10  **适用**：Windows 端 Avalonia 设置界面。
**配套**：`docs/design/avalonia-mapping.md`（令牌 → Avalonia 资源键）、
`docs/design/ui-redesign-prototype-20261110.html`（可交互高保真原型）。

---

## 0. 定位与原则

这是一个**常驻后台的 Windows 桌面工具的设置窗口**，不是文档站、不是网页后台。
原设计系统（shadcn/ui 纯黑白极简）是 Web 文档站的视觉语言，落到这里产生四个问题：
产品无辨识度；信息层级扁平；状态表达只有一个 10px 圆点；所有功能挤在一条长滚动流里。

v2 的定位：**克制的 Windows 11 / Fluent 质感 + 状态优先的信息架构**。

1. 品牌色只做点缀（主操作、选中态、图标、状态强调），大面积永远是中性面。
2. 状态是第一公民：网络状态、链路健康、关键指标占据首屏，不是一行小字。
3. 分区导航取代长滚动：高频配置与低频诊断各归其位。
4. 桌面密度：控件比 Web 更紧，间距基准 4px，不照搬 1400px 网页容器。
5. 可访问性：色永不单独承载含义（必配图标 + 文字），键盘焦点可见。

---

## 1. 颜色角色

### 亮色

| 角色 | 值 | 用途 |
|---|---|---|
| 工作区 `CnkBg` | `#f3f5f9` | 窗口内容画布 |
| 顶栏 `CnkTitleBar` | `#fbfcfe` | 应用头（含极淡品牌色晕染） |
| 卡片 `CnkSurface` | `#ffffff` | 卡片、弹层 |
| 次级面 `CnkSurfaceSubtle` | `#f7f9fc` | 链路图底、折叠区 |
| 悬停面 `CnkSurfaceStrong` | `#eff3f9` | 悬停、次级按钮 |
| 控制台 `CnkConsole` | `#f8fafc` | 日志面板 |
| 侧栏 `CnkSidebar` | `#f9fafc` | 导航区 |
| 正文 `CnkText` | `#16202e` | 标题、正文、输入 |
| 次级 `CnkTextSecondary` | `#46566b` | 说明文字、日志正文 |
| 弱化 `CnkTextMuted` | `#7b8b9f` | 标签、时间戳、caption |
| 最弱 `CnkTextFaint` | `#a8b4c3` | 禁用态、装饰文字（不承载正文） |
| 品牌 `CnkBrand` | `#2f6bff` | 主按钮、选中态、导航指示、图标 |
| 品牌 hover / active | `#1f56e0` / `#1846c2` | |
| 品牌浅底 `CnkBrandSoft` | `#e8efff` | 选中导航项、徽标底 |
| 品牌上文字 `CnkOnBrand` | `#ffffff` | |
| 边框 `CnkBorder` | `#e2e8f1` | 发丝线、卡片描边 |
| 强边框 `CnkBorderStrong` | `#cbd5e3` | 输入控件边框 |
| 聚焦环 `CnkRing` | `#612f6bff` | 2px 环 + 3px 光晕，仅键盘聚焦 |
| 成功 / 浅底 | `#16a34a` / `#e7f6ec` | 在线、保存成功 |
| 信息 / 浅底 | `#2563eb` / `#e9f0ff` | 进行中、提示 |
| 警告 / 浅底 | `#d97706` / `#fef2df` | 非校园网、需注意 |
| 危险 / hover / 浅底 | `#dc2626` / `#b91c1c` / `#fdeaea` | 认证失败、删除 |
| 离线 / 浅底 | `#64748b` / `#eef1f5` | 离线、守护未运行 |

### 暗色

不是亮色的数学反转，也不是纯黑：画布 `#0f1218`、卡片 `#1a1f28`、正文 `#edf2f9`、
品牌提亮到 `#5b8cff`（保证暗底对比度）、语义色同步提亮。
所有键与亮色**一一对应**，不得出现只在一侧存在的键（静态检查会断言）。

### 状态语义（六态）

| 守护状态 | 语义色 | 呈现 |
|---|---|---|
| `online` | 成功绿 | 状态胶囊 + 大卡 + 四段链路全绿 |
| `authenticating` | 信息蓝 | 呼吸脉冲 + 链路进行中节点 |
| `auth_failed` | 危险红 | 失败胶囊 + 门户节点标红 + 底部错误行 |
| `not_campus` | 警告琥珀 | 「当前网络无需认证」 |
| `offline` | 中性灰 | **不用红色**：离线不是错误 |
| 守护未运行 | 中性灰 | 全链路置灰 + 明确禁用提示 |

> 「终端 IP 已在线 / inuse」判成功；会话存在 ≠ 转发面可用，仍须出网验证。
> 界面呈现不得绕过这一层，否则僵尸会话永远查不出来。

---

## 2. 形状、深度、密度

| 项 | 值 | 说明 |
|---|---|---|
| 控件圆角 | 8px | 输入、按钮、下拉、徽标 |
| 卡片圆角 | 12px | 大面 16px，状态点 pill |
| 控件高 | 34 / 36px | 输入 34、按钮 36，桌面密度 |
| 卡片层次 | 1px `CnkBorder` + `CnkShadowCard` | 两级轻阴影，平面卡片不再"贴"在画布上 |
| 浮层阴影 | `CnkShadowRaised` | 对话框、Toast |
| 间距基准 | 4px | 卡片内 18、卡片间 16、控件间隔 8 |
| 动效 | 150ms `cubic-bezier(.2,.7,.3,1)` | hover / 切换；尊重系统「减少动画」 |
| 对比度 | 正文 ≥ 14:1 | `CnkTextFaint` 只用于禁用与装饰 |

---

## 3. 字体与字阶

```
Sans : "Segoe UI Variable Text", "Segoe UI", "Microsoft YaHei UI", "微软雅黑", sans-serif
Mono : "Cascadia Code", "Cascadia Mono", Consolas, "Microsoft YaHei UI", monospace
```

中文回退是**平台必需**（产品界面全中文），不是引入第二套字体。不下载任何专有字体。

| 用途 | 键 | 值 |
|---|---|---|
| 页面标题 | `CnkFontSizeTitle` | 20 / 650 |
| 卡片标题 | `CnkFontSizeHeading` | 14 / 650 |
| 表单标签 | `CnkFontSizeLabel` | 12 / 600 |
| 正文 / 按钮 | `CnkFontSizeBody` | 13.5 / 400 |
| 辅助说明 | `CnkFontSizeCaption` | 11.5 / 400 |
| 日志等宽 | `CnkFontSizeMono` | 11.5 / 400 / 行高 1.6 |

---

## 4. 组件规格

| 组件 | 规格 |
|---|---|
| 输入框 | 高 34、圆角 8、1px `CnkInputBorder`、内边距 8×11；聚焦 → 品牌色边框 + 3px 光晕 |
| 下拉 / 数字输入 | 同输入框；右侧 chevron 14px |
| 主按钮 | 填充 `CnkBrand`、文字 `CnkOnBrand`、圆角 8、高 36、内边距 8×14 |
| 次级 / 描边 / 幽灵 | `CnkSurfaceStrong` / 白底 + 发丝线 / 透明 hover 面色阶 |
| 危险按钮 | `CnkDanger`，只用于删除类操作 |
| 开关 | 40×22 轨道、16px 滑块、位移 18px / 150ms |
| 卡片 | `CnkSurface` + 1px `CnkBorder` + 12px 圆角 + 轻阴影 + 18px 内边距 |
| 状态胶囊 | pill、`CnkXxxSoft` 底 + 语义色文字 + 状态点 |
| 导航项 | 高 36、圆角 8、选中 `CnkBrandSoft` + 左侧 3px 品牌指示条 |
| 徽标 | pill、等宽 11/600、内边距 2×8 |
| 日志控制台 | `CnkConsole` 底、等宽、级别着色、可选中复制 |
| Toast | 卡片式、左侧 3px 语义色条、4 秒自动消失 |

---

## 5. 信息架构

单窗口 1000×700（最小 880×600），三段 Shell：

```
┌ 应用头：品牌标 + 名称 + 版本 + 面包屑 | 全局状态胶囊 + 主题切换 ┐ 48
├ 侧栏 208px ───────────┬ 内容区（仅此区滚动）───────────────┤
│ 工作台                 │                                      │
│  概览 / 档案 /         │  分区内容                             │
│  高级 / 日志           │                                      │
│ 配置档案（快捷列表）    │                                      │
│ 守护进程状态           │                                      │
├ 底栏：提示/错误（可截断）        [刷新] [测试登录] [保存] ────┤ 64
└──────────────────────────────────────────────────────────────┘
```

| 分区 | 承载 | 解决什么 |
|---|---|---|
| 概览 | 状态大卡 + 四段链路图 + 四项指标 + 最近事件 + 当前档案/探测参数摘要 | 原来只有一个绿点，运维信息全靠翻日志 |
| 配置档案 | 档案 chips、连接信息、认证策略、密码三态提示 | 高频配置独立成页，不再和标准设置混排 |
| 高级设置 | 探测参数、常规开关、密码迁移 | 低频项移出首屏 |
| 运行日志 | 等宽控制台、级别着色、打开日志目录 | 原来要滚到页面最底 |

**导航是纯视图层状态**：不新增 ViewModel 字段，不改变 IPC 契约与数据语义。

---

## 6. 无障碍与交互

- 聚焦环只在键盘聚焦出现（Avalonia `FocusAdorner` / `:focus-visible`），鼠标点击不显示。
- Tab 顺序覆盖所有交互元素；按钮文字必须显式居中（ContentControl 默认左对齐）。
- 长文案（状态、错误）放星号列并 `TextTrimming=CharacterEllipsis`，绝不把右侧按钮顶出窗口。
- 系统开启「减少动画」时去掉位移与缩放，只保留颜色/透明度。
- 状态变化必须同时改变：色 + 图标 + 文字，不能只换一个圆点颜色。

---

## 7. 落地约束

1. 色值**只能**出现在 `Theme.Light.axaml` / `Theme.Dark.axaml`；其它 XAML 一律 `{DynamicResource Cnk*}`。
2. 控件样式集中在 `Controls.axaml`；视图文件只写结构与绑定，不内联样式、不写裸数字尺寸。
3. 明暗两字典键集合必须完全一致（`tools/check_ui_tokens.ps1` 断言）。
4. 令牌逐值断言在 `src/ui/CampusNet.UI/SelfTest.cs`（`--self-test`），改色必须同步期望值。
5. 不碰 IPC 协议、`MainViewModel` / `ProfileViewModel` 的属性与命令、C++ 守护的密码存储三态。
6. 所有界面控件必须对应真实的 ViewModel 属性；禁止画出不存在的功能。
7. 示例/截图数据一律脱敏：IPv4 只用文档保留段，账号打码，永不出现真实 params / rcn / 密码。

---

## 8. 验收

- `pwsh -File tools/check_ui_tokens.ps1` 退出码 0。
- `dotnet build src/ui/CampusNet.UI -c Release` 无错误。
- `CampusNet.UI.exe --self-test` 输出 `PASS`（明暗令牌、圆角、字号、字体回退、窗口图标）。
- 明 / 暗 / 键盘焦点三态截图与 §1–§4 逐项核对。
- 构建后同步 `build\Release`，断言新 exe 含本次新增标记；`cnk_tests.exe` 全量通过。
