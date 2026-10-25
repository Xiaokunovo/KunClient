# 彩蛋

> 维护者须知：下面这些**不是 bug**，是有意留的趣味功能。改动相关代码前先读这一段。

---

## 1. 版本号七连击

**位置**：关于页 →「版本信息」卡片 →「版本:」那一行的版本号标签

**触发**：连续点击 **7 次**，相邻两次间隔必须 **< 800ms**。任何一次超时都会从头计数。

**效果**：
- Toast：「彩蛋：版本号七连击达成 —— 这手速，9008 都追不上你」
- 版本号标签样式脉冲：Accent + 700 字重 → 160ms → Accent2 + 600 → 260ms → 清空局部 QSS 回落全局

**实现**：`ui/pages/AboutPage.cpp`
- 常量：`kVersionClickTarget = 7`、`kVersionClickWindowMs = 800`
- `eventFilter()` 拦截 `QEvent::MouseButtonPress`（不是 Release —— 见下方 §3）
- `triggerVersionEgg()` 执行效果
- 用 `QTimer` + 局部 QSS，**不用** `QPropertyAnimation`

---

## 2. Konami 码

**位置**：任意页面（MainShell 的 qApp 级 eventFilter）

**触发**：依次按 `↑` `↑` `↓` `↓` `←` `→` `←` `→` `B` `A`

**效果**：3 条 Toast，间隔 400ms：
1. 「秘技已激活」
2. 「已解锁隐藏能力：分区表倒背如流（仅限嘴上）」
3. 「刷机人的老规矩 —— 刷前三炷香，刷完别拔线」

**实现**：`ui/MainShell.cpp`
- 序列：`kKonamiSequence[]`（同时也是白名单 —— 按到表外的键就清零）
- `feedKonamiKey()` / `resetKonamiProgress()` / `triggerKonamiEgg()`
- `isTextInputWidget()` 做输入框保护
- `m_konamiToastTimer` 驱动 400ms 步进

**放过的情况**（不拦截，不影响用户体验）：
- 焦点在 `QLineEdit` / `QTextEdit` / `QPlainTextEdit`（含 `IosSearchField` 等子类）
- 带 `Ctrl` / `Alt` / `Meta` 的组合键
- `isAutoRepeat` 自动重复
- 命中前一律不 `consume`

---

## 3. 为什么这些写法这么"讲究"

几条踩过的坑，写在这儿免得后人重犯：

**① 版本号用 `MouseButtonPress` 而不是 `MouseButtonRelease`**

`QLabel` 在 `textInteractionFlags` 为 `NoTextInteraction` 时会把 press / release 都 `ignore()` 掉，事件随后沿父链上传。按下这一拍一定先送到指针下的控件（这里就是标签）并先过事件过滤器，最稳。对一个连点彩蛋来说「按下即算一次」与「松开才算」没有体验差异。

**② 版本号动画不用 `QPropertyAnimation` 改 `geometry`**

这个标签由 `QFormLayout` 摆位，改 geometry 的动画会被布局下一次重排无声吃掉，还会在中途闪一下错位。同样原因见 `ui/theme/Motion.h` 里「布局管辖的控件不能用 reveal/conceal」那条红线。只碰 `color` / `font-weight`，字形宽度变化由 `QFormLayout` 自己重排。

**③ Toast 文案里不放 emoji**

`Toast` 按 `QFontMetrics` 量宽，emoji 会走字体回退导致量不准，末尾被省略号截断。

**④ Konami 用 `qobject_cast` 而不是 `inherits()`**

`QLineEdit` 在项目里有一串子类（`IosSearchField` 等），`qobject_cast` 走元对象，子类一样能认出来。

**⑤ 序列表同时是白名单**

按到表外的任意键就清零，避免用户按了一万次 `A` 再敲 `B` 也算命中。

---

## 4. 想加新彩蛋？

遵循同样的原则：

- **不碰布局**：动画只改颜色/透明度/字重的 QSS，不要动 geometry
- **要能撤销**：彩蛋失败或被打断时不能留下坏状态
- **不打扰用户**：输入框内一律放过；不要拦截会影响正常操作的按键
- **留维护者注释**：像上面那样写清楚「这不是 bug」以及触发表
- **别用 emoji**：`Toast` 量宽会不准
- **别引入新依赖**：只用项目已用的 Qt 模块
