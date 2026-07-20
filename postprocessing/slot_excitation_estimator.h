#pragma once

#include "em/em_solution.h"

#include <functional>

namespace postprocessing
{
class SlotExcitationEstimator
{
public:
    double normalizedCoupling(const em::FieldSolution &solution,
                              const em::SlotGeometry &slot,
                              const std::function<bool()> &cancellation_requested = {}) const;
};
}
