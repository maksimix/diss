#pragma once

#include "waveguide_types.h"

#include <functional>

class WaveguideCalculator
{
public:
    WaveguideCalculationResult calculate(
        const WaveguideParameters &parameters,
        const std::function<bool()> &cancellation_requested = {},
        const std::function<void(const QString &)> &progress_reporter = {}) const;
};
