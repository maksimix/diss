#pragma once

#include "em/em_solution.h"

#include <QtCore/QMetaType>
#include <QtCore/QString>
#include <QtCore/QVector>
#include <QtGui/QColor>
#include <QtGui/QVector3D>

#include <algorithm>
#include <cmath>
#include <memory>

struct PecPlateParameters
{
    QString name = QStringLiteral("plate_1");
    bool enabled = true;
    double x_min_mm = -5.0;
    double x_max_mm = 5.0;
    double y_min_mm = -4.0;
    double y_max_mm = 4.0;
    double z_min_mm = -0.25;
    double z_max_mm = 0.25;
    double rotation_x_deg = 0.0;
    double rotation_y_deg = 0.0;
    double rotation_z_deg = 0.0;
    // Окно насквозь через пластину (диафрагма/ирис).
    bool aperture_enabled = false;
    int aperture_shape = 0;             // 0 — прямоугольное, 1 — круглое
    double aperture_width_mm = 10.0;
    double aperture_height_mm = 4.0;
    double aperture_radius_mm = 3.0;
    double aperture_offset_x_mm = 0.0;
    double aperture_offset_y_mm = 0.0;
    // Прямоугольный язычок в плоскости пластины: снизу по центральной линии
    // окна, толщина по z равна толщине пластины.
    bool post_enabled = false;
    double post_width_mm = 2.0;
    double post_height_mm = 5.0;
};

struct WaveguideParameters
{
    // Форма сечения: 0 — прямоугольное, 1 — круглое. У круглого используется
    // radius_mm, у прямоугольного — width_mm и depth_mm.
    int cross_section = 0;
    double radius_mm = 10.0;
    double width_mm = 22.86;
    double length_mm = 50.0;
    double depth_mm = 10.16;
    double wall_thickness_mm = 0.1;
    double wall_conductivity_s_per_m = 0.0;   // 0 => идеальный проводник (без потерь)
    // Уровень качества FEM-расчёта: 0 — быстро, 1 — обычное, 2 — высокое,
    // 3 — максимальное. Влияет на шаг сетки, порядок элементов и бюджет решателя.
    int accuracy_level = 1;
    // Метод расчёта: 0 — автоматически, 1 — аналитический (пустой волновод),
    // 2 — поперечные сечения, 3 — частичные области, 4 — метод конечных элементов.
    int solver_method = 0;
    // Линейный решатель FEM: 0 — автоматически, 1 — прямой (разложение),
    // 2 — итерационный (GMRES). На измельчённой сетке итерационный не сходится,
    // поэтому автоматический выбор берёт прямой, пока хватает памяти.
    int linear_solver_method = 0;
    double frequency_ghz = 10.0;
    bool slot_enabled = false;
    double slot_length_mm = 12.0;
    double slot_width_mm = 1.0;
    double slot_offset_x_mm = 0.0;
    double slot_offset_z_mm = 0.0;
    double slot_rotation_deg = 0.0;
    int slot_surface = 0; // 0 top, 1 right, 2 bottom, 3 left
    QVector<PecPlateParameters> pec_plates;
};

enum class FieldGlyphType
{
    ElectricArrow,
    ElectricLine,
    MagneticArrow,
    MagneticLine,
    SurfaceCurrentArrow,
    SurfaceCurrentLine,
    PoyntingArrow
};

struct FieldGlyph
{
    FieldGlyphType type = FieldGlyphType::ElectricArrow;
    QVector<QVector3D> points;
    QColor color;
    double magnitude = 0.0;

    // Optional payload for arrows that represent an oscillating vector phasor.
    // When animated is true the widget recomputes the instantaneous vector
    // Re(F * e^{j*phase}) every frame instead of using the static geometry.
    bool animated = false;
    QVector3D anchor;             // mm — arrow midpoint
    QVector3D phasor_real;        // Re(F), field units
    QVector3D phasor_imag;        // Im(F), field units
    double reference_magnitude = 0.0;  // max |F| used for normalization
    double animation_length_mm = 0.0;  // full arrow length at |instantaneous| == reference
};

enum class FieldSlicePlane
{
    HorizontalXZ,   // y = const, spans x and z (H-plane cut)
    VerticalYZ      // x = const, spans y and z (E-plane cut)
};

struct FieldSliceCell
{
    QVector3D center;        // mm
    QVector3D u_half;        // mm, half-extent along the first in-plane axis
    QVector3D v_half;        // mm, half-extent along the second in-plane axis
    double envelope = 0.0;   // |F| amplitude, field units
    QVector3D phasor_real;   // Re(F vector) for instantaneous animation
    QVector3D phasor_imag;   // Im(F vector)
};

struct FieldSlice
{
    bool valid = false;
    FieldSlicePlane plane = FieldSlicePlane::HorizontalXZ;
    QVector<FieldSliceCell> cells;
    double maximum_value = 0.0;   // max envelope over the plane (legend scale)
    QString value_label = QStringLiteral("|E|, В/м");
};

// Perceptual heat colormap shared by the slice renderer and the legend widget.
// t in [0, 1] maps dark blue -> cyan -> green -> yellow -> red -> near white.
inline QColor fieldHeatColor(double t)
{
    t = std::clamp(t, 0.0, 1.0);
    struct Stop { double t, r, g, b; };
    static const Stop stops[] = {
        {0.00, 0.03, 0.05, 0.18},
        {0.20, 0.09, 0.34, 0.74},
        {0.40, 0.10, 0.72, 0.72},
        {0.60, 0.30, 0.82, 0.24},
        {0.78, 0.96, 0.86, 0.15},
        {0.92, 0.95, 0.34, 0.12},
        {1.00, 1.00, 0.95, 0.90},
    };
    constexpr int count = static_cast<int>(sizeof(stops) / sizeof(stops[0]));
    for (int index = 1; index < count; ++index) {
        if (t <= stops[index].t) {
            const Stop &low = stops[index - 1];
            const Stop &high = stops[index];
            const double span = std::max(1.0e-9, high.t - low.t);
            const double ratio = (t - low.t) / span;
            return QColor::fromRgbF(low.r + (high.r - low.r) * ratio,
                                    low.g + (high.g - low.g) * ratio,
                                    low.b + (high.b - low.b) * ratio);
        }
    }
    return QColor::fromRgbF(stops[count - 1].r, stops[count - 1].g, stops[count - 1].b);
}

// Nonlinear compression that lifts weak regions (e.g. the shadow behind a
// plate) so they remain visible next to the bright incident field.
inline double fieldHeatNormalize(double value, double maximum, double lift = 40.0)
{
    if (!(maximum > 0.0) || !(value > 0.0)) {
        return 0.0;
    }
    const double ratio = std::clamp(value / maximum, 0.0, 1.0);
    return std::log10(1.0 + lift * ratio) / std::log10(1.0 + lift);
}

struct WaveguideMode
{
    QString name;
    bool transverse_electric = true;
    int m = 0;
    int n = 0;
    double cutoff_ghz = 0.0;
    bool propagates = false;
};

struct WaveguideCalculationResult
{
    WaveguideParameters parameters;
    bool valid = false;
    bool cancelled = false;
    QString error_message;

    double inner_width_mm = 0.0;
    double inner_depth_mm = 0.0;
    double area_mm2 = 0.0;
    double cavity_volume_mm3 = 0.0;
    double metal_volume_mm3 = 0.0;

    QVector<WaveguideMode> modes;
    WaveguideMode selected_mode;
    bool has_propagating_mode = false;
    double wavelength0_mm = 0.0;
    double guide_wavelength_mm = 0.0;
    double beta_rad_per_m = 0.0;
    double attenuation_np_per_m = 0.0;
    double conductor_attenuation_np_per_m = 0.0;
    double stored_electric_energy_j = 0.0;
    double stored_magnetic_energy_j = 0.0;
    double quality_factor = 0.0;
    double slot_normalized_coupling = 0.0;
    double incident_power_w = 0.0;
    double reflected_power_w = 0.0;
    double transmitted_power_w = 0.0;
    double dissipated_power_w = 0.0;
    double input_power_w = 0.0;
    double output_power_w = 0.0;
    double s11_magnitude = 0.0;
    double s21_magnitude = 0.0;
    double power_balance_relative_error = 0.0;
    QString solver_backend;
    // Диагностика FEM: заполняется и при неудачном расчёте, чтобы по сообщению
    // об ошибке было видно, на какой сетке он шёл.
    int mesh_tetrahedron_count = 0;
    int fem_unknown_count = 0;
    int linear_iterations = 0;
    double linear_relative_residual = 0.0;
    QVector<QString> solver_warnings;

    QVector<FieldGlyph> field_glyphs;
    FieldSlice horizontal_slice;   // |E| on the y = 0 H-plane cut
    FieldSlice vertical_slice;     // |E| on the x = 0 E-plane cut
    std::shared_ptr<const em::FieldSolution> field_solution;
};

Q_DECLARE_METATYPE(WaveguideParameters)
Q_DECLARE_METATYPE(WaveguideCalculationResult)
