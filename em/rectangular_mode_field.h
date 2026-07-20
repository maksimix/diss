#pragma once

#include "em_solution.h"

#include <optional>

namespace em
{
struct AxialFieldRegion
{
    double minimum_z_m = 0.0;
    double maximum_z_m = 0.0;
};

class RectangularModeFieldEvaluator final : public IFieldEvaluator
{
public:
    RectangularModeFieldEvaluator(
        const SimulationRequest &request,
        const ModeDescriptor &mode,
        Complex forward_longitudinal_amplitude,
        Complex backward_longitudinal_amplitude = 0.0,
        std::optional<AxialFieldRegion> active_region = std::nullopt);

    bool contains(const Vec3 &position_m) const override;
    FieldPhasor evaluate(const Vec3 &position_m) const override;

private:
    RectangularWaveguideGeometry geometry_;
    ModeDescriptor mode_;
    double angular_frequency_rad_per_s_ = 0.0;
    Complex forward_longitudinal_amplitude_ = 0.0;
    Complex backward_longitudinal_amplitude_ = 0.0;
    double kx_per_m_ = 0.0;
    double ky_per_m_ = 0.0;
    double cutoff_wavenumber_squared_per_m2_ = 0.0;
    Complex permeability_h_per_m_ = vacuum_permeability_h_per_m;
    Complex permittivity_f_per_m_ = vacuum_permittivity_f_per_m;
    std::optional<AxialFieldRegion> active_region_;
};
}
