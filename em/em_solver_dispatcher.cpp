#include "em_solver_dispatcher.h"

#include "rectangular_waveguide_solver.h"
#include "transverse_pec_partition_solver.h"

#include <algorithm>
#include <utility>

namespace em
{
namespace
{
bool hasEnabledSlots(const EmModel &model)
{
    return std::any_of(model.slot_geometries.begin(),
                       model.slot_geometries.end(),
                       [](const SlotGeometry &slot) { return slot.enabled; });
}

bool hasEnabledPlates(const EmModel &model)
{
    return std::any_of(model.pec_plates.begin(),
                       model.pec_plates.end(),
                       [](const PecPlateGeometry &plate) { return plate.enabled; });
}

bool hasEnabledDielectrics(const EmModel &model)
{
    return std::any_of(model.dielectric_blocks.begin(),
                       model.dielectric_blocks.end(),
                       [](const DielectricBlockGeometry &block) { return block.enabled; });
}
}

EmSolverDispatcher::EmSolverDispatcher(
    std::shared_ptr<const IFemFrequencyDomainBackend> fem_backend)
    : fem_backend_(std::move(fem_backend))
{
}

FieldSolution EmSolverDispatcher::solve(const SimulationRequest &request,
                                        const SolveControl &control) const
{
    const bool enabled_slots = hasEnabledSlots(request.model);
    const bool enabled_plates = hasEnabledPlates(request.model);
    const bool enabled_dielectrics = hasEnabledDielectrics(request.model);
    if (!enabled_plates && !enabled_dielectrics &&
        (!enabled_slots ||
         request.settings.geometry_approximation_policy ==
             GeometryApproximationPolicy::UnperturbedBackgroundForSlots)) {
        return RectangularWaveguideSolver().solve(request, control);
    }

    if (enabled_plates && !enabled_dielectrics &&
        TransversePecPartitionSolver::canSolve(request)) {
        return TransversePecPartitionSolver().solve(request, control);
    }

    return FemFrequencyDomainSolver(fem_backend_).solve(request, control);
}
}
