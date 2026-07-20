#pragma once

#include "em_solver.h"

#include <string>

namespace em
{
struct TransversePecPartition
{
    PecPlateGeometry geometry;
    double input_face_z_m = 0.0;
    double output_face_z_m = 0.0;
};

class TransversePecPartitionSolver final : public IEmSolver
{
public:
    static bool canSolve(const SimulationRequest &request,
                         TransversePecPartition *partition = nullptr,
                         std::string *reason = nullptr);

    FieldSolution solve(const SimulationRequest &request,
                        const SolveControl &control = {}) const override;
};
}
