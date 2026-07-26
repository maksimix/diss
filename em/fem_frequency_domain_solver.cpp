#include "fem_frequency_domain_solver.h"
#ifdef EMWS_WITH_MFEM
#include "mfem_frequency_domain_backend.h"
#endif

#include <utility>

#include <cmath>

namespace em
{
FemFrequencyDomainSolver::FemFrequencyDomainSolver(
    std::shared_ptr<const IFemFrequencyDomainBackend> backend)
    : backend_(std::move(backend))
{
#ifdef EMWS_WITH_MFEM
    if (!backend_) {
        backend_ = createDefaultFemBackend();
    }
#endif
}

FieldSolution FemFrequencyDomainSolver::solve(const SimulationRequest &request,
                                              const SolveControl &control) const
{
    FieldSolution invalid_solution;
    invalid_solution.request = request;
    invalid_solution.diagnostics.backend_name = "Complex H(curl) FEM";
    const auto &fem = request.settings.fem;
    if (fem.mesh.element_order < 1 || fem.mesh.element_order > 6) {
        invalid_solution.error_message = "FEM element order must be in [1, 6].";
        return invalid_solution;
    }
    if (!(fem.relative_tolerance > 0.0) || !std::isfinite(fem.relative_tolerance) ||
        fem.maximum_iterations < 1 || fem.port_mode_count < 1) {
        invalid_solution.error_message = "Invalid FEM solver convergence settings.";
        return invalid_solution;
    }
    if (fem.pml.enabled &&
        (fem.pml.polynomial_order < 1 || !(fem.pml.target_reflection > 0.0) ||
         !(fem.pml.target_reflection < 1.0))) {
        invalid_solution.error_message = "Invalid PML settings.";
        return invalid_solution;
    }

    if (backend_) {
        return backend_->solve(request, control);
    }

    FieldSolution solution;
    solution.request = request;
    solution.diagnostics.backend_name = "Complex H(curl) FEM";
    if (control.isCancellationRequested()) {
        solution.cancelled = true;
        solution.error_message = "Calculation cancelled.";
        return solution;
    }

    solution.error_message =
        "This geometry requires a complex 3D H(curl) FEM backend, but no FEM backend is linked.";
    return solution;
}
}
