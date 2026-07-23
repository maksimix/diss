#include "em/derived_fields.h"
#include "em/em_solver_dispatcher.h"
#include "em/gmsh_tetrahedral_mesher.h"
#ifdef KRUTIEV_WITH_MFEM
#include "em/mfem_frequency_domain_backend.h"
#endif
#include "em/mode_matching_iris_solver.h"
#include "em/analytic_waveguide_solver.h"
#include "em/cylindrical_bessel.h"
#include "em/transverse_pec_partition_solver.h"
#include "postprocessing/field_visualization_generator.h"
#include "postprocessing/slot_excitation_estimator.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

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

// std::to_string prints a 1e-14 residual as "0.000000", which is precisely the
// number a failing message has to show.
std::string formatNumber(double value)
{
    std::ostringstream text;
    text << value;
    return text.str();
}

// Wall-clock cost of one test. A single default (second-order elements, which
// this file cannot pin without stopping to test them) turned this suite from
// 3 s into minutes, and a total alone does not say which test grew. Anything
// under a second stays silent: most of the tests here are closed-form and would
// bury the handful of runs that actually cost something.
template <typename Test>
void runTest(const std::string &name, Test test)
{
    const auto start = std::chrono::steady_clock::now();
    test();
    const double elapsed_s =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    if (elapsed_s >= 1.0) {
        std::cout << "  " << name << ": " << formatNumber(elapsed_s) << " s\n";
    }
}

// The two FEM tests below factorize systems of tens of thousands of unknowns,
// and an unoptimized Eigen is not a little slower at that but about a hundred
// times slower: the three-mesh passivity sweep costs 26 s in Release and just
// under an hour in Debug, and the default-configuration solve would take some
// four hours. So the size of those two runs follows the build. The Release run
// is the guard - the defect it protects against was invisible on small systems -
// and the Debug run is there to catch what only Debug catches, an assert inside
// the library, which the same code path trips at any size. Every test prints the
// size it actually ran, so neither run can be mistaken for the other.
constexpr bool build_is_optimized =
#ifdef NDEBUG
    true;
#else
    false;
#endif

bool hasWarningContaining(const em::FieldSolution &solution, const std::string &fragment)
{
    return std::any_of(solution.diagnostics.warnings.begin(),
                       solution.diagnostics.warnings.end(),
                       [&fragment](const std::string &warning) {
                           return warning.find(fragment) != std::string::npos;
                       });
}

// Cell sizes the geometry script asks for inside its local refinement boxes,
// one per Field[Box]. This is the whole observable content of the refinement:
// how many details were registered and how fine each one wants to be.
std::vector<double> refinementCellSizes(const std::string &script)
{
    std::vector<double> sizes_m;
    const std::string key = "].VIn = ";
    for (std::size_t at = script.find(key); at != std::string::npos;
         at = script.find(key, at + key.size())) {
        sizes_m.push_back(std::strtod(script.c_str() + at + key.size(), nullptr));
    }
    std::sort(sizes_m.begin(), sizes_m.end());
    return sizes_m;
}

bool hasRefinementCellSize(const std::string &script, double expected_size_m)
{
    const std::vector<double> sizes_m = refinementCellSizes(script);
    return std::any_of(sizes_m.begin(),
                       sizes_m.end(),
                       [expected_size_m](double size_m) {
                           // The script carries the size as text with the
                           // default six significant digits.
                           return std::abs(size_m - expected_size_m) <=
                                  1.0e-5 * expected_size_m;
                       });
}

std::string describeCellSizes(const std::string &script)
{
    const std::vector<double> sizes_m = refinementCellSizes(script);
    std::ostringstream text;
    text << sizes_m.size() << " boxes {";
    for (std::size_t index = 0; index < sizes_m.size(); ++index) {
        text << (index == 0 ? "" : ", ") << sizes_m[index] * 1.0e3;
    }
    text << "} mm";
    return text.str();
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
                      const em::WaveguideGeometry &geometry,
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
    const em::FieldSolution solution = em::AnalyticWaveguideSolver().solve(request);
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
    const em::FieldSolution solution = em::AnalyticWaveguideSolver().solve(request);
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
    const em::FieldSolution solution = em::AnalyticWaveguideSolver().solve(request);
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
    const em::FieldSolution solution = em::AnalyticWaveguideSolver().solve(request);
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
        em::AnalyticWaveguideSolver().solve(createRequest(5.0e9));
    expectTrue(solution.success, "below-cutoff model remains a valid solve result");
    expectTrue(!solution.has_selected_mode, "no propagating mode is selected below cutoff");
    expectTrue(!solution.diagnostics.warnings.empty(), "below-cutoff result explains the state");
}

void testLossyMaterialAndValidation()
{
    em::SimulationRequest lossy_request = createRequest(10.0e9);
    lossy_request.model.filling_material.conductivity_s_per_m = 0.02;
    const em::FieldSolution lossy_solution =
        em::AnalyticWaveguideSolver().solve(lossy_request);
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
    expectTrue(!em::AnalyticWaveguideSolver().solve(invalid_frequency_request).success,
               "non-finite frequency is rejected");

    em::SimulationRequest active_material_request = createRequest(10.0e9);
    active_material_request.model.filling_material.relative_permittivity =
        em::Complex(1.0, 0.01);
    expectTrue(!em::AnalyticWaveguideSolver().solve(active_material_request).success,
               "active material sign is rejected by passive solver");

    em::SimulationRequest invalid_tm_request = createRequest(10.0e9);
    invalid_tm_request.excitation.automatic = false;
    invalid_tm_request.excitation.family = em::ModeFamily::TransverseMagnetic;
    invalid_tm_request.excitation.m = 1;
    invalid_tm_request.excitation.n = 0;
    expectTrue(!em::AnalyticWaveguideSolver().solve(invalid_tm_request).success,
               "nonexistent TM10 mode is rejected");

    em::SimulationRequest invalid_power_request = createRequest(10.0e9);
    invalid_power_request.settings.normalization_power_w = 0.0;
    expectTrue(!em::AnalyticWaveguideSolver().solve(invalid_power_request).success,
               "zero normalization power is rejected");
}

void testVisualizationPrimitives()
{
    const em::SimulationRequest request = createRequest(10.0e9);
    const em::FieldSolution solution = em::AnalyticWaveguideSolver().solve(request);
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
    const em::FieldSolution solution = em::AnalyticWaveguideSolver().solve(request);
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

// Концентрация стрелок — пользовательская настройка: больше плотность — больше
// стрелок E, H и J. Линии поля и стрелки Пойнтинга настройка не трогает.
void testArrowDensitySetting()
{
    const em::SimulationRequest request = createRequest(10.0e9);
    const em::FieldSolution solution = em::AnalyticWaveguideSolver().solve(request);
    expectTrue(solution.success && solution.has_selected_mode,
               "arrow-density test has a field solution");

    const auto census = [](const std::vector<postprocessing::VisualizationPrimitive> &primitives,
                           postprocessing::FieldQuantity quantity,
                           postprocessing::PrimitiveKind kind) {
        int count = 0;
        for (const postprocessing::VisualizationPrimitive &primitive : primitives) {
            if (primitive.quantity == quantity && primitive.kind == kind) {
                ++count;
            }
        }
        return count;
    };

    const postprocessing::FieldVisualizationGenerator generator;
    postprocessing::FieldVisualizationSettings sparse;
    sparse.arrow_density = 0.25;
    postprocessing::FieldVisualizationSettings dense;
    dense.arrow_density = 4.0;
    const std::vector<postprocessing::VisualizationPrimitive> sparse_primitives =
        generator.generate(solution, sparse);
    const std::vector<postprocessing::VisualizationPrimitive> normal_primitives =
        generator.generate(solution);
    const std::vector<postprocessing::VisualizationPrimitive> dense_primitives =
        generator.generate(solution, dense);

    using postprocessing::FieldQuantity;
    using postprocessing::PrimitiveKind;
    const FieldQuantity arrow_quantities[] = {FieldQuantity::Electric,
                                              FieldQuantity::Magnetic,
                                              FieldQuantity::SurfaceCurrent};
    for (const FieldQuantity quantity : arrow_quantities) {
        const int sparse_count = census(sparse_primitives, quantity, PrimitiveKind::Arrow);
        const int normal_count = census(normal_primitives, quantity, PrimitiveKind::Arrow);
        const int dense_count = census(dense_primitives, quantity, PrimitiveKind::Arrow);
        expectTrue(sparse_count > 0, "quarter density keeps some arrows");
        expectTrue(sparse_count < normal_count, "quarter density thins the arrows");
        expectTrue(normal_count < dense_count, "quadruple density adds arrows");
    }

    expectTrue(census(sparse_primitives, FieldQuantity::Poynting, PrimitiveKind::Arrow) ==
                   census(dense_primitives, FieldQuantity::Poynting, PrimitiveKind::Arrow),
               "Poynting arrows ignore the arrow-density setting");
    expectTrue(census(sparse_primitives, FieldQuantity::Magnetic, PrimitiveKind::Polyline) ==
                   census(dense_primitives, FieldQuantity::Magnetic, PrimitiveKind::Polyline),
               "field lines ignore the arrow-density setting");
}

// Срез |E| умеет вставать на смещённую плоскость (перенос среза в интерфейсе),
// а огрубление сетки для стопки объёмной заливки действительно уменьшает
// число клеток.
void testSliceOffsetAndResolution()
{
    const em::SimulationRequest request = createRequest(10.0e9);
    const em::FieldSolution solution = em::AnalyticWaveguideSolver().solve(request);
    const postprocessing::FieldVisualizationGenerator generator;

    const postprocessing::FieldSliceData centered =
        generator.generateSlice(solution, postprocessing::SlicePlaneKind::VerticalYZ);
    const double offset_fraction = 0.30;
    const postprocessing::FieldSliceData shifted =
        generator.generateSlice(solution,
                                postprocessing::SlicePlaneKind::VerticalYZ,
                                offset_fraction);
    expectTrue(centered.valid && shifted.valid, "offset-slice test slices are valid");

    const double expected_x_m = offset_fraction * request.model.waveguide.inner_width_m;
    bool on_shifted_plane = !shifted.cells.empty();
    for (const postprocessing::SliceSampleCell &cell : shifted.cells) {
        on_shifted_plane =
            on_shifted_plane && std::abs(cell.center_m.x - expected_x_m) < 1.0e-9;
    }
    expectTrue(on_shifted_plane, "shifted vertical slice lies on the x = offset plane");

    // TE10: |E| спадает от середины как sin(pi x'/a); на x = 0.3a от центра
    // остаётся sin(0.8 pi) = 0.59 от максимума.
    expectTrue(shifted.maximum_value < 0.75 * centered.maximum_value,
               "TE10 |E| maximum drops away from the guide centre");
    expectTrue(shifted.maximum_value > 0.25 * centered.maximum_value,
               "shifted slice still sees a finite field");

    const postprocessing::FieldSliceData coarse =
        generator.generateSlice(solution,
                                postprocessing::SlicePlaneKind::HorizontalXZ,
                                0.0,
                                0.55);
    const postprocessing::FieldSliceData fine =
        generator.generateSlice(solution, postprocessing::SlicePlaneKind::HorizontalXZ);
    expectTrue(coarse.valid && fine.valid &&
                   coarse.cells.size() < fine.cells.size() / 2,
               "volume-stack resolution scale really coarsens the slice grid");
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
        em::AnalyticWaveguideSolver().solve(centered_request);
    const postprocessing::SlotExcitationEstimator estimator;
    expectTrue(estimator.normalizedCoupling(centered_solution, centered_slot) < 1.0e-6,
               "centered longitudinal TE10 slot has zero first-order coupling");

    em::SlotGeometry offset_slot = centered_slot;
    offset_slot.center_u_m = 0.20 * centered_request.model.waveguide.inner_width_m;
    em::SimulationRequest offset_request = createRequest(10.0e9);
    offset_request.model.slot_geometries.push_back(offset_slot);
    const em::FieldSolution offset_solution =
        em::AnalyticWaveguideSolver().solve(offset_request);
    expectTrue(estimator.normalizedCoupling(offset_solution, offset_slot) > 0.10,
               "offset longitudinal slot couples to crossing surface current");

    em::SlotGeometry transverse_slot = centered_slot;
    transverse_slot.rotation_rad = 0.5 * em::pi;
    em::SimulationRequest transverse_request = createRequest(10.0e9);
    transverse_request.model.slot_geometries.push_back(transverse_slot);
    const em::FieldSolution transverse_solution =
        em::AnalyticWaveguideSolver().solve(transverse_request);
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
        em::AnalyticWaveguideSolver().solve(createRequest(10.0e9), solve_control);
    expectTrue(cancelled_solution.cancelled, "solver reports cooperative cancellation");
    expectTrue(!cancelled_solution.success && !cancelled_solution.field,
               "cancelled solver result cannot be consumed as a field solution");

    int cancellation_poll_count = 0;
    em::SolveControl delayed_control;
    delayed_control.cancellation_requested = [&cancellation_poll_count]() {
        return ++cancellation_poll_count > 4;
    };
    const em::FieldSolution interrupted_solution =
        em::AnalyticWaveguideSolver().solve(createRequest(10.0e9), delayed_control);
    expectTrue(interrupted_solution.cancelled && cancellation_poll_count > 4,
               "solver polls cancellation during computation");

    const em::FieldSolution solution =
        em::AnalyticWaveguideSolver().solve(createRequest(10.0e9));
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
    const em::FieldSolution solution = em::AnalyticWaveguideSolver().solve(request);
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
        em::AnalyticWaveguideSolver().solve(createRequest(10.0e9));
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
    const em::FieldSolution solution = em::AnalyticWaveguideSolver().solve(request);
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

    // Animated arrows must stay on one axis: they reverse and change length with
    // the phase, but never sweep around. H is elliptically polarised in a
    // travelling wave, so the stored phasor is deliberately locked to the field
    // line direction.
    int checked_axes = 0;
    for (const postprocessing::VisualizationPrimitive &primitive : primitives) {
        if (!primitive.animated || !(primitive.reference_magnitude > 0.0)) {
            continue;
        }
        const em::Vec3 early = em::realAtPhase(primitive.phasor, 0.3);
        const em::Vec3 late = em::realAtPhase(primitive.phasor, 1.7);
        const double scale = em::magnitude(early) * em::magnitude(late);
        if (!(scale > 1.0e-24)) {
            continue;
        }
        const em::Vec3 twist{early.y * late.z - early.z * late.y,
                             early.z * late.x - early.x * late.z,
                             early.x * late.y - early.y * late.x};
        expectTrue(em::magnitude(twist) / scale < 1.0e-9,
                   "an animated arrow keeps its axis across phases (no rotation)");
        ++checked_axes;
    }
    expectTrue(checked_axes > 0, "animated arrows were actually checked for rotation");
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

    // A rectangular-window iris has an exact semi-analytic solution and is
    // routed to the mode-matching backend, not to FEM.
    const em::EmSolverDispatcher dispatcher(std::make_shared<TestFemBackend>());
    const em::FieldSolution solution = dispatcher.solve(request);
    expectTrue(solution.diagnostics.backend_name.rfind("Mode matching", 0) == 0,
               "a rectangular iris is routed to the mode-matching backend: " +
                   solution.diagnostics.backend_name);

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

em::SimulationRequest makeIrisRequest(double a, double b,
                                      double window_w, double window_h,
                                      double thickness)
{
    em::SimulationRequest request;
    request.frequency_hz = 10.0e9;
    request.model.waveguide.inner_width_m = a;
    request.model.waveguide.inner_height_m = b;
    request.model.waveguide.length_m = 50.0e-3;
    request.model.waveguide.wall_thickness_m = 0.1e-3;
    request.excitation.automatic = false;
    request.excitation.family = em::ModeFamily::TransverseElectric;
    request.excitation.m = 1;
    request.excitation.n = 0;
    request.settings.maximum_m = 3;
    request.settings.maximum_n = 3;
    request.settings.normalization_power_w = 1.0;

    em::PecPlateGeometry iris;
    iris.enabled = true;
    iris.center_m = {0.0, 0.0, 0.0};
    iris.size_m = {a, b, thickness};
    iris.aperture_enabled = true;
    iris.aperture_shape = em::PlateApertureShape::Rectangular;
    iris.aperture_width_m = window_w;
    iris.aperture_height_m = window_h;
    request.model.pec_plates.push_back(iris);
    return request;
}

void testModeMatchingIris()
{
    const em::ModeMatchingIrisSolver solver;
    const double a = 22.66e-3;
    const double b = 9.96e-3;

    // Reference iris: window 0.5a x 0.5b, thickness 0.5 mm.
    const em::FieldSolution solution =
        solver.solve(makeIrisRequest(a, b, 0.5 * a, 0.5 * b, 0.5e-3));
    expectTrue(solution.success && solution.field,
               "mode matching solves the rectangular iris");
    const double s11 = std::abs(solution.scattering.s11);
    const double s21 = std::abs(solution.scattering.s21);

    // Lossless mode matching is structurally unitary: the truncated system
    // conserves power to machine precision, not merely approximately. Stated
    // twice, because the reported power balance is assembled from the same
    // S-parameters and could balance while both were wrong.
    expectTrue(solution.diagnostics.power_balance_relative_error < 1.0e-12,
               "mode matching conserves power to machine precision: " +
                   std::to_string(solution.diagnostics.power_balance_relative_error));
    expectNear(s11 * s11 + s21 * s21, 1.0, 1.0e-12,
               "the lossless iris scattering matrix is unitary");
    // Converged value 0.771 (8x8 vs 12x12 aperture modes differ by 0.002).
    expectNear(s11, 0.769, 0.02, "iris |S11| matches the converged mode-matching value");
    expectTrue(std::abs(solution.scattering.s12 - solution.scattering.s21) < 1.0e-12,
               "mode matching is reciprocal");
    expectNear(std::abs(solution.scattering.s22), s11, 1.0e-9,
               "a centred iris has |S22| = |S11|");

    // Marcuvitz thin symmetric inductive window, d = a/2, full height:
    // B/Y0 = (lambda_g/a) ctg^2(pi d / 2a)  =>  |S11| = 0.6558. The formula is
    // itself first-order accurate, so a 5 % band is the honest comparison.
    {
        const double a_std = 22.86e-3;
        const double b_std = 10.16e-3;
        const em::FieldSolution inductive =
            solver.solve(makeIrisRequest(a_std, b_std, 0.5 * a_std, b_std, 0.05e-3));
        expectTrue(inductive.success, "thin inductive window solves");
        expectNear(std::abs(inductive.scattering.s11), 0.6558, 0.05,
                   "thin inductive window matches the Marcuvitz susceptance");
    }

    // Limiting cases: a tiny window reflects almost everything, a window nearly
    // as large as the guide is almost transparent.
    {
        const em::FieldSolution tiny =
            solver.solve(makeIrisRequest(a, b, 0.1 * a, 0.1 * b, 0.5e-3));
        expectTrue(tiny.success && std::abs(tiny.scattering.s11) > 0.99 &&
                       std::abs(tiny.scattering.s21) < 0.02,
                   "a tiny window is an almost perfect reflector");
        const em::FieldSolution open =
            solver.solve(makeIrisRequest(a, b, 0.9 * a, 0.9 * b, 0.5e-3));
        expectTrue(open.success && std::abs(open.scattering.s11) < 0.05 &&
                       std::abs(open.scattering.s21) > 0.99,
                   "a nearly full window is almost transparent");
    }

    // The reconstructed field satisfies Maxwell in the guide region: the modes
    // are exact solutions, so the numerical-curl residual is at rounding level.
    if (solution.field) {
        const em::Vec3 position_m{2.0e-3, 1.0e-3, -8.0e-3};
        const double step_m = 2.0e-7;
        const double omega = 2.0 * em::pi * 10.0e9;
        const em::Complex imaginary_unit(0.0, 1.0);
        const em::FieldPhasor field = solution.field->evaluate(position_m);
        const em::ComplexVec3 curl_e =
            numericalCurlElectric(*solution.field, position_m, step_m);
        const em::ComplexVec3 faraday_term =
            scaled(field.magnetic_a_per_m,
                   imaginary_unit * omega * em::vacuum_permeability_h_per_m);
        const em::ComplexVec3 residual = curl_e + faraday_term;
        const double scale = std::max(em::magnitude(curl_e), em::magnitude(faraday_term));
        expectTrue(em::magnitude(residual) / scale < 1.0e-6,
                   "mode-matching field satisfies the Faraday law");
    }
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

// Tetrahedra of a mesh gmsh has just written. The mesher always asks for the
// msh2 format, so every element is one line and its type is the second field;
// type 4 is the linear tetrahedron. Returns -1 when the file is unreadable.
int countMeshTetrahedra(const std::filesystem::path &mesh_path)
{
    std::ifstream mesh_file(mesh_path);
    if (!mesh_file) {
        return -1;
    }
    std::string line;
    while (std::getline(mesh_file, line) && line.rfind("$Elements", 0) != 0) {
    }
    int element_count = 0;
    if (!(mesh_file >> element_count)) {
        return -1;
    }
    std::getline(mesh_file, line);
    int tetrahedron_count = 0;
    for (int index = 0; index < element_count && std::getline(mesh_file, line); ++index) {
        std::istringstream fields(line);
        int element_identifier = 0;
        int element_type = 0;
        fields >> element_identifier >> element_type;
        if (element_type == 4) {
            ++tetrahedron_count;
        }
    }
    return tetrahedron_count;
}

// The model the refinement cost is measured on: a 0.5 mm plate across the whole
// cross-section, either blind or with a round window. The window rim is the
// thin detail local refinement exists for; the plate face away from the rim is
// not one, and that is the distinction the cost caps below check. Only the
// guide is shortened and the global cell coarsened, both to keep the test
// inside a couple of seconds.
// Both refinement knobs are pinned here rather than inherited. What this test
// measures is where the fine cells go, not how fine they are allowed to get,
// and the two are not separable: the size ratio is a floor under the cell size,
// so lowering it silently turns the measurement off. The product default moved
// from 20 to 4 while this file was being written and the window rim cost fell
// from 39x to 2.9x - the refinement still going exactly where it should, but
// clamped, and the test read it as "no refinement at all".
em::SimulationRequest makeRefinementCostRequest(bool with_window,
                                                double elements_across_feature)
{
    em::SimulationRequest request;
    request.frequency_hz = 10.0e9;
    request.model.waveguide.inner_width_m = 22.66e-3;
    request.model.waveguide.inner_height_m = 9.96e-3;
    request.model.waveguide.length_m = 12.0e-3;
    request.model.waveguide.wall_thickness_m = 0.1e-3;
    request.settings.fem.mesh.maximum_element_size_m = 4.0e-3;
    request.settings.fem.mesh.minimum_size_ratio = 20.0;
    request.settings.fem.mesh.elements_across_smallest_feature = elements_across_feature;
    // The plate thickness is the detail under test, so the plate has to be a
    // solid volume no matter what the automatic zero-thickness sheet rule says.
    request.settings.fem.mesh.sheet_thickness_threshold_m = 0.0;

    em::PecPlateGeometry plate;
    plate.enabled = true;
    plate.center_m = {0.0, 0.0, 0.0};
    plate.size_m = {22.66e-3, 9.96e-3, 0.5e-3};
    plate.aperture_enabled = with_window;
    plate.aperture_shape = em::PlateApertureShape::Circular;
    plate.aperture_radius_m = 4.6e-3;
    request.model.pec_plates.push_back(plate);
    return request;
}

// Each mesh gets its own directory so that a stale .msh from an earlier model
// can never be counted as this one.
int meshTetrahedronCount(const em::SimulationRequest &request, const std::string &tag)
{
    const std::filesystem::path output_directory =
        std::filesystem::temp_directory_path() / "krutiev_refinement_cost_test" / tag;
    const em::FemMeshFiles files =
        em::GmshTetrahedralMesher().generate(request, output_directory);
    if (!files.error_message.empty()) {
        return -1;
    }
    return countMeshTetrahedra(files.mesh_path);
}

// Local refinement has to stay a local cost. While the refinement box covered
// the whole plate face instead of the edges the fluid actually wraps around,
// the windowed plate cost 105.8 times the unrefined mesh and the blind plate
// 101.2 times; measured on this machine now, with the CST gmsh, they cost 39.2
// and exactly 1.00 times. The caps sit between the two behaviours, not next to
// either: over global element sizes 3.960 / 4.000 / 4.001 / 4.007 / 4.041 mm
// the windowed ratio moved only between 38.5 and 40.1 (and the old one between
// 104.1 and 108.6), so 60 is far outside the noise of both.
void testPlateRefinementCost()
{
    const int blind_plain = meshTetrahedronCount(makeRefinementCostRequest(false, 0.0),
                                                 "blind_plain");
    if (blind_plain <= 0) {
        std::cout << "SKIP: plate refinement cost (Gmsh produced no mesh).\n";
        return;
    }
    const int blind_refined = meshTetrahedronCount(makeRefinementCostRequest(false, 3.0),
                                                   "blind_refined");
    const int windowed_plain = meshTetrahedronCount(makeRefinementCostRequest(true, 0.0),
                                                    "windowed_plain");
    const int windowed_refined = meshTetrahedronCount(makeRefinementCostRequest(true, 3.0),
                                                      "windowed_refined");
    expectTrue(blind_refined > 0 && windowed_plain > 0 && windowed_refined > 0,
               "every refinement-cost mesh was generated");
    if (blind_refined <= 0 || windowed_plain <= 0 || windowed_refined <= 0) {
        return;
    }

    // A plate that blocks the whole cross-section leaves no thin fluid anywhere:
    // its thickness is a detail of the metal, not of the volume being meshed, so
    // refinement must cost nothing at all.
    expectTrue(blind_refined <= 2 * blind_plain,
               "a blind plate across the section costs no refinement: " +
                   std::to_string(blind_refined) + " against " +
                   std::to_string(blind_plain) + " tetrahedra");

    // The window rim is a genuine thin detail, so it does cost - but locally.
    expectTrue(windowed_refined <= 60 * windowed_plain,
               "the window rim refinement stays local: " +
                   std::to_string(windowed_refined) + " against " +
                   std::to_string(windowed_plain) + " tetrahedra");

    // Without this the cap above would also pass on a build where the local
    // refinement stopped working altogether.
    expectTrue(windowed_refined >= 4 * windowed_plain,
               "the window rim is actually refined: " +
                   std::to_string(windowed_refined) + " against " +
                   std::to_string(windowed_plain) + " tetrahedra");
}

// elements_across_smallest_feature = 0 (or less) is documented as "no local
// refinement", and the mesh a user gets then must be exactly the mesh from
// before local refinement existed: not a single size field in the script.
void testRefinementSwitchesOffCompletely()
{
    em::SimulationRequest request = makeRefinementCostRequest(true, 3.0);
    // Window rim, stub and the gaps beside it: the model with the most details
    // to register, so the check is not passing for lack of anything to emit.
    request.model.pec_plates.front().post_enabled = true;
    request.model.pec_plates.front().post_width_m = 1.5e-3;
    request.model.pec_plates.front().post_height_m = 0.5 * 9.96e-3;
    expectTrue(em::GmshTetrahedralMesher::buildGeometryScript(request).find("Field[") !=
                   std::string::npos,
               "the iris with a stub does ask for local size fields");

    for (const double elements_across_feature : {0.0, -1.0}) {
        request.settings.fem.mesh.elements_across_smallest_feature = elements_across_feature;
        expectTrue(em::GmshTetrahedralMesher::buildGeometryScript(request).find("Field[") ==
                       std::string::npos,
                   "elements_across_smallest_feature = " +
                       formatNumber(elements_across_feature) +
                       " emits no size field at all");
    }
}

// A capacitive obstacle: a plate welded to both side walls, thick enough to be
// meshed as a volume, with a narrow strip of fluid left above and below it. That
// strip is the only small feature in the model and the whole reflection is
// decided in it, so this is the sharpest test of whether asking for more
// elements across the smallest feature does anything at all.
// It used to do nothing. The fluid beside the plate was sized by the PLATE
// THICKNESS instead of by its own width, which inverts the rule - the thicker
// the plate, the coarser the cell demanded in the gap beside it - and 3 mm / 3 =
// 1 mm was then discarded as "already resolved" by the 2.8 mm global size. The
// knob was inert: 0, 1, 2 and 3 elements across the feature produced the same
// script, the same mesh and bit-identical S-parameters (0.91760474 four times).
em::SimulationRequest makeCapacitiveObstacleRequest(double elements_across_feature)
{
    em::SimulationRequest request;
    request.frequency_hz = 10.0e9;
    request.model.waveguide.inner_width_m = 22.66e-3;
    request.model.waveguide.inner_height_m = 9.96e-3;
    // Only the guide length is cut down from the reference model: the gap, the
    // plate and the cell size - everything the knob acts on - are kept.
    request.model.waveguide.length_m = 12.0e-3;
    request.model.waveguide.wall_thickness_m = 0.1e-3;
    request.settings.fem.mesh.maximum_element_size_m = 2.8e-3;
    request.settings.fem.mesh.minimum_size_ratio = 20.0;
    request.settings.fem.mesh.elements_across_smallest_feature = elements_across_feature;
    // The plate is 3 mm thick on purpose: a thickness far larger than the gap is
    // what made the old rule ask for coarse cells in a narrow gap.
    request.settings.fem.mesh.sheet_thickness_threshold_m = 0.0;

    em::PecPlateGeometry obstacle;
    obstacle.enabled = true;
    obstacle.center_m = {0.0, 0.0, 0.0};
    // 9.96 - 2 x 0.95 = 8.06 mm tall, so 0.95 mm of fluid above and below.
    obstacle.size_m = {22.66e-3, 8.06e-3, 3.0e-3};
    request.model.pec_plates.push_back(obstacle);
    return request;
}

void testCapacitiveGapFollowsTheFeatureKnob()
{
    constexpr double gap_m = 0.95e-3;
    expectTrue(refinementCellSizes(em::GmshTetrahedralMesher::buildGeometryScript(
                                       makeCapacitiveObstacleRequest(0.0)))
                   .empty(),
               "no local refinement is asked for at zero elements across the feature");

    double previous_finest_cell_m = std::numeric_limits<double>::infinity();
    for (const double elements_across_feature : {1.0, 2.0, 3.0}) {
        const std::string script = em::GmshTetrahedralMesher::buildGeometryScript(
            makeCapacitiveObstacleRequest(elements_across_feature));
        const double expected_cell_size_m = gap_m / elements_across_feature;
        // The gap is sized by its OWN width: this exact number is the whole
        // point, and it is the number the old code could not produce.
        expectTrue(hasRefinementCellSize(script, expected_cell_size_m),
                   "the 0.95 mm gap asks for " + formatNumber(expected_cell_size_m * 1.0e3) +
                       " mm cells at " + formatNumber(elements_across_feature) +
                       " elements across it: " + describeCellSizes(script));
        // Read back out of the script, not recomputed from the same arithmetic
        // the assertion above already used: what has to move is the mesh the
        // knob produces, and it used to be the same mesh for every value.
        const std::vector<double> sizes_m = refinementCellSizes(script);
        const double finest_cell_m = sizes_m.empty()
                                         ? std::numeric_limits<double>::infinity()
                                         : sizes_m.front();
        expectTrue(finest_cell_m < previous_finest_cell_m,
                   "the mesh the knob asks for gets finer at " +
                       formatNumber(elements_across_feature) + " elements: " +
                       describeCellSizes(script));
        previous_finest_cell_m = finest_cell_m;
    }

    // The script is where the knob is read, but the mesh is what the solver
    // sees, so the counts have to move too. Two meshes are enough to prove the
    // knob is live and three would cost minutes: at three elements across the
    // gap the cells are 0.32 mm over the full width of the guide.
    const int plain = meshTetrahedronCount(makeCapacitiveObstacleRequest(0.0),
                                           "capacitive_plain");
    if (plain <= 0) {
        std::cout << "SKIP: capacitive gap refinement (Gmsh produced no mesh).\n";
        return;
    }
    const int refined = meshTetrahedronCount(makeCapacitiveObstacleRequest(2.0),
                                             "capacitive_refined");
    expectTrue(refined > 0, "the refined capacitive mesh was generated");
    expectTrue(refined >= 2 * plain,
               "asking for two elements across the 0.95 mm gap changes the mesh: " +
                   std::to_string(refined) + " against " + std::to_string(plain) +
                   " tetrahedra");
}

// The reference iris of the whole project: a 0.5 mm plate across the full
// cross-section with a round window and a stub reaching into it. Everything the
// local refinement can register is present in it at once.
em::SimulationRequest makeReferenceIrisRequest(double length_m, double element_size_m)
{
    em::SimulationRequest request;
    request.frequency_hz = 10.0e9;
    request.model.waveguide.inner_width_m = 22.66e-3;
    request.model.waveguide.inner_height_m = 9.96e-3;
    request.model.waveguide.length_m = length_m;
    request.model.waveguide.wall_thickness_m = 0.1e-3;
    request.excitation.automatic = false;
    request.excitation.family = em::ModeFamily::TransverseElectric;
    request.excitation.m = 1;
    request.excitation.n = 0;
    request.settings.fem.mesh.maximum_element_size_m = element_size_m;
    // The plate thickness is one of the details under test, so the plate stays a
    // volume whatever the automatic zero-thickness rule would say.
    request.settings.fem.mesh.sheet_thickness_threshold_m = 0.0;

    em::PecPlateGeometry iris;
    iris.enabled = true;
    iris.center_m = {0.0, 0.0, 0.0};
    iris.size_m = {22.66e-3, 9.96e-3, 0.5e-3};
    iris.aperture_enabled = true;
    iris.aperture_shape = em::PlateApertureShape::Circular;
    iris.aperture_radius_m = 4.6e-3;
    iris.post_enabled = true;
    iris.post_width_m = 0.49e-3;
    iris.post_height_m = 4.98e-3;
    request.model.pec_plates.push_back(iris);
    return request;
}

// Field[Box] is axis aligned and cannot follow a rotation, so a rotated plate
// gets no local refinement at all. The test used to be on the angle itself, at
// 1e-10 rad, which is not a tolerance but a comparison with zero: on a WR-90
// plate 1e-9 rad displaces the geometry by 1e-11 m, a hundred million times
// less than the finest cell, and it switched the entire refinement off. What the
// user got was the unrefined mesh, whose answer follows the element size instead
// of the geometry - |S11| over four neighbouring sizes spread by 80 % of its own
// mean. The criterion has to be the displacement the rotation causes, measured
// against the cell size that has to resolve it.
void testMicroscopicPlateRotationKeepsRefinement()
{
    em::SimulationRequest upright = makeReferenceIrisRequest(50.0e-3, 2.8e-3);
    // Pinned so that the length the criterion compares against is a known
    // number: the finest cell is 2.8 / 20 = 0.14 mm and the half diagonal of
    // this plate is 12.38 mm, so a rotation stops counting as none at
    // 0.1 x 0.14 / 12.38 = 1.13e-3 rad.
    upright.settings.fem.mesh.minimum_size_ratio = 20.0;
    const std::vector<double> upright_sizes_m = refinementCellSizes(
        em::GmshTetrahedralMesher::buildGeometryScript(upright));
    expectTrue(upright_sizes_m.size() >= 8,
               "the upright reference iris registers its small features: " +
                   std::to_string(upright_sizes_m.size()) + " refinement boxes");

    // Either side of that boundary, and six orders of magnitude below it. The
    // far ends alone would be satisfied by any angular tolerance between 1e-9
    // and 1e-2 rad; the 1e-3 / 2e-3 pair narrows that to the one place the
    // displacement rule puts the boundary for this plate and this cell size.
    for (const double rotation_rad : {1.0e-9, 1.0e-6, 1.0e-3}) {
        em::SimulationRequest rotated = upright;
        rotated.model.pec_plates.front().rotation_rad.z = rotation_rad;
        const std::string script = em::GmshTetrahedralMesher::buildGeometryScript(rotated);
        expectTrue(refinementCellSizes(script) == upright_sizes_m,
                   "a " + formatNumber(rotation_rad) +
                       " rad rotation leaves the refinement untouched: " +
                       describeCellSizes(script));
        expectTrue(script.find("Warning(") == std::string::npos,
                   "a " + formatNumber(rotation_rad) +
                       " rad rotation is not reported as a rotated plate");
    }

    // A rotation that does move the geometry still gets no refinement - that is
    // a limitation of Field[Box], not a defect - but it must say so instead of
    // silently handing back a mesh whose answer depends on the element size.
    for (const double rotation_rad : {2.0e-3, 1.0e-2}) {
        em::SimulationRequest tilted = upright;
        tilted.model.pec_plates.front().rotation_rad.z = rotation_rad;
        const std::string script = em::GmshTetrahedralMesher::buildGeometryScript(tilted);
        // Counted through the box sizes and not through the string "Field[": the
        // comment the script carries next to the warning names Field[Box] itself.
        expectTrue(refinementCellSizes(script).empty(),
                   "a plate rotated by " + formatNumber(rotation_rad) +
                       " rad gets no axis-aligned refinement boxes: " +
                       describeCellSizes(script));
        expectTrue(script.find("Warning(\"Rotated PEC plate") != std::string::npos,
                   "a plate rotated by " + formatNumber(rotation_rad) +
                       " rad is reported as unrefined");
    }
}

// Below lambda / 100 the mesher stops building the plate as a volume and embeds
// it as a zero-thickness PEC sheet. The sheet used to get no edge refinement at
// all, which left the sharpest edge of the whole family unresolved: the field
// goes as rho^(-1/2) on a zero-thickness rim against rho^(-1/3) on a right
// angled one. A sheet has no thickness of its own to size the band on, so the
// band is built on the thickness the sheet MODEL stands for - the threshold
// itself - and that is also what makes the mesh continuous where the two models
// meet, which is the property this test pins down.
em::SimulationRequest makePlateThicknessRequest(double thickness_m)
{
    em::SimulationRequest request = makeReferenceIrisRequest(12.0e-3, 2.8e-3);
    request.settings.fem.mesh.minimum_size_ratio = 20.0;
    request.settings.fem.mesh.elements_across_smallest_feature = 1.0;
    // The automatic rule, i.e. the switch between the two models, is what is
    // being tested here.
    request.settings.fem.mesh.sheet_thickness_threshold_m = -1.0;
    request.model.pec_plates.front().post_enabled = false;
    request.model.pec_plates.front().size_m.z = thickness_m;
    return request;
}

void testZeroThicknessSheetKeepsItsEdgeRefinement()
{
    // lambda / 100 at 10 GHz. Both the switch and the band width are built on it.
    const double sheet_threshold_m =
        0.01 * em::speed_of_light_m_per_s / 10.0e9;

    const std::string sheet_script =
        em::GmshTetrahedralMesher::buildGeometryScript(makePlateThicknessRequest(0.05e-3));
    // Without this the test would keep passing on a build where the plate stopped
    // being a sheet at all and got its refinement as an ordinary volume.
    expectTrue(sheet_script.find("pecSheets[] +=") != std::string::npos,
               "a 0.05 mm plate is meshed as a zero-thickness sheet");
    expectTrue(hasRefinementCellSize(sheet_script, sheet_threshold_m),
               "the sheet window rim is refined on the thickness the sheet model "
               "stands for (" + formatNumber(sheet_threshold_m * 1.0e3) + " mm): " +
                   describeCellSizes(sheet_script));

    // Continuity across the switch: 0.29 mm is a sheet, 0.31 mm is a volume, and
    // the mesh either side of the threshold has to be the same mesh. It was not:
    // the volume got a 0.31 mm band and the sheet got no band at all.
    const std::string thin_volume_script =
        em::GmshTetrahedralMesher::buildGeometryScript(makePlateThicknessRequest(0.31e-3));
    expectTrue(thin_volume_script.find("pecSheets[] +=") == std::string::npos,
               "a 0.31 mm plate is still meshed as a volume");
    expectTrue(hasRefinementCellSize(thin_volume_script, 0.31e-3),
               "the volume window rim is refined on the plate thickness: " +
                   describeCellSizes(thin_volume_script));
    const std::string thin_sheet_script =
        em::GmshTetrahedralMesher::buildGeometryScript(makePlateThicknessRequest(0.29e-3));
    expectTrue(thin_sheet_script.find("pecSheets[] +=") != std::string::npos,
               "a 0.29 mm plate is meshed as a sheet");
    expectTrue(refinementCellSizes(thin_sheet_script).size() > 4 &&
                   refinementCellSizes(thin_sheet_script).size() ==
                       refinementCellSizes(thin_volume_script).size(),
               "the same plate 0.02 mm either side of the sheet threshold registers "
               "the same details: " + describeCellSizes(thin_sheet_script) +
                   " against " + describeCellSizes(thin_volume_script));

    // The free outer edge of a sheet: the same plate cut down so that it ends
    // 0.95 mm short of the top and bottom wall instead of spanning the section.
    em::SimulationRequest free_edge = makePlateThicknessRequest(0.05e-3);
    free_edge.model.pec_plates.front().aperture_enabled = false;
    free_edge.model.pec_plates.front().size_m.y = 8.06e-3;
    const std::string free_edge_script =
        em::GmshTetrahedralMesher::buildGeometryScript(free_edge);
    expectTrue(hasRefinementCellSize(free_edge_script, sheet_threshold_m),
               "the free edge of a sheet is refined on the sheet substitute "
               "thickness: " + describeCellSizes(free_edge_script));
    expectTrue(hasRefinementCellSize(free_edge_script, 0.95e-3),
               "the fluid gap left beside the sheet is refined on its own width: " +
                   describeCellSizes(free_edge_script));
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
    // Pinned, not inherited. Every number quoted in this test - the 8.9 degree
    // discretisation error and the mesh sequence it converges on - was measured
    // with first-order elements, and what the test checks is the port
    // de-embedding, which the element order has nothing to do with. The default
    // order is exercised by testDefaultConfigurationSolvesWithTheDirectSolver
    // and by testSecondOrderSolutionIsNotCalledActive, which are about it.
    request.settings.fem.mesh.element_order = 1;

    int progress_report_count = 0;
    bool has_linear_solver_stage = false;
    em::SolveControl progress_control;
    progress_control.progress_reporter = [&](const std::string &stage) {
        ++progress_report_count;
        // Which of the two appears depends on the linear solver the backend
        // picked for this system, and on a mesh this small the automatic choice
        // is the direct one whenever the build has Eigen.
        has_linear_solver_stage = has_linear_solver_stage ||
                                  stage.find("GMRES") != std::string::npos ||
                                  stage.find("Sparse LU") != std::string::npos;
    };
    const em::FieldSolution solution =
        em::MfemFrequencyDomainBackend().solve(request, progress_control);
    expectTrue(solution.success && solution.field,
               "MFEM solves a driven empty WR-90 guide");
    expectTrue(progress_report_count > 0 && has_linear_solver_stage,
               "FEM backend names the linear solver it is running in its progress stages");
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

    // All backends must reference the S-parameters to the physical port planes,
    // so an empty guide has S21 = exp(-gamma L) in every one of them. Checking
    // only the magnitude once hid a de-embedding bug that removed the whole
    // guide length from the FEM phase (an 84 degree error at this mesh).
    const em::FieldSolution analytic = em::AnalyticWaveguideSolver().solve(request);
    expectTrue(analytic.success && analytic.has_selected_mode,
               "analytic reference for the phase comparison solves");
    const double phase_difference =
        std::abs(std::arg(solution.scattering.s21) - std::arg(analytic.scattering.s21));
    const double wrapped_difference =
        std::min(phase_difference, 2.0 * em::pi - phase_difference) * 180.0 / em::pi;
    // Measured discretisation error at this 4 mm mesh is 8.9 deg and converges
    // as 3.4 / 1.8 / 0.75 deg at 2.8 / 2.0 / 1.5 mm.
    expectTrue(wrapped_difference < 15.0,
               "FEM S21 phase agrees with the analytic exp(-gamma L): " +
                   std::to_string(wrapped_difference) + " deg");
}

#ifdef KRUTIEV_WITH_EIGEN
em::SimulationRequest makeLinearSolverRequest(em::LinearSolverMethod method)
{
    em::SimulationRequest request = createRequest(10.0e9);
    request.excitation.automatic = false;
    request.excitation.family = em::ModeFamily::TransverseElectric;
    request.excitation.m = 1;
    request.excitation.n = 0;
    // Small enough that GMRES reaches 1e-8 in 160 iterations, so the iterative
    // result is a reference and not a second unknown. Second-order elements on
    // the same mesh need 2000 and take half a minute, and what is compared here
    // is two solvers on one matrix, not the matrix.
    request.settings.fem.mesh.maximum_element_size_m = 4.0e-3;
    request.settings.fem.mesh.element_order = 1;
    request.settings.fem.relative_tolerance = 1.0e-8;
    request.settings.fem.maximum_iterations = 2000;
    request.settings.fem.linear_solver_method = method;
    return request;
}

void testDirectAndIterativeSolversAgree()
{
    const em::FieldSolution direct = em::MfemFrequencyDomainBackend().solve(
        makeLinearSolverRequest(em::LinearSolverMethod::Direct), {});
    const em::FieldSolution iterative = em::MfemFrequencyDomainBackend().solve(
        makeLinearSolverRequest(em::LinearSolverMethod::Iterative), {});
    expectTrue(direct.success && iterative.success,
               "both linear solvers solve the empty WR-90 guide");
    if (!direct.success || !iterative.success) {
        return;
    }

    // Without these the comparison could quietly degenerate into GMRES against
    // GMRES: the backend falls back on the Krylov solver whenever the
    // factorization refuses the system, and it still reports success.
    expectTrue(direct.diagnostics.backend_name.find("SparseLU") != std::string::npos &&
                   direct.diagnostics.linear_iterations == 0,
               "the direct request really was factorized: " +
                   direct.diagnostics.backend_name);
    expectTrue(iterative.diagnostics.backend_name.find("GMRES") != std::string::npos &&
                   iterative.diagnostics.linear_iterations > 0,
               "the iterative request really ran GMRES: " +
                   iterative.diagnostics.backend_name);
    expectTrue(iterative.diagnostics.linear_relative_residual < 1.0e-6,
               "GMRES converged far enough to serve as the reference: " +
                   formatNumber(iterative.diagnostics.linear_relative_residual));

    // The real 2N x 2N block system handed to the factorization has to match the
    // complex operator MFEM assembled. Negating the off-diagonal blocks - the
    // imaginary part - still factorizes, still reports success and even leaves
    // |S21| at 0.989320 to six digits; only the complex value moves, from
    // (-0.209271, -0.966933) to (+0.209255, -0.966936). So the comparison must
    // be on the complex numbers. Measured difference between the two solvers is
    // 1.4e-10 on S11 and 4.2e-10 on S21, against 0.42 for the flipped sign.
    expectComplexNear(direct.scattering.s11, iterative.scattering.s11, 1.0e-5,
                      "direct and iterative solvers agree on S11");
    expectComplexNear(direct.scattering.s21, iterative.scattering.s21, 1.0e-5,
                      "direct and iterative solvers agree on S21");
    // A conjugated block layout shows up in the phase alone, so the reference
    // S21 must carry one. Measured -1.784 rad on this mesh.
    expectTrue(std::abs(std::arg(direct.scattering.s21)) > 0.5,
               "the solver comparison runs on an S21 that carries a phase: " +
                   formatNumber(std::arg(direct.scattering.s21)) + " rad");

    // A factorization solves the assembled system, it does not approach it.
    // Measured 1.2e-14 here; the same quantity is 1.4 when the imaginary block
    // enters with the wrong sign, so this one number is the sharpest guard on
    // the block layout that the test has.
    expectTrue(direct.diagnostics.linear_relative_residual < 1.0e-10,
               "the direct solver leaves a machine-precision residual: " +
                   formatNumber(direct.diagnostics.linear_relative_residual));
}

// The most expensive test in this file, and deliberately so: it is the only one
// that runs the product's own configuration on a system of the size a user
// actually asks for, and the defect it guards was invisible on anything smaller.
//
// The direct path used to read Eigen's m_info between analyzePattern() and
// factorize(), where nothing has written it yet. In Release the stack garbage it
// found rejected the factorization on every real model - 3 runs out of 3 on four
// models between 63033 and 106439 unknowns - with an empty reason string, while
// the small systems the rest of this file solves happened to read a zero and
// passed. The user was then handed to GMRES, which on the refined mesh the same
// configuration produces does not converge at all: 8000 iterations, 615 s, a
// true residual of 2.0e-3 and no S-parameters. The same model factorizes in
// 286 s. A test on a small system cannot see any of this.
//
// Two other defects are folded in because they need the same run:
//   - the direct path used to accept whatever the factorization returned. With
//     the solution vector deliberately doubled inside the solver it still
//     reported success and a full set of S-parameters at a true relative
//     residual of 1.00. That corruption hook cannot be reached from outside the
//     backend, so the acceptance criterion is guarded by its two observable
//     consequences instead: the residual it is supposed to enforce, and the
//     unitarity of the S-parameters, which no residual bookkeeping feeds and
//     which a doubled solution breaks by a factor of four.
//   - the standard configuration has to reach an answer at all. Everything the
//     "normal" accuracy level sets - element order, elements across the smallest
//     feature, minimum size ratio, refinement factor and the automatic choice of
//     linear solver - is left at its default here, and only the global cell size
//     is set, to 3 mm against the 2.03 mm the automatic rule would pick. That
//     one number is the entire difference between this test and a product run,
//     and it is a factor of three in the unknown count. The level-to-settings
//     mapping itself lives in a Qt translation unit this binary does not link,
//     so what is pinned here is the settings the "normal" level assigns, not the
//     assignment.
void testDefaultConfigurationSolvesWithTheDirectSolver()
{
    // The geometry a fresh model starts from: WR-90 and one plate at the centre.
    em::SimulationRequest request;
    request.frequency_hz = 10.0e9;
    request.model.waveguide.inner_width_m = 22.86e-3;
    request.model.waveguide.inner_height_m = 10.16e-3;
    request.model.waveguide.length_m = 50.0e-3;
    request.model.waveguide.wall_thickness_m = 0.1e-3;
    request.excitation.automatic = false;
    request.excitation.family = em::ModeFamily::TransverseElectric;
    request.excitation.m = 1;
    request.excitation.n = 0;
    request.settings.fem.mesh.maximum_element_size_m =
        build_is_optimized ? 3.0e-3 : 6.0e-3;
    const int minimum_unknown_count = build_is_optimized ? 30000 : 4000;

    em::PecPlateGeometry plate;
    plate.enabled = true;
    plate.center_m = {0.0, 0.0, 0.0};
    plate.size_m = {10.0e-3, 8.0e-3, 0.5e-3};
    request.model.pec_plates.push_back(plate);

    // Half the defect is the refined mesh itself: the cell size contrast local
    // refinement creates is what the iterative solver cannot handle, so a run
    // that reached an answer with the refinement switched off would prove
    // nothing about the configuration a user gets.
    expectTrue(!refinementCellSizes(em::GmshTetrahedralMesher::buildGeometryScript(request))
                    .empty(),
               "the default configuration does ask for local mesh refinement");

    const em::FieldSolution solution = em::MfemFrequencyDomainBackend().solve(request, {});
    std::cout << "  default configuration: " << solution.diagnostics.fem_unknown_count
              << " unknowns, " << solution.diagnostics.backend_name << ", residual "
              << formatNumber(solution.diagnostics.linear_relative_residual)
              << ", |S11| = " << formatNumber(std::abs(solution.scattering.s11))
              << ", |S21| = " << formatNumber(std::abs(solution.scattering.s21)) << '\n';
    expectTrue(solution.success && solution.field,
               "the default configuration reaches an answer: " + solution.error_message);
    if (!solution.success) {
        return;
    }

    // The size is part of what is being tested: below this the defect above hides.
    expectTrue(solution.diagnostics.fem_unknown_count >= minimum_unknown_count,
               "the default configuration is solved on a system of realistic size: " +
                   std::to_string(solution.diagnostics.fem_unknown_count) + " unknowns");
    expectTrue(solution.diagnostics.backend_name.find("SparseLU") != std::string::npos &&
                   solution.diagnostics.linear_iterations == 0,
               "the system really was factorized rather than quietly handed to GMRES: " +
                   solution.diagnostics.backend_name);
    expectTrue(!hasWarningContaining(solution, "Direct solver unavailable"),
               "the factorization was not refused");
    // A factorization solves the system; it does not approach it. Measured 1e-13
    // on systems from 1453 to 112246 unknowns, and the backend's own acceptance
    // bar is 1e-8. The lower bound is not pedantry: an acceptance test on a
    // number nobody computes would pass on any solution at all, and on a system
    // this size the true residual cannot be exactly zero.
    expectTrue(solution.diagnostics.linear_relative_residual > 0.0 &&
                   solution.diagnostics.linear_relative_residual < 1.0e-10,
               "the accepted factorization left a measured, machine-precision "
               "residual: " +
                   formatNumber(solution.diagnostics.linear_relative_residual));

    // Independent of every number the solver reports about itself: a lossless,
    // non-radiating obstacle cannot return more power than it was given, and
    // cannot swallow a noticeable part of it either.
    const double scattered_power =
        std::norm(solution.scattering.s11) + std::norm(solution.scattering.s21);
    expectTrue(scattered_power > 0.97 && scattered_power < 1.03,
               "the accepted solution scatters the incident power: |S11|^2 + |S21|^2 = " +
                   formatNumber(scattered_power));
    expectTrue(std::abs(solution.scattering.s11) > 0.01,
               "the default-configuration model actually reflects, so the run is not "
               "trivially unitary: |S11| = " +
                   formatNumber(std::abs(solution.scattering.s11)));
}

// The passivity warning used to fire on the most accurate results in the suite.
// Its floor was 1e-9 of the incident power, three orders below anything the
// chain can deliver: the port projection samples the mode on a 36 x 24 grid and
// the mesh carries its own error, so with second-order elements the residual
// 1 - |S11|^2 - |S21|^2 stops falling with the mesh at about 1e-4 and its SIGN
// turns random. A run that came out at |S21| = 1.0000338 was then reported as
// an active - power-producing - waveguide.
void testSecondOrderSolutionIsNotCalledActive()
{
    bool has_active_run = false;
    // The 2.6 mm mesh is the most expensive of the three and the least needed:
    // 3.2 mm already lands above unity, which is the case the test exists for.
    const std::vector<double> element_sizes_m =
        build_is_optimized ? std::vector<double>{4.0e-3, 3.2e-3, 2.6e-3}
                           : std::vector<double>{4.0e-3, 3.2e-3};
    for (const double element_size_m : element_sizes_m) {
        em::SimulationRequest request = createRequest(10.0e9);
        request.model.waveguide.length_m = 40.0e-3;
        request.excitation.automatic = false;
        request.excitation.family = em::ModeFamily::TransverseElectric;
        request.excitation.m = 1;
        request.excitation.n = 0;
        request.settings.fem.mesh.maximum_element_size_m = element_size_m;
        // The default order is the point of the test; the direct solver keeps the
        // linear solve out of the residual being measured.
        request.settings.fem.linear_solver_method = em::LinearSolverMethod::Direct;

        const em::FieldSolution solution =
            em::MfemFrequencyDomainBackend().solve(request, {});
        const std::string label = formatNumber(element_size_m * 1.0e3) + " mm";
        expectTrue(solution.success && solution.request.settings.fem.mesh.element_order >= 2,
                   "the empty guide solves at the default element order at " + label);
        if (!solution.success) {
            continue;
        }
        const double scattered_power =
            std::norm(solution.scattering.s11) + std::norm(solution.scattering.s21);
        std::cout << "  passivity at " << label << ": |S11|^2 + |S21|^2 - 1 = "
                  << formatNumber(scattered_power - 1.0) << ", defect "
                  << formatNumber(solution.diagnostics.unitarity_defect) << '\n';
        has_active_run = has_active_run || scattered_power > 1.0 + 1.0e-9;

        expectTrue(!hasWarningContaining(solution, "Passivity violated"),
                   "a converged second-order run is not reported as active at " + label);
        // The other half of the same statement: the threshold was raised to 1e-4
        // because that is where this quantity levels off, not to silence it. A
        // run that misses it by more than that is a real defect and has to be
        // seen, so the test fails if the number ever gets there.
        expectTrue(solution.diagnostics.unitarity_defect < 1.0e-4,
                   "the second-order unitarity residual stays inside the noise floor "
                   "the threshold is built on at " + label + ": " +
                       formatNumber(solution.diagnostics.unitarity_defect));
    }
    // Without this the test would keep passing on a build where every run came
    // out passive, i.e. where the old 1e-9 floor would never have fired either
    // and nothing about the change would be under test.
    expectTrue(has_active_run,
               "at least one second-order run exceeds unity by more than the 1e-9 the "
               "old floor called a passivity violation");
}
#endif

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

struct ShortCircuitReflection
{
    em::Complex s11;
    em::Complex s21;
    bool solved = false;
};

// A solid transverse PEC sheet across the whole cross-section is an exact short
// circuit: S11 = -exp(-2 gamma d), S21 = 0, where d is the distance from the
// input port plane to the sheet. The geometry carries no small feature to
// resolve, so a deviation here is a port, assembly or de-embedding error rather
// than a discretisation error.
ShortCircuitReflection solveTransverseShortCircuit(double distance_to_short_m)
{
    em::SimulationRequest request = createRequest(10.0e9);
    request.excitation.automatic = false;
    request.excitation.family = em::ModeFamily::TransverseElectric;
    request.excitation.m = 1;
    request.excitation.n = 0;
    request.settings.fem.mesh.maximum_element_size_m = 3.0e-3;
    // First order on purpose: the quoted 0.4 .. 3.0 degree phase errors and the
    // 0.84 percent slope deviation are first-order measurements, and the sweep
    // is four solves, so the order is the difference between 13 s and 53 s.
    request.settings.fem.mesh.element_order = 1;
    request.settings.fem.relative_tolerance = 1.0e-6;
    request.settings.fem.maximum_iterations = 4000;

    em::PecPlateGeometry short_circuit;
    short_circuit.enabled = true;
    short_circuit.center_m = {
        0.0,
        0.0,
        -0.5 * request.model.waveguide.length_m + distance_to_short_m,
    };
    // Below lambda / 100 the mesher builds the plate as a zero-thickness sheet,
    // so the reflecting plane is exactly the plate centre and the analytic
    // distance needs no thickness correction.
    short_circuit.size_m = {
        request.model.waveguide.inner_width_m,
        request.model.waveguide.inner_height_m,
        0.2e-3,
    };
    request.model.pec_plates.push_back(short_circuit);

    const em::FieldSolution solution =
        em::MfemFrequencyDomainBackend().solve(request, {});
    ShortCircuitReflection reflection;
    reflection.solved = solution.success && solution.field != nullptr;
    reflection.s11 = solution.scattering.s11;
    reflection.s21 = solution.scattering.s21;
    return reflection;
}

void testMfemTransverseShortCircuit()
{
    em::SimulationRequest reference_request = createRequest(10.0e9);
    reference_request.excitation.automatic = false;
    reference_request.excitation.family = em::ModeFamily::TransverseElectric;
    reference_request.excitation.m = 1;
    reference_request.excitation.n = 0;
    const em::FieldSolution reference =
        em::AnalyticWaveguideSolver().solve(reference_request);
    expectTrue(reference.success && reference.has_selected_mode,
               "analytic TE10 reference for the short-circuit test solves");
    if (!reference.has_selected_mode) {
        return;
    }
    const em::Complex gamma_per_m = reference.selected_mode.propagation_constant_per_m;
    const double analytic_beta_per_m = std::imag(gamma_per_m);

    const double distances_m[] = {5.0e-3, 10.0e-3, 15.0e-3, 20.0e-3};
    std::vector<double> measured_distances_m;
    std::vector<double> unwrapped_phase_rad;
    double previous_phase_rad = 0.0;

    for (const double distance_m : distances_m) {
        const ShortCircuitReflection reflection = solveTransverseShortCircuit(distance_m);
        const std::string label = std::to_string(distance_m * 1000.0) + " mm";
        expectTrue(reflection.solved,
                   "MFEM solves the transverse short circuit at " + label);
        if (!reflection.solved) {
            continue;
        }

        // Measured spread over the sweep: 0.001 .. 0.010 in magnitude, exactly
        // zero transmission (the short splits the mesh into two regions).
        expectNear(std::abs(reflection.s11), 1.0, 0.03,
                   "short-circuit |S11| stays unity at " + label);
        expectNear(std::abs(reflection.s21), 0.0, 0.01,
                   "short-circuit |S21| vanishes at " + label);

        const em::Complex expected_s11 = -std::exp(-2.0 * gamma_per_m * distance_m);
        double phase_error_rad =
            std::abs(std::arg(reflection.s11) - std::arg(expected_s11));
        phase_error_rad = std::min(phase_error_rad, 2.0 * em::pi - phase_error_rad);
        const double phase_error_deg = phase_error_rad * 180.0 / em::pi;
        // Referencing S11 to the wrong plane rotates it by 2 beta times the
        // wrong offset: de-embedding half the guide instead of the sampling
        // offset alone would already be 227 degrees off. Measured error over the
        // sweep is 0.4 .. 3.0 degrees.
        expectTrue(phase_error_deg < 8.0,
                   "short-circuit S11 phase matches -exp(-2 gamma d) at " + label + ": " +
                       std::to_string(phase_error_deg) + " deg");

        double phase_rad = std::arg(reflection.s11);
        if (!unwrapped_phase_rad.empty()) {
            // Consecutive distances shift the phase by less than pi, so the
            // branch follows from continuity alone and never from the analytic
            // propagation constant that the fit is supposed to validate.
            phase_rad += 2.0 * em::pi *
                         std::round((previous_phase_rad - phase_rad) / (2.0 * em::pi));
        }
        previous_phase_rad = phase_rad;
        measured_distances_m.push_back(distance_m);
        unwrapped_phase_rad.push_back(phase_rad);
    }

    expectTrue(measured_distances_m.size() >= 3,
               "the short-circuit phase sweep produced enough points to fit");
    if (measured_distances_m.size() < 3) {
        return;
    }

    double distance_sum_m = 0.0;
    double phase_sum_rad = 0.0;
    for (std::size_t index = 0; index < measured_distances_m.size(); ++index) {
        distance_sum_m += measured_distances_m[index];
        phase_sum_rad += unwrapped_phase_rad[index];
    }
    const double mean_distance_m = distance_sum_m / measured_distances_m.size();
    const double mean_phase_rad = phase_sum_rad / unwrapped_phase_rad.size();
    double covariance = 0.0;
    double variance = 0.0;
    for (std::size_t index = 0; index < measured_distances_m.size(); ++index) {
        const double distance_deviation_m = measured_distances_m[index] - mean_distance_m;
        covariance += distance_deviation_m * (unwrapped_phase_rad[index] - mean_phase_rad);
        variance += distance_deviation_m * distance_deviation_m;
    }
    const double slope_rad_per_m = covariance / variance;
    // The fit is insensitive to any constant port offset, so it isolates the
    // propagation constant the FEM mesh actually carries. Measured deviation at
    // this 3 mm mesh is 0.84 percent (159.6 against 158.2 rad/m).
    const double extracted_beta_per_m = -0.5 * slope_rad_per_m;
    expectNear(extracted_beta_per_m, analytic_beta_per_m,
               0.02 * analytic_beta_per_m,
               "short-circuit phase slope reproduces the TE10 phase constant");
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

em::SimulationRequest createCircularRequest(double frequency_hz, double radius_m)
{
    em::SimulationRequest request;
    request.frequency_hz = frequency_hz;
    request.model.waveguide.cross_section = em::WaveguideCrossSection::Circular;
    request.model.waveguide.inner_radius_m = radius_m;
    request.model.waveguide.length_m = 50.0e-3;
    request.model.waveguide.wall_thickness_m = 0.1e-3;
    request.settings.maximum_m = 3;
    request.settings.maximum_n = 3;
    request.settings.normalization_power_w = 1.0;
    return request;
}

void testCircularBesselRoots()
{
    // Табличные значения нулей J_m и J_m' (Abramowitz & Stegun, табл. 9.5).
    expectNear(em::besselJZero(0, 1), 2.4048255577, 1.0e-8, "p_01 = 2.40483");
    expectNear(em::besselJZero(0, 2), 5.5200781103, 1.0e-8, "p_02 = 5.52008");
    expectNear(em::besselJZero(1, 1), 3.8317059702, 1.0e-8, "p_11 = 3.83171");
    expectNear(em::besselJZero(2, 1), 5.1356223019, 1.0e-8, "p_21 = 5.13562");
    expectNear(em::besselJDerivativeZero(1, 1), 1.8411837814, 1.0e-8, "p'_11 = 1.84118");
    expectNear(em::besselJDerivativeZero(2, 1), 3.0542369282, 1.0e-8, "p'_21 = 3.05424");
    expectNear(em::besselJDerivativeZero(0, 1), 3.8317059702, 1.0e-8, "p'_01 = 3.83171");
    expectNear(em::besselJDerivativeZero(1, 2), 5.3314427735, 1.0e-8, "p'_12 = 5.33144");

    // Найденные корни действительно обнуляют свою функцию.
    expectNear(em::besselJ(0, em::besselJZero(0, 1)), 0.0, 1.0e-12, "J0 в своём нуле равен нулю");
    expectNear(em::besselJDerivative(1, em::besselJDerivativeZero(1, 1)),
               0.0,
               1.0e-12,
               "J1' в своём нуле равен нулю");
}

void testCircularWaveguideModes()
{
    // Радиус подобран так, чтобы на 10 ГГц распространялась только TE11:
    // f_c(TE11) = 1.8412 c / (2 pi a) = 8.79 ГГц, f_c(TM01) = 11.5 ГГц.
    const double radius_m = 10.0e-3;
    const em::SimulationRequest request = createCircularRequest(10.0e9, radius_m);
    const em::FieldSolution solution = em::AnalyticWaveguideSolver().solve(request);
    expectTrue(solution.success, "круглый волновод считается");
    expectTrue(solution.has_selected_mode, "мода круглого волновода выбрана");
    expectTrue(solution.selected_mode.family == em::ModeFamily::TransverseElectric &&
                   solution.selected_mode.m == 1 && solution.selected_mode.n == 1,
               "низшая мода круглого волновода — TE11");

    const double expected_te11_hz =
        1.8411837814 * em::speed_of_light_m_per_s / (2.0 * em::pi * radius_m);
    expectNear(solution.selected_mode.cutoff_frequency_hz,
               expected_te11_hz,
               expected_te11_hz * 1.0e-9,
               "частота отсечки TE11");

    // Следующая мода по частоте отсечки — TM01 с нулём J_0.
    const double expected_tm01_hz =
        2.4048255577 * em::speed_of_light_m_per_s / (2.0 * em::pi * radius_m);
    const auto tm01 = std::find_if(solution.available_modes.begin(),
                                   solution.available_modes.end(),
                                   [](const em::ModeDescriptor &mode) {
                                       return mode.family == em::ModeFamily::TransverseMagnetic &&
                                              mode.m == 0 && mode.n == 1;
                                   });
    expectTrue(tm01 != solution.available_modes.end(), "TM01 присутствует в наборе мод");
    if (tm01 != solution.available_modes.end()) {
        expectNear(tm01->cutoff_frequency_hz,
                   expected_tm01_hz,
                   expected_tm01_hz * 1.0e-9,
                   "частота отсечки TM01");
        expectTrue(!tm01->propagating, "TM01 на 10 ГГц ещё заперта");
    }

    // Мощность нормирована на 1 Вт: независимый полярный интеграл вектора
    // Пойнтинга по сечению.
    if (solution.field) {
        constexpr int radial_samples = 200;
        constexpr int azimuthal_samples = 240;
        const double dr_m = radius_m / radial_samples;
        const double dphi_rad = 2.0 * em::pi / azimuthal_samples;
        double power_w = 0.0;
        for (int radial_index = 0; radial_index < radial_samples; ++radial_index) {
            const double r_m = (radial_index + 0.5) * dr_m;
            for (int azimuthal_index = 0; azimuthal_index < azimuthal_samples;
                 ++azimuthal_index) {
                const double phi_rad = (azimuthal_index + 0.5) * dphi_rad;
                const em::FieldPhasor sample = solution.field->evaluate(
                    {r_m * std::cos(phi_rad), r_m * std::sin(phi_rad), -0.02});
                power_w += em::timeAveragePoynting(sample.electric_v_per_m,
                                                   sample.magnetic_a_per_m)
                               .z *
                           r_m * dr_m * dphi_rad;
            }
        }
        expectNear(power_w, 1.0, 5.0e-3, "мощность круглой моды нормирована на 1 Вт");
    }
}

void testCircularPecBoundaryAndMaxwell()
{
    const double radius_m = 10.0e-3;
    em::SimulationRequest request = createCircularRequest(10.0e9, radius_m);
    const em::FieldSolution solution = em::AnalyticWaveguideSolver().solve(request);
    if (!solution.field) {
        fail("круглая мода не дала поля");
        return;
    }

    // На идеально проводящей стенке тангенциальное электрическое поле (E_phi и
    // E_z) должно обращаться в ноль; нормальная составляющая E_r — нет.
    double maximum_tangential = 0.0;
    double maximum_total = 0.0;
    for (const double phi_rad : {0.3, 1.1, 2.7, 4.2, 5.6}) {
        const double sample_radius_m = radius_m * (1.0 - 1.0e-7);
        const em::FieldPhasor sample = solution.field->evaluate(
            {sample_radius_m * std::cos(phi_rad), sample_radius_m * std::sin(phi_rad), -0.013});
        const em::Complex azimuthal = -sample.electric_v_per_m.x * std::sin(phi_rad) +
                                      sample.electric_v_per_m.y * std::cos(phi_rad);
        const double tangential =
            std::sqrt(std::norm(azimuthal) + std::norm(sample.electric_v_per_m.z));
        maximum_tangential = std::max(maximum_tangential, tangential);
        maximum_total = std::max(maximum_total, em::magnitude(sample.electric_v_per_m));
    }
    expectTrue(maximum_total > 0.0, "поле у стенки круглого волновода не нулевое");
    expectTrue(maximum_tangential < 1.0e-6 * std::max(1.0, maximum_total),
               "тангенциальное E на стенке круглого волновода обращается в ноль");

    // Уравнения Максвелла в точке внутри сечения.
    const em::Vec3 position_m{0.0031, -0.0017, -0.0063};
    const double step_m = 2.0e-7;
    const em::FieldPhasor field = solution.field->evaluate(position_m);
    const em::Complex imaginary_unit(0.0, 1.0);
    const double omega = 2.0 * em::pi * request.frequency_hz;
    const em::ComplexVec3 curl_e = numericalCurlElectric(*solution.field, position_m, step_m);
    const em::ComplexVec3 curl_h = numericalCurlMagnetic(*solution.field, position_m, step_m);
    const em::ComplexVec3 faraday_term =
        scaled(field.magnetic_a_per_m, imaginary_unit * omega * em::vacuum_permeability_h_per_m);
    const em::ComplexVec3 ampere_term =
        scaled(field.electric_v_per_m, imaginary_unit * omega * em::vacuum_permittivity_f_per_m);
    const double faraday_scale = std::max(em::magnitude(curl_e), em::magnitude(faraday_term));
    const double ampere_scale = std::max(em::magnitude(curl_h), em::magnitude(ampere_term));
    expectTrue(em::magnitude(curl_e + faraday_term) < 1.0e-5 * faraday_scale,
               "закон Фарадея выполняется для круглой моды");
    expectTrue(em::magnitude(curl_h - ampere_term) < 1.0e-5 * ampere_scale,
               "закон Ампера выполняется для круглой моды");
}

void testCircularDispatchPolicy()
{
    em::EmSolverDispatcher dispatcher(std::make_shared<TestFemBackend>());

    const em::FieldSolution empty = dispatcher.solve(createCircularRequest(10.0e9, 10.0e-3));
    expectTrue(empty.success, "пустой круглый волновод считается диспетчером");
    expectTrue(empty.diagnostics.backend_name == "Analytic circular-waveguide TE/TM",
               "круглый волновод уходит в аналитический решатель");

    // Пластины в круглом сечении пока не поддержаны: диспетчер обязан отказать
    // с объяснением, а не молча посчитать прямоугольную геометрию в FEM.
    em::SimulationRequest with_plate = createCircularRequest(10.0e9, 10.0e-3);
    em::PecPlateGeometry plate;
    plate.enabled = true;
    plate.size_m = {5.0e-3, 5.0e-3, 0.5e-3};
    with_plate.model.pec_plates.push_back(plate);
    const em::FieldSolution rejected_plate = dispatcher.solve(with_plate);
    expectTrue(!rejected_plate.success && !rejected_plate.error_message.empty(),
               "пластина в круглом волноводе отклоняется с сообщением");

    // Явно выбранный метод, не работающий с круглым сечением, тоже отклоняется.
    em::SimulationRequest forced_fem = createCircularRequest(10.0e9, 10.0e-3);
    forced_fem.settings.solver_method = em::SolverMethod::FiniteElement;
    const em::FieldSolution rejected_fem = dispatcher.solve(forced_fem);
    expectTrue(!rejected_fem.success && !rejected_fem.error_message.empty(),
               "FEM для круглого волновода отклоняется с сообщением");
}

int main()
{
    testCircularBesselRoots();
    testCircularWaveguideModes();
    testCircularPecBoundaryAndMaxwell();
    testCircularDispatchPolicy();
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
    testArrowDensitySetting();
    testSliceOffsetAndResolution();
    testSlotCurrentMaskAndExcitationEstimate();
    testCooperativeCancellation();
    testSolverDispatchPolicy();
    testFullPecPartition();
    testFieldSliceAndAnimation();
    testPartitionSliceShadowAndPoynting();
    testIrisPlateWithAperture();
    testCircularIrisWithPost();
    testModeMatchingIris();
    runTest("testFemGeometryGeneration", testFemGeometryGeneration);
    testRefinementSwitchesOffCompletely();
    runTest("testMicroscopicPlateRotationKeepsRefinement",
            testMicroscopicPlateRotationKeepsRefinement);
    runTest("testZeroThicknessSheetKeepsItsEdgeRefinement",
            testZeroThicknessSheetKeepsItsEdgeRefinement);
    runTest("testCapacitiveGapFollowsTheFeatureKnob",
            testCapacitiveGapFollowsTheFeatureKnob);
    runTest("testPlateRefinementCost", testPlateRefinementCost);
#ifdef KRUTIEV_WITH_MFEM
    runTest("testMfemEmptyGuide", testMfemEmptyGuide);
#ifdef KRUTIEV_WITH_EIGEN
    runTest("testDirectAndIterativeSolversAgree", testDirectAndIterativeSolversAgree);
    runTest("testSecondOrderSolutionIsNotCalledActive",
            testSecondOrderSolutionIsNotCalledActive);
    runTest("testDefaultConfigurationSolvesWithTheDirectSolver",
            testDefaultConfigurationSolvesWithTheDirectSolver);
#endif
    testMfemSlotFringing();
    runTest("testMfemTransverseShortCircuit", testMfemTransverseShortCircuit);
    testMfemCenteredPecPost();
#endif

    // A build without MFEM or without Eigen silently drops whole groups, and the
    // remaining ones still finish with "All EM core tests passed" - measured at
    // 6.2 s against 396.9 s for the full binary, i.e. losing 98% of the coverage
    // looked exactly like success. The guards are visible from inside this file,
    // so the warning belongs here and works under either build system.
    const char *const compiled_out_groups[] = {
#ifndef KRUTIEV_WITH_MFEM
        "KRUTIEV_WITH_MFEM: every finite element test, including the ports, the "
        "short circuit and the slot",
#endif
#ifndef KRUTIEV_WITH_EIGEN
        "KRUTIEV_WITH_EIGEN: testDirectAndIterativeSolversAgree, the only check "
        "of the direct solver block layout",
#endif
        nullptr,
    };
    if (compiled_out_groups[0] != nullptr) {
        std::cout << "\n!!! TEST GROUPS COMPILED OUT OF THIS BINARY !!!\n";
        for (int index = 0; compiled_out_groups[index] != nullptr; ++index) {
            std::cout << "  * " << compiled_out_groups[index] << "\n";
        }
        std::cout << "  A pass of the remaining tests says nothing about the "
                     "groups above.\n\n";
    }

    if (failure_count == 0) {
        std::cout << "All EM core tests passed.\n";
        return EXIT_SUCCESS;
    }

    std::cerr << failure_count << " EM core test(s) failed.\n";
    return EXIT_FAILURE;
}
