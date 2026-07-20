#include "mfem_frequency_domain_backend.h"

#include "rectangular_waveguide_solver.h"

#ifdef KRUTIEV_MFEM_CONFIG_FILE
#define MFEM_CONFIG_FILE KRUTIEV_MFEM_CONFIG_FILE
#endif
#include <mfem.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <limits>
#include <memory>
#include <mutex>
#include <sstream>
#include <vector>

namespace em
{
namespace
{
struct ElementGrid
{
    double minimum[3] = {0.0, 0.0, 0.0};
    double cell_size[3] = {1.0, 1.0, 1.0};
    int cells[3] = {1, 1, 1};
    std::vector<std::array<double, 6>> element_bounds;
    std::vector<std::vector<int>> cell_elements;
};

struct FemState
{
    std::unique_ptr<mfem::Mesh> mesh;
    std::unique_ptr<mfem::ND_FECollection> collection;
    std::unique_ptr<mfem::FiniteElementSpace> space;
    std::unique_ptr<mfem::ComplexGridFunction> electric;
    std::vector<Material> materials;
    double omega = 0.0;
    ElementGrid grid;
};

int gridCoordinate(const ElementGrid &grid, int axis, double value)
{
    const int cell = static_cast<int>((value - grid.minimum[axis]) /
                                      grid.cell_size[axis]);
    return std::clamp(cell, 0, grid.cells[axis] - 1);
}

int gridCellIndex(const ElementGrid &grid, int x_cell, int y_cell, int z_cell)
{
    return (z_cell * grid.cells[1] + y_cell) * grid.cells[0] + x_cell;
}

void buildElementGrid(FemState &state)
{
    mfem::Mesh &mesh = *state.mesh;
    ElementGrid &grid = state.grid;
    const int element_count = mesh.GetNE();
    constexpr double infinity = std::numeric_limits<double>::infinity();
    double global_minimum[3] = {infinity, infinity, infinity};
    double global_maximum[3] = {-infinity, -infinity, -infinity};

    grid.element_bounds.assign(element_count,
                               {infinity, infinity, infinity,
                                -infinity, -infinity, -infinity});
    for (int element = 0; element < element_count; ++element) {
        mfem::Array<int> vertices;
        mesh.GetElementVertices(element, vertices);
        std::array<double, 6> &bounds = grid.element_bounds[element];
        for (int vertex = 0; vertex < vertices.Size(); ++vertex) {
            const double *coordinates = mesh.GetVertex(vertices[vertex]);
            for (int axis = 0; axis < 3; ++axis) {
                bounds[axis] = std::min(bounds[axis], coordinates[axis]);
                bounds[axis + 3] = std::max(bounds[axis + 3], coordinates[axis]);
            }
        }
        for (int axis = 0; axis < 3; ++axis) {
            const double padding = 1.0e-10 +
                                   1.0e-6 * (bounds[axis + 3] - bounds[axis]);
            bounds[axis] -= padding;
            bounds[axis + 3] += padding;
            global_minimum[axis] = std::min(global_minimum[axis], bounds[axis]);
            global_maximum[axis] = std::max(global_maximum[axis], bounds[axis + 3]);
        }
    }

    const int cells_per_axis = std::clamp(
        static_cast<int>(std::cbrt(static_cast<double>(std::max(1, element_count)))),
        1,
        48);
    for (int axis = 0; axis < 3; ++axis) {
        grid.minimum[axis] = global_minimum[axis];
        grid.cells[axis] = cells_per_axis;
        grid.cell_size[axis] = std::max(1.0e-12,
                                        (global_maximum[axis] - global_minimum[axis]) /
                                            cells_per_axis);
    }
    grid.cell_elements.assign(static_cast<std::size_t>(cells_per_axis) *
                                  cells_per_axis * cells_per_axis,
                              {});
    for (int element = 0; element < element_count; ++element) {
        const std::array<double, 6> &bounds = grid.element_bounds[element];
        const int x_first = gridCoordinate(grid, 0, bounds[0]);
        const int x_last = gridCoordinate(grid, 0, bounds[3]);
        const int y_first = gridCoordinate(grid, 1, bounds[1]);
        const int y_last = gridCoordinate(grid, 1, bounds[4]);
        const int z_first = gridCoordinate(grid, 2, bounds[2]);
        const int z_last = gridCoordinate(grid, 2, bounds[5]);
        for (int z_cell = z_first; z_cell <= z_last; ++z_cell) {
            for (int y_cell = y_first; y_cell <= y_last; ++y_cell) {
                for (int x_cell = x_first; x_cell <= x_last; ++x_cell) {
                    grid.cell_elements[gridCellIndex(grid, x_cell, y_cell, z_cell)]
                        .push_back(element);
                }
            }
        }
    }
}

struct PmlDomain
{
    bool enabled = false;
    double x_min = 0.0;
    double x_max = 0.0;
    double y_min = 0.0;
    double y_max = 0.0;
    double thickness = 0.0;
    int order = 3;
    double attenuation = 0.0;
    double wavenumber = 0.0;
};

Complex stretch(double coordinate, double minimum, double maximum, const PmlDomain &pml)
{
    if (!pml.enabled || !(pml.thickness > 0.0) || !(pml.wavenumber > 0.0)) {
        return 1.0;
    }
    double distance = 0.0;
    if (coordinate < minimum) {
        distance = minimum - coordinate;
    } else if (coordinate > maximum) {
        distance = coordinate - maximum;
    }
    if (!(distance > 0.0)) {
        return 1.0;
    }
    distance = std::min(distance, pml.thickness);
    const double coefficient = pml.order * pml.attenuation /
                               (pml.wavenumber * std::pow(pml.thickness, pml.order));
    // e^{+j omega t} convention: the imaginary stretch must be negative so the
    // outgoing wave e^{-j k s y} decays inside the layer instead of growing.
    return Complex(1.0,
                   -coefficient * std::pow(distance, static_cast<double>(pml.order - 1)));
}

class MaxwellMatrixCoefficient final : public mfem::MatrixCoefficient
{
public:
    enum class Term
    {
        Curl,
        Mass
    };

    MaxwellMatrixCoefficient(const std::vector<Material> &materials,
                             const PmlDomain &pml,
                             double omega,
                             Term term,
                             bool imaginary,
                             bool positive = false)
        : mfem::MatrixCoefficient(3),
          materials_(materials),
          pml_(pml),
          omega_(omega),
          term_(term),
          imaginary_(imaginary),
          positive_(positive)
    {
    }

    void Eval(mfem::DenseMatrix &matrix,
              mfem::ElementTransformation &transformation,
              const mfem::IntegrationPoint &point) override
    {
        mfem::Vector position(3);
        transformation.Transform(point, position);
        const std::size_t material_index =
            transformation.Attribute > 0 &&
                    static_cast<std::size_t>(transformation.Attribute) <= materials_.size()
                ? static_cast<std::size_t>(transformation.Attribute - 1)
                : 0;
        const Material &material = materials_[material_index];
        const Complex mu = vacuum_permeability_h_per_m * material.relative_permeability;
        const Complex epsilon =
            vacuum_permittivity_f_per_m * material.relative_permittivity -
            Complex(0.0, material.conductivity_s_per_m / omega_);
        const Complex physical_base = term_ == Term::Curl ? 1.0 / mu
                                                          : -omega_ * omega_ * epsilon;
        const Complex base = positive_
                                 ? Complex(std::abs(physical_base), 0.0)
                                 : physical_base;

        const Complex sx = stretch(position[0], pml_.x_min, pml_.x_max, pml_);
        const Complex sy = stretch(position[1], pml_.y_min, pml_.y_max, pml_);
        const Complex sz = 1.0;
        const Complex determinant = sx * sy * sz;
        const Complex factors[3] = {
            term_ == Term::Curl ? sx * sx / determinant : determinant / (sx * sx),
            term_ == Term::Curl ? sy * sy / determinant : determinant / (sy * sy),
            term_ == Term::Curl ? sz * sz / determinant : determinant / (sz * sz),
        };

        matrix.SetSize(3);
        matrix = 0.0;
        for (int component = 0; component < 3; ++component) {
            const Complex value = positive_
                                      ? base * std::abs(factors[component])
                                      : base * factors[component];
            matrix(component, component) = imaginary_ ? std::imag(value) : std::real(value);
        }
    }

private:
    const std::vector<Material> &materials_;
    PmlDomain pml_;
    double omega_ = 0.0;
    Term term_ = Term::Curl;
    bool imaginary_ = false;
    bool positive_ = false;
};

class SolveProgressMonitor final : public mfem::IterativeSolverMonitor
{
public:
    SolveProgressMonitor(const SolveControl &control,
                         std::string label,
                         int maximum_iterations)
        : control_(control),
          label_(std::move(label)),
          maximum_iterations_(maximum_iterations)
    {
    }

    void MonitorResidual(int iteration,
                         double norm,
                         const mfem::Vector &,
                         bool final) override
    {
        if (iteration == 0 || !(initial_norm_ > 0.0)) {
            initial_norm_ = std::max(static_cast<double>(norm), 1.0e-300);
        }
        if (final || iteration % 100 == 0) {
            std::ostringstream stage;
            stage << label_ << ": iteration " << iteration << " of "
                  << maximum_iterations_ << ", relative residual "
                  << std::scientific << std::setprecision(2)
                  << norm / initial_norm_;
            control_.reportProgress(stage.str());
        }
    }

private:
    const SolveControl &control_;
    std::string label_;
    int maximum_iterations_ = 0;
    double initial_norm_ = 0.0;
};

bool findPoint(const FemState &state,
               const Vec3 &position,
               int &element_index,
               mfem::IntegrationPoint &reference_point)
{
    const ElementGrid &grid = state.grid;
    if (grid.cell_elements.empty()) {
        return false;
    }
    const double coordinates[3] = {position.x, position.y, position.z};
    for (int axis = 0; axis < 3; ++axis) {
        const double extent = grid.cell_size[axis] * grid.cells[axis];
        if (coordinates[axis] < grid.minimum[axis] ||
            coordinates[axis] > grid.minimum[axis] + extent) {
            return false;
        }
    }

    mfem::Vector physical_point(3);
    physical_point[0] = position.x;
    physical_point[1] = position.y;
    physical_point[2] = position.z;
    const int cell_index = gridCellIndex(grid,
                                         gridCoordinate(grid, 0, position.x),
                                         gridCoordinate(grid, 1, position.y),
                                         gridCoordinate(grid, 2, position.z));
    for (const int index : grid.cell_elements[cell_index]) {
        const std::array<double, 6> &bounds = grid.element_bounds[index];
        if (position.x < bounds[0] || position.x > bounds[3] ||
            position.y < bounds[1] || position.y > bounds[4] ||
            position.z < bounds[2] || position.z > bounds[5]) {
            continue;
        }
        // A caller-owned transformation keeps point location thread-safe:
        // Mesh::GetElementTransformation(index) hands out a pointer to shared
        // mesh state, which concurrent samplers would corrupt.
        mfem::IsoparametricTransformation local_transformation;
        state.mesh->GetElementTransformation(index, &local_transformation);
        mfem::InverseElementTransformation inverse(&local_transformation);
        inverse.SetPrintLevel(-1);
        const int result = inverse.Transform(physical_point, reference_point);
        if (result == mfem::InverseElementTransformation::Inside) {
            element_index = index;
            return true;
        }
    }
    return false;
}

class MfemFieldEvaluator final : public IFieldEvaluator
{
public:
    explicit MfemFieldEvaluator(std::shared_ptr<const FemState> state)
        : state_(std::move(state))
    {
    }

    // MFEM keeps shared mutable state inside FiniteElementSpace/GridFunction
    // evaluation (beyond the element transformation, which is handled locally
    // below), so sampling must stay on one thread.
    bool supportsConcurrentEvaluation() const override { return false; }

    bool contains(const Vec3 &position_m) const override
    {
        int element_index = -1;
        mfem::IntegrationPoint point;
        return findPoint(*state_, position_m, element_index, point);
    }

    FieldPhasor evaluate(const Vec3 &position_m) const override
    {
        int element_index = -1;
        mfem::IntegrationPoint point;
        if (!findPoint(*state_, position_m, element_index, point)) {
            return {};
        }
        // Local transformation (not the mesh-owned shared one) so that several
        // threads can sample the field concurrently.
        mfem::IsoparametricTransformation transformation;
        state_->mesh->GetElementTransformation(element_index, &transformation);
        transformation.SetIntPoint(&point);

        mfem::Vector electric_real(3);
        mfem::Vector electric_imaginary(3);
        mfem::Vector curl_real(3);
        mfem::Vector curl_imaginary(3);
        state_->electric->real().GetVectorValue(transformation, point, electric_real);
        state_->electric->imag().GetVectorValue(transformation, point, electric_imaginary);
        state_->electric->real().GetCurl(transformation, curl_real);
        state_->electric->imag().GetCurl(transformation, curl_imaginary);

        const int attribute = state_->mesh->GetAttribute(element_index);
        const std::size_t material_index =
            attribute > 0 && static_cast<std::size_t>(attribute) <= state_->materials.size()
                ? static_cast<std::size_t>(attribute - 1)
                : 0;
        const Complex mu = vacuum_permeability_h_per_m *
                           state_->materials[material_index].relative_permeability;
        const Complex magnetic_factor = Complex(0.0, 1.0) / (state_->omega * mu);

        FieldPhasor result;
        result.electric_v_per_m = {
            Complex(electric_real[0], electric_imaginary[0]),
            Complex(electric_real[1], electric_imaginary[1]),
            Complex(electric_real[2], electric_imaginary[2]),
        };
        result.magnetic_a_per_m = {
            magnetic_factor * Complex(curl_real[0], curl_imaginary[0]),
            magnetic_factor * Complex(curl_real[1], curl_imaginary[1]),
            magnetic_factor * Complex(curl_real[2], curl_imaginary[2]),
        };
        return result;
    }

private:
    std::shared_ptr<const FemState> state_;
};

std::filesystem::path uniqueWorkingDirectory(const std::filesystem::path &base)
{
    static std::atomic<unsigned long long> sequence{0};
    const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
    return base / ("solve_" + std::to_string(ticks) + "_" +
                   std::to_string(sequence.fetch_add(1)));
}

mfem::Array<int> boundaryMarker(const mfem::Mesh &mesh, int attribute)
{
    mfem::Array<int> marker(mesh.bdr_attributes.Size() ? mesh.bdr_attributes.Max() : 0);
    marker = 0;
    if (attribute > 0 && attribute <= marker.Size()) {
        marker[attribute - 1] = 1;
    }
    return marker;
}

Complex projectPortElectric(const IFieldEvaluator &field,
                            const IFieldEvaluator &mode,
                            const RectangularWaveguideGeometry &geometry,
                            double z_m)
{
    constexpr int x_samples = 36;
    constexpr int y_samples = 24;
    Complex numerator = 0.0;
    double denominator = 0.0;
    for (int x_index = 0; x_index < x_samples; ++x_index) {
        const double x_m = -0.5 * geometry.inner_width_m +
                           (x_index + 0.5) * geometry.inner_width_m / x_samples;
        for (int y_index = 0; y_index < y_samples; ++y_index) {
            const double y_m = -0.5 * geometry.inner_height_m +
                               (y_index + 0.5) * geometry.inner_height_m / y_samples;
            const ComplexVec3 value = field.evaluate({x_m, y_m, z_m}).electric_v_per_m;
            const ComplexVec3 reference =
                mode.evaluate({x_m, y_m, -0.5 * geometry.length_m}).electric_v_per_m;
            numerator += value.x * std::conj(reference.x) +
                         value.y * std::conj(reference.y) +
                         value.z * std::conj(reference.z);
            denominator += std::norm(reference.x) + std::norm(reference.y) +
                           std::norm(reference.z);
        }
    }
    return denominator > 0.0 ? numerator / denominator : Complex{};
}
}

MfemFrequencyDomainBackend::MfemFrequencyDomainBackend(
    std::filesystem::path gmsh_executable,
    std::filesystem::path working_directory)
    : mesher_(std::move(gmsh_executable)),
      working_directory_(working_directory.empty()
                             ? std::filesystem::temp_directory_path() / "krutiev_fem"
                             : std::move(working_directory))
{
}

FieldSolution MfemFrequencyDomainBackend::solve(const SimulationRequest &request,
                                                const SolveControl &control) const
{
    FieldSolution solution;
    solution.request = request;
    solution.diagnostics.backend_name = "MFEM complex H(curl) FEM";
    if (control.isCancellationRequested()) {
        solution.cancelled = true;
        solution.error_message = "Calculation cancelled.";
        return solution;
    }

    const std::filesystem::path solve_directory =
        uniqueWorkingDirectory(working_directory_);
    control.reportProgress("Gmsh: building the tetrahedral mesh...");
    const FemMeshFiles mesh_files = mesher_.generate(request, solve_directory);
    if (!mesh_files.error_message.empty()) {
        solution.error_message = mesh_files.error_message;
        return solution;
    }

    auto state = std::make_shared<FemState>();
    try {
        state->mesh = std::make_unique<mfem::Mesh>(mesh_files.mesh_path.string().c_str(), 1, 1);
    } catch (const std::exception &exception) {
        solution.error_message = std::string("MFEM cannot read tetrahedral mesh: ") +
                                 exception.what();
        return solution;
    }
    for (int level = 0; level < request.settings.fem.mesh.uniform_refinement_levels; ++level) {
        state->mesh->UniformRefinement();
    }
    if (state->mesh->Dimension() != 3) {
        solution.error_message = "FEM mesh is not three-dimensional.";
        return solution;
    }
    buildElementGrid(*state);

    state->materials.push_back(request.model.filling_material);
    for (const DielectricBlockGeometry &block : request.model.dielectric_blocks) {
        if (block.enabled) {
            state->materials.push_back(block.material);
        }
    }
    state->omega = 2.0 * pi * request.frequency_hz;
    state->collection = std::make_unique<mfem::ND_FECollection>(
        request.settings.fem.mesh.element_order, 3);
    state->space = std::make_unique<mfem::FiniteElementSpace>(
        state->mesh.get(), state->collection.get());
    state->electric = std::make_unique<mfem::ComplexGridFunction>(state->space.get());
    *state->electric = 0.0;

    solution.diagnostics.mesh_tetrahedron_count = state->mesh->GetNE();
    solution.diagnostics.fem_unknown_count = state->space->GetTrueVSize();
    {
        std::ostringstream stage;
        stage << "FEM mesh ready: " << solution.diagnostics.mesh_tetrahedron_count
              << " tetrahedra, " << solution.diagnostics.fem_unknown_count
              << " unknowns.";
        control.reportProgress(stage.str());
    }

    const auto &waveguide = request.model.waveguide;
    const Complex material_product = request.model.filling_material.relative_permittivity *
                                     request.model.filling_material.relative_permeability;
    const double bulk_wavenumber = state->omega / speed_of_light_m_per_s *
                                   std::sqrt(std::max(0.0, std::real(material_product)));
    const int m = request.excitation.automatic ? 1 : request.excitation.m;
    const int n = request.excitation.automatic ? 0 : request.excitation.n;
    const double cutoff_wavenumber = std::hypot(m * pi / waveguide.inner_width_m,
                                                n * pi / waveguide.inner_height_m);
    if (!(bulk_wavenumber > cutoff_wavenumber)) {
        solution.error_message = "Selected port mode is below cutoff at the FEM frequency.";
        return solution;
    }
    const double beta = std::sqrt(bulk_wavenumber * bulk_wavenumber -
                                  cutoff_wavenumber * cutoff_wavenumber);

    PmlDomain pml;
    const bool has_slots = std::any_of(request.model.slot_geometries.begin(),
                                       request.model.slot_geometries.end(),
                                       [](const SlotGeometry &slot) { return slot.enabled; });
    pml.enabled = has_slots && request.settings.fem.pml.enabled;
    pml.thickness = request.settings.fem.pml.thickness_m > 0.0
                        ? request.settings.fem.pml.thickness_m
                        : 0.35 * speed_of_light_m_per_s / request.frequency_hz;
    pml.x_min = -0.5 * waveguide.inner_width_m - waveguide.wall_thickness_m - pml.thickness;
    pml.x_max = 0.5 * waveguide.inner_width_m + waveguide.wall_thickness_m + pml.thickness;
    pml.y_min = -0.5 * waveguide.inner_height_m - waveguide.wall_thickness_m - pml.thickness;
    pml.y_max = 0.5 * waveguide.inner_height_m + waveguide.wall_thickness_m + pml.thickness;
    pml.order = request.settings.fem.pml.polynomial_order;
    pml.attenuation = -0.5 * std::log(request.settings.fem.pml.target_reflection);
    pml.wavenumber = bulk_wavenumber;

    MaxwellMatrixCoefficient curl_real(state->materials, pml, state->omega,
                                        MaxwellMatrixCoefficient::Term::Curl, false);
    MaxwellMatrixCoefficient curl_imaginary(state->materials, pml, state->omega,
                                             MaxwellMatrixCoefficient::Term::Curl, true);
    MaxwellMatrixCoefficient mass_real(state->materials, pml, state->omega,
                                        MaxwellMatrixCoefficient::Term::Mass, false);
    MaxwellMatrixCoefficient mass_imaginary(state->materials, pml, state->omega,
                                             MaxwellMatrixCoefficient::Term::Mass, true);

    constexpr auto convention = mfem::ComplexOperator::HERMITIAN;
    mfem::SesquilinearForm form(state->space.get(), convention);
    form.AddDomainIntegrator(new mfem::CurlCurlIntegrator(curl_real),
                             new mfem::CurlCurlIntegrator(curl_imaginary));
    form.AddDomainIntegrator(new mfem::VectorFEMassIntegrator(mass_real),
                             new mfem::VectorFEMassIntegrator(mass_imaginary));

    mfem::Array<int> input_marker = boundaryMarker(*state->mesh, 101);
    mfem::Array<int> output_marker = boundaryMarker(*state->mesh, 102);
    mfem::Array<int> pec_marker = boundaryMarker(*state->mesh, 103);
    mfem::Array<int> port_marker = input_marker;
    for (int index = 0; index < port_marker.Size(); ++index) {
        port_marker[index] = port_marker[index] || output_marker[index];
    }

    const Complex mu = vacuum_permeability_h_per_m *
                       request.model.filling_material.relative_permeability;
    const Complex port_coefficient = Complex(0.0, beta) / mu;
    mfem::ConstantCoefficient port_real(std::real(port_coefficient));
    mfem::ConstantCoefficient port_imaginary(std::imag(port_coefficient));
    form.AddBoundaryIntegrator(new mfem::VectorFEMassIntegrator(port_real),
                               new mfem::VectorFEMassIntegrator(port_imaginary),
                               port_marker);

    SimulationRequest incident_request = request;
    incident_request.model.slot_geometries.clear();
    incident_request.model.pec_plates.clear();
    incident_request.model.dielectric_blocks.clear();
    const FieldSolution incident = RectangularWaveguideSolver().solve(incident_request);
    if (!incident.success || !incident.field) {
        solution.error_message = "Cannot construct the normalized incident port mode: " +
                                 incident.error_message;
        return solution;
    }
    solution.available_modes = incident.available_modes;
    solution.has_selected_mode = incident.has_selected_mode;
    solution.selected_mode = incident.selected_mode;

    const double input_port_z = -0.5 * waveguide.length_m;
    auto source_value = [field = incident.field, port_coefficient, input_port_z](
                            const mfem::Vector &position,
                            mfem::Vector &value,
                            bool imaginary) {
        const FieldPhasor sample = field->evaluate({position[0], position[1], input_port_z});
        const Complex scale = 2.0 * port_coefficient;
        const Complex components[3] = {
            scale * sample.electric_v_per_m.x,
            scale * sample.electric_v_per_m.y,
            scale * sample.electric_v_per_m.z,
        };
        value.SetSize(3);
        for (int component = 0; component < 3; ++component) {
            value[component] = imaginary ? std::imag(components[component])
                                         : std::real(components[component]);
        }
    };
    mfem::VectorFunctionCoefficient source_real(
        3, [source_value](const mfem::Vector &position, mfem::Vector &value) {
            source_value(position, value, false);
        });
    mfem::VectorFunctionCoefficient source_imaginary(
        3, [source_value](const mfem::Vector &position, mfem::Vector &value) {
            source_value(position, value, true);
        });
    mfem::ComplexLinearForm right_hand_side(state->space.get(), convention);
    right_hand_side.AddBoundaryIntegrator(
        new mfem::VectorFEDomainLFIntegrator(source_real),
        new mfem::VectorFEDomainLFIntegrator(source_imaginary),
        input_marker);
    right_hand_side = 0.0;
    right_hand_side.Assemble();

    mfem::Array<int> essential_dofs;
    state->space->GetEssentialTrueDofs(pec_marker, essential_dofs);
    control.reportProgress("Assembling the complex H(curl) FEM system...");
    form.Assemble(0);
    mfem::OperatorPtr system_operator;
    mfem::Vector right_vector;
    mfem::Vector solution_vector;
    form.FormLinearSystem(essential_dofs,
                          *state->electric,
                          right_hand_side,
                          system_operator,
                          solution_vector,
                          right_vector);
    const double right_norm = right_vector.Norml2();
    if (!(right_norm > 0.0) || !std::isfinite(right_norm)) {
        solution.error_message = "FEM port excitation assembled a zero right-hand side.";
        return solution;
    }

    MaxwellMatrixCoefficient positive_curl(state->materials, pml, state->omega,
                                             MaxwellMatrixCoefficient::Term::Curl,
                                             false, true);
    MaxwellMatrixCoefficient positive_mass(state->materials, pml, state->omega,
                                             MaxwellMatrixCoefficient::Term::Mass,
                                             false, true);
    mfem::BilinearForm preconditioner_form(state->space.get());
    preconditioner_form.AddDomainIntegrator(new mfem::CurlCurlIntegrator(positive_curl));
    preconditioner_form.AddDomainIntegrator(new mfem::VectorFEMassIntegrator(positive_mass));
    preconditioner_form.Assemble();
    mfem::OperatorPtr preconditioner_operator;
    preconditioner_form.SetDiagonalPolicy(mfem::Operator::DIAG_ONE);
    preconditioner_form.FormSystemMatrix(essential_dofs, preconditioner_operator);
    mfem::GSSmoother real_smoother(*preconditioner_operator.As<mfem::SparseMatrix>());
    mfem::ScaledOperator imaginary_smoother(&real_smoother, -1.0);
    mfem::Array<int> block_offsets(3);
    block_offsets[0] = 0;
    block_offsets[1] = state->space->GetTrueVSize();
    block_offsets[2] = state->space->GetTrueVSize();
    block_offsets.PartialSum();
    mfem::BlockDiagonalPreconditioner block_preconditioner(block_offsets);
    block_preconditioner.SetDiagonalBlock(0, &real_smoother);
    block_preconditioner.SetDiagonalBlock(1, &imaginary_smoother);

    mfem::GMRESSolver solver;
    solver.SetPrintLevel(-1);
    solver.SetKDim(200);
    solver.SetMaxIter(request.settings.fem.maximum_iterations);
    solver.SetRelTol(request.settings.fem.relative_tolerance);
    solver.SetAbsTol(0.0);
    solver.SetOperator(*system_operator);
    solver.SetPreconditioner(block_preconditioner);
    SolveProgressMonitor solve_monitor(control,
                                       "GMRES",
                                       request.settings.fem.maximum_iterations);
    solver.SetMonitor(solve_monitor);
    control.reportProgress("GMRES: solving the linear system...");
    solver.Mult(right_vector, solution_vector);
    const auto explicitRelativeResidual = [&]() {
        mfem::Vector residual(right_vector.Size());
        system_operator->Mult(solution_vector, residual);
        residual -= right_vector;
        return residual.Norml2() / right_norm;
    };
    int total_iterations = solver.GetNumIterations();
    double relative_residual = explicitRelativeResidual();
    const double accepted_tolerance = std::max(request.settings.fem.relative_tolerance,
                                               1.0e-6);
    constexpr double usable_relative_residual = 1.0e-3;
    if (!solver.GetConverged() && relative_residual > accepted_tolerance) {
        // The GS-preconditioned indefinite system often stalls near 1e-4,
        // which is still far below the FEM discretization error, so the retry
        // aims at a genuinely relaxed target instead of repeating the same
        // unreachable tolerance. A cold restart with the larger Krylov space
        // converges further than warming up from the stalled iterate.
        const double relaxed_tolerance = std::clamp(accepted_tolerance * 100.0,
                                                    1.0e-4,
                                                    usable_relative_residual);
        const int retry_iterations = std::max(6000,
                                              3 * request.settings.fem.maximum_iterations);
        mfem::GMRESSolver retry_solver;
        retry_solver.SetPrintLevel(-1);
        retry_solver.SetKDim(500);
        retry_solver.SetMaxIter(retry_iterations);
        retry_solver.SetRelTol(relaxed_tolerance);
        retry_solver.SetAbsTol(0.0);
        retry_solver.SetOperator(*system_operator);
        retry_solver.SetPreconditioner(block_preconditioner);
        SolveProgressMonitor retry_monitor(control,
                                           "GMRES retry (relaxed tolerance)",
                                           retry_iterations);
        retry_solver.SetMonitor(retry_monitor);
        control.reportProgress("GMRES did not converge, retrying with a relaxed tolerance...");
        retry_solver.Mult(right_vector, solution_vector);
        total_iterations += retry_solver.GetNumIterations();
        relative_residual = explicitRelativeResidual();
        if (!(relative_residual <= usable_relative_residual)) {
            solution.diagnostics.linear_iterations = total_iterations;
            solution.diagnostics.linear_relative_residual = relative_residual;
            std::ostringstream error;
            error << "MFEM GMRES did not converge: relative residual="
                  << relative_residual << ", iterations=" << total_iterations << ".";
            solution.error_message = error.str();
            return solution;
        }
        if (relative_residual > relaxed_tolerance) {
            std::ostringstream stall_warning;
            stall_warning << "GMRES stalled at relative residual " << relative_residual
                          << " (requested " << request.settings.fem.relative_tolerance
                          << "); fields and S-parameters are approximate.";
            solution.diagnostics.warnings.push_back(stall_warning.str());
        }
    }
    solution.diagnostics.linear_iterations = total_iterations;
    solution.diagnostics.linear_relative_residual = relative_residual;
    form.RecoverFEMSolution(solution_vector, right_hand_side, *state->electric);

    solution.field = std::make_shared<MfemFieldEvaluator>(state);
    control.reportProgress("Projecting S-parameters at the ports...");
    const double sampling_offset = std::max(1.0e-8, waveguide.length_m * 1.0e-6);
    const double input_z = -0.5 * waveguide.length_m + sampling_offset;
    const double output_z = 0.5 * waveguide.length_m - sampling_offset;
    const double input_distance = sampling_offset;
    const double output_distance = waveguide.length_m - sampling_offset;
    const Complex input_projection =
        projectPortElectric(*solution.field, *incident.field, waveguide, input_z);
    const Complex output_projection =
        projectPortElectric(*solution.field, *incident.field, waveguide, output_z);
    const Complex incident_at_input = std::exp(Complex(0.0, -beta * input_distance));
    solution.scattering.s11 =
        (input_projection - incident_at_input) *
        std::exp(Complex(0.0, -beta * input_distance));
    solution.scattering.s21 =
        output_projection * std::exp(Complex(0.0, beta * output_distance));
    solution.diagnostics.incident_power_w = request.settings.normalization_power_w;
    solution.diagnostics.reflected_power_w =
        std::norm(solution.scattering.s11) * solution.diagnostics.incident_power_w;
    solution.diagnostics.transmitted_power_w =
        std::norm(solution.scattering.s21) * solution.diagnostics.incident_power_w;
    solution.diagnostics.dissipated_power_w = std::max(
        0.0,
        solution.diagnostics.incident_power_w - solution.diagnostics.reflected_power_w -
            solution.diagnostics.transmitted_power_w);
    solution.diagnostics.warnings.push_back(
        "S11 and S21 are projected for port-1 excitation; S12/S22 require a second solve.");
    solution.diagnostics.estimated_pml_reflection =
        pml.enabled ? request.settings.fem.pml.target_reflection : 0.0;
    solution.success = true;
    return solution;
}

std::shared_ptr<const IFemFrequencyDomainBackend> createDefaultFemBackend()
{
    return std::make_shared<MfemFrequencyDomainBackend>();
}
}
