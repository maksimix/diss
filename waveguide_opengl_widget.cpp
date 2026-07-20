#include "waveguide_opengl_widget.h"

#include <QtCore/QRect>
#include <QtGui/QKeyEvent>
#include <QtGui/QMouseEvent>
#include <QtGui/QWheelEvent>
#include <QtGui/qopengl.h>

#include <algorithm>
#include <cmath>
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
    update();
}

void WaveguideOpenGLWidget::setModelPreview(const WaveguideParameters &parameters)
{
    result_.parameters = parameters;
    result_.inner_width_mm = std::max(0.0,
                                      parameters.width_mm - 2.0 * parameters.wall_thickness_mm);
    result_.inner_depth_mm = std::max(0.0,
                                      parameters.depth_mm - 2.0 * parameters.wall_thickness_mm);
    result_.valid = parameters.width_mm > 0.0 &&
                    parameters.depth_mm > 0.0 &&
                    parameters.length_mm > 0.0;
    update();
}

void WaveguideOpenGLWidget::setSelectedPlateIndex(int plate_index)
{
    selected_plate_index_ = plate_index;
    update();
}

void WaveguideOpenGLWidget::setFieldDisplayMode(FieldDisplayMode mode)
{
    field_display_mode_ = mode;
    update();
}

void WaveguideOpenGLWidget::setSliceVisible(bool visible)
{
    show_slice_ = visible;
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
        if (show_slice_) {
            drawFieldSlice();
        }
        drawPecPlates();
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

void WaveguideOpenGLWidget::drawWaveguide() const
{
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

    drawBox(outer_x0, outer_x1, inner_y1, outer_y1, z0, z1, metal_color, 0.22);
    drawBox(outer_x0, outer_x1, outer_y0, inner_y0, z0, z1, metal_color, 0.22);
    drawBox(outer_x0, inner_x0, inner_y0, inner_y1, z0, z1, metal_color, 0.22);
    drawBox(inner_x1, outer_x1, inner_y0, inner_y1, z0, z1, metal_color, 0.22);

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

            if (plate.post_enabled && plate.post_radius_mm > 0.0 &&
                plate.post_length_mm > 0.0) {
                const double pr = plate.post_radius_mm;
                const double pz = 0.5 * plate.post_length_mm;
                setColor(selected ? QColor(255, 196, 96) : QColor(196, 170, 120), 0.96);
                glBegin(GL_QUADS);
                for (int i = 0; i < segments; ++i) {
                    const double a0 = 2.0 * pi * i / segments;
                    const double a1 = 2.0 * pi * (i + 1) / segments;
                    glVertex3d(cx + pr * std::cos(a0), cy + pr * std::sin(a0), -pz);
                    glVertex3d(cx + pr * std::cos(a1), cy + pr * std::sin(a1), -pz);
                    glVertex3d(cx + pr * std::cos(a1), cy + pr * std::sin(a1), pz);
                    glVertex3d(cx + pr * std::cos(a0), cy + pr * std::sin(a0), pz);
                }
                glEnd();
                for (int face = 0; face < 2; ++face) {
                    const double z = face == 0 ? -pz : pz;
                    glBegin(GL_TRIANGLE_FAN);
                    glVertex3d(cx, cy, z);
                    for (int i = 0; i <= segments; ++i) {
                        const double a = 2.0 * pi * i / segments;
                        glVertex3d(cx + pr * std::cos(a), cy + pr * std::sin(a), z);
                    }
                    glEnd();
                }
            }
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

        if (glyph.type == FieldGlyphType::ElectricLine && glyph.points.size() > 3) {
            drawPolylineWithArrow(glyph.points, glyph.color, 0.42, 1, 1.05f);
        } else if ((glyph.type == FieldGlyphType::ElectricArrow ||
                    glyph.type == FieldGlyphType::MagneticArrow ||
                    glyph.type == FieldGlyphType::SurfaceCurrentArrow ||
                    glyph.type == FieldGlyphType::PoyntingArrow) &&
            glyph.points.size() >= 2) {
            drawArrow(glyph);
        } else if (glyph.type == FieldGlyphType::MagneticLine) {
            drawPolylineWithArrow(glyph.points, glyph.color, 0.64, 2, 1.45f);
        } else if (glyph.type == FieldGlyphType::SurfaceCurrentLine) {
            drawPolylineWithArrow(glyph.points, glyph.color, 0.42, 2, 1.1f);
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

void WaveguideOpenGLWidget::drawFieldSlice() const
{
    const FieldSlice &slice = slice_plane_ == FieldSlicePlane::HorizontalXZ
                                  ? result_.horizontal_slice
                                  : result_.vertical_slice;
    if (!slice.valid || slice.cells.isEmpty() || slice.maximum_value <= 0.0) {
        return;
    }

    const GLboolean depth_was_enabled = ::glIsEnabled(GL_DEPTH_TEST);
    ::glDisable(GL_DEPTH_TEST);

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
        const double t = fieldHeatNormalize(value, slice.maximum_value);
        const QColor color = fieldHeatColor(t);
        glColor4d(color.redF(), color.greenF(), color.blueF(), 0.82);
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

void WaveguideOpenGLWidget::drawPolylineWithArrow(const QVector<QVector3D> &points,
                                                  const QColor &color,
                                                  double arrow_size,
                                                  int arrow_count,
                                                  float line_width) const
{
    if (points.size() < 2) {
        return;
    }

    setColor(color, 0.88);
    ::glLineWidth(line_width);
    glBegin(GL_LINE_STRIP);
    for (const QVector3D &point : points) {
        glVertex3f(point.x(), point.y(), point.z());
    }
    glEnd();

    const int point_count = static_cast<int>(points.size());
    const int bounded_arrow_count = std::clamp(arrow_count, 1, 3);
    for (int arrow_number = 0; arrow_number < bounded_arrow_count; ++arrow_number) {
        const double ratio = static_cast<double>(arrow_number + 1) /
                             static_cast<double>(bounded_arrow_count + 1);
        const int arrow_index = std::clamp(static_cast<int>(std::round(ratio * (point_count - 1))),
                                           1,
                                           point_count - 1);
        const QVector3D direction = points[arrow_index] - points[arrow_index - 1];
        QVector3D plane_normal = QVector3D::crossProduct(points[1] - points[0],
                                                         points[std::min(point_count - 1, 3)] - points[0]);
        if (plane_normal.lengthSquared() < 1.0e-8f) {
            plane_normal = QVector3D(0.0f, 1.0f, 0.0f);
        }
        const QVector3D side_hint = QVector3D::crossProduct(plane_normal, direction);
        drawArrowHead(points[arrow_index], direction, side_hint, color, arrow_size);
    }
}

void WaveguideOpenGLWidget::drawAxes() const
{
    const double axis_length = std::max(18.0, modelRadiusMm() * 0.72);

    ::glLineWidth(1.5f);
    glBegin(GL_LINES);
    setColor(QColor(220, 76, 76), 0.9);
    glVertex3d(0.0, 0.0, 0.0);
    glVertex3d(axis_length, 0.0, 0.0);
    setColor(QColor(86, 210, 120), 0.9);
    glVertex3d(0.0, 0.0, 0.0);
    glVertex3d(0.0, axis_length, 0.0);
    setColor(QColor(82, 150, 240), 0.9);
    glVertex3d(0.0, 0.0, 0.0);
    glVertex3d(0.0, 0.0, axis_length);
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

    const double x = result_.parameters.width_mm;
    const double y = result_.parameters.depth_mm;
    const double z = result_.parameters.length_mm;
    return 0.5 * std::sqrt(x * x + y * y + z * z);
}

void WaveguideOpenGLWidget::setColor(const QColor &color, double alpha) const
{
    glColor4d(color.redF(), color.greenF(), color.blueF(), alpha);
}
