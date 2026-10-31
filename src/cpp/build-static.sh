#!/usr/bin/env bash
# ─────────────────────────────────────────────────────────────────────────────
#  KunClient 静态单文件构建（MSYS2 UCRT64 + 静态 Qt6）
#
#  产物（dist-static/）：
#    默认（无 --suffix）：KunClient.exe / KunClient-<ver>.exe / KunClient-<ver>-upx.exe
#    --suffix min      ：KunClient-<ver>-min.exe / KunClient-<ver>-min-upx.exe / KunClient-<ver>-min.7z
#
#  🔴 strip 是这条链路的必需步骤：静态 Qt 带完整符号时约 67MB，strip 后约 40MB
#     （省 ~40%）。以前只有 preset 手动跑、没人 strip，所以静态包一直虚胖。
#
#  体积分层（2026-10-11 实测，2026-10-24 复核）：
#    67.6MB  未 strip（带全部调试符号）
#    ~39.8MB strip + KC_SIZE_OPT（preset 已默认 -Os + --gc-sections，再省 ~2%）
#    ~27.7MB strip + **精简 Qt**（E:/qt6-min/6.12.0，剥离 OpenGL/Vulkan/OpenSSL/SQL/
#            PrintSupport/PDF/Movie 等未用特性；同 Qt 6.12.0，功能等价，−34.7%）
#    8.5MB   精简 Qt + UPX --best --lzma（**默认**；⚠️ 装了 360/杀软的机器有误报风险）
#    7.3MB   精简 Qt + 7z 压缩包（--7z 可选；体积最小且零风险，用户需解压一次）
#
#  🔴 2026-10-14 实测：exe 是**代码主导**（`.text` 占 72%，无 ICU 数据块、TLS 走 schannel
#     未链 OpenSSL）⇒ 任何压缩/编译选项都动不了本体，只能靠外壳压缩或重编 Qt。
#  🔴 2026-10-24：换用自建精简 Qt（E:/qt6-min/6.12.0）后 strip 后 27,709,952 B，
#     与基线 8/8 张截图逐字节相同，--netcheck / --env-check 均 rc=0。
#  🔴 7-Zip 官方的 7z.sfx / 7zCon.sfx（v26.02 实测）**不读 SFX 配置** ⇒ 只能做
#     「双击解出自解压包，用户再点一次」，做不到自动启动。
#
#  用法（可从仓库任意目录调用，脚本自己定位 src/cpp）：
#    ./build-static.sh                  # 自动选 Qt（有精简 Qt 就用它）+ 构建 + strip + 部署 + UPX + md5 + 联网自检
#    ./build-static.sh --std-qt         # 强制用 MSYS2 的 qt6-static（旧行为，产物 ~42.4MB）
#    ./build-static.sh --min-qt         # 强制用精简 Qt（E:/qt6-min/6.12.0，缺失即报错）
#    ./build-static.sh --7z             # 额外产出 7z（~7.3MB）
#    ./build-static.sh --no-upx         # 跳过 UPX（只要 strip 后原始单文件时用）
#    ./build-static.sh --suffix min     # 产物名带后缀，避免覆盖已发布的回滚底本
#    ./build-static.sh --min-qt --7z --suffix min   # ← 当前发布推荐配方
#    ./build-static.sh --clean          # 先把旧构建目录改名留档（不用 rm -rf），再全量构建
#
#  🔴 2026-10-16 起 **UPX 单文件为默认打包方式**（用户拍板「换一种方式打包」）：
#     strip 后 41.1MB → UPX 12.6MB（30.7%），**免解压、双击即用**；精简 Qt 下为
#     27.7MB → 8.5MB。⚠️ 代价：UPX 壳在 360/火绒等杀软上有误报风险，分发前先查杀。
# ─────────────────────────────────────────────────────────────────────────────
set -euo pipefail

# 🔴 PATH 必须最先修：从 PowerShell 直接 `bash build-static.sh` 时，PATH 里只有 Windows
#    目录，dirname/grep/sed/stat/md5sum/cp 全部 MISSING，脚本会在第一行就莫名其妙失败
#    （2026-10-24 实测：`bash src/cpp/build-static.sh` → `dirname: command not found`）。
export PATH="/usr/bin:/bin:/e/msys64/ucrt64/bin:$PATH"

SRC_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DIST_DIR="$SRC_DIR/dist-static"
MSYS_ROOT="E:/msys64"
UCRT_BIN="$MSYS_ROOT/ucrt64/bin"
STD_QT_PREFIX="$MSYS_ROOT/ucrt64/qt6-static"
# 自建精简 Qt 前缀（可用 KC_MIN_QT_PREFIX 覆盖）。版本必须与项目要求一致（Qt 6.12.0）。
MIN_QT_PREFIX="${KC_MIN_QT_PREFIX:-E:/qt6-min/6.12.0}"

# 7-Zip 的位置别写死：Win11 自带的是 C:/Windows/system32/7z.exe（2026-10-21 实测 25.01），
# 装了官方包的是 Program Files。按顺序找，找不到再报错。
SEVENZIP=""
for cand in "C:/Program Files/7-Zip/7z.exe" "C:/Program Files (x86)/7-Zip/7z.exe" "C:/Windows/system32/7z.exe"; do
    [ -x "$cand" ] && SEVENZIP="$cand" && break
done
[ -n "$SEVENZIP" ] || SEVENZIP="$(command -v 7z || true)"

log()  { printf '\033[36m[%s]\033[0m %s\n' "$(date +%H:%M:%S)" "$*"; }
warn() { printf '\033[33m[%s] ⚠️  %s\033[0m\n' "$(date +%H:%M:%S)" "$*"; }
fail() { printf '\033[31m[错误]\033[0m %s\n' "$*" >&2; exit 1; }

usage() {
    sed -n '27,35p' "${BASH_SOURCE[0]}"
}

[ -x "$UCRT_BIN/cmake.exe" ] || fail "找不到 cmake：$UCRT_BIN/cmake.exe"
[ -x "$UCRT_BIN/ninja.exe" ] || fail "找不到 ninja：$UCRT_BIN/ninja.exe"

# 版本号：从 kc/version.h 取（与 set-version.js 同一落点）
VER=$(grep -oP 'kVersionString = "\K[0-9.]+' "$SRC_DIR/kc/version.h" | head -1)
[ -n "$VER" ] || fail "读不到版本号（kc/version.h）"

CLEAN=0
DO_UPX=1          # 2026-10-16 起默认产出 UPX 单文件；--no-upx 关闭
DO_7Z=0
DO_SFX=0
QT_MODE="auto"    # auto | std | min
SUFFIX=""
JOBS="${KC_JOBS:-$(nproc 2>/dev/null || echo 12)}"

while [ $# -gt 0 ]; do
    case "$1" in
        --clean) CLEAN=1 ;;
        --upx) DO_UPX=1 ;;
        --no-upx) DO_UPX=0 ;;
        --7z) DO_7Z=1 ;;
        --sfx) DO_SFX=1 ;;
        --std-qt) QT_MODE="std" ;;
        --min-qt) QT_MODE="min" ;;
        --suffix)
            [ $# -ge 2 ] || fail "--suffix 需要一个参数（例如 --suffix min）"
            SUFFIX="$2"; shift ;;
        --suffix=*) SUFFIX="${1#--suffix=}" ;;
        -h|--help) usage; exit 0 ;;
        *) echo "未知参数：$1" >&2; echo "用 --help 查看用法" >&2; exit 2 ;;
    esac
    shift
done
if [ -n "$SUFFIX" ]; then
    case "$SUFFIX" in
        *[!A-Za-z0-9._-]*) fail "--suffix 只允许字母/数字/点/下划线/连字符：$SUFFIX" ;;
    esac
fi

# ── Qt 模式解析 ──────────────────────────────────────────────────────────────
MIN_QT_CONFIG="$MIN_QT_PREFIX/lib/cmake/Qt6/Qt6Config.cmake"
if [ "$QT_MODE" = "auto" ]; then
    if [ -f "$MIN_QT_CONFIG" ]; then
        QT_MODE="min"
        log "[模式] 自动探测：发现精简 Qt 前缀 $MIN_QT_PREFIX → 使用 **min** 构建（体积约 −35%）"
        log "       如需旧的 msys2 qt6-static，请显式加 --std-qt"
    else
        QT_MODE="std"
        log "[模式] 自动探测：未发现 $MIN_QT_PREFIX → 回落 **std**（$STD_QT_PREFIX）"
    fi
elif [ "$QT_MODE" = "min" ]; then
    [ -f "$MIN_QT_CONFIG" ] || fail "强制 --min-qt，但找不到精简 Qt：$MIN_QT_CONFIG"
    log "[模式] 强制 min（$MIN_QT_PREFIX）"
else
    log "[模式] 强制 std（$STD_QT_PREFIX）"
fi

if [ "$QT_MODE" = "min" ]; then
    BUILD_DIR="$SRC_DIR/build-static-min"
else
    BUILD_DIR="$SRC_DIR/build-static"
fi
BTYPE_LABEL="$([ "$QT_MODE" = "min" ] && echo '精简 Qt' || echo '标准静态 Qt')"

# ── 产物命名（--suffix 一律带后缀，绝不覆盖已发布底本）──────────────────────
if [ -n "$SUFFIX" ]; then
    PKG_BASE="KunClient-$VER-$SUFFIX"
else
    PKG_BASE="KunClient-$VER"
fi
EXE_VER="$DIST_DIR/$PKG_BASE.exe"
EXE_UPX="$DIST_DIR/$PKG_BASE-upx.exe"
EXE_7Z="$DIST_DIR/$PKG_BASE.7z"
EXE_SFX="$DIST_DIR/$PKG_BASE-sfx.exe"

START_TS=$(date +%s)
# 🔴 必须显著打印用的是哪套 Qt：默认会自动选精简 Qt，产物体积差 35%，
#    以后有人对不上体积时，第一眼就该在这里看到答案。
if [ "$QT_MODE" = "min" ]; then
    log "[Qt] 精简 Qt  $MIN_QT_PREFIX   （去除 OpenGL/Vulkan/OpenSSL/SQL/PrintSupport/PDF/Movie）"
else
    log "[Qt] 系统 qt6-static  $STD_QT_PREFIX"
fi
log "构建 v$VER ｜ $BTYPE_LABEL ｜ 构建目录 $(basename "$BUILD_DIR") ｜ 产物前缀 $PKG_BASE"

if [ "$CLEAN" = "1" ]; then
    if [ -e "$BUILD_DIR" ]; then
        # 🔴 不用 rm -rf 删大目录：本机有批量删除守卫，会假失败。改名留档不触发。
        OLD="${BUILD_DIR}.old-$(date +%m%d-%H%M%S)"
        log "[0/5] --clean：$BUILD_DIR → $OLD"
        mv "$BUILD_DIR" "$OLD"
    else
        log "[0/5] --clean：$BUILD_DIR 不存在，跳过"
    fi
fi

# 缓存前缀校验：模式错了会让 CMake 复用旧前缀直接报错，这里先给出可读原因。
if [ -f "$BUILD_DIR/CMakeCache.txt" ]; then
    CACHED_PREFIX=$(grep -m1 '^CMAKE_PREFIX_PATH' "$BUILD_DIR/CMakeCache.txt" | cut -d= -f2- || true)
    WANT_PREFIX="$([ "$QT_MODE" = "min" ] && echo "$MIN_QT_PREFIX" || echo "$STD_QT_PREFIX")"
    if [ -n "$CACHED_PREFIX" ] && [ "$CACHED_PREFIX" != "$WANT_PREFIX" ]; then
        fail "构建目录已有缓存且 Qt 前缀不一致（缓存：$CACHED_PREFIX / 期望：$WANT_PREFIX）→ 请加 --clean 重跑"
    fi
fi

log "[1/5] CMake 配置（$BTYPE_LABEL）+ 构建"
if [ "$QT_MODE" = "min" ]; then
    # 精简 Qt 不能用 preset（preset 里 CMAKE_PREFIX_PATH 写死了 msys2 的 qt6-static），
    # 必须显式给 -DCMAKE_PREFIX_PATH，并把 msys2 的两个 Qt 前缀 ignore 掉，
    # 否则 find_package(Qt6) 会先命中 msys2 的旧前缀、静默链回 42MB 那套。
    CMAKE_ARGS=(
        -S "$SRC_DIR" -B "$BUILD_DIR" -G Ninja
        -DCMAKE_BUILD_TYPE=Release
        -DCMAKE_C_COMPILER="$UCRT_BIN/gcc.exe"
        -DCMAKE_CXX_COMPILER="$UCRT_BIN/c++.exe"
        -DCMAKE_MAKE_PROGRAM="$UCRT_BIN/ninja.exe"
        -DCMAKE_C_FLAGS="-pipe"
        -DCMAKE_CXX_FLAGS="-pipe"
        -DCMAKE_PREFIX_PATH="$MIN_QT_PREFIX"
        -DCMAKE_IGNORE_PREFIX_PATH="$STD_QT_PREFIX;$MSYS_ROOT/mingw64"
        -DKC_FORCE_STATIC_DEPS=ON
        -DKC_SIZE_OPT=ON
        -DKC_DROP_OPENSSL=ON
        -DBUILD_TESTING=OFF
    )
    # 可选：编译器启动器（某些沙箱会剥掉子进程 TMP/TEMP，导致 GCC 报
    # `Cannot create temporary file in C:\\WINDOWS`）。本机正常环境不需要，
    # 需要时用 KC_CC_LAUNCHER 指定，形如 "python.exe;wrapper.py"。
    if [ -n "${KC_CC_LAUNCHER:-}" ]; then
        CMAKE_ARGS+=(
            -DCMAKE_C_COMPILER_LAUNCHER="$KC_CC_LAUNCHER"
            -DCMAKE_CXX_COMPILER_LAUNCHER="$KC_CC_LAUNCHER"
            -DCMAKE_RC_COMPILER_LAUNCHER="$KC_CC_LAUNCHER"
            -DCMAKE_C_LINKER_LAUNCHER="$KC_CC_LAUNCHER"
            -DCMAKE_CXX_LINKER_LAUNCHER="$KC_CC_LAUNCHER"
        )
        log "  编译器启动器：$KC_CC_LAUNCHER"
    else
        # 显式清空：老构建目录可能缓存着某个环境专用的启动器（例如带绝对路径的
        # python wrapper），不清掉会一直被 ninja 继承，换台机器就编不动。
        CMAKE_ARGS+=(
            -DCMAKE_C_COMPILER_LAUNCHER=
            -DCMAKE_CXX_COMPILER_LAUNCHER=
            -DCMAKE_RC_COMPILER_LAUNCHER=
            -DCMAKE_C_LINKER_LAUNCHER=
            -DCMAKE_CXX_LINKER_LAUNCHER=
        )
    fi
    "$UCRT_BIN/cmake.exe" "${CMAKE_ARGS[@]}" || fail "CMake 配置失败"
else
    # 标准模式保持原行为走 preset（preset 的 binaryDir 就是 build-static）
    ( cd "$SRC_DIR" && "$UCRT_BIN/cmake.exe" --preset ucrt64-static-release ) || fail "CMake 配置失败"
fi

# 🔴 直接调 ninja 而不是 `cmake --build`：后者被掐断时可能假报 RC=0，
#    要用「产物存在 + 新鲜度」来判定成功。
"$UCRT_BIN/ninja.exe" -C "$BUILD_DIR" -j "$JOBS" || fail "ninja 构建失败"

SRC_EXE="$BUILD_DIR/KunClient.exe"
[ -f "$SRC_EXE" ] || fail "构建结束但找不到 $SRC_EXE"
BEFORE=$(stat -c%s "$SRC_EXE")
log "构建产物：$BEFORE 字节（$(( BEFORE / 1024 / 1024 )) MB）"

log "[2/5] strip 调试符号"
# 🔴 **必须 strip 副本，不能原地 strip 构建目录里的 exe**：
#    strip 不是幂等操作（2026-10-24 实测：同一个已 strip 的 PE，再 strip 一次还会改
#    DOS stub 的 3 个字节 + PE checksum 才收敛）。而 ninja 增量构建「no work to do」
#    时不会重新产出未 strip 的 exe ⇒ 原地 strip 会让「同一条命令连跑两次」得到不同
#    md5，复核时会被误判成产物不稳定。改成「拷一份 → strip 副本 → 部署副本」，
#    构建目录里始终保留 linker 原始输出。
# 🔴 strip 默认会把 PE TimeDateStamp 改成「当前时间」⇒ 每次构建 md5 都变
#    （2026-10-24 实测：同一份 linker 输出，隔 3 秒 strip 两次，md5 不同，
#    差异只在 0x88 时间戳 + 0xD8 checksum）。给 strip 一个**固定常量**的
#    SOURCE_DATE_EPOCH，才能做到「同一份源码 → 同一个 md5」。
#    ⚠️ 注意别用「linker 自己的 TimeDateStamp」当 SDE：那个值本身随构建时间变，
#    实测反而让两次构建 md5 不同（SDE 必须与构建时间无关才叫可复现）。
#    代价：产物 PE 时间戳变成 1970-01-01（功能/版本资源都不受影响，已实测
#    FileVersion 仍为 2.0.6.4、--netcheck rc=0）。要真实时间戳就 KC_REPRODUCIBLE=0。
if [ "${KC_REPRODUCIBLE:-1}" = "1" ]; then
    export SOURCE_DATE_EPOCH="${SOURCE_DATE_EPOCH:-0}"
    log "  strip 时间戳固定为 SOURCE_DATE_EPOCH=$SOURCE_DATE_EPOCH（可复现构建；KC_REPRODUCIBLE=0 关闭）"
fi
SRC_STRIPPED="$BUILD_DIR/KunClient.stripped.exe"
rm -f "$SRC_STRIPPED"
cp -f "$SRC_EXE" "$SRC_STRIPPED"
"$UCRT_BIN/strip.exe" --strip-all "$SRC_STRIPPED" || fail "strip 失败"
AFTER=$(stat -c%s "$SRC_STRIPPED")
if [ "$AFTER" -lt "$BEFORE" ]; then
    log "体积：$BEFORE → $AFTER 字节（减少 $(( (BEFORE - AFTER) * 100 / BEFORE ))%）"
elif [ "$AFTER" -eq "$BEFORE" ]; then
    log "体积：$AFTER 字节（linker 输出已无调试符号，strip 前后一致）"
else
    fail "strip 后体积反而变大（$BEFORE → $AFTER），构建环境异常"
fi

log "[3/5] 部署 → dist-static/"
mkdir -p "$DIST_DIR"
cp -f "$SRC_STRIPPED" "$EXE_VER"
if [ -z "$SUFFIX" ]; then
    # 只有无后缀（常规流程）才更新裸名 KunClient.exe；带 --suffix 时不动它，
    # 避免一个「min 变体」把 dist-static/KunClient.exe 悄悄换掉。
    cp -f "$SRC_STRIPPED" "$DIST_DIR/KunClient.exe" 2>/dev/null \
        || warn "KunClient.exe 被占用（正在运行？），只写带版本号副本"
else
    log "  带 --suffix，跳过裸名 KunClient.exe（不覆盖既有别名）"
fi
# 🔴 不用 cmp：MSYS2 的 coreutils 包里没有 cmp.exe（2026-10-24 实测），
#    这行曾让整个构建在最后一步假失败。md5sum 一定在，用它比对。
[ "$(md5sum "$SRC_STRIPPED" | cut -d' ' -f1)" = "$(md5sum "$EXE_VER" | cut -d' ' -f1)" ] \
    || fail "部署校验失败：$EXE_VER 与 strip 产物 md5 不一致"
log "  已写 $(basename "$EXE_VER")（$AFTER 字节）"

if [ "$DO_UPX" = "1" ]; then
    [ -x "$UCRT_BIN/upx.exe" ] || fail "找不到 upx：pacman -S mingw-w64-ucrt-x86_64-upx"
    log "  UPX --best --lzma → $(basename "$EXE_UPX")"
    # 🔴 两条红线叠在一起：
    #   1) UPX 遇到已存在的目标文件会直接跳过（退出码非 0），而它的输出又被我们过滤掉
    #      → dist-static 里躺着上一版旧包，md5 却报"三方一致"。必须先删再压。
    #   2) UPX 是**直接在目标路径上写**的，中途被打断/被杀会留下几百字节的半成品空壳
    #      （2026-10-24 实测：dist-static 里出现过 128 字节的 -min-upx.exe）。
    #      这种残骸一旦被 release --packed 选中上传，用户下到的就是坏 exe。
    #      ⇒ 压到**构建目录的临时文件**，校验（体积 > 1MB 且 upx -t 通过）后再 mv 进 dist。
    rm -f "$EXE_UPX"
    UPX_TMP="$BUILD_DIR/upx-package.tmp.exe"
    rm -f "$UPX_TMP"
    UPX_LOG="$BUILD_DIR/upx-package.log"
    if ! "$UCRT_BIN/upx.exe" --best --lzma -o "$UPX_TMP" "$SRC_STRIPPED" >"$UPX_LOG" 2>&1; then
        tail -20 "$UPX_LOG" >&2 || true
        rm -f "$UPX_TMP"
        fail "UPX 压缩失败（日志：$UPX_LOG）"
    fi
    grep -E -- "->|Packed" "$UPX_LOG" | sed 's/^/    /' || true
    UPX_BYTES=$(stat -c%s "$UPX_TMP")
    if [ "$UPX_BYTES" -lt 1048576 ]; then
        rm -f "$UPX_TMP"
        fail "UPX 产物异常（只有 $UPX_BYTES 字节，疑似半成品），已删除，不落 dist"
    fi
    "$UCRT_BIN/upx.exe" -t "$UPX_TMP" >/dev/null 2>&1 || { rm -f "$UPX_TMP"; fail "UPX 产物自检（upx -t）失败"; }
    mv -f "$UPX_TMP" "$EXE_UPX" || fail "UPX 产物落盘失败"
    log "  UPX 自检通过：$UPX_BYTES 字节（$(basename "$EXE_UPX")）"
    log "  ⚠️ UPX 壳在 360/火绒等杀软上有误报风险，用作分发前先过一遍查杀"
fi

if [ "$DO_7Z" = "1" ]; then
    [ -x "$SEVENZIP" ] || fail "找不到 7-Zip：$SEVENZIP"
    log "  7z -mx=9 → $(basename "$EXE_7Z")"
    # 🔴 与 UPX 同一条红线：目标文件已存在时，7z 的 `a` 是「往旧归档里加/换成员」，
    #    归档头（时间戳等）可能不变，肉眼看着"压过了"其实压的是上一版内容。
    #    必须先删再压，保证 dist-static 里的 7z 与同轮 exe 严格对应。
    #    同样先压到构建目录的临时文件，校验后再 mv，避免留下半截归档。
    # 🔴 必须 -mtm=off -mtc=off -mta=off：7z 默认把源文件的 mtime/ctime/atime 写进
    #    归档头，而部署出来的 exe 每次都是「刚 cp 的现在时间」⇒ 同一份源码连跑两次，
    #    exe 的 md5 一样、7z 的 md5 却不一样（2026-10-24 实测：6ba455… vs b40d13…）。
    #    关掉这三种时间戳后，归档内容只由「文件名 + 数据」决定，可复现。
    rm -f "$EXE_7Z"
    SEVENZIP_TMP="$BUILD_DIR/7z-package.tmp.7z"
    rm -f "$SEVENZIP_TMP"
    "$SEVENZIP" a -t7z -mx=9 -bso0 -bsp0 -mtm=off -mtc=off -mta=off "$SEVENZIP_TMP" "$EXE_VER" >/dev/null \
        || { rm -f "$SEVENZIP_TMP"; fail "7z 压缩失败"; }
    ZIP_BYTES=$(stat -c%s "$SEVENZIP_TMP")
    if [ "$ZIP_BYTES" -lt 1048576 ]; then
        rm -f "$SEVENZIP_TMP"
        fail "7z 产物异常（只有 $ZIP_BYTES 字节，疑似半成品），已删除，不落 dist"
    fi
    "$SEVENZIP" t -bso0 -bsp0 "$SEVENZIP_TMP" >/dev/null 2>&1 || { rm -f "$SEVENZIP_TMP"; fail "7z 产物自检（7z t）失败"; }
    mv -f "$SEVENZIP_TMP" "$EXE_7Z" || fail "7z 产物落盘失败"
    log "  7z 自检通过：$ZIP_BYTES 字节（$(basename "$EXE_7Z")）"
fi

if [ "$DO_SFX" = "1" ]; then
    SFX_STUB="/c/Program Files/7-Zip/7z.sfx"
    [ -f "$SFX_STUB" ] || fail "找不到 7z.sfx：$SFX_STUB"
    log "  7z SFX 自解压 → $(basename "$EXE_SFX")"
    rm -f "$EXE_SFX"
    # 🔴 载荷成员名必须是 KunClient.exe：解出来后就是可直接运行的名字（不是带版本号的那个）。
    TMPPAY="$(mktemp -d)"
    cp -f "$EXE_VER" "$TMPPAY/KunClient.exe"
    ( cd "$TMPPAY" && "$SEVENZIP" a -t7z -mx=9 -bso0 -bsp0 payload.7z KunClient.exe >/dev/null )
    cat "$SFX_STUB" "$TMPPAY/payload.7z" > "$EXE_SFX"
    rm -rf "$TMPPAY"
    log "    ⚠️ 官方 7z.sfx 不读 SFX 配置 ⇒ 只能「双击解出自解压包」，不会自动启动"
fi

log "[3.5/5] 单文件依赖闸门（导入表必须无第三方 DLL）"
# 🔴 静态包唯一的承诺是「一个文件就能跑」。一旦某次改动把某个依赖从静态 .a 退回
#    导入库（CMakeLists 的 KC_FORCE_STATIC_DEPS 被关掉、或新加的库没有 .a），
#    产物照样能构建成功，但用户在干净机器上会看到「找不到 xxx.dll」。
#    所以这里不看体积、不看能否在本机启动（本机 PATH 里有 mingw64，恒真），
#    只读 PE 导入表。
DEP_CHECK="$SRC_DIR/../../tools/check-package-deps.py"
PYTHON_BIN=""
for _cand in "$MSYS_ROOT/mingw64/bin/python.exe" "$MSYS_ROOT/usr/bin/python3.exe" python python3; do
    if command -v "$_cand" >/dev/null 2>&1; then PYTHON_BIN="$_cand"; break; fi
done
if [ -f "$DEP_CHECK" ] && [ -n "$PYTHON_BIN" ]; then
    for _f in "$EXE_VER" "$EXE_UPX"; do
        [ -f "$_f" ] || continue
        "$PYTHON_BIN" "$DEP_CHECK" "$_f" \
            || fail "$(basename "$_f") 不是真单文件（导入表里有第三方 DLL）—— 不要分发"
    done
else
    warn "找不到 tools/check-package-deps.py 或 python，跳过单文件依赖闸门"
fi

log "[4/5] 联网自检（--netcheck）"
NETCHECK_RC=0
( cd "$DIST_DIR" && env -u KC_SKIP_LEGAL ./"$PKG_BASE.exe" --netcheck 2>&1 | sed 's/^/    /' ) || NETCHECK_RC=$?
if [ "$NETCHECK_RC" -eq 0 ]; then
    log "  --netcheck rc=0"
else
    warn "--netcheck rc=$NETCHECK_RC（网络/服务端问题不阻断构建，但发布前需人工确认）"
fi

log "[5/5] 产物清单 + md5"
for f in "$EXE_VER" "$EXE_UPX" "$EXE_7Z" "$EXE_SFX"; do
    if [ -f "$f" ]; then
        printf '    %12d B  %s  %s\n' "$(stat -c%s "$f")" "$(md5sum "$f" | cut -d' ' -f1)" "$(basename "$f")"
    fi
done
ELAPSED=$(( $(date +%s) - START_TS ))
log "构建耗时：$(( ELAPSED / 60 ))分$(( ELAPSED % 60 ))秒 ｜ 模式=$QT_MODE ｜ 后缀=${SUFFIX:-（无）}"
echo "=== BUILD_STATIC_DONE ==="
