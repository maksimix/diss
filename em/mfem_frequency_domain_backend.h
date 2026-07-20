#pragma once

#include "fem_frequency_domain_solver.h"
#include "gmsh_tetrahedral_mesher.h"

#include <filesystem>

namespace em
{
class MfemFrequencyDomainBackend final : public IFemFrequencyDomainBackend
{
public:
    explicit MfemFrequencyDomainBackend(
        std::filesystem::path gmsh_executable = {},
        std::filesystem::path working_directory = {});

    FieldSolution solve(const SimulationRequest &request,
                        const SolveControl &control) const override;

private:
    GmshTetrahedralMesher mesher_;
    std::filesystem::path working_directory_;
};

std::shared_ptr<const IFemFrequencyDomainBackend> createDefaultFemBackend();
}
