#include "slot_excitation_estimator.h"

#include "em/derived_fields.h"

#include <algorithm>
#include <cmath>

namespace postprocessing
{
namespace
{
struct WallFrame
{
    em::Vec3 normal_from_metal_to_field;
    em::Vec3 u_axis;
    double half_u_m = 0.0;
};

WallFrame wallFrame(em::WallSurface wall,
                    const em::RectangularWaveguideGeometry &geometry)
{
    switch (wall) {
    case em::WallSurface::Top:
        return {{0.0, -1.0, 0.0}, {1.0, 0.0, 0.0}, 0.5 * geometry.inner_width_m};
    case em::WallSurface::Right:
        return {{-1.0, 0.0, 0.0}, {0.0, 1.0, 0.0}, 0.5 * geometry.inner_height_m};
    case em::WallSurface::Bottom:
        return {{0.0, 1.0, 0.0}, {1.0, 0.0, 0.0}, 0.5 * geometry.inner_width_m};
    case em::WallSurface::Left:
        return {{1.0, 0.0, 0.0}, {0.0, 1.0, 0.0}, 0.5 * geometry.inner_height_m};
    }
    return {};
}

em::Vec3 wallPoint(em::WallSurface wall,
                   const em::RectangularWaveguideGeometry &geometry,
                   double u_m,
                   double z_m)
{
    switch (wall) {
    case em::WallSurface::Top:
        return {u_m, 0.5 * geometry.inner_height_m, z_m};
    case em::WallSurface::Right:
        return {0.5 * geometry.inner_width_m, u_m, z_m};
    case em::WallSurface::Bottom:
        return {u_m, -0.5 * geometry.inner_height_m, z_m};
    case em::WallSurface::Left:
        return {-0.5 * geometry.inner_width_m, u_m, z_m};
    }
    return {};
}

em::ComplexVec3 currentAt(const em::FieldSolution &solution,
                          const em::SlotGeometry &slot,
                          const WallFrame &frame,
                          double u_m,
                          double z_m)
{
    const em::RectangularWaveguideGeometry &geometry = solution.request.model.waveguide;
    const double offset_m = std::max(1.0e-9,
                                     std::min(geometry.inner_width_m,
                                              geometry.inner_height_m) *
                                         1.0e-6);
    const em::Vec3 sample_point_m = wallPoint(slot.wall, geometry, u_m, z_m) +
                                    frame.normal_from_metal_to_field * offset_m;
    const em::FieldPhasor field = solution.field->evaluate(sample_point_m);
    return em::surfaceCurrent(frame.normal_from_metal_to_field,
                              field.magnetic_a_per_m);
}

double normalizedSinc(double value)
{
    return std::abs(value) < 1.0e-12 ? 1.0 : std::sin(value) / value;
}
}

double SlotExcitationEstimator::normalizedCoupling(const em::FieldSolution &solution,
                                                   const em::SlotGeometry &slot,
                                                   const std::function<bool()> &cancellation_requested) const
{
    if (!slot.enabled || !solution.success || !solution.has_selected_mode || !solution.field ||
        (cancellation_requested && cancellation_requested())) {
        return 0.0;
    }

    const em::RectangularWaveguideGeometry &geometry = solution.request.model.waveguide;
    const WallFrame frame = wallFrame(slot.wall, geometry);
    const double half_length_m = 0.5 * geometry.length_m;
    double maximum_current_a_per_m = 0.0;
    for (int u_index = 0; u_index < 81; ++u_index) {
        if (cancellation_requested && cancellation_requested()) {
            return 0.0;
        }
        const double u_m = -frame.half_u_m +
                           2.0 * frame.half_u_m * u_index / 80.0;
        for (int z_index = 0; z_index < 17; ++z_index) {
            const double z_m = -half_length_m +
                               2.0 * half_length_m * z_index / 16.0;
            maximum_current_a_per_m = std::max(
                maximum_current_a_per_m,
                em::magnitude(currentAt(solution, slot, frame, u_m, z_m)));
        }
    }
    if (maximum_current_a_per_m <= 1.0e-18) {
        return 0.0;
    }
    if (cancellation_requested && cancellation_requested()) {
        return 0.0;
    }

    const em::ComplexVec3 slot_current = currentAt(solution,
                                                   slot,
                                                   frame,
                                                   slot.center_u_m,
                                                   slot.center_z_m);
    const em::Vec3 z_axis{0.0, 0.0, 1.0};
    const em::Vec3 width_axis = frame.u_axis * std::cos(slot.rotation_rad) -
                                z_axis * std::sin(slot.rotation_rad);
    const em::Complex crossing_current = slot_current.x * width_axis.x +
                                         slot_current.y * width_axis.y +
                                         slot_current.z * width_axis.z;
    const double beta_per_m = std::imag(
        solution.selected_mode.propagation_constant_per_m);
    const double length_factor = std::abs(normalizedSinc(0.5 * beta_per_m * slot.length_m));
    return std::clamp(std::abs(crossing_current) / maximum_current_a_per_m * length_factor,
                      0.0,
                      1.0);
}
}
