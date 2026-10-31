// 批量刷机队列的状态机回归（2026-10-31）。
//
// 这个模块最容易出的错是「状态迁移悄悄放行」和「一台失败拖死整批」，所以断言
// 集中在这两块：
//   · 失败**不阻塞**后续 —— 这是与快捷指令「失败即中止」刻意相反的口径；
//   · 非法迁移一律返回 false 且**不改状态**（UI 的按钮可用性靠它）；
//   · 入队去重 / 空序列号拒绝 / 运行中不可删。

#include "kc/tools/FlashQueue.h"

#include <QString>
#include <QVector>

#include <iostream>
#include <string>

namespace {

int g_failures = 0;

void expect(bool ok, const char* what, const std::string& detail = {})
{
    if (ok) {
        std::cout << "PASS " << what << '\n';
    } else {
        ++g_failures;
        std::cerr << "FAIL " << what << (detail.empty() ? "" : ": " + detail) << '\n';
    }
}

kc::tools::QueueItem makeItem(const QString& serial, const QString& package = QStringLiteral("umi.xml"))
{
    kc::tools::QueueItem item;
    item.serial = serial;
    item.model = QStringLiteral("小米 23127PN0CC");
    item.packagePath = package;
    return item;
}

/// 造一个 n 台设备的队列（COM1..COMn）。
kc::tools::FlashQueue makeQueue(int count)
{
    kc::tools::FlashQueue queue;
    for (int i = 1; i <= count; ++i) {
        queue.add(makeItem(QStringLiteral("SERIAL%1").arg(i)));
    }
    return queue;
}

} // namespace

int main()
{
    using namespace kc::tools;

    // ── ① 状态名双向映射 ────────────────────────────────────────────────
    {
        const QVector<QueueState> all{ QueueState::Pending, QueueState::Running,
                                       QueueState::Done, QueueState::Failed,
                                       QueueState::Skipped };
        for (QueueState state : all) {
            const QString name = queueStateName(state);
            expect(!name.isEmpty(), "state-name-not-empty");
            QueueState back = QueueState::Done;
            expect(queueStateFromName(name, &back), "state-name-roundtrip",
                   name.toStdString());
            expect(back == state, "state-name-roundtrip-same");
        }
        // 英文名也认（日志 / 配置用）。
        QueueState en = QueueState::Done;
        expect(queueStateFromName(QStringLiteral("running"), &en) && en == QueueState::Running,
               "state-name-english");
        // 认不出返回 false 且不改 out。
        QueueState keep = QueueState::Skipped;
        expect(!queueStateFromName(QStringLiteral("不存在的状态"), &keep),
               "state-name-unknown-rejected");
        expect(keep == QueueState::Skipped, "state-name-unknown-keeps-out");
        expect(!queueStateFromName(QStringLiteral("pending"), nullptr),
               "state-name-null-out");
    }

    // ── ② 入队：空序列号 / 重复序列号拒绝 ───────────────────────────────
    {
        FlashQueue queue;
        expect(queue.isEmpty(), "queue-starts-empty");
        expect(queue.add(makeItem(QStringLiteral("SERIAL1"))), "add-first");
        expect(queue.size() == 1, "add-size", std::to_string(queue.size()));

        // 重复序列号：同一台机器排两次会互相覆盖结果。
        expect(!queue.add(makeItem(QStringLiteral("SERIAL1"))), "add-duplicate-rejected");
        expect(queue.size() == 1, "add-duplicate-no-growth", std::to_string(queue.size()));

        // 空 / 纯空白序列号：定位不到机器。
        expect(!queue.add(makeItem(QString())), "add-empty-serial-rejected");
        expect(!queue.add(makeItem(QStringLiteral("   "))), "add-blank-serial-rejected");

        // 外部塞一个 Done 状态进来，入队后必须被压回 Pending。
        QueueItem sneaky = makeItem(QStringLiteral("SERIAL2"));
        sneaky.state = QueueState::Done;
        sneaky.message = QStringLiteral("旧消息");
        expect(queue.add(sneaky), "add-forces-pending");
        QueueItem stored;
        expect(queue.item(QStringLiteral("SERIAL2"), &stored), "add-item-readable");
        expect(stored.state == QueueState::Pending, "add-state-reset-to-pending");
        expect(stored.message.isEmpty(), "add-message-cleared");
    }

    // ── ③ next()：只取 Pending，且取完置 Running ────────────────────────
    {
        FlashQueue queue = makeQueue(3);
        expect(queue.nextIndex() == 0, "next-index-first");

        QueueItem out;
        expect(queue.next(&out), "next-ok");
        expect(out.serial == QStringLiteral("SERIAL1"), "next-first-serial",
               out.serial.toStdString());
        expect(out.packagePath == QStringLiteral("umi.xml"), "next-carries-package");

        QueueItem after;
        expect(queue.item(QStringLiteral("SERIAL1"), &after), "next-item-readable");
        expect(after.state == QueueState::Running, "next-sets-running");
        expect(queue.nextIndex() == 1, "next-index-advances",
               std::to_string(queue.nextIndex()));

        // 空队列取不到。
        FlashQueue empty;
        expect(!empty.next(nullptr), "next-empty-returns-false");
        expect(empty.nextIndex() == -1, "next-empty-index-minus-one");
    }

    // ── ④ 🔴 失败不阻塞：一台失败，后面的照刷 ──────────────────────────
    {
        FlashQueue queue = makeQueue(4);

        // 第 1 台成功
        expect(queue.next(nullptr), "seq-first");
        expect(queue.markDone(QStringLiteral("SERIAL1"), QStringLiteral("写入完成")),
               "seq-first-done");

        // 第 2 台失败 —— 关键：不能因此让 SERIAL3/4 取不到
        expect(queue.next(nullptr), "seq-second");
        expect(queue.markFailed(QStringLiteral("SERIAL2"), QStringLiteral("传输中断")),
               "seq-second-failed");
        expect(queue.hasPending(), "failed-does-not-block-pending");
        expect(queue.nextIndex() == 2, "failed-does-not-advance-index",
               std::to_string(queue.nextIndex()));

        // 第 3、4 台继续正常跑完
        expect(queue.next(nullptr), "seq-third-after-failure");
        expect(queue.markDone(QStringLiteral("SERIAL3")), "seq-third-done");
        expect(queue.next(nullptr), "seq-fourth-after-failure");
        expect(queue.markDone(QStringLiteral("SERIAL4")), "seq-fourth-done");
        expect(!queue.hasPending(), "seq-no-more-pending");
        expect(!queue.next(nullptr), "seq-next-returns-false-at-end");

        const QueueSummary s = queue.summary();
        expect(s.total == 4, "seq-summary-total", std::to_string(s.total));
        expect(s.done == 3, "seq-summary-done", std::to_string(s.done));
        expect(s.failed == 1, "seq-summary-failed", std::to_string(s.failed));
        expect(s.skipped == 0, "seq-summary-skipped", std::to_string(s.skipped));
        expect(s.total == s.done + s.failed + s.skipped, "seq-summary-sums");

        // 失败原因被记下来（现场要看到为什么挂）。
        QueueItem failedItem;
        expect(queue.item(QStringLiteral("SERIAL2"), &failedItem), "seq-failed-readable");
        expect(failedItem.message == QStringLiteral("传输中断"), "seq-failed-message",
               failedItem.message.toStdString());
    }

    // ── ⑤ 非法迁移：返回 false 且状态不变 ───────────────────────────────
    {
        FlashQueue queue = makeQueue(2);

        // Pending 不能直接 Done / Failed（没经过 Running = 没真的刷）。
        expect(!queue.markDone(QStringLiteral("SERIAL1")), "reject-pending-to-done");
        expect(!queue.markFailed(QStringLiteral("SERIAL1"), QStringLiteral("x")),
               "reject-pending-to-failed");
        QueueItem check;
        expect(queue.item(QStringLiteral("SERIAL1"), &check), "reject-item-readable");
        expect(check.state == QueueState::Pending, "reject-state-unchanged");

        // Pending 可以 skip。
        expect(queue.skip(QStringLiteral("SERIAL1")), "allow-pending-to-skipped");
        expect(queue.item(QStringLiteral("SERIAL1"), &check), "skip-item-readable");
        expect(check.state == QueueState::Skipped, "skip-sets-skipped");

        // 终态不能再 Done/Failed/Skip（只能 reset）。
        expect(!queue.markDone(QStringLiteral("SERIAL1")), "reject-skipped-to-done");
        expect(!queue.markFailed(QStringLiteral("SERIAL1"), QStringLiteral("x")),
               "reject-skipped-to-failed");
        expect(!queue.skip(QStringLiteral("SERIAL1")), "reject-skipped-to-skipped");

        // 不存在的序列号：全部返回 false，不崩。
        expect(!queue.markDone(QStringLiteral("没有这个")), "reject-unknown-serial-done");
        expect(!queue.markRunning(QStringLiteral("没有这个")), "reject-unknown-serial-running");
        expect(!queue.remove(QStringLiteral("没有这个")), "reject-unknown-serial-remove");
        expect(queue.indexOf(QStringLiteral("没有这个")) == -1, "index-of-unknown-minus-one");

        // 运行中不能跳过、不能删除（worker 的回报会找不到落点）。
        expect(queue.next(nullptr), "running-setup-next");
        expect(!queue.skip(QStringLiteral("SERIAL2")), "reject-running-to-skipped");
        expect(!queue.remove(QStringLiteral("SERIAL2")), "reject-remove-running");
        expect(queue.size() == 2, "reject-remove-running-keeps-size",
               std::to_string(queue.size()));
    }

    // ── ⑥ reset / resetOne：重试用 ──────────────────────────────────────
    {
        FlashQueue queue = makeQueue(3);
        queue.next(nullptr);                                        // SERIAL1 → Running
        queue.markFailed(QStringLiteral("SERIAL1"), QStringLiteral("失败"));
        queue.next(nullptr);                                        // SERIAL2 → Running
        queue.markDone(QStringLiteral("SERIAL2"));

        QueueSummary before = queue.summary();
        expect(before.done == 1 && before.failed == 1, "reset-setup");

        // 单项重试：只把 SERIAL1 拉回 Pending，SERIAL2 保持 Done。
        expect(queue.resetOne(QStringLiteral("SERIAL1")), "reset-one-ok");
        QueueItem item;
        expect(queue.item(QStringLiteral("SERIAL1"), &item), "reset-one-readable");
        expect(item.state == QueueState::Pending, "reset-one-to-pending");
        expect(item.message.isEmpty(), "reset-one-clears-message");
        expect(queue.item(QStringLiteral("SERIAL2"), &item), "reset-one-other-readable");
        expect(item.state == QueueState::Done, "reset-one-leaves-others");

        // 整批重置：全部回 Pending。
        queue.reset();
        for (const QueueItem& each : queue.items()) {
            expect(each.state == QueueState::Pending, "reset-all-pending");
            expect(each.message.isEmpty(), "reset-all-clears-message");
        }
        const QueueSummary after = queue.summary();
        expect(after.total == 3 && after.done == 0 && after.failed == 0
                   && after.skipped == 0,
               "reset-all-summary");
        expect(queue.nextIndex() == 0, "reset-all-next-index-zero");
    }

    // ── ⑦ 汇总：Running 与 Pending 不算进任何一列，但算进 total ──────────
    {
        FlashQueue queue = makeQueue(4);
        queue.next(nullptr);                                        // 1 → Running
        queue.markDone(QStringLiteral("SERIAL1"));
        queue.next(nullptr);                                        // 2 → Running
        queue.markFailed(QStringLiteral("SERIAL2"), QStringLiteral("x"));
        queue.skip(QStringLiteral("SERIAL3"));                      // 3 → Skipped
        // 4 还是 Pending

        const QueueSummary s = queue.summary();
        expect(s.total == 4, "summary-total", std::to_string(s.total));
        expect(s.done == 1, "summary-done", std::to_string(s.done));
        expect(s.failed == 1, "summary-failed", std::to_string(s.failed));
        expect(s.skipped == 1, "summary-skipped", std::to_string(s.skipped));
        expect(s.done + s.failed + s.skipped == 3, "summary-running-pending-excluded",
               std::to_string(s.done + s.failed + s.skipped));

        // 空队列的汇总全是 0。
        const QueueSummary empty = FlashQueue().summary();
        expect(empty.total == 0 && empty.done == 0 && empty.failed == 0
                   && empty.skipped == 0,
               "summary-empty-all-zero");
    }

    // ── ⑧ remove / clear ───────────────────────────────────────────────
    {
        FlashQueue queue = makeQueue(3);
        expect(queue.remove(QStringLiteral("SERIAL2")), "remove-ok");
        expect(queue.size() == 2, "remove-size", std::to_string(queue.size()));
        expect(queue.indexOf(QStringLiteral("SERIAL2")) == -1, "remove-gone");
        // 顺序保持（删中间那个，剩下的还是 1、3）。
        expect(queue.items().at(0).serial == QStringLiteral("SERIAL1"), "remove-keeps-order-0");
        expect(queue.items().at(1).serial == QStringLiteral("SERIAL3"), "remove-keeps-order-1");

        queue.clear();
        expect(queue.isEmpty(), "clear-empty");
        expect(queue.size() == 0, "clear-size");
        expect(!queue.hasPending(), "clear-no-pending");
    }

    // ── ⑨ item() 找不到时不改 out ───────────────────────────────────────
    {
        FlashQueue queue = makeQueue(1);
        QueueItem sentinel = makeItem(QStringLiteral("不许被改"));
        sentinel.state = QueueState::Failed;
        const bool found = queue.item(QStringLiteral("不存在"), &sentinel);
        expect(!found, "item-unknown-false");
        expect(sentinel.serial == QStringLiteral("不许被改"), "item-unknown-keeps-out");
        expect(sentinel.state == QueueState::Failed, "item-unknown-keeps-state");
        expect(queue.item(QStringLiteral("SERIAL1"), nullptr), "item-null-out-still-finds");
    }

    if (g_failures == 0) {
        std::cout << "ALL PASS\n";
        return 0;
    }
    std::cerr << g_failures << " FAILURE(S)\n";
    return 1;
}
