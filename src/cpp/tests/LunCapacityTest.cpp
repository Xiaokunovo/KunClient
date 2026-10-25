#include "storage/LunCapacity.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QString>

#include <iostream>

namespace {

int failures = 0;

void check(bool ok, const char* what)
{
    if (!ok) {
        std::cerr << "FAIL: " << what << '\n';
        ++failures;
    }
}

} // namespace

// LUN 容量解析回归：NUM_DISK_SECTORS 表达式 + getstorageinfo 三种响应形态。
// 2026-10-13 用户报修「lun0-5 分区没数据时无法选择刷机并写入刷机包」——
// 空设备没有 GPT，容量只能来自 loader；表达式求不到值必须 fail-closed。
int main()
{
    using swt::edl::parseLunSizes;
    using swt::edl::resolveSectorExpression;

    // ── resolveSectorExpression ──────────────────────────────────────────────
    quint64 value = 0;
    // 真实固件包形态：BackupGPT start_sector="NUM_DISK_SECTORS-5." 贴 LUN 末尾
    check(resolveSectorExpression(QStringLiteral("NUM_DISK_SECTORS-5."), 30119931,
                                  &value) &&
              value == 30119926,
          "BackupGPT expression must resolve to the last 5 sectors of the LUN");
    check(resolveSectorExpression(QStringLiteral("NUM_DISK_SECTORS"), 1000, &value) &&
              value == 1000,
          "bare NUM_DISK_SECTORS resolves to the full LUN size");
    check(resolveSectorExpression(QStringLiteral("NUM_DISK_SECTORS-5"), 1000, &value) &&
              value == 995,
          "trailing dot is optional");
    check(resolveSectorExpression(QStringLiteral("NUM_DISK_SECTORS+5."), 1000, &value) &&
              value == 1005,
          "addition expression resolves");
    check(resolveSectorExpression(QStringLiteral("  NUM_DISK_SECTORS - 5.  "), 1000,
                                  &value) && value == 995,
          "whitespace around the expression is tolerated");
    // 🔴 空设备：容量未知（0）必须 fail-closed，绝不能把 0 当容量算偏移
    check(!resolveSectorExpression(QStringLiteral("NUM_DISK_SECTORS-5."), 0, &value),
          "unknown LUN capacity (0) must fail closed instead of writing sector 0");
    check(!resolveSectorExpression(QStringLiteral("NUM_DISK_SECTORS-99"), 10, &value),
          "expression that would resolve negative is rejected");
    check(!resolveSectorExpression(QStringLiteral("NUM_DISK_SECTORS*5"), 10, &value),
          "unsupported operator is rejected");
    check(!resolveSectorExpression(QStringLiteral("garbage"), 10, &value),
          "non-expression text is rejected");
    check(!resolveSectorExpression(QString(), 10, &value),
          "empty expression is rejected");

    // ── parseLunSizes ────────────────────────────────────────────────────────
    // ① lunN_size / lun_N_size 带下标字段（最可信）
    {
        QJsonObject info;
        info.insert(QStringLiteral("lun0_size"), 1000);
        info.insert(QStringLiteral("lun2_size"), 3000);
        const QHash<quint32, quint64> sizes = parseLunSizes(info, 1);
        check(sizes.size() == 2 && sizes.value(0) == 1000 && sizes.value(2) == 3000 &&
                  !sizes.contains(1),
              "indexed lunN_size fields map by their own index, not the query LUN");
    }
    {
        QJsonObject info;
        info.insert(QStringLiteral("lun_1_size"), 2000);
        const QHash<quint32, quint64> sizes = parseLunSizes(info, 0);
        check(sizes.size() == 1 && sizes.value(1) == 2000,
              "lun_N_size underscore form is recognized");
    }
    {
        QJsonObject info;
        info.insert(QStringLiteral("lun1_size"), 2000);
        info.insert(QStringLiteral("total_blocks"), 100);
        info.insert(QStringLiteral("block_size"), 4096);
        const QHash<quint32, quint64> sizes = parseLunSizes(info, 1);
        check(sizes.size() == 1 && sizes.value(1) == 2000,
              "indexed fields take priority over total_blocks");
    }
    // ② total_blocks × block_size：标量，归属本次查询的 LUN
    {
        QJsonObject info;
        info.insert(QStringLiteral("total_blocks"), 7340032);
        info.insert(QStringLiteral("block_size"), 4096);
        const QHash<quint32, quint64> sizes = parseLunSizes(info, 2);
        check(sizes.size() == 1 && sizes.value(2) == 7340032ULL * 4096ULL,
              "total_blocks x block_size is attributed to the queried LUN");
        check(parseLunSizes(info, -1).isEmpty(),
              "scalar total_blocks without a query LUN is not attributed to anything");
    }
    {
        QJsonObject info;
        info.insert(QStringLiteral("total_blocks"), 9007199254740992.0); // 2^53
        info.insert(QStringLiteral("block_size"), 4096);
        check(parseLunSizes(info, 0).isEmpty(),
              "overflowing total_blocks x block_size is dropped");
    }
    // ③ lun_size：数组按下标，标量归属查询 LUN
    {
        QJsonObject info;
        info.insert(QStringLiteral("lun_size"), QJsonArray{11, 22, 33});
        const QHash<quint32, quint64> sizes = parseLunSizes(info, -1);
        check(sizes.size() == 3 && sizes.value(0) == 11 && sizes.value(1) == 22 &&
                  sizes.value(2) == 33,
              "lun_size array maps by index without needing a query LUN");
    }
    {
        QJsonObject info;
        info.insert(QStringLiteral("lun_size"), 5000);
        check(parseLunSizes(info, 3).value(3) == 5000,
              "scalar lun_size is attributed to the query LUN");
        check(parseLunSizes(info, -1).isEmpty(),
              "scalar lun_size without a query LUN is not attributed to anything");
    }
    // 0 / 非数字 / 空值一律不收
    {
        QJsonObject info;
        info.insert(QStringLiteral("lun0_size"), 0);
        info.insert(QStringLiteral("lun1_size"), QStringLiteral("not-a-number"));
        check(parseLunSizes(info, 0).isEmpty(),
              "zero and non-numeric values are ignored");
    }
    {
        QJsonObject info;
        info.insert(QStringLiteral("lun1_size"), QStringLiteral("0x2710"));
        check(parseLunSizes(info, 1).value(1) == 10000,
              "hex string lun size is parsed");
    }
    check(parseLunSizes(QJsonObject{}, 0).isEmpty(),
          "empty storage_info yields nothing");

    if (failures != 0) {
        std::cerr << failures << " LunCapacity assertion(s) failed\n";
        return 1;
    }
    std::cout << "LUN capacity resolution tests passed\n";
    return 0;
}
