# KunClient 商业壳：VMProtect

> 决策来源：`src/cpp/BUILD.md` 第 123 行原列为「如需更强上商用壳（如 VMProtect）另行决策」，
> 现决策落地为 **壳使用 VMProtect**（`build-release.sh` 已自动接线）。

## 唯一外部依赖

VMProtect 是**付费商业壳**。本仓库**不内置、不提供**任何安装包 / 破解 / 授权绕过。
要真正加壳，你只需提供一样东西：**合法授权的 VMProtect（安装目录或 VMProtect_Con.exe + 授权）**。
其余全部已自动化（见下方一键脚本）。

## 现状

- 本机**当前未安装** VMProtect（无 `VMProtect_Con.exe`、无授权、仓库内无 `.vmp`）。
- 因此 `build-release.sh` 现在**与原先完全一致**：strip + StrObf + 原生反调试，保护步骤静默跳过。
- 一旦你把合法授权的 VMProtect 装好并生成工程，下一次发版就会自动加壳，**无需改任何脚本**。

## 启用步骤

### 方式 A：一键脚本（推荐，全自动化）

1. 安装合法授权的 VMProtect（含 `VMProtect_Con.exe` / `VMProtectGUI.exe`）。
2. 双击 `tools/vmp/enable-vmp.bat`（或在仓库根目录跑 `tools\vmp\enable-vmp.bat "D:\VMProtect"` 指定安装目录）。
   - 脚本自动复制 `VMProtect_Con.exe` → `tools/vmp/`；
   - 若 `tools/vmp/KunClient.vmp` 还没有，会**拉起 GUI 让你点一下 Save Project**（按脚本里列出的 Input/Output/Mode 填好保存）；
   - 随后自动跑 `build-release.sh` 完成加壳构建。

### 方式 B：手工

1. 安装合法授权的 VMProtect（必须包含控制台版 `VMProtect_Con.exe`）。
2. 打开 VMProtect GUI → `File → Open` → 选择 `src/cpp/dist/KunClient.exe`。
   - **Miscellaneous**：勾选 `Compress output file`；可填 Watermark（版本/构建号）。
   - **Options → Mode**：`Virtualization`（或 `Mutation + Virtualization`，强度↔体积/速度权衡）。
     授权支持时勾 `Ultra` 增强虚拟化。
   - **Procedures**：strip 后二进制无符号名，按函数名选不可行；普通用法直接整文件保护即可。
     若需定点保护关键逻辑（如 `kc::security` 的反调试 / StrObf 解密 / 许可校验），
     可在 `build-release.sh` **不加 `--no-strip`** 时先构建，用 GUI 加载带符号的
     `build-release/KunClient.exe` 选函数，再把工程用于受保护产物。
   - **Settings**：填入合法 License。
   - 顶部 `Input file` = `src/cpp/dist/KunClient.exe`；
     `Output file` = `src/cpp/dist/KunClient.vmp.exe`。
3. `File → Save Project As` → 保存为 `tools/vmp/KunClient.vmp`。
4. 把 `VMProtect_Con.exe` 放到 `tools/vmp/`。
   （如放在别处，可在 `build-release.sh` 调用前 `export VMP_BIN=/path/to/VMProtect_Con.exe`。）
5. 重新跑 `build-release.sh`，日志出现 `[VMP]` 即表示已加壳。

## 契约（与 build-release.sh 约定）

| 条件 | 行为 |
|------|------|
| `VMProtect_Con.exe` 或 `KunClient.vmp` 任一缺失 | 跳过保护，构建与原先一致（不报错） |
| 两者都在，VMProtect 执行成功 | `dist/KunClient.vmp.exe` 覆盖回 `dist/KunClient.exe` |
| 两者都在，VMProtect 报错/零退出 | 构建**中止**（不静默发未保护成品） |

## 红线

- **md5 现为「保护后」产物**：发版三方一致校验（构建→strip→复制→scp 单链）要以加壳后的
  `dist/KunClient.exe` 为准；`build-release.sh` 的 md5 步骤已在加壳之后执行。
- VMProtect 与 MinGW64/SEH 兼容。若启用激进虚拟化导致运行期异常，先用
  `build-release.sh --no-strip` 构建，在 VMProtect GUI 单独用 `--screenshot` 探针验证能起来，
  再回到自动管线。
- 仅用**合法授权**的 VMProtect；本仓库不内置、不提供任何破解/授权绕过。
