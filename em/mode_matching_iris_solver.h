#pragma once

#include "em_solver.h"

#include <string>

namespace em
{
// Метод частичных областей (метод сшивания по модам) для поперечной
// PEC-пластины конечной толщины с прямоугольным окном (диафрагмы/ирисы).
//
// Structure: guide A (a x b) for z < z1, aperture guide B (w x h, an offset
// rectangular waveguide) for z1 < z < z2 = z1 + t, guide A again for z > z2.
// Fields are expanded over the complete TE/TM mode sets of each region and the
// tangential E and H continuity is enforced on both interface planes; the
// resulting block system yields the modal amplitudes, the S-parameters and a
// closed-form field everywhere.
class ModeMatchingIrisSolver final : public IEmSolver
{
public:
    // True when the request is exactly the geometry this method handles: one
    // enabled unrotated plate spanning the whole cross-section strictly between
    // the ports, with a rectangular through window and no stub, no slots, no
    // dielectric blocks.
    static bool canSolve(const SimulationRequest &request, std::string *reason = nullptr);

    FieldSolution solve(const SimulationRequest &request,
                        const SolveControl &control = {}) const override;
};
}
