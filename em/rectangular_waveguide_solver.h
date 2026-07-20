#pragma once

#include "em_solver.h"

namespace em
{
class RectangularWaveguideSolver final : public IEmSolver
{
public:
    FieldSolution solve(const SimulationRequest &request,
                        const SolveControl &control = {}) const override;

private:
    std::vector<ModeDescriptor> enumerateModes(const SimulationRequest &request,
                                               const SolveControl &control) const;
};
}
