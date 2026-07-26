#include "mfem_frequency_domain_backend.h"

// Before anything else, so that no other header can pull <windows.h> in without
// these guards: the min/max macros collide with std::min/std::max used all over
// this file. Only GlobalMemoryStatusEx is wanted from it.
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include "analytic_waveguide_solver.h"

#ifdef KRUTIEV_MFEM_CONFIG_FILE
#define MFEM_CONFIG_FILE KRUTIEV_MFEM_CONFIG_FILE
#endif
#include <mfem.hpp>

#ifdef KRUTIEV_WITH_EIGEN
#ifdef _MSC_VER
// Eigen is noisy under /W4: constant conditionals in its template dispatch and
// locals that shadow class members. Not our code, and not worth 80 warnings.
#pragma warning(push)
#pragma warning(disable : 4127 4458)
#endif
#include <Eigen/SparseCore>
#include <Eigen/SparseLU>
#ifdef _MSC_VER
#pragma warning(pop)
#endif
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <future>
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
            // MFEM hands the monitor the norm of M(b - A x), not of b - A x, so
            // it is a different number from SolverDiagnostics::
            // linear_relative_residual and has to be named differently or the
            // two get compared.
            std::ostringstream stage;
            stage << label_ << ": iteration " << iteration << " of "
                  << maximum_iterations_ << ", preconditioned relative residual "
                  << std::scientific << std::setprecision(2)
                  << norm / initial_norm_;
            control_.reportProgress(stage.str());
        }
        if (control_.isCancellationRequested()) {
            // The one way out of an MFEM Krylov solve: IterativeSolver stops as
            // soon as its controller claims convergence. Nothing downstream may
            // trust that claim, so the flag below is what the caller reads.
            cancelled_ = true;
            converged = true;
        }
    }

    bool wasCancelled() const { return cancelled_; }

private:
    const SolveControl &control_;
    std::string label_;
    int maximum_iterations_ = 0;
    double initial_norm_ = 0.0;
    bool cancelled_ = false;
};

// Peak process memory of the sparse LU factorisation on the real 2N x 2N block
// system, in gigabytes, as a function of the complex unknown count N. Fill-in
// grows faster than the matrix, so this is a power law and not a line, and the
// exponent is chosen to keep the curve above every measured point: an
// underestimate means a half-hour factorisation that ends in thrashing or
// bad_alloc, an overestimate only sends the model to the iterative solver or to
// a clear refusal. Peaks measured in Release, estimate in brackets:
//   21953 unknowns 0.69 GB (0.74)   63033 3.56 GB (4.92)
//  104839 unknowns 6.01 GB (12.2)  112246 10.89 GB (13.8)
// and, on earlier runs of the same models, 30k 1.3 GB, 53k 3.1, 57k 3.6,
// 81k 7.8, 110k 11.0, 142k 18.8, 179k 23.1. Two models of equal size can differ
// by a factor of two in fill-in (104839 and 112246 above), which is why the
// curve is an envelope rather than a fit through the middle.
double estimatedDirectMemoryGb(int unknown_count)
{
    constexpr double reference_unknowns = 30000.0;
    constexpr double reference_gigabytes = 1.3;
    constexpr double growth_exponent = 1.79;
    const double unknowns = std::max(1.0, static_cast<double>(unknown_count));
    return reference_gigabytes *
           std::pow(unknowns / reference_unknowns, growth_exponent);
}

// Wall time of the same factorisation. Good for one significant figure and no
// more: measured against estimate it came out 13.5 s (13), 282 s (179), 397 s
// (639) and 926 s (758) on the four models above, i.e. wrong by up to a factor
// of 1.6 in both directions. It exists to tell the user roughly how long the
// silence will last, not to promise anything.
double estimatedDirectSeconds(int unknown_count)
{
    constexpr double reference_unknowns = 30000.0;
    constexpr double reference_seconds = 28.0;
    constexpr double growth_exponent = 2.5;
    const double unknowns = std::max(1.0, static_cast<double>(unknown_count));
    return reference_seconds *
           std::pow(unknowns / reference_unknowns, growth_exponent);
}

// Free physical memory: what decides whether the factorisation fits, as opposed
// to what is installed. 0 means the platform could not be asked.
double availablePhysicalMemoryGb()
{
#ifdef _WIN32
    MEMORYSTATUSEX status;
    status.dwLength = sizeof(status);
    if (GlobalMemoryStatusEx(&status)) {
        return static_cast<double>(status.ullAvailPhys) /
               (1024.0 * 1024.0 * 1024.0);
    }
#endif
    return 0.0;
}

struct MeshSizeSpread
{
    double smallest_m = 0.0;
    double largest_m = 0.0;
    double contrast = 1.0;
    // Extremes, reported alongside the percentiles so that a mesh whose worst
    // cells are far outside the working range is visible rather than averaged
    // away.
    double minimum_m = 0.0;
    double maximum_m = 0.0;
};

// The cell size contrast of the mesh. Local refinement is what creates it, and
// it - not the unknown count - is what decides whether the Gauss-Seidel
// preconditioned GMRES can solve the system at all. Measured on the iris model
// of this project (22.66 x 9.96 x 50 mm, 0.5 mm plate, round window, stub):
// contrast 6.8:1 converges in 5140 iterations to 2.2e-6 in 124 s and lands
// within 7 digits of the factorised answer, 8:1 was measured to need 7087
// iterations for 4.3e-6, and 20.7:1 does not converge at all (8000 iterations,
// residual stuck at 2.0e-3 after 615 s of wasted time).
// Taken between percentiles and not between the extremes: every boolean cut
// leaves gmsh a few sliver tetrahedra, and a handful of slivers must not decide
// which solver runs.
MeshSizeSpread measureMeshSizeSpread(mfem::Mesh &mesh)
{
    MeshSizeSpread spread;
    const int element_count = mesh.GetNE();
    if (element_count <= 0) {
        return spread;
    }
    std::vector<double> element_sizes_m;
    element_sizes_m.reserve(static_cast<std::size_t>(element_count));
    for (int element = 0; element < element_count; ++element) {
        element_sizes_m.push_back(mesh.GetElementSize(element, 0));
    }
    std::sort(element_sizes_m.begin(), element_sizes_m.end());
    const auto percentile = [&element_sizes_m, element_count](double fraction) {
        const double position = fraction * (element_count - 1);
        const std::size_t index = static_cast<std::size_t>(
            std::clamp(position, 0.0, static_cast<double>(element_count - 1)));
        return element_sizes_m[index];
    };
    spread.smallest_m = percentile(0.01);
    spread.largest_m = percentile(0.99);
    spread.minimum_m = element_sizes_m.front();
    spread.maximum_m = element_sizes_m.back();
    if (spread.smallest_m > 0.0) {
        spread.contrast = spread.largest_m / spread.smallest_m;
    }
    return spread;
}

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

#ifdef KRUTIEV_WITH_EIGEN
// Factorises the real 2N x 2N block form of the complex system and solves it.
// Returns an empty string on success, otherwise the reason the caller has to
// fall back on the Krylov solver.
std::string solveWithSparseLu(const mfem::ComplexSparseMatrix &complex_matrix,
                              const mfem::Vector &right_vector,
                              mfem::Vector &solution_vector)
{
    try {
        Eigen::SparseMatrix<double, Eigen::ColMajor, int> matrix;
        int block_size = 0;
        {
            // GetSystemMatrix() allocates a fresh CSR triple and hands ownership
            // to the returned SparseMatrix, which owns and frees all three
            // arrays. It is released as soon as Eigen holds its own copy: the
            // factorization that follows needs every byte it can get.
            const std::unique_ptr<mfem::SparseMatrix> block_matrix(
                complex_matrix.GetSystemMatrix());
            if (!block_matrix || !block_matrix->Finalized()) {
                return "the complex system did not produce a finalized block matrix";
            }
            // The block rows are assembled real part first, imaginary part
            // second, which is already ascending, but the flag on the freshly
            // wrapped CSR says otherwise and Eigen's mapped view trusts the
            // stored order.
            block_matrix->SortColumnIndices();

            block_size = block_matrix->Height();
            if (block_size != right_vector.Size() ||
                block_size != solution_vector.Size() ||
                block_size != block_matrix->Width()) {
                return "block matrix and right-hand side sizes disagree";
            }
            const int nonzero_count = block_matrix->GetI()[block_size];
            const Eigen::Map<const Eigen::SparseMatrix<double, Eigen::RowMajor, int>>
                mapped_rows(block_size,
                            block_size,
                            nonzero_count,
                            block_matrix->GetI(),
                            block_matrix->GetJ(),
                            block_matrix->GetData());
            matrix = mapped_rows;
        }

        Eigen::SparseLU<Eigen::SparseMatrix<double, Eigen::ColMajor, int>,
                        Eigen::COLAMDOrdering<int>>
            factorization;
        // No info() check between these two calls. Eigen's SparseLU leaves
        // m_info uninitialised in its constructor and analyzePattern() never
        // writes it - every assignment to m_info is inside factorize(). Reading
        // it here returned stack garbage in Release and rejected the direct
        // solver on every real model with an empty reason string ("sparse LU
        // symbolic analysis failed: "), 3 runs out of 3 on 4 models between
        // 63033 and 106439 unknowns, while small test models happened to pass.
        // In Debug the same read tripped Eigen's m_isInitialized assert.
        factorization.analyzePattern(matrix);
        factorization.factorize(matrix);
        if (factorization.info() != Eigen::Success) {
            return "sparse LU factorization failed: " + factorization.lastErrorMessage();
        }
        const Eigen::Map<const Eigen::VectorXd> right_hand_side(right_vector.HostRead(),
                                                                block_size);
        const Eigen::VectorXd result = factorization.solve(right_hand_side);
        // info() says nothing here either - SparseLU::_solve_impl does not touch
        // m_info - so the back substitution is checked on its output instead.
        // The caller then measures the true residual, which is the real verdict.
        if (!result.allFinite()) {
            return "sparse LU back substitution produced a non-finite solution";
        }
        std::copy(result.data(), result.data() + block_size, solution_vector.HostWrite());
    } catch (const std::bad_alloc &) {
        return "sparse LU ran out of memory";
    } catch (const std::exception &exception) {
        // Not a reliable memory guard on its own: with allocation denied hard
        // enough, Eigen's factorization faults instead of throwing (measured
        // under a 500 MB process commit cap on a 23325-unknown system). The
        // memory budget in FemSolverSettings is what actually keeps us out of it.
        return std::string("sparse LU failed: ") + exception.what();
    }
    return {};
}
#endif

Complex projectPortElectric(const IFieldEvaluator &field,
                            const IFieldEvaluator &mode,
                            const WaveguideGeometry &geometry,
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
            // У круглого тракта сетка выборки строится по описанному квадрату,
            // поэтому углы лежат вне полости: там поля нет, и в проекцию моды
            // они внесли бы только шум.
            if (!insideCrossSection(geometry, x_m, y_m)) {
                continue;
            }
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

    // Gmsh runs as a blocking child process, so this is the first moment after
    // it that a cancellation can be honoured at all.
    if (control.isCancellationRequested()) {
        solution.cancelled = true;
        solution.error_message = "Calculation cancelled.";
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
    const MeshSizeSpread mesh_spread = measureMeshSizeSpread(*state->mesh);
    {
        std::ostringstream stage;
        stage << "FEM mesh ready: " << solution.diagnostics.mesh_tetrahedron_count
              << " tetrahedra, " << solution.diagnostics.fem_unknown_count
              << " unknowns, cells " << std::fixed << std::setprecision(3)
              << mesh_spread.smallest_m * 1.0e3 << " to "
              << mesh_spread.largest_m * 1.0e3 << " mm (extremes "
              << mesh_spread.minimum_m * 1.0e3 << " / "
              << mesh_spread.maximum_m * 1.0e3 << "), size contrast "
              << std::setprecision(1) << mesh_spread.contrast << ":1.";
        control.reportProgress(stage.str());
    }

    // Which solver can actually deliver an answer here. The direct path is
    // bounded by memory; the iterative path is bounded by the cell size
    // contrast, which local refinement raises together with the unknown count -
    // so a threshold on unknowns, as used before, sent exactly the refined
    // models to the solver that cannot solve them.
    const int unknown_count = solution.diagnostics.fem_unknown_count;
    const double estimated_memory_gb = estimatedDirectMemoryGb(unknown_count);
    const double estimated_direct_seconds = estimatedDirectSeconds(unknown_count);
    double memory_budget_gb = request.settings.fem.direct_solver_memory_budget_gb;
    if (!(memory_budget_gb > 0.0)) {
        const double available_gb = availablePhysicalMemoryGb();
        // The remaining fifth is for everything else on the machine; the
        // estimate above is already a peak for the whole process. A platform
        // that cannot be asked is assumed small - 4 GB is the 64k-unknown point
        // of the measured table, so the assumption still allows real models.
        constexpr double unknown_platform_budget_gb = 4.0;
        memory_budget_gb = available_gb > 0.0 ? 0.8 * available_gb
                                              : unknown_platform_budget_gb;
    }
    // 8:1 is the highest contrast measured to converge, and only just: 7087
    // iterations against the 8000 the first attempt and the retry allow
    // together. 20.7:1 does not converge at any iteration count tried.
    constexpr double iterative_contrast_limit = 8.0;
    const bool direct_fits_memory = estimated_memory_gb <= memory_budget_gb;
    const bool iterative_can_converge = mesh_spread.contrast <= iterative_contrast_limit;

    const LinearSolverMethod requested_method = request.settings.fem.linear_solver_method;
    bool plan_is_direct = requested_method == LinearSolverMethod::Direct;
#ifdef KRUTIEV_WITH_EIGEN
    if (requested_method == LinearSolverMethod::Automatic) {
        plan_is_direct = direct_fits_memory || !iterative_can_converge;
    }
#else
    plan_is_direct = false;
#endif
    if (requested_method == LinearSolverMethod::Automatic && !direct_fits_memory &&
        !iterative_can_converge) {
        // Neither path can finish this model, and saying so now costs seconds
        // instead of the quarter of an hour the iterative solver used to spend
        // before admitting the same thing.
        std::ostringstream error;
        error << std::fixed << std::setprecision(1)
              << "No linear solver can handle this model: the mesh cell size "
                 "contrast is " << mesh_spread.contrast
              << ":1, above the " << std::setprecision(0) << iterative_contrast_limit
              << ":1 the iterative solver still converges at, and factorizing "
              << unknown_count << " unknowns directly needs about "
              << std::setprecision(1) << estimated_memory_gb << " GB against "
              << memory_budget_gb
              << " GB available. Lower the accuracy level: a coarser mesh, "
                 "fewer elements across the smallest feature, a smaller minimum "
                 "size ratio or first-order elements all reduce both numbers.";
        solution.error_message = error.str();
        return solution;
    }
    {
        std::ostringstream stage;
        stage << "Linear solver: " << (plan_is_direct ? "sparse LU" : "GMRES")
              << " (direct needs about " << std::fixed << std::setprecision(1)
              << estimated_memory_gb << " GB of " << memory_budget_gb
              << " GB available and about " << std::setprecision(0)
              << estimated_direct_seconds << " s; iterative needs a cell size "
                 "contrast of at most " << std::setprecision(0)
              << iterative_contrast_limit << ":1, this mesh has "
              << std::setprecision(1) << mesh_spread.contrast << ":1).";
        control.reportProgress(stage.str());
    }
    if (plan_is_direct && !direct_fits_memory) {
        std::ostringstream memory_warning;
        memory_warning << std::fixed << std::setprecision(1)
                       << "Factorizing " << unknown_count
                       << " unknowns is estimated to need " << estimated_memory_gb
                       << " GB against " << memory_budget_gb
                       << " GB available; the machine may start swapping.";
        solution.diagnostics.warnings.push_back(memory_warning.str());
        control.reportProgress(memory_warning.str());
    }
    if (!plan_is_direct && !iterative_can_converge) {
        std::ostringstream contrast_warning;
        contrast_warning << std::fixed << std::setprecision(1)
                         << "Mesh cell size contrast " << mesh_spread.contrast
                         << ":1 is above the " << std::setprecision(0)
                         << iterative_contrast_limit
                         << ":1 at which the preconditioned GMRES was last "
                            "measured to converge; it may stall.";
        solution.diagnostics.warnings.push_back(contrast_warning.str());
        control.reportProgress(contrast_warning.str());
    }

    const auto &waveguide = request.model.waveguide;
    const Complex material_product = request.model.filling_material.relative_permittivity *
                                     request.model.filling_material.relative_permeability;
    const double bulk_wavenumber = state->omega / speed_of_light_m_per_s *
                                   std::sqrt(std::max(0.0, std::real(material_product)));
    // Опорная мода порта: тот же пустой тракт, решённый замкнутыми формулами.
    // Отсечка берётся из неё, а не считается здесь по прямоугольной формуле:
    // формула m*pi/a, n*pi/b неверна для круглого сечения (там нули функций
    // Бесселя) и не знает, какую именно моду выбрал пользователь.
    SimulationRequest incident_request = request;
    incident_request.model.slot_geometries.clear();
    incident_request.model.pec_plates.clear();
    incident_request.model.dielectric_blocks.clear();
    incident_request.model.shapes.clear();
    const FieldSolution incident = AnalyticWaveguideSolver().solve(incident_request);
    if (!incident.success || !incident.field || !incident.has_selected_mode) {
        solution.error_message = "Cannot construct the normalized incident port mode: " +
                                 incident.error_message;
        return solution;
    }
    solution.available_modes = incident.available_modes;
    solution.has_selected_mode = incident.has_selected_mode;
    solution.selected_mode = incident.selected_mode;

    const double cutoff_wavenumber = incident.selected_mode.cutoff_wavenumber_per_m;
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
    if (control.isCancellationRequested()) {
        solution.cancelled = true;
        solution.error_message = "Calculation cancelled.";
        return solution;
    }
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

    // Computed the same way for both solvers, from the operator itself rather
    // than from anything a solver reports: for the direct path it is also the
    // proof that the real 2N x 2N block layout matches the complex operator.
    const auto explicitRelativeResidual = [&]() {
        mfem::Vector residual(right_vector.Size());
        system_operator->Mult(solution_vector, residual);
        residual -= right_vector;
        return residual.Norml2() / right_norm;
    };

    if (control.isCancellationRequested()) {
        solution.cancelled = true;
        solution.error_message = "Calculation cancelled.";
        return solution;
    }

    bool solved_directly = false;
    int total_iterations = 0;
    double relative_residual = 0.0;
#ifdef KRUTIEV_WITH_EIGEN
    if (plan_is_direct) {
        mfem::ComplexSparseMatrix *complex_matrix =
            system_operator.Is<mfem::ComplexSparseMatrix>();
        std::string failure;
        bool cancelled_while_factorizing = false;
        if (complex_matrix == nullptr) {
            failure = "the assembled system is not a serial complex sparse matrix";
        } else {
            std::ostringstream stage;
            stage << "Sparse LU: factorizing " << unknown_count
                  << " unknowns, expect roughly " << std::fixed << std::setprecision(0)
                  << estimated_direct_seconds << " s and " << std::setprecision(1)
                  << estimated_memory_gb << " GB...";
            control.reportProgress(stage.str());
            // Eigen's factorisation is one blocking call with no callback, so it
            // cannot be interrupted. Running it on its own thread at least keeps
            // this one free to show that the program is alive and to notice a
            // cancellation; the cancellation is acted on only once the call
            // returns, because killing the thread would leak the factors and
            // leave Eigen's allocator in an undefined state.
            std::future<std::string> factorization =
                std::async(std::launch::async, [&]() {
                    return solveWithSparseLu(*complex_matrix, right_vector,
                                             solution_vector);
                });
            constexpr auto heartbeat_period = std::chrono::seconds(10);
            const auto factorization_start = std::chrono::steady_clock::now();
            while (factorization.wait_for(heartbeat_period) !=
                   std::future_status::ready) {
                const double elapsed_s = std::chrono::duration<double>(
                                             std::chrono::steady_clock::now() -
                                             factorization_start)
                                             .count();
                cancelled_while_factorizing = cancelled_while_factorizing ||
                                              control.isCancellationRequested();
                std::ostringstream beat;
                beat << "Sparse LU: factorizing, " << std::fixed << std::setprecision(0)
                     << elapsed_s << " s of roughly " << estimated_direct_seconds
                     << " s elapsed"
                     << (cancelled_while_factorizing
                             ? " (cancelled, but the factorization cannot be "
                               "interrupted and has to finish)"
                             : "")
                     << ".";
                control.reportProgress(beat.str());
            }
            failure = factorization.get();
        }
        if (cancelled_while_factorizing || control.isCancellationRequested()) {
            solution.cancelled = true;
            solution.error_message = "Calculation cancelled.";
            return solution;
        }
        if (failure.empty()) {
            relative_residual = explicitRelativeResidual();
            // The direct path used to have no acceptance test at all: whatever
            // came out was reported as success, and with the solution vector
            // deliberately doubled inside the solver it still returned success
            // and a full set of S-parameters at a relative residual of 1.00.
            // A factorisation that worked leaves 1e-13 to 1e-14 on these systems
            // (measured 1.2e-14 to 2.6e-13 over 1453 to 112246 unknowns), so the
            // bar can sit five orders above that and still be eight orders below
            // a broken one. With the same corruption in place the check now
            // rejects the factorisation and GMRES solves the system instead.
            constexpr double direct_residual_limit = 1.0e-8;
            if (relative_residual <= direct_residual_limit) {
                solved_directly = true;
            } else {
                std::ostringstream residual_failure;
                residual_failure << std::scientific << std::setprecision(3)
                                 << "the factorization left a relative residual of "
                                 << relative_residual << ", above the "
                                 << direct_residual_limit << " a working one leaves";
                failure = residual_failure.str();
                relative_residual = 0.0;
            }
        }
        if (!failure.empty()) {
            // A failed factorization leaves the vector half overwritten, so the
            // Krylov solver must not inherit it as an initial guess.
            solution_vector = 0.0;
            std::ostringstream fallback_warning;
            fallback_warning << "Direct solver unavailable (" << failure
                             << "); solved with preconditioned GMRES instead.";
            solution.diagnostics.warnings.push_back(fallback_warning.str());
            // Also on the live channel: the Krylov fallback can run for minutes
            // and the user should know why before it finishes.
            control.reportProgress(fallback_warning.str());
        }
    }
#else
    if (requested_method == LinearSolverMethod::Direct) {
        solution.diagnostics.warnings.push_back(
            "Direct solver requested but this build has no Eigen; solved with "
            "preconditioned GMRES instead.");
    }
#endif

    if (!solved_directly) {
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
        // Three Gauss-Seidel sweeps per application instead of the default one. The
        // extra sweeps cost little next to a GMRES iteration on this indefinite
        // system but shorten the Krylov space a lot: on the empty 22.86x10.16 mm
        // guide at 10 GHz with a 3 mm mesh, GMRES needs 870 iterations with one
        // sweep and 194 with three (7.1 s vs 2.6 s); with the 11x5 mm iris it is
        // 1357 versus 238 iterations (12.5 s vs 3.3 s).
        constexpr int smoother_sweep_count = 3;
        mfem::GSSmoother real_smoother(*preconditioner_operator.As<mfem::SparseMatrix>(),
                                       0,
                                       smoother_sweep_count);
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
        if (solve_monitor.wasCancelled()) {
            solution.cancelled = true;
            solution.error_message = "Calculation cancelled.";
            return solution;
        }
        total_iterations = solver.GetNumIterations();
        relative_residual = explicitRelativeResidual();
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
            if (retry_monitor.wasCancelled()) {
                solution.cancelled = true;
                solution.error_message = "Calculation cancelled.";
                return solution;
            }
            total_iterations += retry_solver.GetNumIterations();
            relative_residual = explicitRelativeResidual();
            if (!(relative_residual <= usable_relative_residual)) {
                solution.diagnostics.linear_iterations = total_iterations;
                solution.diagnostics.linear_relative_residual = relative_residual;
                std::ostringstream error;
                error << "MFEM GMRES did not converge: true relative residual="
                      << relative_residual << ", iterations=" << total_iterations << ".";
                solution.error_message = error.str();
                return solution;
            }
            if (relative_residual > relaxed_tolerance) {
                std::ostringstream stall_warning;
                stall_warning << "GMRES stalled at true relative residual "
                              << relative_residual << " (the requested "
                              << request.settings.fem.relative_tolerance
                              << " applies to the preconditioned one); fields and "
                                 "S-parameters are approximate.";
                solution.diagnostics.warnings.push_back(stall_warning.str());
            }
        }
    }
    // linear_iterations stays 0 for the direct solver, so the method itself has
    // to be named somewhere the user can see it.
    solution.diagnostics.backend_name = solved_directly
                                            ? "MFEM complex H(curl) FEM, Eigen SparseLU"
                                            : "MFEM complex H(curl) FEM, preconditioned GMRES";
    solution.diagnostics.linear_iterations = total_iterations;
    solution.diagnostics.linear_relative_residual = relative_residual;
    form.RecoverFEMSolution(solution_vector, right_hand_side, *state->electric);

    solution.field = std::make_shared<MfemFieldEvaluator>(state);
    control.reportProgress("Projecting S-parameters at the ports...");
    const double sampling_offset = std::max(1.0e-8, waveguide.length_m * 1.0e-6);
    const double input_z = -0.5 * waveguide.length_m + sampling_offset;
    const double output_z = 0.5 * waveguide.length_m - sampling_offset;
    // Both S-parameters are referenced to the physical port planes, exactly as
    // in the analytic and mode-matching backends: only the small sampling
    // offset is de-embedded, never the guide length itself. De-embedding the
    // whole length here made an empty guide report S21 = 1 instead of
    // exp(-gamma L), so the same structure changed phase with the solver.
    const double input_distance = sampling_offset;
    const double output_distance = sampling_offset;
    const Complex input_projection =
        projectPortElectric(*solution.field, *incident.field, waveguide, input_z);
    const Complex output_projection =
        projectPortElectric(*solution.field, *incident.field, waveguide, output_z);
    const Complex incident_at_input = std::exp(Complex(0.0, -beta * input_distance));
    solution.scattering.s11 =
        (input_projection - incident_at_input) *
        std::exp(Complex(0.0, -beta * input_distance));
    solution.scattering.s21 =
        output_projection * std::exp(Complex(0.0, -beta * output_distance));

    // The powers below are all derived from the two projected S-parameters, so
    // their sum balances by construction and proves nothing. The single piece of
    // independent information is the unitarity residual 1 - |S11|^2 - |S21|^2:
    // the port projection never enforces it, so for a model without a physical
    // power sink it measures the numerical error of the whole solve.
    const double incident_power_w = request.settings.normalization_power_w;
    const double reflected_fraction = std::norm(solution.scattering.s11);
    const double transmitted_fraction = std::norm(solution.scattering.s21);
    const double unitarity_residual = 1.0 - reflected_fraction - transmitted_fraction;
    solution.diagnostics.incident_power_w = incident_power_w;
    solution.diagnostics.reflected_power_w = reflected_fraction * incident_power_w;
    solution.diagnostics.transmitted_power_w = transmitted_fraction * incident_power_w;
    // Deliberately not clamped at zero: a negative remainder means the structure
    // came out active, which is exactly the failure a clamp would hide.
    solution.diagnostics.dissipated_power_w = unitarity_residual * incident_power_w;
    solution.diagnostics.unitarity_defect = std::abs(unitarity_residual);
    solution.diagnostics.power_balance_relative_error =
        solution.diagnostics.unitarity_defect;

    const bool has_lossy_material =
        std::any_of(state->materials.begin(),
                    state->materials.end(),
                    [](const Material &material) {
                        return material.conductivity_s_per_m > 0.0 ||
                               std::imag(material.relative_permittivity) != 0.0 ||
                               std::imag(material.relative_permeability) != 0.0;
                    });
    // Wall loss is not modelled here, so a PML that carries slot radiation out of
    // the domain is the only other place the missing power can legitimately go.
    const bool has_power_sink = has_lossy_material || pml.enabled;
    // Below this the residual is dominated by the port projection and mesh
    // discretisation error, which the S-parameters themselves already carry.
    constexpr double unitarity_warning_threshold = 0.02;
    // Floor for calling the residual negative. It has to sit at the accuracy the
    // whole chain can reach, not at rounding: the port projection samples the
    // mode on a 36 x 24 grid and the mesh carries its own error. Measured on the
    // empty guide with second-order elements and the direct solver (linear
    // residual 1e-13, so nothing else is in the way), the defect over
    // h = 5.0 / 4.0 / 3.2 / 2.6 mm is 1.2e-04 / 6.2e-05 / 1.6e-07 / 6.8e-05: it
    // stops falling with the mesh and its sign turns random, so the finest of
    // the four came out at |S21| = 1.0000338 > 1 - and the old floor of 1e-9
    // reported "passivity violated" on the most accurate mesh of the set. With
    // first-order elements the same defect stays between 1.1e-02 and 2.9e-02,
    // a hundred times above this floor, so a real violation still fires.
    const double passivity_floor_w = 1.0e-4 * incident_power_w;
    if (solution.diagnostics.dissipated_power_w < -passivity_floor_w) {
        std::ostringstream passivity_warning;
        passivity_warning << "Passivity violated numerically: |S11|^2 + |S21|^2 = "
                          << reflected_fraction + transmitted_fraction
                          << " exceeds 1 by " << -unitarity_residual
                          << "; the reported dissipated power is negative.";
        solution.diagnostics.warnings.push_back(passivity_warning.str());
    }
    if (has_power_sink) {
        std::ostringstream sink_note;
        sink_note << "Power balance residual " << unitarity_residual
                  << " also absorbs physical loss ("
                  << (has_lossy_material ? "lossy filling" : "PML radiation")
                  << "), so it is not a pure numerical error measure.";
        solution.diagnostics.warnings.push_back(sink_note.str());
    } else if (solution.diagnostics.unitarity_defect > unitarity_warning_threshold) {
        std::ostringstream defect_warning;
        defect_warning << "Unitarity defect " << solution.diagnostics.unitarity_defect
                       << " on a lossless model: the S-parameters carry at least that "
                          "much numerical error; refine the mesh or tighten the "
                          "solver tolerance.";
        solution.diagnostics.warnings.push_back(defect_warning.str());
    }
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
