#include "transverse_pec_partition_solver.h"

#include "rectangular_mode_field.h"
#include "analytic_waveguide_solver.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

namespace em
{
namespace
{
constexpr double geometry_tolerance_m = 1.0e-10;
constexpr double angle_tolerance_rad = 1.0e-10;
constexpr double power_tolerance_w = 1.0e-15;

bool finiteVector(const Vec3 &value)
{
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

bool hasEnabledSlots(const EmModel &model)
{
    return std::any_of(model.slot_geometries.begin(),
                       model.slot_geometries.end(),
                       [](const SlotGeometry &slot) { return slot.enabled; });
}

void setReason(std::string *reason, const std::string &value)
{
    if (reason != nullptr) {
        *reason = value;
    }
}
}

bool TransversePecPartitionSolver::canSolve(const SimulationRequest &request,
                                            TransversePecPartition *partition,
                                            std::string *reason)
{
    if (hasEnabledSlots(request.model)) {
        setReason(reason, "A slot and a PEC partition must be solved together by the FEM backend.");
        return false;
    }

    std::vector<const PecPlateGeometry *> enabled_plates;
    for (const PecPlateGeometry &plate : request.model.pec_plates) {
        if (plate.enabled) {
            enabled_plates.push_back(&plate);
        }
    }
    if (enabled_plates.size() != 1) {
        setReason(reason,
                  "The analytic partition backend requires exactly one enabled PEC plate.");
        return false;
    }

    const PecPlateGeometry &plate = *enabled_plates.front();
    if (!finiteVector(plate.center_m) || !finiteVector(plate.size_m) ||
        !finiteVector(plate.rotation_rad) ||
        plate.size_m.x <= 0.0 || plate.size_m.y <= 0.0 || plate.size_m.z <= 0.0) {
        setReason(reason, "PEC plate dimensions, position, and rotation must be finite and positive.");
        return false;
    }
    // A plate with a window is an iris: it transmits through the aperture, so it
    // must never be reduced to the closed-form short circuit. The same applies
    // to a post standing in that window.
    if (plateHasOpening(plate) || plateHasPost(plate)) {
        setReason(reason,
                  "A plate with an aperture is an iris and requires mode matching or the FEM backend.");
        return false;
    }
    if (std::abs(plate.rotation_rad.x) > angle_tolerance_rad ||
        std::abs(plate.rotation_rad.y) > angle_tolerance_rad ||
        std::abs(plate.rotation_rad.z) > angle_tolerance_rad) {
        setReason(reason,
                  "Only an axis-aligned plate normal to the propagation axis is analytic.");
        return false;
    }

    const WaveguideGeometry &guide = request.model.waveguide;
    const double plate_min_x_m = plate.center_m.x - 0.5 * plate.size_m.x;
    const double plate_max_x_m = plate.center_m.x + 0.5 * plate.size_m.x;
    const double plate_min_y_m = plate.center_m.y - 0.5 * plate.size_m.y;
    const double plate_max_y_m = plate.center_m.y + 0.5 * plate.size_m.y;
    const bool covers_cross_section =
        plate_min_x_m <= -0.5 * guide.inner_width_m + geometry_tolerance_m &&
        plate_max_x_m >= 0.5 * guide.inner_width_m - geometry_tolerance_m &&
        plate_min_y_m <= -0.5 * guide.inner_height_m + geometry_tolerance_m &&
        plate_max_y_m >= 0.5 * guide.inner_height_m - geometry_tolerance_m;
    if (!covers_cross_section) {
        setReason(reason,
                  "A partial diaphragm or iris requires mode matching or the FEM backend.");
        return false;
    }

    const double input_face_z_m = plate.center_m.z - 0.5 * plate.size_m.z;
    const double output_face_z_m = plate.center_m.z + 0.5 * plate.size_m.z;
    const double input_port_z_m = -0.5 * guide.length_m;
    const double output_port_z_m = 0.5 * guide.length_m;
    if (input_face_z_m <= input_port_z_m + geometry_tolerance_m ||
        output_face_z_m >= output_port_z_m - geometry_tolerance_m) {
        setReason(reason, "The PEC partition must lie strictly between both port planes.");
        return false;
    }

    if (partition != nullptr) {
        partition->geometry = plate;
        partition->input_face_z_m = input_face_z_m;
        partition->output_face_z_m = output_face_z_m;
    }
    if (reason != nullptr) {
        reason->clear();
    }
    return true;
}

FieldSolution TransversePecPartitionSolver::solve(const SimulationRequest &request,
                                                  const SolveControl &control) const
{
    TransversePecPartition partition;
    std::string unsupported_reason;
    if (!canSolve(request, &partition, &unsupported_reason)) {
        FieldSolution solution;
        solution.request = request;
        solution.diagnostics.backend_name = "Analytic full transverse PEC partition";
        solution.error_message = unsupported_reason;
        return solution;
    }
    if (control.isCancellationRequested()) {
        FieldSolution solution;
        solution.request = request;
        solution.cancelled = true;
        solution.error_message = "Calculation cancelled.";
        return solution;
    }

    SimulationRequest incident_request = request;
    incident_request.model.pec_plates.clear();
    incident_request.model.slot_geometries.clear();
    FieldSolution solution = AnalyticWaveguideSolver().solve(incident_request, control);
    solution.request = request;
    solution.diagnostics.backend_name = "Analytic full transverse PEC partition";
    if (!solution.success || solution.cancelled || !solution.has_selected_mode ||
        !solution.field) {
        return solution;
    }
    if (control.isCancellationRequested()) {
        solution.success = false;
        solution.cancelled = true;
        solution.has_selected_mode = false;
        solution.forward_longitudinal_amplitude = 0.0;
        solution.backward_longitudinal_amplitude = 0.0;
        solution.field.reset();
        solution.error_message = "Calculation cancelled.";
        return solution;
    }

    const WaveguideGeometry &guide = request.model.waveguide;
    const double input_port_z_m = -0.5 * guide.length_m;
    const double output_port_z_m = 0.5 * guide.length_m;
    const double distance_to_input_face_m = partition.input_face_z_m - input_port_z_m;
    const double distance_from_output_face_m = output_port_z_m - partition.output_face_z_m;
    const Complex gamma = solution.selected_mode.propagation_constant_per_m;
    const Complex round_trip_to_input_face =
        std::exp(-2.0 * gamma * distance_to_input_face_m);
    const Complex round_trip_to_output_face =
        std::exp(-2.0 * gamma * distance_from_output_face_m);

    solution.scattering.s11 = -round_trip_to_input_face;
    solution.scattering.s12 = 0.0;
    solution.scattering.s21 = 0.0;
    solution.scattering.s22 = -round_trip_to_output_face;

    const double longitudinal_reflection_sign =
        solution.selected_mode.family == ModeFamily::TransverseElectric ? -1.0 : 1.0;
    solution.backward_longitudinal_amplitude =
        longitudinal_reflection_sign * solution.forward_longitudinal_amplitude *
        round_trip_to_input_face;
    solution.field = std::make_shared<RectangularModeFieldEvaluator>(
        request,
        solution.selected_mode,
        solution.forward_longitudinal_amplitude,
        solution.backward_longitudinal_amplitude,
        AxialFieldRegion{input_port_z_m, partition.input_face_z_m});

    const double incident_power_w = solution.diagnostics.input_power_w;
    const double reflected_power_w = incident_power_w * std::norm(solution.scattering.s11);
    const double dissipated_power_w = std::max(0.0, incident_power_w - reflected_power_w);
    solution.diagnostics.incident_power_w = incident_power_w;
    solution.diagnostics.reflected_power_w = reflected_power_w;
    solution.diagnostics.transmitted_power_w = 0.0;
    solution.diagnostics.dissipated_power_w = dissipated_power_w;
    solution.diagnostics.output_power_w = 0.0;
    solution.diagnostics.power_balance_relative_error =
        std::abs(incident_power_w - reflected_power_w - dissipated_power_w) /
        std::max(power_tolerance_w, std::abs(incident_power_w));
    solution.diagnostics.warnings.push_back(
        "The reconstructed field is the response to excitation from port 1; the isolated port-2 region is zero.");
    return solution;
}
}
