#include "ui/dialogs/ComplianceConfirmDialog.h"

#include <QCheckBox>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>

namespace kc::ui {

ComplianceConfirmDialog::ComplianceConfirmDialog(const QString& actionName,
                                                 const QString& riskText, QWidget* parent)
    : AnimatedDialog(parent)
{
    setWindowTitle(QStringLiteral("合规确认 · %1").arg(actionName));
    setModal(true);
    setFixedSize(480, 474);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(20, 16, 20, 14);
    root->setSpacing(12);

    auto* risk = new QLabel(riskText, this);
    risk->setObjectName(QStringLiteral("legalBody"));
    risk->setWordWrap(true);
    risk->setTextInteractionFlags(Qt::TextSelectableByMouse);

    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QScrollArea::NoFrame);
    auto* host = new QWidget(scroll);
    auto* hostLayout = new QVBoxLayout(host);
    hostLayout->setContentsMargins(0, 0, 0, 0);
    hostLayout->setSpacing(12);

    auto* legalNote = new QLabel(QStringLiteral(
        "仅可在设备为本人合法所有、或已获得设备所有者明确授权（如维修接单）的情况下继续。"
        "严禁用于来历不明、遗失或被盗的设备。\n\n"
        "将本功能用于他人或来历不明的设备，不只是违约，还可能构成刑事犯罪："
        "涉嫌《刑法》第285条（非法获取计算机信息系统数据、非法控制计算机信息系统罪）、"
        "第286条（破坏计算机信息系统罪）、第264条（盗窃罪）、"
        "第312条（掩饰、隐瞒犯罪所得、犯罪所得收益罪）、"
        "第253条之一（侵犯公民个人信息罪），并可能触犯《反电信网络诈骗法》第14条。责任由操作者自负。"), host);
    legalNote->setObjectName(QStringLiteral("legalWarning"));
    legalNote->setWordWrap(true);

    m_chkOwn = new QCheckBox(
        QStringLiteral("我确认该设备为本人合法所有，或已获得设备所有者的明确授权，并愿意承担相应责任。"), host);
    m_chkOwn->setObjectName(QStringLiteral("dialogHint"));
    m_chkLegal = new QCheckBox(
        QStringLiteral("我已知晓：将该功能用于他人或来历不明的设备可能构成刑事犯罪，责任由我本人承担。"), host);
    m_chkLegal->setObjectName(QStringLiteral("dialogHint"));

    hostLayout->addWidget(risk);
    hostLayout->addWidget(legalNote);
    hostLayout->addWidget(m_chkOwn);
    hostLayout->addWidget(m_chkLegal);
    hostLayout->addStretch();
    scroll->setWidget(host);

    auto* cancel = new QPushButton(QStringLiteral("取消"), this);
    m_ok = new QPushButton(QStringLiteral("确认执行"), this);
    m_ok->setObjectName(QStringLiteral("primaryButton"));
    m_ok->setCursor(Qt::PointingHandCursor);
    m_ok->setEnabled(false);   // 未勾选确认时保持禁用，拒绝执行不留口子

    auto* buttonRow = new QHBoxLayout(this);
    buttonRow->addStretch();
    buttonRow->addWidget(cancel);
    buttonRow->addWidget(m_ok);

    root->addWidget(scroll, 1);
    root->addLayout(buttonRow);

    connect(m_chkOwn, &QCheckBox::toggled, this, &ComplianceConfirmDialog::updateOkEnabled);
    connect(m_chkLegal, &QCheckBox::toggled, this, &ComplianceConfirmDialog::updateOkEnabled);
    connect(m_ok, &QPushButton::clicked, this, &QDialog::accept);
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
}

void ComplianceConfirmDialog::updateOkEnabled()
{
    m_ok->setEnabled(m_chkOwn->isChecked() && m_chkLegal->isChecked());
}

bool ComplianceConfirmDialog::confirm(QWidget* parent, const QString& actionName,
                                      const QString& riskText)
{
    ComplianceConfirmDialog dlg(actionName, riskText, parent);
    return dlg.exec() == QDialog::Accepted;
}

} // namespace kc::ui
