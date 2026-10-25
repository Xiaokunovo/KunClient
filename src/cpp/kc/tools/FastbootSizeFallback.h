#pragma once

// ─────────────────────────────────────────────────────────────────────────────
//  FastbootSizeFallback — 「引导未上报分区大小」的补偿回退链
//
//  移植自 YukiClient `Core/FastbootSizeFallback.cs`（913 行，逻辑逐条对齐）。
//
//  真机错误原文（非 A/B 单槽机型刷 boot 时）：
//      fastboot: error: cannot get boot partition size
//  原因是 bootloader 的 `getvar:partition-size:boot` 返回空，新版 fastboot 直接
//  拒绝刷写；而某些老版本（33.0.1）会跳过尺寸检查硬写，能过。
//
//  补救策略 = 把**本机所有 fastboot** 运行时枚举出来当候选，逐个试：
//    ① 当前正在用的那个（随包 fastboot.exe）
//    ② 随包旧版（tools/fastboot33.exe、tools/LegacyFb/fastboot33.exe）
//    ③ 程序目录 / 程序目录 LegacyFb
//    ④ 系统 fastboot（System32 / PATH）
//  每个候选**先带 --force 再不带**；任一次退出码 0 即成功。
//  每次尝试都把完整命令行原文写进日志，便于下次真机日志直接看出
//  「哪个二进制 + 要不要 --force」能过。
//
//  🔴 只对**这一种**错误启用：其它失败一律原样返回。给所有失败都加 --force
//     等于把尺寸保护全关掉，会把镜像写坏。
// ─────────────────────────────────────────────────────────────────────────────

#include <QString>
#include <QStringList>
#include <QVector>

#include <functional>

namespace kc {

class FastbootSizeFallback final {
public:
    struct Attempt {
        QString binary;
        QString command;       ///< 完整命令行（含 -s / --force），便于日志复盘
        bool usedForce = false;
        bool success = false;
        QString output;
    };

    struct Result {
        bool success = false;
        QString output;
        QString error;
        QString binary;        ///< 最终成功的二进制；全失败时为空
        QVector<Attempt> attempts;
    };

    /// 是否为「引导未上报分区大小」这一类错误（大小写不敏感，容错几种原文）。
    static bool isPartitionSizeUnavailable(const QString& text);

    /// 跑一条 fastboot 命令。arguments 不含 exe 名，
    /// 例如 `flash boot "D:/boot.img"` / `erase metadata`。
    /// 只有命中上面那条错误才会换二进制 / 加 --force 重试。
    static Result execute(const QString& arguments, int timeoutMs = 600000,
                          const QString& deviceSerial = QString(),
                          const std::function<void(const QString&)>& log = {});

    /// 候选二进制列表（绝对路径，已去重，按优先级排序）。
    /// 暴露出来便于单测与排障日志。
    static QStringList candidates();
};

} // namespace kc
