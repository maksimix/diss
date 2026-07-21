#include "field_visualization_generator.h"

#include "em/derived_fields.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <functional>
#include <thread>
#include <vector>

namespace postprocessing
{
namespace
{
constexpr double vector_tolerance = 1.0e-18;
constexpr int maximum_workers = 32;

// Returns the worker count used by parallelFor for a given workload size.
int workerCount(int item_count)
{
    if (item_count < 512) {
        return 1;
    }
    const unsigned int hardware = std::thread::hardware_concurrency();
    const int available = hardware > 0 ? static_cast<int>(hardware) : 1;
    return std::clamp(available, 1, maximum_workers);
}

// Strided parallel loop. The body receives (item index, worker index) so it can
// accumulate into per-worker slots without locking. When enabled is false the
// loop runs serially, which is required for field evaluators that are not
// safe to call concurrently.
template <typename Body>
void parallelFor(bool enabled, int item_count, const Body &body)
{
    const int workers = enabled ? workerCount(item_count) : 1;
    if (workers <= 1) {
        for (int index = 0; index < item_count; ++index) {
            body(index, 0);
        }
        return;
    }

    std::vector<std::thread> threads;
    threads.reserve(static_cast<std::size_t>(workers));
    for (int worker = 0; worker < workers; ++worker) {
        threads.emplace_back([&body, item_count, workers, worker]() {
            for (int index = worker; index < item_count; index += workers) {
                body(index, worker);
            }
        });
    }
    for (std::thread &thread : threads) {
        thread.join();
    }
}

struct VectorSample
{
    em::Vec3 vector;
    double magnitude = 0.0;
};

struct WallDefinition
{
    em::WallSurface wall = em::WallSurface::Top;
    em::Vec3 normal_from_metal_to_field;
    double half_u_m = 0.0;
};

struct SurfaceState
{
    double u_m = 0.0;
    double z_m = 0.0;
};

em::Vec3 addScaled(const em::Vec3 &position,
                   const em::Vec3 &direction,
                   double scale)
{
    return position + direction * scale;
}

bool insideVolume(const em::WaveguideGeometry &geometry,
                  const em::Vec3 &position_m)
{
    const double tolerance_m = 1.0e-10;
    // Сечение может быть и круглым: проверка формы вынесена в модель, поэтому
    // обрезка линий поля по стенке одинаково работает для обоих случаев.
    return em::insideCrossSection(geometry, position_m.x, position_m.y, tolerance_m) &&
           position_m.z >= -0.5 * geometry.length_m - tolerance_m &&
           position_m.z <= 0.5 * geometry.length_m + tolerance_m;
}

em::Vec3 lastInsidePoint(const em::WaveguideGeometry &geometry,
                         const em::Vec3 &inside_point_m,
                         const em::Vec3 &outside_point_m)
{
    double inside_ratio = 0.0;
    double outside_ratio = 1.0;
    for (int iteration = 0; iteration < 28; ++iteration) {
        const double middle_ratio = 0.5 * (inside_ratio + outside_ratio);
        const em::Vec3 middle_point =
            inside_point_m + (outside_point_m - inside_point_m) * middle_ratio;
        if (insideVolume(geometry, middle_point)) {
            inside_ratio = middle_ratio;
        } else {
            outside_ratio = middle_ratio;
        }
    }
    return inside_point_m + (outside_point_m - inside_point_m) * inside_ratio;
}

VectorSample volumeVectorAt(const em::FieldSolution &solution,
                            FieldQuantity quantity,
                            const em::Vec3 &position_m,
                            double phase_rad)
{
    if (!solution.field || !solution.field->contains(position_m)) {
        return {};
    }

    const em::FieldPhasor field = solution.field->evaluate(position_m);
    VectorSample sample;
    if (quantity == FieldQuantity::Electric) {
        sample.vector = em::instantaneousElectric(field, phase_rad);
    } else if (quantity == FieldQuantity::Magnetic) {
        sample.vector = em::instantaneousMagnetic(field, phase_rad);
    } else {
        sample.vector = em::timeAveragePoynting(field.electric_v_per_m,
                                                field.magnetic_a_per_m);
    }
    sample.magnitude = em::magnitude(sample.vector);
    return sample;
}

em::Vec3 unitVolumeDirection(const em::FieldSolution &solution,
                             FieldQuantity quantity,
                             const em::Vec3 &position_m,
                             double phase_rad,
                             double minimum_magnitude)
{
    const VectorSample sample = volumeVectorAt(solution, quantity, position_m, phase_rad);
    return sample.magnitude > minimum_magnitude
               ? sample.vector / sample.magnitude
               : em::Vec3{};
}

em::Vec3 advanceVolumeRk4(const em::FieldSolution &solution,
                          FieldQuantity quantity,
                          const em::Vec3 &position_m,
                          double phase_rad,
                          double signed_step_m,
                          double minimum_magnitude)
{
    const auto direction_at = [&](const em::Vec3 &point_m) {
        return unitVolumeDirection(solution,
                                   quantity,
                                   point_m,
                                   phase_rad,
                                   minimum_magnitude);
    };

    const em::Vec3 k1 = direction_at(position_m);
    if (em::magnitude(k1) < 0.5) {
        return position_m;
    }
    const em::Vec3 k2 = direction_at(addScaled(position_m, k1, 0.5 * signed_step_m));
    const em::Vec3 k3 = direction_at(addScaled(position_m, k2, 0.5 * signed_step_m));
    const em::Vec3 k4 = direction_at(addScaled(position_m, k3, signed_step_m));
    if (em::magnitude(k2) < 0.5 || em::magnitude(k3) < 0.5 || em::magnitude(k4) < 0.5) {
        return addScaled(position_m, k1, signed_step_m);
    }

    return position_m + (k1 + k2 * 2.0 + k3 * 2.0 + k4) * (signed_step_m / 6.0);
}

std::vector<em::Vec3> traceVolumeDirection(const em::FieldSolution &solution,
                                           FieldQuantity quantity,
                                           const em::Vec3 &seed_m,
                                           double phase_rad,
                                           double signed_step_m,
                                           double minimum_magnitude,
                                           int maximum_steps,
                                           const GenerationControl &control)
{
    const em::WaveguideGeometry &geometry = solution.request.model.waveguide;
    std::vector<em::Vec3> points;
    em::Vec3 position_m = seed_m;

    for (int step_index = 0; step_index < maximum_steps; ++step_index) {
        if (control.isCancellationRequested()) {
            return {};
        }
        const em::Vec3 next_position_m = advanceVolumeRk4(solution,
                                                          quantity,
                                                          position_m,
                                                          phase_rad,
                                                          signed_step_m,
                                                          minimum_magnitude);
        if (em::magnitude(next_position_m - position_m) < std::abs(signed_step_m) * 0.05) {
            break;
        }
        if (!insideVolume(geometry, next_position_m)) {
            points.push_back(lastInsidePoint(geometry, position_m, next_position_m));
            break;
        }

        points.push_back(next_position_m);
        position_m = next_position_m;
        if (step_index > 20 && em::magnitude(position_m - seed_m) < std::abs(signed_step_m) * 0.8) {
            points.push_back(seed_m);
            break;
        }
    }

    return points;
}

std::vector<em::Vec3> traceVolumeLine(const em::FieldSolution &solution,
                                      FieldQuantity quantity,
                                      const em::Vec3 &seed_m,
                                      double phase_rad,
                                      double step_m,
                                      double minimum_magnitude,
                                      const GenerationControl &control)
{
    std::vector<em::Vec3> backward = traceVolumeDirection(solution,
                                                          quantity,
                                                          seed_m,
                                                          phase_rad,
                                                          -step_m,
                                                          minimum_magnitude,
                                                          480,
                                                          control);
    if (control.isCancellationRequested()) {
        return {};
    }
    std::vector<em::Vec3> forward = traceVolumeDirection(solution,
                                                         quantity,
                                                         seed_m,
                                                         phase_rad,
                                                         step_m,
                                                         minimum_magnitude,
                                                         480,
                                                         control);
    if (control.isCancellationRequested()) {
        return {};
    }
    std::reverse(backward.begin(), backward.end());
    backward.push_back(seed_m);
    backward.insert(backward.end(), forward.begin(), forward.end());
    return backward;
}

double maximumVolumeMagnitude(const em::FieldSolution &solution,
                              FieldQuantity quantity,
                              double phase_rad,
                              const GenerationControl &control)
{
    const em::WaveguideGeometry &geometry = solution.request.model.waveguide;
    double maximum_magnitude = 0.0;
    for (int x_index = 0; x_index < 9; ++x_index) {
        if (control.isCancellationRequested()) {
            return 0.0;
        }
        const double x_m = -0.48 * geometry.inner_width_m +
                           x_index * 0.96 * geometry.inner_width_m / 8.0;
        for (int y_index = 0; y_index < 7; ++y_index) {
            const double y_m = -0.48 * geometry.inner_height_m +
                               y_index * 0.96 * geometry.inner_height_m / 6.0;
            for (int z_index = 0; z_index < 11; ++z_index) {
                const double z_m = -0.48 * geometry.length_m +
                                   z_index * 0.96 * geometry.length_m / 10.0;
                maximum_magnitude = std::max(maximum_magnitude,
                                             volumeVectorAt(solution,
                                                            quantity,
                                                            {x_m, y_m, z_m},
                                                            phase_rad)
                                                 .magnitude);
            }
        }
    }
    return maximum_magnitude;
}

bool acceptSeed(double normalized_magnitude, int first_index, int second_index)
{
    if (normalized_magnitude < 0.10) {
        return false;
    }
    if (normalized_magnitude < 0.36) {
        return (first_index + 2 * second_index) % 3 == 0;
    }
    if (normalized_magnitude < 0.68) {
        return (first_index + second_index) % 2 == 0;
    }
    return true;
}

bool isDominantTe10(const em::FieldSolution &solution)
{
    const em::ModeDescriptor &mode = solution.selected_mode;
    return mode.family == em::ModeFamily::TransverseElectric &&
           mode.m == 1 && mode.n == 0;
}

void appendTe10ElectricFluxArrows(std::vector<VisualizationPrimitive> &primitives,
                                  const em::FieldSolution &solution,
                                  double phase_rad,
                                  const GenerationControl &control)
{
    const em::WaveguideGeometry &geometry = solution.request.model.waveguide;
    double maximum_magnitude = 0.0;
    for (int z_index = 0; z_index < 11; ++z_index) {
        if (control.isCancellationRequested()) {
            return;
        }
        const double z_m = -0.48 * geometry.length_m +
                           z_index * 0.96 * geometry.length_m / 10.0;
        for (int x_index = 0; x_index < 33; ++x_index) {
            const double x_m = -0.48 * geometry.inner_width_m +
                               x_index * 0.96 * geometry.inner_width_m / 32.0;
            const em::FieldPhasor field = solution.field->evaluate({x_m, 0.0, z_m});
            maximum_magnitude = std::max(maximum_magnitude,
                                         std::abs(field.electric_v_per_m.y));
        }
    }
    if (maximum_magnitude <= vector_tolerance) {
        return;
    }

    constexpr int maximum_arrows_per_slice = 13;
    constexpr int z_count = 11;
    const double maximum_arrow_length_m = 0.94 * geometry.inner_height_m;
    const em::Complex phase = std::polar(1.0, phase_rad);

    for (int z_index = 0; z_index < z_count; ++z_index) {
        if (control.isCancellationRequested()) {
            return;
        }

        const double z_m = -0.45 * geometry.length_m +
                           z_index * 0.90 * geometry.length_m / (z_count - 1);
        const int arrow_count = maximum_arrows_per_slice;
        for (int arrow_index = 0; arrow_index < arrow_count; ++arrow_index) {
            // Equal flux intervals: density follows |sin(pi * x' / a)| for TE10.
            const double flux_quantile =
                (static_cast<double>(arrow_index) + 0.5) / arrow_count;
            const double transverse_phase_rad =
                std::acos(std::clamp(1.0 - 2.0 * flux_quantile, -1.0, 1.0));
            const double x_m = geometry.inner_width_m *
                               (transverse_phase_rad / em::pi - 0.5);
            const em::FieldPhasor field = solution.field->evaluate({x_m, 0.0, z_m});
            const em::Complex electric_y_at_phase = field.electric_v_per_m.y * phase;
            const double normalized_magnitude =
                std::abs(field.electric_v_per_m.y) / maximum_magnitude;
            if (normalized_magnitude < 0.025) {
                continue;
            }

            double signed_component = std::real(electric_y_at_phase);
            if (std::abs(signed_component) < vector_tolerance) {
                signed_component = std::imag(electric_y_at_phase);
            }
            const em::Vec3 direction{0.0, signed_component >= 0.0 ? 1.0 : -1.0, 0.0};
            const double arrow_length_m = maximum_arrow_length_m * normalized_magnitude;
            const em::Vec3 center_m{x_m, 0.0, z_m};

            VisualizationPrimitive primitive;
            primitive.quantity = FieldQuantity::Electric;
            primitive.kind = PrimitiveKind::Arrow;
            primitive.points_m = {
                center_m - direction * (0.5 * arrow_length_m),
                center_m + direction * (0.5 * arrow_length_m),
            };
            primitive.normalized_magnitude = std::min(1.0, normalized_magnitude);
            primitive.animated = true;
            primitive.anchor_m = center_m;
            primitive.phasor = {0.0, field.electric_v_per_m.y, 0.0};
            primitive.reference_magnitude = maximum_magnitude;
            primitive.arrow_length_m = maximum_arrow_length_m;
            primitives.push_back(std::move(primitive));
        }
    }
}

void appendVolumeLines(std::vector<VisualizationPrimitive> &primitives,
                       const em::FieldSolution &solution,
                       FieldQuantity quantity,
                       double phase_rad,
                       const GenerationControl &control)
{
    const em::WaveguideGeometry &geometry = solution.request.model.waveguide;
    const double maximum_magnitude = maximumVolumeMagnitude(solution,
                                                             quantity,
                                                             phase_rad,
                                                             control);
    if (maximum_magnitude <= vector_tolerance) {
        return;
    }

    const double step_m = std::min({geometry.inner_width_m,
                                    geometry.inner_height_m,
                                    geometry.length_m}) /
                          82.0;
    const double minimum_magnitude = maximum_magnitude * 0.025;
    const int x_count = quantity == FieldQuantity::Electric ? 7 : 6;
    const int y_count = quantity == FieldQuantity::Electric ? 3 : 4;
    const int z_count = quantity == FieldQuantity::Electric ? 8 : 7;

    for (int z_index = 0; z_index < z_count; ++z_index) {
        if (control.isCancellationRequested()) {
            return;
        }
        const double z_m = -0.44 * geometry.length_m +
                           z_index * 0.88 * geometry.length_m /
                               std::max(1, z_count - 1);
        for (int x_index = 0; x_index < x_count; ++x_index) {
            const double x_m = -0.43 * geometry.inner_width_m +
                               x_index * 0.86 * geometry.inner_width_m /
                                   std::max(1, x_count - 1);
            for (int y_index = 0; y_index < y_count; ++y_index) {
                const double y_m = -0.40 * geometry.inner_height_m +
                                   y_index * 0.80 * geometry.inner_height_m /
                                       std::max(1, y_count - 1);
                const em::Vec3 seed_m{x_m, y_m, z_m};
                const double normalized_magnitude =
                    volumeVectorAt(solution, quantity, seed_m, phase_rad).magnitude /
                    maximum_magnitude;
                if (!acceptSeed(normalized_magnitude,
                                x_index + y_index * x_count,
                                z_index)) {
                    continue;
                }

                std::vector<em::Vec3> points_m = traceVolumeLine(solution,
                                                                 quantity,
                                                                 seed_m,
                                                                 phase_rad,
                                                                 step_m,
                                                                 minimum_magnitude,
                                                                 control);
                if (control.isCancellationRequested()) {
                    return;
                }
                if (points_m.size() < 5) {
                    continue;
                }

                VisualizationPrimitive primitive;
                primitive.quantity = quantity;
                primitive.kind = PrimitiveKind::Polyline;
                primitive.points_m = std::move(points_m);
                primitive.normalized_magnitude = std::min(1.0, normalized_magnitude);
                primitives.push_back(std::move(primitive));
            }
        }
    }
}

em::Vec3 surfacePoint(const WallDefinition &wall,
                      const em::WaveguideGeometry &geometry,
                      double u_m,
                      double z_m)
{
    switch (wall.wall) {
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

bool isInsideSlot(const em::FieldSolution &solution,
                  em::WallSurface wall,
                  double u_m,
                  double z_m)
{
    for (const em::SlotGeometry &slot : solution.request.model.slot_geometries) {
        if (!slot.enabled || slot.wall != wall) {
            continue;
        }
        const double du_m = u_m - slot.center_u_m;
        const double dz_m = z_m - slot.center_z_m;
        const double sin_angle = std::sin(slot.rotation_rad);
        const double cos_angle = std::cos(slot.rotation_rad);
        const double length_coordinate_m = du_m * sin_angle + dz_m * cos_angle;
        const double width_coordinate_m = du_m * cos_angle - dz_m * sin_angle;
        if (std::abs(length_coordinate_m) <= 0.5 * slot.length_m &&
            std::abs(width_coordinate_m) <= 0.5 * slot.width_m) {
            return true;
        }
    }
    return false;
}

VectorSample surfaceCurrentAt(const em::FieldSolution &solution,
                              const WallDefinition &wall,
                              double u_m,
                              double z_m,
                              double phase_rad)
{
    if (isInsideSlot(solution, wall.wall, u_m, z_m)) {
        return {};
    }

    const em::WaveguideGeometry &geometry = solution.request.model.waveguide;
    const double inside_offset_m =
        std::max(1.0e-9, std::min(geometry.inner_width_m, geometry.inner_height_m) * 1.0e-6);
    const em::Vec3 evaluation_point_m =
        surfacePoint(wall, geometry, u_m, z_m) +
        wall.normal_from_metal_to_field * inside_offset_m;
    const em::FieldPhasor field = solution.field->evaluate(evaluation_point_m);
    const em::ComplexVec3 current_phasor =
        em::surfaceCurrent(wall.normal_from_metal_to_field, field.magnetic_a_per_m);

    VectorSample sample;
    sample.vector = em::realAtPhase(current_phasor, phase_rad);
    const double normal_component = em::dot(sample.vector, wall.normal_from_metal_to_field);
    sample.vector = sample.vector - wall.normal_from_metal_to_field * normal_component;
    sample.magnitude = em::magnitude(sample.vector);
    return sample;
}

SurfaceState unitSurfaceDirection(const em::FieldSolution &solution,
                                  const WallDefinition &wall,
                                  const SurfaceState &state,
                                  double phase_rad,
                                  double minimum_magnitude)
{
    const VectorSample sample = surfaceCurrentAt(solution,
                                                 wall,
                                                 state.u_m,
                                                 state.z_m,
                                                 phase_rad);
    if (sample.magnitude <= minimum_magnitude) {
        return {};
    }
    if (wall.wall == em::WallSurface::Top || wall.wall == em::WallSurface::Bottom) {
        return {sample.vector.x / sample.magnitude, sample.vector.z / sample.magnitude};
    }
    return {sample.vector.y / sample.magnitude, sample.vector.z / sample.magnitude};
}

SurfaceState addScaled(const SurfaceState &state,
                       const SurfaceState &direction,
                       double scale)
{
    return {state.u_m + direction.u_m * scale,
            state.z_m + direction.z_m * scale};
}

SurfaceState advanceSurfaceRk4(const em::FieldSolution &solution,
                               const WallDefinition &wall,
                               const SurfaceState &state,
                               double phase_rad,
                               double signed_step_m,
                               double minimum_magnitude)
{
    const auto direction_at = [&](const SurfaceState &value) {
        return unitSurfaceDirection(solution,
                                    wall,
                                    value,
                                    phase_rad,
                                    minimum_magnitude);
    };
    const SurfaceState k1 = direction_at(state);
    const SurfaceState k2 = direction_at(addScaled(state, k1, 0.5 * signed_step_m));
    const SurfaceState k3 = direction_at(addScaled(state, k2, 0.5 * signed_step_m));
    const SurfaceState k4 = direction_at(addScaled(state, k3, signed_step_m));
    return {
        state.u_m + signed_step_m *
                          (k1.u_m + 2.0 * k2.u_m + 2.0 * k3.u_m + k4.u_m) / 6.0,
        state.z_m + signed_step_m *
                          (k1.z_m + 2.0 * k2.z_m + 2.0 * k3.z_m + k4.z_m) / 6.0,
    };
}

bool insideSurface(const em::FieldSolution &solution,
                   const WallDefinition &wall,
                   const SurfaceState &state)
{
    const double half_length_m = 0.5 * solution.request.model.waveguide.length_m;
    return std::abs(state.u_m) <= wall.half_u_m &&
           std::abs(state.z_m) <= half_length_m &&
           !isInsideSlot(solution, wall.wall, state.u_m, state.z_m);
}

std::vector<em::Vec3> traceSurfaceDirection(const em::FieldSolution &solution,
                                            const WallDefinition &wall,
                                            const SurfaceState &seed,
                                            double phase_rad,
                                            double signed_step_m,
                                            double minimum_magnitude,
                                            const GenerationControl &control)
{
    std::vector<em::Vec3> points_m;
    SurfaceState state = seed;
    const em::WaveguideGeometry &geometry = solution.request.model.waveguide;
    for (int step_index = 0; step_index < 420; ++step_index) {
        if (control.isCancellationRequested()) {
            return {};
        }
        const SurfaceState next_state = advanceSurfaceRk4(solution,
                                                          wall,
                                                          state,
                                                          phase_rad,
                                                          signed_step_m,
                                                          minimum_magnitude);
        const double state_delta_m = std::hypot(next_state.u_m - state.u_m,
                                                next_state.z_m - state.z_m);
        if (state_delta_m < std::abs(signed_step_m) * 0.05 ||
            !insideSurface(solution, wall, next_state)) {
            break;
        }
        state = next_state;
        points_m.push_back(surfacePoint(wall, geometry, state.u_m, state.z_m));
        if (step_index > 20 &&
            std::hypot(state.u_m - seed.u_m, state.z_m - seed.z_m) <
                std::abs(signed_step_m) * 0.8) {
            points_m.push_back(surfacePoint(wall, geometry, seed.u_m, seed.z_m));
            break;
        }
    }
    return points_m;
}

double maximumSurfaceCurrent(const em::FieldSolution &solution,
                             const WallDefinition &wall,
                             double phase_rad,
                             const GenerationControl &control)
{
    const double half_length_m = 0.5 * solution.request.model.waveguide.length_m;
    double maximum_magnitude = 0.0;
    for (int u_index = 0; u_index < 11; ++u_index) {
        if (control.isCancellationRequested()) {
            return 0.0;
        }
        const double u_m = -0.96 * wall.half_u_m +
                           u_index * 1.92 * wall.half_u_m / 10.0;
        for (int z_index = 0; z_index < 15; ++z_index) {
            const double z_m = -0.96 * half_length_m +
                               z_index * 1.92 * half_length_m / 14.0;
            maximum_magnitude = std::max(maximum_magnitude,
                                         surfaceCurrentAt(solution,
                                                          wall,
                                                          u_m,
                                                          z_m,
                                                          phase_rad)
                                             .magnitude);
        }
    }
    return maximum_magnitude;
}

struct PlateState
{
    double x_m = 0.0;
    double y_m = 0.0;
};

bool isAxisAlignedTransversePlate(const em::PecPlateGeometry &plate)
{
    constexpr double angle_tolerance_rad = 1.0e-10;
    return plate.enabled && plate.size_m.x > 0.0 && plate.size_m.y > 0.0 &&
           plate.size_m.z > 0.0 &&
           std::abs(plate.rotation_rad.x) <= angle_tolerance_rad &&
           std::abs(plate.rotation_rad.y) <= angle_tolerance_rad &&
           std::abs(plate.rotation_rad.z) <= angle_tolerance_rad;
}

VectorSample plateCurrentAt(const em::FieldSolution &solution,
                            const em::PecPlateGeometry &plate,
                            const PlateState &state,
                            double phase_rad)
{
    const em::WaveguideGeometry &geometry = solution.request.model.waveguide;
    const double input_face_z_m = plate.center_m.z - 0.5 * plate.size_m.z;
    const double sample_offset_m =
        std::max(1.0e-9,
                 std::min(geometry.inner_width_m, geometry.inner_height_m) * 1.0e-6);
    const em::Vec3 normal_from_metal_to_field{0.0, 0.0, -1.0};
    const em::FieldPhasor field =
        solution.field->evaluate({state.x_m,
                                  state.y_m,
                                  input_face_z_m - sample_offset_m});
    const em::ComplexVec3 current_phasor =
        em::surfaceCurrent(normal_from_metal_to_field, field.magnetic_a_per_m);

    VectorSample sample;
    sample.vector = em::realAtPhase(current_phasor, phase_rad);
    sample.vector.z = 0.0;
    sample.magnitude = em::magnitude(sample.vector);
    return sample;
}

PlateState unitPlateDirection(const em::FieldSolution &solution,
                              const em::PecPlateGeometry &plate,
                              const PlateState &state,
                              double phase_rad,
                              double minimum_magnitude)
{
    const VectorSample sample = plateCurrentAt(solution, plate, state, phase_rad);
    return sample.magnitude > minimum_magnitude
               ? PlateState{sample.vector.x / sample.magnitude,
                            sample.vector.y / sample.magnitude}
               : PlateState{};
}

PlateState addScaled(const PlateState &state,
                     const PlateState &direction,
                     double scale)
{
    return {state.x_m + direction.x_m * scale,
            state.y_m + direction.y_m * scale};
}

PlateState advancePlateRk4(const em::FieldSolution &solution,
                           const em::PecPlateGeometry &plate,
                           const PlateState &state,
                           double phase_rad,
                           double signed_step_m,
                           double minimum_magnitude)
{
    const auto direction_at = [&](const PlateState &value) {
        return unitPlateDirection(solution,
                                  plate,
                                  value,
                                  phase_rad,
                                  minimum_magnitude);
    };
    const PlateState k1 = direction_at(state);
    const PlateState k2 = direction_at(addScaled(state, k1, 0.5 * signed_step_m));
    const PlateState k3 = direction_at(addScaled(state, k2, 0.5 * signed_step_m));
    const PlateState k4 = direction_at(addScaled(state, k3, signed_step_m));
    return {
        state.x_m + signed_step_m *
                        (k1.x_m + 2.0 * k2.x_m + 2.0 * k3.x_m + k4.x_m) / 6.0,
        state.y_m + signed_step_m *
                        (k1.y_m + 2.0 * k2.y_m + 2.0 * k3.y_m + k4.y_m) / 6.0,
    };
}

// True when a point on the plate (given in the plate's local x/y) falls inside
// the window of a diaphragm, i.e. where there is no metal.
bool insidePlateAperture(const em::PecPlateGeometry &plate,
                         double local_x_m,
                         double local_y_m)
{
    if (!em::plateHasOpening(plate)) {
        return false;
    }
    // The stub is metal again, so it does not count as an opening.
    if (em::insidePlateStub(plate, local_x_m, local_y_m)) {
        return false;
    }
    const double dx_m = local_x_m - plate.aperture_offset_x_m;
    const double dy_m = local_y_m - plate.aperture_offset_y_m;
    if (plate.aperture_shape == em::PlateApertureShape::Circular) {
        return std::hypot(dx_m, dy_m) <= plate.aperture_radius_m;
    }
    return std::abs(dx_m) <= 0.5 * plate.aperture_width_m &&
           std::abs(dy_m) <= 0.5 * plate.aperture_height_m;
}

bool insidePlateFace(const em::WaveguideGeometry &geometry,
                     const em::PecPlateGeometry &plate,
                     const PlateState &state)
{
    const double minimum_x_m = std::max(-0.5 * geometry.inner_width_m,
                                        plate.center_m.x - 0.5 * plate.size_m.x);
    const double maximum_x_m = std::min(0.5 * geometry.inner_width_m,
                                        plate.center_m.x + 0.5 * plate.size_m.x);
    const double minimum_y_m = std::max(-0.5 * geometry.inner_height_m,
                                        plate.center_m.y - 0.5 * plate.size_m.y);
    const double maximum_y_m = std::min(0.5 * geometry.inner_height_m,
                                        plate.center_m.y + 0.5 * plate.size_m.y);
    return state.x_m >= minimum_x_m && state.x_m <= maximum_x_m &&
           state.y_m >= minimum_y_m && state.y_m <= maximum_y_m &&
           !insidePlateAperture(plate,
                                state.x_m - plate.center_m.x,
                                state.y_m - plate.center_m.y);
}

std::vector<em::Vec3> tracePlateDirection(const em::FieldSolution &solution,
                                          const em::PecPlateGeometry &plate,
                                          const PlateState &seed,
                                          double phase_rad,
                                          double signed_step_m,
                                          double minimum_magnitude,
                                          const GenerationControl &control)
{
    const em::WaveguideGeometry &geometry = solution.request.model.waveguide;
    const double input_face_z_m = plate.center_m.z - 0.5 * plate.size_m.z;
    std::vector<em::Vec3> points_m;
    PlateState state = seed;
    for (int step_index = 0; step_index < 360; ++step_index) {
        if (control.isCancellationRequested()) {
            return {};
        }
        const PlateState next_state = advancePlateRk4(solution,
                                                       plate,
                                                       state,
                                                       phase_rad,
                                                       signed_step_m,
                                                       minimum_magnitude);
        const double delta_m = std::hypot(next_state.x_m - state.x_m,
                                          next_state.y_m - state.y_m);
        if (delta_m < std::abs(signed_step_m) * 0.05 ||
            !insidePlateFace(geometry, plate, next_state)) {
            break;
        }
        state = next_state;
        points_m.push_back({state.x_m, state.y_m, input_face_z_m});
        if (step_index > 20 &&
            std::hypot(state.x_m - seed.x_m, state.y_m - seed.y_m) <
                std::abs(signed_step_m) * 0.8) {
            points_m.push_back({seed.x_m, seed.y_m, input_face_z_m});
            break;
        }
    }
    return points_m;
}

double maximumPlateCurrent(const em::FieldSolution &solution,
                           const em::PecPlateGeometry &plate,
                           double phase_rad,
                           const GenerationControl &control)
{
    const em::WaveguideGeometry &geometry = solution.request.model.waveguide;
    double maximum_magnitude = 0.0;
    for (int x_index = 0; x_index < 13; ++x_index) {
        if (control.isCancellationRequested()) {
            return 0.0;
        }
        const double x_m = -0.48 * geometry.inner_width_m +
                           x_index * 0.96 * geometry.inner_width_m / 12.0;
        for (int y_index = 0; y_index < 9; ++y_index) {
            const double y_m = -0.48 * geometry.inner_height_m +
                               y_index * 0.96 * geometry.inner_height_m / 8.0;
            maximum_magnitude = std::max(maximum_magnitude,
                                         plateCurrentAt(solution,
                                                        plate,
                                                        {x_m, y_m},
                                                        phase_rad)
                                             .magnitude);
        }
    }
    return maximum_magnitude;
}

void appendPlateCurrentLines(std::vector<VisualizationPrimitive> &primitives,
                             const em::FieldSolution &solution,
                             double phase_rad,
                             const GenerationControl &control)
{
    const em::WaveguideGeometry &geometry = solution.request.model.waveguide;
    const double step_m = std::min(geometry.inner_width_m,
                                   geometry.inner_height_m) /
                          72.0;
    for (const em::PecPlateGeometry &plate : solution.request.model.pec_plates) {
        if (!isAxisAlignedTransversePlate(plate) || control.isCancellationRequested()) {
            continue;
        }
        const double maximum_magnitude = maximumPlateCurrent(solution,
                                                              plate,
                                                              phase_rad,
                                                              control);
        if (maximum_magnitude <= vector_tolerance) {
            continue;
        }
        const double minimum_magnitude = maximum_magnitude * 0.025;
        const double input_face_z_m = plate.center_m.z - 0.5 * plate.size_m.z;
        constexpr int x_count = 9;
        constexpr int y_count = 7;
        for (int x_index = 0; x_index < x_count; ++x_index) {
            if (control.isCancellationRequested()) {
                return;
            }
            const double x_m = -0.44 * geometry.inner_width_m +
                               x_index * 0.88 * geometry.inner_width_m /
                                   (x_count - 1);
            for (int y_index = 0; y_index < y_count; ++y_index) {
                const double y_m = -0.42 * geometry.inner_height_m +
                                   y_index * 0.84 * geometry.inner_height_m /
                                       (y_count - 1);
                const PlateState seed{x_m, y_m};
                const double normalized_magnitude =
                    plateCurrentAt(solution, plate, seed, phase_rad).magnitude /
                    maximum_magnitude;
                if (!acceptSeed(normalized_magnitude, x_index, y_index)) {
                    continue;
                }

                std::vector<em::Vec3> backward = tracePlateDirection(solution,
                                                                      plate,
                                                                      seed,
                                                                      phase_rad,
                                                                      -step_m,
                                                                      minimum_magnitude,
                                                                      control);
                if (control.isCancellationRequested()) {
                    return;
                }
                std::vector<em::Vec3> forward = tracePlateDirection(solution,
                                                                     plate,
                                                                     seed,
                                                                     phase_rad,
                                                                     step_m,
                                                                     minimum_magnitude,
                                                                     control);
                if (control.isCancellationRequested()) {
                    return;
                }
                std::reverse(backward.begin(), backward.end());
                backward.push_back({x_m, y_m, input_face_z_m});
                backward.insert(backward.end(), forward.begin(), forward.end());
                if (backward.size() < 5) {
                    continue;
                }

                VisualizationPrimitive primitive;
                primitive.quantity = FieldQuantity::SurfaceCurrent;
                primitive.kind = PrimitiveKind::Polyline;
                primitive.points_m = std::move(backward);
                primitive.normalized_magnitude = std::min(1.0, normalized_magnitude);
                primitives.push_back(std::move(primitive));
            }
        }
    }
}

void appendSurfaceCurrentLines(std::vector<VisualizationPrimitive> &primitives,
                               const em::FieldSolution &solution,
                               double phase_rad,
                               const GenerationControl &control)
{
    const em::WaveguideGeometry &geometry = solution.request.model.waveguide;
    if (em::isCircular(geometry)) {
        // Токи по стенке разложены по четырём плоским граням; у цилиндрической
        // стенки параметризация другая (азимут вместо координаты вдоль грани),
        // поэтому для круглого сечения глифы стеночных токов не строятся.
        return;
    }
    const std::array<WallDefinition, 4> walls{{
        {em::WallSurface::Top, {0.0, -1.0, 0.0}, 0.5 * geometry.inner_width_m},
        {em::WallSurface::Right, {-1.0, 0.0, 0.0}, 0.5 * geometry.inner_height_m},
        {em::WallSurface::Bottom, {0.0, 1.0, 0.0}, 0.5 * geometry.inner_width_m},
        {em::WallSurface::Left, {1.0, 0.0, 0.0}, 0.5 * geometry.inner_height_m},
    }};
    const double half_length_m = 0.5 * geometry.length_m;
    const double step_m = std::min({geometry.inner_width_m,
                                    geometry.inner_height_m,
                                    geometry.length_m}) /
                          75.0;

    for (const WallDefinition &wall : walls) {
        if (control.isCancellationRequested()) {
            return;
        }
        const double maximum_magnitude = maximumSurfaceCurrent(solution,
                                                                wall,
                                                                phase_rad,
                                                                control);
        if (maximum_magnitude <= vector_tolerance) {
            continue;
        }
        const double minimum_magnitude = maximum_magnitude * 0.025;
        constexpr int u_count = 7;
        constexpr int z_count = 11;
        for (int z_index = 0; z_index < z_count; ++z_index) {
            const double z_m = -0.92 * half_length_m +
                               z_index * 1.84 * half_length_m / (z_count - 1);
            for (int u_index = 0; u_index < u_count; ++u_index) {
                const double u_m = -0.88 * wall.half_u_m +
                                   u_index * 1.76 * wall.half_u_m / (u_count - 1);
                const double normalized_magnitude =
                    surfaceCurrentAt(solution, wall, u_m, z_m, phase_rad).magnitude /
                    maximum_magnitude;
                if (!acceptSeed(normalized_magnitude, u_index, z_index) ||
                    isInsideSlot(solution, wall.wall, u_m, z_m)) {
                    continue;
                }

                const SurfaceState seed{u_m, z_m};
                std::vector<em::Vec3> backward = traceSurfaceDirection(solution,
                                                                       wall,
                                                                       seed,
                                                                       phase_rad,
                                                                       -step_m,
                                                                       minimum_magnitude,
                                                                       control);
                if (control.isCancellationRequested()) {
                    return;
                }
                std::vector<em::Vec3> forward = traceSurfaceDirection(solution,
                                                                      wall,
                                                                      seed,
                                                                      phase_rad,
                                                                      step_m,
                                                                      minimum_magnitude,
                                                                      control);
                if (control.isCancellationRequested()) {
                    return;
                }
                std::reverse(backward.begin(), backward.end());
                backward.push_back(surfacePoint(wall, geometry, u_m, z_m));
                backward.insert(backward.end(), forward.begin(), forward.end());
                if (backward.size() < 5) {
                    continue;
                }

                VisualizationPrimitive primitive;
                primitive.quantity = FieldQuantity::SurfaceCurrent;
                primitive.kind = PrimitiveKind::Polyline;
                primitive.points_m = std::move(backward);
                primitive.normalized_magnitude = std::min(1.0, normalized_magnitude);
                primitives.push_back(std::move(primitive));
            }
        }
    }
    appendPlateCurrentLines(primitives, solution, phase_rad, control);
}

void appendWallElectricArrows(std::vector<VisualizationPrimitive> &primitives,
                              const em::FieldSolution &solution,
                              double phase_rad,
                              double maximum_electric,
                              const GenerationControl &control)
{
    if (maximum_electric <= vector_tolerance) {
        return;
    }
    const em::WaveguideGeometry &geometry = solution.request.model.waveguide;
    if (em::isCircular(geometry)) {
        return;   // см. appendSurfaceCurrentLines: параметризация стенки другая
    }
    const std::array<WallDefinition, 4> walls{{
        {em::WallSurface::Top, {0.0, -1.0, 0.0}, 0.5 * geometry.inner_width_m},
        {em::WallSurface::Right, {-1.0, 0.0, 0.0}, 0.5 * geometry.inner_height_m},
        {em::WallSurface::Bottom, {0.0, 1.0, 0.0}, 0.5 * geometry.inner_width_m},
        {em::WallSurface::Left, {1.0, 0.0, 0.0}, 0.5 * geometry.inner_height_m},
    }};
    const double half_length_m = 0.5 * geometry.length_m;
    const double base_length_m = std::min(geometry.inner_width_m,
                                          geometry.inner_height_m) *
                                 0.13;
    const double sample_offset_m = base_length_m * 0.12;

    for (const WallDefinition &wall : walls) {
        if (control.isCancellationRequested()) {
            return;
        }
        for (int z_index = 0; z_index < 9; ++z_index) {
            const double z_m = -0.90 * half_length_m +
                               z_index * 1.80 * half_length_m / 8.0;
            for (int u_index = 0; u_index < 6; ++u_index) {
                const double u_m = -0.86 * wall.half_u_m +
                                   u_index * 1.72 * wall.half_u_m / 5.0;
                if (isInsideSlot(solution, wall.wall, u_m, z_m)) {
                    continue;
                }
                const em::Vec3 wall_point_m = surfacePoint(wall, geometry, u_m, z_m);
                const em::Vec3 sample_point_m =
                    wall_point_m + wall.normal_from_metal_to_field * sample_offset_m;
                const VectorSample sample = volumeVectorAt(solution,
                                                           FieldQuantity::Electric,
                                                           sample_point_m,
                                                           phase_rad);
                const double normalized_magnitude = sample.magnitude / maximum_electric;
                if (!acceptSeed(normalized_magnitude, u_index, z_index)) {
                    continue;
                }
                const em::Vec3 direction = em::normalized(sample.vector);
                const double arrow_length_m = base_length_m *
                                              (0.48 + 0.52 * normalized_magnitude);
                const em::Vec3 center_m = wall_point_m +
                                          wall.normal_from_metal_to_field *
                                              (0.52 * arrow_length_m);

                VisualizationPrimitive primitive;
                primitive.quantity = FieldQuantity::Electric;
                primitive.kind = PrimitiveKind::Arrow;
                primitive.points_m = {
                    center_m - direction * (0.43 * arrow_length_m),
                    center_m + direction * (0.43 * arrow_length_m),
                };
                primitive.normalized_magnitude = std::min(1.0, normalized_magnitude);
                primitives.push_back(std::move(primitive));
            }
        }
    }
}

void appendPoyntingArrows(std::vector<VisualizationPrimitive> &primitives,
                          const em::FieldSolution &solution,
                          const GenerationControl &control)
{
    const em::WaveguideGeometry &geometry = solution.request.model.waveguide;
    const double maximum_magnitude = maximumVolumeMagnitude(solution,
                                                             FieldQuantity::Poynting,
                                                             0.0,
                                                             control);
    if (maximum_magnitude <= vector_tolerance) {
        return;
    }
    // Physical scale of the net power flow: a single propagating watt carries
    // roughly P / (a*b) of axial Poynting density. In a pure standing wave
    // (e.g. behind a full short) the net flux is identically zero, so the field
    // magnitude collapses to rounding noise. Suppress the arrows entirely when
    // the peak power density is a tiny fraction of one guided watt.
    const double cross_section_m2 = em::crossSectionArea(geometry);
    const double reference_power_density =
        cross_section_m2 > 0.0
            ? std::max(solution.request.settings.normalization_power_w, 1.0e-12) /
                  cross_section_m2
            : 0.0;
    const double absolute_floor = 1.0e-3 * reference_power_density;
    if (maximum_magnitude <= absolute_floor) {
        return;
    }
    const double base_length_m = std::min({geometry.inner_width_m,
                                          geometry.inner_height_m,
                                          geometry.length_m}) *
                                 0.22;
    for (int z_index = 0; z_index < 8; ++z_index) {
        if (control.isCancellationRequested()) {
            return;
        }
        const double z_m = -0.42 * geometry.length_m +
                           z_index * 0.84 * geometry.length_m / 7.0;
        for (int x_index = 0; x_index < 6; ++x_index) {
            const double x_m = -0.40 * geometry.inner_width_m +
                               x_index * 0.80 * geometry.inner_width_m / 5.0;
            for (int y_index = 0; y_index < 3; ++y_index) {
                const double y_m = -0.32 * geometry.inner_height_m +
                                   y_index * 0.64 * geometry.inner_height_m / 2.0;
                const em::Vec3 center_m{x_m, y_m, z_m};
                const VectorSample sample = volumeVectorAt(solution,
                                                           FieldQuantity::Poynting,
                                                           center_m,
                                                           0.0);
                if (sample.magnitude <= absolute_floor) {
                    continue;
                }
                const double normalized_magnitude = sample.magnitude / maximum_magnitude;
                if (!acceptSeed(normalized_magnitude, x_index + 6 * y_index, z_index)) {
                    continue;
                }
                const em::Vec3 direction = em::normalized(sample.vector);
                const double arrow_length_m = base_length_m *
                                              (0.48 + 0.52 * normalized_magnitude);
                VisualizationPrimitive primitive;
                primitive.quantity = FieldQuantity::Poynting;
                primitive.kind = PrimitiveKind::Arrow;
                primitive.points_m = {
                    center_m - direction * (0.5 * arrow_length_m),
                    center_m + direction * (0.5 * arrow_length_m),
                };
                primitive.normalized_magnitude = std::min(1.0, normalized_magnitude);
                primitives.push_back(std::move(primitive));
            }
        }
    }
}

bool hasEnabledPlates(const em::FieldSolution &solution)
{
    for (const em::PecPlateGeometry &plate : solution.request.model.pec_plates) {
        if (plate.enabled) {
            return true;
        }
    }
    return false;
}

em::ComplexVec3 quantityPhasor(const em::FieldPhasor &field, FieldQuantity quantity)
{
    return quantity == FieldQuantity::Magnetic ? field.magnetic_a_per_m
                                               : field.electric_v_per_m;
}

// Locks an animated arrow to one axis: the stored phasor becomes d*(F.d), so
// Re(phasor * e^{j*phase}) always lies along +/-d. The arrow then pulses and
// reverses along the field line instead of sweeping around it.
//
// This is exact for a standing wave, where the field really does oscillate
// along a fixed direction. In a travelling wave the field is elliptically
// polarised and genuinely rotates; the component perpendicular to d is dropped
// so that the arrows stay tangent to the drawn field lines.
em::ComplexVec3 lockPhasorToDirection(const em::ComplexVec3 &phasor,
                                      const em::Vec3 &direction)
{
    const em::Complex projection = phasor.x * direction.x +
                                   phasor.y * direction.y +
                                   phasor.z * direction.z;
    return {projection * direction.x, projection * direction.y, projection * direction.z};
}

double maximumFieldEnvelope(const em::FieldSolution &solution,
                            FieldQuantity quantity,
                            const GenerationControl &control)
{
    const em::WaveguideGeometry &geometry = solution.request.model.waveguide;
    double maximum = 0.0;
    for (int x_index = 0; x_index < 9; ++x_index) {
        if (control.isCancellationRequested()) {
            return 0.0;
        }
        const double x_m = -0.48 * geometry.inner_width_m +
                           x_index * 0.96 * geometry.inner_width_m / 8.0;
        for (int y_index = 0; y_index < 7; ++y_index) {
            const double y_m = -0.48 * geometry.inner_height_m +
                               y_index * 0.96 * geometry.inner_height_m / 6.0;
            for (int z_index = 0; z_index < 11; ++z_index) {
                const double z_m = -0.48 * geometry.length_m +
                                   z_index * 0.96 * geometry.length_m / 10.0;
                const em::Vec3 point_m{x_m, y_m, z_m};
                if (!solution.field->contains(point_m)) {
                    continue;
                }
                maximum = std::max(
                    maximum,
                    em::magnitude(
                        quantityPhasor(solution.field->evaluate(point_m), quantity)));
            }
        }
    }
    return maximum;
}

// True three-dimensional field arrows (all vector components). Unlike the
// stylised TE10 flux arrows and the streamlines, these reveal the transverse
// and longitudinal components that appear near a plate or post, and they carry
// the complex phasor so the widget can animate the running wave. Used for both
// the electric and the magnetic field.
void appendVolumeVectorArrows(std::vector<VisualizationPrimitive> &primitives,
                              const em::FieldSolution &solution,
                              FieldQuantity quantity,
                              double phase_rad,
                              const GenerationControl &control)
{
    const em::WaveguideGeometry &geometry = solution.request.model.waveguide;
    const double maximum_envelope = maximumFieldEnvelope(solution, quantity, control);
    if (maximum_envelope <= vector_tolerance) {
        return;
    }
    const double base_length_m = std::min({geometry.inner_width_m,
                                          geometry.inner_height_m,
                                          geometry.length_m}) *
                                 0.40;
    constexpr int x_count = 9;
    constexpr int y_count = 3;
    constexpr int z_count = 13;
    for (int z_index = 0; z_index < z_count; ++z_index) {
        if (control.isCancellationRequested()) {
            return;
        }
        const double z_m = -0.45 * geometry.length_m +
                           z_index * 0.90 * geometry.length_m / (z_count - 1);
        for (int x_index = 0; x_index < x_count; ++x_index) {
            const double x_m = -0.42 * geometry.inner_width_m +
                               x_index * 0.84 * geometry.inner_width_m / (x_count - 1);
            for (int y_index = 0; y_index < y_count; ++y_index) {
                const double y_m = -0.36 * geometry.inner_height_m +
                                   y_index * 0.72 * geometry.inner_height_m / (y_count - 1);
                const em::Vec3 center_m{x_m, y_m, z_m};
                if (!solution.field->contains(center_m)) {
                    continue;
                }
                const em::FieldPhasor field = solution.field->evaluate(center_m);
                const em::ComplexVec3 phasor = quantityPhasor(field, quantity);
                const double envelope = em::magnitude(phasor);
                const double normalized_magnitude = envelope / maximum_envelope;
                if (!acceptSeed(normalized_magnitude,
                                x_index + x_count * y_index,
                                z_index)) {
                    continue;
                }
                const em::Vec3 instantaneous = em::realAtPhase(phasor, phase_rad);
                const double instantaneous_magnitude = em::magnitude(instantaneous);
                if (instantaneous_magnitude < vector_tolerance) {
                    continue;
                }
                const em::Vec3 direction = instantaneous / instantaneous_magnitude;
                const double arrow_length_m =
                    base_length_m * (0.4 + 0.6 * std::min(1.0, normalized_magnitude));

                VisualizationPrimitive primitive;
                primitive.quantity = quantity;
                primitive.kind = PrimitiveKind::Arrow;
                primitive.points_m = {
                    center_m - direction * (0.5 * arrow_length_m),
                    center_m + direction * (0.5 * arrow_length_m),
                };
                primitive.normalized_magnitude = std::min(1.0, normalized_magnitude);
                primitive.animated = true;
                primitive.anchor_m = center_m;
                primitive.phasor = lockPhasorToDirection(phasor, direction);
                primitive.reference_magnitude = maximum_envelope;
                primitive.arrow_length_m = base_length_m;
                primitives.push_back(std::move(primitive));
            }
        }
    }
}

double vectorComponent(const em::Vec3 &vector, int index)
{
    return index == 0 ? vector.x : (index == 1 ? vector.y : vector.z);
}

// Apply the plate rotation about the world axes in X, then Y, then Z order,
// matching both the gmsh mesher and drawPecPlates (composite Rz*Ry*Rx).
em::Vec3 rotateVector(const em::Vec3 &rotation_rad, const em::Vec3 &vector)
{
    em::Vec3 result = vector;
    {
        const double c = std::cos(rotation_rad.x);
        const double s = std::sin(rotation_rad.x);
        result = {result.x, c * result.y - s * result.z, s * result.y + c * result.z};
    }
    {
        const double c = std::cos(rotation_rad.y);
        const double s = std::sin(rotation_rad.y);
        result = {c * result.x + s * result.z, result.y, -s * result.x + c * result.z};
    }
    {
        const double c = std::cos(rotation_rad.z);
        const double s = std::sin(rotation_rad.z);
        result = {c * result.x - s * result.y, s * result.x + c * result.y, result.z};
    }
    return result;
}

// Animatable surface-current arrows on the four guide walls. The streamlines
// are traced once and cannot follow the phase, so these carry the complex
// J_s = n x H phasor and let the walls animate together with E and H.
void appendWallCurrentArrows(std::vector<VisualizationPrimitive> &primitives,
                             const em::FieldSolution &solution,
                             double phase_rad,
                             const GenerationControl &control)
{
    const em::WaveguideGeometry &geometry = solution.request.model.waveguide;
    if (em::isCircular(geometry)) {
        return;   // см. appendSurfaceCurrentLines: параметризация стенки другая
    }
    const std::array<WallDefinition, 4> walls{{
        {em::WallSurface::Top, {0.0, -1.0, 0.0}, 0.5 * geometry.inner_width_m},
        {em::WallSurface::Right, {-1.0, 0.0, 0.0}, 0.5 * geometry.inner_height_m},
        {em::WallSurface::Bottom, {0.0, 1.0, 0.0}, 0.5 * geometry.inner_width_m},
        {em::WallSurface::Left, {1.0, 0.0, 0.0}, 0.5 * geometry.inner_height_m},
    }};
    const double half_length_m = 0.5 * geometry.length_m;
    const double inside_offset_m =
        std::max(1.0e-9,
                 std::min(geometry.inner_width_m, geometry.inner_height_m) * 1.0e-6);
    const double arrow_base_m =
        std::min(geometry.inner_width_m, geometry.inner_height_m) * 0.34;
    constexpr int u_count = 5;
    constexpr int z_count = 9;

    for (const WallDefinition &wall : walls) {
        if (control.isCancellationRequested()) {
            return;
        }
        struct WallHit
        {
            em::Vec3 point_m;
            em::ComplexVec3 phasor;
            double envelope = 0.0;
        };
        std::vector<WallHit> hits;
        double maximum_envelope = 0.0;

        for (int z_index = 0; z_index < z_count; ++z_index) {
            const double z_m = -0.88 * half_length_m +
                               z_index * 1.76 * half_length_m / (z_count - 1);
            for (int u_index = 0; u_index < u_count; ++u_index) {
                const double u_m = -0.80 * wall.half_u_m +
                                   u_index * 1.60 * wall.half_u_m / (u_count - 1);
                if (isInsideSlot(solution, wall.wall, u_m, z_m)) {
                    continue;
                }
                const em::Vec3 wall_point_m = surfacePoint(wall, geometry, u_m, z_m);
                const em::Vec3 sample_point_m =
                    wall_point_m + wall.normal_from_metal_to_field * inside_offset_m;
                if (!solution.field->contains(sample_point_m)) {
                    continue;
                }
                const em::FieldPhasor field = solution.field->evaluate(sample_point_m);
                const em::ComplexVec3 phasor =
                    em::surfaceCurrent(wall.normal_from_metal_to_field,
                                       field.magnetic_a_per_m);
                const double envelope = em::magnitude(phasor);
                if (envelope <= vector_tolerance) {
                    continue;
                }
                // The arrow must sit exactly on the metal: a surface current
                // lives on the wall, not in the volume above it.
                maximum_envelope = std::max(maximum_envelope, envelope);
                hits.push_back({wall_point_m, phasor, envelope});
            }
        }

        if (maximum_envelope <= vector_tolerance) {
            continue;
        }
        for (const WallHit &hit : hits) {
            const double normalized_magnitude = hit.envelope / maximum_envelope;
            if (normalized_magnitude < 0.08) {
                continue;
            }
            em::Vec3 instantaneous = em::realAtPhase(hit.phasor, phase_rad);
            instantaneous = instantaneous -
                            wall.normal_from_metal_to_field *
                                em::dot(instantaneous, wall.normal_from_metal_to_field);
            const double instantaneous_magnitude = em::magnitude(instantaneous);
            const em::Vec3 direction = instantaneous_magnitude > vector_tolerance
                                           ? instantaneous / instantaneous_magnitude
                                           : em::Vec3{0.0, 0.0, 1.0};
            const double arrow_length_m =
                arrow_base_m * (0.4 + 0.6 * std::min(1.0, normalized_magnitude));
            VisualizationPrimitive primitive;
            primitive.quantity = FieldQuantity::SurfaceCurrent;
            primitive.kind = PrimitiveKind::Arrow;
            primitive.points_m = {
                hit.point_m - direction * (0.5 * arrow_length_m),
                hit.point_m + direction * (0.5 * arrow_length_m),
            };
            primitive.normalized_magnitude = std::min(1.0, normalized_magnitude);
            primitive.animated = true;
            primitive.anchor_m = hit.point_m;
            primitive.phasor = lockPhasorToDirection(hit.phasor, direction);
            primitive.reference_magnitude = maximum_envelope;
            primitive.arrow_length_m = arrow_base_m;
            primitives.push_back(std::move(primitive));
        }
    }
}

// Surface-current arrows on every face of every enabled plate, including
// rotated plates and the side and downstream faces (the streamline pass only
// covers the axis-aligned upstream face).
void appendPlateCurrentArrows(std::vector<VisualizationPrimitive> &primitives,
                              const em::FieldSolution &solution,
                              double phase_rad,
                              const GenerationControl &control)
{
    const em::WaveguideGeometry &geometry = solution.request.model.waveguide;
    const double offset_m =
        std::max(1.0e-9,
                 std::min(geometry.inner_width_m, geometry.inner_height_m) * 1.0e-6);
    const double arrow_base_m =
        std::min(geometry.inner_width_m, geometry.inner_height_m) * 0.10;
    const em::Vec3 unit_axis[3] = {{1.0, 0.0, 0.0}, {0.0, 1.0, 0.0}, {0.0, 0.0, 1.0}};
    constexpr int samples_u = 5;
    constexpr int samples_v = 5;

    struct FaceHit
    {
        em::Vec3 point_m;
        em::Vec3 current;
        em::ComplexVec3 phasor;
        double magnitude = 0.0;
        double envelope = 0.0;
    };

    for (const em::PecPlateGeometry &plate : solution.request.model.pec_plates) {
        if (!plate.enabled || control.isCancellationRequested()) {
            continue;
        }
        const em::Vec3 half{0.5 * plate.size_m.x,
                            0.5 * plate.size_m.y,
                            0.5 * plate.size_m.z};
        std::vector<FaceHit> hits;
        double maximum_magnitude = 0.0;

        for (int axis = 0; axis < 3; ++axis) {
            for (int sign_index = 0; sign_index < 2; ++sign_index) {
                const double sign = sign_index == 0 ? 1.0 : -1.0;
                const int a1 = (axis + 1) % 3;
                const int a2 = (axis + 2) % 3;
                const em::Vec3 local_normal = unit_axis[axis] * sign;
                const em::Vec3 world_normal = rotateVector(plate.rotation_rad, local_normal);
                for (int iu = 0; iu < samples_u; ++iu) {
                    const double fu = samples_u > 1
                                          ? -0.8 + 1.6 * iu / (samples_u - 1)
                                          : 0.0;
                    for (int iv = 0; iv < samples_v; ++iv) {
                        const double fv = samples_v > 1
                                              ? -0.8 + 1.6 * iv / (samples_v - 1)
                                              : 0.0;
                        const em::Vec3 local_point =
                            local_normal * vectorComponent(half, axis) +
                            unit_axis[a1] * (fu * vectorComponent(half, a1)) +
                            unit_axis[a2] * (fv * vectorComponent(half, a2));
                        // A diaphragm carries no metal inside its window.
                        if (insidePlateAperture(plate, local_point.x, local_point.y)) {
                            continue;
                        }
                        const em::Vec3 world_point =
                            plate.center_m + rotateVector(plate.rotation_rad, local_point);
                        const em::Vec3 sample_point = world_point + world_normal * offset_m;
                        if (!solution.field->contains(sample_point)) {
                            continue;
                        }
                        const em::FieldPhasor field = solution.field->evaluate(sample_point);
                        const em::ComplexVec3 current_phasor =
                            em::surfaceCurrent(world_normal, field.magnetic_a_per_m);
                        em::Vec3 current = em::realAtPhase(current_phasor, phase_rad);
                        const double normal_component = em::dot(current, world_normal);
                        current = current - world_normal * normal_component;
                        const double magnitude = em::magnitude(current);
                        const double envelope = em::magnitude(current_phasor);
                        if (envelope <= vector_tolerance) {
                            continue;
                        }
                        maximum_magnitude = std::max(maximum_magnitude, envelope);
                        hits.push_back({world_point + world_normal * (2.0 * offset_m),
                                        current,
                                        current_phasor,
                                        magnitude,
                                        envelope});
                    }
                }
            }
        }

        if (maximum_magnitude <= vector_tolerance) {
            continue;
        }
        for (const FaceHit &hit : hits) {
            const double normalized_magnitude = hit.envelope / maximum_magnitude;
            if (normalized_magnitude < 0.06) {
                continue;
            }
            const double arrow_length_m =
                arrow_base_m * (0.4 + 0.6 * std::min(1.0, normalized_magnitude));
            const em::Vec3 direction = hit.magnitude > vector_tolerance
                                           ? hit.current / hit.magnitude
                                           : em::Vec3{0.0, 0.0, 1.0};
            VisualizationPrimitive primitive;
            primitive.quantity = FieldQuantity::SurfaceCurrent;
            primitive.kind = PrimitiveKind::Arrow;
            primitive.points_m = {
                hit.point_m - direction * (0.5 * arrow_length_m),
                hit.point_m + direction * (0.5 * arrow_length_m),
            };
            primitive.normalized_magnitude = std::min(1.0, normalized_magnitude);
            primitive.animated = true;
            primitive.anchor_m = hit.point_m;
            primitive.phasor = lockPhasorToDirection(hit.phasor, direction);
            primitive.reference_magnitude = maximum_magnitude;
            primitive.arrow_length_m = arrow_base_m;
            primitives.push_back(std::move(primitive));
        }
    }
}
}

std::vector<VisualizationPrimitive> FieldVisualizationGenerator::generate(
    const em::FieldSolution &solution,
    const FieldVisualizationSettings &settings,
    const GenerationControl &control) const
{
    std::vector<VisualizationPrimitive> primitives;
    if (!solution.success || !solution.has_selected_mode || !solution.field ||
        control.isCancellationRequested()) {
        return primitives;
    }

    if (settings.generate_electric) {
        if (isDominantTe10(solution)) {
            appendTe10ElectricFluxArrows(primitives,
                                         solution,
                                         settings.phase_rad,
                                         control);
        } else {
            appendVolumeLines(primitives,
                              solution,
                              FieldQuantity::Electric,
                              settings.phase_rad,
                              control);
        }
        if (control.isCancellationRequested()) {
            return {};
        }
        if (!isDominantTe10(solution)) {
            appendWallElectricArrows(primitives,
                                     solution,
                                     settings.phase_rad,
                                     maximumVolumeMagnitude(solution,
                                                            FieldQuantity::Electric,
                                                            settings.phase_rad,
                                                            control),
                                     control);
        }
        if (control.isCancellationRequested()) {
            return {};
        }
        // Near a plate the field is no longer a pure mode, so also show the true
        // three-dimensional E vector (E_x, E_y, E_z), not only the mode profile.
        if (hasEnabledPlates(solution)) {
            appendVolumeVectorArrows(primitives,
                                     solution,
                                     FieldQuantity::Electric,
                                     settings.phase_rad,
                                     control);
            if (control.isCancellationRequested()) {
                return {};
            }
        }
    }
    if (settings.generate_magnetic) {
        appendVolumeLines(primitives,
                          solution,
                          FieldQuantity::Magnetic,
                          settings.phase_rad,
                          control);
        if (control.isCancellationRequested()) {
            return {};
        }
        // Streamlines are traced once and cannot follow the phase, so the
        // magnetic field also gets animatable vector arrows — otherwise H would
        // stand still while E oscillates.
        appendVolumeVectorArrows(primitives,
                                 solution,
                                 FieldQuantity::Magnetic,
                                 settings.phase_rad,
                                 control);
        if (control.isCancellationRequested()) {
            return {};
        }
    }
    if (settings.generate_surface_current) {
        appendSurfaceCurrentLines(primitives, solution, settings.phase_rad, control);
        if (control.isCancellationRequested()) {
            return {};
        }
        appendPlateCurrentArrows(primitives, solution, settings.phase_rad, control);
        if (control.isCancellationRequested()) {
            return {};
        }
        appendWallCurrentArrows(primitives, solution, settings.phase_rad, control);
        if (control.isCancellationRequested()) {
            return {};
        }
    }
    if (settings.generate_poynting) {
        appendPoyntingArrows(primitives, solution, control);
        if (control.isCancellationRequested()) {
            return {};
        }
    }
    return primitives;
}

FieldSliceData FieldVisualizationGenerator::generateSlice(
    const em::FieldSolution &solution,
    SlicePlaneKind plane,
    const GenerationControl &control) const
{
    FieldSliceData slice;
    slice.plane = plane;
    slice.quantity = FieldQuantity::Electric;
    if (!solution.success || !solution.has_selected_mode || !solution.field ||
        control.isCancellationRequested()) {
        return slice;
    }

    const em::WaveguideGeometry &geometry = solution.request.model.waveguide;
    const em::Vec3 v_axis{0.0, 0.0, 1.0};       // the slice always spans z
    const double v_span_m = geometry.length_m;
    em::Vec3 u_axis;
    double u_span_m = 0.0;
    if (plane == SlicePlaneKind::HorizontalXZ) {
        u_axis = {1.0, 0.0, 0.0};
        u_span_m = geometry.inner_width_m;
    } else {
        u_axis = {0.0, 1.0, 0.0};
        u_span_m = geometry.inner_height_m;
    }

    const int v_count = 220;
    const int u_count = std::clamp(
        static_cast<int>(std::round(v_count * u_span_m / std::max(1.0e-9, v_span_m))),
        24,
        160);
    const double du_m = u_span_m / u_count;
    const double dv_m = v_span_m / v_count;

    // Sampling the plane dominates slice generation (tens of thousands of field
    // evaluations), and every cell is independent, so it runs across all cores
    // whenever the evaluator allows concurrent sampling (closed-form modes do;
    // the MFEM backend does not). Only worker 0 polls the cancellation callback:
    // the callback belongs to the caller and need not be thread-safe.
    const int cell_count = u_count * v_count;
    slice.cells.assign(static_cast<std::size_t>(cell_count), SliceSampleCell{});
    std::vector<double> worker_maximum(maximum_workers, 0.0);
    std::atomic<bool> cancelled{false};

    parallelFor(solution.field->supportsConcurrentEvaluation(), cell_count,
                [&](int index, int worker) {
        if (cancelled.load(std::memory_order_relaxed)) {
            return;
        }
        if (worker == 0 && (index % 1024) == 0 && control.isCancellationRequested()) {
            cancelled.store(true, std::memory_order_relaxed);
            return;
        }
        const int v_index = index / u_count;
        const int u_index = index % u_count;
        const double v_m = -0.5 * v_span_m + (v_index + 0.5) * dv_m;
        const double u_m = -0.5 * u_span_m + (u_index + 0.5) * du_m;
        const em::Vec3 point_m = u_axis * u_m + v_axis * v_m;

        SliceSampleCell cell;
        cell.center_m = point_m;
        cell.u_half_m = u_axis * (0.5 * du_m);
        cell.v_half_m = v_axis * (0.5 * dv_m);
        if (solution.field->contains(point_m)) {
            const em::FieldPhasor field = solution.field->evaluate(point_m);
            cell.phasor = field.electric_v_per_m;
            cell.envelope = em::magnitude(field.electric_v_per_m);
            worker_maximum[static_cast<std::size_t>(worker)] =
                std::max(worker_maximum[static_cast<std::size_t>(worker)], cell.envelope);
        }
        slice.cells[static_cast<std::size_t>(index)] = cell;
    });

    if (cancelled.load(std::memory_order_relaxed) || control.isCancellationRequested()) {
        return FieldSliceData{};
    }
    const double maximum =
        *std::max_element(worker_maximum.begin(), worker_maximum.end());
    slice.maximum_value = maximum;
    slice.valid = maximum > 0.0;
    return slice;
}
}
