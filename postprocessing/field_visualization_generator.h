#pragma once

#include "em/em_solution.h"

#include <functional>
#include <vector>

namespace postprocessing
{
enum class FieldQuantity
{
    Electric,
    Magnetic,
    SurfaceCurrent,
    Poynting
};

enum class PrimitiveKind
{
    Polyline,
    Arrow
};

struct VisualizationPrimitive
{
    FieldQuantity quantity = FieldQuantity::Electric;
    PrimitiveKind kind = PrimitiveKind::Polyline;
    std::vector<em::Vec3> points_m;
    double normalized_magnitude = 0.0;

    // Optional payload for an oscillating vector arrow. When animated is true
    // the renderer may recompute Re(phasor * e^{j*phase}) each frame; points_m
    // still holds the static snapshot at the generation phase.
    bool animated = false;
    em::Vec3 anchor_m;
    em::ComplexVec3 phasor;          // complex field vector at the anchor
    double reference_magnitude = 0.0;  // global max |F| for this quantity
    double arrow_length_m = 0.0;       // full arrow length at |instantaneous| == reference

    // Optional payload for an animated field line, one entry per point of
    // points_m. A traced line cannot be retraced every frame, but the field
    // along it still oscillates: the line is tangent to the field by
    // construction, so the complex component along the tangent carries the
    // whole story. Its phase says when that piece of the line peaks and its
    // magnitude how strongly, so a bright band travels along the line instead
    // of the picture standing still.
    std::vector<double> vertex_phase_rad;
    std::vector<double> vertex_amplitude;   // |component| / max along this line
};

enum class SlicePlaneKind
{
    HorizontalXZ,   // normal along y
    VerticalYZ      // normal along x
};

struct SliceSampleCell
{
    em::Vec3 center_m;
    em::Vec3 u_half_m;
    em::Vec3 v_half_m;
    double envelope = 0.0;
    em::ComplexVec3 phasor;   // complex field vector (electric) at the cell centre
};

struct FieldSliceData
{
    bool valid = false;
    SlicePlaneKind plane = SlicePlaneKind::HorizontalXZ;
    FieldQuantity quantity = FieldQuantity::Electric;
    std::vector<SliceSampleCell> cells;
    double maximum_value = 0.0;
};

struct FieldVisualizationSettings
{
    double phase_rad = em::pi / 4.0;
    bool generate_electric = true;
    bool generate_magnetic = true;
    bool generate_surface_current = true;
    bool generate_poynting = true;
    // Концентрация стрелок E, H и J (потока TE10, пристеночных, объёмных и
    // токов на стенках и пластинах): во сколько раз больше или меньше стрелок
    // строится по сравнению с обычной плотностью. Значение зажимается в
    // [0.25, 4.0]; длина стрелки уменьшается во столько же раз, во сколько
    // сгущается её сетка посева, иначе соседние стрелки налезают друг на
    // друга. Линии поля и стрелки Пойнтинга не масштабируются.
    double arrow_density = 1.0;
};

struct GenerationControl
{
    std::function<bool()> cancellation_requested;

    bool isCancellationRequested() const
    {
        return cancellation_requested && cancellation_requested();
    }
};

class FieldVisualizationGenerator
{
public:
    std::vector<VisualizationPrimitive> generate(
        const em::FieldSolution &solution,
        const FieldVisualizationSettings &settings = {},
        const GenerationControl &control = {}) const;

    // Samples |E| on a cut plane through the guide and returns a filled grid of
    // cells with the complex field retained for phase animation.
    //
    // offset_fraction сдвигает плоскость вдоль её нормали: доля поперечного
    // размера (высоты для горизонтального среза, ширины для вертикального) в
    // пределах [-0.5, 0.5]; края зажимаются чуть внутрь, чтобы срез не лёг на
    // стенку. resolution_scale < 1 даёт более грубую сетку — так стопка срезов
    // объёмной заливки не раздувает память и время выборки.
    FieldSliceData generateSlice(
        const em::FieldSolution &solution,
        SlicePlaneKind plane = SlicePlaneKind::HorizontalXZ,
        double offset_fraction = 0.0,
        double resolution_scale = 1.0,
        const GenerationControl &control = {}) const;
};
}
