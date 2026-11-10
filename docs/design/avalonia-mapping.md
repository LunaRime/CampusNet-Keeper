# Avalonia 设计令牌映射（DESIGN.md v2 → Avalonia）

**真源**：项目根 `DESIGN.md`（v2 现代桌面原生，2026-11-10）。本文件只做**翻译**，不新增设计值。
**取代**：原 shadcn/ui 映射（纯黑白、Geist、零阴影、网页 36px 控件）。

---

## 1. 资源键（明暗两份必须一一对应）

`Theme.Light.axaml` / `Theme.Dark.axaml`，由 `App.axaml` 的 `ResourceDictionary.ThemeDictionaries` 挂载。
色值**只能**出现在这两份文件里。

| 键 | 亮色 | 暗色 | 角色 |
|---|---|---|---|
| `CnkBg` | `#f3f5f9` | `#0f1218` | 工作区画布 |
| `CnkTitleBar` | `#fbfcfe` | `#151a22` | 应用头 |
| `CnkSurface` | `#ffffff` | `#1a1f28` | 卡片 / 弹层 |
| `CnkSurfaceStrong` | `#eff3f9` | `#252c39` | 悬停 / 次级按钮 |
| `CnkSurfaceSubtle` | `#f7f9fc` | `#202632` | 链路图底 / 折叠区 |
| `CnkConsole` | `#f8fafc` | `#141922` | 日志控制台 |
| `CnkSidebar` | `#f9fafc` | `#161b23` | 导航区 |
| `CnkText` | `#16202e` | `#edf2f9` | 正文 |
| `CnkTextSecondary` | `#46566b` | `#b2bfd0` | 次级文字 |
| `CnkTextMuted` | `#7b8b9f` | `#8593a6` | 弱化文字 |
| `CnkTextFaint` | `#a8b4c3` | `#5c6a7c` | 禁用 / 装饰 |
| `CnkBrand` | `#2f6bff` | `#5b8cff` | 品牌主色 |
| `CnkBrandHover` | `#1f56e0` | `#7aa2ff` | 主按钮 hover |
| `CnkBrandActive` | `#1846c2` | `#93b4ff` | 主按钮 pressed |
| `CnkBrandSoft` | `#e8efff` | `#1e2c4a` | 选中项底 / 徽标底 |
| `CnkOnBrand` | `#ffffff` | `#0f1218` | 品牌色上的文字 |
| `CnkBorder` | `#e2e8f1` | `#2a3240` | 发丝线 |
| `CnkBorderStrong` | `#cbd5e3` | `#3b4657` | 强边框 |
| `CnkInputBorder` | `#cbd5e3` | `#3b4657` | 输入控件边框 |
| `CnkRing` | `#612f6bff` | `#785b8cff` | 聚焦环（AARRGGBB） |
| `CnkSuccess` / `CnkSuccessSoft` | `#16a34a` / `#e7f6ec` | `#3ecf7a` / `#14301f` | 在线 |
| `CnkInfo` / `CnkInfoSoft` | `#2563eb` / `#e9f0ff` | `#6f9bff` / `#1b2a4a` | 进行中 |
| `CnkWarning` / `CnkWarningSoft` | `#d97706` / `#fef2df` | `#f0a942` / `#34250e` | 警告 |
| `CnkDanger` / `CnkDangerHover` / `CnkDangerSoft` | `#dc2626` / `#b91c1c` / `#fdeaea` | `#ff6b6b` / `#ff8f8f` / `#3a1a1c` | 失败 / 删除 |
| `CnkOffline` / `CnkOfflineSoft` | `#64748b` / `#eef1f5` | `#94a3b8` / `#242c38` | 离线（中性） |
| `CnkSelection` | `#1a2f6bff` | `#245b8cff` | 文本选区 |

**阴影**（`BoxShadows` 资源，非画刷）：

| 键 | 亮色 | 暗色 |
|---|---|---|
| `CnkShadowCard` | `0 1 2 0 #0A102038, 0 4 14 0 #0A102038` | `0 1 2 0 #28000000, 0 6 18 0 #28000000` |
| `CnkShadowRaised` | `0 2 6 0 #12102038, 0 12 28 0 #12102038` | `0 2 8 0 #36000000, 0 14 30 0 #36000000` |

## 2. 形状 / 尺寸 / 字阶

| 键 | 值 | 用途 |
|---|---|---|
| `CnkRadiusXs` / `Sm` / `Md` / `Lg` / `Pill` | 4 / 8 / 12 / 16 / 9999 | 小徽标 / 控件 / 卡片 / 大面 / 状态点 |
| `CnkFontSizeTitle` | 20 | 页面标题 |
| `CnkFontSizeHeading` | 14 | 卡片标题 |
| `CnkFontSizeLabel` | 12 | 表单标签 |
| `CnkFontSizeBody` | 13.5 | 正文 / 按钮 |
| `CnkFontSizeCaption` | 11.5 | 辅助说明 |
| `CnkFontSizeMono` | 11.5 | 日志 |
| `CnkControlHeight` / `CnkButtonHeight` | 34 / 36 | 输入 / 按钮 |

## 3. 字体栈（`App.axaml` 应用级资源）

```
CnkFontSans : Segoe UI Variable Text, Segoe UI, Microsoft YaHei UI, 微软雅黑, sans-serif
CnkFontMono : Cascadia Code, Cascadia Mono, Consolas, Microsoft YaHei UI, monospace
```

中文回退是平台必需；`SelfTest.cs` 会断言两栈分别含 `Microsoft YaHei UI` 与 `Consolas`。

## 4. 组件 → 样式类（`Controls.axaml`）

| 类 | 关键点 |
|---|---|
| `Button`（基础） | 显式 `HorizontalContentAlignment=Center`、高 36、圆角 8 |
| `.primary` / `.secondary` / `.outline` / `.ghost` / `.danger` | 五态，hover 走 `/template/ ContentPresenter` 覆盖 Fluent 默认 |
| `TextBox` / `ComboBox` / `NumericUpDown` | 高 34、圆角 8、聚焦改品牌色边框 |
| `CheckBox` / `ToggleSwitch` | 选中填充 `CnkBrand`，不用 Fluent 青色强调 |
| `Border.card` | 面色 + 1px 边框 + 12px 圆角 + `CnkShadowCard` |
| `Border.dot` + `.online/.busy/.failed` | 状态点，颜色随状态切令牌 |
| `TextBlock.title/.heading/.label/.caption/.error` | 字阶与前景层级 |
| `ListBoxItem` | 选中 = `CnkSurfaceStrong` + 面色阶，不引入彩色高亮 |

## 5. 状态语义 → 资源

| 守护状态 | 令牌 | 呈现位置 |
|---|---|---|
| `online` | `CnkSuccess` + `CnkSuccessSoft` | 胶囊、大卡状态点、链路节点 |
| `authenticating` | `CnkInfo` + `CnkInfoSoft` | 同上 |
| `auth_failed` / `not_campus` | `CnkDanger` / `CnkWarning` | 同上 + 底栏错误行 |
| `offline` / 守护未运行 | `CnkOffline` + `CnkOfflineSoft` | 中性灰，绝不用红 |

状态切换在视图层用 `Classes` / `IsVisible` 完成，**不新增 ViewModel 属性**。

## 6. 静态与运行时检查

| 检查 | 位置 | 断言 |
|---|---|---|
| XML 良构 | `tools/check_ui_tokens.ps1` | 所有 `.axaml/.csproj/.manifest` |
| 明暗键一致 | 同上 | 两份字典 `x:Key` 集合差集为空 |
| 禁裸色值 | 同上 | 除 `Theme.*.axaml` 外无 `#[0-9a-fA-F]{3,8}` |
| 引用可解析 | 同上 | 所有 `{DynamicResource Cnk*}` 在键池内 |
| 布局回归 | 同上 | `StatusText` / `LastError` 有 `TextTrimming`；基础 Button 样式居中 |
| 逐值断言 | `SelfTest.cs`（`--self-test`） | 每个色值 / 圆角 / 字号 / 字体栈 / 窗口图标 |
| CI | `.github/workflows/ci.yml` 的 `ui` job | 构建 + ContractCheck + 静态检查 |

> 改任何色值、圆角、字号或字体栈，必须**同时**更新 `SelfTest.cs` 的期望值，
> 否则 `--self-test` 会失败——这是刻意的，防止"看着对"的占位实现混过去。

## 7. 与 v1（shadcn/ui）的差异

| 维度 | v1 | v2 |
|---|---|---|
| 色彩 | 纯黑白，零品牌色 | 中性面 + 克制品牌蓝 + 语义色 |
| 暗色 | 亮色数学反转（纯黑 `#0a0a0a`） | 独立调校的深灰阶，品牌提亮 |
| 深度 | 卡片零阴影，仅发丝线 | 发丝线 + 两级轻阴影 |
| 圆角 | 6 / 8 / 12 | 8 / 12 / 16 |
| 控件高 | 36 | 34 / 36 |
| 字体 | Geist + 中文回退 | Segoe UI Variable + 中文回退（原生） |
| 布局 | 248px 档案栏 + 单页长滚动 | 208px 导航侧栏 + 四分区 |
| 状态 | 10px 圆点 | 状态胶囊 + 大卡 + 四段链路 + 四指标 |
