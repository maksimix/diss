#pragma once

#include "waveguide_types.h"

#include <functional>

class WaveguideCalculator
{
public:
    // arrow_density — концентрация стрелок E/H/J в визуализации поля (1.0 —
    // обычная). Настройка отображения, а не физики: на решатель не влияет.
    WaveguideCalculationResult calculate(
        const WaveguideParameters &parameters,
        const std::function<bool()> &cancellation_requested = {},
        const std::function<void(const QString &)> &progress_reporter = {},
        double arrow_density = 1.0) const;
};
