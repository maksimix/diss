#pragma once

#include "em_solver.h"

#include <memory>

namespace em
{
class IFemFrequencyDomainBackend
{
public:
    virtual ~IFemFrequencyDomainBackend() = default;
    virtual FieldSolution solve(const SimulationRequest &request,
                                const SolveControl &control) const = 0;
};

class FemFrequencyDomainSolver final : public IEmSolver
{
public:
    explicit FemFrequencyDomainSolver(
        std::shared_ptr<const IFemFrequencyDomainBackend> backend = nullptr);

    FieldSolution solve(const SimulationRequest &request,
                        const SolveControl &control = {}) const override;

private:
    std::shared_ptr<const IFemFrequencyDomainBackend> backend_;
};
}
