#include "analytic_waveguide_solver.h"

#include "circular_mode_field.h"
#include "cylindrical_bessel.h"
#include "rectangular_mode_field.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <sstream>
#include <vector>

namespace em
{
namespace
{
constexpr double numerical_tolerance = 1.0e-12;

std::string modeName(const ModeDescriptor &mode)
{
    std::ostringstream stream;
    stream << (mode.family == ModeFamily::TransverseElectric ? "TE" : "TM")
           << mode.m << mode.n;
    return stream.str();
}

bool modeLess(const ModeDescriptor &left, const ModeDescriptor &right)
{
    if (std::abs(left.cutoff_frequency_hz - right.cutoff_frequency_hz) > 1.0e-6) {
        return left.cutoff_frequency_hz < right.cutoff_frequency_hz;
    }

    return left.family == ModeFamily::TransverseElectric &&
           right.family == ModeFamily::TransverseMagnetic;
}

Complex passiveSquareRoot(const Complex &value)
{
    Complex root = std::sqrt(value);
    if (std::real(root) < 0.0 ||
        (std::abs(std::real(root)) < numerical_tolerance && std::imag(root) < 0.0)) {
        root = -root;
    }
    return root;
}

bool finiteComplex(const Complex &value)
{
    return std::isfinite(std::real(value)) && std::isfinite(std::imag(value));
}

// Квадратура по сечению: прямоугольник разбивается равномерной сеткой,
// круг — полярной (r, phi) сеткой, где вес ячейки r * dr * dphi. Полярная
// сетка предпочтительнее декартовой с отбраковкой: она точно покрывает круг и
// не даёт «ступенчатую» границу, из-за которой интеграл мощности сходился бы
// заметно медленнее.
struct QuadratureSample
{
    double x_m = 0.0;
    double y_m = 0.0;
    double weight_m2 = 0.0;
};

std::vector<QuadratureSample> crossSectionQuadrature(const WaveguideGeometry &geometry)
{
    std::vector<QuadratureSample> samples;
    if (isCircular(geometry)) {
        constexpr int radial_samples = 64;
        constexpr int azimuthal_samples = 128;
        const double dr_m = geometry.inner_radius_m / radial_samples;
        const double dphi_rad = 2.0 * pi / azimuthal_samples;
        samples.reserve(static_cast<size_t>(radial_samples) * azimuthal_samples);
        for (int radial_index = 0; radial_index < radial_samples; ++radial_index) {
            const double radius_m = (radial_index + 0.5) * dr_m;
            const double weight_m2 = radius_m * dr_m * dphi_rad;
            for (int azimuthal_index = 0; azimuthal_index < azimuthal_samples;
                 ++azimuthal_index) {
                const double azimuth_rad = (azimuthal_index + 0.5) * dphi_rad;
                samples.push_back({radius_m * std::cos(azimuth_rad),
                                   radius_m * std::sin(azimuth_rad),
                                   weight_m2});
            }
        }
        return samples;
    }

    constexpr int x_samples = 80;
    constexpr int y_samples = 56;
    const double dx_m = geometry.inner_width_m / x_samples;
    const double dy_m = geometry.inner_height_m / y_samples;
    samples.reserve(static_cast<size_t>(x_samples) * y_samples);
    for (int x_index = 0; x_index < x_samples; ++x_index) {
        const double x_m = -0.5 * geometry.inner_width_m + (x_index + 0.5) * dx_m;
        for (int y_index = 0; y_index < y_samples; ++y_index) {
            const double y_m = -0.5 * geometry.inner_height_m + (y_index + 0.5) * dy_m;
            samples.push_back({x_m, y_m, dx_m * dy_m});
        }
    }
    return samples;
}

// Точки на стенке для контурного интеграла |H_tangential|^2. Касательных две:
// лежащая в плоскости сечения (даётся здесь) и продольная z, общая для всех
// стенок.
struct WallSample
{
    double x_m = 0.0;
    double y_m = 0.0;
    Vec3 in_plane_tangent;
    double weight_m = 0.0;
};

std::vector<WallSample> wallQuadrature(const WaveguideGeometry &geometry)
{
    std::vector<WallSample> samples;
    // Точки сдвигаются внутрь на малую долю размера: ровно на стенке
    // вычислитель поля стоит на границе своей области определения.
    if (isCircular(geometry)) {
        constexpr int azimuthal_samples = 256;
        const double inset_m = std::max(1.0e-9, geometry.inner_radius_m * 1.0e-6);
        const double radius_m = geometry.inner_radius_m - inset_m;
        const double dphi_rad = 2.0 * pi / azimuthal_samples;
        const double weight_m = geometry.inner_radius_m * dphi_rad;
        samples.reserve(azimuthal_samples);
        for (int index = 0; index < azimuthal_samples; ++index) {
            const double azimuth_rad = (index + 0.5) * dphi_rad;
            samples.push_back({radius_m * std::cos(azimuth_rad),
                               radius_m * std::sin(azimuth_rad),
                               Vec3{-std::sin(azimuth_rad), std::cos(azimuth_rad), 0.0},
                               weight_m});
        }
        return samples;
    }

    constexpr int x_samples = 80;
    constexpr int y_samples = 56;
    const double dx_m = geometry.inner_width_m / x_samples;
    const double dy_m = geometry.inner_height_m / y_samples;
    const double inset_m =
        std::max(1.0e-9, std::min(geometry.inner_width_m, geometry.inner_height_m) * 1.0e-6);
    samples.reserve(2 * (x_samples + y_samples));
    for (int x_index = 0; x_index < x_samples; ++x_index) {
        const double x_m = -0.5 * geometry.inner_width_m + (x_index + 0.5) * dx_m;
        for (const double wall_y_m : {0.5 * geometry.inner_height_m - inset_m,
                                      -0.5 * geometry.inner_height_m + inset_m}) {
            samples.push_back({x_m, wall_y_m, Vec3{1.0, 0.0, 0.0}, dx_m});
        }
    }
    for (int y_index = 0; y_index < y_samples; ++y_index) {
        const double y_m = -0.5 * geometry.inner_height_m + (y_index + 0.5) * dy_m;
        for (const double wall_x_m : {0.5 * geometry.inner_width_m - inset_m,
                                      -0.5 * geometry.inner_width_m + inset_m}) {
            samples.push_back({wall_x_m, y_m, Vec3{0.0, 1.0, 0.0}, dy_m});
        }
    }
    return samples;
}

double integrateForwardPower(const IFieldEvaluator &field,
                             const WaveguideGeometry &geometry,
                             double z_m,
                             const SolveControl &control)
{
    double power_w = 0.0;
    for (const QuadratureSample &sample : crossSectionQuadrature(geometry)) {
        if (control.isCancellationRequested()) {
            return std::numeric_limits<double>::quiet_NaN();
        }
        const FieldPhasor value = field.evaluate({sample.x_m, sample.y_m, z_m});
        power_w += timeAveragePoynting(value.electric_v_per_m, value.magnetic_a_per_m).z *
                   sample.weight_m2;
    }
    return power_w;
}

bool matchesSelection(const ModeDescriptor &mode, const ModeSelection &selection)
{
    return mode.family == selection.family && mode.m == selection.m && mode.n == selection.n;
}

struct GuideEnergetics
{
    double perimeter_tangential_h2 = 0.0;  // integral of |H_tan|^2 around the walls
    double cross_section_e2 = 0.0;         // integral of |E|^2 over the cross-section
    double cross_section_h2 = 0.0;         // integral of |H|^2 over the cross-section
    double axial_power_w = 0.0;            // transmitted power through the plane
    bool finite = true;
};

// Cross-section and wall integrals used by the perturbation conductor-loss and
// stored-energy formulas, evaluated on the transverse plane at z_m.
GuideEnergetics integrateEnergetics(const IFieldEvaluator &field,
                                    const WaveguideGeometry &geometry,
                                    double z_m,
                                    const SolveControl &control)
{
    GuideEnergetics result;
    for (const QuadratureSample &sample : crossSectionQuadrature(geometry)) {
        if (control.isCancellationRequested()) {
            result.finite = false;
            return result;
        }
        const FieldPhasor value = field.evaluate({sample.x_m, sample.y_m, z_m});
        const double e2 = std::norm(value.electric_v_per_m.x) +
                          std::norm(value.electric_v_per_m.y) +
                          std::norm(value.electric_v_per_m.z);
        const double h2 = std::norm(value.magnetic_a_per_m.x) +
                          std::norm(value.magnetic_a_per_m.y) +
                          std::norm(value.magnetic_a_per_m.z);
        result.cross_section_e2 += e2 * sample.weight_m2;
        result.cross_section_h2 += h2 * sample.weight_m2;
        result.axial_power_w +=
            timeAveragePoynting(value.electric_v_per_m, value.magnetic_a_per_m).z *
            sample.weight_m2;
    }

    for (const WallSample &sample : wallQuadrature(geometry)) {
        const ComplexVec3 magnetic =
            field.evaluate({sample.x_m, sample.y_m, z_m}).magnetic_a_per_m;
        const Complex tangential = magnetic.x * sample.in_plane_tangent.x +
                                   magnetic.y * sample.in_plane_tangent.y;
        result.perimeter_tangential_h2 +=
            (std::norm(tangential) + std::norm(magnetic.z)) * sample.weight_m;
    }
    return result;
}
}

std::vector<ModeDescriptor> enumerateWaveguideModes(const SimulationRequest &request,
                                                    const SolveControl &control)
{
    std::vector<ModeDescriptor> modes;
    const WaveguideGeometry &geometry = request.model.waveguide;
    const Material &material = request.model.filling_material;
    const double relative_permittivity = std::max(numerical_tolerance,
                                                   std::real(material.relative_permittivity));
    const double relative_permeability = std::max(numerical_tolerance,
                                                   std::real(material.relative_permeability));
    const double material_wave_speed_m_per_s =
        speed_of_light_m_per_s /
        std::sqrt(relative_permittivity * relative_permeability);
    const double angular_frequency_rad_per_s = 2.0 * pi * request.frequency_hz;
    const Complex permeability_h_per_m =
        vacuum_permeability_h_per_m * material.relative_permeability;
    const Complex permittivity_f_per_m =
        vacuum_permittivity_f_per_m * material.relative_permittivity -
        Complex(0.0, material.conductivity_s_per_m / angular_frequency_rad_per_s);
    const Complex medium_wavenumber_squared_per_m2 =
        angular_frequency_rad_per_s * angular_frequency_rad_per_s *
        permeability_h_per_m * permittivity_f_per_m;
    const int maximum_m = std::max(request.settings.maximum_m,
                                   request.excitation.automatic ? 1 : request.excitation.m);
    const int maximum_n = std::max(request.settings.maximum_n,
                                   request.excitation.automatic ? 1 : request.excitation.n);

    // Общая часть: по критическому волновому числу строятся частота отсечки и
    // постоянная распространения. Отличается только набор k_c.
    const auto make_mode = [&](ModeFamily family, int m, int n, double cutoff_wavenumber_per_m) {
        ModeDescriptor mode;
        mode.family = family;
        mode.m = m;
        mode.n = n;
        mode.cutoff_wavenumber_per_m = cutoff_wavenumber_per_m;
        mode.cutoff_frequency_hz =
            material_wave_speed_m_per_s * cutoff_wavenumber_per_m / (2.0 * pi);
        mode.propagation_constant_per_m =
            passiveSquareRoot(cutoff_wavenumber_per_m * cutoff_wavenumber_per_m -
                              medium_wavenumber_squared_per_m2);
        mode.propagating = request.frequency_hz > mode.cutoff_frequency_hz;
        return mode;
    };

    if (isCircular(geometry)) {
        // Круглый волновод: k_c = p'_mn / a для TE и p_mn / a для TM, где p'_mn
        // и p_mn — нули J_m' и J_m. Азимутальный индекс m считается от нуля, а
        // радиальный n — от единицы, поэтому низшая мода это TE11.
        const double radius_m = geometry.inner_radius_m;
        for (int m = 0; m <= maximum_m; ++m) {
            if (control.isCancellationRequested()) {
                break;
            }
            for (int n = 1; n <= std::max(1, maximum_n); ++n) {
                const double te_root = besselJDerivativeZero(m, n);
                if (te_root > 0.0) {
                    modes.push_back(make_mode(ModeFamily::TransverseElectric,
                                              m,
                                              n,
                                              te_root / radius_m));
                }
                const double tm_root = besselJZero(m, n);
                if (tm_root > 0.0) {
                    modes.push_back(make_mode(ModeFamily::TransverseMagnetic,
                                              m,
                                              n,
                                              tm_root / radius_m));
                }
            }
        }
        std::sort(modes.begin(), modes.end(), modeLess);
        return modes;
    }

    for (int m = 0; m <= maximum_m; ++m) {
        if (control.isCancellationRequested()) {
            break;
        }
        for (int n = 0; n <= maximum_n; ++n) {
            if (m == 0 && n == 0) {
                continue;
            }

            const double kx_per_m = m * pi / geometry.inner_width_m;
            const double ky_per_m = n * pi / geometry.inner_height_m;
            const double cutoff_wavenumber_per_m =
                std::sqrt(kx_per_m * kx_per_m + ky_per_m * ky_per_m);
            modes.push_back(
                make_mode(ModeFamily::TransverseElectric, m, n, cutoff_wavenumber_per_m));

            if (m > 0 && n > 0) {
                modes.push_back(
                    make_mode(ModeFamily::TransverseMagnetic, m, n, cutoff_wavenumber_per_m));
            }
        }
    }

    std::sort(modes.begin(), modes.end(), modeLess);
    return modes;
}

FieldSolution AnalyticWaveguideSolver::solve(const SimulationRequest &request,
                                                const SolveControl &control) const
{
    FieldSolution solution;
    solution.request = request;
    solution.diagnostics.backend_name = "Analytic rectangular-waveguide TE/TM";

    const auto cancel = [&solution]() {
        solution.success = false;
        solution.cancelled = true;
        solution.has_selected_mode = false;
        solution.forward_longitudinal_amplitude = 0.0;
        solution.backward_longitudinal_amplitude = 0.0;
        solution.field.reset();
        solution.error_message = "Calculation cancelled.";
        return solution;
    };
    if (control.isCancellationRequested()) {
        return cancel();
    }

    const WaveguideGeometry &geometry = request.model.waveguide;
    const bool circular = isCircular(geometry);
    solution.diagnostics.backend_name = circular
                                            ? "Analytic circular-waveguide TE/TM"
                                            : "Analytic rectangular-waveguide TE/TM";
    const bool cross_section_valid =
        circular ? (std::isfinite(geometry.inner_radius_m) && geometry.inner_radius_m > 0.0)
                 : (std::isfinite(geometry.inner_width_m) &&
                    std::isfinite(geometry.inner_height_m) &&
                    geometry.inner_width_m > 0.0 && geometry.inner_height_m > 0.0);
    if (!cross_section_valid || !std::isfinite(geometry.length_m) ||
        !std::isfinite(geometry.wall_thickness_m) ||
        geometry.length_m <= 0.0 || geometry.wall_thickness_m < 0.0) {
        solution.error_message = "Waveguide inner dimensions and length must be positive.";
        return solution;
    }
    if (!std::isfinite(request.frequency_hz) || request.frequency_hz <= 0.0) {
        solution.error_message = "Frequency must be positive.";
        return solution;
    }
    const Material &material = request.model.filling_material;
    if (!finiteComplex(material.relative_permittivity) ||
        !finiteComplex(material.relative_permeability) ||
        !std::isfinite(material.conductivity_s_per_m) ||
        std::real(material.relative_permittivity) <= 0.0 ||
        std::real(material.relative_permeability) <= 0.0 ||
        std::imag(material.relative_permittivity) > numerical_tolerance ||
        std::imag(material.relative_permeability) > numerical_tolerance ||
        material.conductivity_s_per_m < 0.0) {
        solution.error_message = "Material parameters are not passive positive media.";
        return solution;
    }
    if (!std::isfinite(request.settings.normalization_power_w) ||
        request.settings.normalization_power_w <= 0.0 ||
        request.settings.maximum_m < 0 || request.settings.maximum_n < 0) {
        solution.error_message = "Mode limits and normalization power are invalid.";
        return solution;
    }
    // Допустимые пары индексов у сечений разные: в круглом волноводе радиальный
    // индекс n начинается с единицы, а азимутальный m = 0 разрешён обоим
    // семействам (например, TE01 и TM01 — реальные моды круглого волновода).
    const bool invalid_indices =
        circular ? (request.excitation.m < 0 || request.excitation.n < 1)
                 : (request.excitation.m < 0 || request.excitation.n < 0 ||
                    (request.excitation.family == ModeFamily::TransverseElectric &&
                     request.excitation.m == 0 && request.excitation.n == 0) ||
                    (request.excitation.family == ModeFamily::TransverseMagnetic &&
                     (request.excitation.m == 0 || request.excitation.n == 0)));
    if (!request.excitation.automatic && invalid_indices) {
        solution.error_message = "The requested TE/TM mode indices are invalid.";
        return solution;
    }

    solution.available_modes = enumerateWaveguideModes(request, control);
    if (control.isCancellationRequested()) {
        return cancel();
    }
    const auto selected_iterator = std::find_if(
        solution.available_modes.begin(),
        solution.available_modes.end(),
        [&request](const ModeDescriptor &mode) {
            return request.excitation.automatic ? mode.propagating
                                                : matchesSelection(mode, request.excitation);
        });

    solution.success = true;
    if (selected_iterator == solution.available_modes.end()) {
        solution.diagnostics.warnings.push_back(
            request.excitation.automatic
                ? "No propagating mode exists at the requested frequency."
                : "The requested mode is outside the enumerated mode set.");
        return solution;
    }

    solution.selected_mode = *selected_iterator;
    solution.has_selected_mode = true;
    if (!solution.selected_mode.propagating) {
        solution.diagnostics.warnings.push_back(
            modeName(solution.selected_mode) + " is evanescent at the requested frequency.");
    }

    const auto make_field = [&request, circular](const ModeDescriptor &mode, Complex amplitude) {
        return circular ? std::static_pointer_cast<const IFieldEvaluator>(
                              std::make_shared<CircularModeFieldEvaluator>(request,
                                                                           mode,
                                                                           amplitude))
                        : std::static_pointer_cast<const IFieldEvaluator>(
                              std::make_shared<RectangularModeFieldEvaluator>(request,
                                                                              mode,
                                                                              amplitude));
    };

    const std::shared_ptr<const IFieldEvaluator> unit_field =
        make_field(solution.selected_mode, 1.0);
    const double input_z_m = -0.5 * geometry.length_m;
    const GuideEnergetics unit_energetics =
        integrateEnergetics(*unit_field, geometry, input_z_m, control);
    if (control.isCancellationRequested() || !unit_energetics.finite) {
        return cancel();
    }
    const double unit_power_w = unit_energetics.axial_power_w;

    const double angular_frequency_rad_per_s = 2.0 * pi * request.frequency_hz;
    const Material &loss_material = request.model.filling_material;
    const Complex permeability_h_per_m =
        vacuum_permeability_h_per_m * loss_material.relative_permeability;
    const Complex permittivity_f_per_m =
        vacuum_permittivity_f_per_m * loss_material.relative_permittivity -
        Complex(0.0, loss_material.conductivity_s_per_m / angular_frequency_rad_per_s);

    double amplitude = 1.0;
    double conductor_attenuation_np_per_m = 0.0;
    if (solution.selected_mode.propagating) {
        if (!(unit_power_w > numerical_tolerance) || !std::isfinite(unit_power_w)) {
            solution.success = false;
            solution.error_message = "Unable to normalize the selected mode to real power.";
            solution.has_selected_mode = false;
            return solution;
        }
        amplitude = std::sqrt(request.settings.normalization_power_w / unit_power_w);

        // Perturbation conductor loss: alpha_c = P_wall / (2 P_transmitted),
        // where P_wall = (R_s / 2) * closed-loop integral of |H_tangential|^2 and
        // R_s = sqrt(omega * mu / (2 * sigma_wall)) is the surface resistance.
        // The ratio is amplitude-independent, so it is taken from the unit field.
        if (geometry.wall_conductivity_s_per_m > 0.0) {
            const double surface_resistance_ohm =
                std::sqrt(angular_frequency_rad_per_s * std::real(permeability_h_per_m) /
                          (2.0 * geometry.wall_conductivity_s_per_m));
            const double wall_loss_per_length_w =
                0.5 * surface_resistance_ohm * unit_energetics.perimeter_tangential_h2;
            const double candidate = wall_loss_per_length_w / (2.0 * unit_power_w);
            if (std::isfinite(candidate) && candidate > 0.0) {
                conductor_attenuation_np_per_m = candidate;
                solution.selected_mode.propagation_constant_per_m +=
                    Complex(conductor_attenuation_np_per_m, 0.0);
            }
        }
    }
    solution.diagnostics.conductor_attenuation_np_per_m = conductor_attenuation_np_per_m;

    solution.field = make_field(solution.selected_mode, amplitude);
    solution.forward_longitudinal_amplitude = amplitude;
    solution.diagnostics.input_power_w =
        integrateForwardPower(*solution.field, geometry, input_z_m, control);
    solution.diagnostics.output_power_w =
        integrateForwardPower(*solution.field,
                              geometry,
                              0.5 * geometry.length_m,
                              control);
    if (control.isCancellationRequested()) {
        return cancel();
    }

    const Complex transmission =
        std::exp(-solution.selected_mode.propagation_constant_per_m * geometry.length_m);
    solution.scattering.s12 = transmission;
    solution.scattering.s21 = transmission;
    const double expected_output_power_w =
        solution.diagnostics.input_power_w * std::norm(transmission);
    solution.diagnostics.incident_power_w = solution.diagnostics.input_power_w;
    solution.diagnostics.reflected_power_w = 0.0;
    solution.diagnostics.transmitted_power_w = solution.diagnostics.output_power_w;
    solution.diagnostics.dissipated_power_w =
        std::max(0.0,
                 solution.diagnostics.incident_power_w -
                     solution.diagnostics.transmitted_power_w);
    const double scattering_power_error =
        std::abs(solution.diagnostics.output_power_w - expected_output_power_w) /
        std::max(numerical_tolerance, std::abs(expected_output_power_w));
    const double conservation_error =
        std::abs(solution.diagnostics.incident_power_w -
                 solution.diagnostics.reflected_power_w -
                 solution.diagnostics.transmitted_power_w -
                 solution.diagnostics.dissipated_power_w) /
        std::max(numerical_tolerance,
                 std::abs(solution.diagnostics.incident_power_w));
    solution.diagnostics.power_balance_relative_error =
        std::max(scattering_power_error, conservation_error);

    // Stored energy (for the normalized power level) and the loss-limited quality
    // factor Q = omega * (W_e + W_m) / P_loss, with P_loss = 2 * alpha_total * P_0.
    if (solution.selected_mode.propagating) {
        const double amplitude_squared = amplitude * amplitude;
        const double electric_energy_per_length_j_per_m =
            0.25 * std::real(permittivity_f_per_m) * amplitude_squared *
            unit_energetics.cross_section_e2;
        const double magnetic_energy_per_length_j_per_m =
            0.25 * std::real(permeability_h_per_m) * amplitude_squared *
            unit_energetics.cross_section_h2;
        solution.diagnostics.stored_electric_energy_j =
            electric_energy_per_length_j_per_m * geometry.length_m;
        solution.diagnostics.stored_magnetic_energy_j =
            magnetic_energy_per_length_j_per_m * geometry.length_m;
        const double total_attenuation_np_per_m =
            std::real(solution.selected_mode.propagation_constant_per_m);
        const double reference_power_w =
            std::max(numerical_tolerance, request.settings.normalization_power_w);
        if (total_attenuation_np_per_m > numerical_tolerance) {
            solution.diagnostics.quality_factor =
                angular_frequency_rad_per_s *
                (electric_energy_per_length_j_per_m + magnetic_energy_per_length_j_per_m) /
                (2.0 * total_attenuation_np_per_m * reference_power_w);
        }
    }

    const bool has_enabled_slots =
        std::any_of(request.model.slot_geometries.begin(),
                    request.model.slot_geometries.end(),
                    [](const SlotGeometry &slot) { return slot.enabled; });
    if (has_enabled_slots) {
        solution.diagnostics.warnings.push_back(
            "Slots are present in the model but are not part of the analytic empty-guide backend.");
    }
    const bool has_enabled_plates =
        std::any_of(request.model.pec_plates.begin(),
                    request.model.pec_plates.end(),
                    [](const PecPlateGeometry &plate) { return plate.enabled; });
    if (has_enabled_plates) {
        solution.diagnostics.warnings.push_back(
            "PEC plates are present in the model but require mode matching or the FEM backend.");
    }

    return solution;
}
}
