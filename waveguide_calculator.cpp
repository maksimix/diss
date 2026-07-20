#include "waveguide_calculator.h"

#include "em/em_solver_dispatcher.h"
#include "postprocessing/qt_field_glyph_adapter.h"
#include "postprocessing/slot_excitation_estimator.h"

#include <algorithm>
#include <cmath>
#include <memory>

namespace
{
double mmToM(double value_mm)
{
    return value_mm * 1.0e-3;
}

double mToMm(double value_m)
{
    return value_m * 1.0e3;
}

bool positiveFinite(double value)
{
    return std::isfinite(value) && value > 0.0;
}

QString modeName(const em::ModeDescriptor &mode)
{
    return QStringLiteral("%1%2%3")
        .arg(mode.family == em::ModeFamily::TransverseElectric
                 ? QStringLiteral("TE")
                 : QStringLiteral("TM"))
        .arg(mode.m)
        .arg(mode.n);
}

WaveguideMode toWaveguideMode(const em::ModeDescriptor &mode)
{
    WaveguideMode result;
    result.name = modeName(mode);
    result.transverse_electric = mode.family == em::ModeFamily::TransverseElectric;
    result.m = mode.m;
    result.n = mode.n;
    result.cutoff_ghz = mode.cutoff_frequency_hz * 1.0e-9;
    result.propagates = mode.propagating;
    return result;
}

em::WallSurface toEmWallSurface(int surface)
{
    switch (surface) {
    case 1:
        return em::WallSurface::Right;
    case 2:
        return em::WallSurface::Bottom;
    case 3:
        return em::WallSurface::Left;
    default:
        return em::WallSurface::Top;
    }
}

QString validateParameters(const WaveguideParameters &parameters)
{
    if (!positiveFinite(parameters.length_mm) ||
        !positiveFinite(parameters.width_mm) ||
        !positiveFinite(parameters.depth_mm)) {
        return QStringLiteral("Размеры волновода должны быть конечными и больше нуля.");
    }
    if (!positiveFinite(parameters.wall_thickness_mm)) {
        return QStringLiteral("Толщина стенки должна быть конечной и больше нуля.");
    }
    if (parameters.width_mm <= 2.0 * parameters.wall_thickness_mm ||
        parameters.depth_mm <= 2.0 * parameters.wall_thickness_mm) {
        return QStringLiteral("Толщина стенки перекрывает внутреннюю полость волновода.");
    }
    if (!positiveFinite(parameters.frequency_ghz)) {
        return QStringLiteral("Частота должна быть конечной и больше нуля.");
    }
    for (const PecPlateParameters &plate : parameters.pec_plates) {
        const bool finite = std::isfinite(plate.x_min_mm) && std::isfinite(plate.x_max_mm) &&
                            std::isfinite(plate.y_min_mm) && std::isfinite(plate.y_max_mm) &&
                            std::isfinite(plate.z_min_mm) && std::isfinite(plate.z_max_mm) &&
                            std::isfinite(plate.rotation_x_deg) &&
                            std::isfinite(plate.rotation_y_deg) &&
                            std::isfinite(plate.rotation_z_deg);
        if (!finite || plate.x_max_mm <= plate.x_min_mm ||
            plate.y_max_mm <= plate.y_min_mm || plate.z_max_mm <= plate.z_min_mm) {
            return QStringLiteral("Некорректные координаты PEC-пластины.");
        }
        const double half_inner_width =
            0.5 * (parameters.width_mm - 2.0 * parameters.wall_thickness_mm);
        const double half_inner_height =
            0.5 * (parameters.depth_mm - 2.0 * parameters.wall_thickness_mm);
        const double half_length = 0.5 * parameters.length_mm;
        if (plate.enabled &&
            (plate.x_min_mm < -half_inner_width || plate.x_max_mm > half_inner_width ||
             plate.y_min_mm < -half_inner_height || plate.y_max_mm > half_inner_height ||
             plate.z_min_mm < -half_length || plate.z_max_mm > half_length)) {
            return QStringLiteral("PEC-пластина должна находиться внутри полости волновода.");
        }
    }
    if (!parameters.slot_enabled) {
        return {};
    }
    if (!positiveFinite(parameters.slot_length_mm) ||
        !positiveFinite(parameters.slot_width_mm) ||
        !std::isfinite(parameters.slot_offset_x_mm) ||
        !std::isfinite(parameters.slot_offset_z_mm) ||
        !std::isfinite(parameters.slot_rotation_deg)) {
        return QStringLiteral("Размеры, положение и угол щели должны быть конечными.");
    }

    const double inner_width_mm = parameters.width_mm - 2.0 * parameters.wall_thickness_mm;
    const double inner_height_mm = parameters.depth_mm - 2.0 * parameters.wall_thickness_mm;
    const int slot_surface = std::clamp(parameters.slot_surface, 0, 3);
    const double slot_span_mm = slot_surface == 1 || slot_surface == 3
                                    ? inner_height_mm
                                    : inner_width_mm;
    const double angle_rad = parameters.slot_rotation_deg * em::pi / 180.0;
    const double half_slot_width_mm = 0.5 * parameters.slot_width_mm;
    const double half_slot_length_mm = 0.5 * parameters.slot_length_mm;
    const double u_extent_mm = std::abs(std::cos(angle_rad)) * half_slot_width_mm +
                               std::abs(std::sin(angle_rad)) * half_slot_length_mm;
    const double z_extent_mm = std::abs(std::sin(angle_rad)) * half_slot_width_mm +
                               std::abs(std::cos(angle_rad)) * half_slot_length_mm;
    const double half_free_u_mm = 0.5 * slot_span_mm - u_extent_mm;
    const double half_free_z_mm = 0.5 * parameters.length_mm - z_extent_mm;
    if (half_free_u_mm < 0.0 || half_free_z_mm < 0.0 ||
        std::abs(parameters.slot_offset_x_mm) > half_free_u_mm ||
        std::abs(parameters.slot_offset_z_mm) > half_free_z_mm) {
        return QStringLiteral("Положение или поворот выводят щель за выбранную стенку.");
    }
    return {};
}

em::SimulationRequest buildRequest(const WaveguideParameters &parameters,
                                   double inner_width_mm,
                                   double inner_height_mm)
{
    em::SimulationRequest request;
    request.frequency_hz = parameters.frequency_ghz * 1.0e9;
    request.model.waveguide.inner_width_m = mmToM(inner_width_mm);
    request.model.waveguide.inner_height_m = mmToM(inner_height_mm);
    request.model.waveguide.length_m = mmToM(parameters.length_mm);
    request.model.waveguide.wall_thickness_m = mmToM(parameters.wall_thickness_mm);
    request.model.waveguide.wall_conductivity_s_per_m =
        std::max(0.0, parameters.wall_conductivity_s_per_m);
    request.settings.maximum_m = 3;
    request.settings.maximum_n = 3;
    request.settings.normalization_power_w = 1.0;
    request.settings.fem.relative_tolerance = 1.0e-6;
    request.settings.fem.maximum_iterations = 1200;

    if (parameters.slot_enabled) {
        em::SlotGeometry slot;
        slot.enabled = true;
        slot.wall = toEmWallSurface(parameters.slot_surface);
        slot.center_u_m = mmToM(parameters.slot_offset_x_mm);
        slot.center_z_m = mmToM(parameters.slot_offset_z_mm);
        slot.length_m = mmToM(parameters.slot_length_mm);
        slot.width_m = mmToM(parameters.slot_width_mm);
        slot.rotation_rad = parameters.slot_rotation_deg * em::pi / 180.0;
        request.model.slot_geometries.push_back(slot);
        request.settings.geometry_approximation_policy =
            em::GeometryApproximationPolicy::UnperturbedBackgroundForSlots;
    }
    for (const PecPlateParameters &plate_parameters : parameters.pec_plates) {
        if (!plate_parameters.enabled) {
            continue;
        }
        em::PecPlateGeometry plate;
        plate.enabled = true;
        plate.center_m = {
            mmToM(0.5 * (plate_parameters.x_min_mm + plate_parameters.x_max_mm)),
            mmToM(0.5 * (plate_parameters.y_min_mm + plate_parameters.y_max_mm)),
            mmToM(0.5 * (plate_parameters.z_min_mm + plate_parameters.z_max_mm)),
        };
        plate.size_m = {
            mmToM(plate_parameters.x_max_mm - plate_parameters.x_min_mm),
            mmToM(plate_parameters.y_max_mm - plate_parameters.y_min_mm),
            mmToM(plate_parameters.z_max_mm - plate_parameters.z_min_mm),
        };
        plate.rotation_rad = {
            plate_parameters.rotation_x_deg * em::pi / 180.0,
            plate_parameters.rotation_y_deg * em::pi / 180.0,
            plate_parameters.rotation_z_deg * em::pi / 180.0,
        };
        request.model.pec_plates.push_back(plate);
    }
    return request;
}
}

WaveguideCalculationResult WaveguideCalculator::calculate(
    const WaveguideParameters &parameters,
    const std::function<bool()> &cancellation_requested,
    const std::function<void(const QString &)> &progress_reporter) const
{
    WaveguideCalculationResult result;
    result.parameters = parameters;
    const auto cancelled = [&cancellation_requested]() {
        return cancellation_requested && cancellation_requested();
    };
    const auto report = [&progress_reporter](const QString &stage) {
        if (progress_reporter) {
            progress_reporter(stage);
        }
    };
    if (cancelled()) {
        result.cancelled = true;
        return result;
    }
    result.error_message = validateParameters(parameters);
    if (!result.error_message.isEmpty()) {
        return result;
    }

    result.inner_width_mm = parameters.width_mm - 2.0 * parameters.wall_thickness_mm;
    result.inner_depth_mm = parameters.depth_mm - 2.0 * parameters.wall_thickness_mm;
    result.area_mm2 = result.inner_width_mm * result.inner_depth_mm;
    result.cavity_volume_mm3 = result.area_mm2 * parameters.length_mm;
    const double outer_volume_mm3 = parameters.width_mm * parameters.depth_mm *
                                    parameters.length_mm;
    result.metal_volume_mm3 = std::max(0.0,
                                       outer_volume_mm3 - result.cavity_volume_mm3);
    for (const PecPlateParameters &plate : parameters.pec_plates) {
        if (!plate.enabled) {
            continue;
        }
        const double plate_volume_mm3 = (plate.x_max_mm - plate.x_min_mm) *
                                        (plate.y_max_mm - plate.y_min_mm) *
                                        (plate.z_max_mm - plate.z_min_mm);
        result.metal_volume_mm3 += plate_volume_mm3;
        result.cavity_volume_mm3 = std::max(0.0,
                                            result.cavity_volume_mm3 - plate_volume_mm3);
    }

    const em::SimulationRequest request = buildRequest(parameters,
                                                       result.inner_width_mm,
                                                       result.inner_depth_mm);
    em::SolveControl solve_control;
    solve_control.cancellation_requested = cancellation_requested;
    solve_control.progress_reporter = [&report](const std::string &stage) {
        report(QString::fromStdString(stage));
    };
    report(QStringLiteral("Запуск электромагнитного решателя..."));
    const std::shared_ptr<em::FieldSolution> field_solution =
        std::make_shared<em::FieldSolution>(
            em::EmSolverDispatcher().solve(request, solve_control));
    result.field_solution = field_solution;
    if (field_solution->cancelled || cancelled()) {
        result.cancelled = true;
        result.field_solution.reset();
        return result;
    }
    if (!field_solution->success) {
        result.error_message = QString::fromStdString(field_solution->error_message);
        return result;
    }

    result.valid = true;
    result.solver_backend = QString::fromStdString(field_solution->diagnostics.backend_name);
    result.incident_power_w = field_solution->diagnostics.incident_power_w;
    result.reflected_power_w = field_solution->diagnostics.reflected_power_w;
    result.transmitted_power_w = field_solution->diagnostics.transmitted_power_w;
    result.dissipated_power_w = field_solution->diagnostics.dissipated_power_w;
    result.input_power_w = field_solution->diagnostics.input_power_w;
    result.output_power_w = field_solution->diagnostics.output_power_w;
    result.power_balance_relative_error =
        field_solution->diagnostics.power_balance_relative_error;
    result.conductor_attenuation_np_per_m =
        field_solution->diagnostics.conductor_attenuation_np_per_m;
    result.stored_electric_energy_j = field_solution->diagnostics.stored_electric_energy_j;
    result.stored_magnetic_energy_j = field_solution->diagnostics.stored_magnetic_energy_j;
    result.quality_factor = field_solution->diagnostics.quality_factor;
    result.s11_magnitude = std::abs(field_solution->scattering.s11);
    result.s21_magnitude = std::abs(field_solution->scattering.s21);
    result.wavelength0_mm = mToMm(em::speed_of_light_m_per_s / request.frequency_hz);
    for (const std::string &warning : field_solution->diagnostics.warnings) {
        result.solver_warnings.push_back(QString::fromStdString(warning));
    }
    for (const em::ModeDescriptor &mode : field_solution->available_modes) {
        result.modes.push_back(toWaveguideMode(mode));
    }

    if (!field_solution->has_selected_mode || !field_solution->selected_mode.propagating) {
        return result;
    }

    result.has_propagating_mode = true;
    result.selected_mode = toWaveguideMode(field_solution->selected_mode);
    result.beta_rad_per_m = std::imag(
        field_solution->selected_mode.propagation_constant_per_m);
    result.attenuation_np_per_m = std::real(
        field_solution->selected_mode.propagation_constant_per_m);
    result.guide_wavelength_mm = result.beta_rad_per_m > 0.0
                                     ? mToMm(2.0 * em::pi / result.beta_rad_per_m)
                                     : 0.0;
    if (!request.model.slot_geometries.empty()) {
        report(QStringLiteral("Оценка возбуждения щели..."));
        result.slot_normalized_coupling =
            postprocessing::SlotExcitationEstimator().normalizedCoupling(
                *field_solution,
                request.model.slot_geometries.front(),
                cancellation_requested);
        if (cancelled()) {
            result.cancelled = true;
            result.valid = false;
            result.field_solution.reset();
            return result;
        }
    }
    report(QStringLiteral("Построение линий и стрелок поля..."));
    postprocessing::GenerationControl generation_control;
    generation_control.cancellation_requested = cancellation_requested;
    const QtFieldGlyphAdapter adapter;
    result.field_glyphs = adapter.build(*field_solution, generation_control);
    if (cancelled()) {
        result.cancelled = true;
        result.valid = false;
        result.field_glyphs.clear();
        result.field_solution.reset();
        return result;
    }

    report(QStringLiteral("Построение заливки |E| на срезах..."));
    result.horizontal_slice =
        adapter.buildSlice(*field_solution, FieldSlicePlane::HorizontalXZ, generation_control);
    result.vertical_slice =
        adapter.buildSlice(*field_solution, FieldSlicePlane::VerticalYZ, generation_control);
    if (cancelled()) {
        result.cancelled = true;
        result.valid = false;
        result.field_glyphs.clear();
        result.horizontal_slice = {};
        result.vertical_slice = {};
        result.field_solution.reset();
    }
    return result;
}
