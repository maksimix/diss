#include "em_solver_dispatcher.h"

#include "mode_matching_iris_solver.h"
#include "rectangular_waveguide_solver.h"
#include "transverse_pec_partition_solver.h"

#include <algorithm>
#include <string>
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

FieldSolution rejected(const SimulationRequest &request, std::string message)
{
    FieldSolution solution;
    solution.request = request;
    solution.error_message = std::move(message);
    return solution;
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

    // An explicitly chosen method is honoured verbatim: a method that cannot
    // represent this geometry reports why instead of silently falling back, so
    // the printed result always matches the method named in the setup dialog.
    switch (request.settings.solver_method) {
    case SolverMethod::AnalyticRectangular:
        if (enabled_plates || enabled_dielectrics) {
            return rejected(request,
                            "Аналитический метод пустого волновода не учитывает пластины и "
                            "диэлектрики: отключите их или выберите другой метод.");
        }
        return RectangularWaveguideSolver().solve(request, control);
    case SolverMethod::TransversePartition: {
        std::string reason;
        if (!TransversePecPartitionSolver::canSolve(request, nullptr, &reason)) {
            return rejected(request,
                            "Метод поперечных сечений неприменим к этой геометрии: " + reason);
        }
        return TransversePecPartitionSolver().solve(request, control);
    }
    case SolverMethod::ModeMatching: {
        std::string reason;
        if (!ModeMatchingIrisSolver::canSolve(request, &reason)) {
            return rejected(request,
                            "Метод частичных областей неприменим к этой геометрии: " + reason);
        }
        return ModeMatchingIrisSolver().solve(request, control);
    }
    case SolverMethod::FiniteElement:
        return FemFrequencyDomainSolver(fem_backend_).solve(request, control);
    case SolverMethod::Automatic:
        break;
    }

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

    // Rectangular-window irises have an exact semi-analytic solution by mode
    // matching (метод частичных областей); everything else falls back to FEM.
    if (enabled_plates && !enabled_dielectrics && !enabled_slots &&
        ModeMatchingIrisSolver::canSolve(request)) {
        return ModeMatchingIrisSolver().solve(request, control);
    }

    return FemFrequencyDomainSolver(fem_backend_).solve(request, control);
}
}
