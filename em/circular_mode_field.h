#pragma once

#include "em_solution.h"
#include "rectangular_mode_field.h"

#include <optional>

namespace em
{
// Поле TE/TM-моды круглого волновода в замкнутой форме. Соглашения те же, что у
// RectangularModeFieldEvaluator: амплитуда задаёт продольную компоненту (H_z для
// TE, E_z для TM), прямая волна идёт как exp(-gamma * (z + L/2)).
//
// Азимутальная зависимость продольной компоненты — cos(m*phi) по умолчанию. У
// мод с m >= 1 есть вырожденная пара с той же отсечкой: sin(m*phi), повёрнутая
// на 90/m градусов. Флаг azimuthal_sine выбирает её — обе поляризации нужны
// портам FEM, чтобы мощность, ушедшая в ортогональную поляризацию на
// несимметричном теле, не терялась из баланса.
class CircularModeFieldEvaluator final : public IFieldEvaluator
{
public:
    CircularModeFieldEvaluator(
        const SimulationRequest &request,
        const ModeDescriptor &mode,
        Complex forward_longitudinal_amplitude,
        Complex backward_longitudinal_amplitude = 0.0,
        std::optional<AxialFieldRegion> active_region = std::nullopt,
        bool azimuthal_sine = false);

    bool contains(const Vec3 &position_m) const override;
    FieldPhasor evaluate(const Vec3 &position_m) const override;

private:
    WaveguideGeometry geometry_;
    ModeDescriptor mode_;
    double angular_frequency_rad_per_s_ = 0.0;
    Complex forward_longitudinal_amplitude_ = 0.0;
    Complex backward_longitudinal_amplitude_ = 0.0;
    double cutoff_wavenumber_per_m_ = 0.0;
    double cutoff_wavenumber_squared_per_m2_ = 0.0;
    Complex permeability_h_per_m_ = vacuum_permeability_h_per_m;
    Complex permittivity_f_per_m_ = vacuum_permittivity_f_per_m;
    std::optional<AxialFieldRegion> active_region_;
    bool azimuthal_sine_ = false;
};
}
