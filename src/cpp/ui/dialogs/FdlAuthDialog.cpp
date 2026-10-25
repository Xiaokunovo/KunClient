#include "ui/dialogs/FdlAuthDialog.h"

#include "kc/auth/SignCodec.h"
#include "ui/dialogs/GlassDialog.h"
#include "ui/theme/ThemeManager.h"

#include <QClipboard>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QVBoxLayout>

namespace kc::ui {
namespace {

/// 只读多行框（blob 展示用）：选中可复制、不可编辑。
QPlainTextEdit* makeReadOnlyBox(const QString& text, QWidget* parent)
{
    auto* box = new QPlainTextEdit(text, parent);
    box->setReadOnly(true);
    box->setMinimumHeight(84);
    box->setMaximumHeight(96);
    box->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    return box;
}

} // namespace

FdlAuthDialog::FdlAuthDialog(const QString& blob, QWidget* parent)
    : AnimatedDialog(parent)
    , m_blob(blob)
{
    setWindowTitle(QStringLiteral("FDL Auth"));
    resize(540, 520);
    buildUi();
}

void FdlAuthDialog::buildUi()
{
    // 用 GlassDialog 的壳（标题 + bodyLayout + 底部按钮行），观感与其它弹窗同族。
    // 这里刻意**不**继承 GlassDialog（那样要多改一处基类），而是在本类内组合一个
    // GlassDialog 当容器 —— 但 AnimatedDialog 已经是本类的基类，不能双继承 QDialog。
    // 因此这里直接手工搭一个与 GlassDialog 同口径的布局：标题 + 说明 + 两个框。
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(24, 22, 24, 20);
    root->setSpacing(10);

    auto* title = new QLabel(QStringLiteral("FDL Auth"), this);
    QFont tf = title->font();
    tf.setPointSize(15);
    tf.setBold(true);
    title->setFont(tf);
    root->addWidget(title);

    auto* intro = new QLabel(
        QStringLiteral("本工具不计算签名。请把下面的 Blob 复制出去、用小米签名服务算好 sig，"
                       "再把 sig 粘贴进来。"),
        this);
    intro->setWordWrap(true);
    intro->setObjectName(QStringLiteral("settingsHint"));
    root->addWidget(intro);

    // ── Blob（只读 + 复制）────────────────────────────────────────────────
    auto* blobRow = new QHBoxLayout;
    blobRow->setSpacing(8);
    auto* blobLabel = new QLabel(QStringLiteral("Blob（设备读到的 token）"), this);
    blobRow->addWidget(blobLabel);
    blobRow->addStretch();
    auto* copyButton = new QPushButton(QStringLiteral("复制"), this);
    copyButton->setCursor(Qt::PointingHandCursor);
    blobRow->addWidget(copyButton);
    root->addLayout(blobRow);

    m_blobEdit = makeReadOnlyBox(m_blob, this);
    root->addWidget(m_blobEdit);

    connect(copyButton, &QPushButton::clicked, this, [this] {
        QGuiApplication::clipboard()->setText(m_blob);
        updateHint();
    });

    // ── Sig（粘贴）──────────────────────────────────────────────────────
    auto* sigLabel = new QLabel(QStringLiteral("Sig（粘贴算好的签名：十六进制或 Base64）"), this);
    root->addWidget(sigLabel);

    m_sigEdit = new QPlainTextEdit(this);
    m_sigEdit->setMinimumHeight(96);
    m_sigEdit->setPlaceholderText(
        QStringLiteral("例：a1b2c3… 或 带空格/冒号的十六进制，也可以粘 Base64"));
    m_sigEdit->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    root->addWidget(m_sigEdit, 1);

    m_hint = new QLabel(this);
    m_hint->setWordWrap(true);
    m_hint->setObjectName(QStringLiteral("settingsHint"));
    root->addWidget(m_hint);

    // ── 底部按钮 ────────────────────────────────────────────────────────
    auto* buttonRow = new QHBoxLayout;
    buttonRow->addStretch();
    auto* cancelButton = new QPushButton(QStringLiteral("取消"), this);
    cancelButton->setCursor(Qt::PointingHandCursor);
    m_okButton = new QPushButton(QStringLiteral("写入"), this);
    m_okButton->setObjectName(QStringLiteral("primaryButton"));
    m_okButton->setCursor(Qt::PointingHandCursor);
    m_okButton->setEnabled(false);
    buttonRow->addWidget(cancelButton);
    buttonRow->addWidget(m_okButton);
    root->addLayout(buttonRow);

    connect(cancelButton, &QPushButton::clicked, this, [this] { reject(); });
    connect(m_okButton, &QPushButton::clicked, this, [this] { accept(); });
    connect(m_sigEdit, &QPlainTextEdit::textChanged, this, [this] { updateHint(); });

    updateHint();
}

void FdlAuthDialog::updateHint()
{
    if (!m_hint || !m_okButton) return;

    const QString raw = m_sigEdit ? m_sigEdit->toPlainText() : QString();
    if (raw.trimmed().isEmpty()) {
        m_hint->setText(QStringLiteral("尚未粘贴 sig。"));
        m_okButton->setEnabled(false);
        return;
    }

    // 用与 XiaomiEdlPage 落盘前**同一套**解析口径校验，避免「这里能过、那里解不出」。
    const QByteArray bytes = kc::auth::tryParseBytes(raw);
    if (bytes.isEmpty()) {
        m_hint->setText(QStringLiteral("这段内容既不像十六进制、也不像 Base64，请检查是否粘全。"));
        m_okButton->setEnabled(false);
        return;
    }

    m_hint->setText(QStringLiteral("已识别 %1 字节，点「写入」继续。").arg(bytes.size()));
    m_okButton->setEnabled(true);
}

QString FdlAuthDialog::sigText() const
{
    return m_sigEdit ? m_sigEdit->toPlainText().trimmed() : QString();
}

} // namespace kc::ui
