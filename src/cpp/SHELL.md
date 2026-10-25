# KunClient C++ 壳层设计（独立设计版）

> 2026-10-11 · 二稿。应用户要求**不再对应 C# 逐件移植**，按 Qt6 自身的习惯从需求重新推。
> 现状数字仍是实测（`wc -l` / `grep`，见 §1）；与一稿的取舍差异见 §0。

---

## 0. 设计立场：三处刻意**不**照搬 .NET

| .NET 形状 | 为什么不搬 | 独立设计替代 |
|---|---|---|
| 静态单例 + `BeginTask()/EndTask()` 手动配对 | 配对式 API 有「忘了 EndTask」这类 bug 面，状态机靠纪律维持 | **RAII 任务对象 `Activity`**：构造即开始、析构即收尾，忘不掉（§2） |
| 两条镜像进度条（总/分区）+ 6 个固定分段 | 镜像条是「总进度」概念缺位时的补丁；固定分段在空闲时一半是 `-` | **单条状态带 + 阶段化进度**：任务自带阶段表（带权重），一条进度条表达全部层级（§3） |
| 速度平滑散在各页面里自己算 | 同一段速率代码在 3+ 个页面各写一遍，口径不一 | 速率/耗时由**任务板统一采样计算**，页面只报字节数（§2.2） |

保留的只有平台必需件（USB 热插拔、单实例、崩溃兜底、版本注入——这些与 .NET 无关，是壳本身该有的）。

---

## 1. 现状（实测，2026-10-11）

- `ui/MainShell.{h,cpp}`：90 + 492 行。无边框窗、双态侧栏（200/72px 持久化）、16 页注册、
  28px 状态栏 **4 段纯文字**（设备/模式/任务/速度），`onPageStatus` 直收页面信号。
- 页面 17 对 / 7753 行；实测仅 `VisualFlash` 1 页是 PlaceholderPage。
- `BasePage`（142 行）：通知条、LogView、`runOnUi` 跨线程分发、
  `shellStatusChanged(device,mode,hint)` 三参信号**直连** MainShell。
- 标题栏版本**硬编码** `"v2.0.0"`，未读 `kc/version.h`（五落点红线被绕过）。
- 样式 token：`shellStatusBar` / `statusLabel` / `statusValue`（`dark.qss:232`、`light.qss:161`）。
- 工具链：MSYS2 MinGW64 + **Qt 6.5+**（`find_package(Qt6 6.5 REQUIRED COMPONENTS Core Widgets SerialPort Xml Network)`）。

**结论**：壳的骨架够用，缺的是「任务与进度的表达层」。缺什么由下面的模型定义，不再按 .NET 清单对表。

---

## 2. 核心抽象：任务板 + RAII 任务

### 2.1 `kc::ui::Activity` —— 一次长任务的 RAII 对象

```cpp
namespace kc::ui {

class Activity : public QObject {
    Q_OBJECT
public:
    enum Flag { None = 0, Exclusive = 1 };   // Exclusive：任务期锁侧栏/危险按钮

    // —— 阶段模型：任务开始时声明各阶段及权重，进度条按权重合成 ——
    void setStages(std::vector<Stage> stages);       // Stage{ name, weight }
    void beginStage(const QString& name);            // 切阶段（未声明的动态追加，权重=1）
    void advanceStage(quint64 units);                // 当前阶段推进 N 个单位
    void setStageTotal(quint64 total);               // 单位自定义：扇区/字节/文件数都行

    // —— 便签 ——
    void setNote(const QString& note);               // 如 "super_4.img 1.2/4.4 GiB"

    // 全部可在**任意线程**调用：内部 QMetaObject::invokeMethod 排队到 UI 线程。
signals:
    void changed();                                  // 任何字段变化（任务板据此刷新）
};
} // namespace
```

用法（页面侧，`BasePage` 提供入口）：

```cpp
void EdlFlashPage::onFlashClicked() {
    auto act = beginActivity(tr("写入分区"), Activity::Exclusive);   // unique_ptr
    act->setStages({{tr("读取分区表"),1},{tr("写入"),10},{tr("校验"),2}});
    RunWorker([act = act.get()] {           // 工作线程只持裸指针；页面存活期覆盖任务期
        act->beginStage(tr("写入"));
        for (...) { ...; act->advanceStage(bytes); act->setNote(...); }
    });
}   // ← 作用域结束析构：进度定格、状态带回闲、侧栏解锁，无“收尾调用”可忘
```

**要点**
- 析构即收尾：RAII 从语言层面消灭「忘 EndTask」；异常路径同样安全。
- 生命周期契约：**页面持有 `unique_ptr`**，Activity 是页面子对象；工作线程只拿裸指针且不得越过页面存活期
  （与现有 `runOnUi` 线程约定一致）。
- `Exclusive` 任务存活期间任务板发 `busyChanged(true)`：壳置灰侧栏切换/危险按钮——
  防重入闸门**合并**进任务模型，不再做独立的 BusyGate 类。

### 2.2 `kc::ui::ActivityBoard` —— 任务板（壳唯一订阅源）

```cpp
class ActivityBoard : public QObject {
    Q_OBJECT
public:
    static ActivityBoard& instance();
    Activity* begin(const QString& title, Activity::Flags f); // 页面经 BasePage::beginActivity 间接触达
    bool busy() const;                       // 存在 Exclusive 活动
signals:
    void activityChanged();                  // 顶层活动变化（UI 线程发）
    void busyChanged(bool busy);
};
```

- 板内维护活动栈（顶层 = 最新 Exclusive 活动，无 Exclusive 时为最新普通活动）。
- 速率/耗时**在板上算**：`advanceStage` 采样 (时间, 累计单位)，指数平滑出速率
  （量纲为字节才显示 MB/s，非字节任务只显示耗时/计数）——页面里手搓的平滑代码全删。
- 板是 QObject 单例但**只发信号、不画任何东西**；显示唯一入口是 §3 的状态带。

### 2.3 与现有 `shellStatusChanged` 的合并

`BasePage::shellStatusChanged(device,mode,hint)` 直连 MainShell 的旧通道废弃：
设备/模式归**设备芯片**（§3 左端），hint 归 `Activity::setNote`。三参信号保留一个过渡期、
内部转任务板，迁完即删（判据：`grep shellStatusChanged ui/` 只剩 BasePage 一处）。

---

## 3. `ActivityStrip` —— 状态带（替代 4 段文字栏）

```
┌──────────────────────────────────────────────────────────────────────┐
│ ● 小米8 · EDL   ▓▓▓▓▓▓▓▓░░░  写入 · super_a 3/12 · 18%（总 42%）  18.4 MB/s · 00:12 │
└──────────────────────────────────────────────────────────────────────┘
   ↑ 左：设备芯片        ↑ 中：单条进度条（阶段推进，总进度以文字呈现）  ↑ 右：速率·耗时
   空闲态：芯片灰点 + 「就绪」，其余塌缩隐藏（不留 `-` 占位）
```

- `ui/ActivityStrip.{h,cpp}`：自绘进度条（`paintEvent` 圆角 + 居中文字），**必背坑①重入守卫**。
- 高度 30px 一条表达全部层级；阶段切换只换条内文字，不需要第二条镜像条。
- objectName 沿用 `shellStatusBar`，qss 增补 `activityChip` / `activityBar`（dark/light 同步加）；
  保持半透明玻璃底，为 §5 背景层留路。
- 绑定：连接 `ActivityBoard::activityChanged` 重绘（Widgets 端信号足够，不必上属性绑定）。

---

## 4. 平台件（与 .NET 无关，壳本该有的）

| 件 | 设计 | 验收 |
|---|---|---|
| USB 热插拔 `UsbMonitor` | `QAbstractNativeEventFilter` 收 `WM_DEVICECHANGE`(ARRIVAL/REMOVAL)，600ms 去抖发 `deviceChanged()`；DeviceHomePage 订阅 + showEvent 重探 | 拔插后主页 ≤600ms 自动重探 |
| 单实例 | **`QLockFile`**（Qt 自带，锁 `%LOCALAPPDATA%\KunClientState\app.lock`）；已锁则 `QLocalSocket` 唤醒已有窗口前置后退出 | 双开第二次不出窗口且旧窗口前置 |
| 崩溃兜底 | `SetUnhandledExceptionFilter` 写 `%LOCALAPPDATA%\KunClientState\crash.log`（异常码+栈帧地址） | 强制崩溃留痕 |
| 版本注入 | 标题栏读 `kc::kVersionString`：`QString::fromLatin1(...)` —— **必背坑③**（`QStringLiteral` 不能包 constexpr） | 标题栏版本 == version.h，改版本只动 version.h+CMake |

---

## 5. 暂不做（backlog，不阻塞 P0）

- **壁纸背景层**：等 ActivityStrip 玻璃底稳定后再垫（QStackedWidget 下层 QWidget 绘制视频/动图），
  届时校验「进度带文字在动图上可读」。
- **VisualFlash 真身**：独立功能任务。
- 菜单栏：侧栏已承担，不加。

---

## 6. 实施顺序与验收判据（全部实测）

| 步 | 内容 | 判据 |
|---|---|---|
| 1 | `Activity` + `ActivityBoard` + Core 单测 | ctest：RAII 析构收尾 / 阶段权重合成 / 跨线程 advance 不崩 / 速率平滑合理 |
| 2 | `ActivityStrip` + qss token（dark/light） | 截图：芯片·进度·速率·耗时四区齐全；空闲塌缩无 `-` 占位；两主题可读 |
| 3 | BasePage `beginActivity` 接入 + 废弃直连 | `grep shellStatusChanged ui/` 仅剩 BasePage 一处；EDL 刷写状态带走字、Exclusive 期侧栏锁死 |
| 4 | UsbMonitor + QLockFile 单实例 + 崩溃兜底 | §4 表格三判据逐条过 |
| 5 | 版本注入 | 改 version.h 重编，标题栏随之变 |
| 6 | （backlog）背景层 / VisualFlash | §5 |

## 7. 必背坑（逐条对照）

1. 自绘控件 setStyleSheet + changeEvent(StyleChange) 回灌 applyTheme = **无限递归栈溢出 0xC00000FD** → 重入守卫。
2. Qt DLL 整组同源，混部署 = exit 127 → dist 走 ldd 闭包手工部署。
3. `QStringLiteral` 不能包 constexpr 变量 → 版本注入用 `QString::fromLatin1`。
