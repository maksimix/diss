#pragma once

#include "em_math.h"

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

struct RectangularWaveguideGeometry
{
    double inner_width_m = 0.0;
    double inner_height_m = 0.0;
    double length_m = 0.0;
    double wall_thickness_m = 0.0;
    // Finite wall conductivity for the perturbation conductor-loss model.
    // 0 (or negative) means a perfect electric conductor: lossless walls.
    double wall_conductivity_s_per_m = 0.0;
};

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
    int element_order = 1;
    double maximum_element_size_m = 0.0;
    double minimum_element_size_m = 0.0;
    double geometry_tolerance_m = 1.0e-9;
    int uniform_refinement_levels = 0;
    // Scales the automatic element size: below 1 refines the mesh (slower and
    // more accurate), above 1 coarsens it. Ignored when maximum_element_size_m
    // is set explicitly.
    double refinement_factor = 1.0;
};

struct PmlSettings
{
    bool enabled = true;
    double thickness_m = 0.0;
    int polynomial_order = 3;
    double target_reflection = 1.0e-8;
};

struct FemSolverSettings
{
    FemMeshSettings mesh;
    PmlSettings pml;
    double relative_tolerance = 1.0e-8;
    int maximum_iterations = 2000;
    int port_mode_count = 8;
};

struct EmModel
{
    RectangularWaveguideGeometry waveguide;
    Material filling_material;
    std::vector<SlotGeometry> slot_geometries;
    std::vector<PecPlateGeometry> pec_plates;
    std::vector<DielectricBlockGeometry> dielectric_blocks;
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
