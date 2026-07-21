#include "em/derived_fields.h"
#include "em/em_solver_dispatcher.h"
#include "em/gmsh_tetrahedral_mesher.h"
#ifdef KRUTIEV_WITH_MFEM
#include "em/mfem_frequency_domain_backend.h"
#endif
#include "em/rectangular_waveguide_solver.h"
#include "em/transverse_pec_partition_solver.h"
#include "postprocessing/field_visualization_generator.h"
#include "postprocessing/slot_excitation_estimator.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <memory>
#include <string>

namespace
{
class TestFemBackend final : public em::IFemFrequencyDomainBackend
{
public:
    em::FieldSolution solve(const em::SimulationRequest &request,
                            const em::SolveControl &) const override
    {
        em::FieldSolution solution;
        solution.request = request;
        solution.success = true;
        solution.diagnostics.backend_name = "Test FEM backend";
        return solution;
    }
};

int failure_count = 0;

void fail(const std::string &message)
{
    ++failure_count;
    std::cerr << "FAIL: " << message << '\n';
}

void expectTrue(bool condition, const std::string &message)
{
    if (!condition) {
        fail(message);
    }
}

void expectNear(double actual,
                double expected,
                double absolute_tolerance,
                const std::string &message)
{
    if (std::abs(actual - expected) > absolute_tolerance) {
        fail(message + ": actual=" + std::to_string(actual) +
             ", expected=" + std::to_string(expected));
    }
}

void expectComplexNear(const em::Complex &actual,
                       const em::Complex &expected,
                       double absolute_tolerance,
                       const std::string &message)
{
    if (std::abs(actual - expected) > absolute_tolerance) {
        fail(message + ": error=" + std::to_string(std::abs(actual - expected)));
    }
}

em::SimulationRequest createRequest(double frequency_hz)
{
    em::SimulationRequest request;
    request.frequency_hz = frequency_hz;
    request.model.waveguide.inner_width_m = 22.86e-3;
    request.model.waveguide.inner_height_m = 10.16e-3;
    request.model.waveguide.length_m = 50.0e-3;
    request.model.waveguide.wall_thickness_m = 0.1e-3;
    request.settings.maximum_m = 3;
    request.settings.maximum_n = 3;
    request.settings.normalization_power_w = 1.0;
    return request;
}

double integratePower(const em::IFieldEvaluator &field,
                      const em::RectangularWaveguideGeometry &geometry,
                      double z_m)
{
    constexpr int x_samples = 100;
    constexpr int y_samples = 72;
    const double dx_m = geometry.inner_width_m / x_samples;
    const double dy_m = geometry.inner_height_m / y_samples;
    double power_w = 0.0;

    for (int x_index = 0; x_index < x_samples; ++x_index) {
        const double x_m = -0.5 * geometry.inner_width_m +
                           (x_index + 0.5) * dx_m;
        for (int y_index = 0; y_index < y_samples; ++y_index) {
            const double y_m = -0.5 * geometry.inner_height_m +
                               (y_index + 0.5) * dy_m;
            const em::FieldPhasor sample = field.evaluate({x_m, y_m, z_m});
            power_w += em::timeAveragePoynting(sample.electric_v_per_m,
                                               sample.magnetic_a_per_m)
                           .z *
                       dx_m * dy_m;
        }
    }

    return power_w;
}

em::ComplexVec3 scaled(const em::ComplexVec3 &vector, const em::Complex &factor)
{
    return vector * factor;
}

em::ComplexVec3 numericalCurlElectric(const em::IFieldEvaluator &field,
                                      const em::Vec3 &position_m,
                                      double step_m)
{
    const auto sample = [&field](const em::Vec3 &position) {
        return field.evaluate(position).electric_v_per_m;
    };
    const em::ComplexVec3 x_plus = sample({position_m.x + step_m, position_m.y, position_m.z});
    const em::ComplexVec3 x_minus = sample({position_m.x - step_m, position_m.y, position_m.z});
    const em::ComplexVec3 y_plus = sample({position_m.x, position_m.y + step_m, position_m.z});
    const em::ComplexVec3 y_minus = sample({position_m.x, position_m.y - step_m, position_m.z});
    const em::ComplexVec3 z_plus = sample({position_m.x, position_m.y, position_m.z + step_m});
    const em::ComplexVec3 z_minus = sample({position_m.x, position_m.y, position_m.z - step_m});
    const double denominator = 2.0 * step_m;

    return {
        (y_plus.z - y_minus.z - z_plus.y + z_minus.y) / denominator,
        (z_plus.x - z_minus.x - x_plus.z + x_minus.z) / denominator,
        (x_plus.y - x_minus.y - y_plus.x + y_minus.x) / denominator,
    };
}

em::ComplexVec3 numericalCurlMagnetic(const em::IFieldEvaluator &field,
                                      const em::Vec3 &position_m,
                                      double step_m)
{
    const auto sample = [&field](const em::Vec3 &position) {
        return field.evaluate(position).magnetic_a_per_m;
    };
    const em::ComplexVec3 x_plus = sample({position_m.x + step_m, position_m.y, position_m.z});
    const em::ComplexVec3 x_minus = sample({position_m.x - step_m, position_m.y, position_m.z});
    const em::ComplexVec3 y_plus = sample({position_m.x, position_m.y + step_m, position_m.z});
    const em::ComplexVec3 y_minus = sample({position_m.x, position_m.y - step_m, position_m.z});
    const em::ComplexVec3 z_plus = sample({position_m.x, position_m.y, position_m.z + step_m});
    const em::ComplexVec3 z_minus = sample({position_m.x, position_m.y, position_m.z - step_m});
    const double denominator = 2.0 * step_m;

    return {
        (y_plus.z - y_minus.z - z_plus.y + z_minus.y) / denominator,
        (z_plus.x - z_minus.x - x_plus.z + x_minus.z) / denominator,
        (x_plus.y - x_minus.y - y_plus.x + y_minus.x) / denominator,
    };
}

void testTe10CutoffAndPower()
{
    const em::SimulationRequest request = createRequest(10.0e9);
    const em::FieldSolution solution = em::RectangularWaveguideSolver().solve(request);
    expectTrue(solution.success, "TE10 solution succeeds");
    expectTrue(solution.has_selected_mode, "TE10 is selected automatically");
    expectTrue(solution.selected_mode.family == em::ModeFamily::TransverseElectric &&
                   solution.selected_mode.m == 1 && solution.selected_mode.n == 0,
               "dominant mode is TE10");

    const double expected_cutoff_hz =
        em::speed_of_light_m_per_s / (2.0 * request.model.waveguide.inner_width_m);
    expectNear(solution.selected_mode.cutoff_frequency_hz,
               expected_cutoff_hz,
               expected_cutoff_hz * 1.0e-12,
               "TE10 cutoff frequency");

    const double k0_per_m = 2.0 * em::pi * request.frequency_hz /
                            em::speed_of_light_m_per_s;
    const double expected_beta_per_m =
        std::sqrt(k0_per_m * k0_per_m -
                  std::pow(em::pi / request.model.waveguide.inner_width_m, 2.0));
    expectNear(std::imag(solution.selected_mode.propagation_constant_per_m),
               expected_beta_per_m,
               expected_beta_per_m * 1.0e-12,
               "TE10 phase constant");
    expectNear(std::real(solution.selected_mode.propagation_constant_per_m),
               0.0,
               1.0e-12,
               "lossless TE10 attenuation");

    const double input_power_w = integratePower(*solution.field,
                                                request.model.waveguide,
                                                -0.5 * request.model.waveguide.length_m);
    expectNear(input_power_w, 1.0, 2.0e-12, "TE10 is normalized to one watt");
    expectNear(std::abs(solution.scattering.s11), 0.0, 1.0e-14, "uniform guide S11");
    expectNear(std::abs(solution.scattering.s21), 1.0, 1.0e-12, "lossless uniform guide S21");
    expectNear(std::abs(solution.scattering.s12 - solution.scattering.s21),
               0.0,
               1.0e-14,
               "uniform guide reciprocity");
}

void testPecBoundaryCondition()
{
    const em::SimulationRequest request = createRequest(10.0e9);
    const em::FieldSolution solution = em::RectangularWaveguideSolver().solve(request);
    const double half_width_m = 0.5 * request.model.waveguide.inner_width_m;
    const double half_height_m = 0.5 * request.model.waveguide.inner_height_m;
    const double z_m = -0.013;

    for (double y_ratio : {-0.73, -0.21, 0.34, 0.81}) {
        for (double x_m : {-half_width_m, half_width_m}) {
            const em::ComplexVec3 electric =
                solution.field->evaluate({x_m, y_ratio * half_height_m, z_m})
                    .electric_v_per_m;
            expectTrue(std::abs(electric.y) < 1.0e-9 && std::abs(electric.z) < 1.0e-9,
                       "tangential electric field vanishes on x PEC wall");
        }
    }

    for (double x_ratio : {-0.82, -0.27, 0.18, 0.69}) {
        for (double y_m : {-half_height_m, half_height_m}) {
            const em::ComplexVec3 electric =
                solution.field->evaluate({x_ratio * half_width_m, y_m, z_m})
                    .electric_v_per_m;
            expectTrue(std::abs(electric.x) < 1.0e-9 && std::abs(electric.z) < 1.0e-9,
                       "tangential electric field vanishes on y PEC wall");
        }
    }
}

void testMaxwellResiduals(const em::ModeSelection &selection, double frequency_hz)
{
    em::SimulationRequest request = createRequest(frequency_hz);
    request.excitation = selection;
    const em::FieldSolution solution = em::RectangularWaveguideSolver().solve(request);
    expectTrue(solution.success && solution.has_selected_mode, "selected mode is solved");
    if (!solution.field) {
        return;
    }

    const em::Vec3 position_m{0.0031, -0.0017, -0.0063};
    const double step_m = 2.0e-7;
    const em::FieldPhasor field = solution.field->evaluate(position_m);
    const em::Complex imaginary_unit(0.0, 1.0);
    const double omega = 2.0 * em::pi * request.frequency_hz;
    const em::Complex mu = em::vacuum_permeability_h_per_m;
    const em::Complex epsilon = em::vacuum_permittivity_f_per_m;

    const em::ComplexVec3 curl_e = numericalCurlElectric(*solution.field, position_m, step_m);
    const em::ComplexVec3 curl_h = numericalCurlMagnetic(*solution.field, position_m, step_m);
    const em::ComplexVec3 faraday_term =
        scaled(field.magnetic_a_per_m, imaginary_unit * omega * mu);
    const em::ComplexVec3 ampere_term =
        scaled(field.electric_v_per_m, imaginary_unit * omega * epsilon);
    const em::ComplexVec3 faraday_residual = curl_e + faraday_term;
    const em::ComplexVec3 ampere_residual = curl_h - ampere_term;
    const double faraday_scale = std::max(em::magnitude(curl_e), em::magnitude(faraday_term));
    const double ampere_scale = std::max(em::magnitude(curl_h), em::magnitude(ampere_term));

    expectTrue(em::magnitude(faraday_residual) / faraday_scale < 2.0e-7,
               "Faraday residual is small");
    expectTrue(em::magnitude(ampere_residual) / ampere_scale < 2.0e-7,
               "Ampere residual is small");
}

void testTm11AndDerivedFields()
{
    em::ModeSelection selection;
    selection.automatic = false;
    selection.family = em::ModeFamily::TransverseMagnetic;
    selection.m = 1;
    selection.n = 1;
    em::SimulationRequest request = createRequest(25.0e9);
    request.excitation = selection;
    const em::FieldSolution solution = em::RectangularWaveguideSolver().solve(request);
    expectTrue(solution.success && solution.has_selected_mode, "TM11 solution succeeds");
    expectTrue(solution.selected_mode.propagating, "TM11 propagates at 25 GHz");
    expectNear(integratePower(*solution.field,
                              request.model.waveguide,
                              -0.5 * request.model.waveguide.length_m),
               1.0,
               2.0e-12,
               "TM11 is normalized to one watt");

    const double half_height_m = 0.5 * request.model.waveguide.inner_height_m;
    const em::FieldPhasor wall_field = solution.field->evaluate({0.002, half_height_m, -0.01});
    const em::Vec3 normal_from_metal_to_field{0.0, -1.0, 0.0};
    const em::ComplexVec3 current =
        em::surfaceCurrent(normal_from_metal_to_field, wall_field.magnetic_a_per_m);
    expectNear(std::abs(current.y), 0.0, 1.0e-12, "surface current is tangent to PEC");
}

void testBelowCutoff()
{
    const em::FieldSolution solution =
        em::RectangularWaveguideSolver().solve(createRequest(5.0e9));
    expectTrue(solution.success, "below-cutoff model remains a valid solve result");
    expectTrue(!solution.has_selected_mode, "no propagating mode is selected below cutoff");
    expectTrue(!solution.diagnostics.warnings.empty(), "below-cutoff result explains the state");
}

void testLossyMaterialAndValidation()
{
    em::SimulationRequest lossy_request = createRequest(10.0e9);
    lossy_request.model.filling_material.conductivity_s_per_m = 0.02;
    const em::FieldSolution lossy_solution =
        em::RectangularWaveguideSolver().solve(lossy_request);
    expectTrue(lossy_solution.success && lossy_solution.has_selected_mode,
               "passive lossy filling is solved");
    expectTrue(std::real(lossy_solution.selected_mode.propagation_constant_per_m) > 0.0,
               "passive lossy filling has positive attenuation");
    expectTrue(std::abs(lossy_solution.scattering.s21) < 1.0,
               "lossy uniform guide transmission is below unity");
    expectTrue(lossy_solution.diagnostics.transmitted_power_w <
                   lossy_solution.diagnostics.incident_power_w,
               "lossy uniform guide loses forward power");
    expectTrue(lossy_solution.diagnostics.dissipated_power_w > 0.0,
               "lossy uniform guide reports dissipated power");
    expectTrue(lossy_solution.diagnostics.power_balance_relative_error < 2.0e-12,
               "lossy uniform guide satisfies power balance");

    em::SimulationRequest invalid_frequency_request = createRequest(10.0e9);
    invalid_frequency_request.frequency_hz =
        std::numeric_limits<double>::quiet_NaN();
    expectTrue(!em::RectangularWaveguideSolver().solve(invalid_frequency_request).success,
               "non-finite frequency is rejected");

    em::SimulationRequest active_material_request = createRequest(10.0e9);
    active_material_request.model.filling_material.relative_permittivity =
        em::Complex(1.0, 0.01);
    expectTrue(!em::RectangularWaveguideSolver().solve(active_material_request).success,
               "active material sign is rejected by passive solver");

    em::SimulationRequest invalid_tm_request = createRequest(10.0e9);
    invalid_tm_request.excitation.automatic = false;
    invalid_tm_request.excitation.family = em::ModeFamily::TransverseMagnetic;
    invalid_tm_request.excitation.m = 1;
    invalid_tm_request.excitation.n = 0;
    expectTrue(!em::RectangularWaveguideSolver().solve(invalid_tm_request).success,
               "nonexistent TM10 mode is rejected");

    em::SimulationRequest invalid_power_request = createRequest(10.0e9);
    invalid_power_request.settings.normalization_power_w = 0.0;
    expectTrue(!em::RectangularWaveguideSolver().solve(invalid_power_request).success,
               "zero normalization power is rejected");
}

void testVisualizationPrimitives()
{
    const em::SimulationRequest request = createRequest(10.0e9);
    const em::FieldSolution solution = em::RectangularWaveguideSolver().solve(request);
    const std::vector<postprocessing::VisualizationPrimitive> primitives =
        postprocessing::FieldVisualizationGenerator().generate(solution);
    int electric_count = 0;
    int magnetic_count = 0;
    int current_count = 0;
    int poynting_count = 0;
    const double half_width_m = 0.5 * request.model.waveguide.inner_width_m;
    const double half_height_m = 0.5 * request.model.waveguide.inner_height_m;
    const double half_length_m = 0.5 * request.model.waveguide.length_m;

    for (const postprocessing::VisualizationPrimitive &primitive : primitives) {
        expectTrue(primitive.points_m.size() >= 2, "visualization primitive has geometry");
        expectTrue(primitive.normalized_magnitude >= 0.0 &&
                       primitive.normalized_magnitude <= 1.0,
                   "visualization magnitude is normalized");
        if (primitive.quantity == postprocessing::FieldQuantity::Electric) {
            ++electric_count;
            if (primitive.kind == postprocessing::PrimitiveKind::Polyline) {
                for (std::size_t index = 1; index < primitive.points_m.size(); ++index) {
                    const em::Vec3 &previous = primitive.points_m[index - 1];
                    const em::Vec3 &current = primitive.points_m[index];
                    const bool both_on_top = std::abs(previous.y - half_height_m) < 1.0e-8 &&
                                             std::abs(current.y - half_height_m) < 1.0e-8;
                    const bool both_on_bottom = std::abs(previous.y + half_height_m) < 1.0e-8 &&
                                                std::abs(current.y + half_height_m) < 1.0e-8;
                    expectTrue(!(both_on_top || both_on_bottom) ||
                                   em::magnitude(current - previous) < 1.0e-8,
                               "electric lines do not run tangentially along PEC walls");
                }
            }
        } else if (primitive.quantity == postprocessing::FieldQuantity::Magnetic) {
            ++magnetic_count;
        } else if (primitive.quantity == postprocessing::FieldQuantity::SurfaceCurrent) {
            ++current_count;
            for (const em::Vec3 &point_m : primitive.points_m) {
                const bool on_x_wall = std::abs(std::abs(point_m.x) - half_width_m) < 1.0e-9;
                const bool on_y_wall = std::abs(std::abs(point_m.y) - half_height_m) < 1.0e-9;
                expectTrue(on_x_wall || on_y_wall,
                           "surface current points remain on a PEC wall");
                expectTrue(std::abs(point_m.z) <= half_length_m + 1.0e-9,
                           "surface current remains inside waveguide length");
            }
        } else if (primitive.quantity == postprocessing::FieldQuantity::Poynting) {
            ++poynting_count;
            const em::Vec3 direction = primitive.points_m[1] - primitive.points_m[0];
            expectTrue(direction.z > 0.0,
                       "time-average Poynting arrows point in the propagation direction");
        }
    }

    expectTrue(electric_count > 0, "electric visualization is generated");
    expectTrue(magnetic_count > 0, "magnetic visualization is generated");
    expectTrue(current_count > 0, "surface-current visualization is generated");
    expectTrue(poynting_count > 0, "Poynting visualization is generated");
}

void testTe10ElectricFluxDensity()
{
    const em::SimulationRequest request = createRequest(10.0e9);
    const em::FieldSolution solution = em::RectangularWaveguideSolver().solve(request);
    expectTrue(solution.success && solution.field,
               "TE10 electric-density test has a field solution");
    if (!solution.field) {
        return;
    }

    const double half_width_m = 0.5 * request.model.waveguide.inner_width_m;
    const em::ComplexVec3 center_electric =
        solution.field->evaluate({0.0, 0.0, -0.012}).electric_v_per_m;
    const em::ComplexVec3 near_wall_electric =
        solution.field->evaluate({0.88 * half_width_m, 0.0, -0.012}).electric_v_per_m;
    expectTrue(em::magnitude(center_electric) > 4.0 * em::magnitude(near_wall_electric),
               "TE10 electric-field magnitude peaks at the center across x");

    postprocessing::FieldVisualizationSettings settings;
    settings.generate_electric = true;
    settings.generate_magnetic = false;
    settings.generate_surface_current = false;
    settings.generate_poynting = false;
    const std::vector<postprocessing::VisualizationPrimitive> primitives =
        postprocessing::FieldVisualizationGenerator().generate(solution, settings);
    int central_line_count = 0;
    int outer_line_count = 0;
    bool has_mode_arrow = false;
    double central_arrow_length_m = 0.0;
    double outer_arrow_length_m = 0.0;
    double maximum_arrow_end_y_m = 0.0;
    double minimum_arrow_center_z_m = 1.0e300;
    double maximum_arrow_center_z_m = -1.0e300;
    for (const postprocessing::VisualizationPrimitive &primitive : primitives) {
        if (primitive.quantity != postprocessing::FieldQuantity::Electric ||
            primitive.kind != postprocessing::PrimitiveKind::Arrow ||
            primitive.points_m.size() != 2) {
            continue;
        }
        const em::Vec3 center_m =
            (primitive.points_m[0] + primitive.points_m[1]) * 0.5;
        if (std::abs(center_m.y) > 0.10 * request.model.waveguide.inner_height_m) {
            continue;
        }
        minimum_arrow_center_z_m = std::min(minimum_arrow_center_z_m, center_m.z);
        maximum_arrow_center_z_m = std::max(maximum_arrow_center_z_m, center_m.z);
        const double arrow_length_m =
            em::magnitude(primitive.points_m[1] - primitive.points_m[0]);
        has_mode_arrow = has_mode_arrow || arrow_length_m > 0.0;
        maximum_arrow_end_y_m = std::max(maximum_arrow_end_y_m,
                                         std::max(std::abs(primitive.points_m[0].y),
                                                  std::abs(primitive.points_m[1].y)));
        const double x_m = center_m.x;
        if (std::abs(x_m) < 0.20 * request.model.waveguide.inner_width_m) {
            ++central_line_count;
            central_arrow_length_m += arrow_length_m;
        } else if (std::abs(x_m) > 0.32 * request.model.waveguide.inner_width_m) {
            ++outer_line_count;
            outer_arrow_length_m += arrow_length_m;
        }
    }

    expectTrue(central_line_count > 0,
               "TE10 visualization includes central electric lines");
    expectTrue(outer_line_count > 0,
               "TE10 visualization includes low-amplitude electric lines away from center");
    expectTrue(central_line_count >= 3 * outer_line_count,
               "TE10 electric-line density follows the central field maximum");
    expectTrue(has_mode_arrow,
               "TE10 electric arrows have nonzero amplitude length");
    expectTrue(maximum_arrow_end_y_m > 0.45 * request.model.waveguide.inner_height_m,
               "central TE10 electric arrows nearly reach both PEC walls");
    expectTrue(minimum_arrow_center_z_m < -0.35 * request.model.waveguide.length_m &&
                   maximum_arrow_center_z_m > 0.35 * request.model.waveguide.length_m,
               "TE10 electric arrows span the full waveguide length");
    if (central_line_count > 0 && outer_line_count > 0) {
        expectTrue(central_arrow_length_m / central_line_count >
                       2.0 * outer_arrow_length_m / outer_line_count,
                   "TE10 electric-arrow length follows the central field maximum");
    }
}

void testSlotCurrentMaskAndExcitationEstimate()
{
    em::SimulationRequest centered_request = createRequest(10.0e9);
    em::SlotGeometry centered_slot;
    centered_slot.enabled = true;
    centered_slot.wall = em::WallSurface::Top;
    centered_slot.length_m = 10.0e-3;
    centered_slot.width_m = 1.0e-3;
    centered_request.model.slot_geometries.push_back(centered_slot);
    const em::FieldSolution centered_solution =
        em::RectangularWaveguideSolver().solve(centered_request);
    const postprocessing::SlotExcitationEstimator estimator;
    expectTrue(estimator.normalizedCoupling(centered_solution, centered_slot) < 1.0e-6,
               "centered longitudinal TE10 slot has zero first-order coupling");

    em::SlotGeometry offset_slot = centered_slot;
    offset_slot.center_u_m = 0.20 * centered_request.model.waveguide.inner_width_m;
    em::SimulationRequest offset_request = createRequest(10.0e9);
    offset_request.model.slot_geometries.push_back(offset_slot);
    const em::FieldSolution offset_solution =
        em::RectangularWaveguideSolver().solve(offset_request);
    expectTrue(estimator.normalizedCoupling(offset_solution, offset_slot) > 0.10,
               "offset longitudinal slot couples to crossing surface current");

    em::SlotGeometry transverse_slot = centered_slot;
    transverse_slot.rotation_rad = 0.5 * em::pi;
    em::SimulationRequest transverse_request = createRequest(10.0e9);
    transverse_request.model.slot_geometries.push_back(transverse_slot);
    const em::FieldSolution transverse_solution =
        em::RectangularWaveguideSolver().solve(transverse_request);
    expectTrue(estimator.normalizedCoupling(transverse_solution, transverse_slot) > 0.25,
               "transverse slot couples to longitudinal surface current");

    const std::vector<postprocessing::VisualizationPrimitive> primitives =
        postprocessing::FieldVisualizationGenerator().generate(offset_solution);
    const double top_y_m = 0.5 * offset_request.model.waveguide.inner_height_m;
    for (const postprocessing::VisualizationPrimitive &primitive : primitives) {
        if (primitive.quantity != postprocessing::FieldQuantity::SurfaceCurrent) {
            continue;
        }
        for (const em::Vec3 &point_m : primitive.points_m) {
            if (std::abs(point_m.y - top_y_m) > 1.0e-9) {
                continue;
            }
            const double du_m = point_m.x - offset_slot.center_u_m;
            const double dz_m = point_m.z - offset_slot.center_z_m;
            expectTrue(std::abs(du_m) > 0.5 * offset_slot.width_m ||
                           std::abs(dz_m) > 0.5 * offset_slot.length_m,
                       "surface-current lines do not cross the slot aperture");
        }
    }
}

void testCooperativeCancellation()
{
    em::SolveControl solve_control;
    solve_control.cancellation_requested = []() { return true; };
    const em::FieldSolution cancelled_solution =
        em::RectangularWaveguideSolver().solve(createRequest(10.0e9), solve_control);
    expectTrue(cancelled_solution.cancelled, "solver reports cooperative cancellation");
    expectTrue(!cancelled_solution.success && !cancelled_solution.field,
               "cancelled solver result cannot be consumed as a field solution");

    int cancellation_poll_count = 0;
    em::SolveControl delayed_control;
    delayed_control.cancellation_requested = [&cancellation_poll_count]() {
        return ++cancellation_poll_count > 4;
    };
    const em::FieldSolution interrupted_solution =
        em::RectangularWaveguideSolver().solve(createRequest(10.0e9), delayed_control);
    expectTrue(interrupted_solution.cancelled && cancellation_poll_count > 4,
               "solver polls cancellation during computation");

    const em::FieldSolution solution =
        em::RectangularWaveguideSolver().solve(createRequest(10.0e9));
    postprocessing::GenerationControl generation_control;
    generation_control.cancellation_requested = []() { return true; };
    const std::vector<postprocessing::VisualizationPrimitive> primitives =
        postprocessing::FieldVisualizationGenerator().generate(solution,
                                                                 {},
                                                                 generation_control);
    expectTrue(primitives.empty(), "cancelled postprocessing returns no partial geometry");
}

void testSolverDispatchPolicy()
{
    const em::EmSolverDispatcher dispatcher(std::make_shared<TestFemBackend>());
    em::SimulationRequest strict_slot_request = createRequest(10.0e9);
    em::SlotGeometry slot;
    slot.enabled = true;
    slot.wall = em::WallSurface::Top;
    slot.length_m = 10.0e-3;
    slot.width_m = 1.0e-3;
    strict_slot_request.model.slot_geometries.push_back(slot);
    const em::FieldSolution strict_slot_solution =
        dispatcher.solve(strict_slot_request);
    expectTrue(strict_slot_solution.success &&
                   strict_slot_solution.diagnostics.backend_name == "Test FEM backend",
               "strict slot geometry is routed to the FEM backend");

    em::SimulationRequest background_request = strict_slot_request;
    background_request.settings.geometry_approximation_policy =
        em::GeometryApproximationPolicy::UnperturbedBackgroundForSlots;
    const em::FieldSolution background_solution =
        dispatcher.solve(background_request);
    expectTrue(background_solution.success,
               "explicit slot background-field approximation remains available");
    expectTrue(!background_solution.diagnostics.warnings.empty(),
               "slot background approximation is explicitly reported");

    em::SimulationRequest partial_plate_request = createRequest(10.0e9);
    em::PecPlateGeometry partial_plate;
    partial_plate.enabled = true;
    partial_plate.center_m = {0.0, 0.0, 0.0};
    partial_plate.size_m = {
        0.5 * partial_plate_request.model.waveguide.inner_width_m,
        partial_plate_request.model.waveguide.inner_height_m,
        1.0e-3,
    };
    partial_plate_request.model.pec_plates.push_back(partial_plate);
    const em::FieldSolution partial_plate_solution =
        dispatcher.solve(partial_plate_request);
    expectTrue(partial_plate_solution.success &&
                   partial_plate_solution.diagnostics.backend_name == "Test FEM backend",
               "partial PEC diaphragm is routed to the FEM backend");
}

void verifyFullPecPartition(const em::ModeSelection &selection,
                            double frequency_hz,
                            const std::string &mode_name)
{
    em::SimulationRequest request = createRequest(frequency_hz);
    request.excitation = selection;
    em::PecPlateGeometry plate;
    plate.enabled = true;
    plate.center_m = {0.0, 0.0, 2.0e-3};
    plate.size_m = {
        request.model.waveguide.inner_width_m,
        request.model.waveguide.inner_height_m,
        1.0e-3,
    };
    request.model.pec_plates.push_back(plate);

    const em::FieldSolution solution = em::EmSolverDispatcher().solve(request);
    expectTrue(solution.success && solution.has_selected_mode && solution.field,
               mode_name + " full PEC partition solution succeeds");
    if (!solution.field) {
        return;
    }
    expectTrue(solution.diagnostics.backend_name ==
                   "Analytic full transverse PEC partition",
               mode_name + " uses the dedicated partition backend");
    expectNear(std::abs(solution.scattering.s21),
               0.0,
               1.0e-15,
               mode_name + " full PEC partition blocks transmission");
    expectNear(std::abs(solution.scattering.s11),
               1.0,
               2.0e-12,
               mode_name + " lossless short reflects all incident power");

    const double input_port_z_m = -0.5 * request.model.waveguide.length_m;
    const double plate_face_z_m = plate.center_m.z - 0.5 * plate.size_m.z;
    const double distance_to_plate_m = plate_face_z_m - input_port_z_m;
    const em::Complex expected_s11 =
        -std::exp(-2.0 * solution.selected_mode.propagation_constant_per_m *
                  distance_to_plate_m);
    expectComplexNear(solution.scattering.s11,
                      expected_s11,
                      2.0e-12,
                      mode_name + " short-circuit reflection phase");

    double maximum_reference_electric = 0.0;
    double maximum_plate_tangential_electric = 0.0;
    for (int x_index = 1; x_index < 6; ++x_index) {
        const double x_m = -0.5 * request.model.waveguide.inner_width_m +
                           x_index * request.model.waveguide.inner_width_m / 6.0;
        for (int y_index = 1; y_index < 5; ++y_index) {
            const double y_m = -0.5 * request.model.waveguide.inner_height_m +
                               y_index * request.model.waveguide.inner_height_m / 5.0;
            const em::ComplexVec3 reference_electric =
                solution.field->evaluate({x_m, y_m, plate_face_z_m - 2.0e-3})
                    .electric_v_per_m;
            const em::ComplexVec3 plate_electric =
                solution.field->evaluate({x_m, y_m, plate_face_z_m})
                    .electric_v_per_m;
            maximum_reference_electric =
                std::max(maximum_reference_electric,
                         em::magnitude(reference_electric));
            maximum_plate_tangential_electric =
                std::max(maximum_plate_tangential_electric,
                         std::hypot(std::abs(plate_electric.x),
                                    std::abs(plate_electric.y)));
        }
    }
    expectTrue(maximum_plate_tangential_electric <=
                   std::max(1.0e-10, maximum_reference_electric * 2.0e-12),
               mode_name + " tangential E vanishes on the transverse PEC face");

    const em::FieldPhasor isolated_region_field =
        solution.field->evaluate({0.001, -0.001, plate.center_m.z + plate.size_m.z});
    expectNear(em::magnitude(isolated_region_field.electric_v_per_m),
               0.0,
               1.0e-15,
               mode_name + " port-2 region electric field is zero for port-1 excitation");
    expectNear(em::magnitude(isolated_region_field.magnetic_a_per_m),
               0.0,
               1.0e-15,
               mode_name + " port-2 region magnetic field is zero for port-1 excitation");
    expectNear(solution.diagnostics.reflected_power_w,
               solution.diagnostics.incident_power_w,
               3.0e-12,
               mode_name + " reflected power equals incident power");
    expectNear(solution.diagnostics.power_balance_relative_error,
               0.0,
               1.0e-14,
               mode_name + " partition power balance");

    postprocessing::FieldVisualizationSettings current_settings;
    current_settings.generate_electric = false;
    current_settings.generate_magnetic = false;
    current_settings.generate_surface_current = true;
    current_settings.generate_poynting = false;
    const std::vector<postprocessing::VisualizationPrimitive> current_primitives =
        postprocessing::FieldVisualizationGenerator().generate(solution,
                                                                 current_settings);
    bool has_partition_current_line = false;
    for (const postprocessing::VisualizationPrimitive &primitive : current_primitives) {
        if (primitive.quantity != postprocessing::FieldQuantity::SurfaceCurrent ||
            primitive.points_m.empty()) {
            continue;
        }
        const bool lies_on_partition =
            std::all_of(primitive.points_m.begin(),
                        primitive.points_m.end(),
                        [plate_face_z_m](const em::Vec3 &point_m) {
                            return std::abs(point_m.z - plate_face_z_m) < 1.0e-10;
                        });
        has_partition_current_line = has_partition_current_line || lies_on_partition;
    }
    expectTrue(has_partition_current_line,
               mode_name + " surface-current lines are generated on the PEC partition");

    const em::Vec3 residual_point_m{0.0023, -0.0014, plate_face_z_m - 4.0e-3};
    const double step_m = 2.0e-7;
    const em::FieldPhasor field = solution.field->evaluate(residual_point_m);
    const em::Complex imaginary_unit(0.0, 1.0);
    const double omega = 2.0 * em::pi * request.frequency_hz;
    const em::ComplexVec3 curl_e =
        numericalCurlElectric(*solution.field, residual_point_m, step_m);
    const em::ComplexVec3 curl_h =
        numericalCurlMagnetic(*solution.field, residual_point_m, step_m);
    const em::ComplexVec3 faraday_term =
        scaled(field.magnetic_a_per_m,
               imaginary_unit * omega * em::vacuum_permeability_h_per_m);
    const em::ComplexVec3 ampere_term =
        scaled(field.electric_v_per_m,
               imaginary_unit * omega * em::vacuum_permittivity_f_per_m);
    const double faraday_scale =
        std::max(em::magnitude(curl_e), em::magnitude(faraday_term));
    const double ampere_scale =
        std::max(em::magnitude(curl_h), em::magnitude(ampere_term));
    expectTrue(em::magnitude(curl_e + faraday_term) / faraday_scale < 3.0e-7,
               mode_name + " standing wave satisfies Faraday law");
    expectTrue(em::magnitude(curl_h - ampere_term) / ampere_scale < 3.0e-7,
               mode_name + " standing wave satisfies Ampere law");
}

void testFullPecPartition()
{
    em::ModeSelection te10;
    te10.automatic = false;
    te10.family = em::ModeFamily::TransverseElectric;
    te10.m = 1;
    te10.n = 0;
    verifyFullPecPartition(te10, 10.0e9, "TE10");

    em::ModeSelection tm11;
    tm11.automatic = false;
    tm11.family = em::ModeFamily::TransverseMagnetic;
    tm11.m = 1;
    tm11.n = 1;
    verifyFullPecPartition(tm11, 25.0e9, "TM11");
}

void testConductorLossAndQ()
{
    em::SimulationRequest request = createRequest(10.0e9);
    request.model.waveguide.wall_conductivity_s_per_m = 5.8e7;   // copper
    const em::FieldSolution solution = em::RectangularWaveguideSolver().solve(request);
    expectTrue(solution.success && solution.has_selected_mode, "lossy-wall TE10 solves");

    const double a = request.model.waveguide.inner_width_m;
    const double b = request.model.waveguide.inner_height_m;
    const double f = request.frequency_hz;
    const double fc = em::speed_of_light_m_per_s / (2.0 * a);
    const double omega = 2.0 * em::pi * f;
    const double eta = std::sqrt(em::vacuum_permeability_h_per_m /
                                 em::vacuum_permittivity_f_per_m);
    const double surface_resistance =
        std::sqrt(omega * em::vacuum_permeability_h_per_m / (2.0 * 5.8e7));
    const double ratio_squared = (fc / f) * (fc / f);
    const double alpha_c_reference =
        surface_resistance / (b * eta * std::sqrt(1.0 - ratio_squared)) *
        (1.0 + (2.0 * b / a) * ratio_squared);

    const double alpha_c = solution.diagnostics.conductor_attenuation_np_per_m;
    expectTrue(alpha_c > 0.0, "copper walls give positive conductor attenuation");
    expectNear(alpha_c, alpha_c_reference, 0.06 * alpha_c_reference,
               "conductor attenuation matches the Pozar TE10 closed form");

    const double alpha_total =
        std::real(solution.selected_mode.propagation_constant_per_m);
    const double expected_s21 = std::exp(-alpha_total * request.model.waveguide.length_m);
    expectTrue(std::abs(solution.scattering.s21) < 1.0,
               "lossy walls attenuate the transmitted wave");
    expectNear(std::abs(solution.scattering.s21), expected_s21, 1.0e-4,
               "transmission magnitude follows exp(-alpha * L)");
    expectTrue(solution.diagnostics.dissipated_power_w > 0.0,
               "lossy walls dissipate power");

    expectTrue(solution.diagnostics.stored_electric_energy_j > 0.0 &&
                   solution.diagnostics.stored_magnetic_energy_j > 0.0,
               "stored electric and magnetic energies are positive");
    const double quality_factor = solution.diagnostics.quality_factor;
    expectTrue(quality_factor > 1000.0 && quality_factor < 50000.0,
               "conductor-loss Q is finite and physically sized: " +
                   std::to_string(quality_factor));

    const em::FieldSolution lossless =
        em::RectangularWaveguideSolver().solve(createRequest(10.0e9));
    expectNear(lossless.diagnostics.conductor_attenuation_np_per_m, 0.0, 1.0e-15,
               "perfect walls report zero conductor attenuation");
    expectNear(std::abs(lossless.scattering.s21), 1.0, 1.0e-12,
               "perfect walls keep |S21| = 1");
    expectTrue(lossless.diagnostics.quality_factor == 0.0,
               "lossless guide reports Q = 0 as the 'infinite' sentinel");
}

void testFieldSliceAndAnimation()
{
    const em::SimulationRequest request = createRequest(10.0e9);
    const em::FieldSolution solution = em::RectangularWaveguideSolver().solve(request);
    const postprocessing::FieldVisualizationGenerator generator;
    const postprocessing::FieldSliceData slice =
        generator.generateSlice(solution, postprocessing::SlicePlaneKind::HorizontalXZ);
    expectTrue(slice.valid && !slice.cells.empty() && slice.maximum_value > 0.0,
               "horizontal |E| slice is generated");

    const double half_width_m = 0.5 * request.model.waveguide.inner_width_m;
    double centre_sum = 0.0;
    double edge_sum = 0.0;
    int centre_count = 0;
    int edge_count = 0;
    for (const postprocessing::SliceSampleCell &cell : slice.cells) {
        if (std::abs(cell.center_m.x) < 0.12 * half_width_m) {
            centre_sum += cell.envelope;
            ++centre_count;
        } else if (std::abs(cell.center_m.x) > 0.85 * half_width_m) {
            edge_sum += cell.envelope;
            ++edge_count;
        }
    }
    expectTrue(centre_count > 0 && edge_count > 0 &&
                   centre_sum / centre_count >
                       4.0 * edge_sum / std::max(1, edge_count),
               "TE10 slice envelope peaks at the guide centre");

    const std::vector<postprocessing::VisualizationPrimitive> primitives =
        generator.generate(solution);
    bool animated_electric = false;
    bool animated_magnetic = false;
    for (const postprocessing::VisualizationPrimitive &primitive : primitives) {
        if (!primitive.animated || !(primitive.reference_magnitude > 0.0)) {
            continue;
        }
        if (primitive.quantity == postprocessing::FieldQuantity::Electric) {
            animated_electric = true;
        } else if (primitive.quantity == postprocessing::FieldQuantity::Magnetic) {
            animated_magnetic = true;
        }
    }
    expectTrue(animated_electric, "TE10 electric arrows carry a non-zero animation phasor");
    expectTrue(animated_magnetic,
               "magnetic arrows carry an animation phasor so H oscillates like E");
}

void testPartitionSliceShadowAndPoynting()
{
    em::ModeSelection te10;
    te10.automatic = false;
    te10.family = em::ModeFamily::TransverseElectric;
    te10.m = 1;
    te10.n = 0;
    em::SimulationRequest request = createRequest(10.0e9);
    request.excitation = te10;
    em::PecPlateGeometry plate;
    plate.enabled = true;
    plate.center_m = {0.0, 0.0, 2.0e-3};
    plate.size_m = {
        request.model.waveguide.inner_width_m,
        request.model.waveguide.inner_height_m,
        1.0e-3,
    };
    request.model.pec_plates.push_back(plate);
    const em::FieldSolution solution = em::EmSolverDispatcher().solve(request);
    expectTrue(solution.success && solution.field, "full partition solves for slice test");
    if (!solution.field) {
        return;
    }

    const postprocessing::FieldVisualizationGenerator generator;
    const postprocessing::FieldSliceData slice =
        generator.generateSlice(solution, postprocessing::SlicePlaneKind::HorizontalXZ);
    const double plate_back_z_m = plate.center_m.z + 0.5 * plate.size_m.z;
    double upstream_sum = 0.0;
    int upstream_count = 0;
    double downstream_maximum = 0.0;
    for (const postprocessing::SliceSampleCell &cell : slice.cells) {
        if (cell.center_m.z < -0.010) {
            upstream_sum += cell.envelope;
            ++upstream_count;
        }
        if (cell.center_m.z > plate_back_z_m + 2.0e-3) {
            downstream_maximum = std::max(downstream_maximum, cell.envelope);
        }
    }
    expectTrue(upstream_count > 0 && upstream_sum > 0.0,
               "upstream slice carries the standing-wave field");
    expectNear(downstream_maximum, 0.0, 1.0e-9,
               "the slice behind a full short is dark (shadow region)");

    postprocessing::FieldVisualizationSettings settings;
    settings.generate_electric = false;
    settings.generate_magnetic = false;
    settings.generate_surface_current = false;
    settings.generate_poynting = true;
    const std::vector<postprocessing::VisualizationPrimitive> poynting =
        generator.generate(solution, settings);
    int poynting_count = 0;
    for (const postprocessing::VisualizationPrimitive &primitive : poynting) {
        if (primitive.quantity == postprocessing::FieldQuantity::Poynting) {
            ++poynting_count;
        }
    }
    expectTrue(poynting_count == 0,
               "lossless standing wave produces no Poynting arrows (noise suppressed)");
}

void testIrisPlateWithAperture()
{
    em::ModeSelection te10;
    te10.automatic = false;
    te10.family = em::ModeFamily::TransverseElectric;
    te10.m = 1;
    te10.n = 0;
    em::SimulationRequest request = createRequest(10.0e9);
    request.excitation = te10;

    // A plate spanning the whole cross-section, but with a window in it.
    em::PecPlateGeometry iris;
    iris.enabled = true;
    iris.center_m = {0.0, 0.0, 2.0e-3};
    iris.size_m = {
        request.model.waveguide.inner_width_m,
        request.model.waveguide.inner_height_m,
        1.0e-3,
    };
    iris.aperture_enabled = true;
    iris.aperture_width_m = 0.5 * request.model.waveguide.inner_width_m;
    iris.aperture_height_m = 0.5 * request.model.waveguide.inner_height_m;
    request.model.pec_plates.push_back(iris);

    // The closed-form short circuit must refuse an iris: it transmits.
    std::string reason;
    expectTrue(!em::TransversePecPartitionSolver::canSolve(request, nullptr, &reason),
               "an apertured plate is not treated as a solid short circuit");
    expectTrue(reason.find("aperture") != std::string::npos ||
                   reason.find("iris") != std::string::npos,
               "the refusal explains that the plate is an iris: " + reason);

    // Without the window the very same plate is a valid analytic short.
    em::SimulationRequest solid_request = request;
    solid_request.model.pec_plates.front().aperture_enabled = false;
    expectTrue(em::TransversePecPartitionSolver::canSolve(solid_request),
               "the same plate without a window still solves as a short circuit");

    // The iris must reach the FEM backend through the dispatcher.
    const em::EmSolverDispatcher dispatcher(std::make_shared<TestFemBackend>());
    const em::FieldSolution solution = dispatcher.solve(request);
    expectTrue(solution.diagnostics.backend_name == "Test FEM backend",
               "an iris is routed to the FEM backend");

    // The mesh script must cut the window out of the plate before subtracting
    // the metal from the fluid.
    const std::string script = em::GmshTetrahedralMesher::buildGeometryScript(request);
    expectTrue(script.find("iris100[] = BooleanDifference") != std::string::npos,
               "the window is cut out of the plate body");
    expectTrue(script.find("pecBodies[] += iris100[];") != std::string::npos,
               "the perforated plate is the PEC body handed to the fluid subtraction");
    expectTrue(script.find("Volume{pecBodies[]}") != std::string::npos,
               "the fluid subtracts the assembled PEC bodies");
}

void testCircularIrisWithPost()
{
    em::ModeSelection te10;
    te10.automatic = false;
    te10.family = em::ModeFamily::TransverseElectric;
    te10.m = 1;
    te10.n = 0;
    em::SimulationRequest request = createRequest(10.0e9);
    request.excitation = te10;

    em::PecPlateGeometry iris;
    iris.enabled = true;
    iris.center_m = {0.0, 0.0, 0.0};
    iris.size_m = {
        request.model.waveguide.inner_width_m,
        request.model.waveguide.inner_height_m,
        0.5e-3,
    };
    iris.aperture_enabled = true;
    iris.aperture_shape = em::PlateApertureShape::Circular;
    iris.aperture_radius_m = 3.0e-3;
    iris.post_enabled = true;
    iris.post_width_m = 1.5e-3;
    // From the plate bottom edge up to the centre of the hole.
    iris.post_height_m = 0.5 * request.model.waveguide.inner_height_m;
    request.model.pec_plates.push_back(iris);

    expectTrue(em::plateHasOpening(iris) && em::plateHasPost(iris),
               "circular aperture and post are recognised");
    std::string reason;
    expectTrue(!em::TransversePecPartitionSolver::canSolve(request, nullptr, &reason),
               "a circular iris is not treated as a solid short circuit");

    const em::EmSolverDispatcher dispatcher(std::make_shared<TestFemBackend>());
    expectTrue(dispatcher.solve(request).diagnostics.backend_name == "Test FEM backend",
               "a circular iris with a post is routed to the FEM backend");

    const std::string script = em::GmshTetrahedralMesher::buildGeometryScript(request);
    expectTrue(script.find("Cylinder(") != std::string::npos,
               "the round hole is cut with a cylinder");
    expectTrue(script.find("iris100[] = BooleanDifference") != std::string::npos,
               "the round hole is cut out of the plate");
    // The stub lies in the plate plane and is welded back onto the plate, so it
    // is a union with the perforated body rather than a separate PEC volume.
    expectTrue(script.find("irisStub100[] = BooleanUnion") != std::string::npos,
               "the stub is welded onto the perforated plate");
    expectTrue(script.find("pecBodies[] += irisStub100[];") != std::string::npos,
               "the plate with its stub is the PEC body subtracted from the fluid");

    // The stub is metal: a point on it is not part of the opening.
    const double bottom_m = -0.5 * iris.size_m.y;
    expectTrue(em::insidePlateStub(iris, 0.0, bottom_m + 0.5 * iris.post_height_m),
               "a point on the stub is recognised as metal");
    expectTrue(!em::insidePlateStub(iris, 0.0, 0.5 * iris.size_m.y - 1.0e-4),
               "a point above the stub is not metal");
}

void testFemGeometryGeneration()
{
    em::SimulationRequest request = createRequest(10.0e9);
    em::SlotGeometry slot;
    slot.enabled = true;
    slot.wall = em::WallSurface::Top;
    slot.center_z_m = 25.0e-3;
    slot.length_m = 12.0e-3;
    slot.width_m = 1.0e-3;
    request.model.slot_geometries.push_back(slot);

    em::PecPlateGeometry plate;
    plate.enabled = true;
    plate.center_m = {0.0, 0.0, 35.0e-3};
    plate.size_m = {5.0e-3, 4.0e-3, 0.5e-3};
    request.model.pec_plates.push_back(plate);

    em::DielectricBlockGeometry dielectric;
    dielectric.enabled = true;
    dielectric.center_m = {0.0, 0.0, 15.0e-3};
    dielectric.size_m = {4.0e-3, 3.0e-3, 2.0e-3};
    dielectric.material.relative_permittivity = 2.2;
    request.model.dielectric_blocks.push_back(dielectric);

    const std::string script = em::GmshTetrahedralMesher::buildGeometryScript(request);
    expectTrue(script.find("BooleanDifference") != std::string::npos,
               "FEM geometry subtracts PEC bodies and perforated walls");
    expectTrue(script.find("BooleanFragments") != std::string::npos,
               "FEM geometry conforms the mesh at dielectric interfaces");
    expectTrue(script.find("Physical Surface(101)") != std::string::npos &&
                   script.find("Physical Surface(102)") != std::string::npos,
               "FEM geometry tags both wave ports");

#ifdef _MSC_VER
    char *gmsh_path = nullptr;
    std::size_t gmsh_path_length = 0;
    if (_dupenv_s(&gmsh_path, &gmsh_path_length, "GMSH_EXECUTABLE") == 0 && gmsh_path) {
        const std::filesystem::path output_directory =
            std::filesystem::temp_directory_path() / "krutiev_fem_mesh_test";
        const em::FemMeshFiles files =
            em::GmshTetrahedralMesher(gmsh_path).generate(request, output_directory);
        expectTrue(files.error_message.empty(), "Gmsh accepts generated FEM geometry");
        expectTrue(std::filesystem::exists(files.mesh_path),
                   "Gmsh creates the tetrahedral FEM mesh");
        std::free(gmsh_path);
    }
#endif
}

#ifdef KRUTIEV_WITH_MFEM
void testMfemEmptyGuide()
{
    em::SimulationRequest request = createRequest(10.0e9);
    request.excitation.automatic = false;
    request.excitation.family = em::ModeFamily::TransverseElectric;
    request.excitation.m = 1;
    request.excitation.n = 0;
    request.settings.fem.mesh.maximum_element_size_m = 4.0e-3;
    request.settings.fem.relative_tolerance = 1.0e-5;
    request.settings.fem.maximum_iterations = 800;

    int progress_report_count = 0;
    bool has_gmres_stage = false;
    em::SolveControl progress_control;
    progress_control.progress_reporter = [&](const std::string &stage) {
        ++progress_report_count;
        has_gmres_stage = has_gmres_stage || stage.find("GMRES") != std::string::npos;
    };
    const em::FieldSolution solution =
        em::MfemFrequencyDomainBackend().solve(request, progress_control);
    expectTrue(solution.success && solution.field,
               "MFEM solves a driven empty WR-90 guide");
    expectTrue(progress_report_count > 0 && has_gmres_stage,
               "FEM backend reports solver progress stages");
    if (!solution.field) {
        return;
    }
    const double center_amplitude =
        em::magnitude(solution.field->evaluate({0.0, 0.0, 0.0}).electric_v_per_m);
    const double side_amplitude = em::magnitude(
        solution.field->evaluate({0.49 * request.model.waveguide.inner_width_m,
                                  0.0,
                                  0.0})
            .electric_v_per_m);
    expectTrue(center_amplitude > 4.0 * side_amplitude,
               "MFEM TE10 electric field has its antinode at the guide center: center=" +
                   std::to_string(center_amplitude) +
                   ", side=" + std::to_string(side_amplitude));
    expectTrue(std::abs(solution.scattering.s11) < 0.12,
               "empty-guide FEM port has low reflected TE10 amplitude");
    expectTrue(std::abs(std::abs(solution.scattering.s21) - 1.0) < 0.12,
               "empty-guide FEM port transmits unit TE10 amplitude");
}

void testMfemSlotFringing()
{
    char *run_test = nullptr;
    std::size_t value_length = 0;
    _dupenv_s(&run_test, &value_length, "KRUTIEV_RUN_FEM_SLOT_TEST");
    const bool enabled = run_test && std::string(run_test) == "1";
    std::free(run_test);
    if (!enabled) {
        return;
    }

    em::SimulationRequest request = createRequest(10.0e9);
    request.model.waveguide.length_m = 30.0e-3;
    request.model.waveguide.wall_thickness_m = 0.5e-3;
    request.excitation.automatic = false;
    request.excitation.family = em::ModeFamily::TransverseElectric;
    request.excitation.m = 1;
    request.excitation.n = 0;
    request.settings.fem.mesh.maximum_element_size_m = 3.5e-3;
    request.settings.fem.relative_tolerance = 2.0e-4;
    request.settings.fem.maximum_iterations = 1200;
    request.settings.fem.pml.target_reflection = 1.0e-5;
    em::SlotGeometry slot;
    slot.enabled = true;
    slot.wall = em::WallSurface::Top;
    slot.center_z_m = 0.0;
    slot.length_m = 8.0e-3;
    slot.width_m = 1.5e-3;
    request.model.slot_geometries.push_back(slot);

    const em::FieldSolution solution = em::MfemFrequencyDomainBackend().solve(request, {});
    expectTrue(solution.success && solution.field,
               "MFEM solves a slotted guide with an exterior PML domain");
    if (!solution.field) {
        return;
    }
    const em::Vec3 exterior_point{
        0.0,
        0.5 * request.model.waveguide.inner_height_m +
            request.model.waveguide.wall_thickness_m + 0.5e-3,
        slot.center_z_m,
    };
    const em::FieldPhasor exterior_field = solution.field->evaluate(exterior_point);
    expectTrue(solution.field->contains(exterior_point) &&
                   em::magnitude(exterior_field.electric_v_per_m) > 1.0e-9,
               "slot aperture produces a nonzero exterior fringing field");
    const double reflected_fraction = std::norm(solution.scattering.s11);
    const double transmitted_fraction = std::norm(solution.scattering.s21);
    expectTrue(reflected_fraction + transmitted_fraction <= 1.0 + 0.02,
               "slot radiation keeps the passive power balance (PML absorbs)");
}

void testMfemCenteredPecPost()
{
    char *run_test = nullptr;
    std::size_t value_length = 0;
    _dupenv_s(&run_test, &value_length, "KRUTIEV_RUN_FEM_PLATE_TEST");
    const bool enabled = run_test && std::string(run_test) == "1";
    std::free(run_test);
    if (!enabled) {
        return;
    }

    em::SimulationRequest request = createRequest(10.0e9);
    request.settings.fem.mesh.maximum_element_size_m = 2.0e-3;
    request.settings.fem.relative_tolerance = 1.0e-6;
    request.settings.fem.maximum_iterations = 4000;
    em::PecPlateGeometry post;
    post.enabled = true;
    post.center_m = {0.0, 0.0, 0.0};
    post.size_m = {0.5e-3,
                   request.model.waveguide.inner_height_m,
                   0.5e-3};
    request.model.pec_plates.push_back(post);

    const em::FieldSolution solution = em::MfemFrequencyDomainBackend().solve(request, {});
    expectTrue(solution.success && solution.field &&
                   std::isfinite(solution.diagnostics.linear_relative_residual),
               "MFEM converges for a centered PEC post joining opposite walls");
}
#endif
}

int main()
{
    testTe10CutoffAndPower();
    testPecBoundaryCondition();

    em::ModeSelection te10;
    te10.automatic = false;
    te10.family = em::ModeFamily::TransverseElectric;
    te10.m = 1;
    te10.n = 0;
    testMaxwellResiduals(te10, 10.0e9);

    em::ModeSelection tm11;
    tm11.automatic = false;
    tm11.family = em::ModeFamily::TransverseMagnetic;
    tm11.m = 1;
    tm11.n = 1;
    testMaxwellResiduals(tm11, 25.0e9);
    testTm11AndDerivedFields();
    testBelowCutoff();
    testLossyMaterialAndValidation();
    testConductorLossAndQ();
    testVisualizationPrimitives();
    testTe10ElectricFluxDensity();
    testSlotCurrentMaskAndExcitationEstimate();
    testCooperativeCancellation();
    testSolverDispatchPolicy();
    testFullPecPartition();
    testFieldSliceAndAnimation();
    testPartitionSliceShadowAndPoynting();
    testIrisPlateWithAperture();
    testCircularIrisWithPost();
    testFemGeometryGeneration();
#ifdef KRUTIEV_WITH_MFEM
    testMfemEmptyGuide();
    testMfemSlotFringing();
    testMfemCenteredPecPost();
#endif

    if (failure_count == 0) {
        std::cout << "All EM core tests passed.\n";
        return EXIT_SUCCESS;
    }

    std::cerr << failure_count << " EM core test(s) failed.\n";
    return EXIT_FAILURE;
}
