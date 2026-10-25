<#
.SYNOPSIS
    KunClient C++ 自动化审计流水线 —— 一条命令跑完全部静态检查。

.DESCRIPTION
    依次执行：
      1. CMake 配置 + 全量编译（捕获所有编译警告）
      2. GCC -fanalyzer 路径敏感分析（抓 UAF / 空指针 / 资源泄漏）
      3. 自定义模式扫描（危险函数、TLS 关闭、弱随机、硬编码密钥等）
      4. 单元测试（ctest）
      5. 生成 Markdown + 控制台摘要

.PARAMETER Mode
    full   —— 全量重编（慢，约 12 分钟，最干净）
    quick  —— 增量编译（快，只重新编译改动过的文件）
    scan   —— 跳过编译，只跑模式扫描（最快，秒级）

.PARAMETER OutDir
    报告输出目录，默认 <src>/audit-out

.PARAMETER Jobs
    并行编译任务数，默认 CPU 核数

.EXAMPLE
    .\run-audit.ps1                  # 增量审计（日常用）
    .\run-audit.ps1 -Mode full       # 发版前全量审计
    .\run-audit.ps1 -Mode scan       # 只扫危险模式

.NOTES
    退出码：0 = 全部通过；1 = 有 Critical/High 级发现或测试失败。
#>

[CmdletBinding()]
param(
    [ValidateSet('full', 'quick', 'scan')]
    [string]$Mode = 'quick',

    [string]$OutDir = '',

    [int]$Jobs = 0,

    # scan 模式默认跳过 -fanalyzer（它是最慢的一步）。需要时显式加这个开关。
    [switch]$WithAnalyzer
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

# ─────────────────────────────────────────────────────────────────────────────
#  环境准备
# ─────────────────────────────────────────────────────────────────────────────

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$SrcDir = $ScriptDir
$MsysRoot = 'E:\msys64'
$MingwBin = Join-Path $MsysRoot 'mingw64\bin'

if (-not (Test-Path $MingwBin)) {
    Write-Host "✗ 找不到 MSYS2 mingw64：$MingwBin" -ForegroundColor Red
    exit 1
}

$env:PATH = "$MingwBin;$env:PATH"

$CMake = Join-Path $MingwBin 'cmake.exe'
$Ninja = Join-Path $MingwBin 'ninja.exe'
$Gxx = Join-Path $MingwBin 'g++.exe'
$Cxx = Join-Path $MingwBin 'c++.exe'
$CTest = Join-Path $MingwBin 'ctest.exe'

foreach ($tool in @($CMake, $Ninja, $CTest)) {
    if (-not (Test-Path $tool)) { Write-Host "✗ 缺少工具：$tool" -ForegroundColor Red; exit 1 }
}

if ($OutDir -eq '') { $OutDir = Join-Path $SrcDir 'audit-out' }
$BuildDir = Join-Path $SrcDir 'build-audit'
if ($Jobs -le 0) { $Jobs = [Environment]::ProcessorCount }

New-Item -ItemType Directory -Path $OutDir -Force | Out-Null

$WarnLog     = Join-Path $OutDir 'warnings.log'
$AnalyzerLog = Join-Path $OutDir 'analyzer.log'
$ScanLog     = Join-Path $OutDir 'scan.log'
$TestLog     = Join-Path $OutDir 'tests.log'
$ReportMd    = Join-Path $OutDir 'audit-summary.md'

$script:Critical = New-Object System.Collections.ArrayList
$script:High     = New-Object System.Collections.ArrayList
$script:Medium   = New-Object System.Collections.ArrayList
$script:Info     = New-Object System.Collections.ArrayList

function Add-Finding {
    param([string]$Level, [string]$File, [string]$Line, [string]$Msg)
    $item = [pscustomobject]@{ File = $File; Line = $Line; Msg = $Msg }
    switch ($Level) {
        'Critical' { [void]$script:Critical.Add($item) }
        'High'     { [void]$script:High.Add($item) }
        'Medium'   { [void]$script:Medium.Add($item) }
        default    { [void]$script:Info.Add($item) }
    }
}

function Write-Banner {
    param([string]$Text)
    Write-Host ''
    Write-Host ('=' * 72) -ForegroundColor DarkCyan
    Write-Host " $Text" -ForegroundColor Cyan
    Write-Host ('=' * 72) -ForegroundColor DarkCyan
}

$ScriptStart = Get-Date

# ─────────────────────────────────────────────────────────────────────────────
#  1. 编译
# ─────────────────────────────────────────────────────────────────────────────

if ($Mode -ne 'scan') {
    Write-Banner "1/4  编译（模式：$Mode）"

    if (-not (Test-Path (Join-Path $BuildDir 'build.ninja'))) {
        Write-Host '  首次配置 CMake…' -ForegroundColor DarkGray
        $cfgArgs = @(
            '-S', $SrcDir, '-B', $BuildDir, '-G', 'Ninja',
            '-DCMAKE_BUILD_TYPE=Release',
            "-DCMAKE_CXX_COMPILER=$($Gxx.Replace('\','/'))",
            "-DCMAKE_PREFIX_PATH=$((Join-Path $MsysRoot 'mingw64').Replace('\','/'))",
            '-DCMAKE_EXPORT_COMPILE_COMMANDS=ON'
        )
        & $CMake @cfgArgs 2>&1 | Out-File -Encoding utf8 (Join-Path $OutDir 'cmake-configure.log')
        if ($LASTEXITCODE -ne 0) {
            Write-Host '  ✗ CMake 配置失败，见 cmake-configure.log' -ForegroundColor Red
            exit 1
        }
    }

    $sw = [Diagnostics.Stopwatch]::StartNew()
    if ($Mode -eq 'full') {
        & $Ninja -C $BuildDir -k 0 -j $Jobs 2>&1 | Out-File -Encoding utf8 $WarnLog
    } else {
        & $Ninja -C $BuildDir -k 0 -j $Jobs 2>&1 | Out-File -Encoding utf8 $WarnLog
    }
    $buildExit = $LASTEXITCODE
    $sw.Stop()

    $warnCount = (Select-String -Path $WarnLog -Pattern ': warning:' -ErrorAction SilentlyContinue | Measure-Object).Count
    $errCount  = (Select-String -Path $WarnLog -Pattern ': error:'   -ErrorAction SilentlyContinue | Measure-Object).Count

    if ($buildExit -ne 0 -or $errCount -gt 0) {
        Write-Host ("  ✗ 编译失败：{0} error（耗时 {1:N1}s）" -f $errCount, $sw.Elapsed.TotalSeconds) -ForegroundColor Red
        Add-Finding 'Critical' 'build' '-' "$errCount 个编译错误，见 warnings.log"
    } else {
        Write-Host ("  ✓ 编译通过，{0} warning（耗时 {1:N1}s）" -f $warnCount, $sw.Elapsed.TotalSeconds) -ForegroundColor Green
        if ($warnCount -gt 0) { Add-Finding 'Info' 'build' '-' "$warnCount 条编译警告（多为误报，需人工判读）" }
    }

    # 真正有意义的警告类型（排除已知的 Qt 内部误报）
    $realWarnPatterns = @(
        @{ Re = '-Wswitch\b';                Lv = 'Medium'; Desc = 'switch 未覆盖全部枚举值' },
        @{ Re = '-Wunused-result';           Lv = 'Medium'; Desc = '忽略 [[nodiscard]] 返回值' },
        @{ Re = '-Wunused-function';         Lv = 'Info';   Desc = '定义了未使用的函数' },
        @{ Re = '-Wmaybe-uninitialized';     Lv = 'High';   Desc = '可能使用未初始化变量' },
        @{ Re = '-Wuninitialized';           Lv = 'High';   Desc = '使用未初始化变量' },
        @{ Re = '-Wreturn-type';             Lv = 'High';   Desc = '控制流可能未返回值' },
        @{ Re = '-Wformat';                  Lv = 'Medium'; Desc = '格式化字符串不匹配' },
        @{ Re = '-Wsign-compare';            Lv = 'Info';   Desc = '有符号/无符号比较' },
        @{ Re = '-Wdelete-non-virtual-dtor'; Lv = 'High';   Desc = '非虚析构基类被 delete' }
    )

    if (Test-Path $WarnLog) {
        foreach ($m in Select-String -Path $WarnLog -Pattern '^(.+?):(\d+):\d+: warning: (.+?)\s*\[(-W[^\]]+)\]') {
            $file = $m.Matches[0].Groups[1].Value
            $line = $m.Matches[0].Groups[2].Value
            $msg  = $m.Matches[0].Groups[3].Value
            $flag = $m.Matches[0].Groups[4].Value
            foreach ($p in $realWarnPatterns) {
                if ($flag -match $p.Re) {
                    $short = Split-Path -Leaf $file
                    Add-Finding $p.Lv $short $line "$($p.Desc)：$msg"
                    break
                }
            }
        }
    }
} else {
    Write-Banner '1/4  编译（已跳过）'
}

# ─────────────────────────────────────────────────────────────────────────────
# ── 第 2 段：GCC -fanalyzer（可跳过）────────────────────────────────────────
# -fanalyzer 是 GCC 的路径敏感分析器，能抓 use-after-free / 空指针解引用 /
# 资源泄漏。代价是慢：27 个文件约 6 分钟，所以 scan 模式默认关闭它。

Write-Banner '2/4  GCC -fanalyzer 路径敏感分析'

$analyzerTargets = @(
    'storage/PayloadParser.cpp', 'storage/GptParser.cpp', 'storage/SparseImage.cpp',
    'storage/DynamicSuper.cpp', 'storage/RawProgramParser.cpp', 'storage/SystemVersionReader.cpp',
    'oplus/AesCfb.cpp', 'oplus/OfpExtractor.cpp', 'oplus/OpsExtractor.cpp',
    'protocol/Sahara.cpp', 'protocol/Firehose.cpp', 'transport/SerialTransport.cpp',
    'app/EdlWorker.cpp', 'core/EdlMaintenance.cpp', 'core/PartitionProtection.cpp',
    'kc/net/ChunkedDownloader.cpp', 'kc/net/HttpClient.cpp',
    'kc/security/Digest.cpp', 'kc/security/Sha256.cpp', 'kc/security/DpapiBlob.cpp',
    'kc/mtk/MtkXflash.cpp', 'kc/mtk/MtkDaFile.cpp', 'kc/mtk/MtkBrom.cpp',
    'kc/mtk/ScatterParser.cpp', 'kc/tools/PlatformToolsManager.cpp',
    'kc/auth/LoginStorage.cpp', 'kc/security/EnvReportService.cpp'
)

function Invoke-Analyzer {
    $dbPath = Join-Path $BuildDir 'compile_commands.json'
    if (-not (Test-Path $dbPath)) {
        Write-Host '  --  无 compile_commands.json，跳过（先跑一次编译）' -ForegroundColor Yellow
        return
    }
    if (-not (Test-Path $Gxx)) {
        Write-Host '  --  找不到 g++，跳过' -ForegroundColor Yellow
        return
    }

    $cc = Get-Content $dbPath -Raw | ConvertFrom-Json
    $tmpObj = Join-Path $BuildDir '_analyzer_tmp.obj'
    Remove-Item $AnalyzerLog -ErrorAction SilentlyContinue
    $analyzed = 0

    Write-Host '  分析中（每个文件需数秒）…' -ForegroundColor DarkGray
    foreach ($t in $analyzerTargets) {
        $sourcePath = Join-Path $SrcDir ($t -replace '/', '\')
        if (-not (Test-Path $sourcePath)) { continue }

        $entry = $cc | Where-Object { $_.file.Replace('\', '/') -like "*/$t" } | Select-Object -First 1
        if (-not $entry) { continue }

        # 从 compile_commands 里取 -I/-D/-isystem/-std，去掉 -o/-c/依赖文件相关项
        $parts = @($entry.command -split ' +' | Where-Object { $_ -ne '' })
        $gargs = @()
        for ($i = 1; $i -lt $parts.Count; $i++) {
            $p = $parts[$i].Trim('"')
            if ($p -eq '-o' -or $p -eq '-MD' -or $p -eq '-MT' -or $p -eq '-MF') { $i++; continue }
            if ($p -eq '-c') { continue }
            if ($p -eq $sourcePath -or $p -match '\.(cpp|cc|cxx|o|obj)$') { continue }
            $gargs += $p
        }

        "===== $t =====" | Add-Content $AnalyzerLog

        # g++ 的诊断走 stderr；用 Out-File 合并，避免 ErrorActionPreference=Stop 中止脚本
        $tmpOut = Join-Path $BuildDir '_analyzer_out.tmp'
        $prevEap = $ErrorActionPreference
        $ErrorActionPreference = 'Continue'
        & $Gxx @gargs '-fanalyzer' '-c' '-o' $tmpObj $sourcePath 2>&1 |
            Out-File -FilePath $tmpOut -Encoding utf8
        $ErrorActionPreference = $prevEap

        if (Test-Path $tmpOut) {
            Get-Content $tmpOut -Encoding UTF8 -ErrorAction SilentlyContinue | Add-Content $AnalyzerLog
            Remove-Item $tmpOut -Force -ErrorAction SilentlyContinue
        }
        Remove-Item $tmpObj -ErrorAction SilentlyContinue
        $analyzed++
    }

    # 只提取真实警告 —— Qt 头文件内部触发的 analyzer 报的是模板实例化误报
    $anWarn = 0
    if (Test-Path $AnalyzerLog) {
        foreach ($m in Select-String -Path $AnalyzerLog -Pattern '^(.+?):(\d+):\d+: warning: (.+?)\s*\[(-W[^\]]+)\]') {
            $file = $m.Matches[0].Groups[1].Value
            if ($file -match 'msys64|qt6') { continue }
            $lineNo = $m.Matches[0].Groups[2].Value
            $msg = $m.Matches[0].Groups[3].Value
            $flag = $m.Matches[0].Groups[4].Value
            $lv = 'Medium'
            if ($flag -match 'uninitialized|use-after-free|deref|double-free|leak') { $lv = 'High' }
            Add-Finding $lv (Split-Path -Leaf $file) $lineNo "analyzer: $msg"
            $anWarn++
        }
    }
    Write-Host ("  OK  分析 {0} 个文件，{1} 条发现" -f $analyzed, $anWarn) -ForegroundColor Green
}

if ($Mode -eq 'scan' -and -not $WithAnalyzer) {
    Write-Host '  --  跳过（scan 模式默认关闭，加 -WithAnalyzer 强制运行）' -ForegroundColor DarkGray
} else {
    Invoke-Analyzer
}


# ─────────────────────────────────────────────────────────────────────────────
#  3. 自定义模式扫描
# ─────────────────────────────────────────────────────────────────────────────

Write-Banner '3/4  危险模式扫描'

$sourceFiles = Get-ChildItem -Path $SrcDir -Recurse -File -Include *.cpp, *.h, *.hpp, *.cc |
    Where-Object { $_.FullName -notmatch '\\build[^\\]*\\|\\dist|_autogen|\\audit-out\\' }

Write-Host ("  扫描 {0} 个源文件…" -f $sourceFiles.Count) -ForegroundColor DarkGray

$rules = @(
    # ── 内存安全 ──
    @{ Name = '危险 C 函数'
       Re = '\b(strcpy|strcat|sprintf|vsprintf|gets|mktemp|tmpnam)\s*\('
       Lv = 'High'; Why = '无边界检查，应改用 strncpy/snprintf/std::string' }

    @{ Name = 'system() 调用'
       # 排除 QRandomGenerator::system()（CSPRNG 入口，非 shell 调用）
       Re = '(?<!QRandomGenerator::)(?<!::)\bsystem\s*\('
       Lv = 'High'; Why = '命令注入面，改用 QProcess 参数数组' }

    @{ Name = 'alloca 使用'
       Re = '\balloca\s*\('
       Lv = 'Medium'; Why = '栈分配无失败检查，大尺寸会直接爆栈' }

    # ── 安全 ──
    @{ Name = '关闭 TLS 校验'
       Re = 'setPeerVerifyMode\s*\(\s*QSslSocket::VerifyNone|ignoreSslErrors\s*\(|setPeerVerifyDepth\s*\(\s*0'
       Lv = 'Critical'; Why = '中间人攻击面' }

    @{ Name = '弱随机数'
       Re = '\b(qrand|srand|std::rand|random_shuffle)\s*\('
       Lv = 'High'; Why = '非密码学随机，密钥/令牌不可用' }

    @{ Name = '时间种子'
       Re = 'srand\s*\(.*time\s*\('
       Lv = 'High'; Why = '可预测的随机序列' }

    # ── 并发 ──
    @{ Name = 'wait() 返回值未检查'
       Re = '\.wait\s*\(\s*\d+\s*\)\s*;'
       Lv = 'Medium'; Why = 'QThread 等待超时后继续访问即 UB' }

    @{ Name = 'BlockingQueuedConnection'
       Re = 'Qt::BlockingQueuedConnection'
       Lv = 'Info'; Why = '跨线程同步等待，确认不在 UI 线程长阻塞' }

    # ── 资源 ──
    @{ Name = '裸 new 无 parent'
       Re = '=\s*new\s+Q[A-Za-z]+\s*\(\s*\)\s*;'
       Lv = 'Medium'; Why = '无 parent 的 QObject 需手动 delete' }

    # ── 缓冲区 ──
    @{ Name = 'memcpy 固定长度'
       Re = '\bmemcpy\s*\([^,]+,[^,]+,\s*(?:sizeof|0x[0-9A-Fa-f]+|\d+)\s*\)'
       Lv = 'Info'; Why = '确认目标缓冲区足够大' }

    # ── 路径 ──
    @{ Name = '路径拼接无校验'
       Re = 'filePath\([^)]*\+\s*QStringLiteral\("/"\)|CacheRoot\(\)\s*\+\s*"/"'
       Lv = 'Medium'; Why = '确认拼接来源可信或已 cleanPath' }
)

Remove-Item $ScanLog -ErrorAction SilentlyContinue
$scanHits = 0

# ─────────────────────────────────────────────────────────────────────────────
#  已核实白名单：这些命中经人工复核确认无害，不再重复告警。
#  格式 "文件名:行号"（哈希前缀匹配，行号漂移后可只写 "文件名:行号"）。
#  维护方式：每次审计后把确认的误报加进来，附一句理由。
# ─────────────────────────────────────────────────────────────────────────────
$acknowledged = @{
    # Qt 的 QLayout 构造后由 setLayout/addLayout 接管所有权，无需 parent
    'PageHeaderBar.cpp:34'  = 'QHBoxLayout 交给 setLayout 接管'
    'PageHeaderBar.cpp:38'  = 'QVBoxLayout 交给 addLayout 接管'
    # QNetworkAccessManager 归属 HttpClient（析构里 delete）或 natively parented
    'HttpClient.cpp:41'     = '已由 HttpClient 析构管理'
    # QSerialPort 由 MtkTransport 的 d-pointer 持有并在析构释放
    'MtkTransport.cpp:34'   = '由 MtkTransport 私有实现持有'
    # QTableWidgetItem：setItem 后表接管所有权
    'MtkFlashPage.cpp:298'  = 'setItem 后由 QTableWidget 接管'
    # 测试代码里的 memcpy 是构造固定测试数据，长度受控
    'DynamicSuperTest.cpp:68'  = '测试构造数据'
    'DynamicSuperTest.cpp:106' = '测试构造数据'
    'DynamicSuperTest.cpp:128' = '测试构造数据'
    'DynamicSuperTest.cpp:140' = '测试构造数据'
    'GptParserTest.cpp:66'     = '测试构造数据'
    'GptParserTest.cpp:328'    = '测试构造数据'
    # BlockingQueuedConnection：已确认调用点都在非 UI 线程或已有超时保护
    'HeadlessController.cpp:221' = 'headless 模式，无 UI 线程'
    'XiaomiEdlPage.cpp:359'      = '已确认有对应超时保护'
}

foreach ($rule in $rules) {
    $hits = $sourceFiles | Select-String -Pattern $rule.Re -ErrorAction SilentlyContinue
    if (-not $hits) { continue }

    # 过滤注释行（简单启发式）
    $real = $hits | Where-Object { $_.Line.Trim() -notmatch '^(//|\*|/\*)' }
    if (-not $real) { continue }

    # 过滤白名单
    $real = $real | Where-Object {
        $key = "$(Split-Path -Leaf $_.Path):$($_.LineNumber)"
        -not $acknowledged.ContainsKey($key)
    }
    if (-not $real) { continue }

    "===== $($rule.Name) [$($rule.Lv)] =====" | Add-Content $ScanLog
    "  原因：$($rule.Why)" | Add-Content $ScanLog
    foreach ($h in $real) {
        $rel = $h.Path.Replace("$SrcDir\", '')
        "  ${rel}:$($h.LineNumber): $($h.Line.Trim())" | Add-Content $ScanLog
        Add-Finding $rule.Lv (Split-Path -Leaf $h.Path) $h.LineNumber "$($rule.Name)：$($rule.Why)"
        $scanHits++
    }
    "" | Add-Content $ScanLog
    Write-Host ("  · {0}: {1} 处" -f $rule.Name, $real.Count) -ForegroundColor DarkYellow
}

Write-Host ("  ✓ 命中 {0} 处（已排除 {1} 条人工核实白名单）" -f $scanHits, $acknowledged.Count) -ForegroundColor Green

# ─────────────────────────────────────────────────────────────────────────────
#  4. 单元测试
# ─────────────────────────────────────────────────────────────────────────────

$testPass = 0; $testFail = 0; $testTotal = 0
if ($Mode -ne 'scan' -and (Test-Path (Join-Path $BuildDir 'CTestTestfile.cmake'))) {
    Write-Banner '4/4  单元测试'
    & $CTest --test-dir $BuildDir --output-on-failure 2>&1 | Out-File -Encoding utf8 $TestLog
    $testExit = $LASTEXITCODE

    if (Test-Path $TestLog) {
        $summary = Select-String -Path $TestLog -Pattern 'tests passed out of (\d+)' | Select-Object -First 1
        if ($summary) { $testTotal = [int]$summary.Matches[0].Groups[1].Value }
        $p = Select-String -Path $TestLog -Pattern '^\s*(\d+)/\d+ Test\s+#\d+: (\S+) \.+ Passed'
        $f = Select-String -Path $TestLog -Pattern '\*\*\*Failed'
        $testPass = if ($p) { $p.Count } else { 0 }
        $testFail = if ($f) { $f.Count } else { 0 }
    }

    if ($testExit -eq 0) {
        Write-Host ("  ✓ {0}/{1} 通过" -f $testPass, $testTotal) -ForegroundColor Green
    } else {
        Write-Host ("  ✗ {0} 个失败" -f $testFail) -ForegroundColor Red
        Add-Finding 'Critical' 'tests' '-' "$testFail 个测试失败，见 tests.log"
    }
} else {
    Write-Banner '4/4  单元测试（已跳过）'
}

# ─────────────────────────────────────────────────────────────────────────────
#  汇总
# ─────────────────────────────────────────────────────────────────────────────

$ScriptEnd = Get-Date
$Duration = $ScriptEnd - $ScriptStart

Write-Banner '审计汇总'

$total = $script:Critical.Count + $script:High.Count + $script:Medium.Count
Write-Host ("  Critical : {0}" -f $script:Critical.Count) -ForegroundColor Red
Write-Host ("  High     : {0}" -f $script:High.Count)     -ForegroundColor DarkYellow
Write-Host ("  Medium   : {0}" -f $script:Medium.Count)   -ForegroundColor Yellow
Write-Host ("  Info     : {0}" -f $script:Info.Count)     -ForegroundColor DarkGray
Write-Host ("  耗时     : {0:N1}s" -f $Duration.TotalSeconds) -ForegroundColor DarkGray

# ── 生成 Markdown 摘要 ──
$md = New-Object System.Text.StringBuilder
[void]$md.AppendLine('# KunClient 审计摘要')
[void]$md.AppendLine('')
[void]$md.AppendLine("> 生成时间：$($ScriptEnd.ToString('yyyy-MM-dd HH:mm:ss'))　模式：**$Mode**　耗时：$('{0:N1}' -f $Duration.TotalSeconds)s")
[void]$md.AppendLine('')
[void]$md.AppendLine('## 概览')
[void]$md.AppendLine('')
[void]$md.AppendLine('| 级别 | 数量 |')
[void]$md.AppendLine('|---|---:|')
[void]$md.AppendLine("| 🔴 Critical | $($script:Critical.Count) |")
[void]$md.AppendLine("| 🟠 High | $($script:High.Count) |")
[void]$md.AppendLine("| 🟡 Medium | $($script:Medium.Count) |")
[void]$md.AppendLine("| ⚪ Info | $($script:Info.Count) |")
[void]$md.AppendLine('')

function Write-Findings {
    param($List, [string]$Title, [string]$Emoji)
    if ($List.Count -eq 0) { return }
    [void]$md.AppendLine("## $Emoji $Title")
    [void]$md.AppendLine('')
    [void]$md.AppendLine('| 文件 | 行 | 说明 |')
    [void]$md.AppendLine('|---|---:|---|')
    foreach ($x in ($List | Sort-Object File, { [int]$_.Line })) {
        $msg = $x.Msg -replace '\|', '\|'
        [void]$md.AppendLine("| ``$($x.File)`` | $($x.Line) | $msg |")
    }
    [void]$md.AppendLine('')
}

Write-Findings $script:Critical 'Critical' '🔴'
Write-Findings $script:High     'High'     '🟠'
Write-Findings $script:Medium   'Medium'   '🟡'

if ($script:Info.Count -gt 0) {
    [void]$md.AppendLine('<details><summary>⚪ Info（' + $script:Info.Count + ' 条，点击展开）</summary>')
    [void]$md.AppendLine('')
    [void]$md.AppendLine('| 文件 | 行 | 说明 |')
    [void]$md.AppendLine('|---|---:|---|')
    foreach ($x in ($script:Info | Sort-Object File, { [int]$_.Line })) {
        $msg = $x.Msg -replace '\|', '\|'
        [void]$md.AppendLine("| ``$($x.File)`` | $($x.Line) | $msg |")
    }
    [void]$md.AppendLine('')
    [void]$md.AppendLine('</details>')
    [void]$md.AppendLine('')
}

[void]$md.AppendLine('## 产物')
[void]$md.AppendLine('')
foreach ($f in @($WarnLog, $AnalyzerLog, $ScanLog, $TestLog)) {
    if (Test-Path $f) {
        $size = [int]((Get-Item $f).Length / 1024)
        [void]$md.AppendLine("- ``$(Split-Path -Leaf $f)`` (${size} KB)")
    }
}

$md.ToString() | Set-Content -Path $ReportMd -Encoding utf8

Write-Host ''
Write-Host "  报告：$ReportMd" -ForegroundColor Cyan
Write-Host "  目录：$OutDir" -ForegroundColor Cyan
Write-Host ''

# 退出码：有 Critical 或 High 即视为需要关注
if ($script:Critical.Count -gt 0 -or $script:High.Count -gt 0) {
    Write-Host '  ⚠ 存在 Critical/High 级发现，请查看报告。' -ForegroundColor Yellow
    exit 1
}
Write-Host '  ✓ 未发现 Critical/High 级问题。' -ForegroundColor Green
exit 0