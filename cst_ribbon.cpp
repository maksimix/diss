#include "cst_ribbon.h"

#include <QtGui/QAction>
#include <QtGui/QPainter>
#include <QtGui/QPainterPath>
#include <QtGui/QPixmap>
#include <QtGui/QPolygonF>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QStackedWidget>
#include <QtWidgets/QTabBar>
#include <QtWidgets/QToolButton>
#include <QtWidgets/QVBoxLayout>

#include <cmath>

namespace
{
constexpr int icon_size = 32;
// M_PI не объявлена в MSVC без _USE_MATH_DEFINES, поэтому константа своя.
constexpr double pi = 3.14159265358979323846;

void drawFolder(QPainter &painter)
{
    painter.setPen(QPen(QColor(180, 140, 40), 1.4));
    painter.setBrush(QColor(245, 200, 90));
    painter.drawRect(QRectF(3, 10, 26, 18));
    painter.drawRect(QRectF(3, 6, 12, 5));
}

void drawFloppy(QPainter &painter)
{
    painter.setPen(QPen(QColor(60, 90, 130), 1.4));
    painter.setBrush(QColor(110, 160, 210));
    painter.drawRect(QRectF(4, 4, 24, 24));
    painter.setBrush(QColor(245, 245, 245));
    painter.drawRect(QRectF(9, 4, 14, 9));
    painter.setBrush(QColor(235, 240, 245));
    painter.drawRect(QRectF(8, 18, 16, 10));
}

void drawWaveguideBox(QPainter &painter, const QColor &face)
{
    const QPolygonF top({QPointF(5, 12), QPointF(15, 6), QPointF(29, 10), QPointF(19, 16)});
    const QPolygonF front({QPointF(5, 12), QPointF(19, 16), QPointF(19, 26), QPointF(5, 22)});
    const QPolygonF side({QPointF(19, 16), QPointF(29, 10), QPointF(29, 20), QPointF(19, 26)});
    painter.setPen(QPen(QColor(70, 90, 110), 1.2));
    painter.setBrush(face.lighter(115));
    painter.drawPolygon(top);
    painter.setBrush(face);
    painter.drawPolygon(front);
    painter.setBrush(face.darker(115));
    painter.drawPolygon(side);
}

void paintRibbonIcon(QPainter &painter, RibbonIcon icon)
{
    switch (icon) {
    case RibbonIcon::Open:
        drawFolder(painter);
        break;
    case RibbonIcon::Save:
        drawFloppy(painter);
        break;
    case RibbonIcon::SaveAs:
        drawFloppy(painter);
        painter.setPen(QPen(QColor(70, 130, 70), 2.0));
        painter.drawLine(QPointF(22, 22), QPointF(28, 22));
        painter.drawLine(QPointF(25, 19), QPointF(25, 25));
        break;
    case RibbonIcon::Quit:
        painter.setPen(QPen(QColor(180, 60, 60), 2.4));
        painter.drawArc(QRectF(7, 7, 18, 18), 60 * 16, 240 * 16);
        painter.drawLine(QPointF(16, 5), QPointF(16, 15));
        break;
    case RibbonIcon::Start: {
        painter.setPen(QPen(QColor(40, 110, 50), 1.4));
        painter.setBrush(QColor(90, 190, 100));
        painter.drawEllipse(QRectF(3, 3, 26, 26));
        painter.setBrush(QColor(255, 255, 255));
        painter.setPen(Qt::NoPen);
        painter.drawPolygon(QPolygonF({QPointF(13, 9), QPointF(23, 16), QPointF(13, 23)}));
        break;
    }
    case RibbonIcon::Setup: {
        painter.setPen(QPen(QColor(90, 100, 110), 1.3));
        painter.setBrush(QColor(175, 185, 195));
        QPainterPath gear;
        const QPointF center(16, 16);
        for (int tooth = 0; tooth < 8; ++tooth) {
            const double angle = tooth * pi / 4.0;
            gear.addRect(QRectF(center.x() + 10.0 * std::cos(angle) - 3.0,
                                center.y() + 10.0 * std::sin(angle) - 3.0,
                                6.0,
                                6.0));
        }
        painter.drawPath(gear.simplified());
        painter.drawEllipse(center, 8.0, 8.0);
        painter.setBrush(QColor(250, 250, 250));
        painter.drawEllipse(center, 3.5, 3.5);
        break;
    }
    case RibbonIcon::Parameters:
        painter.setPen(QPen(QColor(90, 100, 110), 1.2));
        painter.setBrush(QColor(252, 252, 252));
        painter.drawRect(QRectF(4, 6, 24, 20));
        painter.setBrush(QColor(200, 215, 235));
        painter.drawRect(QRectF(4, 6, 24, 6));
        painter.drawLine(QPointF(4, 18), QPointF(28, 18));
        painter.drawLine(QPointF(14, 6), QPointF(14, 26));
        break;
    case RibbonIcon::Waveguide:
        drawWaveguideBox(painter, QColor(150, 185, 215));
        break;
    case RibbonIcon::Slot:
        drawWaveguideBox(painter, QColor(150, 185, 215));
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(210, 70, 60));
        painter.drawRect(QRectF(9, 10, 9, 2.5));
        break;
    case RibbonIcon::Plate:
        drawWaveguideBox(painter, QColor(180, 200, 215));
        painter.setPen(QPen(QColor(150, 90, 40), 1.0));
        painter.setBrush(QColor(225, 155, 70));
        painter.drawPolygon(
            QPolygonF({QPointF(10, 11), QPointF(16, 7.5), QPointF(16, 19), QPointF(10, 22)}));
        break;
    case RibbonIcon::Iris:
        painter.setPen(QPen(QColor(120, 130, 140), 1.2));
        painter.setBrush(QColor(200, 210, 220));
        painter.drawRect(QRectF(5, 4, 22, 24));
        painter.setBrush(QColor(250, 250, 252));
        painter.drawRect(QRectF(11, 11, 10, 10));
        break;
    case RibbonIcon::RoundIris:
        painter.setPen(QPen(QColor(120, 130, 140), 1.2));
        painter.setBrush(QColor(200, 210, 220));
        painter.drawRect(QRectF(5, 4, 22, 24));
        painter.setBrush(QColor(250, 250, 252));
        painter.drawEllipse(QPointF(16, 16), 6.5, 6.5);
        painter.setBrush(QColor(225, 155, 70));
        painter.setPen(Qt::NoPen);
        painter.drawRect(QRectF(14.5, 16, 3, 6.5));
        break;
    case RibbonIcon::Profile:
        painter.setPen(QPen(QColor(70, 100, 130), 1.6));
        painter.setBrush(QColor(175, 205, 230));
        painter.drawPolygon(QPolygonF({QPointF(4, 8),
                                       QPointF(12, 8),
                                       QPointF(12, 13),
                                       QPointF(20, 13),
                                       QPointF(20, 8),
                                       QPointF(28, 8),
                                       QPointF(28, 24),
                                       QPointF(4, 24)}));
        break;
    case RibbonIcon::Excitation:
        painter.setPen(QPen(QColor(200, 120, 40), 2.0));
        painter.drawPolyline(QPolygonF({QPointF(3, 16),
                                        QPointF(8, 7),
                                        QPointF(13, 25),
                                        QPointF(18, 7),
                                        QPointF(23, 25),
                                        QPointF(29, 16)}));
        break;
    case RibbonIcon::Fields:
        painter.setPen(QPen(QColor(60, 110, 190), 1.8));
        painter.drawArc(QRectF(4, 8, 24, 16), 0, 180 * 16);
        painter.drawArc(QRectF(8, 12, 16, 12), 0, 180 * 16);
        painter.setPen(QPen(QColor(200, 70, 60), 1.8));
        painter.drawLine(QPointF(16, 6), QPointF(16, 26));
        break;
    case RibbonIcon::ResetView:
        drawWaveguideBox(painter, QColor(200, 210, 220));
        painter.setPen(QPen(QColor(60, 130, 90), 2.0));
        painter.drawArc(QRectF(6, 6, 20, 20), 30 * 16, 240 * 16);
        break;
    case RibbonIcon::Report:
        painter.setPen(QPen(QColor(120, 130, 140), 1.2));
        painter.setBrush(QColor(252, 252, 252));
        painter.drawRect(QRectF(6, 4, 20, 24));
        painter.setPen(QPen(QColor(70, 120, 190), 1.8));
        painter.drawPolyline(
            QPolygonF({QPointF(9, 22), QPointF(14, 15), QPointF(18, 19), QPointF(23, 9)}));
        break;
    }
}
}

QIcon ribbonIcon(RibbonIcon icon)
{
    QPixmap pixmap(icon_size, icon_size);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    paintRibbonIcon(painter, icon);
    painter.end();
    return QIcon(pixmap);
}

RibbonGroup::RibbonGroup(const QString &title, QWidget *parent)
    : QWidget(parent)
{
    QVBoxLayout *outer = new QVBoxLayout(this);
    outer->setContentsMargins(4, 3, 4, 2);
    outer->setSpacing(2);

    QWidget *content = new QWidget(this);
    content_layout_ = new QHBoxLayout(content);
    content_layout_->setContentsMargins(0, 0, 0, 0);
    content_layout_->setSpacing(2);
    outer->addWidget(content, 1);

    QLabel *caption = new QLabel(title, this);
    caption->setAlignment(Qt::AlignHCenter | Qt::AlignVCenter);
    caption->setStyleSheet(QStringLiteral("color: #4c4c4c; font-size: 8pt;"));
    outer->addWidget(caption);
}

void RibbonGroup::paintEvent(QPaintEvent *)
{
    // Разделитель справа от группы — та же вертикальная линия, что отделяет
    // блоки ленты в CST.
    QPainter painter(this);
    painter.setPen(QColor(214, 214, 214));
    painter.drawLine(width() - 1, 4, width() - 1, height() - 6);
}

QToolButton *RibbonGroup::addLargeButton(QAction *action, RibbonIcon icon)
{
    action->setIcon(ribbonIcon(icon));
    QToolButton *button = new QToolButton(this);
    button->setDefaultAction(action);
    button->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
    button->setIconSize(QSize(icon_size, icon_size));
    button->setAutoRaise(true);
    button->setMinimumWidth(56);
    button->setMaximumWidth(96);
    button->setMinimumHeight(64);
    content_layout_->addWidget(button, 0, Qt::AlignTop);
    small_column_ = nullptr;
    small_button_count_ = 0;
    return button;
}

QVBoxLayout *RibbonGroup::smallButtonColumn()
{
    if (small_column_ == nullptr || small_button_count_ >= 3) {
        small_column_ = new QVBoxLayout();
        small_column_->setContentsMargins(0, 0, 0, 0);
        small_column_->setSpacing(1);
        small_column_->addStretch(1);
        content_layout_->addLayout(small_column_);
        small_button_count_ = 0;
    }
    return small_column_;
}

QToolButton *RibbonGroup::addSmallButton(QAction *action, RibbonIcon icon)
{
    action->setIcon(ribbonIcon(icon));
    QToolButton *button = new QToolButton(this);
    button->setDefaultAction(action);
    button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    button->setIconSize(QSize(16, 16));
    button->setAutoRaise(true);
    QVBoxLayout *column = smallButtonColumn();
    column->insertWidget(small_button_count_, button);
    ++small_button_count_;
    return button;
}

void RibbonGroup::addLabeledWidget(const QString &label, QWidget *widget)
{
    QVBoxLayout *column = smallButtonColumn();
    QWidget *row = new QWidget(this);
    QHBoxLayout *row_layout = new QHBoxLayout(row);
    row_layout->setContentsMargins(2, 1, 2, 1);
    row_layout->setSpacing(4);
    if (!label.isEmpty()) {
        row_layout->addWidget(new QLabel(label, row));
    }
    widget->setParent(row);
    row_layout->addWidget(widget, 1);
    column->insertWidget(small_button_count_, row);
    ++small_button_count_;
}

RibbonTab::RibbonTab(QWidget *parent)
    : QWidget(parent)
{
    layout_ = new QHBoxLayout(this);
    layout_->setContentsMargins(3, 1, 3, 1);
    layout_->setSpacing(0);
    layout_->addStretch(1);
}

RibbonGroup *RibbonTab::addGroup(const QString &title)
{
    RibbonGroup *group = new RibbonGroup(title, this);
    // Растяжка всегда остаётся последней, чтобы группы прижимались влево.
    layout_->insertWidget(layout_->count() - 1, group);
    return group;
}

RibbonBar::RibbonBar(QWidget *parent)
    : QWidget(parent)
{
    QVBoxLayout *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    tab_bar_ = new QTabBar(this);
    tab_bar_->setExpanding(false);
    tab_bar_->setDrawBase(false);
    tab_bar_->setObjectName(QStringLiteral("cstRibbonTabBar"));

    pages_ = new QStackedWidget(this);
    pages_->setObjectName(QStringLiteral("cstRibbonPages"));

    layout->addWidget(tab_bar_);
    layout->addWidget(pages_);

    connect(tab_bar_, &QTabBar::currentChanged, pages_, &QStackedWidget::setCurrentIndex);
}

RibbonTab *RibbonBar::addRibbonTab(const QString &title)
{
    RibbonTab *tab = new RibbonTab(pages_);
    pages_->addWidget(tab);
    tab_bar_->addTab(title);
    return tab;
}

void RibbonBar::setCurrentTabIndex(int index)
{
    tab_bar_->setCurrentIndex(index);
    pages_->setCurrentIndex(index);
}
