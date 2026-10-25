#include "ui/MainWindow.h"
#include "storage/RawProgramParser.h"

#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QLineEdit>
#include <QMenu>
#include <QPushButton>
#include <QStandardItemModel>
#include <QTableWidget>
#include <QTemporaryDir>

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

QPushButton* findButtonByText(const QWidget& root, const QString& text)
{
    for (QPushButton* button : root.findChildren<QPushButton*>()) {
        if (button->text() == text) return button;
    }
    return nullptr;
}

// 左卡页签（objectName = "cardTab"）与顶部导航里的「高级功能」按钮同名，
// 必须按 objectName 过滤，否则 findButtonByText 会抓到被禁用的导航按钮。
QPushButton* findCardTab(const QWidget& root, const QString& text)
{
    for (QPushButton* button : root.findChildren<QPushButton*>()) {
        if (button->objectName() == QStringLiteral("cardTab") &&
            button->text() == text) {
            return button;
        }
    }
    return nullptr;
}

// 「高级功能」入口：2026-10-19 起是页签条右侧的**下拉选择框**
// （objectName = "advancedCombo"），不再是 quickLink 按钮 + 弹窗。
QComboBox* findCombo(const QWidget& root, const QString& objectName)
{
    for (QComboBox* combo : root.findChildren<QComboBox*>()) {
        if (combo->objectName() == objectName) return combo;
    }
    return nullptr;
}

/// 下拉里是否有一条文本匹配、且 data 非空的**动作条目**。
bool hasAction(const QComboBox& combo, const QString& text, QString* idOut = nullptr)
{
    for (int i = 0; i < combo.count(); ++i) {
        if (combo.itemText(i) != text) continue;
        const QString id = combo.itemData(i).toString();
        if (id.isEmpty()) return false;
        if (idOut) *idOut = id;
        return true;
    }
    return false;
}

} // namespace

// 造一个可解析的 rawprogram XML：单条 program，8 扇区 × 4096 字节。
bool writeRawprogramXml(const QString& path, const QString& label, const QString& image)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) return false;
    const QString text = QStringLiteral(
        "<?xml version=\"1.0\" ?>\n<data>\n"
        "<program SECTOR_SIZE_IN_BYTES=\"4096\" physical_partition_number=\"0\" "
        "start_sector=\"0\" num_partition_sectors=\"8\" filename=\"%1\" label=\"%2\"/>\n"
        "</data>\n").arg(image, label);
    return file.write(text.toUtf8()) == text.toUtf8().size();
}

// EDL 高级功能下拉 + 重启菜单的 UI 结构回归（竞品 9 项功能补齐的界面侧）：
//  · 左卡只剩「刷机界面」一个页签；「高级功能」2026-10-19 起是**下拉选择框**
//    （objectName = advancedCombo），旧弹窗 advancedToolsDialog 必须已删除 ——
//    那层 application-modal 弹窗会把里面按钮打开的确认框压在后面看不见；
//  · 下拉内七项维护操作齐全、LUN 选择器 6 档、未连接时动作条目全部置灰（fail-closed）；
//  · 重启菜单四项（常规 2 + BCB 目标 2）。
// 不依赖 show()/isVisibleTo —— 沿用 MainWindowLogTest.cpp 既有的
// 「objectName + property + flags」断言范式。
int main(int argc, char* argv[])
{
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);

    swt::edl::MainWindow window;

    // ── 左卡：唯一页签「刷机界面」，起始 active ────────────────────────────
    QPushButton* flashTab = findCardTab(window, QStringLiteral("刷机界面"));
    if (!flashTab) {
        std::cerr << "flash card tab was not created\n";
        return 1;
    }
    check(flashTab->property("active").toBool(), "flash tab starts active");

    // ── 「高级功能」下拉（2026-10-19：弹窗 → 下拉选择框）───────────────────
    // 用户口径：「把 edl 界面高级功能改成下拉式的选择框，点完这种独立弹窗的按钮
    // 后面就有页面挡住不显示」。所以这里断言的是**下拉**而不是按钮 + 弹窗。
    QComboBox* advancedCombo = findCombo(window, QStringLiteral("advancedCombo"));
    if (!advancedCombo) {
        std::cerr << "advanced tools combo was not created\n";
        return 2;
    }
    check(advancedCombo->currentIndex() == 0,
          "advanced combo shows its placeholder item by default");
    check(advancedCombo->itemText(0).startsWith(QStringLiteral("高级功能")),
          "placeholder item reads 高级功能");
    check(window.findChild<QDialog*>(QStringLiteral("advancedToolsDialog")) == nullptr,
          "the old advanced tools dialog must be gone (it hid the confirm dialogs)");

    // ── 下拉里的七项维护操作（「重启到 Recovery / FastbootD」2026-10-16 已移入
    //    右下角重启菜单，不在下拉里，见下方菜单断言）────────────────────────
    const QStringList advancedControls{
        QStringLiteral("恢复出厂"),
        QStringLiteral("强开 OEM 解锁"),
        QStringLiteral("备份基带分区"),
        QStringLiteral("读取存储信息"),
        QStringLiteral("擦除磁盘 LUN"),
        QStringLiteral("读取 GPT 文件"),
        QStringLiteral("写入 GPT 文件"),
    };
    for (const QString& text : advancedControls) {
        check(hasAction(*advancedCombo, text),
              qPrintable(QStringLiteral("advanced action exists: %1").arg(text)));
    }

    // LUN 选择器：UFS 6 档，数据位与显示一致，默认 LUN 0（下拉在页签条上）。
    QComboBox* lunCombo = findCombo(window, QStringLiteral("lunCombo"));
    if (!lunCombo) {
        std::cerr << "LUN selector was not created\n";
        return 4;
    }
    check(lunCombo->count() == 6 && lunCombo->currentData().toUInt() == 0,
          "LUN selector offers LUN 0..5, default 0");

    // 未连接时动作条目全部置灰（fail-closed：没有会话不许发命令）。
    // 🔴 下拉条目没有 isEnabled() —— 状态在 QStandardItem 的 flags 上。
    if (auto* model = qobject_cast<QStandardItemModel*>(advancedCombo->model())) {
        for (const QString& text : advancedControls) {
            bool found = false;
            for (int i = 0; i < advancedCombo->count(); ++i) {
                if (advancedCombo->itemText(i) != text) continue;
                QStandardItem* item = model->item(i);
                found = true;
                check(item && !(item->flags() & Qt::ItemIsEnabled),
                      qPrintable(QStringLiteral("action disabled while disconnected: %1")
                                     .arg(text)));
            }
            check(found, qPrintable(QStringLiteral("action row found: %1").arg(text)));
        }
    } else {
        check(false, "advanced combo uses a QStandardItemModel");
    }
    check(lunCombo->isEnabled() == false, "LUN selector disabled while disconnected");

    // 🔴 FDL 2026-10-17 已从这里**移出**：它是一条完整的入口流程，藏在「重启设备」
    //    菜单里既不好找、语义上也和「重启」不搭。现在挂在 EDL 页快捷条的独立按钮上
    //    （`EdlFlashPage` 的 `#quickBar`，见 `--page EdlFlash` 截图取证）。
    //    本 fixture 建的是 swt::edl::MainWindow（内嵌 EDL 窗），`quickBar` 属于外壳
    //    的 `EdlFlashPage`、不在这棵树里，所以这里只断言菜单少了一项。
    QMenu* rebootMenu = window.findChild<QMenu*>(QStringLiteral("rebootMenu"));
    if (!rebootMenu) {
        std::cerr << "reboot menu was not created\n";
        return 5;
    }
    const QStringList expectedActions{
        QStringLiteral("重启到系统"),
        QStringLiteral("重启到 9008"),
        QStringLiteral("重启到 Recovery"),
        QStringLiteral("重启到 FastbootD"),
    };
    QStringList actualActions;
    for (QAction* action : rebootMenu->actions()) {
        if (!action->text().isEmpty()) actualActions.push_back(action->text());
    }
    check(actualActions == expectedActions,
          qPrintable(QStringLiteral("reboot menu has the four reboot targets, got: %1")
                         .arg(actualActions.join(QLatin1Char('|')))));

    // ── 标准 + 非标准 XML 混选（2026-10-17）────────────────────────────────
    // 之前两套方案互斥（validateXmlCategory 直接拒），现在允许同时选，但必须
    // ① 文件对话框默认过滤器能看到全部 rawprogram*.xml；
    // ② 载入后表里两套条目都在；
    // ③ 类别显示为「标准 + 非标准 XML」。
    QTemporaryDir mixDir;
    check(mixDir.isValid(), "temp dir for xml mix");
    if (mixDir.isValid()) {
        const QString imageA = mixDir.filePath(QStringLiteral("boot.img"));
        const QString imageB = mixDir.filePath(QStringLiteral("boot_unsparse.img"));
        QFile fileA(imageA);
        QFile fileB(imageB);
        check(fileA.open(QIODevice::WriteOnly) && fileA.write(QByteArray(8 * 4096, 'A')) == 8 * 4096,
              "fixture image A");
        check(fileB.open(QIODevice::WriteOnly) && fileB.write(QByteArray(8 * 4096, 'B')) == 8 * 4096,
              "fixture image B");
        fileA.close();
        fileB.close();

        const QString standardXml = mixDir.filePath(QStringLiteral("rawprogram0.xml"));
        const QString nonstandardXml = mixDir.filePath(QStringLiteral("rawprogram_unsparse0.xml"));
        check(writeRawprogramXml(standardXml, QStringLiteral("boot"), QStringLiteral("boot.img")),
              "fixture standard xml");
        check(writeRawprogramXml(nonstandardXml, QStringLiteral("boot"),
                                 QStringLiteral("boot_unsparse.img")),
              "fixture nonstandard xml");

        // 先离线确认 fixture 本身可解析：解析失败时 loadFirmwareXmlPaths 会弹模态框，
        // 无头测试会挂死，所以这一步必须挡在前面。
        swt::edl::RawProgramPlan fixturePlan;
        QString fixtureError;
        const bool fixtureOk = swt::edl::RawProgramParser::parse(
            {standardXml, nonstandardXml}, &fixturePlan, &fixtureError);
        check(fixtureOk, qPrintable(QStringLiteral("mixed fixture parses: %1").arg(fixtureError)));
        if (fixtureOk) {
            window.loadFirmwareXmlPaths({standardXml, nonstandardXml});
            for (int round = 0; round < 10; ++round) app.processEvents();

            auto* table = window.findChild<QTableWidget*>(QStringLiteral("partitionTable"));
            check(table && table->rowCount() == 2,
                  qPrintable(QStringLiteral("mixed selection loads both entries, rows=%1")
                                 .arg(table ? table->rowCount() : -1)));
            bool labelled = false;
            for (QLineEdit* edit : window.findChildren<QLineEdit*>()) {
                if (edit->text().contains(QStringLiteral("标准 + 非标准 XML"))) labelled = true;
            }
            check(labelled, "mixed selection shows 标准 + 非标准 XML in the firmware field");
        }
    }

    // ── 缺索引的非标准名（rawprogram_ufs.xml）也要算进「非标准」─────────────
    // 分类若要求末尾必须带索引，这类包会被判成 Other，混选检测整块失效。
    QTemporaryDir mixDir2;
    check(mixDir2.isValid(), "temp dir for index-less xml mix");
    if (mixDir2.isValid()) {
        const QString imageC = mixDir2.filePath(QStringLiteral("modem.img"));
        QFile fileC(imageC);
        check(fileC.open(QIODevice::WriteOnly) && fileC.write(QByteArray(8 * 4096, 'C')) == 8 * 4096,
              "fixture image C");
        fileC.close();
        const QString standardXml2 = mixDir2.filePath(QStringLiteral("rawprogram0.xml"));
        const QString indexlessXml = mixDir2.filePath(QStringLiteral("rawprogram_ufs.xml"));
        check(writeRawprogramXml(standardXml2, QStringLiteral("boot"), QStringLiteral("modem.img")),
              "fixture standard xml (index-less case)");
        check(writeRawprogramXml(indexlessXml, QStringLiteral("modem"), QStringLiteral("modem.img")),
              "fixture index-less nonstandard xml");

        swt::edl::RawProgramPlan plan2;
        QString error2;
        const bool ok2 = swt::edl::RawProgramParser::parse({standardXml2, indexlessXml}, &plan2, &error2);
        check(ok2, qPrintable(QStringLiteral("index-less fixture parses: %1").arg(error2)));
        if (ok2) {
            window.loadFirmwareXmlPaths({standardXml2, indexlessXml});
            for (int round = 0; round < 10; ++round) app.processEvents();
            bool labelled = false;
            for (QLineEdit* edit : window.findChildren<QLineEdit*>()) {
                if (edit->text().contains(QStringLiteral("标准 + 非标准 XML"))) labelled = true;
            }
            check(labelled, "index-less rawprogram_*.xml still counts as non-standard (mixed)");
        }
    }

    // ── 「自动选择」按钮 + 包目录自动发现全链路 ────────────────────────────
    // 按钮本身会弹目录选择框（无头测试点不了），所以这里分两段：
    // ① 断言按钮存在且空闲时可用；② 直接走它内部的 discover → loadFirmwareXmlPaths。
    QPushButton* autoXmlButton = findButtonByText(window, QStringLiteral("自动选择"));
    check(autoXmlButton != nullptr, "auto-select xml button exists");
    check(autoXmlButton && autoXmlButton->isEnabled(), "auto-select button enabled while idle");

    QTemporaryDir packageDir;
    check(packageDir.isValid(), "temp dir for auto discovery");
    if (packageDir.isValid()) {
        QDir root(packageDir.path());
        check(root.mkpath(QStringLiteral("IMAGES")), "package IMAGES dir");
        const QString images = root.filePath(QStringLiteral("IMAGES"));
        const QString imageD = QDir(images).filePath(QStringLiteral("boot.img"));
        QFile fileD(imageD);
        check(fileD.open(QIODevice::WriteOnly) && fileD.write(QByteArray(8 * 4096, 'D')) == 8 * 4096,
              "fixture image D");
        fileD.close();
        check(writeRawprogramXml(QDir(images).filePath(QStringLiteral("rawprogram0.xml")),
                                 QStringLiteral("boot"), QStringLiteral("boot.img")),
              "package standard xml");
        check(writeRawprogramXml(QDir(images).filePath(QStringLiteral("rawprogram_unsparse0.xml")),
                                 QStringLiteral("boot"), QStringLiteral("boot.img")),
              "package nonstandard xml");

        QString usedDirectory;
        QString discoverError;
        const QStringList discovered =
            swt::edl::RawProgramParser::discover(packageDir.path(), &usedDirectory, &discoverError);
        check(discovered.size() == 2,
              qPrintable(QStringLiteral("auto discovery finds both schemes, got %1 (%2)")
                             .arg(discovered.size()).arg(discoverError)));
        if (discovered.size() == 2) {
            window.loadFirmwareXmlPaths(discovered);
            for (int round = 0; round < 10; ++round) app.processEvents();
            auto* table = window.findChild<QTableWidget*>(QStringLiteral("partitionTable"));
            check(table && table->rowCount() == 2,
                  qPrintable(QStringLiteral("auto selection loads both entries, rows=%1")
                                 .arg(table ? table->rowCount() : -1)));
            bool labelled = false;
            for (QLineEdit* edit : window.findChildren<QLineEdit*>()) {
                if (edit->text().contains(QStringLiteral("标准 + 非标准 XML"))) labelled = true;
            }
            check(labelled, "auto selection reports 标准 + 非标准 XML");
        }
    }

    if (failures == 0) {
        std::cout << "maintenance-ui-tests: all checks passed\n";
        return 0;
    }
    std::cerr << "maintenance-ui-tests: " << failures << " check(s) failed\n";
    return 1;
}
