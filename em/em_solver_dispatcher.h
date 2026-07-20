#pragma once

#include "em_solver.h"
#include "fem_frequency_domain_solver.h"

#include <memory>

namespace em
{
class EmSolverDispatcher final : public IEmSolver
{
public:
    explicit EmSolverDispatcher(
        std::shared_ptr<const IFemFrequencyDomainBackend> fem_backend = nullptr);

    FieldSolution solve(const SimulationRequest &request,
                        const SolveControl &control = {}) const override;

private:
    std::shared_ptr<const IFemFrequencyDomainBackend> fem_backend_;
};
}
