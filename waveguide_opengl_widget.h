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

class WaveguideOpenGLWidget : public QOpenGLWidget, protected QOpenGLFunctions
{
public:
    explicit WaveguideOpenGLWidget(QWidget *parent = nullptr);

    void setCalculationResult(const WaveguideCalculationResult &result);
    void setModelPreview(const WaveguideParameters &parameters);
    void setSelectedPlateIndex(int plate_index);
    void setFieldDisplayMode(FieldDisplayMode mode);
    void setViewPreset(WaveguideViewPreset preset);
    void setSliceVisible(bool visible);
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
    void drawSlot() const;
    void drawPecPlates() const;
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
    void drawFieldSlice() const;
    void drawArrow(const FieldGlyph &glyph) const;
    void drawArrowHead(const QVector3D &position,
                       const QVector3D &direction,
                       const QVector3D &side_hint,
                       const QColor &color,
                       double size,
                       double alpha = 0.96) const;
    void drawPolylineWithArrow(const QVector<QVector3D> &points,
                               const QColor &color,
                               double arrow_size,
                               int arrow_count,
                               float line_width) const;
    void drawAxes() const;
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
    std::function<void(const WaveguideParameters &)> slot_edited_callback_;
    FieldDisplayMode field_display_mode_ = FieldDisplayMode::Fields;
    WaveguideViewPreset view_preset_ = WaveguideViewPreset::Free3D;
    FieldSlicePlane slice_plane_ = FieldSlicePlane::HorizontalXZ;
    bool show_slice_ = false;
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
};
