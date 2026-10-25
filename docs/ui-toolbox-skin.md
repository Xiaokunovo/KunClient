# UI 换肤规范（经典工具箱版式）

> 目标：把 KunClient 的页面从「现代卡片风」改成参考稿那种**经典桌面刷机工具箱**版式。
> 参考稿的三个页签：Qualcomm / FASTBOOT / Payload。外壳（菜单栏 + 页签 + 底部双进度条 + 分段状态栏）
> 已经由 MainWindow 完成，页面只需要按下面的配方重排**页面内部**。

## 1. 版式配方

```
┌ 页面根（Grid, Background=Theme.BgTint, Margin={StaticResource Tool.PagePad}）───────────┐
│ ┌ 左列 316px ──────────────┐ ┌ 主区（*，通常就是 1~2 个分组框）─────────────────────┐ │
│ │ HeaderedContentControl   │ │ HeaderedContentControl ToolGroup  Header="分区管理"  │ │
│ │   ToolGroup Header="…"   │ │   顶部一行：输入框/下拉 + 右侧「浏览」图标钮        │ │
│ │   标签:值 两列读数       │ │   DataGrid Style=ThemedDataGrid（占满剩余高度）      │ │
│ │ HeaderedContentControl   │ │   底部一行：主操作按钮（左） + 次要按钮（右）        │ │
│ │   ToolGroup Header="日志"│ └──────────────────────────────────────────────────────┘ │
│ │   RichTextBox 日志       │                                                          │
│ └──────────────────────────┘                                                          │
└───────────────────────────────────────────────────────────────────────────────────────┘
```

- **左列不是必须的**：只有「信息 + 日志」成对出现的页面才做两列（参考稿三个页签都是两列）。
  单功能页面就用一列分组框自上而下排（分组框之间 8px）。
- 左列宽度固定 `316`，与外壳左栏（`Controls/DeviceLogRail`）同宽 —— 两者不会同时出现。
- 页面**不再有页面标题**：删掉 `<ctrl:PageHeaderBar …/>`。页签名（Qualcomm/FASTBOOT/…）就是标题。
- 页面**不再自己画底部状态栏/进度条**：进度与状态一律上报 `ShellStatus`（见第 3 节）。

## 2. 可用的样式键（ToolboxStyles.xaml）

| 键 | 目标类型 | 用途 |
|---|---|---|
| `ToolGroup` | `HeaderedContentControl` | **分组框**：白底 + 1px 灰描边 + 标题压在边框线上。用 `Header="分区管理"` |
| `ToolFieldLabel` / `ToolFieldValue` | TextBlock | 分组框里的「标签: 值」读数（标签灰、值深） |
| `ToolIconButton` | Button | 方形图标钮（选目录/选文件），`Content="&#xE8B7;"`（Segoe MDL2 文件夹） |
| `ToolReadOnlyBox` | TextBox | 只读展示框（路径/版本号） |
| `ThemedFlatButton` | Button | 白底 + 淡蓝描边 + 蓝灰字的**圆角矩形**按钮（次级操作） |
| `ThemedPrimaryButton` | Button | 同形的**强调版**（强调蓝描边 + 蓝字 + SemiBold）—— **不是实心填充** |
| `ThemedDangerButton` | Button | 同形，描边与字改成错误红（**不是红底白字**） |
| `ToolIconButton` | Button | 方形图标钮（选目录/选文件），`Content="&#xE8B7;"`（Segoe MDL2 文件夹）。**不吃胶囊/圆角矩形**，是方钮 |
| `ToolFold` | Expander | **折叠区**：收起时只有一行 20px 浅灰小字，点开露出内容 |
| `ThemedInputBox` | TextBox | 输入框 |
| `ThemedComboPill` | ComboBox | 下拉框 |
| `ThemedCheckBox` | CheckBox | 勾选框 |
| `ThemedDataGrid` | DataGrid | 表格（灰表头 + 全网格线 + 24px 行高） |
| `RoundedProgressBar` | ProgressBar | 进度条（有进度时才用；页面里一般不需要） |
| `LogRichTextBox` / `LogTextBox` | RichTextBox/TextBox | 日志框（无边框，面板给边框） |
| `ToolStatusLabel` / `ToolStatusValue` | TextBlock | 状态栏读数（页面里一般用不到） |
| `Tool.PagePad` (Thickness) / `Tool.RowHeight` / `Tool.RowGap` / `Tool.PanelPad` (Double) / `Tool.FieldPad` (Thickness) | | 页面留白与行距 |

分组框用法：

```xml
<HeaderedContentControl Style="{StaticResource ToolGroup}" Header="分区管理">
    <Grid>
        <Grid.RowDefinitions>
            <RowDefinition Height="Auto"/>   <!-- 顶行：路径 + 浏览 -->
            <RowDefinition Height="*"/>      <!-- 表格 -->
            <RowDefinition Height="Auto"/>   <!-- 按钮行 -->
        </Grid.RowDefinitions>
        …
    </Grid>
</HeaderedContentControl>
```

⚠️ `ToolGroup` 的模板已经把内容区留了内边距（`10,16,10,10`），**不要再套一层 Padding**。

## 3. 日志 / 状态约定

```csharp
PageLog.Section(LogTarget, "读分区表");           // 区段标题（两侧横线的蓝字）；同时把外壳「任务」段设成该名字
PageLog.BeginOperation(LogTarget, "检测设备连接...");  // 操作行（还没结果）
PageLog.CompleteOperation(LogTarget, ok: true);       // 给上一行补绿色 OK / 红色 Error
PageLog.Info(LogTarget, "设备序列号: 2268b9d7");
PageLog.Footer(LogTarget, "完成, 耗时: 65ms");     // 右对齐蓝字收尾行
```

- 这三条（Section / Begin+Complete / Footer）就是参考稿日志区的样子，**每个「一键式」操作都应该有这个三明治**。
- 外壳状态栏：`ShellStatus.BeginTask("读分区表")` → `ShellStatus.Report(partition:"super", speed:"18.4 MB/s", totalSize:"1.2 GB / 6.5 GB", percent:42, partitionPercent:88)` → `ShellStatus.EndTask()`。
  设备读数：`ShellStatus.SetDevice(device: "2268b9d7", mode: "Bootloader", hint: "就绪")`，
  或（推荐）探测完调 `DeviceStateHub.Report(device, mode)`（主页/EDL 页已经在这么做）。
- 页面里**保留** `x:Name="NotifyBar"/"NotifyIcon"/"NotifyText"` 那套内联提示条（BasePage 依赖这三个名字），
  但把它改成经典外观：`Background="{DynamicResource Theme.InfoBg}"` + `BorderBrush="{DynamicResource Theme.Info}"`。

## 4. 硬规矩

1. **不许改 `x:Name`、事件处理器名、绑定路径**：代码后置（`*.xaml.cs`）里全是引用，改了直接编译不过。
2. **不许删功能**：按钮/输入框/表格/勾选框一个都不能少，只能换位置和外观。原来 `Visibility="Collapsed"` 的
   条件显隐（如 `PartitionSummaryRow`）必须保持由代码控制，不要写死可见性。
3. **代码后置只做加法**：允许在已有方法里补 `PageLog.Section/Footer`、`ShellStatus.*` 调用；
   不要重写业务流程。确实需要挪控件时才改代码。
4. 颜色一律 `{DynamicResource Theme.*}`，**不要写死色值**（写死 = 深色主题下留浅色补丁）。
5. 中文注释保留、并保持同样的解释密度：改了什么、为什么这么改，都写清楚；不要删掉原有注释。
6. **不要跑构建**（`dotnet build`）：多个页面在并行改，构建由主控统一跑。改完自查 XAML 标签是否闭合、
   资源键是否与上表一致即可。
7. 页面根节点保持 `Background="{DynamicResource Theme.BgTint}"`（全局透明度靠它生效）。

## 5. 一比一还原参考稿（2026-10-03）

参考稿 = `沐妍工具箱` 三张截图（1500×975，**含标题栏**）。原件已存到
`.workbuddy/tmp/skin/ref_src/`、去标题栏并缩到 1264 宽的底图在 `.workbuddy/tmp/skin/ref_clean/`
（**不要再引用 G: 盘原路径**，那块盘中途掉过线）。

### 5.1 色板（实测值，不是目测）

| 令牌 | 值 | 依据 |
|---|---|---|
| `Theme.Bg` / `BgTint` | `#FAFBFF` | 主底占比 60~66% |
| `Theme.Accent` | `#7AB8F3` | 唯一强调色（进度条填充） |
| `Theme.AccentText` | `#10304F` | `#7AB8F3` 很浅，**不能配白字** |
| `Theme.AccentText2` | `#3E8FE0` | 蓝字（链接/品牌） |
| `Theme.TabStripBg` / `StatusBarBg` | `#F0F4FF` | 页签条 + 状态栏带 |
| `Theme.SurfaceAlt` / `PanelHeadBg` | `#F4F7FE` | 表头 / 面板头 |
| `Theme.Border` | `#D5DEEB` | 分组框描边 |
| `Theme.BtnBorder` / `BtnText` | `#B7CBDE` / `#5B7FA0` | 按钮描边 / 按钮字 |
| `Theme.GroupHeader` | `#3F5570` | 分组框标题字（藏青） |

⚠️ **不要照抄 JPG 里读到的"中灰文字"**：参考稿是 JPG，字芯被压缩糊掉了，
整图最暗 0.2% 只能读到 `#4E4F54`。按原图非采样区测得的正文其实是近黑 `#1F2329`。

**尺寸对齐的正确姿势**：参考稿是约 **1.27 倍**（≈125% DPI）的渲染 —— 按钮高 33、表行高 33、
进度条 22 在 1264 宽下。按比例换算回逻辑尺寸后，按钮 26 / 行高 26 / 进度条 17 就**正好**是
我们现在的值。**不要把这些值直接抄成 33/33/22**，那会把 1000×640 档挤变形。

### 5.2 按钮

参考稿里所有按钮都是**圆角矩形**（1264 宽下按钮高 34、圆弧水平内收仅约 5px，
半径 ≈7 物理像素 ≈ 高度的 20%），白底 + 淡蓝描边 + 蓝灰字，**没有任何实心填充按钮**。
→ 用 `CornerRadius="{StaticResource R.Card}"`(4)。
🔴 **不要用 R.Chip/R.Pill(999) 那种全圆角** —— 在 26px 高的按钮上会变成一颗颗药丸，与参考稿不符。

### 5.3 折叠区（多出来的功能怎么收）

参考稿只展示主流程。我们多出来的功能区一律收进 `ToolFold`：

- **每列最多一行折叠区**：多个 Expander 平铺排（4~6 行浅灰小字）会把参考稿干净的版面弄脏。
  能并的并成一个（EDL 左列 = 「更多选项」一个，右列 = 「更多操作」一个）。
- 🔴 **代码控制显隐的东西不许放进折叠区**：`Visibility` 由 .cs 设的控件（如 `PanelOplusAuth`、
  `LoaderProgressRow`）一旦落在收起的 Expander 里就**永远显示不出来**。它们必须留在折叠区外面。
- 🔴 **折叠区里的控件不在可视树里**（Expander 收起时 ContentPresenter 是 Collapsed）→
  按名字查找的**探针**必须先展开再找。`combo_probe2` 已加 `FindSubTab()` 走这条真实路径。

### 5.4 「搜索分区」是右端一枚独立按钮

参考稿那个位置**没有常驻输入框**，只有一枚右对齐的按钮。所以输入框默认 `Collapsed`，
按钮的 `Tag` 写要开关的容器名，处理器用 `BasePage.SearchToggle_Click`（三个页签共用）：

```xml
<TextBox x:Name="TxtPartSearch" Visibility="Collapsed" TextChanged="TxtPartSearch_TextChanged"/>
<Button Content="搜索分区" Click="SearchToggle_Click" Tag="TxtPartSearch"/>
```

再点一次收起并清空过滤词（不清空的话用户会以为表格"少了行"）。
输入框的 `x:Name` 与 `TextChanged` 不变 ⇒ 过滤逻辑一行没动。

### 5.5 🔴 搬完 XAML 必须跑标签配平检查

编译只给一句 `MC3000: The 'Grid' start tag on line N does not match the end tag of 'X'`，
**不给中间上下文**。用 `.workbuddy/tmp/skin/check_tags.py <file.xaml> <根标签>` 定位。

⚠️ 写这类检查器有个坑：**起始标签常常跨多行**，按"逐行跑正则"写会永远匹配不到根标签，
于是报出「根未闭合」这种完全误导的结论。必须在**整段文本**上跑正则，再用匹配位置换算行号。

