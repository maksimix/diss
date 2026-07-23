#include "qt_field_glyph_adapter.h"

#include <QtGui/QVector3D>

#include <algorithm>
#include <complex>

namespace
{
constexpr double meters_to_millimeters = 1.0e3;

QVector3D toQVector(const em::Vec3 &point_m)
{
    return QVector3D(static_cast<float>(point_m.x * meters_to_millimeters),
                     static_cast<float>(point_m.y * meters_to_millimeters),
                     static_cast<float>(point_m.z * meters_to_millimeters));
}

// Field phasors are physical quantities (V/m), not geometry — they must not be
// rescaled by the metre-to-millimetre factor used for positions.
QVector3D phasorReal(const em::ComplexVec3 &vector)
{
    return QVector3D(static_cast<float>(std::real(vector.x)),
                     static_cast<float>(std::real(vector.y)),
                     static_cast<float>(std::real(vector.z)));
}

QVector3D phasorImag(const em::ComplexVec3 &vector)
{
    return QVector3D(static_cast<float>(std::imag(vector.x)),
                     static_cast<float>(std::imag(vector.y)),
                     static_cast<float>(std::imag(vector.z)));
}

QColor quantityColor(postprocessing::FieldQuantity quantity)
{
    switch (quantity) {
    case postprocessing::FieldQuantity::Electric:
        return QColor(72, 148, 255);
    case postprocessing::FieldQuantity::Magnetic:
        return QColor(244, 72, 78);
    case postprocessing::FieldQuantity::SurfaceCurrent:
        return QColor(76, 255, 174);
    case postprocessing::FieldQuantity::Poynting:
        return QColor(255, 205, 72);
    }
    return QColor(230, 230, 230);
}

FieldGlyphType glyphType(const postprocessing::VisualizationPrimitive &primitive)
{
    switch (primitive.quantity) {
    case postprocessing::FieldQuantity::Electric:
        return primitive.kind == postprocessing::PrimitiveKind::Arrow
                   ? FieldGlyphType::ElectricArrow
                   : FieldGlyphType::ElectricLine;
    case postprocessing::FieldQuantity::Magnetic:
        return primitive.kind == postprocessing::PrimitiveKind::Arrow
                   ? FieldGlyphType::MagneticArrow
                   : FieldGlyphType::MagneticLine;
    case postprocessing::FieldQuantity::SurfaceCurrent:
        return primitive.kind == postprocessing::PrimitiveKind::Arrow
                   ? FieldGlyphType::SurfaceCurrentArrow
                   : FieldGlyphType::SurfaceCurrentLine;
    case postprocessing::FieldQuantity::Poynting:
        return FieldGlyphType::PoyntingArrow;
    }
    return FieldGlyphType::ElectricLine;
}

QVector3D arrowSideHint(const QVector3D &start, const QVector3D &end)
{
    QVector3D direction = end - start;
    if (direction.lengthSquared() < 1.0e-10f) {
        return QVector3D(1.0f, 0.0f, 0.0f);
    }
    direction.normalize();
    QVector3D reference = std::abs(QVector3D::dotProduct(direction,
                                                         QVector3D(0.0f, 1.0f, 0.0f))) <
                                  0.86f
                              ? QVector3D(0.0f, 1.0f, 0.0f)
                              : QVector3D(1.0f, 0.0f, 0.0f);
    QVector3D side = QVector3D::crossProduct(direction, reference);
    if (side.lengthSquared() < 1.0e-10f) {
        side = QVector3D(0.0f, 0.0f, 1.0f);
    }
    side.normalize();
    return side;
}
}

QVector<FieldGlyph> QtFieldGlyphAdapter::build(
    const em::FieldSolution &solution,
    const postprocessing::FieldVisualizationSettings &settings,
    const postprocessing::GenerationControl &control) const
{
    const std::vector<postprocessing::VisualizationPrimitive> primitives =
        postprocessing::FieldVisualizationGenerator().generate(solution, settings, control);
    QVector<FieldGlyph> glyphs;
    glyphs.reserve(static_cast<qsizetype>(primitives.size()));

    for (const postprocessing::VisualizationPrimitive &primitive : primitives) {
        if (control.isCancellationRequested()) {
            return {};
        }
        if (primitive.points_m.size() < 2) {
            continue;
        }
        FieldGlyph glyph;
        glyph.type = glyphType(primitive);
        glyph.color = quantityColor(primitive.quantity);
        glyph.magnitude = std::clamp(primitive.normalized_magnitude, 0.0, 1.0);
        glyph.points.reserve(static_cast<qsizetype>(primitive.points_m.size() + 1));
        for (const em::Vec3 &point_m : primitive.points_m) {
            glyph.points.push_back(toQVector(point_m));
        }
        if (primitive.animated && primitive.reference_magnitude > 0.0) {
            glyph.animated = true;
            glyph.anchor = toQVector(primitive.anchor_m);
            glyph.phasor_real = phasorReal(primitive.phasor);
            glyph.phasor_imag = phasorImag(primitive.phasor);
            glyph.reference_magnitude = primitive.reference_magnitude;
            glyph.animation_length_mm = primitive.arrow_length_m * meters_to_millimeters;
        }
        // An animated line carries a phase per vertex instead of a single
        // phasor, so it has no reference magnitude to qualify it above.
        if (primitive.animated &&
            primitive.vertex_phase_rad.size() == primitive.points_m.size() &&
            primitive.vertex_amplitude.size() == primitive.points_m.size()) {
            glyph.animated = true;
            glyph.vertex_phase_rad.reserve(
                static_cast<qsizetype>(primitive.vertex_phase_rad.size()));
            glyph.vertex_amplitude.reserve(
                static_cast<qsizetype>(primitive.vertex_amplitude.size()));
            for (std::size_t index = 0; index < primitive.vertex_phase_rad.size(); ++index) {
                glyph.vertex_phase_rad.push_back(
                    static_cast<float>(primitive.vertex_phase_rad[index]));
                glyph.vertex_amplitude.push_back(
                    static_cast<float>(primitive.vertex_amplitude[index]));
            }
        }

        if (primitive.kind == postprocessing::PrimitiveKind::Arrow && glyph.points.size() >= 2) {
            const QVector3D start = glyph.points[0];
            const QVector3D end = glyph.points[1];
            const double arrow_length = (end - start).length();
            glyph.points.push_back(start + arrowSideHint(start, end) *
                                               static_cast<float>(0.28 * arrow_length));
        }
        glyphs.push_back(std::move(glyph));
    }

    return glyphs;
}

FieldSlice QtFieldGlyphAdapter::buildSlice(
    const em::FieldSolution &solution,
    FieldSlicePlane plane,
    double offset_fraction,
    const postprocessing::GenerationControl &control,
    double resolution_scale) const
{
    const postprocessing::SlicePlaneKind kind =
        plane == FieldSlicePlane::HorizontalXZ
            ? postprocessing::SlicePlaneKind::HorizontalXZ
            : postprocessing::SlicePlaneKind::VerticalYZ;
    const postprocessing::FieldSliceData data =
        postprocessing::FieldVisualizationGenerator().generateSlice(solution,
                                                                    kind,
                                                                    offset_fraction,
                                                                    resolution_scale,
                                                                    control);

    FieldSlice slice;
    slice.plane = plane;
    slice.valid = data.valid;
    slice.maximum_value = data.maximum_value;
    if (!data.valid || control.isCancellationRequested()) {
        return slice;
    }
    slice.cells.reserve(static_cast<qsizetype>(data.cells.size()));
    for (const postprocessing::SliceSampleCell &cell : data.cells) {
        FieldSliceCell converted;
        converted.center = toQVector(cell.center_m);
        converted.u_half = toQVector(cell.u_half_m);
        converted.v_half = toQVector(cell.v_half_m);
        converted.envelope = cell.envelope;
        converted.phasor_real = phasorReal(cell.phasor);
        converted.phasor_imag = phasorImag(cell.phasor);
        slice.cells.push_back(converted);
    }
    return slice;
}

QVector<FieldSlice> QtFieldGlyphAdapter::buildVolumeSlices(
    const em::FieldSolution &solution,
    FieldSlicePlane plane,
    int slice_count,
    const postprocessing::GenerationControl &control) const
{
    // Стопка полупрозрачных срезов вместо честного объёмного рендера: каждая
    // плоскость грубее одиночного среза, иначе десяток плоскостей стоил бы
    // десятикратной выборки поля и десятикратной памяти.
    constexpr double volume_resolution_scale = 0.55;
    const int bounded_count = std::clamp(slice_count, 3, 25);
    QVector<FieldSlice> slices;
    slices.reserve(bounded_count);
    for (int index = 0; index < bounded_count; ++index) {
        if (control.isCancellationRequested()) {
            return {};
        }
        // Равномерно по поперечнику, крайние плоскости — на полшага от стенок.
        const double offset_fraction =
            -0.5 + (index + 0.5) / static_cast<double>(bounded_count);
        FieldSlice slice = buildSlice(solution,
                                      plane,
                                      offset_fraction,
                                      control,
                                      volume_resolution_scale);
        if (!slice.valid) {
            continue;
        }
        slices.push_back(std::move(slice));
    }
    if (control.isCancellationRequested()) {
        return {};
    }
    return slices;
}
