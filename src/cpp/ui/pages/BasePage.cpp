#include "ui/pages/BasePage.h"

#include "ui/widgets/LogView.h"
#include "ui/widgets/NotificationBar.h"

#include <QCoreApplication>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QMetaObject>
#include <QPushButton>
#include <QStyle>
#include <QThread>

#include <utility>

namespace kc::ui {

BasePage::BasePage(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("basePage"));
}

BasePage::~BasePage() = default;

void BasePage::setTitle(const QString& title)
{
    if (m_title == title) return;
    m_title = title;
    emit windowTitleChanged(title);
}

bool BasePage::isOnUiThread() const
{
    // QWidget 属于创建它的线程；QThread::currentThread() 与 thread() 相同即 UI 线程。
    return QThread::currentThread() == thread();
}

void BasePage::runOnUi(std::function<void()> action)
{
    if (!action) return;
    if (isOnUiThread()) {
        // runOnUi 是槽：UI 线程调用时直接同步执行（与 C# OnUiThread 的 CheckAccess 分支一致）。
        action();
        return;
    }
    auto* self = this;
    QMetaObject::invokeMethod(
        self,
        [action = std::move(action)] { action(); },
        Qt::QueuedConnection);
}

void BasePage::onUiThread(std::function<void()> action)
{
    runOnUi(std::move(action));
}

void BasePage::notify(const QString& text, int level, int autoHideMs)
{
    if (text.isEmpty()) return;
    runOnUi([this, text, level, autoHideMs] {
        if (m_notifyBar) m_notifyBar->notify(text, static_cast<NotifyLevel>(level), autoHideMs);
    });
}

void BasePage::success(const QString& message, int autoHideMs)
{
    notify(message, static_cast<int>(NotifyLevel::Success), autoHideMs);
}

void BasePage::error(const QString& message, int autoHideMs)
{
    notify(message, static_cast<int>(NotifyLevel::Error), autoHideMs);
}

void BasePage::hideNotify()
{
    runOnUi([this] {
        // NotificationBar 没有公开的「立即隐藏」；用一条 1ms 生命周期的空档退场不优雅，
        // 这里直接把它藏起来（退场动画由下次 notify 覆盖，语义足够）。
        if (m_notifyBar) m_notifyBar->hide();
    });
}

void BasePage::logInfo(const QString& message)
{
    runOnUi([this, message] { if (m_logView) m_logView->appendInfo(message); });
}

void BasePage::logSuccess(const QString& message)
{
    runOnUi([this, message] { if (m_logView) m_logView->appendSuccess(message); });
}

void BasePage::logWarning(const QString& message)
{
    runOnUi([this, message] { if (m_logView) m_logView->appendWarning(message); });
}

void BasePage::logError(const QString& message)
{
    runOnUi([this, message] { if (m_logView) m_logView->appendError(message); });
}

void BasePage::logDetail(const QString& message)
{
    runOnUi([this, message] { if (m_logView) m_logView->appendDetail(message); });
}

void BasePage::setGlassSheet(bool on)
{
    if (m_glassSheet == on) return;
    m_glassSheet = on;

    // 🔴 QWidget 的**子类**默认不画 QSS 背景 —— 必须显式打开 WA_StyledBackground，
    //    否则 `QWidget#basePage[glass="true"]` 这条规则会被静默忽略（实测：加了
    //    属性 + repolish 仍然一点底都没有，因为 QWidget::paintEvent 根本不走样式表）。
    //    这是 Qt 的老规矩：只有 QFrame / QAbstractButton 这些自带样式绘制的类才免检。
    setAttribute(Qt::WA_StyledBackground, on);

    // 属性 + re-polish：QSS 的 [glass="true"] 分支只认**属性变更时**的重新解析。
    // （见记忆里的红线：改属性不 repolish，样式表当没这回事。）
    setProperty("glass", on ? QStringLiteral("true") : QString());
    style()->unpolish(this);
    style()->polish(this);
    update();
}

void BasePage::searchToggle(QPushButton* button){
    if (!button) return;
    const QString target = button->property("target").toString();
    if (target.isEmpty()) return;
    QWidget* box = findChild<QWidget*>(target);
    if (!box) return;

    if (box->isVisible()) {
        box->hide();
        // 清空 = 取消过滤；容器里若有输入框一并清掉（否则用户以为表格"少了行"）。
        for (QLineEdit* edit : box->findChildren<QLineEdit*>()) edit->clear();
    } else {
        box->show();
        QLineEdit* edit = box->findChild<QLineEdit*>();
        if (!edit && qobject_cast<QLineEdit*>(box)) edit = qobject_cast<QLineEdit*>(box);
        if (edit) {
            edit->setFocus();
            edit->selectAll();
        }
    }
}

void BasePage::setShellStatus(const QString& device, const QString& mode, const QString& hint)
{
    // 外壳通过属性 / 信号读取；无外壳时静默。
    setProperty("shellDevice", device);
    setProperty("shellMode", mode);
    setProperty("shellHint", hint);
    emit shellStatusChanged(device, mode, hint);
}

} // namespace kc::ui
