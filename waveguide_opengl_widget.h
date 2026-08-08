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

// Фон сцены. Тёмный удобен для полей, светлый — для печати и снимков в отчёт,
// как переключение фона в CST.
enum class ViewBackground
{
    Dark,
    Light
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

    // Фон сцены и координатная сетка. Переключаются и кнопками ленты, и
    // клавишами в самом виде, поэтому окно узнаёт о смене через колбэк — иначе
    // отметка на кнопке разошлась бы с тем, что на экране.
    void setBackground(ViewBackground background);
    ViewBackground background() const { return background_; }
    void setGridVisible(bool visible);
    bool isGridVisible() const { return grid_visible_; }
    void setViewSettingsChangedCallback(std::function<void()> callback);

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
    // Гребни, кольцевые сегменты и диэлектрическая сердцевина круглого сечения.
    // Это форма самого тракта, а не вставка в нём, поэтому рисуется вместе с
    // корпусом и не зависит от выбора тела в дереве.
    void drawCircularRidges(double inner_radius, double half_length) const;
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
    // Металл отличается от диэлектрика и от вспомогательной геометрии не
    // цветом, а бликом: доля отражённого света считается по нормали грани и
    // направлению взгляда. Источник закреплён за камерой, поэтому при повороте
    // модели блик едет по поверхности — именно это и читается как металл, а не
    // как полупрозрачная оболочка. Тела рисуются внутри своих поворотов, и для
    // них передаётся поворот локальной системы, иначе блик считался бы по
    // чужим осям.
    QVector3D viewDirectionModelSpace() const;
    QColor metalShade(const QColor &base,
                      const QVector3D &normal,
                      const QMatrix4x4 &frame_rotation = QMatrix4x4()) const;
    void setMetalColor(const QColor &base,
                       const QVector3D &normal,
                       double alpha,
                       const QMatrix4x4 &frame_rotation = QMatrix4x4()) const;
    void drawBox(double min_x,
                 double max_x,
                 double min_y,
                 double max_y,
                 double min_z,
                 double max_z,
                 const QColor &color,
                 double alpha,
                 const QMatrix4x4 &frame_rotation = QMatrix4x4()) const;
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
                       const QColor &edge_color,
                       const QMatrix4x4 &frame_rotation = QMatrix4x4()) const;
    void drawFieldSlice() const;
    // Одна плоскость выборки, показанная точками, а не заливкой: из таких
    // плоскостей складывается объёмное облако. plane_spacing_mm — расстояние до
    // соседней плоскости стопки, в его пределах точке разрешено разбегаться
    // вдоль нормали, чтобы решётка выборки не читалась нарезкой.
    void drawSliceCloud(const FieldSlice &slice,
                        double maximum_value,
                        double alpha,
                        double plane_spacing_mm) const;
    // Клетки одного среза; maximum_value — общий масштаб цвета (у стопки он
    // один на все плоскости, чтобы цвета срезов были сравнимы между собой).
    void drawSliceCells(const FieldSlice &slice,
                        double maximum_value,
                        double alpha) const;
    void drawVolumeSlices() const;
    void drawArrow(const FieldGlyph &glyph) const;
    // Стрелка, ствол которой — кусок самой силовой линии. Прямой отрезок на
    // изогнутом поле срезает угол и повисает рядом с линией, тем заметнее, чем
    // круче изгиб; здесь ствол повторяет траекторию, а наконечник встаёт на её
    // конец и разворачивается вместе с фазой, не сходя с линии.
    void drawCurvedArrow(const FieldGlyph &glyph) const;
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
    // Маленький триэдр ориентации в правом нижнем углу вида.
    void drawOrientationGizmo() const;
    // Цвет очистки экрана по выбранному фону; требует активного контекста.
    void applyClearColor();
    // Координатная сетка на «полу» сцены: даёт масштаб и ощущение глубины.
    void drawGrid() const;
    // Шаг сетки, выбранный по размеру модели из ряда 1-2-5 мм.
    double gridStepMm() const;
    // Цвет светлой линии, пригодный для текущего фона: на белом почти белые
    // рёбра корпуса были бы не видны.
    QColor themedLineColor(const QColor &color_for_dark) const;
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
    ViewBackground background_ = ViewBackground::Dark;
    bool grid_visible_ = true;
    std::function<void()> view_settings_changed_callback_;
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
