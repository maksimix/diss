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
    int mesh_tetrahedron_count = 0;
    int fem_unknown_count = 0;
    int linear_iterations = 0;
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
