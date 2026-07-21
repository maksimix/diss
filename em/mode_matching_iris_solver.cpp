#include "mode_matching_iris_solver.h"

#include "rectangular_waveguide_solver.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <memory>
#include <sstream>
#include <vector>

namespace em
{
namespace
{
constexpr double geometry_tolerance_m = 1.0e-10;
constexpr double angle_tolerance_rad = 1.0e-10;
constexpr double numerical_tolerance = 1.0e-12;

// Same passive branch of the square root as the rectangular-waveguide solver:
// Re(gamma) >= 0, and for a lossless propagating mode Im(gamma) >= 0.
Complex passiveSquareRoot(const Complex &value)
{
    Complex root = std::sqrt(value);
    if (std::real(root) < 0.0 ||
        (std::abs(std::real(root)) < numerical_tolerance && std::imag(root) < 0.0)) {
        root = -root;
    }
    return root;
}

// ------------------------------------------------------------------ modes ---
//
// Orthonormal transverse-E patterns of a rectangular waveguide with origin
// (x0, y0) and size a x b. Both families share the same component shapes:
//   e_x = cex * cos(kx x') * sin(ky y'),   e_y = cey * sin(kx x') * cos(ky y')
// with x' = x - x0, y' = y - y0 and
//   TE (H-modes): cex = +ky * invN,  cey = -kx * invN   (e = z x grad(psi) * invN)
//   TM (E-modes): cex = +kx * invN,  cey = +ky * invN   (e = grad(phi) * invN)
// invN normalizes the pattern to unit L2 norm over the cross-section, which
// makes the mode sets orthonormal (TE-TM cross products integrate to zero).
struct GuideMode
{
    ModeFamily family = ModeFamily::TransverseElectric;
    int m = 0;
    int n = 0;
    double kx = 0.0;
    double ky = 0.0;
    double kc2 = 0.0;
    double inv_norm = 0.0;
    double cex = 0.0;
    double cey = 0.0;
    Complex gamma;
    Complex admittance;
};

struct GuideFrame
{
    double x0 = 0.0;
    double y0 = 0.0;
    double width = 0.0;
    double height = 0.0;
};

std::vector<GuideMode> buildModes(const GuideFrame &frame,
                                  int maximum_m,
                                  int maximum_n,
                                  double omega,
                                  const Complex &mu,
                                  const Complex &epsilon)
{
    const Complex k2 = omega * omega * mu * epsilon;
    const Complex j(0.0, 1.0);
    std::vector<GuideMode> modes;
    modes.reserve(static_cast<std::size_t>(2 * (maximum_m + 1) * (maximum_n + 1)));

    const auto half_integral = [](double length, int index) {
        return index == 0 ? length : 0.5 * length;   // int cos^2; int sin^2 = L/2 (0 if idx 0)
    };

    for (int m = 0; m <= maximum_m; ++m) {
        for (int n = 0; n <= maximum_n; ++n) {
            if (m == 0 && n == 0) {
                continue;
            }
            const double kx = m * pi / frame.width;
            const double ky = n * pi / frame.height;
            const double kc2 = kx * kx + ky * ky;
            const Complex gamma = passiveSquareRoot(Complex(kc2, 0.0) - k2);

            // ||z x grad(psi)||^2 = ky^2 * Icx*Isy + kx^2 * Isx*Icy
            const double integral_cs = half_integral(frame.width, m) *
                                       (n == 0 ? 0.0 : 0.5 * frame.height);
            const double integral_sc = (m == 0 ? 0.0 : 0.5 * frame.width) *
                                       half_integral(frame.height, n);

            {   // TE (H-mode)
                const double norm2 = ky * ky * integral_cs + kx * kx * integral_sc;
                if (norm2 > 0.0) {
                    GuideMode mode;
                    mode.family = ModeFamily::TransverseElectric;
                    mode.m = m;
                    mode.n = n;
                    mode.kx = kx;
                    mode.ky = ky;
                    mode.kc2 = kc2;
                    mode.inv_norm = 1.0 / std::sqrt(norm2);
                    mode.cex = ky * mode.inv_norm;
                    mode.cey = -kx * mode.inv_norm;
                    mode.gamma = gamma;
                    mode.admittance = gamma / (j * omega * mu);
                    modes.push_back(mode);
                }
            }
            if (m >= 1 && n >= 1) {   // TM (E-mode)
                const double norm2 = kx * kx * integral_cs + ky * ky * integral_sc;
                GuideMode mode;
                mode.family = ModeFamily::TransverseMagnetic;
                mode.m = m;
                mode.n = n;
                mode.kx = kx;
                mode.ky = ky;
                mode.kc2 = kc2;
                mode.inv_norm = 1.0 / std::sqrt(norm2);
                mode.cex = kx * mode.inv_norm;
                mode.cey = ky * mode.inv_norm;
                mode.gamma = gamma;
                mode.admittance = j * omega * epsilon / gamma;
                modes.push_back(mode);
            }
        }
    }
    return modes;
}

// ------------------------------------------------- coupling integrals -------
//
// integral over [t1, t2] of trig(kA (x - uA)) * trig(kB (x - uB)) dx in closed
// form, written through cos(k(x-u) + p) with p = 0 for cos and p = -pi/2 for
// sin. The product-to-sum identity gives two terms
//   1/2 [ cos((kA-kB)x + phi-) + cos((kA+kB)x + phi+) ]
// and each primitive is evaluated in the numerically stable midpoint form
//   integral cos(Kx + phi) dx = D * cos(K xbar + phi) * sinc(K D / 2),
// exact for every K including K -> 0.
double stableCosIntegral(double frequency, double phase, double t1, double t2)
{
    const double span = t2 - t1;
    const double midpoint = 0.5 * (t1 + t2);
    const double argument = 0.5 * frequency * span;
    const double sinc = std::abs(argument) < 1.0e-8
                            ? 1.0 - argument * argument / 6.0
                            : std::sin(argument) / argument;
    return span * std::cos(frequency * midpoint + phase) * sinc;
}

double trigProductIntegral(double kA, double uA, bool sinA,
                           double kB, double uB, bool sinB,
                           double t1, double t2)
{
    const double pA = sinA ? -0.5 * pi : 0.0;
    const double pB = sinB ? -0.5 * pi : 0.0;
    const double phase_minus = -kA * uA + kB * uB + pA - pB;
    const double phase_plus = -kA * uA - kB * uB + pA + pB;
    return 0.5 * (stableCosIntegral(kA - kB, phase_minus, t1, t2) +
                  stableCosIntegral(kA + kB, phase_plus, t1, t2));
}

// X[m][k] = integral over the aperture of e_m^A . e_k^B dA
double couplingIntegral(const GuideMode &mode_a, const GuideFrame &frame_a,
                        const GuideMode &mode_b, const GuideFrame &frame_b)
{
    const double x1 = frame_b.x0;
    const double x2 = frame_b.x0 + frame_b.width;
    const double y1 = frame_b.y0;
    const double y2 = frame_b.y0 + frame_b.height;

    // e_x components: cos_x * sin_y ; e_y components: sin_x * cos_y.
    const double term_x =
        mode_a.cex * mode_b.cex *
        trigProductIntegral(mode_a.kx, frame_a.x0, false, mode_b.kx, frame_b.x0, false, x1, x2) *
        trigProductIntegral(mode_a.ky, frame_a.y0, true, mode_b.ky, frame_b.y0, true, y1, y2);
    const double term_y =
        mode_a.cey * mode_b.cey *
        trigProductIntegral(mode_a.kx, frame_a.x0, true, mode_b.kx, frame_b.x0, true, x1, x2) *
        trigProductIntegral(mode_a.ky, frame_a.y0, false, mode_b.ky, frame_b.y0, false, y1, y2);
    return term_x + term_y;
}

// --------------------------------------------------------- linear solve -----
// Dense complex LU with partial pivoting; the matching system is small
// (2 * N_B unknowns), so a self-contained solver keeps the EM core free of
// external linear-algebra dependencies.
bool solveDenseComplex(std::vector<Complex> &matrix,
                       std::vector<Complex> &rhs,
                       int size)
{
    for (int column = 0; column < size; ++column) {
        int pivot = column;
        double best = std::abs(matrix[static_cast<std::size_t>(column) * size + column]);
        for (int row = column + 1; row < size; ++row) {
            const double candidate =
                std::abs(matrix[static_cast<std::size_t>(row) * size + column]);
            if (candidate > best) {
                best = candidate;
                pivot = row;
            }
        }
        if (!(best > 0.0) || !std::isfinite(best)) {
            return false;
        }
        if (pivot != column) {
            for (int k = 0; k < size; ++k) {
                std::swap(matrix[static_cast<std::size_t>(pivot) * size + k],
                          matrix[static_cast<std::size_t>(column) * size + k]);
            }
            std::swap(rhs[static_cast<std::size_t>(pivot)],
                      rhs[static_cast<std::size_t>(column)]);
        }
        const Complex diagonal = matrix[static_cast<std::size_t>(column) * size + column];
        for (int row = column + 1; row < size; ++row) {
            const Complex factor =
                matrix[static_cast<std::size_t>(row) * size + column] / diagonal;
            if (factor == Complex(0.0, 0.0)) {
                continue;
            }
            for (int k = column; k < size; ++k) {
                matrix[static_cast<std::size_t>(row) * size + k] -=
                    factor * matrix[static_cast<std::size_t>(column) * size + k];
            }
            rhs[static_cast<std::size_t>(row)] -= factor * rhs[static_cast<std::size_t>(column)];
        }
    }
    for (int row = size - 1; row >= 0; --row) {
        Complex sum = rhs[static_cast<std::size_t>(row)];
        for (int k = row + 1; k < size; ++k) {
            sum -= matrix[static_cast<std::size_t>(row) * size + k] *
                   rhs[static_cast<std::size_t>(k)];
        }
        rhs[static_cast<std::size_t>(row)] =
            sum / matrix[static_cast<std::size_t>(row) * size + row];
    }
    return true;
}

// ------------------------------------------------------- field evaluator ----
struct EvaluatorMode
{
    GuideMode mode;
    GuideFrame frame;
    Complex forward;          // transverse-E amplitude at z_forward_ref
    Complex backward;         // transverse-E amplitude at z_backward_ref
    double z_forward_ref = 0.0;
    double z_backward_ref = 0.0;
    Complex mu;               // for TE H_z reconstruction
};

FieldPhasor evaluateMode(const EvaluatorMode &entry, const Vec3 &position, double omega)
{
    FieldPhasor field;
    const GuideMode &mode = entry.mode;
    const double x_local = position.x - entry.frame.x0;
    const double y_local = position.y - entry.frame.y0;
    const double cos_x = std::cos(mode.kx * x_local);
    const double sin_x = std::sin(mode.kx * x_local);
    const double cos_y = std::cos(mode.ky * y_local);
    const double sin_y = std::sin(mode.ky * y_local);

    const Complex j(0.0, 1.0);
    const double zeta_forward = position.z - entry.z_forward_ref;
    const double zeta_backward = entry.z_backward_ref - position.z;
    const double decay_forward = std::real(mode.gamma) * zeta_forward;
    const double decay_backward = std::real(mode.gamma) * zeta_backward;
    const Complex amp_forward = (entry.forward != Complex(0.0, 0.0) && decay_forward < 60.0)
                                    ? entry.forward * std::exp(-mode.gamma * zeta_forward)
                                    : Complex(0.0, 0.0);
    const Complex amp_backward = (entry.backward != Complex(0.0, 0.0) && decay_backward < 60.0)
                                     ? entry.backward * std::exp(-mode.gamma * zeta_backward)
                                     : Complex(0.0, 0.0);
    if (amp_forward == Complex(0.0, 0.0) && amp_backward == Complex(0.0, 0.0)) {
        return field;
    }
    const Complex electric_sum = amp_forward + amp_backward;
    const Complex magnetic_sum = amp_forward - amp_backward;

    const double pattern_x = mode.cex * cos_x * sin_y;
    const double pattern_y = mode.cey * sin_x * cos_y;

    field.electric_v_per_m.x = electric_sum * pattern_x;
    field.electric_v_per_m.y = electric_sum * pattern_y;
    // H_t = Y * (fwd - bwd) * (z x e), with z x e = (-e_y, e_x).
    field.magnetic_a_per_m.x = mode.admittance * magnetic_sum * (-pattern_y);
    field.magnetic_a_per_m.y = mode.admittance * magnetic_sum * pattern_x;

    if (mode.family == ModeFamily::TransverseElectric) {
        field.magnetic_a_per_m.z = (mode.kc2 * mode.inv_norm / (j * omega * entry.mu)) *
                                   electric_sum * cos_x * cos_y;
    } else {
        field.electric_v_per_m.z = (mode.kc2 * mode.inv_norm / mode.gamma) *
                                   (-amp_forward + amp_backward) * sin_x * sin_y;
    }
    return field;
}

class ModeMatchingFieldEvaluator final : public IFieldEvaluator
{
public:
    ModeMatchingFieldEvaluator(RectangularWaveguideGeometry geometry,
                               GuideFrame aperture,
                               double z1,
                               double z2,
                               double omega,
                               std::vector<EvaluatorMode> region_a,
                               std::vector<EvaluatorMode> region_b,
                               std::vector<EvaluatorMode> region_c)
        : geometry_(geometry),
          aperture_(aperture),
          z1_(z1),
          z2_(z2),
          omega_(omega),
          region_a_(std::move(region_a)),
          region_b_(std::move(region_b)),
          region_c_(std::move(region_c))
    {
    }

    bool contains(const Vec3 &position_m) const override
    {
        return position_m.x >= -0.5 * geometry_.inner_width_m - geometry_tolerance_m &&
               position_m.x <= 0.5 * geometry_.inner_width_m + geometry_tolerance_m &&
               position_m.y >= -0.5 * geometry_.inner_height_m - geometry_tolerance_m &&
               position_m.y <= 0.5 * geometry_.inner_height_m + geometry_tolerance_m &&
               position_m.z >= -0.5 * geometry_.length_m - geometry_tolerance_m &&
               position_m.z <= 0.5 * geometry_.length_m + geometry_tolerance_m;
    }

    FieldPhasor evaluate(const Vec3 &position_m) const override
    {
        if (!contains(position_m)) {
            return {};
        }
        const std::vector<EvaluatorMode> *modes = nullptr;
        if (position_m.z < z1_) {
            modes = &region_a_;
        } else if (position_m.z > z2_) {
            modes = &region_c_;
        } else {
            // Inside the plate: field exists only in the aperture waveguide.
            const bool inside_aperture =
                position_m.x >= aperture_.x0 - geometry_tolerance_m &&
                position_m.x <= aperture_.x0 + aperture_.width + geometry_tolerance_m &&
                position_m.y >= aperture_.y0 - geometry_tolerance_m &&
                position_m.y <= aperture_.y0 + aperture_.height + geometry_tolerance_m;
            if (!inside_aperture) {
                return {};
            }
            modes = &region_b_;
        }
        FieldPhasor total;
        for (const EvaluatorMode &entry : *modes) {
            const FieldPhasor contribution = evaluateMode(entry, position_m, omega_);
            total.electric_v_per_m = total.electric_v_per_m + contribution.electric_v_per_m;
            total.magnetic_a_per_m = total.magnetic_a_per_m + contribution.magnetic_a_per_m;
        }
        return total;
    }

private:
    RectangularWaveguideGeometry geometry_;
    GuideFrame aperture_;
    double z1_ = 0.0;
    double z2_ = 0.0;
    double omega_ = 0.0;
    std::vector<EvaluatorMode> region_a_;
    std::vector<EvaluatorMode> region_b_;
    std::vector<EvaluatorMode> region_c_;
};

// Keep only the strongest contributions so field sampling stays fast; the
// dropped far-evanescent modes matter only in a vanishing neighbourhood of the
// interfaces.
void pruneModes(std::vector<EvaluatorMode> &modes, std::size_t maximum_count)
{
    double maximum = 0.0;
    for (const EvaluatorMode &entry : modes) {
        maximum = std::max(maximum, std::max(std::abs(entry.forward), std::abs(entry.backward)));
    }
    const double floor = 1.0e-9 * maximum;
    modes.erase(std::remove_if(modes.begin(), modes.end(),
                               [floor](const EvaluatorMode &entry) {
                                   return std::abs(entry.forward) < floor &&
                                          std::abs(entry.backward) < floor;
                               }),
                modes.end());
    if (modes.size() > maximum_count) {
        std::partial_sort(modes.begin(), modes.begin() + maximum_count, modes.end(),
                          [](const EvaluatorMode &left, const EvaluatorMode &right) {
                              const double l = std::max(std::abs(left.forward),
                                                        std::abs(left.backward));
                              const double r = std::max(std::abs(right.forward),
                                                        std::abs(right.backward));
                              return l > r;
                          });
        modes.resize(maximum_count);
    }
}

void setReason(std::string *reason, const std::string &value)
{
    if (reason != nullptr) {
        *reason = value;
    }
}
}

bool ModeMatchingIrisSolver::canSolve(const SimulationRequest &request, std::string *reason)
{
    for (const SlotGeometry &slot : request.model.slot_geometries) {
        if (slot.enabled) {
            setReason(reason, "Slots require the FEM backend.");
            return false;
        }
    }
    for (const DielectricBlockGeometry &block : request.model.dielectric_blocks) {
        if (block.enabled) {
            setReason(reason, "Dielectric blocks require the FEM backend.");
            return false;
        }
    }

    const PecPlateGeometry *plate = nullptr;
    for (const PecPlateGeometry &candidate : request.model.pec_plates) {
        if (!candidate.enabled) {
            continue;
        }
        if (plate != nullptr) {
            setReason(reason, "Mode matching handles exactly one plate.");
            return false;
        }
        plate = &candidate;
    }
    if (plate == nullptr) {
        setReason(reason, "No enabled plate.");
        return false;
    }
    if (std::abs(plate->rotation_rad.x) > angle_tolerance_rad ||
        std::abs(plate->rotation_rad.y) > angle_tolerance_rad ||
        std::abs(plate->rotation_rad.z) > angle_tolerance_rad) {
        setReason(reason, "Only an unrotated transverse plate is supported.");
        return false;
    }
    if (!plateHasOpening(*plate) ||
        plate->aperture_shape != PlateApertureShape::Rectangular) {
        setReason(reason,
                  "Mode matching needs a rectangular aperture (circular apertures require "
                  "circular-guide modes and go to the FEM backend).");
        return false;
    }
    if (plateHasPost(*plate)) {
        setReason(reason, "A stub in the window breaks the rectangular aperture; use FEM.");
        return false;
    }

    const RectangularWaveguideGeometry &guide = request.model.waveguide;
    const double plate_min_x = plate->center_m.x - 0.5 * plate->size_m.x;
    const double plate_max_x = plate->center_m.x + 0.5 * plate->size_m.x;
    const double plate_min_y = plate->center_m.y - 0.5 * plate->size_m.y;
    const double plate_max_y = plate->center_m.y + 0.5 * plate->size_m.y;
    const bool covers_cross_section =
        plate_min_x <= -0.5 * guide.inner_width_m + geometry_tolerance_m &&
        plate_max_x >= 0.5 * guide.inner_width_m - geometry_tolerance_m &&
        plate_min_y <= -0.5 * guide.inner_height_m + geometry_tolerance_m &&
        plate_max_y >= 0.5 * guide.inner_height_m - geometry_tolerance_m;
    if (!covers_cross_section) {
        setReason(reason, "The plate must span the whole cross-section (an iris).");
        return false;
    }

    const double aperture_min_x =
        plate->center_m.x + plate->aperture_offset_x_m - 0.5 * plate->aperture_width_m;
    const double aperture_max_x = aperture_min_x + plate->aperture_width_m;
    const double aperture_min_y =
        plate->center_m.y + plate->aperture_offset_y_m - 0.5 * plate->aperture_height_m;
    const double aperture_max_y = aperture_min_y + plate->aperture_height_m;
    if (aperture_min_x < -0.5 * guide.inner_width_m - geometry_tolerance_m ||
        aperture_max_x > 0.5 * guide.inner_width_m + geometry_tolerance_m ||
        aperture_min_y < -0.5 * guide.inner_height_m - geometry_tolerance_m ||
        aperture_max_y > 0.5 * guide.inner_height_m + geometry_tolerance_m) {
        setReason(reason, "The aperture must lie inside the waveguide cross-section.");
        return false;
    }

    const double z1 = plate->center_m.z - 0.5 * plate->size_m.z;
    const double z2 = plate->center_m.z + 0.5 * plate->size_m.z;
    if (z1 <= -0.5 * guide.length_m + geometry_tolerance_m ||
        z2 >= 0.5 * guide.length_m - geometry_tolerance_m) {
        setReason(reason, "The iris must lie strictly between both port planes.");
        return false;
    }
    if (reason != nullptr) {
        reason->clear();
    }
    return true;
}

FieldSolution ModeMatchingIrisSolver::solve(const SimulationRequest &request,
                                            const SolveControl &control) const
{
    FieldSolution solution;
    solution.request = request;
    solution.diagnostics.backend_name = "Mode matching (метод частичных областей)";

    std::string reason;
    if (!canSolve(request, &reason)) {
        solution.error_message = reason;
        return solution;
    }
    if (control.isCancellationRequested()) {
        solution.cancelled = true;
        solution.error_message = "Calculation cancelled.";
        return solution;
    }

    // Incident empty-guide solve: gives the mode table, the selected mode and
    // the 1 W power normalization, exactly like the analytic partition solver.
    SimulationRequest incident_request = request;
    incident_request.model.pec_plates.clear();
    incident_request.model.slot_geometries.clear();
    FieldSolution incident = RectangularWaveguideSolver().solve(incident_request, control);
    if (!incident.success || incident.cancelled || !incident.has_selected_mode ||
        !incident.field) {
        incident.request = request;
        incident.diagnostics.backend_name = solution.diagnostics.backend_name;
        return incident;
    }
    solution.available_modes = incident.available_modes;
    solution.has_selected_mode = true;
    solution.selected_mode = incident.selected_mode;
    solution.success = true;
    solution.diagnostics.input_power_w = incident.diagnostics.input_power_w;

    control.reportProgress("Mode matching: building the mode sets...");

    const RectangularWaveguideGeometry &guide = request.model.waveguide;
    const PecPlateGeometry *plate = nullptr;
    for (const PecPlateGeometry &candidate : request.model.pec_plates) {
        if (candidate.enabled) {
            plate = &candidate;
        }
    }

    const double omega = 2.0 * pi * request.frequency_hz;
    const Material &material = request.model.filling_material;
    const Complex mu = vacuum_permeability_h_per_m * material.relative_permeability;
    const Complex epsilon =
        vacuum_permittivity_f_per_m * material.relative_permittivity -
        Complex(0.0, material.conductivity_s_per_m / omega);

    const GuideFrame frame_a{-0.5 * guide.inner_width_m,
                             -0.5 * guide.inner_height_m,
                             guide.inner_width_m,
                             guide.inner_height_m};
    const GuideFrame frame_b{plate->center_m.x + plate->aperture_offset_x_m -
                                 0.5 * plate->aperture_width_m,
                             plate->center_m.y + plate->aperture_offset_y_m -
                                 0.5 * plate->aperture_height_m,
                             plate->aperture_width_m,
                             plate->aperture_height_m};
    const double z1 = plate->center_m.z - 0.5 * plate->size_m.z;
    const double z2 = plate->center_m.z + 0.5 * plate->size_m.z;
    const double thickness = z2 - z1;

    // Relative convergence: the spectral content per direction must match the
    // region size ratio, N_A / N_B ~ a / w, or the aperture-edge behaviour is
    // misrepresented and the S-parameters converge to a wrong limit.
    const int aperture_m = 8;
    const int aperture_n = 8;
    const int guide_m = std::clamp(
        static_cast<int>(std::lround(aperture_m * frame_a.width / frame_b.width)),
        std::max(aperture_m, solution.selected_mode.m + 2),
        40);
    const int guide_n = std::clamp(
        static_cast<int>(std::lround(aperture_n * frame_a.height / frame_b.height)),
        std::max(aperture_n, solution.selected_mode.n + 2),
        40);

    const std::vector<GuideMode> modes_a =
        buildModes(frame_a, guide_m, guide_n, omega, mu, epsilon);
    const std::vector<GuideMode> modes_b =
        buildModes(frame_b, aperture_m, aperture_n, omega, mu, epsilon);
    const int count_a = static_cast<int>(modes_a.size());
    const int count_b = static_cast<int>(modes_b.size());

    int incident_index = -1;
    for (int index = 0; index < count_a; ++index) {
        if (modes_a[static_cast<std::size_t>(index)].family ==
                solution.selected_mode.family &&
            modes_a[static_cast<std::size_t>(index)].m == solution.selected_mode.m &&
            modes_a[static_cast<std::size_t>(index)].n == solution.selected_mode.n) {
            incident_index = index;
            break;
        }
    }
    if (incident_index < 0) {
        solution.success = false;
        solution.error_message = "The selected mode is outside the mode-matching basis.";
        return solution;
    }
    if (control.isCancellationRequested()) {
        solution.success = false;
        solution.cancelled = true;
        solution.error_message = "Calculation cancelled.";
        return solution;
    }

    control.reportProgress("Mode matching: coupling integrals and matching system...");

    // Coupling matrix X (count_a x count_b) and the interface admittance matrix
    // G = X^T Y_A X restricted to the aperture basis.
    std::vector<double> coupling(static_cast<std::size_t>(count_a) * count_b, 0.0);
    for (int row = 0; row < count_a; ++row) {
        for (int column = 0; column < count_b; ++column) {
            coupling[static_cast<std::size_t>(row) * count_b + column] =
                couplingIntegral(modes_a[static_cast<std::size_t>(row)], frame_a,
                                 modes_b[static_cast<std::size_t>(column)], frame_b);
        }
    }

    std::vector<Complex> interface(static_cast<std::size_t>(count_b) * count_b,
                                   Complex(0.0, 0.0));
    for (int row = 0; row < count_a; ++row) {
        const Complex admittance = modes_a[static_cast<std::size_t>(row)].admittance;
        const double *coupling_row = &coupling[static_cast<std::size_t>(row) * count_b];
        for (int k = 0; k < count_b; ++k) {
            if (coupling_row[k] == 0.0) {
                continue;
            }
            const Complex weighted = admittance * coupling_row[k];
            for (int l = 0; l < count_b; ++l) {
                interface[static_cast<std::size_t>(k) * count_b + l] +=
                    weighted * coupling_row[l];
            }
        }
    }

    // Through-plate propagation factors P_n = exp(-gamma_n t) with the backward
    // aperture amplitudes referenced at z2, so no growing exponentials appear.
    std::vector<Complex> through(static_cast<std::size_t>(count_b), Complex(0.0, 0.0));
    for (int k = 0; k < count_b; ++k) {
        const Complex gamma = modes_b[static_cast<std::size_t>(k)].gamma;
        through[static_cast<std::size_t>(k)] =
            std::real(gamma) * thickness < 300.0 ? std::exp(-gamma * thickness)
                                                 : Complex(0.0, 0.0);
    }

    // Matching equations for unit incident amplitude a_i = 1 (basis-orthonormal):
    //   (G + Y_B) c + (G - Y_B) P d = 2 X^T Y_A a
    //   (G - Y_B) P c + (G + Y_B) d = 0
    // where the first row enforces E- and H-continuity at z1 and the second at
    // z2 with no wave incident from the far side.
    const int system_size = 2 * count_b;
    std::vector<Complex> system(static_cast<std::size_t>(system_size) * system_size,
                                Complex(0.0, 0.0));
    std::vector<Complex> right_hand_side(static_cast<std::size_t>(system_size),
                                         Complex(0.0, 0.0));
    for (int k = 0; k < count_b; ++k) {
        const Complex aperture_admittance = modes_b[static_cast<std::size_t>(k)].admittance;
        for (int l = 0; l < count_b; ++l) {
            const Complex g = interface[static_cast<std::size_t>(k) * count_b + l];
            const Complex plus = g + (k == l ? aperture_admittance : Complex(0.0, 0.0));
            const Complex minus = g - (k == l ? aperture_admittance : Complex(0.0, 0.0));
            system[static_cast<std::size_t>(k) * system_size + l] = plus;
            system[static_cast<std::size_t>(k) * system_size + count_b + l] =
                minus * through[static_cast<std::size_t>(l)];
            system[static_cast<std::size_t>(count_b + k) * system_size + l] =
                minus * through[static_cast<std::size_t>(l)];
            system[static_cast<std::size_t>(count_b + k) * system_size + count_b + l] = plus;
        }
        right_hand_side[static_cast<std::size_t>(k)] =
            2.0 * modes_a[static_cast<std::size_t>(incident_index)].admittance *
            coupling[static_cast<std::size_t>(incident_index) * count_b + k];
    }

    if (!solveDenseComplex(system, right_hand_side, system_size)) {
        solution.success = false;
        solution.error_message = "The mode-matching system is singular.";
        return solution;
    }
    if (control.isCancellationRequested()) {
        solution.success = false;
        solution.cancelled = true;
        solution.error_message = "Calculation cancelled.";
        return solution;
    }

    const auto aperture_forward = [&](int k) {
        return right_hand_side[static_cast<std::size_t>(k)];
    };
    const auto aperture_backward = [&](int k) {
        return right_hand_side[static_cast<std::size_t>(count_b + k)];
    };

    // Reflected amplitudes b = X (c + P d) - a and transmitted f = X (P c + d).
    std::vector<Complex> reflected(static_cast<std::size_t>(count_a), Complex(0.0, 0.0));
    std::vector<Complex> transmitted(static_cast<std::size_t>(count_a), Complex(0.0, 0.0));
    for (int row = 0; row < count_a; ++row) {
        Complex sum_reflected(0.0, 0.0);
        Complex sum_transmitted(0.0, 0.0);
        const double *coupling_row = &coupling[static_cast<std::size_t>(row) * count_b];
        for (int k = 0; k < count_b; ++k) {
            const Complex pass = through[static_cast<std::size_t>(k)];
            sum_reflected += coupling_row[k] * (aperture_forward(k) + pass * aperture_backward(k));
            sum_transmitted += coupling_row[k] * (pass * aperture_forward(k) + aperture_backward(k));
        }
        reflected[static_cast<std::size_t>(row)] = sum_reflected;
        transmitted[static_cast<std::size_t>(row)] = sum_transmitted;
    }
    reflected[static_cast<std::size_t>(incident_index)] -= 1.0;

    const Complex iris_s11 = reflected[static_cast<std::size_t>(incident_index)];
    const Complex iris_s21 = transmitted[static_cast<std::size_t>(incident_index)];

    const Complex gamma_incident = modes_a[static_cast<std::size_t>(incident_index)].gamma;
    const double distance_in = z1 + 0.5 * guide.length_m;
    const double distance_out = 0.5 * guide.length_m - z2;
    solution.scattering.s11 = iris_s11 * std::exp(-2.0 * gamma_incident * distance_in);
    solution.scattering.s22 = iris_s11 * std::exp(-2.0 * gamma_incident * distance_out);
    solution.scattering.s21 =
        iris_s21 * std::exp(-gamma_incident * (distance_in + distance_out));
    solution.scattering.s12 = solution.scattering.s21;

    const double incident_power = solution.diagnostics.input_power_w;
    const double reflected_fraction = std::norm(iris_s11);
    const double transmitted_fraction = std::norm(iris_s21);
    solution.diagnostics.incident_power_w = incident_power;
    solution.diagnostics.reflected_power_w = reflected_fraction * incident_power;
    solution.diagnostics.transmitted_power_w = transmitted_fraction * incident_power;
    solution.diagnostics.output_power_w = solution.diagnostics.transmitted_power_w;
    solution.diagnostics.dissipated_power_w =
        std::max(0.0, incident_power * (1.0 - reflected_fraction - transmitted_fraction));
    solution.diagnostics.power_balance_relative_error =
        std::abs(1.0 - reflected_fraction - transmitted_fraction);
    {
        std::ostringstream note;
        note << "Mode matching truncation: " << count_a << " guide and " << count_b
             << " aperture modes (relative convergence " << guide_m << "x" << guide_n
             << " / " << aperture_m << "x" << aperture_n << ").";
        solution.diagnostics.warnings.push_back(note.str());
    }
    if (guide.wall_conductivity_s_per_m > 0.0) {
        solution.diagnostics.warnings.push_back(
            "Wall conductor loss is not included in the mode-matching backend.");
    }

    control.reportProgress("Mode matching: reconstructing the field...");

    // Physical incident transverse amplitude at z1 links the orthonormal basis
    // to the 1 W normalized incident field of the empty-guide solver.
    const GuideMode &incident_mode = modes_a[static_cast<std::size_t>(incident_index)];
    const Complex j(0.0, 1.0);
    Complex pattern_scale;
    if (incident_mode.family == ModeFamily::TransverseElectric) {
        pattern_scale = j * omega * mu / (incident_mode.kc2 * incident_mode.inv_norm);
    } else {
        pattern_scale = -incident_mode.gamma / (incident_mode.kc2 * incident_mode.inv_norm);
    }
    const Complex incident_amplitude = incident.forward_longitudinal_amplitude *
                                       std::exp(-gamma_incident * distance_in) *
                                       pattern_scale;

    std::vector<EvaluatorMode> region_a;
    region_a.reserve(static_cast<std::size_t>(count_a));
    for (int row = 0; row < count_a; ++row) {
        EvaluatorMode entry;
        entry.mode = modes_a[static_cast<std::size_t>(row)];
        entry.frame = frame_a;
        entry.mu = mu;
        entry.forward = row == incident_index ? incident_amplitude : Complex(0.0, 0.0);
        entry.backward = reflected[static_cast<std::size_t>(row)] * incident_amplitude;
        entry.z_forward_ref = z1;
        entry.z_backward_ref = z1;
        region_a.push_back(entry);
    }
    std::vector<EvaluatorMode> region_b;
    region_b.reserve(static_cast<std::size_t>(count_b));
    for (int k = 0; k < count_b; ++k) {
        EvaluatorMode entry;
        entry.mode = modes_b[static_cast<std::size_t>(k)];
        entry.frame = frame_b;
        entry.mu = mu;
        entry.forward = aperture_forward(k) * incident_amplitude;
        entry.backward = aperture_backward(k) * incident_amplitude;
        entry.z_forward_ref = z1;
        entry.z_backward_ref = z2;
        region_b.push_back(entry);
    }
    std::vector<EvaluatorMode> region_c;
    region_c.reserve(static_cast<std::size_t>(count_a));
    for (int row = 0; row < count_a; ++row) {
        EvaluatorMode entry;
        entry.mode = modes_a[static_cast<std::size_t>(row)];
        entry.frame = frame_a;
        entry.mu = mu;
        entry.forward = transmitted[static_cast<std::size_t>(row)] * incident_amplitude;
        entry.backward = Complex(0.0, 0.0);
        entry.z_forward_ref = z2;
        entry.z_backward_ref = z2;
        region_c.push_back(entry);
    }
    constexpr std::size_t maximum_evaluator_modes = 240;
    pruneModes(region_a, maximum_evaluator_modes);
    pruneModes(region_b, maximum_evaluator_modes);
    pruneModes(region_c, maximum_evaluator_modes);

    solution.forward_longitudinal_amplitude = incident.forward_longitudinal_amplitude;
    solution.backward_longitudinal_amplitude =
        incident.forward_longitudinal_amplitude * solution.scattering.s11;
    solution.field = std::make_shared<ModeMatchingFieldEvaluator>(guide,
                                                                  frame_b,
                                                                  z1,
                                                                  z2,
                                                                  omega,
                                                                  std::move(region_a),
                                                                  std::move(region_b),
                                                                  std::move(region_c));
    return solution;
}
}
