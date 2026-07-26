#pragma once

#include "em_solver.h"

namespace em
{
// Набор мод сечения на частоте запроса, отсортированный по возрастанию частоты
// отсечки. Тот же перечислитель, которым пользуется решатель: вынесен наружу,
// чтобы интерфейс мог показать отсечки высших мод, не запуская расчёт.
std::vector<ModeDescriptor> enumerateWaveguideModes(const SimulationRequest &request,
                                                    const SolveControl &control = {});

class AnalyticWaveguideSolver final : public IEmSolver
{
public:
    FieldSolution solve(const SimulationRequest &request,
                        const SolveControl &control = {}) const override;
};
}
