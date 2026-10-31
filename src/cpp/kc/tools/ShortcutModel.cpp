#include "kc/tools/ShortcutModel.h"

#include <QChar>

// ─────────────────────────────────────────────────────────────────────────────
//  快捷指令的实现
//
//  分三段：
//   ① 动作表（枚举 ↔ 中文名 ↔ 参数口径）—— 界面下拉、文本序列化、执行计划三处
//      共用，改动作只改这一处；
//   ② 一行文本的读写（转义 / 宽容解析 / 整表）；
//   ③ 执行编排（逐步回调、失败即中止）。
//
//  没有任何 UI / 设备依赖，可以直接喂字符串单测。
// ─────────────────────────────────────────────────────────────────────────────

namespace kc::tools {
namespace {

/// 单个字段的转义：先把换行压成空格（一行一条是硬约束），再按 escapeChars 逐个
/// 换成 `%XX`。用扫描法而不是 QString::replace 链：replace 链的顺序一旦写反，
/// `%25` 会被后续规则二次转义，出问题时很难看出来。
///   `%` 必须无条件转义：否则名字里原本就有的 "%7C" 在解析时会被还原成 "|"。
QString escapeField(const QString& text, const QString& escapeChars)
{
    QString out;
    out.reserve(text.size() + 8);
    for (const QChar ch : text) {
        if (ch == QLatin1Char('\r') || ch == QLatin1Char('\n')) {
            out += QLatin1Char(' ');
            continue;
        }
        if (ch == QLatin1Char('%')) {
            out += QStringLiteral("%25");
            continue;
        }
        if (escapeChars.contains(ch)) {
            // 这些字符全在 ASCII 段，两位十六进制足够；toUpper 后与手写格式一致。
            out += QLatin1Char('%');
            out += QStringLiteral("%1")
                       .arg(static_cast<int>(ch.unicode()), 2, 16, QLatin1Char('0'))
                       .toUpper();
            continue;
        }
        out += ch;
    }
    return out;
}

/// 单个 `%XX` 解码。只认我们自己会写出的那五个码位；其它 `%XX` 原样保留 ——
/// 用户手改配置时可能写了 `%E4` 之类的 UTF-8 片段，把它当单字节解出来只会更乱。
bool decodeEscape(QChar hi, QChar lo, QChar* out)
{
    const ushort h = hi.unicode();
    const ushort l = lo.unicode();
    const auto hexValue = [](ushort c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    const int hv = hexValue(h);
    const int lv = hexValue(l);
    if (hv < 0 || lv < 0) return false;

    const int code = hv * 16 + lv;
    switch (code) {
        case 0x25: *out = QLatin1Char('%');  return true;
        case 0x7C: *out = QLatin1Char('|');  return true;
        case 0x3B: *out = QLatin1Char(';');  return true;
        case 0x3A: *out = QLatin1Char(':');  return true;
        case 0x2C: *out = QLatin1Char(',');  return true;
        default: return false;
    }
}

/// 反转义：逐字符扫描，遇到能识别的 `%XX` 就还原，否则原样抄。
/// 🔴 不能用 `QString::replace("%25", "%")` 打头：那样 "%253B" 会被连还原两次。
QString unescapeField(const QString& text)
{
    QString out;
    out.reserve(text.size());
    for (qsizetype i = 0; i < text.size(); ++i) {
        const QChar ch = text.at(i);
        if (ch == QLatin1Char('%') && i + 2 < text.size()) {
            QChar decoded;
            if (decodeEscape(text.at(i + 1), text.at(i + 2), &decoded)) {
                out += decoded;
                i += 2;
                continue;
            }
        }
        out += ch;
    }
    return out;
}

/// 名称 / 标签的转义集：`|` 切字段、`;` 切步骤、`:` 切动作与参数。
/// 标签多一个 `,`（标签之间的分隔符）。
const QString& nameEscapeChars()
{
    static const QString chars = QStringLiteral("|;:");
    return chars;
}

const QString& tagEscapeChars()
{
    static const QString chars = QStringLiteral("|;:,");
    return chars;
}

/// 参数的转义集：`|` 会切字段、`;` 会切步骤。**不转 `:`** —— 参数里只按第一个
/// `:` 切，后面的冒号本来就属于参数值（`D:/backup` 这种 Windows 路径转成
/// `D%3A/backup` 反而没人看得懂）。
const QString& parameterEscapeChars()
{
    static const QString chars = QStringLiteral("|;");
    return chars;
}

/// 单步的「动作名[:参数]」。参数为空时只写动作名（`连接设备:;` 这种尾巴没人想看）。
QString formatStep(const ShortcutStep& step)
{
    QString text = shortcutActionName(step.action);
    if (text.isEmpty()) text = QStringLiteral("未知动作");
    if (!step.parameter.isEmpty())
        text += QLatin1Char(':') + escapeField(step.parameter, parameterEscapeChars());
    return text;
}

/// 单步的人话描述（describePlan 与 runner 的进度回调共用一份口径）。
QString describeStep(const ShortcutStep& step)
{
    const QString name = shortcutActionName(step.action);
    const QString action = name.isEmpty() ? QStringLiteral("未知动作") : name;
    const QString parameter = step.parameter.trimmed();

    if (step.action == ShortcutAction::Wait) {
        // 等待要念成「等待 5 秒」，而不是「等待 → 5」。
        bool ok = false;
        const int seconds = parameter.toInt(&ok);
        if (ok && seconds > 0) return QStringLiteral("等待 %1 秒").arg(seconds);
        return QStringLiteral("等待（秒数未填）");
    }

    if (parameter.isEmpty()) return action;
    return QStringLiteral("%1 → %2").arg(action, parameter);
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
//  ① 动作表
// ─────────────────────────────────────────────────────────────────────────────

QString shortcutActionName(ShortcutAction action)
{
    switch (action) {
        case ShortcutAction::Connect:        return QStringLiteral("连接设备");
        case ShortcutAction::ReadGpt:        return QStringLiteral("读取 GPT 分区表");
        case ShortcutAction::BackupAll:      return QStringLiteral("回读全部分区");
        case ShortcutAction::FlashXml:       return QStringLiteral("XML 刷机");
        case ShortcutAction::VerifyChecksum: return QStringLiteral("校验文件摘要");
        case ShortcutAction::Reboot:         return QStringLiteral("重启设备");
        case ShortcutAction::Wait:           return QStringLiteral("等待");
    }
    return QString();   // 未知值：调用方按「未知动作」处理
}

bool shortcutActionFromName(const QString& name, ShortcutAction* out)
{
    const QString key = name.trimmed();
    if (key.isEmpty()) return false;
    for (const ShortcutAction action : allShortcutActions()) {
        if (shortcutActionName(action) == key) {
            if (out) *out = action;
            return true;
        }
    }
    return false;
}

QVector<ShortcutAction> allShortcutActions()
{
    // 顺序 = 枚举顺序，界面下拉直接按这个顺序排；新增动作只改这一处。
    return {
        ShortcutAction::Connect,
        ShortcutAction::ReadGpt,
        ShortcutAction::BackupAll,
        ShortcutAction::FlashXml,
        ShortcutAction::VerifyChecksum,
        ShortcutAction::Reboot,
        ShortcutAction::Wait,
    };
}

bool shortcutActionNeedsParameter(ShortcutAction action)
{
    switch (action) {
        case ShortcutAction::BackupAll:
        case ShortcutAction::FlashXml:
        case ShortcutAction::VerifyChecksum:
        case ShortcutAction::Reboot:
        case ShortcutAction::Wait:
            return true;
        case ShortcutAction::Connect:
        case ShortcutAction::ReadGpt:
            return false;
    }
    return false;
}

QString shortcutActionParameterHint(ShortcutAction action)
{
    switch (action) {
        case ShortcutAction::Connect:        return QString();
        case ShortcutAction::ReadGpt:        return QString();
        case ShortcutAction::BackupAll:      return QStringLiteral("输出目录");
        case ShortcutAction::FlashXml:       return QStringLiteral("XML 路径");
        case ShortcutAction::VerifyChecksum: return QStringLiteral("文件路径");
        case ShortcutAction::Reboot:         return QStringLiteral("system / edl");
        case ShortcutAction::Wait:           return QStringLiteral("秒数");
    }
    return QString();
}

// ─────────────────────────────────────────────────────────────────────────────
//  ② 一行文本的读写
// ─────────────────────────────────────────────────────────────────────────────

QString formatShortcut(const Shortcut& shortcut)
{
    // 名称与标签先转义再拼：不转的话名字里一个 `|` 就会把整行切成四段。
    const QString name = escapeField(shortcut.name, nameEscapeChars());

    QStringList tags;
    tags.reserve(shortcut.tags.size());
    for (const QString& tag : shortcut.tags) {
        const QString clean = escapeField(tag, tagEscapeChars()).trimmed();
        if (!clean.isEmpty()) tags.push_back(clean);   // 空标签没有信息量，不写进去
    }

    QStringList steps;
    steps.reserve(shortcut.steps.size());
    for (const ShortcutStep& step : shortcut.steps) steps.push_back(formatStep(step));

    // 字段之间固定 ` | `（带空格），步骤之间固定 `; `：人眼看得出结构，split 也稳。
    return QStringLiteral("%1 | %2 | %3")
        .arg(name, tags.join(QLatin1Char(',')), steps.join(QStringLiteral("; ")));
}

bool parseShortcut(const QString& line, Shortcut* out)
{
    QString text = line;
    if (text.endsWith(QLatin1Char('\r'))) text.chop(1);
    if (text.trimmed().isEmpty()) return false;

    // 字段按 `|` 切。名称 / 标签里的 `|` 已被转义，正常数据永远是 3 段。
    const QStringList fields = text.split(QLatin1Char('|'));
    if (fields.size() < 2) return false;

    Shortcut parsed;
    parsed.name = unescapeField(fields.at(0).trimmed());

    QString stepsField;
    if (fields.size() == 2) {
        // 宽容：标签段整段省略也认（`名称 | 步骤…`）。
        stepsField = fields.at(1);
    } else {
        const QString tagsField = fields.at(1);
        for (const QString& rawTag : tagsField.split(QLatin1Char(','))) {
            const QString tag = unescapeField(rawTag.trimmed()).trimmed();
            if (!tag.isEmpty()) parsed.tags.push_back(tag);
        }
        // 手改文件里步骤段出现未转义的 `|` 时，剩下的段并回去当参数的一部分，
        // 别让一条手滑的配置把整条指令判死。
        stepsField = fields.mid(2).join(QLatin1Char('|'));
    }

    const QString stepsText = stepsField.trimmed();
    if (stepsText.isEmpty()) return false;   // 没有步骤的指令没有意义

    for (const QString& rawStep : stepsText.split(QLatin1Char(';'))) {
        const QString piece = rawStep.trimmed();
        if (piece.isEmpty()) continue;   // `;;` 之类的空档直接跳过

        // 只按**第一个** `:` 切：参数值里的冒号（`D:/backup`）属于参数本身。
        const qsizetype colon = piece.indexOf(QLatin1Char(':'));
        const QString actionName = colon < 0 ? piece : piece.left(colon).trimmed();
        const QString parameter =
            colon < 0 ? QString() : unescapeField(piece.mid(colon + 1).trimmed());

        ShortcutAction action = ShortcutAction::Connect;
        if (!shortcutActionFromName(actionName, &action)) return false;   // 认不出的动作名

        ShortcutStep step;
        step.action = action;
        step.parameter = parameter;
        parsed.steps.push_back(step);
    }

    if (parsed.steps.isEmpty()) return false;
    if (out) *out = parsed;
    return true;
}

QString formatShortcutList(const QVector<Shortcut>& shortcuts)
{
    QStringList lines;
    lines.reserve(shortcuts.size());
    for (const Shortcut& shortcut : shortcuts) lines.push_back(formatShortcut(shortcut));
    return lines.join(QLatin1Char('\n'));
}

QVector<Shortcut> parseShortcutList(const QString& text)
{
    QVector<Shortcut> shortcuts;
    const QStringList lines = text.split(QLatin1Char('\n'));
    shortcuts.reserve(lines.size());
    for (const QString& rawLine : lines) {
        QString line = rawLine;
        if (line.endsWith(QLatin1Char('\r'))) line.chop(1);
        line = line.trimmed();
        // 空行与 `#` 注释行跳过 —— 这份配置是给人手改的，注释是刚需。
        if (line.isEmpty() || line.startsWith(QLatin1Char('#'))) continue;

        Shortcut shortcut;
        // 坏一行只丢这一行：一条手滑的配置不该把用户其余的指令全带走。
        if (parseShortcut(line, &shortcut)) shortcuts.push_back(shortcut);
    }
    return shortcuts;
}

// ─────────────────────────────────────────────────────────────────────────────
//  ③ 校验 / 执行计划 / 执行器
// ─────────────────────────────────────────────────────────────────────────────

bool validateShortcut(const Shortcut& shortcut, QString* error)
{
    if (error) error->clear();

    if (shortcut.name.trimmed().isEmpty()) {
        if (error) *error = QStringLiteral("指令名称不能为空");
        return false;
    }
    if (shortcut.steps.isEmpty()) {
        if (error) *error = QStringLiteral("至少需要一个步骤");
        return false;
    }
    for (const ShortcutStep& step : shortcut.steps) {
        // 动作名取不到 = 枚举值被写坏了（正常路径不可能发生，但别让它静默变成
        // 「连接设备」跑出去）。
        if (shortcutActionName(step.action).isEmpty()) {
            if (error) *error = QStringLiteral("有步骤的动作无法识别");
            return false;
        }
    }
    return true;
}

QStringList describePlan(const Shortcut& shortcut)
{
    QStringList plan;
    plan.reserve(shortcut.steps.size());
    for (qsizetype i = 0; i < shortcut.steps.size(); ++i) {
        // 序号从 1 开始：这是给人看的「第几步」，不是下标。
        plan.push_back(QStringLiteral("%1. %2").arg(i + 1).arg(describeStep(shortcut.steps.at(i))));
    }
    return plan;
}

ShortcutRunResult ShortcutRunner::run(const Shortcut& shortcut)
{
    ShortcutRunResult result;

    // 没有处理器 = 页面忘了注入。宁可报错也不能「假装跑完了」——
    // 用户会以为备份已经做好。
    if (!m_step) {
        result.error = QStringLiteral("没有注入步骤处理器，无法执行");
        return result;
    }
    if (shortcut.steps.isEmpty()) {
        result.error = QStringLiteral("这条指令没有步骤");
        return result;
    }

    const int total = static_cast<int>(shortcut.steps.size());
    for (int i = 0; i < total; ++i) {
        const ShortcutStep& step = shortcut.steps.at(i);
        const QString description = describeStep(step);

        // 进度在**动作开始前**报：界面据此把这一行标成「进行中」。
        if (m_progress) m_progress(i, total, description);

        QString stepError;
        if (!m_step(step, &stepError)) {
            // 🔴 失败即中止，不继续后面的步骤 —— 刷机流程里「读 GPT 失败还继续刷」
            //    是灾难。completedSteps 就是「已经安全完成的步数」。
            result.ok = false;
            result.completedSteps = i;
            result.failedStep = description;
            result.error = stepError.trimmed().isEmpty()
                               ? QStringLiteral("步骤失败：%1").arg(description)
                               : stepError;
            return result;
        }
        result.completedSteps = i + 1;
    }

    result.ok = true;
    return result;
}

} // namespace kc::tools
