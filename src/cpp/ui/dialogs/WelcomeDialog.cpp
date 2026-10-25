#include "ui/dialogs/WelcomeDialog.h"

#include "ui/theme/ThemeManager.h"
#include "ui/widgets/AppIcons.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QSettings>
#include <QStackedWidget>
#include <QVBoxLayout>
#include <QWindow>

#include <cstring>

namespace kc::ui {
namespace {

/// 一次性标记的键。用 QSettings 存在本机（%APPDATA%），所以「第一次打开」
/// 是按**这台机器上的这个用户**算的 —— 换机器会重新引导一次，这是期望行为。
constexpr const char* kCompletedKey = "onboarding/completed";

// ── 大图标块 ────────────────────────────────────────────────────────────────
/// 96×96 圆角方块 + 白色矢量图标。渐变底色一页一色，四页连起来是一组色相
/// （蓝 → 青 → 紫 → 琥珀），翻页时能感到「在往前走」而不是四张一样的图。
class WelcomeBadge : public QWidget {
public:
    WelcomeBadge(AppIcon icon, const QColor& c1, const QColor& c2, QWidget* parent = nullptr)
        : QWidget(parent), m_icon(icon), m_c1(c1), m_c2(c2)
    {
        setFixedSize(96, 96);
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);

        const QRectF r = QRectF(rect());
        QLinearGradient g(r.topLeft(), r.bottomRight());
        g.setColorAt(0.0, m_c1);
        g.setColorAt(1.0, m_c2);
        p.setPen(Qt::NoPen);
        p.setBrush(g);
        p.drawRoundedRect(r, 28, 28);

        // 上沿一道内高光：没有它整块是「贴纸」，有了才有玻璃的厚度。
        QPainterPath clip;
        clip.addRoundedRect(r, 28, 28);
        p.save();
        p.setClipPath(clip);
        QLinearGradient hl(r.topLeft(), QPointF(r.left(), r.top() + r.height() * 0.55));
        hl.setColorAt(0.0, QColor(255, 255, 255, 64));
        hl.setColorAt(1.0, QColor(255, 255, 255, 0));
        p.fillRect(r, hl);
        p.restore();

        // 字形占方块 0.54，与主屏图标的 0.64 相比留更多呼吸（这里方块更大）。
        const qreal side = r.width() * 0.54;
        const QRectF box(r.center().x() - side / 2.0, r.center().y() - side / 2.0, side, side);
        drawAppIcon(p, box, m_icon, QColor(255, 255, 255, 242));
    }

private:
    AppIcon m_icon;
    QColor m_c1;
    QColor m_c2;
};

// ── 页脚圆点 ────────────────────────────────────────────────────────────────
/// 当前页拉成 20×6 的胶囊，其余是 10×6 的圆点（iOS 分页控件的口径）。
class StepDots : public QWidget {
public:
    explicit StepDots(int count, QWidget* parent = nullptr) : QWidget(parent), m_count(count) {
        setFixedSize(totalWidth(), 10);
    }

    void setIndex(int index) {
        if (m_index == index) return;
        m_index = index;
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override {
        const bool dark = ThemeManager::instance().isDark();
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setPen(Qt::NoPen);

        constexpr qreal kDot = 10.0;
        constexpr qreal kActive = 20.0;
        constexpr qreal kGap = 6.0;
        constexpr qreal kH = 6.0;
        qreal x = (width() - totalWidth()) / 2.0;
        for (int i = 0; i < m_count; ++i) {
            const qreal w = (i == m_index) ? kActive : kDot;
            const QColor c = (i == m_index)
                                 ? ThemeManager::instance().color(ColorToken::Accent)
                                 : (dark ? QColor(255, 255, 255, 64) : QColor(15, 23, 42, 46));
            p.setBrush(c);
            p.drawRoundedRect(QRectF(x, (height() - kH) / 2.0, w, kH), kH / 2.0, kH / 2.0);
            x += w + kGap;
        }
    }

private:
    static constexpr qreal totalWidth() { return 20.0 + 10.0 * 3 + 6.0 * 3; }   // 4 页

    int m_count = 4;
    int m_index = 0;
};

/// 一页的内容定义。
struct PageDef {
    AppIcon icon;
    QColor c1;
    QColor c2;
    const char* title;
    const char* body;    // 富文本（不写内联颜色 —— 深浅主题共用一份）
};

const PageDef kPages[] = {
    { AppIcon::Home,
      QColor(0x3D, 0x8B, 0xFF), QColor(0x0A, 0x5C, 0xE0),
      "欢迎使用 KunClient",
      "<p>一个把刷机、设备管理、诊断收在一处的桌面工具。</p>"
      "<p>这是第一次打开，花一分钟走完三步导览 —— 之后不会再打扰你。</p>" },

    { AppIcon::Grid,
      QColor(0x22, 0xB8, 0xA8), QColor(0x0E, 0x7F, 0x8C),
      "主屏就是导航",
      "<ul>"
      "<li>点主屏上的图标进入功能页</li>"
      "<li>左上角箭头、或从窗口左边缘往右拖，回到主屏</li>"
      "<li>顶栏按住往下拉，拉开控制中心（主题、玻璃、背景、日志档位都在那）</li>"
      "</ul>" },

    { AppIcon::Cpu,
      QColor(0x8A, 0x6B, 0xF0), QColor(0x5B, 0x3F, 0xC4),
      "动手之前，先让工具替你检查",
      "<ul>"
      "<li>插上 9008 设备会被自动识别并连接，不用手动挑串口</li>"
      "<li>选好 rawprogram XML 会先做离线预检：扇区、溢出、文件长度、重叠</li>"
      "<li>日志分层显示，每一步带真实耗时；出问题先看最后一条 Error</li>"
      "</ul>" },

    { AppIcon::Toolbox,
      QColor(0xF0, 0xA0, 0x30), QColor(0xC0, 0x6A, 0x08),
      "可以开始了",
      "<p>三条经验，比任何教程都管用：</p>"
      "<ul>"
      "<li>动手前先备份，尤其是 userdata 和 persist</li>"
      "<li>刷写过程中别拔线，也别重复点开始</li>"
      "<li>出问题先导出日志，再重试</li>"
      "</ul>" },
};

} // namespace

WelcomeDialog::WelcomeDialog(QWidget* parent)
    : AnimatedDialog(parent)
{
    setObjectName(QStringLiteral("welcomeDialog"));
    setWindowTitle(QStringLiteral("欢迎"));
    setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint);
    setAttribute(Qt::WA_TranslucentBackground);
    setModal(true);
    setRiseEnabled(false);   // 和登录框一样：居中大窗不做上浮，只淡入
    resize(780, 566);
    buildUi();
    goTo(0);
}

bool WelcomeDialog::isCompleted()
{
    return QSettings().value(QString::fromLatin1(kCompletedKey), false).toBool();
}

void WelcomeDialog::markCompleted()
{
    QSettings().setValue(QString::fromLatin1(kCompletedKey), true);
}

void WelcomeDialog::resetCompleted()
{
    QSettings().remove(QString::fromLatin1(kCompletedKey));
}

void WelcomeDialog::buildUi()
{
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(40, 34, 40, 26);
    root->setSpacing(0);

    m_stack = new QStackedWidget(this);
    m_stack->setObjectName(QStringLiteral("welcomeStack"));
    m_pageCount = static_cast<int>(sizeof(kPages) / sizeof(kPages[0]));
    for (int i = 0; i < m_pageCount; ++i) m_stack->addWidget(buildPage(i));
    root->addWidget(m_stack, 1);

    // ── 页脚：跳过 · 圆点 · 下一步 ────────────────────────────────────────
    auto* footer = new QWidget(this);
    auto* row = new QHBoxLayout(footer);
    row->setContentsMargins(0, 18, 0, 0);
    row->setSpacing(0);

    m_skip = new QPushButton(QStringLiteral("跳过"), footer);
    m_skip->setObjectName(QStringLiteral("welcomeSkip"));
    m_skip->setCursor(Qt::PointingHandCursor);
    m_skip->setFocusPolicy(Qt::NoFocus);
    connect(m_skip, &QPushButton::clicked, this, [this] {
        markCompleted();
        accept();
    });
    row->addWidget(m_skip, 0, Qt::AlignLeft);

    row->addStretch(1);
    auto* dots = new StepDots(m_pageCount, footer);
    dots->setObjectName(QStringLiteral("welcomeDots"));
    m_setDots = [dots](int index) { dots->setIndex(index); };
    row->addWidget(dots, 0, Qt::AlignCenter);
    m_stepLabel = new QLabel(footer);
    m_stepLabel->setObjectName(QStringLiteral("welcomeStep"));
    m_stepLabel->hide();   // 有圆点就不重复报页码
    row->addWidget(m_stepLabel, 0, Qt::AlignCenter);
    row->addStretch(1);

    m_next = new QPushButton(QStringLiteral("下一步"), footer);
    m_next->setObjectName(QStringLiteral("primaryButton"));
    m_next->setCursor(Qt::PointingHandCursor);
    m_next->setMinimumWidth(112);
    connect(m_next, &QPushButton::clicked, this, [this] {
        if (m_index + 1 >= m_pageCount) {
            markCompleted();
            accept();
            return;
        }
        goTo(m_index + 1);
    });
    row->addWidget(m_next, 0, Qt::AlignRight);

    root->addWidget(footer);
}

QWidget* WelcomeDialog::buildPage(int index)
{
    const PageDef& def = kPages[index];

    auto* page = new QWidget(this);
    auto* col = new QVBoxLayout(page);
    col->setContentsMargins(0, 0, 0, 0);
    col->setSpacing(0);

    col->addStretch(2);

    auto* badgeRow = new QHBoxLayout;
    badgeRow->addStretch(1);
    badgeRow->addWidget(new WelcomeBadge(def.icon, def.c1, def.c2, page));
    badgeRow->addStretch(1);
    col->addLayout(badgeRow);

    col->addSpacing(22);

    auto* title = new QLabel(QString::fromUtf8(def.title), page);
    title->setObjectName(QStringLiteral("welcomeTitle"));
    title->setAlignment(Qt::AlignCenter);
    col->addWidget(title);

    col->addSpacing(12);

    auto* body = new QLabel(QString::fromUtf8(def.body), page);
    body->setObjectName(QStringLiteral("welcomeBody"));
    body->setTextFormat(Qt::RichText);
    body->setWordWrap(true);
    // 🔴 必须给固定栏宽：QLabel 的富文本 sizeHint 在 wordWrap 下没有确定的换行基准，
    //    交给布局自由决定时会被压到 ~200px 宽、且只画得下第一段（实测：第二段整段被裁）。
    //    钉死宽度后 heightForWidth 才有基准，四页正文才都能完整显示。
    body->setFixedWidth(520);
    // 列表页左对齐（居中会让换行后的第二行参差不齐），纯段落页居中。
    const bool hasList = std::strstr(def.body, "<ul>") != nullptr;
    body->setAlignment(hasList ? Qt::AlignLeft : Qt::AlignHCenter);
    // 🔴 不写内联颜色：深浅主题共用同一份 HTML，颜色交给 QSS 的 #welcomeBody。

    auto* bodyRow = new QHBoxLayout;
    bodyRow->setContentsMargins(0, 0, 0, 0);
    bodyRow->addStretch(1);
    bodyRow->addWidget(body, 0, Qt::AlignTop);
    bodyRow->addStretch(1);
    col->addLayout(bodyRow);

    col->addStretch(3);
    return page;
}

void WelcomeDialog::goTo(int index)
{
    m_index = qBound(0, index, m_pageCount - 1);
    m_stack->setCurrentIndex(m_index);
    // 圆点跟着走 —— 唯一的状态源是 m_index，别让调用方各改一份。
    if (m_setDots) m_setDots(m_index);
    updateFooter();
}

void WelcomeDialog::previewPage(int index)
{
    goTo(index);
}

void WelcomeDialog::updateFooter()
{
    if (m_next)
        m_next->setText(m_index + 1 >= m_pageCount ? QStringLiteral("开始使用")
                                                  : QStringLiteral("下一步"));
    if (m_skip) m_skip->setVisible(m_index + 1 < m_pageCount);
}

void WelcomeDialog::paintEvent(QPaintEvent*)
{
    const bool dark = ThemeManager::instance().isDark();
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    const QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    QPainterPath shape;
    shape.addRoundedRect(r, 26, 26);
    p.setClipPath(shape);

    // 底：一道极缓的斜向渐变，避免大面积纯色显得死板。
    QLinearGradient g(r.topLeft(), r.bottomRight());
    if (dark) {
        g.setColorAt(0.0, QColor(0x1A, 0x1F, 0x27));
        g.setColorAt(1.0, QColor(0x10, 0x13, 0x18));
    } else {
        g.setColorAt(0.0, QColor(0xFD, 0xFE, 0xFF));
        g.setColorAt(1.0, QColor(0xEE, 0xF3, 0xFB));
    }
    p.fillPath(shape, g);

    // 上沿内高光 + 一圈收边。
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(dark ? QColor(255, 255, 255, 16) : QColor(255, 255, 255, 220), 1.0));
    p.drawPath(shape);
    p.setPen(QPen(dark ? QColor(255, 255, 255, 26) : QColor(0xD3, 0xDD, 0xEC), 1.0));
    p.drawRoundedRect(r.adjusted(1, 1, -1, -1), 25, 25);
}

void WelcomeDialog::mousePressEvent(QMouseEvent* event)
{
    // 无边框窗：按住空白处拖窗口（与主窗口顶栏同一套原生 startSystemMove）。
    if (event->button() == Qt::LeftButton && windowHandle()) {
        windowHandle()->startSystemMove();
        event->accept();
        return;
    }
    AnimatedDialog::mousePressEvent(event);
}

} // namespace kc::ui
