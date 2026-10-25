#!/usr/bin/env bash
# ─────────────────────────────────────────────────────────────────────────────
#  KunClient — Release 构建 + 部署脚本（MSYS2 MinGW64）
#
#  做什么：
#    1. 配置 CMake（Release）+ Ninja 构建 KunClient.exe
#    2. 用 ldd 求 Qt/运行库依赖闭包，连同 platforms/styles 插件拷进 dist/
#    3. strip 掉调试符号（体积约减 28%），strip 后**实测能否起来**
#    4. 加壳 / 压缩（互斥，优先级 VMP > UPX）：
#       · VMProtect 商业壳：tools/vmp/ 下齐备 VMProtect_Con.exe + KunClient.vmp 则自动加壳
#       · UPX 5.x：`--best --lzma` 压缩主程序 + dist 顶层 DLL
#         （🔴 **插件目录一律不压**：platforms 压了起不来、tls 压了 HTTPS 全废，
#          见第 5 步注释；实测：整包 89MB → 29MB，主程序 3.89MB → 1.22MB）
#    5. 打印 KunClient.exe 的 md5（已是加壳 / 压缩后产物）
#
#  为什么不用 windeployqt6：
#    本机 windeployqt6 内部 spawn `qtpaths` 必失败（管道被拦），
#    所以改为「ldd 求闭包 → 拷 DLL → 手动放 platforms 插件」。
#    判据：产物在不带 /e/msys64/mingw64/bin 的 PATH 下也能起来（脚本第 6 步实测）。
#
#  用法：
#    ./build-release.sh                 # 全量：配置 + 构建 + 部署 + strip + [VMP|UPX] + 校验
#    ./build-release.sh --no-strip      # 保留调试符号（排查崩溃用）
#    ./build-release.sh --no-upx        # 完全不做压缩（排查崩溃建议：栈回溯才是真实地址）
#    ./build-release.sh --upx-exe-only  # 只压主程序、不压 DLL（杀软误报时用这个更保守）
#    ./build-release.sh --clean         # 先删掉 build-release/ 再全量构建
# ─────────────────────────────────────────────────────────────────────────────
set -uo pipefail

# 脚本所在目录（= src/cpp）
SRC_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# ── 工具链定位 ───────────────────────────────────────────────────────────────
# ⚠️ 本机 Git Bash 里的 /mingw64 指向的是另一套 .NET 工具链目录，
#    不是 MSYS2 的 mingw64 —— 所以一律用绝对路径 /e/msys64/mingw64。
MSYS_ROOT="${MSYS_ROOT:-/e/msys64}"
MINGW_BIN="$MSYS_ROOT/mingw64/bin"
export PATH="$MINGW_BIN:/usr/bin:/bin"
export MSYSTEM=MINGW64

# 用**独立的** release 构建目录，避免和日常 debug 构建（build/）互相踩：
# 两个进程同时在一个 build/ 里跑 ninja 会撞 .ninja_lock（failed recompaction: Permission denied）。
BUILD_DIR="$SRC_DIR/build-release"
DIST_DIR="$SRC_DIR/dist"
QT_PLUGINS="$MSYS_ROOT/mingw64/share/qt6/plugins"

# VMProtect 商业壳（"壳使用 vmp"）：**必须**辅助脚本 + 控制台程序 + 工程文件三者齐备
# 才算「可用」。🔴 只判断辅助脚本存在是错的 —— protect-vmp.sh 本身是随仓库提交的，
#    它内部还会自己检查 VMProtect_Con.exe / KunClient.vmp，缺了就打印「跳过」并 exit 0。
#    只看辅助脚本的话，会误判成「VMP 可用」从而跳掉 UPX，最后**什么都没压**
#    （2026-10-16 实测踩过：构建出来仍是 89MB 未压缩）。
VMP_DIR="$(cd "$SRC_DIR/../.." && pwd)/tools/vmp"
VMP_HELPER="$VMP_DIR/protect-vmp.sh"
VMP_BIN="${VMP_BIN:-$VMP_DIR/VMProtect_Con.exe}"
VMP_PROJECT="$VMP_DIR/KunClient.vmp"
VMP_READY=0
[ -f "$VMP_HELPER" ] && [ -f "$VMP_BIN" ] && [ -f "$VMP_PROJECT" ] && VMP_READY=1

# UPX 可执行文件（MSYS2 包：pacman -S mingw-w64-x86_64-upx）
UPX_BIN="$MINGW_BIN/upx.exe"

STRIP=1
UPX=1
UPX_EXE_ONLY=0
CLEAN=0
for arg in "$@"; do
    case "$arg" in
        --no-strip)      STRIP=0 ;;
        --no-upx)        UPX=0 ;;
        --upx-exe-only)  UPX_EXE_ONLY=1 ;;
        --clean)         CLEAN=1 ;;
        -h|--help)  sed -n '2,27p' "${BASH_SOURCE[0]}"; exit 0 ;;
        *) echo "未知参数：$arg（--help 查看用法）" >&2; exit 2 ;;
    esac
done

log()  { printf '\033[36m[%s]\033[0m %s\n' "$(date +%H:%M:%S)" "$*"; }
fail() { printf '\033[31m[错误]\033[0m %s\n' "$*" >&2; exit 1; }

# ── 前置检查 ─────────────────────────────────────────────────────────────────
[ -x "$MINGW_BIN/cmake.exe" ] || fail "找不到 cmake：$MINGW_BIN/cmake.exe"
[ -x "$MINGW_BIN/ninja.exe" ] || fail "找不到 ninja：$MINGW_BIN/ninja.exe"
[ -x "$MINGW_BIN/g++.exe" ]   || fail "找不到 g++：$MINGW_BIN/g++.exe"

# ── [0] 清理 ─────────────────────────────────────────────────────────────────
if [ "$CLEAN" = "1" ]; then
    log "[0/6] --clean：删除 $BUILD_DIR"
    rm -rf "$BUILD_DIR"
fi

# ── [1] CMake 配置 + 构建 ────────────────────────────────────────────────────
log "[1/6] CMake 配置（Release）+ 构建"
cmake -S "$SRC_DIR" -B "$BUILD_DIR" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER="$MINGW_BIN/gcc.exe" \
    -DCMAKE_CXX_COMPILER="$MINGW_BIN/g++.exe" \
    -DBUILD_TESTING=ON \
    || fail "CMake 配置失败"

cmake --build "$BUILD_DIR" --target KunClient -j \
    || fail "构建失败"

SRC_EXE="$BUILD_DIR/KunClient.exe"
[ -f "$SRC_EXE" ] || fail "构建结束但找不到 $SRC_EXE"
log "构建产物：$SRC_EXE（$(stat -c%s "$SRC_EXE") 字节）"

# ── [2] 部署：ldd 求依赖闭包 ─────────────────────────────────────────────────
log "[2/6] 求依赖闭包并拷贝 Qt/运行库 DLL → dist/"
rm -rf "$DIST_DIR"
mkdir -p "$DIST_DIR"
cp -f "$SRC_EXE" "$DIST_DIR/KunClient.exe"

declare -A seen
queue=("$DIST_DIR/KunClient.exe")
copied=0
while [ ${#queue[@]} -gt 0 ]; do
    cur="${queue[0]}"; queue=("${queue[@]:1}")
    while read -r dll; do
        [ -z "$dll" ] && continue
        base=$(basename "$dll")
        [ -n "${seen[$base]:-}" ] && continue
        seen[$base]=1
        [ -f "$DIST_DIR/$base" ] && continue
        if [ -f "$MINGW_BIN/$base" ]; then
            cp -f "$MINGW_BIN/$base" "$DIST_DIR/$base"
            copied=$((copied + 1))
            queue+=("$DIST_DIR/$base")
        fi
    done < <(ldd "$cur" 2>/dev/null | awk -v b="$MINGW_BIN" '$3 ~ b"/" {print $3}')
done
log "主程序依赖闭包：拷入 $copied 个 DLL"

# ── [3] Qt 插件（platforms 必须；styles 增强外观；tls **登录必需**；imageformats 壁纸必需）──
# 🔴 tls 插件是 HTTPS 的后端：漏了它 QSslSocket::supportsSsl() 恒为 false，
#    登录/所有网络请求一律 "TLS initialization failed"（界面显示「无法连接服务器」）。
#    ldd 抓不到这一层 —— Qt 是运行时按插件目录加载 TLS 后端，不是链接期依赖。
# 🔴 imageformats 是**自定义背景**的后端（2026-10-12 加）：QtGui 只内建 png/bmp，
#    jpg/jpeg/gif/ico 全在插件里；动态版漏拷 → 用户选 jpg 壁纸一律「没反应」，
#    而且是静默失败（同 2.0.0.3 静态包漏 qt_import_plugins 的那个坑）。
log "[3/6] 拷贝 Qt 插件（platforms / styles / tls / imageformats）"
mkdir -p "$DIST_DIR/platforms" "$DIST_DIR/styles" "$DIST_DIR/tls" "$DIST_DIR/imageformats"
[ -f "$QT_PLUGINS/platforms/qwindows.dll" ] \
    || fail "找不到平台插件：$QT_PLUGINS/platforms/qwindows.dll"
cp -f "$QT_PLUGINS/platforms/qwindows.dll" "$DIST_DIR/platforms/"
[ -f "$QT_PLUGINS/styles/qmodernwindowsstyle.dll" ] \
    && cp -f "$QT_PLUGINS/styles/qmodernwindowsstyle.dll" "$DIST_DIR/styles/"
[ -d "$QT_PLUGINS/tls" ] || fail "找不到 TLS 插件目录：$QT_PLUGINS/tls"
cp -f "$QT_PLUGINS"/tls/*.dll "$DIST_DIR/tls/"
if [ -d "$QT_PLUGINS/imageformats" ]; then
    cp -f "$QT_PLUGINS"/imageformats/*.dll "$DIST_DIR/imageformats/" 2>/dev/null
    log "imageformats：$(ls "$DIST_DIR/imageformats" | wc -l) 个格式插件"
else
    log "⚠️ 找不到 $QT_PLUGINS/imageformats —— jpg/gif 壁纸将无法显示"
fi
# OpenSSL 动态库：openssl 后端运行时 LoadLibrary 它，ldd 闭包不覆盖。
for ossl in libssl-3-x64.dll libcrypto-3-x64.dll; do
    [ -f "$MINGW_BIN/$ossl" ] && cp -f "$MINGW_BIN/$ossl" "$DIST_DIR/$ossl"
done

# 插件自身也带依赖（如 Qt6Gui），再求一次闭包补齐
for p in "$DIST_DIR"/platforms/*.dll "$DIST_DIR"/styles/*.dll "$DIST_DIR"/tls/*.dll "$DIST_DIR"/imageformats/*.dll; do
    [ -f "$p" ] || continue
    while read -r dll; do
        base=$(basename "$dll")
        [ -n "${seen[$base]:-}" ] && continue
        seen[$base]=1
        if [ -f "$MINGW_BIN/$base" ]; then
            cp -f "$MINGW_BIN/$base" "$DIST_DIR/$base"
            queue+=("$DIST_DIR/$base")
        fi
    done < <(ldd "$p" 2>/dev/null | awk -v b="$MINGW_BIN" '$3 ~ b"/" {print $3}')
done

# 构建目录里的那份 exe 也放一份 tls/OpenSSL：开发态直接双击 build-release/KunClient.exe
# 时同样按 <appdir>/tls 找后端，缺了就是「无法连接服务器」（与 dist 同因）。
mkdir -p "$BUILD_DIR/tls"
cp -f "$QT_PLUGINS"/tls/*.dll "$BUILD_DIR/tls/" 2>/dev/null
for ossl in libssl-3-x64.dll libcrypto-3-x64.dll; do
    [ -f "$MINGW_BIN/$ossl" ] && cp -f "$MINGW_BIN/$ossl" "$BUILD_DIR/$ossl"
done

# ── [4] strip ────────────────────────────────────────────────────────────────
if [ "$STRIP" = "1" ]; then
    SIZE_BEFORE=$(stat -c%s "$DIST_DIR/KunClient.exe")
    log "[4/6] strip 调试符号"
    "$MINGW_BIN/strip.exe" "$DIST_DIR/KunClient.exe" \
        || fail "strip 失败"
    SIZE_AFTER=$(stat -c%s "$DIST_DIR/KunClient.exe")
    log "体积：$SIZE_BEFORE → $SIZE_AFTER 字节（减少 $(( (SIZE_BEFORE - SIZE_AFTER) * 100 / SIZE_BEFORE ))%）"
else
    log "[4/6] 跳过 strip（--no-strip）"
fi

# ── [5] 加壳 / 压缩：VMP > UPX > 不做（**三者互斥，不能叠加**）──────────────────
#  🔴 UPX 与 VMProtect 不能同时用：UPX 会整体改写 PE 结构与节区，VMP 再在其上做保护
#     会互相破坏（轻则起不来，重则产物静默损坏）。所以按「保护优先于压缩」排序：
#     VMP 齐备就用 VMP 并**跳过 UPX**；否则用 UPX。
#  ⚠️ UPX 压缩后的 exe 在部分杀软的启发式里可能被误报 —— 发布前请自行过一遍。
#  ⚠️ 排查崩溃时用 --no-upx：压缩后栈回溯看到的是 UPX 的解压桩，不是真实地址。
PACK_INFO="未加壳 / 未压缩"
if [ "$VMP_READY" = "1" ]; then
    if [ "$STRIP" = "1" ]; then
        # strip 后符号已去，VMP 按工程配置整文件保护即可
        log "[5/6] VMProtect 商业壳（VMP 优先，跳过 UPX）"
        VMP_DIST="$DIST_DIR" "$VMP_HELPER" \
            || fail "VMProtect 商业壳保护失败"
        PACK_INFO="VMProtect 商业壳"
    else
        log "[5/6] 跳过 VMProtect：--no-strip 保留符号，保护请在 strip 产物上做"
        PACK_INFO="跳过（--no-strip）"
    fi
elif [ "$UPX" = "0" ]; then
    log "[5/6] 跳过加壳 / 压缩（--no-upx，且 VMProtect 不可用）"
    PACK_INFO="跳过（--no-upx）"
elif [ "$STRIP" != "1" ]; then
    log "[5/6] 跳过 UPX：--no-strip 保留符号，压缩请在 strip 产物上做"
    PACK_INFO="跳过（--no-strip）"
elif [ ! -x "$UPX_BIN" ]; then
    log "[5/6] 跳过 UPX：未找到 $UPX_BIN（pacman -S mingw-w64-x86_64-upx 可装）"
    PACK_INFO="未加壳 / 未压缩（缺 UPX）"
else
    SIZE_BEFORE=$(stat -c%s "$DIST_DIR/KunClient.exe")
    PKG_BEFORE=$(du -sk "$DIST_DIR" | cut -f1)

    # 🔴 **插件目录一律不压**（platforms / styles / tls / imageformats）。
    #    · platforms/qwindows.dll 压了 → LoadLibrary 失败 → 进程根本起不来，
    #      症状是退出码 127、无任何错误输出，看起来像「缺 DLL」但 DLL 一个不少。
    #    · tls/*.dll 压了 → **进程能起来，但 HTTPS 全废**：插件加载失败后
    #      QSslSocket::supportsSsl() == 0，所有 /api/v2 请求变成 HTTP 0，
    #      用户看到的是登录页「签名握手失败（HTTP 0）」。2026-10-17 实测定位：
    #      dist/tls/qopensslbackend.dll 391238 → 133190 字节后 supportsSsl=0；
    #      换回未压缩副本立刻恢复（supportsSsl=1 backend=openssl）。
    #    插件总共不到 2MB，压缩收益微乎其微 —— 只压 dist 顶层的运行库。
    UPX_TARGETS=("$DIST_DIR/KunClient.exe")
    if [ "$UPX_EXE_ONLY" = "1" ]; then
        log "[5/6] UPX 压缩主程序（--upx-exe-only）"
    else
        log "[5/6] UPX 压缩主程序 + 顶层 DLL（插件目录不压）"
        while IFS= read -r f; do
            UPX_TARGETS+=("$f")
        done < <(find "$DIST_DIR" -maxdepth 1 -name '*.dll' | sort)
    fi

    "$UPX_BIN" --best --lzma --quiet "${UPX_TARGETS[@]}" \
        || fail "UPX 压缩失败"

    SIZE_AFTER=$(stat -c%s "$DIST_DIR/KunClient.exe")
    PKG_AFTER=$(du -sk "$DIST_DIR" | cut -f1)
    log "主程序：$SIZE_BEFORE → $SIZE_AFTER 字节（为原体积的 $(( SIZE_AFTER * 100 / SIZE_BEFORE ))%）"
    log "整包  ：$(( PKG_BEFORE / 1024 ))MB → $(( PKG_AFTER / 1024 ))MB（$(( PKG_AFTER * 100 / PKG_BEFORE ))%）"
    PACK_INFO="UPX --best --lzma（$(( PKG_BEFORE / 1024 ))MB → $(( PKG_AFTER / 1024 ))MB）"
fi

# ── [6] 脱环境实测 + md5 + 清单 ──────────────────────────────────────────────
log "[6/6] 脱环境实测：PATH 中剔除 mingw64，截图模式验证 exe 能起来"
CLN_PATH=$(echo "$PATH" | tr ':' '\n' \
    | grep -v '/msys64/mingw64/bin' | grep -v '^/mingw64/bin$' | paste -sd: -)
SHOT="$DIST_DIR/.preview.png"
rm -f "$SHOT"
# 🔴 必须带 KC_SKIP_LOGIN=1：启动登录墙会挡在 MainShell 之前，截图定时器
#    永远等不到主窗口 → 这一步会挂死（构建卡住几十分钟不报错）。
( cd "$DIST_DIR" && env PATH="$CLN_PATH" KC_SKIP_LEGAL=1 KC_SKIP_LOGIN=1 \
    ./KunClient.exe --screenshot "$SHOT" >"$DIST_DIR/.run.log" 2>&1 )
RUN_RC=$?
if [ -f "$SHOT" ]; then
    log "实测通过：exe 在无 mingw64 环境下正常启动（退出码 $RUN_RC）"
    rm -f "$SHOT" "$DIST_DIR/.run.log"
else
    echo "--- .run.log ---" >&2
    cat "$DIST_DIR/.run.log" 2>/dev/null >&2 || true
    fail "实测失败：exe 未能生成截图（退出码 $RUN_RC），部署可能缺 DLL 或加壳后异常"
fi

EXE="$DIST_DIR/KunClient.exe"
if command -v md5sum >/dev/null 2>&1; then
    MD5=$(md5sum "$EXE" | awk '{print $1}')
else
    MD5=$("$MINGW_BIN/../usr/bin/md5sum.exe" "$EXE" 2>/dev/null | awk '{print $1}')
fi

echo ""
echo "============================================================"
echo "  构建完成"
echo "============================================================"
echo "  EXE : $EXE"
echo "  大小: $(stat -c%s "$EXE") 字节 ($(du -h "$EXE" | cut -f1))"
echo "  加壳: $PACK_INFO"
echo "  MD5 : $MD5"
echo "  文件: $(find "$DIST_DIR" -type f | wc -l) 个，总计 $(du -sh "$DIST_DIR" | cut -f1)"
echo "============================================================"
echo ""

log "dist/ 顶层文件列表："
find "$DIST_DIR" -maxdepth 1 -type f -printf '  %f\n' | sort
log "插件："
find "$DIST_DIR/platforms" "$DIST_DIR/styles" -type f -printf '  %p\n' 2>/dev/null \
    | sed "s#$DIST_DIR/##"

echo "=== BUILD_RELEASE_DONE ==="
