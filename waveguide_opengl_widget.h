#pragma once

#include "waveguide_types.h"

#include <QtCore/QPoint>
#include <QtCore/QTimer>
#include <QtGui/QMatrix4x4>
#include <QtGui/QOpenGLFunctions>
#include <QtOpenGLWidgets/QOpenGLWidget>

#include <functional>

enum class FieldDisplayMode
{
    Fields,
    Electric,
    Magnetic,
    Current,
    Poynting,
    Both
};

enum class WaveguideViewPreset
{
    Free3D,
    Top,
    Side
};

// Заливка |E|: выключена, одна плоскость среза или стопка полупрозрачных
// срезов через всю полость (грубое объёмное представление поля).
enum class FieldFillMode
{
    None,
    Slice,
    Volume
};

class WaveguideOpenGLWidget : public QOpenGLWidget, protected QOpenGLFunctions
{
public:
    explicit WaveguideOpenGLWidget(QWidget *parent = nullptr);

    void setCalculationResult(const WaveguideCalculationResult &result);
    // Заменяет только линии и стрелки поля, не трогая остальной результат:
    // так ползунок концентрации стрелок обходится без повторного расчёта.
    void setFieldGlyphs(const QVector<FieldGlyph> &glyphs);
    // Заменяет один срез |E| (например, после переноса плоскости), не трогая
    // остальной результат.
    void setSlice(FieldSlicePlane plane, const FieldSlice &slice);
    // Стопка срезов для объёмной заливки; строится по требованию и живёт
    // отдельно от результата расчёта.
    void setVolumeSlices(const QVector<FieldSlice> &slices);
    void setModelPreview(const WaveguideParameters &parameters);
    void setSelectedPlateIndex(int plate_index);
    void setSelectedShapeIndex(int shape_index);
    // Выбран сам волновод: в автоматическом режиме корпус тогда рисуется
    // сплошным, как в CST при выделении внешнего объекта.
    void setShellSelected(bool selected);
    void setFieldDisplayMode(FieldDisplayMode mode);
    void setViewPreset(WaveguideViewPreset preset);
    void setFieldFillMode(FieldFillMode mode);
    void setSlicePlane(FieldSlicePlane plane);
    void setAnimationEnabled(bool enabled);
    void setSlotEditedCallback(std::function<void(const WaveguideParameters &)> callback);
    void resetView();

protected:
    void initializeGL() override;
    void resizeGL(int width, int height) override;
    void paintGL() override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;

private:
    struct SlotSurfaceInfo
    {
        int id = 0;
        QVector3D normal;
        QVector3D u_axis;
        QVector3D v_axis;
        QVector3D origin;
        double half_u_mm = 0.0;
        double half_z_mm = 0.0;
    };

    struct PickRay
    {
        QVector3D origin;
        QVector3D direction;
    };

    void setupProjection();
    void setupModelView();
    void drawWaveguide() const;
    void drawCircularShell(double inner_radius,
                           double outer_radius,
                           double z0,
                           double z1,
                           const QColor &metal_color,
                           const QColor &edge_color,
                           double body_alpha) const;
    void drawSlot() const;
    // Непрозрачность стенок тракта по режиму отображения корпуса: в
    // автоматическом режиме корпус светлеет, как только в дереве выбрано тело
    // или пластина, — иначе вставку внутри тракта не разглядеть сбоку.
    double shellAlpha() const;
    void drawPecPlates() const;
    // Свободные тела пользователя. Вычитаемые тела рисуются каркасом другого
    // цвета: в кадре они означают не металл, а вырезанную в нём полость.
    void drawUserShapes() const;
    void drawShapeCylinder(const ShapeParameters &shape,
                           const QColor &body_color,
                           double body_alpha,
                           const QColor &edge_color,
                           bool wireframe) const;
    void drawShapePrism(const ShapeParameters &shape,
                        const QColor &body_color,
                        double body_alpha,
                        const QColor &edge_color,
                        bool wireframe) const;
    void drawBox(double min_x,
                 double max_x,
                 double min_y,
                 double max_y,
                 double min_z,
                 double max_z,
                 const QColor &color,
                 double alpha) const;
    void drawBoxEdges(double min_x,
                      double max_x,
                      double min_y,
                      double max_y,
                      double min_z,
                      double max_z,
                      const QColor &color) const;
    void drawFields() const;
    void drawPlateStub(const PecPlateParameters &plate,
                       double half_x,
                       double half_y,
                       double half_z,
                       const QColor &body_color,
                       double body_alpha,
                       const QColor &edge_color) const;
    void drawFieldSlice() const;
    // Клетки одного среза; maximum_value — общий масштаб цвета (у стопки он
    // один на все плоскости, чтобы цвета срезов были сравнимы между собой).
    void drawSliceCells(const FieldSlice &slice,
                        double maximum_value,
                        double alpha) const;
    void drawVolumeSlices() const;
    void drawArrow(const FieldGlyph &glyph) const;
    void drawArrowHead(const QVector3D &position,
                       const QVector3D &direction,
                       const QVector3D &side_hint,
                       const QColor &color,
                       double size,
                       double alpha = 0.96) const;
    // Arrow heads are placed at a fixed spacing along the line rather than a
    // fixed count, so a long loop carries more of them than a short one.
    void drawPolylineWithArrow(const FieldGlyph &glyph,
                               double arrow_size,
                               double arrow_spacing_mm,
                               float line_width) const;
    void drawAxes() const;
    // Буква у конца оси, нарисованная отрезками и всегда развёрнутая к камере:
    // текстовый рендер сюда тянуть незачем, а три глифа рисуются шестью линиями.
    void drawAxisLabel(char letter,
                       const QVector3D &position,
                       double size,
                       const QColor &color) const;
    void drawPropagationArrow() const;
    bool shouldDrawGlyph(FieldGlyphType type) const;
    double modelRadiusMm() const;
    QMatrix4x4 projectionMatrix() const;
    QMatrix4x4 modelViewMatrix() const;
    SlotSurfaceInfo slotSurfaceInfo(int surface_id) const;
    QVector3D slotCenterModel(const WaveguideParameters &parameters) const;
    bool buildPickRay(const QPoint &screen_position, PickRay *ray) const;
    bool intersectSurface(const PickRay &ray,
                          const SlotSurfaceInfo &surface,
                          QVector3D *hit,
                          double *distance) const;
    bool pickWall(const QPoint &screen_position,
                  int *surface_id,
                  double *u_mm,
                  double *z_mm) const;
    bool pickSlot(const QPoint &screen_position) const;
    void updateInteractiveSlotPosition(const QPoint &screen_position);
    void rotateInteractiveSlot(const QPoint &screen_position, const QPoint &delta);
    void clampInteractiveSlotParameters();
    void notifySlotEdited();
    void setColor(const QColor &color, double alpha = 1.0) const;

    WaveguideCalculationResult result_;
    QVector<FieldSlice> volume_slices_;
    std::function<void(const WaveguideParameters &)> slot_edited_callback_;
    FieldDisplayMode field_display_mode_ = FieldDisplayMode::Fields;
    WaveguideViewPreset view_preset_ = WaveguideViewPreset::Free3D;
    FieldSlicePlane slice_plane_ = FieldSlicePlane::HorizontalXZ;
    FieldFillMode fill_mode_ = FieldFillMode::None;
    bool animation_enabled_ = false;
    QTimer animation_timer_;
    QPoint last_mouse_position_;
    double animation_phase_ = 0.0;
    double rotation_x_ = -24.0;
    double rotation_y_ = 34.0;
    double camera_distance_factor_ = 2.7;
    bool slot_selected_ = false;
    bool slot_editing_ = false;
    bool slot_rotating_ = false;
    int selected_plate_index_ = -1;
    int selected_shape_index_ = -1;
    bool shell_selected_ = false;
};
