#include "rectangular_mode_field.h"

#include <algorithm>
#include <cmath>

namespace em
{
namespace
{
constexpr double geometry_tolerance_m = 1.0e-12;
}

RectangularModeFieldEvaluator::RectangularModeFieldEvaluator(
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
      active_region_(active_region)
{
    const Material &material = request.model.filling_material;
    permeability_h_per_m_ = vacuum_permeability_h_per_m * material.relative_permeability;
    permittivity_f_per_m_ = vacuum_permittivity_f_per_m * material.relative_permittivity -
                            Complex(0.0,
                                    material.conductivity_s_per_m /
                                        angular_frequency_rad_per_s_);
    kx_per_m_ = mode.m * pi / geometry_.inner_width_m;
    ky_per_m_ = mode.n * pi / geometry_.inner_height_m;
    cutoff_wavenumber_squared_per_m2_ = kx_per_m_ * kx_per_m_ +
                                         ky_per_m_ * ky_per_m_;
}

bool RectangularModeFieldEvaluator::contains(const Vec3 &position_m) const
{
    return position_m.x >= -0.5 * geometry_.inner_width_m - geometry_tolerance_m &&
           position_m.x <= 0.5 * geometry_.inner_width_m + geometry_tolerance_m &&
           position_m.y >= -0.5 * geometry_.inner_height_m - geometry_tolerance_m &&
           position_m.y <= 0.5 * geometry_.inner_height_m + geometry_tolerance_m &&
           position_m.z >= -0.5 * geometry_.length_m - geometry_tolerance_m &&
           position_m.z <= 0.5 * geometry_.length_m + geometry_tolerance_m;
}

FieldPhasor RectangularModeFieldEvaluator::evaluate(const Vec3 &position_m) const
{
    if (!contains(position_m) || cutoff_wavenumber_squared_per_m2_ <= 0.0) {
        return {};
    }
    if (active_region_ &&
        (position_m.z < active_region_->minimum_z_m - geometry_tolerance_m ||
         position_m.z > active_region_->maximum_z_m + geometry_tolerance_m)) {
        return {};
    }

    const double local_x_m = position_m.x + 0.5 * geometry_.inner_width_m;
    const double local_y_m = position_m.y + 0.5 * geometry_.inner_height_m;
    const double distance_from_input_m = position_m.z + 0.5 * geometry_.length_m;
    const double sin_x = std::sin(kx_per_m_ * local_x_m);
    const double cos_x = std::cos(kx_per_m_ * local_x_m);
    const double sin_y = std::sin(ky_per_m_ * local_y_m);
    const double cos_y = std::cos(ky_per_m_ * local_y_m);
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

    FieldPhasor field;
    if (mode_.family == ModeFamily::TransverseElectric) {
        const Complex transverse_derivative_x = -axial_factor * kx_per_m_ * sin_x * cos_y;
        const Complex transverse_derivative_y = -axial_factor * ky_per_m_ * cos_x * sin_y;
        const Complex mixed_derivative_x = -axial_derivative * kx_per_m_ * sin_x * cos_y;
        const Complex mixed_derivative_y = -axial_derivative * ky_per_m_ * cos_x * sin_y;

        field.electric_v_per_m.x =
            -imaginary_unit * angular_frequency_rad_per_s_ * permeability_h_per_m_ /
            cutoff_wavenumber_squared_per_m2_ * transverse_derivative_y;
        field.electric_v_per_m.y =
            imaginary_unit * angular_frequency_rad_per_s_ * permeability_h_per_m_ /
            cutoff_wavenumber_squared_per_m2_ * transverse_derivative_x;
        field.magnetic_a_per_m.x = mixed_derivative_x /
                                   cutoff_wavenumber_squared_per_m2_;
        field.magnetic_a_per_m.y = mixed_derivative_y /
                                   cutoff_wavenumber_squared_per_m2_;
        field.magnetic_a_per_m.z = axial_factor * cos_x * cos_y;
    } else {
        const Complex transverse_derivative_x = axial_factor * kx_per_m_ * cos_x * sin_y;
        const Complex transverse_derivative_y = axial_factor * ky_per_m_ * sin_x * cos_y;
        const Complex mixed_derivative_x = axial_derivative * kx_per_m_ * cos_x * sin_y;
        const Complex mixed_derivative_y = axial_derivative * ky_per_m_ * sin_x * cos_y;

        field.electric_v_per_m.x = mixed_derivative_x /
                                   cutoff_wavenumber_squared_per_m2_;
        field.electric_v_per_m.y = mixed_derivative_y /
                                   cutoff_wavenumber_squared_per_m2_;
        field.electric_v_per_m.z = axial_factor * sin_x * sin_y;
        field.magnetic_a_per_m.x =
            imaginary_unit * angular_frequency_rad_per_s_ * permittivity_f_per_m_ /
            cutoff_wavenumber_squared_per_m2_ * transverse_derivative_y;
        field.magnetic_a_per_m.y =
            -imaginary_unit * angular_frequency_rad_per_s_ * permittivity_f_per_m_ /
            cutoff_wavenumber_squared_per_m2_ * transverse_derivative_x;
    }

    return field;
}
}
