#include "waveguide_opengl_widget.h"

#include <QtCore/QRect>
#include <QtGui/QKeyEvent>
#include <QtGui/QMouseEvent>
#include <QtGui/QPainterPath>
#include <QtGui/QPolygonF>
#include <QtGui/QWheelEvent>
#include <QtGui/qopengl.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace
{
constexpr double pi = 3.14159265358979323846;
constexpr double min_camera_factor = 0.24;
constexpr double max_camera_factor = 10.0;

QVector3D safeNormal(const QVector3D &vector, const QVector3D &fallback)
{
    if (vector.lengthSquared() < 1.0e-8f) {
        return fallback;
    }

    return vector.normalized();
}

// Разбиение простого многоугольника на треугольники отсечением ушей. Торец
// призмы бывает невыпуклым — буквы C, Г, T именно такие, — и веером из одной
// точки он закрывается неверно: часть треугольников ложится поверх выреза, и
// тело выглядит то дырявым, то залитым насквозь.
//
// Контуры сюда приходят из булевых операций, обрезки по полости и врезки дырок
// мостиками, поэтому обязательны три вещи, на которых наивное отсечение глохнет
// и оставляет область незалитой (пластина с C-вырезом рисовалась без внешнего
// кольца металла — «металл только в островке»):
//   - коллинеарные вершины (булева операция дробит прямые стороны) — ухо
//     нулевой площади не срезается никогда, они выбрасываются отдельно;
//   - сдвоенные вершины мостиков (bridgeHoles проходит разрез дважды) — они
//     лежат ровно на сторонах соседних ушей, и нестрогая проверка «внутри»
//     блокировала любое ухо рядом с мостиком; принадлежность считается строго;
//   - допуски по площади, а не точные нули: после пересечений координаты
//     совпадают лишь с точностью округления.
QVector<int> triangulateProfile(const QVector<QPointF> &profile)
{
    QVector<int> triangles;
    const int count = profile.size();
    if (count < 3) {
        return triangles;
    }
    const auto cross = [](const QPointF &origin, const QPointF &first, const QPointF &second) {
        return (first.x() - origin.x()) * (second.y() - origin.y()) -
               (first.y() - origin.y()) * (second.x() - origin.x());
    };
    double reach = 0.0;
    for (const QPointF &point : profile) {
        reach = std::max({reach, std::abs(point.x()), std::abs(point.y())});
    }
    const double area_epsilon = std::max(1.0e-12, reach * reach * 1.0e-12);

    double signed_area = 0.0;
    for (int index = 0; index < count; ++index) {
        const QPointF &from = profile[index];
        const QPointF &to = profile[(index + 1) % count];
        signed_area += from.x() * to.y() - to.x() * from.y();
    }
    // Обход приводится к положительному: дальше «ухо» распознаётся по знаку
    // векторного произведения, и для обратного обхода знак был бы другим.
    QVector<int> remaining;
    remaining.reserve(count);
    for (int index = 0; index < count; ++index) {
        remaining.push_back(signed_area >= 0.0 ? index : count - 1 - index);
    }

    int guard = 0;
    const int guard_limit = 8 * count + 16;
    while (remaining.size() > 3 && guard++ < guard_limit) {
        bool clipped = false;
        // Сначала вершины нулевой площади: и коллинеарные, и точки-дубли
        // мостиков просто выбрасываются, треугольника они не дают.
        for (int position = 0; position < remaining.size(); ++position) {
            const QPointF &a =
                profile[remaining[(position + remaining.size() - 1) % remaining.size()]];
            const QPointF &b = profile[remaining[position]];
            const QPointF &c = profile[remaining[(position + 1) % remaining.size()]];
            if (std::abs(cross(a, b, c)) <= area_epsilon) {
                remaining.removeAt(position);
                clipped = true;
                break;
            }
        }
        if (clipped) {
            continue;
        }
        for (int position = 0; position < remaining.size(); ++position) {
            const int previous = remaining[(position + remaining.size() - 1) % remaining.size()];
            const int current = remaining[position];
            const int next = remaining[(position + 1) % remaining.size()];
            const QPointF &a = profile[previous];
            const QPointF &b = profile[current];
            const QPointF &c = profile[next];
            if (cross(a, b, c) <= area_epsilon) {
                continue;   // вершина вогнутая — ухом быть не может
            }
            bool contains_other = false;
            for (const int index : remaining) {
                if (index == previous || index == current || index == next) {
                    continue;
                }
                // Строго внутри: точка на стороне уха (дубль вершины мостика)
                // уху не мешает.
                const QPointF &point = profile[index];
                if (cross(a, b, point) > area_epsilon && cross(b, c, point) > area_epsilon &&
                    cross(c, a, point) > area_epsilon) {
                    contains_other = true;
                    break;
                }
            }
            if (contains_other) {
                continue;
            }
            triangles << previous << current << next;
            remaining.removeAt(position);
            clipped = true;
            break;
        }
        if (!clipped) {
            break;   // профиль с самопересечением: закрываем тем, что уже есть
        }
    }
    if (remaining.size() == 3) {
        triangles << remaining[0] << remaining[1] << remaining[2];
    }
    return triangles;
}

// Точка строго внутри многоугольника — центр первого попавшегося уха. Первая
// вершина контура для проверок вложенности не годится: после булевых операций
// она часто лежит ровно на границе соседнего контура, и чёт-нечет отвечает
// случайным образом.
QPointF polygonInteriorPoint(const QPolygonF &polygon)
{
    const int count = polygon.size();
    if (count < 3) {
        return polygon.isEmpty() ? QPointF() : polygon.first();
    }
    const QVector<QPointF> points(polygon.begin(), polygon.end());
    const QVector<int> triangles = triangulateProfile(points);
    if (triangles.size() >= 3) {
        const QPointF &a = points[triangles[0]];
        const QPointF &b = points[triangles[1]];
        const QPointF &c = points[triangles[2]];
        return QPointF((a.x() + b.x() + c.x()) / 3.0, (a.y() + b.y() + c.y()) / 3.0);
    }
    QPointF sum;
    for (const QPointF &point : polygon) {
        sum += point;
    }
    return sum / count;
}

// Плоское тело, приведённое к «профиль плюс толщина вдоль оси». В таком виде
// булевы операции над телами одной оси и одной толщины считаются как обычные
// операции над плоскими контурами, и вырез виден в кадре дыркой, а не рамкой
// поверх целого металла.
struct PlanarSolid
{
    int axis = 2;              // 0 — X, 1 — Y, 2 — Z
    double axial_min_mm = 0.0;
    double axial_max_mm = 0.0;
    QPolygonF outline;         // в мировых координатах плоскости (u, v)
};

// Центр тела в координатах плоскости выбранной оси. Порядок (u, v) тот же, что
// у сеточного генератора: ось Z — (x, y), ось Y — (z, x), ось X — (y, z).
QPointF planarCenter(int axis, const ShapeParameters &shape)
{
    switch (axis) {
    case 0:
        return QPointF(shape.center_y_mm, shape.center_z_mm);
    case 1:
        return QPointF(shape.center_z_mm, shape.center_x_mm);
    default:
        return QPointF(shape.center_x_mm, shape.center_y_mm);
    }
}

double planarAxialCenter(int axis, const ShapeParameters &shape)
{
    switch (axis) {
    case 0:
        return shape.center_x_mm;
    case 1:
        return shape.center_y_mm;
    default:
        return shape.center_z_mm;
    }
}

// Приведение тела к плоскому виду. Поворот вокруг собственной оси допустим — он
// разворачивает профиль внутри его же плоскости; поворот вокруг любой из двух
// других осей выводит профиль из плоскости, и плоским контуром такое тело уже
// не описать.
bool toPlanarSolid(const ShapeParameters &shape, PlanarSolid *solid)
{
    int axis = 2;
    double thickness_mm = 0.0;
    QPolygonF outline;
    switch (shape.kind) {
    case 1: {   // цилиндр: профиль — окружность в плоскости своей оси
        if (!(shape.radius_mm > 0.0) || !(shape.length_mm > 0.0)) {
            return false;
        }
        axis = std::clamp(shape.axis, 0, 2);
        thickness_mm = shape.length_mm;
        constexpr int segments = 48;
        for (int segment = 0; segment < segments; ++segment) {
            const double angle = 2.0 * pi * segment / segments;
            outline << QPointF(shape.radius_mm * std::cos(angle),
                               shape.radius_mm * std::sin(angle));
        }
        break;
    }
    case 2: {   // призма: профиль задан пользователем
        if (!(shape.length_mm > 0.0) || shape.profile_mm.size() < 3) {
            return false;
        }
        axis = std::clamp(shape.axis, 0, 2);
        thickness_mm = shape.length_mm;
        for (const QPointF &point : shape.profile_mm) {
            outline << point;
        }
        break;
    }
    default: {   // брусок: плоским считается поперёк своего самого тонкого размера
        if (!(shape.size_x_mm > 0.0) || !(shape.size_y_mm > 0.0) ||
            !(shape.size_z_mm > 0.0)) {
            return false;
        }
        const double sizes[3] = {shape.size_x_mm, shape.size_y_mm, shape.size_z_mm};
        axis = 2;
        for (int index = 0; index < 3; ++index) {
            if (sizes[index] < sizes[axis]) {
                axis = index;
            }
        }
        thickness_mm = sizes[axis];
        const double half_u = 0.5 * (axis == 0 ? shape.size_y_mm : shape.size_x_mm);
        const double half_v = 0.5 * (axis == 2 ? shape.size_y_mm : shape.size_z_mm);
        // Для оси Y плоскость — (z, x): по u идёт z, по v идёт x.
        const double half_u_final = axis == 1 ? 0.5 * shape.size_z_mm : half_u;
        const double half_v_final = axis == 1 ? 0.5 * shape.size_x_mm : half_v;
        outline << QPointF(-half_u_final, -half_v_final) << QPointF(half_u_final, -half_v_final)
                << QPointF(half_u_final, half_v_final) << QPointF(-half_u_final, half_v_final);
        break;
    }
    }

    const double rotations_deg[3] = {shape.rotation_x_deg, shape.rotation_y_deg,
                                     shape.rotation_z_deg};
    for (int index = 0; index < 3; ++index) {
        if (index != axis && rotations_deg[index] != 0.0) {
            return false;
        }
    }
    // Поворот вокруг собственной оси — обычный поворот профиля вокруг центра
    // тела. Формула одна для всех трёх осей: пары (u, v) подобраны так, что
    // тройка (u, v, ось) остаётся правой.
    const double angle_rad = rotations_deg[axis] * pi / 180.0;
    if (angle_rad != 0.0) {
        const double cosine = std::cos(angle_rad);
        const double sine = std::sin(angle_rad);
        for (QPointF &point : outline) {
            point = QPointF(point.x() * cosine - point.y() * sine,
                            point.x() * sine + point.y() * cosine);
        }
    }

    const QPointF center = planarCenter(axis, shape);
    for (QPointF &point : outline) {
        point += center;
    }
    const double axial_center = planarAxialCenter(axis, shape);
    solid->axis = axis;
    solid->axial_min_mm = axial_center - 0.5 * thickness_mm;
    solid->axial_max_mm = axial_center + 0.5 * thickness_mm;
    solid->outline = outline;
    return true;
}

// Контуры из QPainterPath приходят замкнутыми — с повтором первой точки в
// конце, — а совпадающие подряд вершины дают треугольники нулевой площади, на
// которых отсечение ушей останавливается. Здесь они убираются.
QPolygonF cleanedContour(const QPolygonF &contour)
{
    constexpr double tolerance_mm = 1.0e-7;
    QPolygonF cleaned;
    for (const QPointF &point : contour) {
        if (!cleaned.isEmpty()) {
            const QPointF delta = point - cleaned.last();
            if (std::abs(delta.x()) < tolerance_mm && std::abs(delta.y()) < tolerance_mm) {
                continue;
            }
        }
        cleaned << point;
    }
    while (cleaned.size() > 1) {
        const QPointF delta = cleaned.last() - cleaned.first();
        if (std::abs(delta.x()) < tolerance_mm && std::abs(delta.y()) < tolerance_mm) {
            cleaned.removeLast();
            continue;
        }
        break;
    }
    return cleaned;
}

// Врезает дырки во внешний контур мостиками: контур с дыркой разрезается до
// односвязного, и его уже можно разбить на треугольники отсечением ушей.
// Мостик проводится от самой правой вершины дырки к ближайшей вершине внешнего
// контура — для очертаний, которые встречаются в диафрагмах, этого довольно.
QVector<QPointF> bridgeHoles(const QPolygonF &outer, const QVector<QPolygonF> &holes)
{
    const auto signed_area = [](const QVector<QPointF> &polygon) {
        double area = 0.0;
        for (int index = 0; index < polygon.size(); ++index) {
            const QPointF &from = polygon[index];
            const QPointF &to = polygon[(index + 1) % polygon.size()];
            area += from.x() * to.y() - to.x() * from.y();
        }
        return area;
    };

    QVector<QPointF> contour;
    for (const QPointF &point : outer) {
        contour.push_back(point);
    }
    for (const QPolygonF &hole : holes) {
        if (hole.size() < 3 || contour.size() < 3) {
            continue;
        }
        int hole_start = 0;
        for (int index = 1; index < hole.size(); ++index) {
            if (hole[index].x() > hole[hole_start].x()) {
                hole_start = index;
            }
        }
        int outer_index = 0;
        double best_distance = std::numeric_limits<double>::max();
        for (int index = 0; index < contour.size(); ++index) {
            const QPointF delta = contour[index] - hole[hole_start];
            const double distance = delta.x() * delta.x() + delta.y() * delta.y();
            if (distance < best_distance) {
                best_distance = distance;
                outer_index = index;
            }
        }
        // Дырка должна обходиться против внешнего контура — тогда после врезки
        // она остаётся отверстием, а не вторым куском металла. Направление
        // обхода у QPainterPath не гарантировано, поэтому оно определяется по
        // знаку площади, а не предполагается.
        QVector<QPointF> hole_points;
        hole_points.reserve(hole.size());
        for (const QPointF &point : hole) {
            hole_points.push_back(point);
        }
        const bool same_orientation =
            (signed_area(contour) >= 0.0) == (signed_area(hole_points) >= 0.0);
        QVector<QPointF> merged;
        merged.reserve(contour.size() + hole.size() + 2);
        for (int index = 0; index <= outer_index; ++index) {
            merged.push_back(contour[index]);
        }
        for (int step = 0; step <= hole.size(); ++step) {
            const int offset = step % hole.size();
            const int hole_index =
                same_orientation
                    ? (hole_start - offset + hole.size()) % hole.size()
                    : (hole_start + offset) % hole.size();
            merged.push_back(hole[hole_index]);
        }
        merged.push_back(contour[outer_index]);
        for (int index = outer_index + 1; index < contour.size(); ++index) {
            merged.push_back(contour[index]);
        }
        contour = merged;
    }
    return contour;
}

double normalizedRotationDeg(double value)
{
    double normalized = std::fmod(value, 360.0);
    if (normalized > 180.0) {
        normalized -= 360.0;
    } else if (normalized < -180.0) {
        normalized += 360.0;
    }

    return normalized;
}
}

WaveguideOpenGLWidget::WaveguideOpenGLWidget(QWidget *parent)
    : QOpenGLWidget(parent)
{
    setMinimumSize(640, 420);
    setFocusPolicy(Qt::StrongFocus);

    animation_timer_.setInterval(33);
    connect(&animation_timer_, &QTimer::timeout, this, [this]() {
        animation_phase_ += 0.14;
        if (animation_phase_ > 2.0 * pi) {
            animation_phase_ -= 2.0 * pi;
        }
        update();
    });
}

void WaveguideOpenGLWidget::setCalculationResult(const WaveguideCalculationResult &result)
{
    result_ = result;
    // Стопка объёмной заливки построена по прежнему решению; до перестройки
    // показывать её поверх нового поля нельзя.
    volume_slices_.clear();
    update();
}

void WaveguideOpenGLWidget::setFieldGlyphs(const QVector<FieldGlyph> &glyphs)
{
    // Смена концентрации стрелок перестраивает только глифы: срезы, геометрия
    // и числа результата остаются от того же расчёта.
    result_.field_glyphs = glyphs;
    update();
}

void WaveguideOpenGLWidget::setSlice(FieldSlicePlane plane, const FieldSlice &slice)
{
    if (plane == FieldSlicePlane::HorizontalXZ) {
        result_.horizontal_slice = slice;
    } else {
        result_.vertical_slice = slice;
    }
    update();
}

void WaveguideOpenGLWidget::setVolumeSlices(const QVector<FieldSlice> &slices)
{
    volume_slices_ = slices;
    update();
}

void WaveguideOpenGLWidget::setModelPreview(const WaveguideParameters &parameters)
{
    result_.parameters = parameters;
    if (parameters.cross_section == 1) {
        // У круглого сечения внутренние «ширина» и «глубина» равны внутреннему
        // диаметру: это описанный квадрат, по которому строится и рамка вида.
        const double inner_diameter_mm =
            std::max(0.0, 2.0 * (parameters.radius_mm - parameters.wall_thickness_mm));
        result_.inner_width_mm = inner_diameter_mm;
        result_.inner_depth_mm = inner_diameter_mm;
        result_.valid = parameters.radius_mm > 0.0 && parameters.length_mm > 0.0;
    } else {
        result_.inner_width_mm =
            std::max(0.0, parameters.width_mm - 2.0 * parameters.wall_thickness_mm);
        result_.inner_depth_mm =
            std::max(0.0, parameters.depth_mm - 2.0 * parameters.wall_thickness_mm);
        result_.valid = parameters.width_mm > 0.0 &&
                        parameters.depth_mm > 0.0 &&
                        parameters.length_mm > 0.0;
    }
    update();
}

void WaveguideOpenGLWidget::setSelectedPlateIndex(int plate_index)
{
    selected_plate_index_ = plate_index;
    update();
}

void WaveguideOpenGLWidget::setSelectedShapeIndex(int shape_index)
{
    selected_shape_index_ = shape_index;
    update();
}

void WaveguideOpenGLWidget::setShellSelected(bool selected)
{
    shell_selected_ = selected;
    update();
}

void WaveguideOpenGLWidget::setFieldDisplayMode(FieldDisplayMode mode)
{
    field_display_mode_ = mode;
    update();
}

void WaveguideOpenGLWidget::setFieldFillMode(FieldFillMode mode)
{
    fill_mode_ = mode;
    update();
}

void WaveguideOpenGLWidget::setSlicePlane(FieldSlicePlane plane)
{
    slice_plane_ = plane;
    update();
}

void WaveguideOpenGLWidget::setAnimationEnabled(bool enabled)
{
    animation_enabled_ = enabled;
    if (enabled) {
        animation_timer_.start();
    } else {
        animation_timer_.stop();
        update();
    }
}

void WaveguideOpenGLWidget::setViewPreset(WaveguideViewPreset preset)
{
    view_preset_ = preset;

    if (view_preset_ == WaveguideViewPreset::Top) {
        rotation_x_ = 90.0;
        rotation_y_ = 0.0;
        camera_distance_factor_ = 2.15;
    } else if (view_preset_ == WaveguideViewPreset::Side) {
        rotation_x_ = 0.0;
        rotation_y_ = -90.0;
        camera_distance_factor_ = 2.35;
    } else {
        rotation_x_ = -24.0;
        rotation_y_ = 34.0;
        camera_distance_factor_ = 2.7;
    }

    update();
}

void WaveguideOpenGLWidget::setSlotEditedCallback(std::function<void(const WaveguideParameters &)> callback)
{
    slot_edited_callback_ = std::move(callback);
}

void WaveguideOpenGLWidget::resetView()
{
    setViewPreset(view_preset_);
}

void WaveguideOpenGLWidget::initializeGL()
{
    initializeOpenGLFunctions();
    glClearColor(0.055f, 0.065f, 0.075f, 1.0f);
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_MULTISAMPLE);
    glShadeModel(GL_SMOOTH);
}

void WaveguideOpenGLWidget::resizeGL(int width, int height)
{
    glViewport(0, 0, width, std::max(1, height));
}

void WaveguideOpenGLWidget::paintGL()
{
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    setupProjection();
    setupModelView();

    drawAxes();
    if (result_.valid) {
        drawWaveguide();
        if (fill_mode_ == FieldFillMode::Slice) {
            drawFieldSlice();
        } else if (fill_mode_ == FieldFillMode::Volume) {
            drawVolumeSlices();
        }
        drawPecPlates();
        drawUserShapes();
        drawFields();
        drawPropagationArrow();
    }
}

void WaveguideOpenGLWidget::mousePressEvent(QMouseEvent *event)
{
    setFocus(Qt::MouseFocusReason);
    last_mouse_position_ = event->pos();

    if (view_preset_ != WaveguideViewPreset::Free3D) {
        event->accept();
        return;
    }

    if (event->button() == Qt::LeftButton &&
        result_.valid &&
        result_.parameters.slot_enabled) {
        if ((event->modifiers() & Qt::ControlModifier) &&
            (slot_selected_ || slot_editing_) &&
            pickSlot(event->pos())) {
            slot_selected_ = true;
            slot_rotating_ = true;
            event->accept();
            return;
        }

        if (pickSlot(event->pos())) {
            slot_selected_ = true;
            update();
            event->accept();
            return;
        }

        if (!slot_editing_) {
            slot_selected_ = false;
            update();
        }
    }
}

void WaveguideOpenGLWidget::mouseMoveEvent(QMouseEvent *event)
{
    const QPoint delta = event->pos() - last_mouse_position_;
    last_mouse_position_ = event->pos();

    if (view_preset_ != WaveguideViewPreset::Free3D) {
        event->accept();
        return;
    }

    if (result_.valid && result_.parameters.slot_enabled && slot_editing_) {
        if ((event->buttons() & Qt::LeftButton) &&
            (slot_rotating_ || (event->modifiers() & Qt::ControlModifier))) {
            rotateInteractiveSlot(event->pos(), delta);
        } else {
            updateInteractiveSlotPosition(event->pos());
        }
        event->accept();
        return;
    }

    if ((event->buttons() & Qt::LeftButton) &&
        result_.valid &&
        result_.parameters.slot_enabled &&
        slot_rotating_) {
        rotateInteractiveSlot(event->pos(), delta);
        event->accept();
        return;
    }

    if (event->buttons() & Qt::LeftButton) {
        rotation_x_ += delta.y() * 0.45;
        rotation_y_ += delta.x() * 0.45;
        update();
    }
}

void WaveguideOpenGLWidget::mouseDoubleClickEvent(QMouseEvent *event)
{
    if (view_preset_ != WaveguideViewPreset::Free3D) {
        event->accept();
        return;
    }

    if (event->button() == Qt::LeftButton &&
        result_.valid &&
        result_.parameters.slot_enabled &&
        pickSlot(event->pos())) {
        slot_selected_ = true;
        slot_editing_ = true;
        setFocus(Qt::MouseFocusReason);
        update();
        event->accept();
        return;
    }

    QOpenGLWidget::mouseDoubleClickEvent(event);
}

void WaveguideOpenGLWidget::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        slot_rotating_ = false;
    }

    QOpenGLWidget::mouseReleaseEvent(event);
}

void WaveguideOpenGLWidget::keyPressEvent(QKeyEvent *event)
{
    if (view_preset_ != WaveguideViewPreset::Free3D) {
        QOpenGLWidget::keyPressEvent(event);
        return;
    }

    if ((event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) && slot_editing_) {
        slot_editing_ = false;
        slot_rotating_ = false;
        slot_selected_ = true;
        notifySlotEdited();
        update();
        event->accept();
        return;
    }

    QOpenGLWidget::keyPressEvent(event);
}

void WaveguideOpenGLWidget::wheelEvent(QWheelEvent *event)
{
    const double step = event->angleDelta().y() > 0 ? 0.88 : 1.13;
    camera_distance_factor_ = std::clamp(camera_distance_factor_ * step,
                                         min_camera_factor,
                                         max_camera_factor);
    update();
}

void WaveguideOpenGLWidget::setupProjection()
{
    const double aspect = static_cast<double>(std::max(1, width())) /
                          static_cast<double>(std::max(1, height()));
    const double near_plane = 0.5;
    const double far_plane = 10000.0;
    const double fov_y_rad = 42.0 * pi / 180.0;
    const double top = near_plane * std::tan(fov_y_rad * 0.5);
    const double right = top * aspect;

    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glFrustum(-right, right, -top, top, near_plane, far_plane);
}

void WaveguideOpenGLWidget::setupModelView()
{
    const double radius = modelRadiusMm();

    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glTranslated(0.0, 0.0, -std::max(5.0, radius * camera_distance_factor_));
    glRotated(rotation_x_, 1.0, 0.0, 0.0);
    glRotated(rotation_y_, 0.0, 1.0, 0.0);
}

QMatrix4x4 WaveguideOpenGLWidget::projectionMatrix() const
{
    const double aspect = static_cast<double>(std::max(1, width())) /
                          static_cast<double>(std::max(1, height()));
    const double near_plane = 0.5;
    const double far_plane = 10000.0;
    const double fov_y_rad = 42.0 * pi / 180.0;
    const double top = near_plane * std::tan(fov_y_rad * 0.5);
    const double right = top * aspect;

    QMatrix4x4 projection;
    projection.frustum(static_cast<float>(-right),
                       static_cast<float>(right),
                       static_cast<float>(-top),
                       static_cast<float>(top),
                       static_cast<float>(near_plane),
                       static_cast<float>(far_plane));
    return projection;
}

QMatrix4x4 WaveguideOpenGLWidget::modelViewMatrix() const
{
    const double radius = modelRadiusMm();
    QMatrix4x4 model_view;
    model_view.translate(0.0f,
                         0.0f,
                         static_cast<float>(-std::max(5.0, radius * camera_distance_factor_)));
    model_view.rotate(static_cast<float>(rotation_x_), 1.0f, 0.0f, 0.0f);
    model_view.rotate(static_cast<float>(rotation_y_), 0.0f, 1.0f, 0.0f);
    return model_view;
}

WaveguideOpenGLWidget::SlotSurfaceInfo WaveguideOpenGLWidget::slotSurfaceInfo(int surface_id) const
{
    SlotSurfaceInfo surface;
    surface.id = std::clamp(surface_id, 0, 3);

    const double outer_width_mm = result_.parameters.width_mm;
    const double outer_depth_mm = result_.parameters.depth_mm;
    const double eps_mm = std::max(0.02, result_.parameters.wall_thickness_mm * 0.12);

    surface.v_axis = QVector3D(0.0f, 0.0f, 1.0f);
    surface.half_z_mm = 0.5 * result_.parameters.length_mm;

    if (surface.id == 1) {
        surface.normal = QVector3D(1.0f, 0.0f, 0.0f);
        surface.u_axis = QVector3D(0.0f, 1.0f, 0.0f);
        surface.origin = QVector3D(static_cast<float>(0.5 * outer_width_mm + eps_mm), 0.0f, 0.0f);
        surface.half_u_mm = 0.5 * result_.inner_depth_mm;
    } else if (surface.id == 2) {
        surface.normal = QVector3D(0.0f, -1.0f, 0.0f);
        surface.u_axis = QVector3D(1.0f, 0.0f, 0.0f);
        surface.origin = QVector3D(0.0f, static_cast<float>(-0.5 * outer_depth_mm - eps_mm), 0.0f);
        surface.half_u_mm = 0.5 * result_.inner_width_mm;
    } else if (surface.id == 3) {
        surface.normal = QVector3D(-1.0f, 0.0f, 0.0f);
        surface.u_axis = QVector3D(0.0f, 1.0f, 0.0f);
        surface.origin = QVector3D(static_cast<float>(-0.5 * outer_width_mm - eps_mm), 0.0f, 0.0f);
        surface.half_u_mm = 0.5 * result_.inner_depth_mm;
    } else {
        surface.normal = QVector3D(0.0f, 1.0f, 0.0f);
        surface.u_axis = QVector3D(1.0f, 0.0f, 0.0f);
        surface.origin = QVector3D(0.0f, static_cast<float>(0.5 * outer_depth_mm + eps_mm), 0.0f);
        surface.half_u_mm = 0.5 * result_.inner_width_mm;
    }

    return surface;
}

QVector3D WaveguideOpenGLWidget::slotCenterModel(const WaveguideParameters &parameters) const
{
    const SlotSurfaceInfo surface = slotSurfaceInfo(parameters.slot_surface);
    return surface.origin +
           surface.u_axis * static_cast<float>(parameters.slot_offset_x_mm) +
           surface.v_axis * static_cast<float>(parameters.slot_offset_z_mm);
}

bool WaveguideOpenGLWidget::buildPickRay(const QPoint &screen_position, PickRay *ray) const
{
    if (!ray || width() <= 0 || height() <= 0) {
        return false;
    }

    const QRect viewport(0, 0, width(), height());
    const float win_x = static_cast<float>(screen_position.x());
    const float win_y = static_cast<float>(height() - screen_position.y());
    const QMatrix4x4 projection = projectionMatrix();
    const QMatrix4x4 model_view = modelViewMatrix();

    const QVector3D near_point = QVector3D(win_x, win_y, 0.0f).unproject(model_view,
                                                                          projection,
                                                                          viewport);
    const QVector3D far_point = QVector3D(win_x, win_y, 1.0f).unproject(model_view,
                                                                         projection,
                                                                         viewport);
    const QVector3D direction = far_point - near_point;
    if (direction.lengthSquared() < 1.0e-8f) {
        return false;
    }

    ray->origin = near_point;
    ray->direction = direction.normalized();
    return true;
}

bool WaveguideOpenGLWidget::intersectSurface(const PickRay &ray,
                                             const SlotSurfaceInfo &surface,
                                             QVector3D *hit,
                                             double *distance) const
{
    const double denominator = static_cast<double>(QVector3D::dotProduct(ray.direction, surface.normal));
    if (std::abs(denominator) < 1.0e-8) {
        return false;
    }

    const double t = static_cast<double>(QVector3D::dotProduct(surface.origin - ray.origin,
                                                               surface.normal)) /
                     denominator;
    if (t < 0.0) {
        return false;
    }

    if (hit) {
        *hit = ray.origin + ray.direction * static_cast<float>(t);
    }
    if (distance) {
        *distance = t;
    }
    return true;
}

bool WaveguideOpenGLWidget::pickWall(const QPoint &screen_position,
                                     int *surface_id,
                                     double *u_mm,
                                     double *z_mm) const
{
    PickRay ray;
    if (!buildPickRay(screen_position, &ray)) {
        return false;
    }

    bool found = false;
    double nearest_distance = 0.0;
    int picked_surface_id = 0;
    double picked_u_mm = 0.0;
    double picked_z_mm = 0.0;
    const double tolerance_mm = 0.8;

    for (int current_surface_id = 0; current_surface_id < 4; ++current_surface_id) {
        const SlotSurfaceInfo surface = slotSurfaceInfo(current_surface_id);
        QVector3D hit;
        double distance = 0.0;
        if (!intersectSurface(ray, surface, &hit, &distance)) {
            continue;
        }

        const QVector3D delta = hit - surface.origin;
        const double local_u_mm = static_cast<double>(QVector3D::dotProduct(delta, surface.u_axis));
        const double local_z_mm = static_cast<double>(QVector3D::dotProduct(delta, surface.v_axis));
        if (std::abs(local_u_mm) > surface.half_u_mm + tolerance_mm ||
            std::abs(local_z_mm) > surface.half_z_mm + tolerance_mm) {
            continue;
        }

        if (!found || distance < nearest_distance) {
            found = true;
            nearest_distance = distance;
            picked_surface_id = current_surface_id;
            picked_u_mm = std::clamp(local_u_mm, -surface.half_u_mm, surface.half_u_mm);
            picked_z_mm = std::clamp(local_z_mm, -surface.half_z_mm, surface.half_z_mm);
        }
    }

    if (!found) {
        return false;
    }

    if (surface_id) {
        *surface_id = picked_surface_id;
    }
    if (u_mm) {
        *u_mm = picked_u_mm;
    }
    if (z_mm) {
        *z_mm = picked_z_mm;
    }
    return true;
}

bool WaveguideOpenGLWidget::pickSlot(const QPoint &screen_position) const
{
    PickRay ray;
    if (!buildPickRay(screen_position, &ray)) {
        return false;
    }

    const WaveguideParameters &parameters = result_.parameters;
    const SlotSurfaceInfo surface = slotSurfaceInfo(parameters.slot_surface);
    QVector3D hit;
    if (!intersectSurface(ray, surface, &hit, nullptr)) {
        return false;
    }

    const double angle_rad = parameters.slot_rotation_deg * pi / 180.0;
    const QVector3D length_axis = safeNormal(surface.v_axis * static_cast<float>(std::cos(angle_rad)) +
                                                surface.u_axis * static_cast<float>(std::sin(angle_rad)),
                                            surface.v_axis);
    const QVector3D width_axis = safeNormal(surface.u_axis * static_cast<float>(std::cos(angle_rad)) -
                                               surface.v_axis * static_cast<float>(std::sin(angle_rad)),
                                           surface.u_axis);
    const QVector3D delta = hit - slotCenterModel(parameters);
    const double length_coord_mm = static_cast<double>(QVector3D::dotProduct(delta, length_axis));
    const double width_coord_mm = static_cast<double>(QVector3D::dotProduct(delta, width_axis));
    const double tolerance_mm = std::max(1.2, parameters.slot_width_mm * 1.7);

    return std::abs(length_coord_mm) <= 0.5 * parameters.slot_length_mm + tolerance_mm &&
           std::abs(width_coord_mm) <= 0.5 * parameters.slot_width_mm + tolerance_mm;
}

void WaveguideOpenGLWidget::updateInteractiveSlotPosition(const QPoint &screen_position)
{
    int surface_id = 0;
    double u_mm = 0.0;
    double z_mm = 0.0;
    if (!pickWall(screen_position, &surface_id, &u_mm, &z_mm)) {
        return;
    }

    result_.parameters.slot_enabled = true;
    result_.parameters.slot_surface = surface_id;
    result_.parameters.slot_offset_x_mm = u_mm;
    result_.parameters.slot_offset_z_mm = z_mm;
    clampInteractiveSlotParameters();
    notifySlotEdited();
    update();
}

void WaveguideOpenGLWidget::rotateInteractiveSlot(const QPoint &, const QPoint &delta)
{
    result_.parameters.slot_rotation_deg = normalizedRotationDeg(result_.parameters.slot_rotation_deg +
                                                                 delta.x() * 0.72);
    clampInteractiveSlotParameters();
    notifySlotEdited();
    update();
}

void WaveguideOpenGLWidget::clampInteractiveSlotParameters()
{
    WaveguideParameters &parameters = result_.parameters;
    parameters.slot_surface = std::clamp(parameters.slot_surface, 0, 3);
    parameters.slot_rotation_deg = normalizedRotationDeg(parameters.slot_rotation_deg);

    const SlotSurfaceInfo surface = slotSurfaceInfo(parameters.slot_surface);
    const double angle_rad = parameters.slot_rotation_deg * pi / 180.0;
    const double half_width_mm = 0.5 * parameters.slot_width_mm;
    const double half_length_mm = 0.5 * parameters.slot_length_mm;
    const double u_extent_mm = std::abs(std::cos(angle_rad)) * half_width_mm +
                               std::abs(std::sin(angle_rad)) * half_length_mm;
    const double z_extent_mm = std::abs(std::sin(angle_rad)) * half_width_mm +
                               std::abs(std::cos(angle_rad)) * half_length_mm;
    const double max_u_mm = std::max(0.0, surface.half_u_mm - u_extent_mm);
    const double max_z_mm = std::max(0.0, surface.half_z_mm - z_extent_mm);

    parameters.slot_offset_x_mm = std::clamp(parameters.slot_offset_x_mm, -max_u_mm, max_u_mm);
    parameters.slot_offset_z_mm = std::clamp(parameters.slot_offset_z_mm, -max_z_mm, max_z_mm);
}

void WaveguideOpenGLWidget::notifySlotEdited()
{
    if (slot_edited_callback_) {
        slot_edited_callback_(result_.parameters);
    }
}

// Кольцевая стенка круглого волновода: боковые поверхности двух соосных
// цилиндров и торцевые кольца между ними.
void WaveguideOpenGLWidget::drawCircularShell(double inner_radius,
                                              double outer_radius,
                                              double z0,
                                              double z1,
                                              const QColor &metal_color,
                                              const QColor &edge_color,
                                              double body_alpha) const
{
    constexpr int segment_count = 64;
    const auto angle_at = [](int index) {
        return 2.0 * pi * index / segment_count;
    };

    // В режиме каркаса поверхности не рисуются вовсе: полностью прозрачная
    // грань всё равно пишет глубину и заслоняет то, что стоит за ней.
    setColor(metal_color, body_alpha);
    glBegin(GL_QUADS);
    for (int index = 0; body_alpha > 0.0 && index < segment_count; ++index) {
        const double a0 = angle_at(index);
        const double a1 = angle_at(index + 1);
        const double c0 = std::cos(a0);
        const double s0 = std::sin(a0);
        const double c1 = std::cos(a1);
        const double s1 = std::sin(a1);

        // Внешняя боковая поверхность.
        glVertex3d(outer_radius * c0, outer_radius * s0, z0);
        glVertex3d(outer_radius * c1, outer_radius * s1, z0);
        glVertex3d(outer_radius * c1, outer_radius * s1, z1);
        glVertex3d(outer_radius * c0, outer_radius * s0, z1);

        // Внутренняя боковая поверхность (стенка канала).
        glVertex3d(inner_radius * c0, inner_radius * s0, z0);
        glVertex3d(inner_radius * c1, inner_radius * s1, z0);
        glVertex3d(inner_radius * c1, inner_radius * s1, z1);
        glVertex3d(inner_radius * c0, inner_radius * s0, z1);

        // Торцевые кольца.
        for (const double z : {z0, z1}) {
            glVertex3d(inner_radius * c0, inner_radius * s0, z);
            glVertex3d(outer_radius * c0, outer_radius * s0, z);
            glVertex3d(outer_radius * c1, outer_radius * s1, z);
            glVertex3d(inner_radius * c1, inner_radius * s1, z);
        }
    }
    glEnd();

    ::glLineWidth(1.4f);
    setColor(edge_color, 0.5);
    for (const double radius : {inner_radius, outer_radius}) {
        for (const double z : {z0, z1}) {
            glBegin(GL_LINE_LOOP);
            for (int index = 0; index < segment_count; ++index) {
                const double angle = angle_at(index);
                glVertex3d(radius * std::cos(angle), radius * std::sin(angle), z);
            }
            glEnd();
        }
    }
}

double WaveguideOpenGLWidget::shellAlpha() const
{
    switch (result_.parameters.shell_display) {
    case 1:
        return 0.82;   // сплошной: внутренности не видно
    case 2:
        return 0.22;   // полупрозрачный
    case 3:
        return 0.0;    // каркас: только рёбра
    default:
        break;
    }
    // Автоматически: выбранное в дереве тело или пластина должны быть видны
    // сквозь стенку, иначе вставку внутри тракта не рассмотреть сбоку. Выбран
    // сам волновод — наоборот, корпус закрывает внутренности, как в CST.
    if (selected_plate_index_ >= 0 || selected_shape_index_ >= 0) {
        return 0.07;
    }
    return shell_selected_ ? 0.82 : 0.22;
}

void WaveguideOpenGLWidget::drawWaveguide() const
{
    const QColor shell_metal_color(116, 132, 148);
    const QColor shell_edge_color(212, 226, 240);
    const double body_alpha = shellAlpha();
    if (result_.parameters.cross_section == 1) {
        const double outer_radius = result_.parameters.radius_mm;
        const double inner_radius = std::max(0.0, 0.5 * result_.inner_width_mm);
        const double half_length = 0.5 * result_.parameters.length_mm;
        drawCircularShell(inner_radius,
                          outer_radius,
                          -half_length,
                          half_length,
                          shell_metal_color,
                          shell_edge_color,
                          body_alpha);

        // Подсветка входного и выходного отверстий, как у прямоугольного тракта.
        ::glLineWidth(2.0f);
        setColor(QColor(128, 210, 255), 0.72);
        constexpr int segment_count = 64;
        for (const double z : {-half_length, half_length}) {
            glBegin(GL_LINE_LOOP);
            for (int index = 0; index < segment_count; ++index) {
                const double angle = 2.0 * pi * index / segment_count;
                glVertex3d(inner_radius * std::cos(angle), inner_radius * std::sin(angle), z);
            }
            glEnd();
        }
        return;
    }

    const double outer_width = result_.parameters.width_mm;
    const double outer_depth = result_.parameters.depth_mm;
    const double length = result_.parameters.length_mm;
    const double inner_width = result_.inner_width_mm;
    const double inner_depth = result_.inner_depth_mm;

    const double outer_x0 = -0.5 * outer_width;
    const double outer_x1 = 0.5 * outer_width;
    const double outer_y0 = -0.5 * outer_depth;
    const double outer_y1 = 0.5 * outer_depth;
    const double inner_x0 = -0.5 * inner_width;
    const double inner_x1 = 0.5 * inner_width;
    const double inner_y0 = -0.5 * inner_depth;
    const double inner_y1 = 0.5 * inner_depth;
    const double z0 = -0.5 * length;
    const double z1 = 0.5 * length;
    const QColor metal_color(116, 132, 148);
    const QColor edge_color(212, 226, 240);

    if (body_alpha > 0.0) {
        drawBox(outer_x0, outer_x1, inner_y1, outer_y1, z0, z1, metal_color, body_alpha);
        drawBox(outer_x0, outer_x1, outer_y0, inner_y0, z0, z1, metal_color, body_alpha);
        drawBox(outer_x0, inner_x0, inner_y0, inner_y1, z0, z1, metal_color, body_alpha);
        drawBox(inner_x1, outer_x1, inner_y0, inner_y1, z0, z1, metal_color, body_alpha);
    }

    ::glLineWidth(1.4f);
    drawBoxEdges(outer_x0, outer_x1, inner_y1, outer_y1, z0, z1, edge_color);
    drawBoxEdges(outer_x0, outer_x1, outer_y0, inner_y0, z0, z1, edge_color);
    drawBoxEdges(outer_x0, inner_x0, inner_y0, inner_y1, z0, z1, edge_color);
    drawBoxEdges(inner_x1, outer_x1, inner_y0, inner_y1, z0, z1, edge_color);

    ::glLineWidth(2.0f);
    setColor(QColor(128, 210, 255), 0.72);
    glBegin(GL_LINE_LOOP);
    glVertex3d(inner_x0, inner_y0, z0);
    glVertex3d(inner_x1, inner_y0, z0);
    glVertex3d(inner_x1, inner_y1, z0);
    glVertex3d(inner_x0, inner_y1, z0);
    glEnd();
    glBegin(GL_LINE_LOOP);
    glVertex3d(inner_x0, inner_y0, z1);
    glVertex3d(inner_x1, inner_y0, z1);
    glVertex3d(inner_x1, inner_y1, z1);
    glVertex3d(inner_x0, inner_y1, z1);
    glEnd();

    drawSlot();
}

void WaveguideOpenGLWidget::drawUserShapes() const
{
    const QVector<ShapeParameters> &shapes = result_.parameters.shapes;
    if (shapes.isEmpty()) {
        return;
    }

    // Тела, лежащие в одной плоскости и одинаково толстые, складываются в один
    // контур обычными плоскими булевыми операциями. Так вычитание видно в кадре
    // дыркой в металле, а не рамкой поверх целой пластины: настоящий объёмный
    // CSG считает сеточный генератор, но до отрисовки его результат не доходит.
    struct PlanarGroup
    {
        int axis = 2;
        double axial_min_mm = 0.0;
        double axial_max_mm = 0.0;
        QPainterPath path;
        bool holds_selection = false;
    };

    constexpr double axial_tolerance_mm = 1.0e-6;
    QVector<PlanarGroup> groups;
    QVector<int> standalone;
    PlanarGroup current;
    bool current_started = false;
    // Металл за стенкой в расчёте не существует: сеточный генератор вычитает
    // тела из объёма полости, и всё, что выходит за стенку, отрезается ею.
    // Отрисовка обязана показывать то же самое — иначе основание, нарочно
    // заведённое в стенку для надёжной булевой сварки, торчит сквозь корпус,
    // как у T-вставок круглого тракта. Точно обрезается группа вдоль оси Z:
    // только у неё сечение полости постоянно вдоль оси тела.
    // Полость берётся с волосяным запасом: пластина во всё сечение совпадает с
    // ней ровно, а пересечение двух совпадающих границ рождает у Qt мусорные
    // осколочные контуры, на которых ломается заливка.
    constexpr double cavity_margin_mm = 1.0e-3;
    QPainterPath cavity_cross_section;
    if (result_.inner_width_mm > 0.0 && result_.inner_depth_mm > 0.0) {
        if (result_.parameters.cross_section == 1) {
            const double inner_radius_mm = 0.5 * result_.inner_width_mm + cavity_margin_mm;
            cavity_cross_section.addEllipse(QPointF(0.0, 0.0), inner_radius_mm,
                                            inner_radius_mm);
        } else {
            const double half_width_mm = 0.5 * result_.inner_width_mm + cavity_margin_mm;
            const double half_depth_mm = 0.5 * result_.inner_depth_mm + cavity_margin_mm;
            cavity_cross_section.addRect(QRectF(-half_width_mm,
                                                -half_depth_mm,
                                                2.0 * half_width_mm,
                                                2.0 * half_depth_mm));
        }
    }
    const auto flush_current = [&groups, &current, &current_started, &cavity_cross_section]() {
        if (!current_started) {
            return;
        }
        if (current.axis == 2 && !cavity_cross_section.isEmpty()) {
            current.path = current.path.intersected(cavity_cross_section);
        }
        groups.push_back(current);
        current_started = false;
    };
    const auto outline_path = [](const QPolygonF &outline) {
        QPainterPath path;
        path.addPolygon(outline);
        path.closeSubpath();
        return path;
    };

    for (int shape_index = 0; shape_index < shapes.size(); ++shape_index) {
        const ShapeParameters &shape = shapes[shape_index];
        if (!shape.enabled) {
            continue;
        }
        PlanarSolid solid;
        const bool planar = toPlanarSolid(shape, &solid);
        if (shape.operation == 0) {
            if (!planar) {
                flush_current();
                standalone.push_back(shape_index);
                continue;
            }
            const bool joins_current =
                current_started && current.axis == solid.axis &&
                std::abs(current.axial_min_mm - solid.axial_min_mm) < axial_tolerance_mm &&
                std::abs(current.axial_max_mm - solid.axial_max_mm) < axial_tolerance_mm;
            if (joins_current) {
                current.path = current.path.united(outline_path(solid.outline));
            } else {
                flush_current();
                current.axis = solid.axis;
                current.axial_min_mm = solid.axial_min_mm;
                current.axial_max_mm = solid.axial_max_mm;
                current.path = outline_path(solid.outline);
                current.holds_selection = false;
                current_started = true;
            }
            current.holds_selection =
                current.holds_selection || shape_index == selected_shape_index_;
            continue;
        }
        // Вычитание и пересечение применимы, только если резак проходит толщину
        // группы насквозь: иначе он срезал бы её лишь частично, а плоский контур
        // этого не выразит.
        const bool cuts_through =
            planar && current_started && current.axis == solid.axis &&
            solid.axial_min_mm <= current.axial_min_mm + axial_tolerance_mm &&
            solid.axial_max_mm >= current.axial_max_mm - axial_tolerance_mm;
        if (!cuts_through) {
            standalone.push_back(shape_index);
            continue;
        }
        current.path = shape.operation == 1
                           ? current.path.subtracted(outline_path(solid.outline))
                           : current.path.intersected(outline_path(solid.outline));
        current.holds_selection = current.holds_selection || shape_index == selected_shape_index_;
    }
    flush_current();

    for (const PlanarGroup &group : groups) {
        QVector<QPolygonF> contours;
        for (const QPolygonF &raw_contour : group.path.simplified().toSubpathPolygons()) {
            const QPolygonF contour = cleanedContour(raw_contour);
            if (contour.size() >= 3) {
                contours.push_back(contour);
            }
        }
        if (contours.isEmpty()) {
            continue;
        }
        const int axis = group.axis;
        const auto vertex = [axis](const QPointF &plane_point, double axial) {
            switch (axis) {
            case 0:
                return QVector3D(static_cast<float>(axial),
                                 static_cast<float>(plane_point.x()),
                                 static_cast<float>(plane_point.y()));
            case 1:
                return QVector3D(static_cast<float>(plane_point.y()),
                                 static_cast<float>(axial),
                                 static_cast<float>(plane_point.x()));
            default:
                return QVector3D(static_cast<float>(plane_point.x()),
                                 static_cast<float>(plane_point.y()),
                                 static_cast<float>(axial));
            }
        };
        // Вложенность контуров: контур внутри нечётного числа других — дырка,
        // внутри чётного — самостоятельный кусок металла. Проверяется точка
        // строго внутри контура: первая вершина после булевых операций часто
        // лежит ровно на границе соседнего контура, и там чёт-нечет отвечает
        // случайным образом.
        QVector<QPointF> interior_points(contours.size());
        for (int index = 0; index < contours.size(); ++index) {
            interior_points[index] = polygonInteriorPoint(contours[index]);
        }
        QVector<int> depth(contours.size(), 0);
        for (int index = 0; index < contours.size(); ++index) {
            for (int other = 0; other < contours.size(); ++other) {
                if (other != index &&
                    contours[other].containsPoint(interior_points[index], Qt::OddEvenFill)) {
                    ++depth[index];
                }
            }
        }

        const QColor body_color = group.holds_selection ? QColor(62, 151, 210)
                                                        : QColor(145, 157, 168);
        const QColor edge_color = group.holds_selection ? QColor(255, 210, 68)
                                                        : QColor(225, 235, 242);
        setColor(body_color, group.holds_selection ? 0.98 : 0.9);
        for (int index = 0; index < contours.size(); ++index) {
            if (depth[index] % 2 != 0 || contours[index].size() < 3) {
                continue;
            }
            // Дырки этого куска — контуры на единицу глубже, лежащие внутри него.
            QVector<QPolygonF> holes;
            for (int other = 0; other < contours.size(); ++other) {
                if (other != index && depth[other] == depth[index] + 1 &&
                    contours[index].containsPoint(interior_points[other], Qt::OddEvenFill)) {
                    holes.push_back(contours[other]);
                }
            }
            const QVector<QPointF> contour = bridgeHoles(contours[index], holes);
            const QVector<int> triangles = triangulateProfile(contour);
            glBegin(GL_TRIANGLES);
            for (const double axial : {group.axial_min_mm, group.axial_max_mm}) {
                for (int corner = 0; corner < triangles.size(); ++corner) {
                    const QVector3D position = vertex(contour[triangles[corner]], axial);
                    glVertex3f(position.x(), position.y(), position.z());
                }
            }
            glEnd();
        }

        // Боковые стенки строятся по всем контурам сразу: у дырки они и есть
        // стенки выреза.
        glBegin(GL_QUADS);
        for (const QPolygonF &contour : contours) {
            for (int index = 0; index < contour.size(); ++index) {
                const QPointF &from = contour[index];
                const QPointF &to = contour[(index + 1) % contour.size()];
                const QVector3D a = vertex(from, group.axial_min_mm);
                const QVector3D b = vertex(to, group.axial_min_mm);
                const QVector3D c = vertex(to, group.axial_max_mm);
                const QVector3D d = vertex(from, group.axial_max_mm);
                glVertex3f(a.x(), a.y(), a.z());
                glVertex3f(b.x(), b.y(), b.z());
                glVertex3f(c.x(), c.y(), c.z());
                glVertex3f(d.x(), d.y(), d.z());
            }
        }
        glEnd();

        ::glLineWidth(group.holds_selection ? 2.2f : 1.5f);
        setColor(edge_color, 0.95);
        for (const QPolygonF &contour : contours) {
            for (const double axial : {group.axial_min_mm, group.axial_max_mm}) {
                glBegin(GL_LINE_LOOP);
                for (const QPointF &point : contour) {
                    const QVector3D position = vertex(point, axial);
                    glVertex3f(position.x(), position.y(), position.z());
                }
                glEnd();
            }
            glBegin(GL_LINES);
            for (const QPointF &point : contour) {
                const QVector3D bottom = vertex(point, group.axial_min_mm);
                const QVector3D top = vertex(point, group.axial_max_mm);
                glVertex3f(bottom.x(), bottom.y(), bottom.z());
                glVertex3f(top.x(), top.y(), top.z());
            }
            glEnd();
        }
    }

    // Тела, не уложившиеся в плоскую группу: повёрнутые, стоящие поперёк или
    // режущие толщину лишь частично. Они рисуются как есть, а вычитаемые —
    // каркасом: в кадре они означают полость, а не металл.
    for (const int shape_index : standalone) {
        const ShapeParameters &shape = shapes[shape_index];
        const bool selected = shape_index == selected_shape_index_;
        const bool wireframe = shape.operation != 0;
        const QColor body_color = selected ? QColor(62, 151, 210)
                                           : (wireframe ? QColor(210, 130, 60)
                                                        : QColor(145, 157, 168));
        const double body_alpha = wireframe ? 0.0 : (selected ? 0.98 : 0.9);
        const QColor edge_color = selected ? QColor(255, 210, 68)
                                           : (wireframe ? QColor(235, 165, 90)
                                                        : QColor(225, 235, 242));

        glPushMatrix();
        glTranslated(shape.center_x_mm, shape.center_y_mm, shape.center_z_mm);
        // Тот же порядок, что у сеточного генератора: X, затем Y, затем Z.
        glRotated(shape.rotation_z_deg, 0.0, 0.0, 1.0);
        glRotated(shape.rotation_y_deg, 0.0, 1.0, 0.0);
        glRotated(shape.rotation_x_deg, 1.0, 0.0, 0.0);
        switch (shape.kind) {
        case 1:
            drawShapeCylinder(shape, body_color, body_alpha, edge_color, wireframe);
            break;
        case 2:
            drawShapePrism(shape, body_color, body_alpha, edge_color, wireframe);
            break;
        default: {
            const double half_x = 0.5 * shape.size_x_mm;
            const double half_y = 0.5 * shape.size_y_mm;
            const double half_z = 0.5 * shape.size_z_mm;
            if (!wireframe) {
                drawBox(-half_x, half_x, -half_y, half_y, -half_z, half_z, body_color,
                        body_alpha);
            }
            ::glLineWidth(selected ? 2.2f : 1.5f);
            drawBoxEdges(-half_x, half_x, -half_y, half_y, -half_z, half_z, edge_color);
            break;
        }
        }
        glPopMatrix();
    }
}

void WaveguideOpenGLWidget::drawShapeCylinder(const ShapeParameters &shape,
                                              const QColor &body_color,
                                              double body_alpha,
                                              const QColor &edge_color,
                                              bool wireframe) const
{
    constexpr int segments = 40;
    const double half_length = 0.5 * shape.length_mm;
    const double radius = shape.radius_mm;
    // Ось тела задаёт, какая пара координат образует окружность: остальная
    // геометрия одинакова, поэтому точка строится одной лямбдой.
    const auto point = [&shape, radius, half_length](double angle, double axial) {
        const double u = radius * std::cos(angle);
        const double v = radius * std::sin(angle);
        switch (shape.axis) {
        case 0:
            return QVector3D(static_cast<float>(axial * half_length),
                             static_cast<float>(u),
                             static_cast<float>(v));
        case 1:
            return QVector3D(static_cast<float>(v),
                             static_cast<float>(axial * half_length),
                             static_cast<float>(u));
        default:
            return QVector3D(static_cast<float>(u),
                             static_cast<float>(v),
                             static_cast<float>(axial * half_length));
        }
    };

    if (!wireframe) {
        setColor(body_color, body_alpha);
        glBegin(GL_QUAD_STRIP);
        for (int segment = 0; segment <= segments; ++segment) {
            const double angle = 2.0 * pi * segment / segments;
            const QVector3D bottom = point(angle, -1.0);
            const QVector3D top = point(angle, 1.0);
            glVertex3f(bottom.x(), bottom.y(), bottom.z());
            glVertex3f(top.x(), top.y(), top.z());
        }
        glEnd();
        for (const double axial : {-1.0, 1.0}) {
            // Торец — веер от точки на оси тела к ободу.
            glBegin(GL_TRIANGLE_FAN);
            glVertex3f(shape.axis == 0 ? static_cast<float>(axial * half_length) : 0.0f,
                       shape.axis == 1 ? static_cast<float>(axial * half_length) : 0.0f,
                       shape.axis == 2 ? static_cast<float>(axial * half_length) : 0.0f);
            for (int segment = 0; segment <= segments; ++segment) {
                const QVector3D rim = point(2.0 * pi * segment / segments, axial);
                glVertex3f(rim.x(), rim.y(), rim.z());
            }
            glEnd();
        }
    }

    setColor(edge_color, 0.95);
    for (const double axial : {-1.0, 1.0}) {
        glBegin(GL_LINE_LOOP);
        for (int segment = 0; segment < segments; ++segment) {
            const QVector3D rim = point(2.0 * pi * segment / segments, axial);
            glVertex3f(rim.x(), rim.y(), rim.z());
        }
        glEnd();
    }
    glBegin(GL_LINES);
    for (int segment = 0; segment < 4; ++segment) {
        const double angle = 0.5 * pi * segment;
        const QVector3D bottom = point(angle, -1.0);
        const QVector3D top = point(angle, 1.0);
        glVertex3f(bottom.x(), bottom.y(), bottom.z());
        glVertex3f(top.x(), top.y(), top.z());
    }
    glEnd();
}

void WaveguideOpenGLWidget::drawShapePrism(const ShapeParameters &shape,
                                           const QColor &body_color,
                                           double body_alpha,
                                           const QColor &edge_color,
                                           bool wireframe) const
{
    const int count = shape.profile_mm.size();
    if (count < 3) {
        return;
    }
    const double half_length = 0.5 * shape.length_mm;
    // Профиль живёт в плоскости, перпендикулярной оси, в тех же координатах,
    // что и в сеточном скрипте: для оси Z это (x, y), для Y — (z, x), для X —
    // (y, z). Иначе тело в кадре и тело в расчёте разъехались бы.
    const auto point = [&shape, half_length](const QPointF &profile_point, double axial) {
        switch (shape.axis) {
        case 0:
            return QVector3D(static_cast<float>(axial * half_length),
                             static_cast<float>(profile_point.x()),
                             static_cast<float>(profile_point.y()));
        case 1:
            return QVector3D(static_cast<float>(profile_point.y()),
                             static_cast<float>(axial * half_length),
                             static_cast<float>(profile_point.x()));
        default:
            return QVector3D(static_cast<float>(profile_point.x()),
                             static_cast<float>(profile_point.y()),
                             static_cast<float>(axial * half_length));
        }
    };

    if (!wireframe) {
        setColor(body_color, body_alpha);
        // Торцы: без них тело выглядит полым — в кадре видна только обводка
        // профиля, а сквозь неё просвечивает всё, что стоит за телом.
        const QVector<int> triangles = triangulateProfile(shape.profile_mm);
        glBegin(GL_TRIANGLES);
        for (const double axial : {-1.0, 1.0}) {
            for (int index = 0; index < triangles.size(); ++index) {
                const QVector3D vertex = point(shape.profile_mm[triangles[index]], axial);
                glVertex3f(vertex.x(), vertex.y(), vertex.z());
            }
        }
        glEnd();
        // Боковая поверхность.
        glBegin(GL_QUADS);
        for (int index = 0; index < count; ++index) {
            const QPointF &from = shape.profile_mm[index];
            const QPointF &to = shape.profile_mm[(index + 1) % count];
            const QVector3D a = point(from, -1.0);
            const QVector3D b = point(to, -1.0);
            const QVector3D c = point(to, 1.0);
            const QVector3D d = point(from, 1.0);
            glVertex3f(a.x(), a.y(), a.z());
            glVertex3f(b.x(), b.y(), b.z());
            glVertex3f(c.x(), c.y(), c.z());
            glVertex3f(d.x(), d.y(), d.z());
        }
        glEnd();
    }

    setColor(edge_color, 0.95);
    for (const double axial : {-1.0, 1.0}) {
        glBegin(GL_LINE_LOOP);
        for (const QPointF &profile_point : shape.profile_mm) {
            const QVector3D vertex = point(profile_point, axial);
            glVertex3f(vertex.x(), vertex.y(), vertex.z());
        }
        glEnd();
    }
    glBegin(GL_LINES);
    for (const QPointF &profile_point : shape.profile_mm) {
        const QVector3D bottom = point(profile_point, -1.0);
        const QVector3D top = point(profile_point, 1.0);
        glVertex3f(bottom.x(), bottom.y(), bottom.z());
        glVertex3f(top.x(), top.y(), top.z());
    }
    glEnd();
}

void WaveguideOpenGLWidget::drawPecPlates() const
{
    for (int plate_index = 0; plate_index < result_.parameters.pec_plates.size(); ++plate_index) {
        const PecPlateParameters &plate = result_.parameters.pec_plates[plate_index];
        if (!plate.enabled) {
            continue;
        }
        const bool selected = plate_index == selected_plate_index_;
        const double center_x = 0.5 * (plate.x_min_mm + plate.x_max_mm);
        const double center_y = 0.5 * (plate.y_min_mm + plate.y_max_mm);
        const double center_z = 0.5 * (plate.z_min_mm + plate.z_max_mm);
        const double half_x = 0.5 * (plate.x_max_mm - plate.x_min_mm);
        const double half_y = 0.5 * (plate.y_max_mm - plate.y_min_mm);
        const double half_z = 0.5 * (plate.z_max_mm - plate.z_min_mm);

        const QColor body_color = selected ? QColor(62, 151, 210) : QColor(145, 157, 168);
        const double body_alpha = selected ? 0.98 : 0.92;
        const QColor edge_color = selected ? QColor(255, 210, 68) : QColor(225, 235, 242);
        const bool has_window = plate.aperture_enabled &&
                                plate.aperture_width_mm > 0.0 &&
                                plate.aperture_height_mm > 0.0;

        glPushMatrix();
        glTranslated(center_x, center_y, center_z);
        // Match the gmsh mesher: rotations are applied about the world axes in
        // X, then Y, then Z order, so the composed matrix is Rz*Ry*Rx.
        glRotated(plate.rotation_z_deg, 0.0, 0.0, 1.0);
        glRotated(plate.rotation_y_deg, 0.0, 1.0, 0.0);
        glRotated(plate.rotation_x_deg, 1.0, 0.0, 0.0);
        const bool circular_window = has_window && plate.aperture_shape == 1;
        if (circular_window) {
            // Plate with a round hole: a radial fan from the circle out to the
            // rectangular outline, drawn on both faces, plus the bore wall.
            constexpr int segments = 48;
            const double cx = plate.aperture_offset_x_mm;
            const double cy = plate.aperture_offset_y_mm;
            const double r = plate.aperture_radius_mm;
            const auto outline_point = [&](double angle) {
                // Where the ray from the hole centre meets the plate rectangle.
                const double dx = std::cos(angle);
                const double dy = std::sin(angle);
                const double tx = std::abs(dx) > 1.0e-9
                                      ? ((dx > 0.0 ? half_x - cx : -half_x - cx) / dx)
                                      : 1.0e30;
                const double ty = std::abs(dy) > 1.0e-9
                                      ? ((dy > 0.0 ? half_y - cy : -half_y - cy) / dy)
                                      : 1.0e30;
                const double t = std::min(tx, ty);
                return QVector3D(static_cast<float>(cx + dx * t),
                                 static_cast<float>(cy + dy * t),
                                 0.0f);
            };

            setColor(body_color, body_alpha);
            for (int face = 0; face < 2; ++face) {
                const double z = face == 0 ? -half_z : half_z;
                glBegin(GL_QUADS);
                for (int i = 0; i < segments; ++i) {
                    const double a0 = 2.0 * pi * i / segments;
                    const double a1 = 2.0 * pi * (i + 1) / segments;
                    const QVector3D o0 = outline_point(a0);
                    const QVector3D o1 = outline_point(a1);
                    glVertex3d(cx + r * std::cos(a0), cy + r * std::sin(a0), z);
                    glVertex3d(cx + r * std::cos(a1), cy + r * std::sin(a1), z);
                    glVertex3d(o1.x(), o1.y(), z);
                    glVertex3d(o0.x(), o0.y(), z);
                }
                glEnd();
            }
            glBegin(GL_QUADS);   // bore wall
            for (int i = 0; i < segments; ++i) {
                const double a0 = 2.0 * pi * i / segments;
                const double a1 = 2.0 * pi * (i + 1) / segments;
                glVertex3d(cx + r * std::cos(a0), cy + r * std::sin(a0), -half_z);
                glVertex3d(cx + r * std::cos(a1), cy + r * std::sin(a1), -half_z);
                glVertex3d(cx + r * std::cos(a1), cy + r * std::sin(a1), half_z);
                glVertex3d(cx + r * std::cos(a0), cy + r * std::sin(a0), half_z);
            }
            glEnd();
            ::glLineWidth(selected ? 3.2f : 2.0f);
            setColor(edge_color, 0.9);
            for (int face = 0; face < 2; ++face) {
                const double z = face == 0 ? -half_z : half_z;
                glBegin(GL_LINE_LOOP);
                for (int i = 0; i < segments; ++i) {
                    const double a = 2.0 * pi * i / segments;
                    glVertex3d(cx + r * std::cos(a), cy + r * std::sin(a), z);
                }
                glEnd();
            }
            drawBoxEdges(-half_x, half_x, -half_y, half_y, -half_z, half_z, edge_color);

            drawPlateStub(plate, half_x, half_y, half_z, body_color, body_alpha, edge_color);
        } else if (has_window) {
            // Draw the diaphragm as four frame segments around the window so the
            // aperture reads as an actual opening.
            const double window_x0 =
                plate.aperture_offset_x_mm - 0.5 * plate.aperture_width_mm;
            const double window_x1 =
                plate.aperture_offset_x_mm + 0.5 * plate.aperture_width_mm;
            const double window_y0 =
                plate.aperture_offset_y_mm - 0.5 * plate.aperture_height_mm;
            const double window_y1 =
                plate.aperture_offset_y_mm + 0.5 * plate.aperture_height_mm;
            const double segments[4][4] = {
                {-half_x, window_x0, -half_y, half_y},        // left bar
                {window_x1, half_x, -half_y, half_y},         // right bar
                {window_x0, window_x1, -half_y, window_y0},   // bottom bar
                {window_x0, window_x1, window_y1, half_y},    // top bar
            };
            for (const auto &segment : segments) {
                if (segment[1] <= segment[0] || segment[3] <= segment[2]) {
                    continue;
                }
                drawBox(segment[0], segment[1], segment[2], segment[3],
                        -half_z, half_z, body_color, body_alpha);
                ::glLineWidth(selected ? 3.2f : 2.0f);
                drawBoxEdges(segment[0], segment[1], segment[2], segment[3],
                             -half_z, half_z, edge_color);
            }
            drawPlateStub(plate, half_x, half_y, half_z, body_color, body_alpha, edge_color);
        } else {
            drawBox(-half_x, half_x, -half_y, half_y, -half_z, half_z,
                    body_color, body_alpha);
            ::glLineWidth(selected ? 3.2f : 2.0f);
            drawBoxEdges(-half_x, half_x, -half_y, half_y, -half_z, half_z, edge_color);
        }

        if (selected) {
            // Keep the selected object's silhouette visible through dense field lines.
            ::glDisable(GL_DEPTH_TEST);
            ::glLineWidth(1.3f);
            drawBoxEdges(-half_x,
                         half_x,
                         -half_y,
                         half_y,
                         -half_z,
                         half_z,
                         QColor(255, 210, 68));
            ::glEnable(GL_DEPTH_TEST);
        }
        glPopMatrix();
    }
}

void WaveguideOpenGLWidget::drawSlot() const
{
    if (!result_.parameters.slot_enabled) {
        return;
    }

    const WaveguideParameters &parameters = result_.parameters;
    const SlotSurfaceInfo surface = slotSurfaceInfo(parameters.slot_surface);
    const double angle_rad = parameters.slot_rotation_deg * pi / 180.0;
    const double half_width_mm = 0.5 * parameters.slot_width_mm;
    const double half_length_mm = 0.5 * parameters.slot_length_mm;
    const QVector3D center = slotCenterModel(parameters);
    const QVector3D length_axis = safeNormal(surface.v_axis * static_cast<float>(std::cos(angle_rad)) +
                                                surface.u_axis * static_cast<float>(std::sin(angle_rad)),
                                            surface.v_axis);
    const QVector3D width_axis = safeNormal(surface.u_axis * static_cast<float>(std::cos(angle_rad)) -
                                               surface.v_axis * static_cast<float>(std::sin(angle_rad)),
                                           surface.u_axis);
    const QVector3D half_length = length_axis * static_cast<float>(half_length_mm);
    const QVector3D half_width = width_axis * static_cast<float>(half_width_mm);
    const QVector3D normal_lift = surface.normal * static_cast<float>(std::max(0.015,
                                                                               parameters.wall_thickness_mm * 0.04));
    const QVector3D p0 = center - half_width - half_length + normal_lift;
    const QVector3D p1 = center + half_width - half_length + normal_lift;
    const QVector3D p2 = center + half_width + half_length + normal_lift;
    const QVector3D p3 = center - half_width + half_length + normal_lift;
    const QColor outline_color = slot_editing_
                                     ? QColor(255, 226, 86)
                                     : (slot_selected_ ? QColor(134, 255, 220) : QColor(82, 172, 255));

    setColor(QColor(16, 70, 126), 0.82);
    glBegin(GL_QUADS);
    glVertex3f(p0.x(), p0.y(), p0.z());
    glVertex3f(p1.x(), p1.y(), p1.z());
    glVertex3f(p2.x(), p2.y(), p2.z());
    glVertex3f(p3.x(), p3.y(), p3.z());
    glEnd();

    ::glLineWidth(slot_selected_ ? 3.0f : 2.1f);
    setColor(outline_color, 0.96);
    glBegin(GL_LINE_LOOP);
    glVertex3f(p0.x(), p0.y(), p0.z());
    glVertex3f(p1.x(), p1.y(), p1.z());
    glVertex3f(p2.x(), p2.y(), p2.z());
    glVertex3f(p3.x(), p3.y(), p3.z());
    glEnd();

    if (slot_selected_) {
        const QVector3D axis_start = center - half_length * 0.72f + normal_lift * 1.6f;
        const QVector3D axis_end = center + half_length * 0.72f + normal_lift * 1.6f;
        ::glLineWidth(1.5f);
        setColor(outline_color, 0.72);
        glBegin(GL_LINES);
        glVertex3f(axis_start.x(), axis_start.y(), axis_start.z());
        glVertex3f(axis_end.x(), axis_end.y(), axis_end.z());
        glEnd();
    }
}

void WaveguideOpenGLWidget::drawBox(double min_x,
                                    double max_x,
                                    double min_y,
                                    double max_y,
                                    double min_z,
                                    double max_z,
                                    const QColor &color,
                                    double alpha) const
{
    setColor(color, alpha);
    glBegin(GL_QUADS);
    glVertex3d(min_x, min_y, min_z);
    glVertex3d(max_x, min_y, min_z);
    glVertex3d(max_x, max_y, min_z);
    glVertex3d(min_x, max_y, min_z);

    glVertex3d(min_x, min_y, max_z);
    glVertex3d(min_x, max_y, max_z);
    glVertex3d(max_x, max_y, max_z);
    glVertex3d(max_x, min_y, max_z);

    glVertex3d(min_x, min_y, min_z);
    glVertex3d(min_x, min_y, max_z);
    glVertex3d(max_x, min_y, max_z);
    glVertex3d(max_x, min_y, min_z);

    glVertex3d(min_x, max_y, min_z);
    glVertex3d(max_x, max_y, min_z);
    glVertex3d(max_x, max_y, max_z);
    glVertex3d(min_x, max_y, max_z);

    glVertex3d(min_x, min_y, min_z);
    glVertex3d(min_x, max_y, min_z);
    glVertex3d(min_x, max_y, max_z);
    glVertex3d(min_x, min_y, max_z);

    glVertex3d(max_x, min_y, min_z);
    glVertex3d(max_x, min_y, max_z);
    glVertex3d(max_x, max_y, max_z);
    glVertex3d(max_x, max_y, min_z);
    glEnd();
}

void WaveguideOpenGLWidget::drawBoxEdges(double min_x,
                                         double max_x,
                                         double min_y,
                                         double max_y,
                                         double min_z,
                                         double max_z,
                                         const QColor &color) const
{
    setColor(color, 0.82);
    glBegin(GL_LINES);
    glVertex3d(min_x, min_y, min_z); glVertex3d(max_x, min_y, min_z);
    glVertex3d(max_x, min_y, min_z); glVertex3d(max_x, max_y, min_z);
    glVertex3d(max_x, max_y, min_z); glVertex3d(min_x, max_y, min_z);
    glVertex3d(min_x, max_y, min_z); glVertex3d(min_x, min_y, min_z);

    glVertex3d(min_x, min_y, max_z); glVertex3d(max_x, min_y, max_z);
    glVertex3d(max_x, min_y, max_z); glVertex3d(max_x, max_y, max_z);
    glVertex3d(max_x, max_y, max_z); glVertex3d(min_x, max_y, max_z);
    glVertex3d(min_x, max_y, max_z); glVertex3d(min_x, min_y, max_z);

    glVertex3d(min_x, min_y, min_z); glVertex3d(min_x, min_y, max_z);
    glVertex3d(max_x, min_y, min_z); glVertex3d(max_x, min_y, max_z);
    glVertex3d(max_x, max_y, min_z); glVertex3d(max_x, max_y, max_z);
    glVertex3d(min_x, max_y, min_z); glVertex3d(min_x, max_y, max_z);
    glEnd();
}

void WaveguideOpenGLWidget::drawFields() const
{
    const GLboolean depth_test_enabled = ::glIsEnabled(GL_DEPTH_TEST);
    ::glDisable(GL_DEPTH_TEST);
    ::glLineWidth(1.0f);

    for (const FieldGlyph &glyph : result_.field_glyphs) {
        if (!shouldDrawGlyph(glyph.type)) {
            continue;
        }

        // Heads every few millimetres of arc length: small enough not to bury
        // the line they sit on, frequent enough to read the direction anywhere
        // along a long loop rather than only at its two thirds.
        const double arrow_spacing_mm = std::max(1.8, modelRadiusMm() * 0.055);
        if (glyph.type == FieldGlyphType::ElectricLine && glyph.points.size() > 3) {
            drawPolylineWithArrow(glyph, 0.30, arrow_spacing_mm, 1.05f);
        } else if ((glyph.type == FieldGlyphType::ElectricArrow ||
                    glyph.type == FieldGlyphType::MagneticArrow ||
                    glyph.type == FieldGlyphType::SurfaceCurrentArrow ||
                    glyph.type == FieldGlyphType::PoyntingArrow) &&
            glyph.points.size() >= 2) {
            drawArrow(glyph);
        } else if (glyph.type == FieldGlyphType::MagneticLine) {
            drawPolylineWithArrow(glyph, 0.34, arrow_spacing_mm, 1.25f);
        } else if (glyph.type == FieldGlyphType::SurfaceCurrentLine) {
            drawPolylineWithArrow(glyph, 0.30, arrow_spacing_mm, 1.1f);
        }
    }

    if (depth_test_enabled) {
        ::glEnable(GL_DEPTH_TEST);
    }
}

void WaveguideOpenGLWidget::drawArrow(const FieldGlyph &glyph) const
{
    QVector3D start = glyph.points[0];
    QVector3D end = glyph.points[1];
    double intensity = std::clamp(glyph.magnitude, 0.0, 1.0);

    if (glyph.animated && animation_enabled_ && glyph.reference_magnitude > 0.0) {
        // Instantaneous vector Re(F * e^{j*phase}) = Re(F)cos - Im(F)sin.
        const QVector3D instantaneous =
            glyph.phasor_real * static_cast<float>(std::cos(animation_phase_)) -
            glyph.phasor_imag * static_cast<float>(std::sin(animation_phase_));
        const double magnitude = static_cast<double>(instantaneous.length());
        const double normalized = std::clamp(magnitude / glyph.reference_magnitude, 0.0, 1.0);
        if (normalized < 0.02) {
            return;   // near a temporal zero-crossing: nothing to draw this frame
        }
        const QVector3D direction = safeNormal(instantaneous, QVector3D(0.0f, 1.0f, 0.0f));
        const float half_length = static_cast<float>(0.5 * glyph.animation_length_mm * normalized);
        start = glyph.anchor - direction * half_length;
        end = glyph.anchor + direction * half_length;
        intensity = normalized;
    }

    const QVector3D direction = safeNormal(end - start, QVector3D(0.0f, 1.0f, 0.0f));
    const QVector3D reference = std::abs(QVector3D::dotProduct(direction,
                                                              QVector3D(0.0f, 1.0f, 0.0f))) < 0.86f
                                    ? QVector3D(0.0f, 1.0f, 0.0f)
                                    : QVector3D(1.0f, 0.0f, 0.0f);
    const QVector3D side_hint = safeNormal(QVector3D::crossProduct(direction, reference),
                                           QVector3D(0.0f, 0.0f, 1.0f));
    const double shaft_length = static_cast<double>((end - start).length());

    const QColor &color = glyph.color;
    const double alpha = 0.34 + 0.62 * intensity;
    setColor(color, alpha);
    ::glLineWidth(static_cast<GLfloat>(0.70 + 1.35 * intensity));
    glBegin(GL_LINES);
    glVertex3f(start.x(), start.y(), start.z());
    glVertex3f(end.x(), end.y(), end.z());
    glEnd();

    drawArrowHead(end,
                  direction,
                  side_hint,
                  color,
                  std::clamp(shaft_length * 0.24, 0.55, 1.35),
                  alpha);
}

void WaveguideOpenGLWidget::drawPlateStub(const PecPlateParameters &plate,
                                          double half_x,
                                          double half_y,
                                          double half_z,
                                          const QColor &body_color,
                                          double body_alpha,
                                          const QColor &edge_color) const
{
    if (!plate.post_enabled || plate.post_width_mm <= 0.0 || plate.post_height_mm <= 0.0) {
        return;
    }
    // Rectangular stub in the plane of the plate, growing up from its bottom
    // edge along the window centre line; same thickness as the plate.
    const double x0 = plate.aperture_offset_x_mm - 0.5 * plate.post_width_mm;
    const double x1 = plate.aperture_offset_x_mm + 0.5 * plate.post_width_mm;
    const double y0 = -half_y;
    const double y1 = std::min(half_y, -half_y + plate.post_height_mm);
    if (x1 <= x0 || y1 <= y0) {
        return;
    }
    Q_UNUSED(half_x);
    drawBox(x0, x1, y0, y1, -half_z, half_z, body_color, body_alpha);
    ::glLineWidth(2.0f);
    drawBoxEdges(x0, x1, y0, y1, -half_z, half_z, edge_color);
}

void WaveguideOpenGLWidget::drawSliceCells(const FieldSlice &slice,
                                           double maximum_value,
                                           double alpha) const
{
    if (!slice.valid || slice.cells.isEmpty() || maximum_value <= 0.0) {
        return;
    }

    const float cos_phase = static_cast<float>(std::cos(animation_phase_));
    const float sin_phase = static_cast<float>(std::sin(animation_phase_));

    glBegin(GL_QUADS);
    for (const FieldSliceCell &cell : slice.cells) {
        double value = cell.envelope;
        if (animation_enabled_) {
            const QVector3D instantaneous = cell.phasor_real * cos_phase -
                                            cell.phasor_imag * sin_phase;
            value = static_cast<double>(instantaneous.length());
        }
        if (value <= 0.0) {
            continue;   // metal or unsampled cell -> leave a clean gap
        }
        const double t = fieldHeatNormalize(value, maximum_value);
        const QColor color = fieldHeatColor(t);
        glColor4d(color.redF(), color.greenF(), color.blueF(), alpha);
        const QVector3D &c = cell.center;
        const QVector3D &u = cell.u_half;
        const QVector3D &v = cell.v_half;
        const QVector3D p0 = c - u - v;
        const QVector3D p1 = c + u - v;
        const QVector3D p2 = c + u + v;
        const QVector3D p3 = c - u + v;
        glVertex3f(p0.x(), p0.y(), p0.z());
        glVertex3f(p1.x(), p1.y(), p1.z());
        glVertex3f(p2.x(), p2.y(), p2.z());
        glVertex3f(p3.x(), p3.y(), p3.z());
    }
    glEnd();
}

void WaveguideOpenGLWidget::drawFieldSlice() const
{
    const FieldSlice &slice = slice_plane_ == FieldSlicePlane::HorizontalXZ
                                  ? result_.horizontal_slice
                                  : result_.vertical_slice;
    const GLboolean depth_was_enabled = ::glIsEnabled(GL_DEPTH_TEST);
    ::glDisable(GL_DEPTH_TEST);
    drawSliceCells(slice, slice.maximum_value, 0.82);
    if (depth_was_enabled) {
        ::glEnable(GL_DEPTH_TEST);
    }
}

void WaveguideOpenGLWidget::drawVolumeSlices() const
{
    if (volume_slices_.isEmpty()) {
        return;
    }

    // Общий масштаб цвета на всю стопку: одна и та же напряжённость должна
    // давать один и тот же цвет на любой глубине.
    double maximum_value = 0.0;
    for (const FieldSlice &slice : volume_slices_) {
        maximum_value = std::max(maximum_value, slice.maximum_value);
    }
    if (maximum_value <= 0.0) {
        return;
    }

    // Полупрозрачные плоскости смешиваются правильно только от дальней к
    // ближней, поэтому стопка сортируется по глубине точки-представителя
    // каждого среза в координатах камеры.
    const QMatrix4x4 model_view = modelViewMatrix();
    QVector<int> order;
    QVector<double> eye_depth;
    order.reserve(volume_slices_.size());
    eye_depth.reserve(volume_slices_.size());
    for (int index = 0; index < volume_slices_.size(); ++index) {
        const FieldSlice &slice = volume_slices_[index];
        const QVector3D representative =
            slice.cells.isEmpty()
                ? QVector3D()
                : slice.cells[slice.cells.size() / 2].center;
        order.push_back(index);
        eye_depth.push_back(
            static_cast<double>(model_view.map(representative).z()));
    }
    std::sort(order.begin(), order.end(), [&eye_depth](int left, int right) {
        return eye_depth[left] < eye_depth[right];   // дальние (z меньше) первыми
    });

    const GLboolean depth_was_enabled = ::glIsEnabled(GL_DEPTH_TEST);
    ::glDisable(GL_DEPTH_TEST);
    // Прозрачность подобрана так, чтобы сквозь стопку читались и дальние
    // плоскости, и стрелки поля поверх неё.
    const double alpha =
        std::clamp(2.6 / std::max(1, static_cast<int>(volume_slices_.size())), 0.10, 0.45);
    for (const int index : order) {
        drawSliceCells(volume_slices_[index], maximum_value, alpha);
    }
    if (depth_was_enabled) {
        ::glEnable(GL_DEPTH_TEST);
    }
}

void WaveguideOpenGLWidget::drawArrowHead(const QVector3D &position,
                                          const QVector3D &direction,
                                           const QVector3D &side_hint,
                                           const QColor &color,
                                           double size,
                                           double alpha) const
{
    const QVector3D forward = safeNormal(direction, QVector3D(0.0f, 1.0f, 0.0f));
    const QVector3D side = safeNormal(side_hint, QVector3D(1.0f, 0.0f, 0.0f));

    setColor(color, alpha);

    const double head_length = size;
    const double half_width = size * 0.18;
    const QVector3D base_center = position - forward * static_cast<float>(head_length);
    const QVector3D left = base_center + side * static_cast<float>(half_width);
    const QVector3D right = base_center - side * static_cast<float>(half_width);

    glBegin(GL_TRIANGLES);
    glVertex3f(position.x(), position.y(), position.z());
    glVertex3f(left.x(), left.y(), left.z());
    glVertex3f(right.x(), right.y(), right.z());
    glEnd();
}

void WaveguideOpenGLWidget::drawPolylineWithArrow(const FieldGlyph &glyph,
                                                  double arrow_size,
                                                  double arrow_spacing_mm,
                                                  float line_width) const
{
    const QVector<QVector3D> &points = glyph.points;
    const int point_count = static_cast<int>(points.size());
    if (point_count < 2) {
        return;
    }

    const bool animated = animation_enabled_ && glyph.animated &&
                          glyph.vertex_phase_rad.size() == points.size() &&
                          glyph.vertex_amplitude.size() == points.size();
    // Instantaneous field along the line, signed: positive where it points the
    // way the line was traced, negative half a period later.
    const auto valueAt = [this, &glyph](int index) {
        return static_cast<double>(glyph.vertex_amplitude[index]) *
               std::cos(static_cast<double>(glyph.vertex_phase_rad[index]) +
                        animation_phase_);
    };

    ::glLineWidth(line_width);
    glBegin(GL_LINE_STRIP);
    for (int index = 0; index < point_count; ++index) {
        // Brightness follows the field, so the crest travels along the line and
        // the picture stops looking like a frozen drawing.
        setColor(glyph.color,
                 animated ? 0.10 + 0.82 * std::abs(valueAt(index)) : 0.88);
        glVertex3f(points[index].x(), points[index].y(), points[index].z());
    }
    glEnd();

    const double spacing_mm = std::max(0.35, arrow_spacing_mm);
    double distance_to_next_mm = 0.5 * spacing_mm;
    for (int index = 1; index < point_count; ++index) {
        const QVector3D segment = points[index] - points[index - 1];
        distance_to_next_mm -= static_cast<double>(segment.length());
        if (distance_to_next_mm > 0.0) {
            continue;
        }
        distance_to_next_mm += spacing_mm;
        if (segment.lengthSquared() < 1.0e-12f) {
            continue;
        }

        double intensity = 1.0;
        QVector3D direction = segment;
        if (animated) {
            const double value = valueAt(index);
            intensity = std::abs(value);
            if (intensity < 0.06) {
                continue;   // this stretch is at a temporal zero right now
            }
            if (value < 0.0) {
                direction = -direction;   // the field has reversed, so does the head
            }
        }

        // The plane of the head comes from the local bend of the line. Taking it
        // once from the first points, as this used to, left every head on a
        // curved loop tilted out of the curve it belongs to.
        const QVector3D previous_segment = points[index - 1] -
                                           points[std::max(0, index - 2)];
        QVector3D plane_normal = QVector3D::crossProduct(previous_segment, segment);
        if (plane_normal.lengthSquared() < 1.0e-12f) {
            plane_normal = std::abs(QVector3D::dotProduct(direction.normalized(),
                                                          QVector3D(0.0f, 1.0f, 0.0f))) < 0.86f
                               ? QVector3D(0.0f, 1.0f, 0.0f)
                               : QVector3D(1.0f, 0.0f, 0.0f);
        }
        QVector3D side_hint = QVector3D::crossProduct(plane_normal, direction);
        if (side_hint.lengthSquared() < 1.0e-12f) {
            side_hint = QVector3D(0.0f, 0.0f, 1.0f);
        }
        drawArrowHead(points[index], direction, side_hint, glyph.color, arrow_size,
                      animated ? 0.30 + 0.66 * intensity : 0.96);
    }
}

void WaveguideOpenGLWidget::drawAxes() const
{
    const double axis_length = std::max(18.0, modelRadiusMm() * 0.72);
    const QColor x_color(220, 76, 76);
    const QColor y_color(86, 210, 120);
    const QColor z_color(82, 150, 240);

    ::glLineWidth(1.5f);
    glBegin(GL_LINES);
    setColor(x_color, 0.9);
    glVertex3d(0.0, 0.0, 0.0);
    glVertex3d(axis_length, 0.0, 0.0);
    setColor(y_color, 0.9);
    glVertex3d(0.0, 0.0, 0.0);
    glVertex3d(0.0, axis_length, 0.0);
    setColor(z_color, 0.9);
    glVertex3d(0.0, 0.0, 0.0);
    glVertex3d(0.0, 0.0, axis_length);
    glEnd();

    // Подписи у концов осей: без них по трём цветным отрезкам не сказать, где
    // ширина, где высота, а где направление распространения.
    const double label_size = std::max(1.5, 0.05 * axis_length);
    const double label_offset = axis_length + 2.2 * label_size;
    drawAxisLabel('X', QVector3D(static_cast<float>(label_offset), 0.0f, 0.0f), label_size,
                  x_color);
    drawAxisLabel('Y', QVector3D(0.0f, static_cast<float>(label_offset), 0.0f), label_size,
                  y_color);
    drawAxisLabel('Z', QVector3D(0.0f, 0.0f, static_cast<float>(label_offset)), label_size,
                  z_color);
}

void WaveguideOpenGLWidget::drawAxisLabel(char letter,
                                          const QVector3D &position,
                                          double size,
                                          const QColor &color) const
{
    // Оси экрана достаются из текущей матрицы вида, поэтому буква всегда
    // повёрнута к наблюдателю, как бы он ни крутил модель.
    GLdouble modelview[16] = {0.0};
    glGetDoublev(GL_MODELVIEW_MATRIX, modelview);
    const QVector3D right(static_cast<float>(modelview[0]),
                          static_cast<float>(modelview[4]),
                          static_cast<float>(modelview[8]));
    const QVector3D up(static_cast<float>(modelview[1]),
                       static_cast<float>(modelview[5]),
                       static_cast<float>(modelview[9]));
    if (right.lengthSquared() < 1.0e-8f || up.lengthSquared() < 1.0e-8f) {
        return;
    }

    // Штрихи глифа в координатах (вправо, вверх), в долях половины высоты буквы.
    struct Stroke { double from_u, from_v, to_u, to_v; };
    static const Stroke x_strokes[] = {{-0.6, -1.0, 0.6, 1.0}, {-0.6, 1.0, 0.6, -1.0}};
    static const Stroke y_strokes[] = {
        {-0.6, 1.0, 0.0, 0.0}, {0.6, 1.0, 0.0, 0.0}, {0.0, 0.0, 0.0, -1.0}};
    static const Stroke z_strokes[] = {
        {-0.6, 1.0, 0.6, 1.0}, {0.6, 1.0, -0.6, -1.0}, {-0.6, -1.0, 0.6, -1.0}};
    const Stroke *strokes = x_strokes;
    int stroke_count = 2;
    if (letter == 'Y') {
        strokes = y_strokes;
        stroke_count = 3;
    } else if (letter == 'Z') {
        strokes = z_strokes;
        stroke_count = 3;
    }

    const QVector3D u_axis = right.normalized() * static_cast<float>(size);
    const QVector3D v_axis = up.normalized() * static_cast<float>(size);
    ::glLineWidth(2.0f);
    setColor(color, 0.95);
    glBegin(GL_LINES);
    for (int index = 0; index < stroke_count; ++index) {
        const Stroke &stroke = strokes[index];
        const QVector3D from = position + u_axis * static_cast<float>(stroke.from_u) +
                               v_axis * static_cast<float>(stroke.from_v);
        const QVector3D to = position + u_axis * static_cast<float>(stroke.to_u) +
                             v_axis * static_cast<float>(stroke.to_v);
        glVertex3f(from.x(), from.y(), from.z());
        glVertex3f(to.x(), to.y(), to.z());
    }
    glEnd();
}

void WaveguideOpenGLWidget::drawPropagationArrow() const
{
    if (!result_.valid) {
        return;
    }

    const double width = result_.parameters.width_mm;
    const double depth = result_.parameters.depth_mm;
    const double length = result_.parameters.length_mm;
    const QVector3D start(static_cast<float>(0.68 * width),
                          static_cast<float>(0.62 * depth),
                          static_cast<float>(-0.34 * length));
    const QVector3D end(static_cast<float>(0.68 * width),
                        static_cast<float>(0.62 * depth),
                        static_cast<float>(0.34 * length));
    const QColor propagation_color(52, 208, 104);

    const GLboolean depth_test_enabled = ::glIsEnabled(GL_DEPTH_TEST);
    ::glDisable(GL_DEPTH_TEST);
    ::glLineWidth(4.0f);
    setColor(propagation_color, 0.94);
    glBegin(GL_LINES);
    glVertex3f(start.x(), start.y(), start.z());
    glVertex3f(end.x(), end.y(), end.z());
    glEnd();

    drawArrowHead(end,
                  QVector3D(0.0f, 0.0f, 1.0f),
                  QVector3D(1.0f, 0.0f, 0.0f),
                  propagation_color,
                  std::max(1.4, modelRadiusMm() * 0.065));

    if (depth_test_enabled) {
        ::glEnable(GL_DEPTH_TEST);
    }
}

bool WaveguideOpenGLWidget::shouldDrawGlyph(FieldGlyphType type) const
{
    if (field_display_mode_ == FieldDisplayMode::Fields) {
        return type == FieldGlyphType::ElectricArrow ||
               type == FieldGlyphType::ElectricLine ||
               type == FieldGlyphType::MagneticArrow ||
               type == FieldGlyphType::MagneticLine;
    }

    if (field_display_mode_ == FieldDisplayMode::Both) {
        return type == FieldGlyphType::ElectricArrow ||
               type == FieldGlyphType::ElectricLine ||
               type == FieldGlyphType::MagneticArrow ||
               type == FieldGlyphType::MagneticLine ||
               type == FieldGlyphType::SurfaceCurrentArrow ||
               type == FieldGlyphType::SurfaceCurrentLine;
    }

    if (field_display_mode_ == FieldDisplayMode::Electric) {
        return type == FieldGlyphType::ElectricArrow ||
               type == FieldGlyphType::ElectricLine;
    }

    if (field_display_mode_ == FieldDisplayMode::Current) {
        return type == FieldGlyphType::SurfaceCurrentLine ||
               type == FieldGlyphType::SurfaceCurrentArrow;
    }

    if (field_display_mode_ == FieldDisplayMode::Poynting) {
        return type == FieldGlyphType::PoyntingArrow;
    }

    return type == FieldGlyphType::MagneticArrow ||
           type == FieldGlyphType::MagneticLine;
}

double WaveguideOpenGLWidget::modelRadiusMm() const
{
    if (!result_.valid) {
        return 60.0;
    }

    const bool circular = result_.parameters.cross_section == 1;
    const double x = circular ? 2.0 * result_.parameters.radius_mm
                              : result_.parameters.width_mm;
    const double y = circular ? 2.0 * result_.parameters.radius_mm
                              : result_.parameters.depth_mm;
    const double z = result_.parameters.length_mm;
    return 0.5 * std::sqrt(x * x + y * y + z * z);
}

void WaveguideOpenGLWidget::setColor(const QColor &color, double alpha) const
{
    glColor4d(color.redF(), color.greenF(), color.blueF(), alpha);
}
