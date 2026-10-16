# Avalonia 设计令牌映射（DESIGN.md → Avalonia）

**来源**：`design-md` MCP 目录的 `shadcn/ui`（spec `webdesignhot/0.2`，更新于 2026-05-06），
已按其原文安装为项目根的 `DESIGN.md`（52,450 字节，含 YAML frontmatter）。

**本文件是什么**：把那份 Web 设计系统落到 **Avalonia** 上的落地桥——Avalonia 没有 CSS 变量，
主题、圆角、字号、组件状态都必须显式建资源与样式。这里只做**翻译**，不新增任何设计值。

**三条技能规矩（本文件全程遵守）**：
1. **不得自造 token**：下面每个颜色/尺寸/圆角/阴影都来自 `DESIGN.md`；它没定义的，本文件明说"未定义"。
2. **一次只用一套设计系统**：只映射 shadcn/ui，不掺别的。
3. **不得静默覆盖**：本仓库此前没有 `DESIGN.md`，安装前已确认。

---

## 1. 颜色角色（明 / 暗）→ Avalonia 资源键

暗色主题是亮色的**数学反转**（DESIGN.md §12），不是另配一套色。

| 角色 | 亮色 | 暗色 | 建议资源键 |
|---|---|---|---|
| 画布 / 背景 | `#ffffff` | `#0a0a0a` | `CnkBg` |
| 正文 / 前景 | `#0a0a0a` | `#fafafa` | `CnkText` |
| 次级文本 | `#3f3f46` | `#d4d4d8` | `CnkTextSecondary` |
| 弱化文本（说明、占位） | `#71717a` | `#a1a1aa` | `CnkTextMuted` |
| 最弱（禁用、caption） | `#a1a1aa` | `#71717a` | `CnkTextFaint` |
| 主操作（品牌） | `#0a0a0a` | `#fafafa` | `CnkBrand` |
| 主操作 hover | `#1a1a1a` | `#e4e4e7` | `CnkBrandHover` |
| 主操作 pressed | `#27272a` | `#d4d4d8` | `CnkBrandActive` |
| 主操作上的文字 | `#fafafa` | `#0a0a0a` | `CnkOnBrand` |
| 面（浅底：代码块、表头） | `#fafafa` | `#171717` | `CnkSurface` |
| 面-强（hover、次级按钮） | `#f4f4f5` | `#27272a` | `CnkSurfaceStrong` |
| 面-浮层（弹层、对话框） | `#ffffff` | `#18181b` | `CnkSurfaceElevated` |
| 边框（发丝线） | `#e4e4e7` | `#27272a` | `CnkBorder` |
| 边框-弱（行分隔） | `#f4f4f5` | `#18181b` | `CnkBorderSoft` |
| 边框-强 | `#d4d4d8` | `#3f3f46` | `CnkBorderStrong` |
| 输入框边框 | `#e4e4e7` | `#27272a` | `CnkInputBorder` |
| 聚焦环 | `#0a0a0a` | `#fafafa` | `CnkRing` |
| 危险（删除类操作） | `#ef4444` | `#7f1d1d` | `CnkDanger` |
| 危险 hover | `#dc2626` | `#991b1b` | `CnkDangerHover` |
| 成功 | `#10b981` | 同 | `CnkSuccess` |
| 警告 | `#f59e0b` | 同 | `CnkWarning` |
| 信息 | `#3b82f6` | 同 | `CnkInfo` |
| 文本选区 | `rgba(10,10,10,0.1)` | `rgba(250,250,250,0.12)` | `CnkSelection` |
| 阴影-环境 | `rgba(0,0,0,0.05)` | `rgba(0,0,0,0.30)` | `CnkShadowAmbient` |
| 阴影-标准（浮层） | `rgba(0,0,0,0.08)` | `rgba(0,0,0,0.45)` | `CnkShadowStandard` |
| 阴影-深（对话框） | `rgba(0,0,0,0.15)` | `rgba(0,0,0,0.60)` | `CnkShadowDeep` |

**不使用的 token**：`chart-1..5`（本项目无图表）、`link/link-hover`（窗口内不用超链接，
需要时取 `CnkText` + 下划线，见 DESIGN.md §2 Interactive）。

## 2. 圆角（单一 `--radius = 8px` 派生）

| 档 | 值 | 用途 | 资源键 |
|---|---|---|---|
| micro | 2px | 复选框勾选区 | `CnkRadiusMicro` |
| xs | 4px | tooltip、kbd、小内联块 | `CnkRadiusXs` |
| **sm** | **6px** | **按钮、输入框、下拉、徽标** | `CnkRadiusSm` |
| **md** | **8px** | **卡片、代码块/日志面板、浮层** | `CnkRadiusMd` |
| lg | 12px | 对话框、大面 | `CnkRadiusLg` |
| pill | 9999px | 圆形状态点 | `CnkRadiusPill` |

## 3. 字体与字号

**字族**（按 DESIGN.md 给的栈，**不要**去获取 Geist —— 技能明确禁止获取专有字体）：

- 无衬线：`"Geist", "Geist Sans", -apple-system, "system-ui", "Segoe UI", Helvetica, Arial, sans-serif`
- 等宽：`"Geist Mono", "JetBrains Mono", ui-monospace, SFMono-Regular, Menlo, Consolas, monospace`

> **中文回退（平台必需，非新增 token）**：Geist 无中日韩字形。本产品界面是中文，
> 故在栈尾追加系统中文字族：无衬线补 `"Microsoft YaHei UI", "微软雅黑"`，
> 等宽栈已含 `Consolas` 可显示中文（观感一般，日志以英文/数字为主）。
> 这是"让既有栈在中文环境可读"，不是引入第二套字体。

**字号**（只取 DESIGN.md 的 type scale）：

| 用途 | token | 值 |
|---|---|---|
| 窗口标题 | `display-h3` | 24 / 600 / -0.01em |
| 区块标题 | `title-sm` | 16 / 600 |
| 表单标签、按钮文字 | `label` | 14 / 500 |
| 正文、输入内容、说明 | `body-small` | 14 / 400 |
| 辅助说明、时间戳 | `caption` | 12 / 400 |
| 日志正文（等宽） | `code-block` | 13.6 / 400 / 行高 1.55 |
| 状态徽标（等宽） | `badge` | 12 / 500 |

## 4. 本窗口需要的组件规格（全部来自 DESIGN.md §4）

| 组件 | 关键数值 |
|---|---|
| `input-text` | 高 36，圆角 **6**，内边距 8×12，边框 `1px CnkInputBorder`；聚焦→边框变 `CnkRing` + 2px 环（2px offset） |
| `textarea` | 同边框与圆角，最小 4 行高 |
| `select`（下拉） | 与 input 同高同边框，右侧 16px chevron |
| `checkbox` / `radio` | 16×16，圆角 4（radio 为圆），选中填充 `CnkBrand` |
| `switch` | 轨道 24×40，滑块 20×20 圆；关 `CnkBorder`、开 `CnkBrand`；滑块位移 16px / 150ms |
| `button-primary` | 填充 `CnkBrand`，文字 `CnkOnBrand`，圆角 6，内边距 8×16，高 36；hover `CnkBrandHover`、pressed `CnkBrandActive` |
| `button-secondary` | 填充 `CnkSurfaceStrong`，文字 `CnkText` |
| `button-outline` | 填充 `CnkBg` + `1px CnkBorder` |
| `button-ghost` | 透明无边框，hover→`CnkSurfaceStrong`（工具栏/图标按钮） |
| `button-destructive` | 填充 `CnkDanger`，文字 `CnkOnBrand`；**只用于删除类操作** |
| `card` | 填充 `CnkBg` + `1px CnkBorder` + 圆角 **8** + 内边距 24；**无阴影**（深度靠"发丝线 + 面色阶"） |
| `tabs` | 下划线式：1px `CnkBorder` 基线，选中项 2px `CnkBrand` 下划线；标签 `label`（14/500） |
| `badge-default` | 填充 `CnkBrand`，文字 `CnkOnBrand`，等宽 12/500，内边距 2×8，圆角 6 |
| `badge-secondary` | 填充 `CnkSurfaceStrong`，文字 `CnkText` |
| `badge-outline` | `CnkBg` + `1px CnkBorder` |
| `code-block`（**日志面板**） | 填充 `CnkSurface`，`1px CnkBorder`，圆角 8，内边距 16，等宽 13.6/1.55 |
| `dialog` | 圆角 **12**，`1px CnkBorder`，阴影 `CnkShadowDeep`，背景遮罩 50% 黑，入场 200ms |
| `toast` | 卡片式：`CnkBg` + 1px 边框 + 圆角 8 + 标准阴影；4 秒自动消失，hover 暂停 |
| `tooltip` | 填充 `CnkBrand`，文字 `CnkOnBrand`，等宽 12/500，内边距 6，圆角 4 |
| `separator` | 1px `CnkBorder` |
| `kbd` | 填充 `CnkSurfaceStrong` + `1px CnkBorder`，圆角 4，等宽 12/500 |

**深度阶梯**（DESIGN.md §7）：0 平面 → 1 发丝线边框 → 2 面色阶（`CnkSurface`）→ 3 软阴影 →
4 标准阴影（浮层）→ 5 深阴影 + 遮罩（对话框）。**卡片默认在 1 层**，不要给卡片加阴影。

## 5. 产品状态 → 设计语义（只用上表 token）

| 网络状态 | 语义 | 呈现 |
|---|---|---|
| 在线 | `CnkSuccess` | 状态点 + `badge-outline` 文案 |
| 被劫持 | `CnkWarning` | 同上 |
| 离线 | `CnkTextMuted` | 同上（中性灰，不用红色——离线不是错误） |
| 认证失败（凭据错） | `CnkDanger` | 同上 + 顶部 Alert 行内提示 |
| 登录中 / 探测中 | `CnkInfo` | 徽标 + 进行中状态 |

## 6. 交互与无障碍硬要求（DESIGN.md §8/§9）

- **聚焦环**：2px 实色 `CnkRing` + 2px offset + 2px 内晕；**只在键盘聚焦时出现**
  （Avalonia 用 `FocusAdorner`，鼠标点击不显示）。
- **时长/缓动**：hover 150ms、输入聚焦 100ms、弹层 250ms、对话框 200ms；
  标准缓动 `cubic-bezier(0.4,0,0.2,1)`、强调缓动 `cubic-bezier(0.16,1,0.3,1)`
  （Avalonia 用 `KeySpline`）。
- **减少动画**：系统开启"减少动画"时，去掉位移与缩放，只保留颜色/透明度。
- **对比度**：正文/背景 19.5:1、弱化文本 4.6:1（AA）。`CnkTextFaint` 只用于禁用与
  装饰性文字，**不得承载正文**。
- **键盘**：Tab 顺序覆盖所有交互元素；对话框捕获焦点、Esc 关闭、焦点归还触发者；
  Tab 控件用方向键切换。

## 7. 与 Web 原版的差异（有意为之，不是混搭）

1. **布局不照搬**：DESIGN.md 描述的是文档站（1400px 容器、240px 侧栏、56px 顶栏）。
   桌面设置窗口的**布局**由本项目计划的任务 24 决定；本映射只固定**视觉语言**。
2. **保留其控件尺寸**：36px 高、6px 圆角在桌面上依然合适（点击目标足够）。
3. **不使用其图表色与营销版式**（hero/lead/prose-width 等）。
4. 中文回退见 §3，属于平台必需。

## 8. 交由任务 23 实现的清单

1. `Theme.Light.axaml` / `Theme.Dark.axaml`：`ResourceDictionary`，键名取 §1，
   用 Avalonia 的 `ThemeVariant` 挂载（`Application.Styles` / `ResourceInclude`）。
2. `Controls.axaml`：把 §4 做成样式——`Button` 五态、`TextBox`、`ComboBox`、`CheckBox`、
   `ToggleSwitch`、`TabControl`、`Border`(card)、徽标、日志用等宽 `SelectableTextBlock`。
3. **首屏验收**：明/暗/键盘焦点三态截图，与 §1–§4 的数值逐项核对。
4. **自动化检查**（可测的部分）：
   - 资源键齐全性单测（§1 的键必须都能在明暗两个字典里解析）
   - 切换主题后无残留旧值（同一键在暗色下解析为 §1 的暗色值）
   - 圆角/字号取自资源而非硬编码（grep 断言 XAML 里不出现裸 `#RRGGBB` 与裸 `36`）
5. **字体**：按 §3 的栈；不下载 Geist。

## 9. MCP 工具输出差异记录（一律以 DESIGN.md 为准）

| 现象 | 处理 |
|---|---|
| `export_design(format=css)` 把圆角导出成 `--radius-button: 8px`、`--radius-card: 12px`，与 frontmatter 的 `sm: 6 / md: 8 / lg: 12` 不符 | 按 **frontmatter + §6 表**（按钮 6、卡片 8、对话框 12） |
| frontmatter 的 `aliases` 块写 `ring: border-strong`，而 `colors` 里显式 `ring: '#0a0a0a'` | 按 **`colors.ring`**；`evaluate_token("ring")` 返回 `#0a0a0a` 可证 |
| `get_component(state=hover)` 返回的仍是基础值，不区分状态增量 | 状态值取调色板的 `brand-hover` / `brand-active` |
| `evaluate_token` 不接受 `theme.ring` 写法，只接受裸 token 名 | 用 `ring`、`bg`、`border` 这类裸名 |

> 顺带记：`DESIGN.md` 安装前本仓库无同名文件；`npx design-md install` 亦可安装，
> 本机改用「MCP 原文直接落盘」以避免正文二次搬运。
