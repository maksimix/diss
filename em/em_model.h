#pragma once

#include "em_math.h"

#include <algorithm>
#include <string>
#include <vector>

namespace em
{
enum class ModeFamily
{
    TransverseElectric,
    TransverseMagnetic
};

enum class WallSurface
{
    Top,
    Right,
    Bottom,
    Left
};

struct Material
{
    std::string name = "Vacuum";
    Complex relative_permittivity = 1.0;
    Complex relative_permeability = 1.0;
    double conductivity_s_per_m = 0.0;
};

enum class WaveguideCrossSection
{
    Rectangular,
    Circular
};

struct WaveguideGeometry
{
    WaveguideCrossSection cross_section = WaveguideCrossSection::Rectangular;
    // Прямоугольное сечение.
    double inner_width_m = 0.0;
    double inner_height_m = 0.0;
    // Круглое сечение.
    double inner_radius_m = 0.0;
    double length_m = 0.0;
    double wall_thickness_m = 0.0;
    // Finite wall conductivity for the perturbation conductor-loss model.
    // 0 (or negative) means a perfect electric conductor: lossless walls.
    double wall_conductivity_s_per_m = 0.0;
};

inline bool isCircular(const WaveguideGeometry &geometry)
{
    return geometry.cross_section == WaveguideCrossSection::Circular;
}

// Полуразмеры описанного прямоугольника сечения. Для круглого волновода это
// радиус по обеим осям, поэтому сетки и рамки, построенные по этим границам,
// остаются корректными — точки вне сечения отсекает insideCrossSection().
inline double crossSectionHalfWidth(const WaveguideGeometry &geometry)
{
    return isCircular(geometry) ? geometry.inner_radius_m : 0.5 * geometry.inner_width_m;
}

inline double crossSectionHalfHeight(const WaveguideGeometry &geometry)
{
    return isCircular(geometry) ? geometry.inner_radius_m : 0.5 * geometry.inner_height_m;
}

inline double crossSectionArea(const WaveguideGeometry &geometry)
{
    return isCircular(geometry)
               ? pi * geometry.inner_radius_m * geometry.inner_radius_m
               : geometry.inner_width_m * geometry.inner_height_m;
}

inline bool insideCrossSection(const WaveguideGeometry &geometry,
                               double x_m,
                               double y_m,
                               double tolerance_m = 0.0)
{
    if (isCircular(geometry)) {
        const double radius_m = geometry.inner_radius_m + tolerance_m;
        return x_m * x_m + y_m * y_m <= radius_m * radius_m;
    }
    return std::abs(x_m) <= 0.5 * geometry.inner_width_m + tolerance_m &&
           std::abs(y_m) <= 0.5 * geometry.inner_height_m + tolerance_m;
}

// Периметр стенки, по которому берётся контурный интеграл потерь.
inline double crossSectionPerimeter(const WaveguideGeometry &geometry)
{
    return isCircular(geometry)
               ? 2.0 * pi * geometry.inner_radius_m
               : 2.0 * (geometry.inner_width_m + geometry.inner_height_m);
}

struct SlotGeometry
{
    bool enabled = false;
    WallSurface wall = WallSurface::Top;
    double center_u_m = 0.0;
    double center_z_m = 0.0;
    double length_m = 0.0;
    double width_m = 0.0;
    double rotation_rad = 0.0;
};

enum class PlateApertureShape
{
    Rectangular,
    Circular
};

struct PecPlateGeometry
{
    bool enabled = false;
    Vec3 center_m;
    Vec3 size_m;
    Vec3 rotation_rad;
    // Optional window cut straight through the plate along its local z axis.
    // A plate that spans the whole cross-section with a window is an iris
    // (diaphragm), not a short circuit.
    bool aperture_enabled = false;
    PlateApertureShape aperture_shape = PlateApertureShape::Rectangular;
    double aperture_width_m = 0.0;      // rectangular window, along local x
    double aperture_height_m = 0.0;     // rectangular window, along local y
    double aperture_radius_m = 0.0;     // circular window
    double aperture_offset_x_m = 0.0;   // window centre, from the plate centre
    double aperture_offset_y_m = 0.0;
    // Optional rectangular stub lying in the plane of the plate: it rises from
    // the plate's bottom edge along the aperture centre line and reaches into
    // the window. Its thickness along the plate normal equals the plate
    // thickness, so it is part of the plate metal, not a free-standing body.
    bool post_enabled = false;
    double post_width_m = 0.0;    // along the plate local x
    double post_height_m = 0.0;   // upwards from the plate bottom edge
};

// True when the plate actually has an opening through it.
inline bool plateHasOpening(const PecPlateGeometry &plate)
{
    if (!plate.aperture_enabled) {
        return false;
    }
    return plate.aperture_shape == PlateApertureShape::Circular
               ? plate.aperture_radius_m > 0.0
               : plate.aperture_width_m > 0.0 && plate.aperture_height_m > 0.0;
}

inline bool plateHasPost(const PecPlateGeometry &plate)
{
    return plate.post_enabled && plate.post_width_m > 0.0 && plate.post_height_m > 0.0;
}

// True for a point (in plate-local x/y, relative to the plate centre) that lies
// on the stub, i.e. on metal that fills part of the window.
inline bool insidePlateStub(const PecPlateGeometry &plate,
                            double local_x_m,
                            double local_y_m)
{
    if (!plateHasPost(plate)) {
        return false;
    }
    const double bottom_m = -0.5 * plate.size_m.y;
    return std::abs(local_x_m - plate.aperture_offset_x_m) <= 0.5 * plate.post_width_m &&
           local_y_m >= bottom_m && local_y_m <= bottom_m + plate.post_height_m;
}

// ------------------------------------------------------------- формы -------
// Свободная геометрия, которую пользователь строит сам, как в CST: список
// примитивов, каждый со своей булевой операцией над накопленным металлом.
// Порядок в списке — это история построения: тела применяются одно за другим,
// поэтому «брусок, затем вычесть цилиндр» и «цилиндр, затем вычесть брусок»
// дают разные модели. Из этого набора собираются и перегородки, и штыри, и
// диафрагмы с окном произвольного очертания — то, чего фиксированная
// PecPlateGeometry описать не может.
enum class ShapeKind
{
    Brick,      // параллелепипед
    Cylinder,   // цилиндр вдоль выбранной оси (диск, штырь, круглая вставка)
    Prism       // призма: замкнутый профиль, вытянутый вдоль выбранной оси
};

enum class ShapeBoolean
{
    Add,        // объединить с металлом
    Subtract,   // вычесть из металла (окно, паз, отверстие)
    Intersect   // оставить общую часть
};

// Ось цилиндра или направление вытягивания призмы. Профиль призмы лежит в
// плоскости, перпендикулярной этой оси, в её собственных координатах: для оси Z
// это (x, y), для оси Y — (z, x), для оси X — (y, z). Такой порядок сохраняет
// правую тройку, поэтому положительный обход профиля остаётся положительным.
enum class ShapeAxis
{
    X,
    Y,
    Z
};

struct ShapeGeometry
{
    std::string name = "shape";
    bool enabled = true;
    ShapeKind kind = ShapeKind::Brick;
    ShapeBoolean operation = ShapeBoolean::Add;
    Vec3 center_m;
    Vec3 rotation_rad;
    Vec3 size_m;               // Brick: полные размеры по осям
    double radius_m = 0.0;     // Cylinder
    double length_m = 0.0;     // Cylinder и Prism: длина вдоль оси
    ShapeAxis axis = ShapeAxis::Z;
    // Prism: замкнутый профиль в плоскости, перпендикулярной оси, относительно
    // центра тела. Последняя точка соединяется с первой автоматически.
    std::vector<Vec2> profile_m;
};

// Тело, которое действительно занимает объём: вырожденное описание (нулевой
// радиус, профиль из двух точек) не даёт ни металла, ни отверстия, и его нельзя
// отправлять в сеточный генератор — булева операция с пустым телом обрывает
// весь скрипт.
inline bool shapeIsSolid(const ShapeGeometry &shape)
{
    switch (shape.kind) {
    case ShapeKind::Brick:
        return shape.size_m.x > 0.0 && shape.size_m.y > 0.0 && shape.size_m.z > 0.0;
    case ShapeKind::Cylinder:
        return shape.radius_m > 0.0 && shape.length_m > 0.0;
    case ShapeKind::Prism:
        return shape.length_m > 0.0 && shape.profile_m.size() >= 3;
    }
    return false;
}

inline bool hasEnabledShapes(const std::vector<ShapeGeometry> &shapes)
{
    for (const ShapeGeometry &shape : shapes) {
        if (shape.enabled && shapeIsSolid(shape)) {
            return true;
        }
    }
    return false;
}

// Габаритный ящик тела в мировых осях — по нему строится локальное измельчение
// сетки и оценка объёма металла. Поворот учитывается грубо, через половину
// диагонали: точный ящик повёрнутой призмы стоил бы разбора профиля, а нужен он
// только для того, чтобы очертить область измельчения с запасом.
inline void shapeBoundingBox(const ShapeGeometry &shape, Vec3 *minimum_m, Vec3 *maximum_m)
{
    double half_x = 0.0;
    double half_y = 0.0;
    double half_z = 0.0;
    const auto axis_half = [&shape](double transverse_half, double axial_half) {
        switch (shape.axis) {
        case ShapeAxis::X:
            return Vec3{axial_half, transverse_half, transverse_half};
        case ShapeAxis::Y:
            return Vec3{transverse_half, axial_half, transverse_half};
        case ShapeAxis::Z:
        default:
            return Vec3{transverse_half, transverse_half, axial_half};
        }
    };
    switch (shape.kind) {
    case ShapeKind::Brick: {
        half_x = 0.5 * shape.size_m.x;
        half_y = 0.5 * shape.size_m.y;
        half_z = 0.5 * shape.size_m.z;
        break;
    }
    case ShapeKind::Cylinder: {
        const Vec3 half = axis_half(shape.radius_m, 0.5 * shape.length_m);
        half_x = half.x;
        half_y = half.y;
        half_z = half.z;
        break;
    }
    case ShapeKind::Prism: {
        double reach = 0.0;
        for (const Vec2 &point : shape.profile_m) {
            reach = std::max(reach, std::max(std::abs(point.x), std::abs(point.y)));
        }
        const Vec3 half = axis_half(reach, 0.5 * shape.length_m);
        half_x = half.x;
        half_y = half.y;
        half_z = half.z;
        break;
    }
    }
    const double rotation_reach =
        std::sqrt(half_x * half_x + half_y * half_y + half_z * half_z) *
        (std::abs(shape.rotation_rad.x) + std::abs(shape.rotation_rad.y) +
         std::abs(shape.rotation_rad.z));
    half_x += rotation_reach;
    half_y += rotation_reach;
    half_z += rotation_reach;
    if (minimum_m != nullptr) {
        *minimum_m = {shape.center_m.x - half_x,
                      shape.center_m.y - half_y,
                      shape.center_m.z - half_z};
    }
    if (maximum_m != nullptr) {
        *maximum_m = {shape.center_m.x + half_x,
                      shape.center_m.y + half_y,
                      shape.center_m.z + half_z};
    }
}

// Наименьший характерный размер тела: по нему сеточный генератор решает, каким
// элементом его разрешать. Для призмы это длина самой короткой стороны профиля,
// потому что именно она, а не габарит, определяет тонкие перемычки.
inline double shapeSmallestFeature(const ShapeGeometry &shape)
{
    switch (shape.kind) {
    case ShapeKind::Brick:
        return std::min({shape.size_m.x, shape.size_m.y, shape.size_m.z});
    case ShapeKind::Cylinder:
        return std::min(2.0 * shape.radius_m, shape.length_m);
    case ShapeKind::Prism: {
        double smallest = shape.length_m;
        const std::size_t count = shape.profile_m.size();
        for (std::size_t index = 0; index < count; ++index) {
            const Vec2 &from = shape.profile_m[index];
            const Vec2 &to = shape.profile_m[(index + 1) % count];
            const double edge = std::sqrt((to.x - from.x) * (to.x - from.x) +
                                          (to.y - from.y) * (to.y - from.y));
            if (edge > 0.0) {
                smallest = std::min(smallest, edge);
            }
        }
        return smallest;
    }
    }
    return 0.0;
}

struct DielectricBlockGeometry
{
    bool enabled = false;
    Vec3 center_m;
    Vec3 size_m;
    Vec3 rotation_rad;
    Material material;
};

struct FemMeshSettings
{
    // Second order buys accuracy that refining the mesh does not. Measured on
    // the 22.66 x 9.96 x 50 mm guide with a 0.5 mm iris (round window
    // r = 4.6 mm, stub 0.49 x 4.98 mm) at 10 GHz, automatic global cell size,
    // factorised so the linear system is exact to 1e-13:
    //   order 1, 1 element per feature,  ratio 4:   21953 unknowns, |S11| 0.83796
    //   order 1, 3 elements per feature, ratio 20: 104839 unknowns, |S11| 0.86179
    //   order 2, 1 element per feature,  ratio 4:  112246 unknowns, |S11| 0.86934
    // against a converged value near 0.87, and the unitarity defect - the one
    // independent error measure in the run - drops from 8.5e-03 to 2.8e-05 with
    // the order, not with the refinement. It is not cheaper: the second-order
    // run cost 778 s and 10.9 GB against 403 s and 6.0 GB for the refined
    // first-order mesh of the same size. It is bought for accuracy.
    int element_order = 2;
    double maximum_element_size_m = 0.0;
    double minimum_element_size_m = 0.0;
    double geometry_tolerance_m = 1.0e-9;
    int uniform_refinement_levels = 0;
    // Scales the automatic element size: below 1 refines the mesh (slower and
    // more accurate), above 1 coarsens it. Ignored when maximum_element_size_m
    // is set explicitly.
    double refinement_factor = 1.0;
    // Local refinement: every small detail of the geometry (plate thickness,
    // window sides, the metal left between window and plate edge, the stub and
    // the gap beside it) gets this many elements across its smallest dimension.
    // A single global element size makes the element count near a 0.4 mm detail
    // jump between neighbouring integers as the global size drifts, and the
    // S-parameters jump with it. 0 or less switches the local refinement off.
    // Measured on the iris model over the global sizes 2.800 / 2.801 / 2.806 /
    // 2.828 mm, first-order elements, factorised: with no local refinement at
    // all |S11| comes out 0.579 / 0.742 / 0.046 / 0.366, a half-spread of 80 %
    // of the mean, which is what these two fields exist to prevent. One element
    // per feature at size ratio 4 gives 0.8295 / 0.8271 / 0.8280 / 0.8192
    // (0.62 %); three elements at ratio 20 gives 0.8601 / 0.8591 / 0.8580 /
    // 0.8548 (0.31 %). Three elements therefore reproduces twice as well at
    // first order and is still not the default, because the accuracy comes from
    // the element order and at second order three elements blow this same model
    // up to 546474 unknowns and an estimated 234 GB of fill-in - no machine.
    // One element per feature at second order stays at 112246 unknowns and
    // 10.9 GB and returns |S11| = 0.86934, closer to the converged value than
    // the 0.86179 of three elements at first order.
    double elements_across_smallest_feature = 1.0;
    // Safety net against an element explosion: no detail may ask for cells finer
    // than the global element size divided by this ratio. It also governs the
    // cell size contrast of the mesh, and that contrast is what decides whether
    // the iterative solver can work at all - see MfemFrequencyDomainBackend.
    // Measured on the iris model: this ratio at 4 gives a mesh contrast of
    // 6.8:1, where GMRES still converges (5140 iterations, 2.2e-6, 124 s); the
    // former default of 20 gives 20.7:1, where it does not converge at any
    // iteration count tried, so every such model could only be factorised.
    double minimum_size_ratio = 4.0;
    // Plates thinner than this are meshed as zero-thickness PEC sheets. A
    // negative value selects the automatic rule (lambda / 100); 0 forces every
    // plate to be a solid volume. Used to cross-check the two models against
    // each other on a thickness where both are computable.
    double sheet_thickness_threshold_m = -1.0;
};

struct PmlSettings
{
    bool enabled = true;
    double thickness_m = 0.0;
    int polynomial_order = 3;
    double target_reflection = 1.0e-8;
};

// How the assembled complex FEM system is solved. The Maxwell operator is
// indefinite, so the preconditioned Krylov method stalls well above the
// requested tolerance on fine meshes and on second-order elements; a sparse LU
// factorisation solves the same system to rounding but its fill-in grows much
// faster than the matrix itself.
enum class LinearSolverMethod
{
    Automatic,
    Direct,
    Iterative
};

struct FemSolverSettings
{
    FemMeshSettings mesh;
    PmlSettings pml;
    // Stopping criterion of the iterative solver, and of nothing else. It is
    // compared against the PRECONDITIONED relative residual, the only one GMRES
    // has at hand; SolverDiagnostics::linear_relative_residual reports the true
    // ||A x - b|| / ||b|| instead. The two are different quantities and must not
    // be read as one, which is why the progress messages name the preconditioned
    // one explicitly. The direct solver ignores this field entirely.
    double relative_tolerance = 1.0e-8;
    int maximum_iterations = 2000;
    int port_mode_count = 8;
    LinearSolverMethod linear_solver_method = LinearSolverMethod::Automatic;
    // How much memory Automatic may commit to the sparse factorisation. 0 or
    // less means "ask the operating system what is free and keep four fifths of
    // that", which is the only honest answer on a machine whose free memory the
    // caller does not know. What the factorisation actually needs is estimated
    // from the unknown count in the backend; the estimate is fitted to measured
    // peak process memory (21953 unknowns 0.69 GB, 63033 3.56 GB, 104839
    // 6.01 GB, 112246 10.89 GB in Release).
    // This replaced an unknown-count limit, which was the wrong criterion twice
    // over: the direct path is bounded by memory, not by unknowns, and the
    // iterative path is bounded by the cell size contrast of the mesh, not by
    // unknowns either. Because refinement raises both at once, a limit on
    // unknowns sent exactly the refined models to the solver that cannot solve
    // them: the iris model at the old defaults has 63033 unknowns, went to
    // GMRES, and came back after 615 s with no S-parameters and a residual of
    // 2.0e-3. The same model factorises in 286 s.
    double direct_solver_memory_budget_gb = 0.0;
};

struct EmModel
{
    WaveguideGeometry waveguide;
    Material filling_material;
    std::vector<SlotGeometry> slot_geometries;
    std::vector<PecPlateGeometry> pec_plates;
    std::vector<DielectricBlockGeometry> dielectric_blocks;
    // Свободные тела пользователя. Их видит только сеточный решатель: замкнутые
    // формулы и методы частичных областей выведены для пластин известного вида и
    // произвольную форму описать не могут.
    std::vector<ShapeGeometry> shapes;
};

struct ModeSelection
{
    bool automatic = true;
    ModeFamily family = ModeFamily::TransverseElectric;
    int m = 1;
    int n = 0;
};

enum class GeometryApproximationPolicy
{
    Strict,
    UnperturbedBackgroundForSlots
};

// Which solver the dispatcher should use. Automatic keeps the historic
// behaviour (cheapest solver that can represent the geometry); the explicit
// values let the user pin one method and get an error instead of a silent
// fallback when the geometry is outside that method's reach.
enum class SolverMethod
{
    Automatic,
    AnalyticRectangular,
    TransversePartition,
    ModeMatching,
    FiniteElement
};

struct SolverSettings
{
    int maximum_m = 3;
    int maximum_n = 3;
    double normalization_power_w = 1.0;
    GeometryApproximationPolicy geometry_approximation_policy =
        GeometryApproximationPolicy::Strict;
    SolverMethod solver_method = SolverMethod::Automatic;
    FemSolverSettings fem;
};

struct SimulationRequest
{
    EmModel model;
    double frequency_hz = 0.0;
    ModeSelection excitation;
    SolverSettings settings;
};
}
