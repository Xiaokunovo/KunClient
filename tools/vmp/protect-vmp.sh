#!/usr/bin/env bash
# ─────────────────────────────────────────────────────────────────────────────
#  protect-vmp.sh — 用 VMProtect 商业壳保护 KunClient.exe（"壳使用 vmp"）
#
#  契约（由用户在 VMProtect GUI 中一次性准备）：
#    1) 安装合法授权的 VMProtect（必须含 VMProtect_Con.exe 控制台）。
#    2) 打开 <repo>/src/cpp/dist/KunClient.exe，配置保护项（见同目录 README.md），
#       保存工程为 <repo>/tools/vmp/KunClient.vmp：
#           InputFile  = <repo>/src/cpp/dist/KunClient.exe
#           OutputFile = <repo>/src/cpp/dist/KunClient.vmp.exe
#    3) 把 VMProtect_Con.exe 放到 <repo>/tools/vmp/（或设环境变量 VMP_BIN 覆盖）。
#
#  行为：
#    · 仅当 VMProtect_Con.exe 与 KunClient.vmp 同时存在时才启用；否则跳过，构建不变。
#    · 成功后把 dist/KunClient.vmp.exe 覆盖回 dist/KunClient.exe（或就地覆盖）。
#    · VMProtect 报错（授权/工程问题）→ 非零退出，构建中止（不静默发未保护包）。
#
#  用法（一般不必手动调，build-release.sh 会自动调用）：
#    VMP_DIST=<dist目录> protect-vmp.sh
# ─────────────────────────────────────────────────────────────────────────────
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
VMP_DIR="${VMP_DIR:-$SCRIPT_DIR}"
VMP_BIN="${VMP_BIN:-$VMP_DIR/VMProtect_Con.exe}"
VMP_PROJECT="$VMP_DIR/KunClient.vmp"

# dist 目录：优先用环境变量 VMP_DIST，否则回退到 <repo>/src/cpp/dist
if [ -n "${VMP_DIST:-}" ]; then
    DIST_DIR="$VMP_DIST"
else
    DIST_DIR="$(cd "$SCRIPT_DIR/../../src/cpp" && pwd)/dist"
fi

log()  { printf '\033[36m[%s]\033[0m %s\n' "$(date +%H:%M:%S)" "$*" >&2; }
fail() { printf '\033[31m[VMP 错误]\033[0m %s\n' "$*" >&2; exit 1; }

[ -f "$DIST_DIR/KunClient.exe" ] || fail "找不到待保护的 exe：$DIST_DIR/KunClient.exe"

if [ ! -x "$VMP_BIN" ]; then
    log "跳过：未找到 VMProtect_Con.exe（$VMP_BIN）。壳保护未启用，构建按原样继续。"
    exit 0
fi
if [ ! -f "$VMP_PROJECT" ]; then
    log "跳过：未找到工程 $VMP_PROJECT。请用 VMProtect GUI 生成后重试。"
    exit 0
fi

# 防重入：清掉上次可能残留的输出文件
rm -f "$DIST_DIR/KunClient.vmp.exe"

log "VMProtect 商业壳：保护 $DIST_DIR/KunClient.exe"
"$VMP_BIN" "$VMP_PROJECT"
RC=$?
[ $RC -eq 0 ] || fail "VMProtect_Con 返回非零（$RC），未生成保护产物，构建中止。"

OUT="$DIST_DIR/KunClient.vmp.exe"
if [ -f "$OUT" ]; then
    mv -f "$OUT" "$DIST_DIR/KunClient.exe" || fail "覆盖回写 KunClient.exe 失败"
    log "已用 VMProtect 保护产物覆盖 dist/KunClient.exe"
else
    # 兼容：用户把 OutputFile 设成与 InputFile 相同时，VMProtect 会就地覆盖
    log "未生成独立输出文件，假定 VMProtect 已就地覆盖输入（Input==Output）。"
fi

# 净化 VMProtect 可能留下的临时文件
rm -f "$DIST_DIR"/*.vmp.tmp "$DIST_DIR"/*.vmp.bak 2>/dev/null || true
exit 0
