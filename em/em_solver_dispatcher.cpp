#include "em_solver_dispatcher.h"

#include "mode_matching_iris_solver.h"
#include "analytic_waveguide_solver.h"
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

    // Круглое сечение пока поддержано только замкнутыми формулами пустого
    // волновода: сеточный генератор строит прямоугольную трубу, а метод
    // частичных областей и метод поперечных сечений выведены для прямоугольных
    // мод. Молчаливый переход на FEM дал бы неверную геометрию, поэтому
    // несовместимые случаи отклоняются с объяснением.
    // Свободные тела пользователя описывает только сеточный решатель: замкнутые
    // формулы и методы частичных областей выведены для пластин известного вида.
    const bool enabled_shapes = hasEnabledShapes(request.model.shapes);

    if (isCircular(request.model.waveguide)) {
        if (enabled_plates || enabled_dielectrics || enabled_slots) {
            return rejected(request,
                            "Для круглого волновода поддержаны пустой тракт и свободные "
                            "тела: пластины-диафрагмы, щели и диэлектрики пока доступны "
                            "только в прямоугольном сечении.");
        }
        // Тела в круглом тракте считает FEM: сеточный генератор строит для него
        // цилиндрическую полость, а моды на портах берутся из решения через
        // функции Бесселя.
        if (enabled_shapes) {
            if (request.settings.solver_method != SolverMethod::Automatic &&
                request.settings.solver_method != SolverMethod::FiniteElement) {
                return rejected(request,
                                "Круглый волновод с телами считается только методом "
                                "конечных элементов: выберите «Автоматически» или «Метод "
                                "конечных элементов».");
            }
            return FemFrequencyDomainSolver(fem_backend_).solve(request, control);
        }
        // Пустой круглый тракт имеет точное решение, и Automatic всегда берёт
        // его. Явный выбор FEM оставлен намеренно: на геометрии с известным
        // ответом он измеряет ошибку дискретизации самого FEM — единственная
        // прямая проверка криволинейной сетки и портов круглого сечения.
        if (request.settings.solver_method == SolverMethod::FiniteElement) {
            return FemFrequencyDomainSolver(fem_backend_).solve(request, control);
        }
        if (request.settings.solver_method != SolverMethod::Automatic &&
            request.settings.solver_method != SolverMethod::AnalyticRectangular) {
            return rejected(request,
                            "Пустой круглый волновод считается точными формулами Бесселя; "
                            "методы поперечных сечений и частичных областей к круглому "
                            "сечению неприменимы.");
        }
        return AnalyticWaveguideSolver().solve(request, control);
    }

    // An explicitly chosen method is honoured verbatim: a method that cannot
    // represent this geometry reports why instead of silently falling back, so
    // the printed result always matches the method named in the setup dialog.
    switch (request.settings.solver_method) {
    case SolverMethod::AnalyticRectangular:
        if (enabled_plates || enabled_dielectrics || enabled_shapes) {
            return rejected(request,
                            "Аналитический метод пустого волновода не учитывает пластины, "
                            "тела и диэлектрики: отключите их или выберите другой метод.");
        }
        return AnalyticWaveguideSolver().solve(request, control);
    case SolverMethod::TransversePartition: {
        std::string reason;
        if (enabled_shapes) {
            return rejected(request,
                            "Метод поперечных сечений не описывает свободные тела: они "
                            "считаются методом конечных элементов.");
        }
        if (!TransversePecPartitionSolver::canSolve(request, nullptr, &reason)) {
            return rejected(request,
                            "Метод поперечных сечений неприменим к этой геометрии: " + reason);
        }
        return TransversePecPartitionSolver().solve(request, control);
    }
    case SolverMethod::ModeMatching: {
        std::string reason;
        if (enabled_shapes) {
            return rejected(request,
                            "Метод частичных областей не описывает свободные тела: они "
                            "считаются методом конечных элементов.");
        }
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

    if (!enabled_shapes && !enabled_plates && !enabled_dielectrics &&
        (!enabled_slots ||
         request.settings.geometry_approximation_policy ==
             GeometryApproximationPolicy::UnperturbedBackgroundForSlots)) {
        return AnalyticWaveguideSolver().solve(request, control);
    }

    if (!enabled_shapes && enabled_plates && !enabled_dielectrics &&
        TransversePecPartitionSolver::canSolve(request)) {
        return TransversePecPartitionSolver().solve(request, control);
    }

    // Rectangular-window irises have an exact semi-analytic solution by mode
    // matching (метод частичных областей); everything else falls back to FEM.
    if (!enabled_shapes && enabled_plates && !enabled_dielectrics && !enabled_slots &&
        ModeMatchingIrisSolver::canSolve(request)) {
        return ModeMatchingIrisSolver().solve(request, control);
    }

    return FemFrequencyDomainSolver(fem_backend_).solve(request, control);
}
}
