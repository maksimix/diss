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

struct PecPlateGeometry
{
    bool enabled = false;
    Vec3 center_m;
    Vec3 size_m;
    Vec3 rotation_rad;
};

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

struct SolverSettings
{
    int maximum_m = 3;
    int maximum_n = 3;
    double normalization_power_w = 1.0;
    GeometryApproximationPolicy geometry_approximation_policy =
        GeometryApproximationPolicy::Strict;
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
