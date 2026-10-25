#pragma once

#include <QHash>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>

namespace swt::edl {

/// 求值 rawprogram XML 里的 `NUM_DISK_SECTORS` 起始扇区表达式
/// （如 `"NUM_DISK_SECTORS-5."` → 该 LUN 总扇区数 − 5，QFIL 注释约定带不带
/// 尾点语义不同，求值前统一 TrimEnd('.')）。
///
/// 🔴 `diskSectors == 0`（LUN 容量未知 —— 典型场景：设备为空 / 分区表不可读 /
/// loader 不返回 lun_size）时**必须返回 false**：调用方据此 fail-closed 中止。
/// 绝不允许把 0 当成「容量就是 0」继续算偏移 —— 那等于把镜像写到扇区 0，
/// 直接覆盖分区表（2026-10-13 空设备刷机包排障的现实入口）。
bool resolveSectorExpression(const QString& expression, quint64 diskSectors,
                             quint64* value);

/// 从 `getstorageinfo` 响应的 `storage_info` JSON 对象里提取**各 LUN 的字节容量**。
///
/// 真实 loader 有三种形态（按可信度排序，与 QFIL 口径一致）：
///  ① `lunN_size` / `lun_N_size` 带下标字段（最可信，天然按 LUN 归属）；
///  ② `total_blocks` × `block_size`（标量，归属本次查询的 `queryLun`）；
///  ③ `lun_size` 数组（按下标）或标量（归属 `queryLun`）。
///
/// `queryLun < 0` 表示「不知道在问哪个 LUN」，此时只收 ① 与 ③-数组，
/// **不收任何标量形态** —— 无法确定归属的值宁可没有，也不能猜错 LUN。
/// 0 / 非数字 / 溢出的值一律丢弃。
QHash<quint32, quint64> parseLunSizes(const QJsonObject& storageInfo, int queryLun);

} // namespace swt::edl
