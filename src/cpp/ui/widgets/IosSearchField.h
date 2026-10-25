#pragma once

// ─────────────────────────────────────────────────────────────────────────────
//  IosSearchField —— iOS 搜索框
//
//  QLineEdit 子类：自己画胶囊底、放大镜、清除按钮，**不依赖 QSS 背景**。
//  因为继承 QLineEdit，原来的 text() / textChanged / setPlaceholderText /
//  setGeometry / show / hide 全部照旧可用，替换成本极低。
//
//      auto* s = new IosSearchField(host);
//      s->setPlaceholderText(QStringLiteral("搜索…"));
//      connect(s, &QLineEdit::textChanged, ...);
// ─────────────────────────────────────────────────────────────────────────────

#include <QLineEdit>

namespace kc::ui {

class IosSearchField : public QLineEdit {
    Q_OBJECT

public:
    explicit IosSearchField(QWidget* parent = nullptr);

    /// 岛体永远是深色时用：强制深色外观（不跟随主题）。
    void setForceDark(bool on);

    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    bool clearHit(const QPoint& pos) const;
    void applyPalette();

    bool m_forceDark = false;
};

} // namespace kc::ui
