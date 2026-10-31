#include "kc/tools/ShortcutModel.h"

#include <QString>
#include <QStringList>
#include <QVector>

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

void checkStr(const QString& got, const QString& want, const char* what)
{
    if (got != want) {
        std::cerr << "FAIL: " << what << "\n  got:  [" << got.toStdString()
                  << "]\n  want: [" << want.toStdString() << "]\n";
        ++failures;
    }
}

} // namespace

// 快捷指令的纯逻辑回归：动作表口径 / 文本序列化往返 / 校验 / 执行编排。
// 这里钉死的都是「错了会静默丢配置或静默跳过步骤」的点：
//   · 名字里带 `|` `;` `:` `%` 时能否原样读回（不转义就会把一行切成多段）；
//   · 失败即中止（刷机流程里「读 GPT 失败还继续刷」是灾难）；
//   · 坏一行不该把整份配置带崩（与 FlashHistory 的 JSONL 同一套宽容口径）。
int main()
{
    using namespace kc::tools;

    // ── ① 动作表：中文名 ↔ 枚举双向一致，且每个动作都有名字 ───────────────
    {
        const QVector<ShortcutAction> all = allShortcutActions();
        check(!all.isEmpty(), "action table is not empty");
        for (ShortcutAction a : all) {
            const QString name = shortcutActionName(a);
            check(!name.isEmpty(), "every action has a display name");

            ShortcutAction back = ShortcutAction::Connect;
            check(shortcutActionFromName(name, &back), "display name round-trips");
            check(back == a, "display name maps back to the same action");
        }
        // 认不出的名字必须返回 false 且不改 *out（界面靠这个判断「配置里写了脏数据」）。
        ShortcutAction keep = ShortcutAction::Reboot;
        check(!shortcutActionFromName(QStringLiteral("不存在动作"), &keep),
              "unknown action name is rejected");
        check(keep == ShortcutAction::Reboot, "rejected name does not clobber out");
    }

    // ── ② 需要参数的动作与提示文案一致 ────────────────────────────────────
    {
        // 不需要参数：连接 / 读 GPT。需要：备份 / 刷机 / 校验 / 重启 / 等待。
        check(!shortcutActionNeedsParameter(ShortcutAction::Connect), "Connect takes no parameter");
        check(!shortcutActionNeedsParameter(ShortcutAction::ReadGpt), "ReadGpt takes no parameter");
        check(shortcutActionNeedsParameter(ShortcutAction::BackupAll), "BackupAll needs a parameter");
        check(shortcutActionNeedsParameter(ShortcutAction::FlashXml), "FlashXml needs a parameter");
        check(shortcutActionNeedsParameter(ShortcutAction::VerifyChecksum),
              "VerifyChecksum needs a parameter");
        check(shortcutActionNeedsParameter(ShortcutAction::Reboot), "Reboot needs a parameter");
        check(shortcutActionNeedsParameter(ShortcutAction::Wait), "Wait needs a parameter");

        // 提示文案：需要参数的非空，不需要参数的为空 —— 界面据此决定是否画输入框。
        for (ShortcutAction a : allShortcutActions()) {
            const QString hint = shortcutActionParameterHint(a);
            check(hint.isEmpty() != shortcutActionNeedsParameter(a),
                  "parameter hint presence matches needsParameter");
        }
    }

    // ── ③ 序列化往返：普通内容 ────────────────────────────────────────────
    {
        Shortcut s;
        s.name = QStringLiteral("标准备份流程");
        s.tags = QStringList{QStringLiteral("常用"), QStringLiteral("备份")};
        s.steps = {
            ShortcutStep{ShortcutAction::Connect, QString()},
            ShortcutStep{ShortcutAction::ReadGpt, QString()},
            ShortcutStep{ShortcutAction::BackupAll, QStringLiteral("D:/backup")},
        };

        const QString line = formatShortcut(s);
        Shortcut out;
        check(parseShortcut(line, &out), "formatted shortcut parses back");
        checkStr(out.name, s.name, "name round-trips");
        check(out.tags == s.tags, "tags round-trip");
        check(out.steps.size() == s.steps.size(), "step count round-trips");
        for (int i = 0; i < out.steps.size() && i < s.steps.size(); ++i) {
            check(out.steps[i].action == s.steps[i].action, "step action round-trips");
            checkStr(out.steps[i].parameter, s.steps[i].parameter, "step parameter round-trips");
        }
    }

    // ── ④ 序列化往返：分隔符与 % 必须转义 ─────────────────────────────────
    // 这一组是整套格式最容易写错的地方：少转义一个字符，存进去就读不回来了。
    {
        Shortcut s;
        s.name = QStringLiteral("a|b;c:d%e");           // 名称里的 4 种分隔符 + %
        s.tags = QStringList{QStringLiteral("x,y"), QStringLiteral("p|q")};
        s.steps = {
            ShortcutStep{ShortcutAction::FlashXml, QStringLiteral("E:/a|b;c%d.xml")},
            ShortcutStep{ShortcutAction::Wait, QStringLiteral("5")},
        };

        const QString line = formatShortcut(s);
        // 一行一条是硬约束：转义到位的话，整条记录里除了分隔用的那个 `|` 之外
        // 不应该再冒出裸的 `|` / `;` / `:`。
        check(line.count(QLatin1Char('|')) == 2, "only the two field separators stay raw");

        Shortcut out;
        check(parseShortcut(line, &out), "escaped shortcut parses back");
        checkStr(out.name, s.name, "name with separators round-trips");
        check(out.tags == s.tags, "tags with separators round-trip");
        check(out.steps.size() == 2, "escaped step count round-trips");
        checkStr(out.steps[0].parameter, s.steps[0].parameter,
                 "parameter with separators round-trips");
    }

    // ── ⑤ 序列化往返：字面量 "%7C" 不能被二次解码 ─────────────────────────
    // `%` 必须最先转义，否则名字里本来就有的 "%7C" 会被还原成 "|"。
    {
        Shortcut s;
        s.name = QStringLiteral("%7C 不是竖线");
        s.steps = {ShortcutStep{ShortcutAction::Connect, QString()}};

        Shortcut out;
        check(parseShortcut(formatShortcut(s), &out), "percent-literal shortcut parses back");
        checkStr(out.name, s.name, "literal %7C is not decoded twice");
    }

    // ── ⑥ 宽容解析：标签段可空 / 可省；无步骤视为非法 ─────────────────────
    {
        Shortcut out;
        // 标签段整个省略：`名称 | 步骤…`
        check(parseShortcut(QStringLiteral("快连 | 连接设备"), &out),
              "shortcut without tags parses");
        checkStr(out.name, QStringLiteral("快连"), "tagless name parses");
        check(out.tags.isEmpty(), "tagless shortcut has no tags");
        check(out.steps.size() == 1, "tagless shortcut keeps its step");

        // 标签段为空但保留分隔符：`名称 |  | 步骤…`
        Shortcut out2;
        check(parseShortcut(QStringLiteral("快连 |  | 连接设备"), &out2),
              "shortcut with empty tags parses");
        check(out2.tags.isEmpty(), "empty tag field yields no tags");

        // 参数里允许出现 `:`，只按第一个切（否则 D:/x 会被切成两段）。
        Shortcut out3;
        check(parseShortcut(QStringLiteral("刷机 |  | XML 刷机:D:/roms/a.xml"), &out3),
              "parameter with colon parses");
        check(out3.steps.size() == 1, "colon parameter keeps one step");
        checkStr(out3.steps[0].parameter, QStringLiteral("D:/roms/a.xml"),
                 "colon in parameter is preserved");

        // 没有步骤 → 非法（一条没有步骤的指令没有意义）。
        Shortcut out4;
        check(!parseShortcut(QStringLiteral("空指令 | 常用 |"), &out4),
              "shortcut without steps is rejected");
        // 完全没有分隔符 → 非法。
        Shortcut out5;
        check(!parseShortcut(QStringLiteral("就是一行字"), &out5),
              "line without separators is rejected");
        // 空行 → 非法。
        Shortcut out6;
        check(!parseShortcut(QString(), &out6), "empty line is rejected");
    }

    // ── ⑦ 整表往返 + 坏行静默丢弃 + 注释行跳过 ────────────────────────────
    {
        QVector<Shortcut> list;
        for (int i = 0; i < 3; ++i) {
            Shortcut s;
            s.name = QStringLiteral("指令%1").arg(i);
            s.steps = {ShortcutStep{ShortcutAction::Connect, QString()}};
            list.append(s);
        }
        const QString text = formatShortcutList(list);
        check(parseShortcutList(text).size() == 3, "shortcut list round-trips");

        // 混入注释 / 空行 / 坏行：只保留能解析的那几条。
        const QString dirty = QStringLiteral("# 这是注释\n\n指令0 | 连接设备\n坏行没有分隔符\n");
        const QVector<Shortcut> parsed = parseShortcutList(dirty);
        check(parsed.size() == 1, "dirty list keeps only parseable lines");
        if (parsed.size() == 1)
            checkStr(parsed[0].name, QStringLiteral("指令0"), "surviving line parses correctly");
    }

    // ── ⑧ 校验：名称空 / 步骤空 → 失败并给中文原因 ────────────────────────
    {
        QString err;

        Shortcut ok;
        ok.name = QStringLiteral("好指令");
        ok.steps = {ShortcutStep{ShortcutAction::Connect, QString()}};
        check(validateShortcut(ok, &err), "valid shortcut passes validation");

        Shortcut noName;
        noName.steps = {ShortcutStep{ShortcutAction::Connect, QString()}};
        check(!validateShortcut(noName, &err), "shortcut without a name is rejected");
        check(!err.isEmpty(), "rejection fills a reason");

        Shortcut noSteps;
        noSteps.name = QStringLiteral("空步骤");
        check(!validateShortcut(noSteps, &err), "shortcut without steps is rejected");

        // 空参数**不算**错：内置「标准备份流程」的 BackupAll 就是运行时再要路径。
        Shortcut emptyParam;
        emptyParam.name = QStringLiteral("备份");
        emptyParam.steps = {ShortcutStep{ShortcutAction::BackupAll, QString()}};
        check(validateShortcut(emptyParam, &err), "empty parameter is allowed");
    }

    // ── ⑨ describePlan：一步一行、带序号、带参数 ──────────────────────────
    {
        Shortcut s;
        s.name = QStringLiteral("计划");
        s.steps = {
            ShortcutStep{ShortcutAction::Connect, QString()},
            ShortcutStep{ShortcutAction::BackupAll, QStringLiteral("D:/backup")},
            ShortcutStep{ShortcutAction::Wait, QStringLiteral("5")},
        };

        const QStringList plan = describePlan(s);
        check(plan.size() == 3, "plan has one line per step");
        if (plan.size() == 3) {
            check(plan[0].startsWith(QStringLiteral("1.")), "first plan line is numbered 1");
            check(plan[0].contains(shortcutActionName(ShortcutAction::Connect)),
                  "first plan line names the action");
            // 带参数的动作要把参数带出来，否则计划表看不出「备份到哪」。
            check(plan[1].contains(QStringLiteral("D:/backup")),
                  "plan line shows the parameter");
            check(plan[2].startsWith(QStringLiteral("3.")), "third plan line is numbered 3");
        }
    }

    // ── ⑩ ShortcutRunner：按序执行 + 进度回调 + 失败即中止 ────────────────
    {
        Shortcut s;
        s.name = QStringLiteral("跑起来");
        s.steps = {
            ShortcutStep{ShortcutAction::Connect, QString()},
            ShortcutStep{ShortcutAction::ReadGpt, QString()},
            ShortcutStep{ShortcutAction::BackupAll, QStringLiteral("D:/b")},
        };

        QVector<int> seen;
        QStringList progressLines;
        ShortcutRunner runner;
        runner.setStepHandler([&seen](const ShortcutStep& step, QString*) {
            seen.append(static_cast<int>(step.action));
            return true;
        });
        runner.setProgressHandler([&progressLines](int index, int total, const QString& desc) {
            progressLines.append(QStringLiteral("%1/%2 %3").arg(index).arg(total).arg(desc));
        });

        const ShortcutRunResult r = runner.run(s);
        check(r.ok, "all-success run reports ok");
        check(r.completedSteps == 3, "all-success run completes every step");
        check(r.failedStep.isEmpty(), "all-success run has no failed step");
        check(seen.size() == 3, "every step handler ran exactly once");
        check(progressLines.size() == 3, "progress fired once per step");
        if (!progressLines.isEmpty())
            check(progressLines.first().startsWith(QStringLiteral("0/3")),
                  "progress index is 0-based and total is the step count");

        // 第二步失败：第三步**不许**执行，completedSteps 记的是安全完成的步数。
        int calls = 0;
        ShortcutRunner failing;
        failing.setStepHandler([&calls](const ShortcutStep& step, QString* err) {
            ++calls;
            if (step.action == ShortcutAction::ReadGpt) {
                if (err) *err = QStringLiteral("设备没响应");
                return false;
            }
            return true;
        });

        const ShortcutRunResult f = failing.run(s);
        check(!f.ok, "failed run reports not ok");
        check(f.completedSteps == 1, "failed run stops at the failing step");
        check(!f.failedStep.isEmpty(), "failed run names the failing step");
        checkStr(f.error, QStringLiteral("设备没响应"), "failed run surfaces the handler error");
        check(calls == 2, "steps after the failure never run");

        // 没有注入 handler 时必须报错，**不能**假装跑完了 ——
        // 否则用户会以为备份已经做好（刷机工具里这是最危险的谎）。
        ShortcutRunner bare;
        const ShortcutRunResult b = bare.run(s);
        check(!b.ok, "runner without a handler refuses to run");
        check(b.completedSteps == 0, "runner without a handler completes nothing");
        check(!b.error.isEmpty(), "runner without a handler explains why");

        // 空步骤表：同样报错（一条没有步骤的指令没有意义）。
        Shortcut empty;
        empty.name = QStringLiteral("空的");
        const ShortcutRunResult e = bare.run(empty);
        check(!e.ok, "empty shortcut is refused");
        check(e.completedSteps == 0, "empty shortcut completes zero steps");
    }

    if (failures == 0)
        std::cout << "ShortcutModelTest: all checks passed\n";
    else
        std::cerr << "ShortcutModelTest: " << failures << " check(s) failed\n";
    return failures == 0 ? 0 : 1;
}
