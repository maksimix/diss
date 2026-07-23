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

bool isCircularSection(const WaveguideParameters &parameters)
{
    return parameters.cross_section == 1;
}

QString validateParameters(const WaveguideParameters &parameters)
{
    if (!positiveFinite(parameters.length_mm)) {
        return QStringLiteral("Длина волновода должна быть конечной и больше нуля.");
    }
    if (!positiveFinite(parameters.wall_thickness_mm)) {
        return QStringLiteral("Толщина стенки должна быть конечной и больше нуля.");
    }
    if (isCircularSection(parameters)) {
        if (!positiveFinite(parameters.radius_mm)) {
            return QStringLiteral("Радиус волновода должен быть конечным и больше нуля.");
        }
        if (parameters.radius_mm <= parameters.wall_thickness_mm) {
            return QStringLiteral("Толщина стенки перекрывает внутреннюю полость волновода.");
        }
        // Круглое сечение пока считается только как пустой тракт: вставки в нём
        // не поддержаны ни одним решателем, поэтому ошибка выдаётся здесь, а не
        // после долгого расчёта.
        const bool has_plates = std::any_of(parameters.pec_plates.cbegin(),
                                            parameters.pec_plates.cend(),
                                            [](const PecPlateParameters &plate) {
                                                return plate.enabled;
                                            });
        if (has_plates || parameters.slot_enabled) {
            return QStringLiteral(
                "В круглом волноводе пока поддержан только пустой тракт: отключите щель и "
                "пластины или вернитесь к прямоугольному сечению.");
        }
        if (!positiveFinite(parameters.frequency_ghz)) {
            return QStringLiteral("Частота должна быть конечной и больше нуля.");
        }
        return {};
    }
    if (!positiveFinite(parameters.width_mm) ||
        !positiveFinite(parameters.depth_mm)) {
        return QStringLiteral("Размеры волновода должны быть конечными и больше нуля.");
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
        if (plate.enabled && plate.aperture_enabled) {
            if (!std::isfinite(plate.aperture_offset_x_mm) ||
                !std::isfinite(plate.aperture_offset_y_mm)) {
                return QStringLiteral("Смещение окна пластины должно быть конечным.");
            }
            const double plate_width_mm = plate.x_max_mm - plate.x_min_mm;
            const double plate_height_mm = plate.y_max_mm - plate.y_min_mm;
            const bool circular = plate.aperture_shape == 1;
            const double half_span_x =
                circular ? plate.aperture_radius_mm : 0.5 * plate.aperture_width_mm;
            const double half_span_y =
                circular ? plate.aperture_radius_mm : 0.5 * plate.aperture_height_mm;
            if (circular ? !positiveFinite(plate.aperture_radius_mm)
                         : (!positiveFinite(plate.aperture_width_mm) ||
                            !positiveFinite(plate.aperture_height_mm))) {
                return QStringLiteral("Размеры окна пластины должны быть конечными и больше нуля.");
            }
            // A rectangular window may reach the plate edge (inductive and
            // capacitive irises, solved by mode matching); a circular one must
            // stay strictly inside, tangency would break the FEM booleans.
            const double slack_mm = circular ? 0.0 : 1.0e-6;
            const bool exceeds_x = circular
                                       ? std::abs(plate.aperture_offset_x_mm) + half_span_x >=
                                             0.5 * plate_width_mm
                                       : std::abs(plate.aperture_offset_x_mm) + half_span_x >
                                             0.5 * plate_width_mm + slack_mm;
            const bool exceeds_y = circular
                                       ? std::abs(plate.aperture_offset_y_mm) + half_span_y >=
                                             0.5 * plate_height_mm
                                       : std::abs(plate.aperture_offset_y_mm) + half_span_y >
                                             0.5 * plate_height_mm + slack_mm;
            if (exceeds_x || exceeds_y) {
                return QStringLiteral("Окно должно помещаться внутри пластины (круглое — не касаясь краёв).");
            }
            if (plate.post_enabled) {
                if (!positiveFinite(plate.post_width_mm) ||
                    !positiveFinite(plate.post_height_mm)) {
                    return QStringLiteral("Ширина и высота язычка должны быть конечными и больше нуля.");
                }
                if (plate.post_width_mm > plate_width_mm) {
                    return QStringLiteral("Язычок шире самой пластины.");
                }
                if (plate.post_height_mm > plate_height_mm) {
                    return QStringLiteral("Язычок выше самой пластины.");
                }
            }
        } else if (plate.enabled && plate.post_enabled) {
            return QStringLiteral("Язычок задаётся вместе с окном в пластине.");
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

// Accuracy levels are measured points, not guesses. Reference model: guide
// 22.66 x 9.96 x 50 mm, full-section plate 0.5 mm thick with a round window
// r = 4.6 mm and a 0.49 x 4.98 mm stub, TE10 at 10 GHz, direct solver, Release.
//
// Two things decide the answer here, and the global element size is neither of
// them: the element order and the local refinement (elements_across_smallest
// _feature together with minimum_size_ratio). With the refinement switched off
// the |S11| of that model swings between 0.0481 and 0.7492 over global steps of
// 2.800 / 2.801 / 2.806 / 2.828 mm - a half-spread of 81.6 % of the mean - so no
// level is allowed to switch it off, not even the fastest one. refinement_factor
// therefore stays at 1 everywhere; moving it only shifts the global size that
// the refinement then overrides near the details.
//
// Half-spread of |S11| over those same four global steps, unknown count, peak
// process memory and wall time per run:
//   fast    order 1, feature 1, ratio 8:  0.231 %,  17530,  0.6 GB,   12 s
//   normal  order 2, feature 1, ratio 4:  0.100 %,  57049,  3.6 GB,  211 s
//   high    order 2, feature 2, ratio 8:  0.039 %, 133542, 18.9 GB, 1602 s
// Read those three lines as a comparison between levels only. They were taken
// with the global element size pinned at 2.8 mm to isolate the mesh-drift
// sensitivity, and the product never pins it: maximum_element_size_m stays 0, so
// the automatic rule applies. Costs measured through the real path, on the model
// the program starts with plus its default plate, are:
//   fast    20366 unknowns, 0.51 GB,  24 s, unitarity defect 4.0e-3
//   normal  28692 unknowns, 1.46 GB,  82 s, unitarity defect 1.7e-5
//   high    62962 unknowns, 5.90 GB, 447 s, unitarity defect 2.3e-5
// The two-hundredfold drop in the unitarity defect between fast and normal is
// the second-order elements: at a proportionally coarser mesh they cost about
// the same and conserve power far better.
// The fast level keeps ratio 8 rather than the 4 of the normal one because at
// ratio 4 the 0.7 mm floor swallows the 0.49 mm stub: same order, same feature
// count, but the half-spread degrades from 0.231 % to 0.623 % for 11343
// unknowns, so the cheaper mesh is not the more reproducible one.
// Accuracy against the mode-matching answer for the same model with a
// rectangular window (exact 0.7685722) improves in the same order: |S11| tends
// to about 0.87 as the refinement grows, and order 2 at feature 1 (0.8681) is
// already closer to that limit than order 1 at feature 3 (0.8572) while costing
// less in every column - which is why "normal" runs second-order elements.
void applyAccuracyLevel(em::FemSolverSettings &fem, int level)
{
    fem.mesh.refinement_factor = 1.0;
    switch (std::clamp(level, 0, 2)) {
    case 0:   // быстро
        fem.mesh.element_order = 1;
        fem.mesh.elements_across_smallest_feature = 1.0;
        fem.mesh.minimum_size_ratio = 8.0;
        fem.maximum_iterations = 1200;
        break;
    case 2:   // высокое
        fem.mesh.element_order = 2;
        fem.mesh.elements_across_smallest_feature = 2.0;
        fem.mesh.minimum_size_ratio = 8.0;
        fem.maximum_iterations = 3000;
        break;
        // A finer level was measured and withdrawn: order 1 with feature 3 and
        // ratio 20 costs 95941 unknowns and 277 s yet lands at |S11| = 0.8572,
        // further from the limit than "normal" at 0.8681 for 57049 unknowns.
        // Above that the iterative solver has no chance either - a 17:1 cell
        // contrast leaves GMRES at a 1e-2 residual after 8000 iterations.
    default:  // обычное
        fem.mesh.element_order = 2;
        fem.mesh.elements_across_smallest_feature = 1.0;
        fem.mesh.minimum_size_ratio = 4.0;
        fem.maximum_iterations = 1200;
        break;
    }
    // The memory budget of the direct solver is deliberately left alone: it
    // defaults to what the machine actually has free, and raising it from here
    // would only trade a fallback to GMRES for swapping. Every number above was
    // measured with the factorisation, so on a machine too small for the level
    // the automatic choice moves to GMRES and the answer degrades - the linear
    // solver box next to the quality box is there to force the factorisation.
}

em::LinearSolverMethod toEmLinearSolverMethod(int method)
{
    switch (method) {
    case 1:
        return em::LinearSolverMethod::Direct;
    case 2:
        return em::LinearSolverMethod::Iterative;
    default:
        return em::LinearSolverMethod::Automatic;
    }
}

em::SolverMethod toEmSolverMethod(int method)
{
    switch (method) {
    case 1:
        return em::SolverMethod::AnalyticRectangular;
    case 2:
        return em::SolverMethod::TransversePartition;
    case 3:
        return em::SolverMethod::ModeMatching;
    case 4:
        return em::SolverMethod::FiniteElement;
    default:
        return em::SolverMethod::Automatic;
    }
}

em::SimulationRequest buildRequest(const WaveguideParameters &parameters,
                                   double inner_width_mm,
                                   double inner_height_mm)
{
    em::SimulationRequest request;
    request.frequency_hz = parameters.frequency_ghz * 1.0e9;
    if (isCircularSection(parameters)) {
        request.model.waveguide.cross_section = em::WaveguideCrossSection::Circular;
        // Для круглого сечения inner_width_mm несёт внутренний диаметр.
        request.model.waveguide.inner_radius_m = mmToM(0.5 * inner_width_mm);
    }
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
    applyAccuracyLevel(request.settings.fem, parameters.accuracy_level);
    request.settings.solver_method = toEmSolverMethod(parameters.solver_method);
    // After the accuracy level, so that an explicit choice overrides the budget
    // the level asked for.
    request.settings.fem.linear_solver_method =
        toEmLinearSolverMethod(parameters.linear_solver_method);

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
        plate.aperture_enabled = plate_parameters.aperture_enabled;
        plate.aperture_shape = plate_parameters.aperture_shape == 1
                                   ? em::PlateApertureShape::Circular
                                   : em::PlateApertureShape::Rectangular;
        plate.aperture_width_m = mmToM(plate_parameters.aperture_width_mm);
        plate.aperture_height_m = mmToM(plate_parameters.aperture_height_mm);
        plate.aperture_radius_m = mmToM(plate_parameters.aperture_radius_mm);
        plate.aperture_offset_x_m = mmToM(plate_parameters.aperture_offset_x_mm);
        plate.aperture_offset_y_m = mmToM(plate_parameters.aperture_offset_y_mm);
        plate.post_enabled = plate_parameters.post_enabled;
        plate.post_width_m = mmToM(plate_parameters.post_width_mm);
        plate.post_height_m = mmToM(plate_parameters.post_height_mm);
        request.model.pec_plates.push_back(plate);
    }
    return request;
}
}

WaveguideCalculationResult WaveguideCalculator::calculate(
    const WaveguideParameters &parameters,
    const std::function<bool()> &cancellation_requested,
    const std::function<void(const QString &)> &progress_reporter,
    double arrow_density) const
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

    // У круглого сечения inner_width_mm и inner_depth_mm — внутренний диаметр
    // по обеим осям: описанный квадрат сечения. Это позволяет отрисовке и
    // постобработке пользоваться теми же границами, а площадь и объём считаются
    // по кругу.
    const bool circular = isCircularSection(parameters);
    const double inner_radius_mm = parameters.radius_mm - parameters.wall_thickness_mm;
    result.inner_width_mm = circular
                                ? 2.0 * inner_radius_mm
                                : parameters.width_mm - 2.0 * parameters.wall_thickness_mm;
    result.inner_depth_mm = circular
                                ? 2.0 * inner_radius_mm
                                : parameters.depth_mm - 2.0 * parameters.wall_thickness_mm;
    result.area_mm2 = circular ? em::pi * inner_radius_mm * inner_radius_mm
                               : result.inner_width_mm * result.inner_depth_mm;
    result.cavity_volume_mm3 = result.area_mm2 * parameters.length_mm;
    const double outer_volume_mm3 =
        circular ? em::pi * parameters.radius_mm * parameters.radius_mm * parameters.length_mm
                 : parameters.width_mm * parameters.depth_mm * parameters.length_mm;
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
        // Диагностику переносим и при неудаче: без неё по сообщению об ошибке
        // невозможно понять, на какой сетке и с каким качеством шёл расчёт.
        result.solver_backend = QString::fromStdString(field_solution->diagnostics.backend_name);
        result.mesh_tetrahedron_count = field_solution->diagnostics.mesh_tetrahedron_count;
        result.fem_unknown_count = field_solution->diagnostics.fem_unknown_count;
        result.linear_iterations = field_solution->diagnostics.linear_iterations;
        result.linear_relative_residual = field_solution->diagnostics.linear_relative_residual;
        for (const std::string &warning : field_solution->diagnostics.warnings) {
            result.solver_warnings.push_back(QString::fromStdString(warning));
        }
        return result;
    }

    result.valid = true;
    result.solver_backend = QString::fromStdString(field_solution->diagnostics.backend_name);
    // Те же четыре величины, что и в ветке неудачи выше. Без них успешный расчёт
    // не сообщал ни размера сетки, ни числа неизвестных, ни достигнутой невязки —
    // то есть именно того, по чему судят о доверии к полученным S-параметрам.
    result.mesh_tetrahedron_count = field_solution->diagnostics.mesh_tetrahedron_count;
    result.fem_unknown_count = field_solution->diagnostics.fem_unknown_count;
    result.linear_iterations = field_solution->diagnostics.linear_iterations;
    result.linear_relative_residual = field_solution->diagnostics.linear_relative_residual;
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
    postprocessing::FieldVisualizationSettings glyph_settings;
    glyph_settings.arrow_density = arrow_density;
    const QtFieldGlyphAdapter adapter;
    result.field_glyphs = adapter.build(*field_solution, glyph_settings, generation_control);
    if (cancelled()) {
        result.cancelled = true;
        result.valid = false;
        result.field_glyphs.clear();
        result.field_solution.reset();
        return result;
    }

    report(QStringLiteral("Построение заливки |E| на срезах..."));
    result.horizontal_slice = adapter.buildSlice(*field_solution,
                                                 FieldSlicePlane::HorizontalXZ,
                                                 0.0,
                                                 generation_control);
    result.vertical_slice = adapter.buildSlice(*field_solution,
                                               FieldSlicePlane::VerticalYZ,
                                               0.0,
                                               generation_control);
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
