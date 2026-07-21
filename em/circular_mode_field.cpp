#include "circular_mode_field.h"

#include "cylindrical_bessel.h"

#include <algorithm>
#include <cmath>

namespace em
{
namespace
{
constexpr double geometry_tolerance_m = 1.0e-12;
}

CircularModeFieldEvaluator::CircularModeFieldEvaluator(
    const SimulationRequest &request,
    const ModeDescriptor &mode,
    Complex forward_longitudinal_amplitude,
    Complex backward_longitudinal_amplitude,
    std::optional<AxialFieldRegion> active_region)
    : geometry_(request.model.waveguide),
      mode_(mode),
      angular_frequency_rad_per_s_(2.0 * pi * request.frequency_hz),
      forward_longitudinal_amplitude_(forward_longitudinal_amplitude),
      backward_longitudinal_amplitude_(backward_longitudinal_amplitude),
      cutoff_wavenumber_per_m_(mode.cutoff_wavenumber_per_m),
      active_region_(active_region)
{
    const Material &material = request.model.filling_material;
    permeability_h_per_m_ = vacuum_permeability_h_per_m * material.relative_permeability;
    permittivity_f_per_m_ = vacuum_permittivity_f_per_m * material.relative_permittivity -
                            Complex(0.0,
                                    material.conductivity_s_per_m /
                                        angular_frequency_rad_per_s_);
    cutoff_wavenumber_squared_per_m2_ = cutoff_wavenumber_per_m_ * cutoff_wavenumber_per_m_;
}

bool CircularModeFieldEvaluator::contains(const Vec3 &position_m) const
{
    return insideCrossSection(geometry_, position_m.x, position_m.y, geometry_tolerance_m) &&
           position_m.z >= -0.5 * geometry_.length_m - geometry_tolerance_m &&
           position_m.z <= 0.5 * geometry_.length_m + geometry_tolerance_m;
}

FieldPhasor CircularModeFieldEvaluator::evaluate(const Vec3 &position_m) const
{
    if (!contains(position_m) || cutoff_wavenumber_squared_per_m2_ <= 0.0) {
        return {};
    }
    if (active_region_ &&
        (position_m.z < active_region_->minimum_z_m - geometry_tolerance_m ||
         position_m.z > active_region_->maximum_z_m + geometry_tolerance_m)) {
        return {};
    }

    // Множители вида m/r на оси сингулярны, хотя само поле там конечно: J_m(k r)
    // обращается в ноль как r^m. Радиус подпирается снизу малой долей радиуса
    // волновода, чтобы отношение оставалось численно устойчивым.
    const double minimum_radius_m = std::max(1.0e-12, geometry_.inner_radius_m * 1.0e-9);
    const double radius_m = std::max(minimum_radius_m,
                                     std::hypot(position_m.x, position_m.y));
    const double azimuth_rad = std::atan2(position_m.y, position_m.x);
    const double distance_from_input_m = position_m.z + 0.5 * geometry_.length_m;

    const int order = mode_.m;
    const double bessel = besselJ(order, cutoff_wavenumber_per_m_ * radius_m);
    const double bessel_derivative =
        besselJDerivative(order, cutoff_wavenumber_per_m_ * radius_m);
    const double cos_azimuth = std::cos(order * azimuth_rad);
    const double sin_azimuth = std::sin(order * azimuth_rad);

    const Complex forward_factor =
        forward_longitudinal_amplitude_ *
        std::exp(-mode_.propagation_constant_per_m * distance_from_input_m);
    const Complex backward_factor =
        backward_longitudinal_amplitude_ *
        std::exp(mode_.propagation_constant_per_m * distance_from_input_m);
    const Complex axial_factor = forward_factor + backward_factor;
    const Complex axial_derivative = mode_.propagation_constant_per_m *
                                     (backward_factor - forward_factor);
    const Complex imaginary_unit(0.0, 1.0);

    // Продольная функция psi = J_m(k_c r) cos(m phi) и её поперечный градиент в
    // цилиндрических координатах. Формулы те же, что в декартовом случае, с
    // заменой d/dx -> d/dr и d/dy -> (1/r) d/dphi.
    const double pattern = bessel * cos_azimuth;
    const double gradient_radial = cutoff_wavenumber_per_m_ * bessel_derivative * cos_azimuth;
    const double gradient_azimuthal = -static_cast<double>(order) * bessel * sin_azimuth /
                                      radius_m;

    Complex radial_component = 0.0;
    Complex azimuthal_component = 0.0;
    FieldPhasor field;
    if (mode_.family == ModeFamily::TransverseElectric) {
        field.magnetic_a_per_m.z = axial_factor * pattern;
        radial_component = axial_derivative * gradient_radial /
                           cutoff_wavenumber_squared_per_m2_;
        azimuthal_component = axial_derivative * gradient_azimuthal /
                              cutoff_wavenumber_squared_per_m2_;
        const Complex electric_scale = imaginary_unit * angular_frequency_rad_per_s_ *
                                       permeability_h_per_m_ /
                                       cutoff_wavenumber_squared_per_m2_;
        const Complex electric_radial = -electric_scale * axial_factor * gradient_azimuthal;
        const Complex electric_azimuthal = electric_scale * axial_factor * gradient_radial;

        field.electric_v_per_m.x = electric_radial * std::cos(azimuth_rad) -
                                   electric_azimuthal * std::sin(azimuth_rad);
        field.electric_v_per_m.y = electric_radial * std::sin(azimuth_rad) +
                                   electric_azimuthal * std::cos(azimuth_rad);
        field.magnetic_a_per_m.x = radial_component * std::cos(azimuth_rad) -
                                   azimuthal_component * std::sin(azimuth_rad);
        field.magnetic_a_per_m.y = radial_component * std::sin(azimuth_rad) +
                                   azimuthal_component * std::cos(azimuth_rad);
    } else {
        field.electric_v_per_m.z = axial_factor * pattern;
        radial_component = axial_derivative * gradient_radial /
                           cutoff_wavenumber_squared_per_m2_;
        azimuthal_component = axial_derivative * gradient_azimuthal /
                              cutoff_wavenumber_squared_per_m2_;
        const Complex magnetic_scale = imaginary_unit * angular_frequency_rad_per_s_ *
                                       permittivity_f_per_m_ /
                                       cutoff_wavenumber_squared_per_m2_;
        const Complex magnetic_radial = magnetic_scale * axial_factor * gradient_azimuthal;
        const Complex magnetic_azimuthal = -magnetic_scale * axial_factor * gradient_radial;

        field.electric_v_per_m.x = radial_component * std::cos(azimuth_rad) -
                                   azimuthal_component * std::sin(azimuth_rad);
        field.electric_v_per_m.y = radial_component * std::sin(azimuth_rad) +
                                   azimuthal_component * std::cos(azimuth_rad);
        field.magnetic_a_per_m.x = magnetic_radial * std::cos(azimuth_rad) -
                                   magnetic_azimuthal * std::sin(azimuth_rad);
        field.magnetic_a_per_m.y = magnetic_radial * std::sin(azimuth_rad) +
                                   magnetic_azimuthal * std::cos(azimuth_rad);
    }

    return field;
}
}
