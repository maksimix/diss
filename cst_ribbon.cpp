#include "cst_ribbon.h"

#include <QtCore/QStringListModel>
#include <QtGui/QAction>
#include <QtGui/QPainter>
#include <QtGui/QPainterPath>
#include <QtGui/QPixmap>
#include <QtGui/QPolygonF>
#include <QtGui/QShortcut>
#include <QtWidgets/QCompleter>
#include <QtWidgets/QGridLayout>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QMenu>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QStackedWidget>
#include <QtWidgets/QTabBar>
#include <QtWidgets/QToolButton>
#include <QtWidgets/QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <utility>

namespace
{
constexpr int icon_size = 32;
constexpr int grid_icon_size = 24;
// M_PI не объявлена в MSVC без _USE_MATH_DEFINES, поэтому константа своя.
constexpr double pi = 3.14159265358979323846;

// Палитра повторяет таблицу стилей окна (cstStyleSheet в main_window.cpp):
// синяя полоса вкладок, серые рамки панелей, светлый заголовок.
const QColor header_text(0xff, 0xff, 0xff);
const QColor panel_caption_bg(0xf0, 0xf0, 0xf0);
const QColor panel_border(0xc4, 0xc4, 0xc4);
const QColor group_separator(0xdc, 0xdc, 0xdc);
const QColor caption_text(0x1f, 0x1f, 0x1f);

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

void drawChevron(QPainter &painter, bool up)
{
    painter.setPen(QPen(header_text, 2.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.setBrush(Qt::NoBrush);
    const double top = up ? 12.0 : 20.0;
    const double bottom = up ? 20.0 : 12.0;
    painter.drawPolyline(QPolygonF({QPointF(8, bottom), QPointF(16, top), QPointF(24, bottom)}));
}

// Изометрический кубик для значков дерева модели: origin — левый верхний угол
// передней грани, size — её сторона.
void drawModelCube(QPainter &painter, const QPointF &origin, double size, const QColor &face)
{
    const double shift_x = 0.55 * size;
    const double shift_y = 0.32 * size;
    const QRectF front(origin.x(), origin.y(), size, size);
    const QPolygonF top({front.topLeft(),
                         front.topLeft() + QPointF(shift_x, -shift_y),
                         front.topRight() + QPointF(shift_x, -shift_y),
                         front.topRight()});
    const QPolygonF side({front.topRight(),
                          front.topRight() + QPointF(shift_x, -shift_y),
                          front.bottomRight() + QPointF(shift_x, -shift_y),
                          front.bottomRight()});
    painter.setPen(QPen(QColor(70, 90, 110), 1.2));
    painter.setBrush(face.lighter(118));
    painter.drawPolygon(top);
    painter.setBrush(face.darker(112));
    painter.drawPolygon(side);
    painter.setBrush(face);
    painter.drawRect(front);
}

// Период синусоиды слева направо; средняя линия mid_y, размах amplitude.
void drawSineWave(QPainter &painter,
                  double x0,
                  double x1,
                  double mid_y,
                  double amplitude,
                  const QColor &color,
                  double width)
{
    painter.setPen(QPen(color, width, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.setBrush(Qt::NoBrush);
    QPolygonF wave;
    constexpr int segment_count = 24;
    for (int index = 0; index <= segment_count; ++index) {
        const double t = static_cast<double>(index) / segment_count;
        wave << QPointF(x0 + (x1 - x0) * t,
                        mid_y - amplitude * std::sin(2.0 * pi * t));
    }
    painter.drawPolyline(wave);
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
    case RibbonIcon::Stop: {
        // Парный к Start: тот же круг, но красный и с белым квадратом стопа.
        painter.setPen(QPen(QColor(140, 40, 40), 1.4));
        painter.setBrush(QColor(210, 80, 75));
        painter.drawEllipse(QRectF(3, 3, 26, 26));
        painter.setBrush(QColor(255, 255, 255));
        painter.setPen(Qt::NoPen);
        painter.drawRect(QRectF(11, 11, 10, 10));
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
    case RibbonIcon::Brick: {
        // Параллелепипед в лёгкой аксонометрии: передняя грань плюс два скоса.
        painter.setPen(QPen(QColor(120, 130, 140), 1.2));
        painter.setBrush(QColor(205, 216, 226));
        painter.drawRect(QRectF(6, 11, 15, 15));
        painter.setBrush(QColor(232, 239, 245));
        painter.drawPolygon(QPolygonF({QPointF(6, 11), QPointF(12, 5), QPointF(27, 5),
                                       QPointF(21, 11)}));
        painter.setBrush(QColor(178, 192, 205));
        painter.drawPolygon(QPolygonF({QPointF(21, 11), QPointF(27, 5), QPointF(27, 20),
                                       QPointF(21, 26)}));
        break;
    }
    case RibbonIcon::Cylinder: {
        painter.setPen(QPen(QColor(120, 130, 140), 1.2));
        painter.setBrush(QColor(205, 216, 226));
        painter.drawRect(QRectF(9, 9, 14, 15));
        painter.setBrush(QColor(232, 239, 245));
        painter.drawEllipse(QPointF(16, 9), 7.0, 3.2);
        painter.setBrush(QColor(186, 199, 211));
        painter.drawEllipse(QPointF(16, 24), 7.0, 3.2);
        break;
    }
    case RibbonIcon::Prism: {
        // Профиль-уголок, вытянутый вглубь: так призма отличается от бруска.
        painter.setPen(QPen(QColor(120, 130, 140), 1.2));
        painter.setBrush(QColor(205, 216, 226));
        painter.drawPolygon(QPolygonF({QPointF(7, 8), QPointF(19, 8), QPointF(19, 14),
                                       QPointF(13, 14), QPointF(13, 25), QPointF(7, 25)}));
        painter.setBrush(QColor(232, 239, 245));
        painter.drawPolygon(QPolygonF({QPointF(7, 8), QPointF(12, 4), QPointF(24, 4),
                                       QPointF(19, 8)}));
        painter.setBrush(QColor(178, 192, 205));
        painter.drawPolygon(QPolygonF({QPointF(19, 8), QPointF(24, 4), QPointF(24, 10),
                                       QPointF(19, 14)}));
        break;
    }
    case RibbonIcon::Boolean: {
        // Два пересекающихся круга — общепринятый знак булевой операции.
        painter.setPen(QPen(QColor(110, 125, 140), 1.4));
        painter.setBrush(QColor(210, 224, 236, 190));
        painter.drawEllipse(QPointF(13, 16), 8.0, 8.0);
        painter.setBrush(QColor(240, 200, 150, 190));
        painter.drawEllipse(QPointF(20, 16), 8.0, 8.0);
        break;
    }
    case RibbonIcon::Septum: {
        // Вид сверху: четыре встречные перегородки в тракте.
        drawWaveguideBox(painter, QColor(235, 241, 246));
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(200, 130, 60));
        painter.drawRect(QRectF(4, 11, 8, 2));
        painter.drawRect(QRectF(20, 11, 8, 2));
        painter.drawRect(QRectF(4, 19, 8, 2));
        painter.drawRect(QRectF(20, 19, 8, 2));
        break;
    }
    case RibbonIcon::CIris: {
        painter.setPen(QPen(QColor(120, 130, 140), 1.2));
        painter.setBrush(QColor(245, 248, 250));
        painter.drawRect(QRectF(4, 6, 24, 20));
        painter.setPen(QPen(QColor(150, 90, 40), 1.2));
        painter.setBrush(QColor(225, 155, 70));
        painter.drawPolygon(QPolygonF({QPointF(23, 14), QPointF(23, 10), QPointF(9, 10),
                                       QPointF(9, 22), QPointF(23, 22), QPointF(23, 18),
                                       QPointF(20, 18), QPointF(20, 19.5), QPointF(12, 19.5),
                                       QPointF(12, 12.5), QPointF(20, 12.5), QPointF(20, 14)}));
        break;
    }
    case RibbonIcon::CornerIris: {
        painter.setPen(QPen(QColor(120, 130, 140), 1.2));
        painter.setBrush(QColor(245, 248, 250));
        painter.drawRect(QRectF(4, 6, 24, 20));
        painter.setPen(QPen(QColor(150, 90, 40), 1.2));
        painter.setBrush(QColor(225, 155, 70));
        painter.drawPolygon(QPolygonF({QPointF(10, 6), QPointF(22, 6), QPointF(22, 10),
                                       QPointF(14, 10), QPointF(14, 15), QPointF(10, 15)}));
        painter.drawPolygon(QPolygonF({QPointF(22, 26), QPointF(10, 26), QPointF(10, 22),
                                       QPointF(18, 22), QPointF(18, 17), QPointF(22, 17)}));
        break;
    }
    case RibbonIcon::TStub: {
        painter.setPen(QPen(QColor(120, 130, 140), 1.2));
        painter.setBrush(QColor(245, 248, 250));
        painter.drawEllipse(QPointF(16, 16), 12.0, 12.0);
        painter.setPen(QPen(QColor(150, 90, 40), 1.2));
        painter.setBrush(QColor(225, 155, 70));
        painter.drawPolygon(QPolygonF({QPointF(4, 15), QPointF(10, 15), QPointF(10, 10),
                                       QPointF(13, 10), QPointF(13, 22), QPointF(10, 22),
                                       QPointF(10, 17), QPointF(4, 17)}));
        painter.drawPolygon(QPolygonF({QPointF(28, 15), QPointF(22, 15), QPointF(22, 10),
                                       QPointF(19, 10), QPointF(19, 22), QPointF(22, 22),
                                       QPointF(22, 17), QPointF(28, 17)}));
        break;
    }
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
    case RibbonIcon::ProfileStep:
        // Уступ на одной стенке: та же заготовка профиля, но несимметричная.
        painter.setPen(QPen(QColor(70, 100, 130), 1.6));
        painter.setBrush(QColor(175, 205, 230));
        painter.drawPolygon(QPolygonF({QPointF(4, 8),
                                       QPointF(16, 8),
                                       QPointF(16, 14),
                                       QPointF(28, 14),
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
    case RibbonIcon::Project:
        // Значок документа на вкладке модели: лист с бегущей волной.
        painter.setPen(QPen(QColor(120, 130, 140), 1.4));
        painter.setBrush(QColor(252, 252, 252));
        painter.drawRect(QRectF(5, 5, 22, 22));
        painter.setPen(QPen(QColor(40, 150, 130), 2.2));
        painter.setBrush(Qt::NoBrush);
        painter.drawPolyline(QPolygonF({QPointF(7, 20),
                                        QPointF(11, 12),
                                        QPointF(16, 20),
                                        QPointF(21, 12),
                                        QPointF(25, 18)}));
        break;
    case RibbonIcon::Tree:
        painter.setPen(QPen(QColor(120, 130, 140), 1.4));
        painter.drawLine(QPointF(8, 6), QPointF(8, 24));
        painter.drawLine(QPointF(8, 12), QPointF(14, 12));
        painter.drawLine(QPointF(8, 18), QPointF(14, 18));
        painter.drawLine(QPointF(8, 24), QPointF(14, 24));
        painter.setBrush(QColor(150, 185, 215));
        painter.drawRect(QRectF(14, 4, 12, 5));
        painter.drawRect(QRectF(14, 10, 12, 4));
        painter.drawRect(QRectF(14, 16, 12, 4));
        painter.drawRect(QRectF(14, 22, 12, 4));
        break;
    case RibbonIcon::Search:
        // Лупа живёт внутри белого поля поиска, поэтому она тёмная.
        painter.setPen(QPen(QColor(0x4a, 0x5a, 0x68), 2.4));
        painter.setBrush(Qt::NoBrush);
        painter.drawEllipse(QPointF(14, 14), 8.0, 8.0);
        painter.drawLine(QPointF(20, 20), QPointF(27, 27));
        break;
    case RibbonIcon::Help: {
        // Синий круг с белым знаком: значок один и тот же на синей шапке ленты и
        // на белом поле кнопок, поэтому одноцветный «?» не годится.
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(0x1b, 0x5b, 0x96));
        painter.drawEllipse(QRectF(2, 2, 28, 28));
        painter.setPen(header_text);
        QFont font = painter.font();
        font.setPointSizeF(17.0);
        font.setBold(true);
        painter.setFont(font);
        painter.drawText(QRectF(0, 0, 32, 32), Qt::AlignCenter, QStringLiteral("?"));
        break;
    }
    case RibbonIcon::Collapse:
        drawChevron(painter, true);
        break;
    case RibbonIcon::Expand:
        drawChevron(painter, false);
        break;
    case RibbonIcon::ComponentGroup:
        // Сборка: задний кубик светлее и меньше, передний — основной.
        drawModelCube(painter, QPointF(13, 11), 10, QColor(196, 214, 230));
        drawModelCube(painter, QPointF(4, 16), 11, QColor(150, 185, 215));
        break;
    case RibbonIcon::Component:
        drawModelCube(painter, QPointF(6, 12), 14, QColor(150, 185, 215));
        break;
    case RibbonIcon::SignalGroup:
        // Оси и две синусоиды: раздел сигналов возбуждения.
        painter.setPen(QPen(QColor(120, 130, 140), 1.4));
        painter.drawLine(QPointF(4, 27), QPointF(29, 27));
        painter.drawLine(QPointF(4, 27), QPointF(4, 4));
        drawSineWave(painter, 6.0, 29.0, 15.0, 9.0, QColor(200, 120, 40), 2.0);
        drawSineWave(painter, 6.0, 29.0, 15.0, 4.5, QColor(90, 150, 210), 1.6);
        break;
    case RibbonIcon::Signal:
        // Одиночный сигнал: нулевая линия, период синусоиды и метка запуска.
        painter.setPen(QPen(QColor(160, 168, 176), 1.0));
        painter.drawLine(QPointF(3, 14), QPointF(29, 14));
        drawSineWave(painter, 3.0, 26.0, 14.0, 8.5, QColor(200, 120, 40), 2.2);
        painter.setPen(QPen(QColor(40, 110, 50), 1.0));
        painter.setBrush(QColor(90, 190, 100));
        painter.drawPolygon(
            QPolygonF({QPointF(22, 21), QPointF(29, 25), QPointF(22, 29)}));
        break;
    case RibbonIcon::Home:
        // Домик вкладки «Старт».
        painter.setPen(QPen(QColor(70, 90, 110), 1.4));
        painter.setBrush(QColor(150, 185, 215));
        painter.drawPolygon(QPolygonF({QPointF(16, 5),
                                       QPointF(27, 15),
                                       QPointF(23, 15),
                                       QPointF(23, 27),
                                       QPointF(9, 27),
                                       QPointF(9, 15),
                                       QPointF(5, 15)}));
        painter.setBrush(QColor(250, 250, 252));
        painter.drawRect(QRectF(13.5, 18, 5, 9));
        break;
    case RibbonIcon::NewProject:
        // Лист документа с зелёным плюсом.
        painter.setPen(QPen(QColor(120, 130, 140), 1.3));
        painter.setBrush(QColor(252, 252, 252));
        painter.drawPolygon(QPolygonF({QPointF(7, 4),
                                       QPointF(19, 4),
                                       QPointF(25, 10),
                                       QPointF(25, 28),
                                       QPointF(7, 28)}));
        painter.setPen(QPen(QColor(40, 150, 80), 2.4, Qt::SolidLine, Qt::RoundCap));
        painter.drawLine(QPointF(16, 15), QPointF(16, 24));
        painter.drawLine(QPointF(11, 19.5), QPointF(21, 19.5));
        break;
    case RibbonIcon::OpenProject:
        // Открытая папка со стрелкой вверх (загрузка проекта).
        drawFolder(painter);
        painter.setPen(QPen(QColor(40, 110, 160), 2.2, Qt::SolidLine, Qt::RoundCap));
        painter.drawLine(QPointF(16, 26), QPointF(16, 15));
        painter.drawPolyline(
            QPolygonF({QPointF(11, 20), QPointF(16, 15), QPointF(21, 20)}));
        break;
    case RibbonIcon::Grid: {
        // Сетка в перспективе: клетки сходятся к горизонту, как пол сцены.
        painter.setPen(QPen(QColor(120, 140, 160), 1.1));
        for (int index = 0; index <= 4; ++index) {
            const double t = index / 4.0;
            const double y = 10.0 + 17.0 * t * t;             // сгущение к дальнему краю
            const double inset = 13.0 * (1.0 - t);            // сужение вдаль
            painter.drawLine(QPointF(3 + inset, y), QPointF(29 - inset, y));
            const double x_top = 16.0 + (index - 2) * 3.2;
            const double x_bottom = 16.0 + (index - 2) * 13.0 / 2.0;
            painter.drawLine(QPointF(x_top, 10), QPointF(x_bottom, 27));
        }
        break;
    }
    case RibbonIcon::Background: {
        // Половина светлая, половина тёмная — переключение фона сцены.
        painter.setPen(QPen(QColor(120, 130, 140), 1.2));
        painter.setBrush(QColor(250, 250, 252));
        painter.drawRect(QRectF(4, 7, 24, 18));
        painter.setBrush(QColor(0x1a, 0x1f, 0x26));
        painter.drawRect(QRectF(16, 7, 12, 18));
        painter.setPen(QPen(QColor(0x4c, 0x6a, 0x86), 1.2));
        painter.setBrush(Qt::NoBrush);
        painter.drawEllipse(QPointF(16, 16), 5.0, 5.0);
        break;
    }
    case RibbonIcon::MathHelp: {
        // Лист с формулой и синим знаком вопроса: справка именно по математике,
        // а не общая справка о программе (та — круглый «?» в шапке ленты).
        painter.setPen(QPen(QColor(120, 130, 140), 1.3));
        painter.setBrush(QColor(252, 252, 252));
        painter.drawPolygon(QPolygonF({QPointF(5, 3),
                                       QPointF(17, 3),
                                       QPointF(23, 9),
                                       QPointF(23, 27),
                                       QPointF(5, 27)}));
        // Условный «интеграл» и дробная черта — намёк на формулы внутри.
        painter.setPen(QPen(QColor(90, 110, 130), 1.3));
        painter.drawLine(QPointF(9, 9), QPointF(19, 9));
        painter.drawLine(QPointF(9, 13), QPointF(15, 13));
        painter.setPen(QPen(QColor(0x1b, 0x5b, 0x96), 1.6));
        QFont formula_font = painter.font();
        formula_font.setPointSizeF(9.0);
        formula_font.setItalic(true);
        painter.setFont(formula_font);
        painter.drawText(QRectF(7, 15, 14, 10), Qt::AlignLeft | Qt::AlignVCenter,
                         QStringLiteral("∫f"));

        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(0x1b, 0x5b, 0x96));
        painter.drawEllipse(QRectF(17, 17, 14, 14));
        painter.setPen(header_text);
        QFont mark_font = painter.font();
        mark_font.setPointSizeF(9.0);
        mark_font.setBold(true);
        mark_font.setItalic(false);
        painter.setFont(mark_font);
        painter.drawText(QRectF(17, 17, 14, 14), Qt::AlignCenter, QStringLiteral("?"));
        break;
    }
    case RibbonIcon::CloseProject:
        // Лист документа с красным крестиком.
        painter.setPen(QPen(QColor(120, 130, 140), 1.3));
        painter.setBrush(QColor(252, 252, 252));
        painter.drawPolygon(QPolygonF({QPointF(7, 4),
                                       QPointF(19, 4),
                                       QPointF(25, 10),
                                       QPointF(25, 28),
                                       QPointF(7, 28)}));
        painter.setPen(QPen(QColor(190, 60, 60), 2.4, Qt::SolidLine, Qt::RoundCap));
        painter.drawLine(QPointF(12, 15), QPointF(20, 24));
        painter.drawLine(QPointF(20, 15), QPointF(12, 24));
        break;
    }
}

QIcon closeCrossIcon(int size = 12, const QColor &color = QColor(0x40, 0x40, 0x40))
{
    QPixmap pixmap(size, size);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(QPen(color, size / 8.0 + 0.6));
    const double inset = size * 0.25;
    painter.drawLine(QPointF(inset, inset), QPointF(size - inset, size - inset));
    painter.drawLine(QPointF(size - inset, inset), QPointF(inset, size - inset));
    painter.end();
    return QIcon(pixmap);
}

// Крестик на вкладке проекта: серый, под курсором — красный, как в браузерах и
// в CST. Стандартный close-button у QTabBar пришлось бы задавать картинкой из
// ресурсов, которых у проекта нет.
class TabCloseButton : public QToolButton
{
public:
    explicit TabCloseButton(QWidget *parent = nullptr)
        : QToolButton(parent)
    {
        setObjectName(QStringLiteral("cstTabCloseButton"));
        setIcon(closeCrossIcon(11));
        setIconSize(QSize(11, 11));
        setFixedSize(16, 16);
        setAutoRaise(true);
        setCursor(Qt::ArrowCursor);
        setToolTip(QStringLiteral("Закрыть проект"));
    }

protected:
    void enterEvent(QEnterEvent *event) override
    {
        setIcon(closeCrossIcon(11, QColor(0xff, 0xff, 0xff)));
        QToolButton::enterEvent(event);
    }

    void leaveEvent(QEvent *event) override
    {
        setIcon(closeCrossIcon(11));
        QToolButton::leaveEvent(event);
    }
};

// Перенос строки нужен только крупным кнопкам: в мелкой кнопке и в подсказке
// подпись идёт одной строкой.
QString singleLineText(const QAction *action)
{
    QString text = action->text();
    text.replace(QLatin1Char('\n'), QLatin1Char(' '));
    return text.simplified();
}

// Подпись команды для строки поиска.
QString actionSearchText(const QAction *action)
{
    QString text = singleLineText(action);
    text.remove(QLatin1Char('&'));
    return text;
}

RibbonBar *findRibbonBar(QWidget *widget)
{
    for (QWidget *parent = widget; parent != nullptr; parent = parent->parentWidget()) {
        if (RibbonBar *bar = qobject_cast<RibbonBar *>(parent)) {
            return bar;
        }
    }
    return nullptr;
}
}

QIcon ribbonIcon(RibbonIcon icon, int size)
{
    QPixmap pixmap(size, size);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.scale(size / static_cast<double>(icon_size), size / static_cast<double>(icon_size));
    paintRibbonIcon(painter, icon);
    painter.end();
    return QIcon(pixmap);
}

RibbonGroup::RibbonGroup(const QString &title, QWidget *parent)
    : QWidget(parent)
{
    QVBoxLayout *outer = new QVBoxLayout(this);
    outer->setContentsMargins(5, 3, 5, 1);
    outer->setSpacing(1);

    QWidget *content = new QWidget(this);
    content_layout_ = new QHBoxLayout(content);
    content_layout_->setContentsMargins(0, 0, 0, 0);
    content_layout_->setSpacing(2);
    outer->addWidget(content, 1);

    QLabel *caption = new QLabel(title, this);
    caption->setAlignment(Qt::AlignHCenter | Qt::AlignVCenter);
    caption->setObjectName(QStringLiteral("cstRibbonGroupCaption"));
    outer->addWidget(caption);
}

void RibbonGroup::paintEvent(QPaintEvent *)
{
    // Разделитель справа от группы — та же вертикальная линия, что отделяет
    // блоки ленты в CST.
    QPainter painter(this);
    painter.setPen(group_separator);
    painter.drawLine(width() - 1, 3, width() - 1, height() - 5);
}

void RibbonGroup::registerForSearch(QAction *action)
{
    if (RibbonBar *bar = findRibbonBar(this)) {
        bar->registerSearchAction(action);
    }
}

QToolButton *RibbonGroup::addLargeButton(QAction *action, RibbonIcon icon, QMenu *menu)
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
    if (menu != nullptr) {
        // Меню остаётся всплывающим окном и принадлежит вызывающей стороне:
        // сделать его потомком кнопки нельзя — Qt нарисует пункты прямо на
        // ленте вместо выпадающего списка.
        button->setMenu(menu);
        button->setPopupMode(QToolButton::MenuButtonPopup);
    }
    content_layout_->addWidget(button, 0, Qt::AlignTop);
    small_column_ = nullptr;
    small_button_count_ = 0;
    icon_grid_ = nullptr;
    icon_button_count_ = 0;
    registerForSearch(action);
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
        icon_grid_ = nullptr;
        icon_button_count_ = 0;
    }
    return small_column_;
}

QGridLayout *RibbonGroup::iconGrid()
{
    if (icon_grid_ == nullptr) {
        icon_grid_ = new QGridLayout();
        icon_grid_->setContentsMargins(0, 0, 0, 0);
        icon_grid_->setSpacing(1);
        content_layout_->addLayout(icon_grid_);
        content_layout_->setAlignment(icon_grid_, Qt::AlignVCenter);
        icon_button_count_ = 0;
        small_column_ = nullptr;
        small_button_count_ = 0;
    }
    return icon_grid_;
}

QToolButton *RibbonGroup::addSmallButton(QAction *action, RibbonIcon icon)
{
    action->setIcon(ribbonIcon(icon));
    QToolButton *button = new QToolButton(this);
    button->setDefaultAction(action);
    button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    button->setIconSize(QSize(16, 16));
    button->setAutoRaise(true);
    // Двухстрочная подпись крупной кнопки растянула бы ряд мелких кнопок втрое,
    // поэтому здесь она разворачивается в строку и следит за сменой текста.
    const auto apply_single_line = [button, action]() {
        button->setText(singleLineText(action));
    };
    apply_single_line();
    connect(action, &QAction::changed, button, apply_single_line);
    QVBoxLayout *column = smallButtonColumn();
    column->insertWidget(small_button_count_, button);
    ++small_button_count_;
    registerForSearch(action);
    return button;
}

QToolButton *RibbonGroup::addIconButton(QAction *action, RibbonIcon icon)
{
    action->setIcon(ribbonIcon(icon));
    QToolButton *button = new QToolButton(this);
    button->setDefaultAction(action);
    button->setToolButtonStyle(Qt::ToolButtonIconOnly);
    button->setIconSize(QSize(grid_icon_size, grid_icon_size));
    button->setAutoRaise(true);
    button->setFixedSize(grid_icon_size + 6, grid_icon_size + 6);
    // Подпись у кнопки не видна, поэтому название команды уходит в подсказку.
    button->setToolTip(action->toolTip().isEmpty()
                           ? actionSearchText(action)
                           : QStringLiteral("%1 — %2")
                                 .arg(actionSearchText(action), action->toolTip()));

    QGridLayout *grid = iconGrid();
    grid->addWidget(button, icon_button_count_ % 2, icon_button_count_ / 2);
    ++icon_button_count_;
    registerForSearch(action);
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

    // Синяя полоса: слева вкладки, справа поиск команд и кнопки сворачивания
    // ленты и справки — как шапка CST Studio Suite.
    QWidget *header = new QWidget(this);
    header->setObjectName(QStringLiteral("cstRibbonHeader"));
    QHBoxLayout *header_layout = new QHBoxLayout(header);
    header_layout->setContentsMargins(2, 0, 6, 0);
    header_layout->setSpacing(4);

    tab_bar_ = new QTabBar(header);
    tab_bar_->setExpanding(false);
    tab_bar_->setDrawBase(false);
    tab_bar_->setObjectName(QStringLiteral("cstRibbonTabBar"));
    header_layout->addWidget(tab_bar_, 0, Qt::AlignBottom);
    header_layout->addStretch(1);

    search_edit_ = new QLineEdit(header);
    search_edit_->setObjectName(QStringLiteral("cstRibbonSearch"));
    search_edit_->setPlaceholderText(QStringLiteral("Поиск команды (Alt+Q)"));
    search_edit_->setFixedWidth(190);
    search_edit_->addAction(ribbonIcon(RibbonIcon::Search, 14), QLineEdit::TrailingPosition);
    header_layout->addWidget(search_edit_, 0, Qt::AlignVCenter);

    search_model_ = new QStringListModel(this);
    QCompleter *completer = new QCompleter(search_model_, this);
    completer->setCaseSensitivity(Qt::CaseInsensitive);
    completer->setFilterMode(Qt::MatchContains);
    completer->setCompletionMode(QCompleter::PopupCompletion);
    search_edit_->setCompleter(completer);
    connect(completer,
            qOverload<const QString &>(&QCompleter::activated),
            this,
            &RibbonBar::activateSearchResult);
    connect(search_edit_, &QLineEdit::returnPressed, this, [this]() {
        activateSearchResult(search_edit_->text());
    });

    QShortcut *search_shortcut = new QShortcut(QKeySequence(Qt::ALT | Qt::Key_Q), this);
    connect(search_shortcut, &QShortcut::activated, this, [this]() {
        search_edit_->setFocus();
        search_edit_->selectAll();
    });

    collapse_button_ = new QToolButton(header);
    collapse_button_->setObjectName(QStringLiteral("cstRibbonHeaderButton"));
    collapse_button_->setIconSize(QSize(12, 12));
    collapse_button_->setAutoRaise(true);
    header_layout->addWidget(collapse_button_, 0, Qt::AlignVCenter);

    QToolButton *help_button = new QToolButton(header);
    help_button->setObjectName(QStringLiteral("cstRibbonHeaderButton"));
    help_button->setIcon(ribbonIcon(RibbonIcon::Help, 12));
    help_button->setIconSize(QSize(12, 12));
    help_button->setAutoRaise(true);
    help_button->setToolTip(QStringLiteral("О программе и горячие клавиши"));
    header_layout->addWidget(help_button, 0, Qt::AlignVCenter);
    connect(help_button, &QToolButton::clicked, this, &RibbonBar::helpRequested);

    pages_ = new QStackedWidget(this);
    pages_->setObjectName(QStringLiteral("cstRibbonPages"));

    // Документная строка под лентой: вкладка «Старт», вкладки открытых проектов
    // и кнопка «+». В CST здесь показано имя одного проекта — тут это
    // полноценные вкладки, между которыми можно переключаться.
    QWidget *document_row = new QWidget(this);
    document_row->setObjectName(QStringLiteral("cstDocumentRow"));
    QHBoxLayout *document_layout = new QHBoxLayout(document_row);
    document_layout->setContentsMargins(0, 0, 0, 0);
    document_layout->setSpacing(0);

    document_tab_bar_ = new QTabBar(document_row);
    document_tab_bar_->setObjectName(QStringLiteral("cstDocumentTabBar"));
    document_tab_bar_->setExpanding(false);
    document_tab_bar_->setDrawBase(false);
    document_tab_bar_->setUsesScrollButtons(true);
    document_tab_bar_->setElideMode(Qt::ElideRight);
    // Крестики ставятся вручную (см. addProjectTab): у вкладки «Старт» его нет,
    // а нарисованный кодом значок не требует файла ресурсов.
    document_tab_bar_->addTab(ribbonIcon(RibbonIcon::Home, 14), QStringLiteral("Старт"));
    document_layout->addWidget(document_tab_bar_, 0);

    new_project_button_ = new QToolButton(document_row);
    new_project_button_->setObjectName(QStringLiteral("cstNewProjectButton"));
    new_project_button_->setText(QStringLiteral("+"));
    new_project_button_->setToolTip(QStringLiteral("Новый проект (Ctrl+N)"));
    new_project_button_->setAutoRaise(true);
    document_layout->addWidget(new_project_button_, 0, Qt::AlignVCenter);
    document_layout->addStretch(1);

    layout->addWidget(header);
    layout->addWidget(pages_);
    layout->addWidget(document_row);

    connect(tab_bar_, &QTabBar::currentChanged, pages_, &QStackedWidget::setCurrentIndex);
    connect(collapse_button_, &QToolButton::clicked, this, [this]() {
        setRibbonCollapsed(pages_->isVisible());
    });
    // Свёрнутая лента разворачивается щелчком по вкладке, как в CST.
    connect(tab_bar_, &QTabBar::tabBarClicked, this, [this]() {
        if (!pages_->isVisible()) {
            setRibbonCollapsed(false);
        }
    });
    connect(document_tab_bar_,
            &QTabBar::currentChanged,
            this,
            &RibbonBar::handleDocumentTabChanged);
    connect(new_project_button_, &QToolButton::clicked, this, &RibbonBar::newProjectRequested);
    setRibbonCollapsed(false);
}

void RibbonBar::handleDocumentTabChanged(int index)
{
    if (index <= 0) {
        emit startPageRequested();
    } else {
        emit projectActivated(index - 1);
    }
}

int RibbonBar::projectTabCount() const
{
    // Все вкладки, кроме постоянной «Старт».
    return std::max(0, document_tab_bar_->count() - 1);
}

int RibbonBar::addProjectTab(const QString &name)
{
    const QSignalBlocker blocker(document_tab_bar_);
    const int tab_index = document_tab_bar_->addTab(ribbonIcon(RibbonIcon::Project, 14), name);

    // Индекс вкладки сдвигается при закрытии соседних, поэтому кнопка ищет свою
    // вкладку по себе самой, а не помнит номер.
    TabCloseButton *close_button = new TabCloseButton(document_tab_bar_);
    document_tab_bar_->setTabButton(tab_index, QTabBar::RightSide, close_button);
    connect(close_button, &QToolButton::clicked, this, [this, close_button]() {
        for (int index = 1; index < document_tab_bar_->count(); ++index) {
            if (document_tab_bar_->tabButton(index, QTabBar::RightSide) == close_button) {
                emit projectCloseRequested(index - 1);
                return;
            }
        }
    });
    return tab_index - 1;
}

void RibbonBar::removeProjectTab(int project_index)
{
    const QSignalBlocker blocker(document_tab_bar_);
    document_tab_bar_->removeTab(project_index + 1);
}

void RibbonBar::setProjectTabName(int project_index, const QString &name)
{
    document_tab_bar_->setTabText(project_index + 1, name);
}

void RibbonBar::setProjectTabToolTip(int project_index, const QString &text)
{
    document_tab_bar_->setTabToolTip(project_index + 1, text);
}

void RibbonBar::setActiveProjectTab(int project_index)
{
    const QSignalBlocker blocker(document_tab_bar_);
    document_tab_bar_->setCurrentIndex(project_index < 0 ? 0 : project_index + 1);
}

void RibbonBar::setRibbonCollapsed(bool collapsed)
{
    pages_->setVisible(!collapsed);
    collapse_button_->setIcon(
        ribbonIcon(collapsed ? RibbonIcon::Expand : RibbonIcon::Collapse, 12));
    collapse_button_->setToolTip(collapsed ? QStringLiteral("Развернуть ленту")
                                           : QStringLiteral("Свернуть ленту"));
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

void RibbonBar::registerSearchAction(QAction *action)
{
    if (action == nullptr || search_actions_.contains(action)) {
        return;
    }

    search_actions_.append(action);
    QStringList names;
    names.reserve(search_actions_.size());
    for (const QAction *known : std::as_const(search_actions_)) {
        names.append(actionSearchText(known));
    }
    search_model_->setStringList(names);
}

void RibbonBar::activateSearchResult(const QString &text)
{
    const QString wanted = text.simplified();
    for (QAction *action : std::as_const(search_actions_)) {
        if (actionSearchText(action).compare(wanted, Qt::CaseInsensitive) == 0) {
            search_edit_->clear();
            search_edit_->clearFocus();
            action->trigger();
            return;
        }
    }
}

CstPanelHeader::CstPanelHeader(const QString &title, QWidget *parent)
    : QWidget(parent)
    , title_(title)
{
    setFixedHeight(20);
    QHBoxLayout *layout = new QHBoxLayout(this);
    layout->setContentsMargins(6, 0, 2, 0);
    layout->addStretch(1);

    close_button_ = new QToolButton(this);
    close_button_->setObjectName(QStringLiteral("cstPanelCloseButton"));
    close_button_->setIcon(closeCrossIcon());
    close_button_->setIconSize(QSize(12, 12));
    close_button_->setFixedSize(16, 16);
    close_button_->setAutoRaise(true);
    close_button_->setVisible(false);
    close_button_->setToolTip(QStringLiteral("Скрыть панель"));
    layout->addWidget(close_button_);
    connect(close_button_, &QToolButton::clicked, this, &CstPanelHeader::closeRequested);
}

void CstPanelHeader::setClosable(bool closable)
{
    close_button_->setVisible(closable);
}

void CstPanelHeader::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.fillRect(rect(), panel_caption_bg);
    painter.setPen(panel_border);
    painter.drawLine(0, height() - 1, width(), height() - 1);
    painter.setPen(caption_text);
    painter.drawText(rect().adjusted(6, 0, -20, 0), Qt::AlignVCenter | Qt::AlignLeft, title_);
}

CstPanel::CstPanel(const QString &title, QWidget *parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("cstPanel"));
    // Рамку панели рисует таблица стилей, а наследник QWidget получает её
    // только с этим атрибутом.
    setAttribute(Qt::WA_StyledBackground, true);
    layout_ = new QVBoxLayout(this);
    layout_->setContentsMargins(0, 0, 0, 0);
    layout_->setSpacing(0);

    header_ = new CstPanelHeader(title, this);
    layout_->addWidget(header_);
    connect(header_, &CstPanelHeader::closeRequested, this, &CstPanel::closeRequested);
}

void CstPanel::setContent(QWidget *content)
{
    content->setParent(this);
    layout_->addWidget(content, 1);
}

void CstPanel::setClosable(bool closable)
{
    header_->setClosable(closable);
}
