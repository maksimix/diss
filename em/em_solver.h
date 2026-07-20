#pragma once

#include "em_solution.h"

#include <functional>
#include <string>

namespace em
{
struct SolveControl
{
    std::function<bool()> cancellation_requested;
    std::function<void(const std::string &)> progress_reporter;

    bool isCancellationRequested() const
    {
        return cancellation_requested && cancellation_requested();
    }

    void reportProgress(const std::string &stage) const
    {
        if (progress_reporter) {
            progress_reporter(stage);
        }
    }
};

class IEmSolver
{
public:
    virtual ~IEmSolver() = default;
    virtual FieldSolution solve(const SimulationRequest &request,
                                const SolveControl &control = {}) const = 0;
};
}
