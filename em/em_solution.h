#pragma once

#include "em_math.h"
#include "em_model.h"

#include <memory>
#include <string>
#include <vector>

namespace em
{
struct FieldPhasor
{
    ComplexVec3 electric_v_per_m;
    ComplexVec3 magnetic_a_per_m;
};

class IFieldEvaluator
{
public:
    virtual ~IFieldEvaluator() = default;

    virtual bool contains(const Vec3 &position_m) const = 0;
    virtual FieldPhasor evaluate(const Vec3 &position_m) const = 0;

    // Whether contains()/evaluate() may be called concurrently from several
    // threads. Closed-form evaluators are pure and safe; the MFEM-backed one is
    // not, because the library keeps shared mutable state inside its finite
    // element space and grid functions.
    virtual bool supportsConcurrentEvaluation() const { return true; }
};

struct ModeDescriptor
{
    ModeFamily family = ModeFamily::TransverseElectric;
    int m = 0;
    int n = 0;
    double cutoff_frequency_hz = 0.0;
    double cutoff_wavenumber_per_m = 0.0;
    Complex propagation_constant_per_m = 0.0;
    bool propagating = false;
};

struct ScatteringMatrix2Port
{
    Complex s11 = 0.0;
    Complex s12 = 0.0;
    Complex s21 = 0.0;
    Complex s22 = 0.0;
};

struct SolverDiagnostics
{
    std::string backend_name;
    double incident_power_w = 0.0;
    double reflected_power_w = 0.0;
    double transmitted_power_w = 0.0;
    double dissipated_power_w = 0.0;
    double input_power_w = 0.0;
    double output_power_w = 0.0;
    double power_balance_relative_error = 0.0;
    // |1 - |S11|^2 - |S21|^2|. A separate quantity from the power balance above:
    // the balance only says that the reported powers add up, while this says the
    // scattering matrix itself is not unitary. With lossy filling or an
    // absorbing PML the number is mostly physical absorption and says nothing
    // about accuracy at all.
    // On a lossless, non-radiating model it is an error measure only while the
    // discretisation error dominates it, and that stops being true sooner than
    // it looks. Measured on the empty 22.86 x 10.16 x 40 mm guide at 10 GHz with
    // the direct solver, so the linear system is solved to 1e-13 and nothing
    // else is in the way, over h = 5.0 / 4.0 / 3.2 / 2.6 mm:
    //   first-order elements  2.9e-02 / 2.2e-02 / 1.1e-02 / 1.3e-02
    //   second-order elements 1.2e-04 / 6.2e-05 / 1.6e-07 / 6.8e-05
    // With second-order elements it no longer falls with the mesh and its sign
    // is random (at h = 2.6 mm it came out as |S21| = 1.0000338 > 1), while the
    // S21 phase error over the same four meshes still improves ten-fold, from
    // 0.070 to 0.007 degrees. Below about 1e-4 the number is port projection
    // noise, not an error estimate, and a smaller value is not a better answer.
    double unitarity_defect = 0.0;
    int mesh_tetrahedron_count = 0;
    int fem_unknown_count = 0;
    int linear_iterations = 0;
    // True relative residual ||A x - b|| / ||b|| of the assembled system,
    // recomputed from the operator after the solve and never taken from what a
    // solver reports. It is NOT the quantity the iterative solver stops on:
    // GMRES is driven by the preconditioned residual, which is what
    // FemSolverSettings::relative_tolerance sets and what the progress messages
    // call "preconditioned relative residual".
    double linear_relative_residual = 0.0;
    double estimated_pml_reflection = 0.0;
    double maximum_pec_tangential_electric_v_per_m = 0.0;
    // Conductor loss and stored-energy diagnostics (analytic guide solver).
    double conductor_attenuation_np_per_m = 0.0;
    double stored_electric_energy_j = 0.0;
    double stored_magnetic_energy_j = 0.0;
    double quality_factor = 0.0;
    std::vector<std::string> warnings;
};

struct FieldSolution
{
    bool success = false;
    bool cancelled = false;
    std::string error_message;
    SimulationRequest request;
    std::vector<ModeDescriptor> available_modes;
    bool has_selected_mode = false;
    ModeDescriptor selected_mode;
    Complex forward_longitudinal_amplitude = 0.0;
    Complex backward_longitudinal_amplitude = 0.0;
    std::shared_ptr<const IFieldEvaluator> field;
    ScatteringMatrix2Port scattering;
    SolverDiagnostics diagnostics;
};
}
