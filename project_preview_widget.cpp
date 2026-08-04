#include "project_preview_widget.h"

#include <QtGui/QPainter>
#include <QtGui/QPainterPath>
#include <QtGui/QPolygonF>

#include <algorithm>
#include <cmath>

namespace
{
constexpr double pi = 3.14159265358979323846;
// Наклон камеры фиксирован: вращается только азимут, как у медленно
// поворачивающейся детали на витрине.
constexpr double elevation_deg = 22.0;
constexpr int frame_interval_ms = 40;   // 25 кадров в секунду
constexpr double degrees_per_frame = 0.7;

const QColor shell_color(0x7f, 0x9a, 0xb5);
const QColor shell_edge(0x4c, 0x6a, 0x86);
const QColor body_color(0xe0, 0x9c, 0x46);
const QColor iris_color(0xc8, 0xd4, 0xdf);
const QColor axis_color(0x9a, 0xa8, 0xb4);

// Рёбра параллелепипеда по его половинным размерам.
void appendBoxEdges(QVector<QPair<QVector3D, QVector3D>> *edges,
                    const QVector3D &center,
                    const QVector3D &half)
{
    const QVector3D corners[8] = {
        center + QVector3D(-half.x(), -half.y(), -half.z()),
        center + QVector3D(half.x(), -half.y(), -half.z()),
        center + QVector3D(half.x(), half.y(), -half.z()),
        center + QVector3D(-half.x(), half.y(), -half.z()),
        center + QVector3D(-half.x(), -half.y(), half.z()),
        center + QVector3D(half.x(), -half.y(), half.z()),
        center + QVector3D(half.x(), half.y(), half.z()),
        center + QVector3D(-half.x(), half.y(), half.z()),
    };
    static const int pairs[12][2] = {{0, 1}, {1, 2}, {2, 3}, {3, 0},
                                     {4, 5}, {5, 6}, {6, 7}, {7, 4},
                                     {0, 4}, {1, 5}, {2, 6}, {3, 7}};
    for (const auto &pair : pairs) {
        edges->append({corners[pair[0]], corners[pair[1]]});
    }
}
}

ProjectPreviewWidget::ProjectPreviewWidget(QWidget *parent)
    : QWidget(parent)
{
    setMinimumSize(96, 68);
    setAttribute(Qt::WA_StyledBackground, false);
    timer_.setInterval(frame_interval_ms);
    connect(&timer_, &QTimer::timeout, this, [this]() {
        azimuth_deg_ = std::fmod(azimuth_deg_ + degrees_per_frame, 360.0);
        update();
    });
}

void ProjectPreviewWidget::setModel(const WaveguideParameters &parameters)
{
    parameters_ = parameters;
    has_model_ = true;

    // Масштаб подбирается по габаритам модели, чтобы длинный волновод и
    // короткий занимали карточку одинаково полно.
    const double half_length = 0.5 * std::max(1.0, parameters.length_mm);
    const double half_width = parameters.cross_section == 1
                                  ? std::max(1.0, parameters.radius_mm)
                                  : 0.5 * std::max(1.0, parameters.width_mm);
    const double half_depth = parameters.cross_section == 1
                                  ? std::max(1.0, parameters.radius_mm)
                                  : 0.5 * std::max(1.0, parameters.depth_mm);
    scale_ = std::max({half_length, half_width, half_depth});
    update();
}

void ProjectPreviewWidget::setSpinning(bool spinning)
{
    if (spinning && isVisible()) {
        timer_.start();
    } else {
        timer_.stop();
    }
}

void ProjectPreviewWidget::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    timer_.start();
}

void ProjectPreviewWidget::hideEvent(QHideEvent *event)
{
    QWidget::hideEvent(event);
    timer_.stop();
}

// Изометрия: поворот вокруг вертикальной оси на azimuth_deg_, затем наклон.
QPointF ProjectPreviewWidget::project(const QVector3D &point) const
{
    const double azimuth = azimuth_deg_ * pi / 180.0;
    const double elevation = elevation_deg * pi / 180.0;
    const double x = point.x() * std::cos(azimuth) + point.z() * std::sin(azimuth);
    const double z = -point.x() * std::sin(azimuth) + point.z() * std::cos(azimuth);
    const double y = point.y();
    return QPointF(x, -(y * std::cos(elevation) + z * std::sin(elevation)));
}

void ProjectPreviewWidget::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    // Подложка карточки: тёмная сцена, как в самом трёхмерном виде.
    const QRectF area = rect().adjusted(0.5, 0.5, -0.5, -0.5);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(0x10, 0x12, 0x16));
    painter.drawRoundedRect(area, 3, 3);

    if (!has_model_) {
        return;
    }

    // Единый масштаб экрана: модель вписывается в карточку с полями.
    const double pixels = 0.42 * std::min(width(), height()) / std::max(1.0e-6, scale_) * 1.9;
    painter.translate(width() / 2.0, height() / 2.0);

    const auto to_screen = [&](const QVector3D &point) {
        const QPointF flat = project(point);
        return QPointF(flat.x() * pixels, flat.y() * pixels);
    };

    QVector<QPair<QVector3D, QVector3D>> shell_edges;
    const double half_length = 0.5 * parameters_.length_mm;
    if (parameters_.cross_section == 1) {
        // Круглый тракт: два кольца по торцам и образующие между ними.
        const double radius = parameters_.radius_mm;
        constexpr int segments = 24;
        QPolygonF front;
        QPolygonF back;
        for (int index = 0; index <= segments; ++index) {
            const double angle = 2.0 * pi * index / segments;
            const QVector3D offset(radius * std::cos(angle), radius * std::sin(angle), 0.0f);
            front.append(to_screen(offset + QVector3D(0, 0, -half_length)));
            back.append(to_screen(offset + QVector3D(0, 0, half_length)));
        }
        painter.setPen(QPen(shell_edge, 1.2));
        painter.setBrush(QColor(shell_color.red(), shell_color.green(), shell_color.blue(), 40));
        painter.drawPolygon(front);
        painter.drawPolygon(back);
        painter.setPen(QPen(shell_color, 1.0));
        for (int index = 0; index < segments; index += 6) {
            painter.drawLine(front.at(index), back.at(index));
        }
    } else {
        appendBoxEdges(&shell_edges,
                       QVector3D(0, 0, 0),
                       QVector3D(0.5f * static_cast<float>(parameters_.width_mm),
                                 0.5f * static_cast<float>(parameters_.depth_mm),
                                 static_cast<float>(half_length)));
        painter.setPen(QPen(shell_color, 1.1));
        for (const auto &edge : shell_edges) {
            painter.drawLine(to_screen(edge.first), to_screen(edge.second));
        }
    }

    // Ось тракта: сразу видно, куда «течёт» волна.
    painter.setPen(QPen(axis_color, 0.9, Qt::DashLine));
    painter.drawLine(to_screen(QVector3D(0, 0, -static_cast<float>(half_length))),
                     to_screen(QVector3D(0, 0, static_cast<float>(half_length))));

    // Пластины и диафрагмы — заполненные четырёхугольники в плоскости XY.
    for (const PecPlateParameters &plate : parameters_.pec_plates) {
        if (!plate.enabled) {
            continue;
        }
        const float z = 0.5f * static_cast<float>(plate.z_min_mm + plate.z_max_mm);
        const QPolygonF face({
            to_screen(QVector3D(static_cast<float>(plate.x_min_mm),
                                static_cast<float>(plate.y_min_mm), z)),
            to_screen(QVector3D(static_cast<float>(plate.x_max_mm),
                                static_cast<float>(plate.y_min_mm), z)),
            to_screen(QVector3D(static_cast<float>(plate.x_max_mm),
                                static_cast<float>(plate.y_max_mm), z)),
            to_screen(QVector3D(static_cast<float>(plate.x_min_mm),
                                static_cast<float>(plate.y_max_mm), z)),
        });
        painter.setPen(QPen(iris_color.darker(140), 1.0));
        painter.setBrush(QColor(iris_color.red(), iris_color.green(), iris_color.blue(), 150));
        painter.drawPolygon(face);

        if (plate.aperture_enabled) {
            // Окно диафрагмы вырезаем визуально — рисуем его цветом сцены.
            painter.setBrush(QColor(0x10, 0x12, 0x16));
            const float cx = static_cast<float>(plate.aperture_offset_x_mm);
            const float cy = static_cast<float>(plate.aperture_offset_y_mm);
            if (plate.aperture_shape == 1) {
                const float r = static_cast<float>(plate.aperture_radius_mm);
                QPolygonF hole;
                for (int index = 0; index <= 20; ++index) {
                    const double angle = 2.0 * pi * index / 20;
                    hole.append(to_screen(QVector3D(cx + r * std::cos(angle),
                                                    cy + r * std::sin(angle),
                                                    z)));
                }
                painter.drawPolygon(hole);
            } else {
                const float hw = 0.5f * static_cast<float>(plate.aperture_width_mm);
                const float hh = 0.5f * static_cast<float>(plate.aperture_height_mm);
                painter.drawPolygon(QPolygonF({
                    to_screen(QVector3D(cx - hw, cy - hh, z)),
                    to_screen(QVector3D(cx + hw, cy - hh, z)),
                    to_screen(QVector3D(cx + hw, cy + hh, z)),
                    to_screen(QVector3D(cx - hw, cy + hh, z)),
                }));
            }
        }
    }

    // Свободные тела — каркасом, чтобы не спорить с пластинами по яркости.
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(body_color, 1.1));
    for (const ShapeParameters &shape : parameters_.shapes) {
        if (!shape.enabled) {
            continue;
        }
        QVector<QPair<QVector3D, QVector3D>> edges;
        const QVector3D center(static_cast<float>(shape.center_x_mm),
                               static_cast<float>(shape.center_y_mm),
                               static_cast<float>(shape.center_z_mm));
        const double radial = shape.kind == 1 ? shape.radius_mm : 0.5 * shape.size_x_mm;
        const QVector3D half(static_cast<float>(shape.kind == 1 ? radial : 0.5 * shape.size_x_mm),
                             static_cast<float>(shape.kind == 1 ? radial : 0.5 * shape.size_y_mm),
                             static_cast<float>(shape.kind == 1 ? 0.5 * shape.length_mm
                                                                : 0.5 * shape.size_z_mm));
        appendBoxEdges(&edges, center, half);
        for (const auto &edge : edges) {
            painter.drawLine(to_screen(edge.first), to_screen(edge.second));
        }
    }

    // Щель в стенке — короткая яркая риска.
    if (parameters_.slot_enabled) {
        const float half_slot = 0.5f * static_cast<float>(parameters_.slot_length_mm);
        const float x = static_cast<float>(parameters_.slot_offset_x_mm);
        const float y = 0.5f * static_cast<float>(parameters_.depth_mm);
        const float z = static_cast<float>(parameters_.slot_offset_z_mm);
        painter.setPen(QPen(QColor(0xd6, 0x4b, 0x40), 2.0));
        painter.drawLine(to_screen(QVector3D(x, y, z - half_slot)),
                         to_screen(QVector3D(x, y, z + half_slot)));
    }
}
