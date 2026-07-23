#include "gmsh_tetrahedral_mesher.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace em
{
namespace
{
std::string quoted(const std::filesystem::path &path)
{
    return "\"" + path.string() + "\"";
}

#ifdef _WIN32
// Directory of the running .exe, not the current directory: the calculation
// starts from whatever folder the user launched the program from, and a
// portable kit has to find its own gmsh regardless.
std::filesystem::path executableDirectory()
{
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
        const DWORD length =
            GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) {
            return {};
        }
        // The call truncates silently and reports the full buffer size, so a
        // result that fills the buffer means the path did not fit.
        if (length < buffer.size()) {
            buffer.resize(length);
            break;
        }
        buffer.resize(2 * buffer.size());
    }
    return std::filesystem::path(buffer).parent_path();
}
#endif

double automaticMeshSize(const SimulationRequest &request)
{
    const auto &waveguide = request.model.waveguide;
    const Complex eps_r = request.model.filling_material.relative_permittivity;
    const Complex mu_r = request.model.filling_material.relative_permeability;
    const double material_scale = std::sqrt(std::max(1.0, std::abs(eps_r * mu_r)));
    const double wavelength = speed_of_light_m_per_s /
                              (request.frequency_hz * material_scale);
    // Five elements across the height keep the serial GMRES/GS solver inside
    // its convergent range; h/8 produced systems that stall near 1e-4 while
    // changing the S-parameters of the converged solution by under 0.2%.
    const double base_size_m = std::min({waveguide.inner_width_m / 8.0,
                                         waveguide.inner_height_m / 5.0,
                                         wavelength / 12.0});
    // The element count per wavelength above is a first-order budget. A Nedelec
    // element of order p carries a degree-p field inside the tetrahedron and
    // reaches the same accuracy on a proportionally coarser mesh, so spending
    // the first-order budget on second-order elements buys nothing and costs
    // roughly five times the unknowns: on the 22.66 x 9.96 mm iris the
    // second-order mesh at the first-order size needs 111494 unknowns and an
    // estimated 13.6 GB, which no ordinary machine can factorise.
    const int element_order = std::clamp(request.settings.fem.mesh.element_order, 1, 3);
    const double factor = std::clamp(request.settings.fem.mesh.refinement_factor, 0.2, 4.0);
    return base_size_m * element_order * factor;
}

// Emits a box centred on `center` and rotates it about `rotation_origin`
// (X, then Y, then Z about the world axes). The separate origin lets a window
// cutter inherit the parent plate's orientation while sitting off-centre.
void appendRotatedBox(std::ostringstream &script,
                      int tag,
                      const Vec3 &center,
                      const Vec3 &size,
                      const Vec3 &rotation,
                      const Vec3 &rotation_origin)
{
    script << "Box(" << tag << ") = {"
           << center.x - 0.5 * size.x << ","
           << center.y - 0.5 * size.y << ","
           << center.z - 0.5 * size.z << ","
           << size.x << "," << size.y << "," << size.z << "};\n";
    if (rotation.x != 0.0) {
        script << "Rotate {{1,0,0},{" << rotation_origin.x << "," << rotation_origin.y << ","
               << rotation_origin.z << "}," << rotation.x << "} { Volume{" << tag << "}; }\n";
    }
    if (rotation.y != 0.0) {
        script << "Rotate {{0,1,0},{" << rotation_origin.x << "," << rotation_origin.y << ","
               << rotation_origin.z << "}," << rotation.y << "} { Volume{" << tag << "}; }\n";
    }
    if (rotation.z != 0.0) {
        script << "Rotate {{0,0,1},{" << rotation_origin.x << "," << rotation_origin.y << ","
               << rotation_origin.z << "}," << rotation.z << "} { Volume{" << tag << "}; }\n";
    }
}

void appendRotatedBox(std::ostringstream &script,
                      int tag,
                      const Vec3 &center,
                      const Vec3 &size,
                      const Vec3 &rotation)
{
    appendRotatedBox(script, tag, center, size, rotation, center);
}

// Cylinder along the plate normal (local z), rotated with the plate. `base` is
// the centre of the bottom cap.
void appendRotatedCylinder(std::ostringstream &script,
                           int tag,
                           const Vec3 &base,
                           double length,
                           double radius,
                           const Vec3 &rotation,
                           const Vec3 &rotation_origin)
{
    script << "Cylinder(" << tag << ") = {" << base.x << "," << base.y << "," << base.z
           << ",0,0," << length << "," << radius << "};\n";
    if (rotation.x != 0.0) {
        script << "Rotate {{1,0,0},{" << rotation_origin.x << "," << rotation_origin.y << ","
               << rotation_origin.z << "}," << rotation.x << "} { Volume{" << tag << "}; }\n";
    }
    if (rotation.y != 0.0) {
        script << "Rotate {{0,1,0},{" << rotation_origin.x << "," << rotation_origin.y << ","
               << rotation_origin.z << "}," << rotation.y << "} { Volume{" << tag << "}; }\n";
    }
    if (rotation.z != 0.0) {
        script << "Rotate {{0,0,1},{" << rotation_origin.x << "," << rotation_origin.y << ","
               << rotation_origin.z << "}," << rotation.z << "} { Volume{" << tag << "}; }\n";
    }
}

// A perfect conductor much thinner than the wavelength carries no useful
// thickness information: the field is excluded within a fraction of a skin
// depth, so the plate is electromagnetically a surface. Meshing such a slab as
// a volume only yields degenerate "pancake" tetrahedra that destroy the
// conditioning of the H(curl) system, so below this threshold the plate is
// built as a zero-thickness sheet embedded in the cavity.
constexpr double sheet_thickness_fraction_of_wavelength = 0.01;   // lambda / 100

// The largest thickness that is still modelled as a surface. A sheet has no
// thickness of its own, so this is the thickness it stands for, and the mesh
// around its edges is built on it.
double sheetSubstituteThickness(double wavelength_m, double threshold_override_m)
{
    return threshold_override_m >= 0.0
               ? threshold_override_m
               : sheet_thickness_fraction_of_wavelength * wavelength_m;
}

bool plateIsThinSheet(const PecPlateGeometry &plate,
                      double wavelength_m,
                      double threshold_override_m)
{
    if (!(plate.size_m.z > 0.0) || !(wavelength_m > 0.0)) {
        return false;
    }
    if (threshold_override_m >= 0.0) {
        return plate.size_m.z < threshold_override_m;
    }
    // Only an unrotated transverse plate: the aperture and the stub live in the
    // plate plane, and a rotated sheet could not be recovered by an axis-aligned
    // bounding box after the boolean fragmentation.
    constexpr double angle_tolerance_rad = 1.0e-10;
    if (std::abs(plate.rotation_rad.x) > angle_tolerance_rad ||
        std::abs(plate.rotation_rad.y) > angle_tolerance_rad ||
        std::abs(plate.rotation_rad.z) > angle_tolerance_rad) {
        return false;
    }
    return plate.size_m.z < sheet_thickness_fraction_of_wavelength * wavelength_m;
}

// Emits the plate as a planar surface: rectangle, minus the window, plus the
// stub. The resulting surface list is accumulated into pecSheets[].
void appendSheetPlate(std::ostringstream &script,
                      int &next_tag,
                      const PecPlateGeometry &plate)
{
    const int plate_tag = next_tag++;
    const double plane_z = plate.center_m.z;
    script << "Rectangle(" << plate_tag << ") = {"
           << plate.center_m.x - 0.5 * plate.size_m.x << ","
           << plate.center_m.y - 0.5 * plate.size_m.y << "," << plane_z << ","
           << plate.size_m.x << "," << plate.size_m.y << "};\n";

    std::ostringstream current;
    current << "{" << plate_tag << "}";
    const std::string sheet_name = "sheet" + std::to_string(plate_tag);

    if (plateHasOpening(plate)) {
        const int window_tag = next_tag++;
        const double window_x = plate.center_m.x + plate.aperture_offset_x_m;
        const double window_y = plate.center_m.y + plate.aperture_offset_y_m;
        if (plate.aperture_shape == PlateApertureShape::Circular) {
            script << "Disk(" << window_tag << ") = {" << window_x << "," << window_y << ","
                   << plane_z << "," << plate.aperture_radius_m << "};\n";
        } else {
            script << "Rectangle(" << window_tag << ") = {"
                   << window_x - 0.5 * plate.aperture_width_m << ","
                   << window_y - 0.5 * plate.aperture_height_m << "," << plane_z << ","
                   << plate.aperture_width_m << "," << plate.aperture_height_m << "};\n";
        }
        script << sheet_name << "[] = BooleanDifference{ Surface" << current.str()
               << "; Delete; }{ Surface{" << window_tag << "}; Delete; };\n";
    } else {
        script << sheet_name << "[] = " << current.str() << ";\n";
    }

    if (plateHasPost(plate)) {
        const int stub_tag = next_tag++;
        const double bottom = plate.center_m.y - 0.5 * plate.size_m.y;
        script << "Rectangle(" << stub_tag << ") = {"
               << plate.center_m.x + plate.aperture_offset_x_m - 0.5 * plate.post_width_m << ","
               << bottom << "," << plane_z << "," << plate.post_width_m << ","
               << plate.post_height_m << "};\n"
               << sheet_name << "[] = BooleanUnion{ Surface{" << sheet_name
               << "[]}; Delete; }{ Surface{" << stub_tag << "}; Delete; };\n";
    }
    script << "pecSheets[] += " << sheet_name << "[];\n";
}

void appendSlotCutter(std::ostringstream &script,
                      int tag,
                      const SlotGeometry &slot,
                      const WaveguideGeometry &waveguide)
{
    const double wall = waveguide.wall_thickness_m;
    Vec3 center;
    Vec3 size;
    Vec3 rotation;
    switch (slot.wall) {
    case WallSurface::Top:
        center = {slot.center_u_m, 0.5 * (waveguide.inner_height_m + wall), slot.center_z_m};
        size = {slot.width_m, 1.2 * wall, slot.length_m};
        rotation.y = slot.rotation_rad;
        break;
    case WallSurface::Bottom:
        center = {slot.center_u_m, -0.5 * (waveguide.inner_height_m + wall), slot.center_z_m};
        size = {slot.width_m, 1.2 * wall, slot.length_m};
        rotation.y = slot.rotation_rad;
        break;
    case WallSurface::Right:
        center = {0.5 * (waveguide.inner_width_m + wall), slot.center_u_m, slot.center_z_m};
        size = {1.2 * wall, slot.width_m, slot.length_m};
        rotation.x = -slot.rotation_rad;
        break;
    case WallSurface::Left:
        center = {-0.5 * (waveguide.inner_width_m + wall), slot.center_u_m, slot.center_z_m};
        size = {1.2 * wall, slot.width_m, slot.length_m};
        rotation.x = -slot.rotation_rad;
        break;
    }
    appendRotatedBox(script, tag, center, size, rotation);
}

// One small detail of the geometry: the axis-aligned box it occupies and the
// element size that resolves it.
struct MeshRefinementRegion
{
    Vec3 minimum_m;
    Vec3 maximum_m;
    double element_size_m = 0.0;
};

struct MeshRefinementLimits
{
    double elements_across_feature = 0.0;
    double smallest_element_size_m = 0.0;
    double global_element_size_m = 0.0;
    // Below this a clearance is not a gap: the boolean welds the two faces
    // together and leaves no fluid between them.
    double geometry_tolerance_m = 0.0;
};

// Upper bound on how far the rotation moves any point of the plate: each small
// rotation about a world axis displaces a point at radius r by r * angle, and
// the half diagonal is the largest radius the plate has.
double plateRotationDisplacement(const PecPlateGeometry &plate)
{
    const double half_diagonal_m = 0.5 * std::sqrt(plate.size_m.x * plate.size_m.x +
                                                   plate.size_m.y * plate.size_m.y +
                                                   plate.size_m.z * plate.size_m.z);
    return half_diagonal_m * (std::abs(plate.rotation_rad.x) +
                              std::abs(plate.rotation_rad.y) +
                              std::abs(plate.rotation_rad.z));
}

// A rotation counts as none while it cannot move any point of the plate by more
// than this fraction of the finest cell the refinement may ask for: an axis
// aligned box then still covers the same fluid, down to a tenth of a cell. A
// tolerance stated as an angle instead would be meaningless - 1e-9 rad is 1e-11 m
// of displacement on a WR-90 plate, yet it used to switch the whole local
// refinement off and with it the only thing keeping the answer stable.
constexpr double rotation_displacement_fraction_of_element = 0.1;

bool plateIsAxisAligned(const PecPlateGeometry &plate, const MeshRefinementLimits &limits)
{
    const double element_size_m = limits.smallest_element_size_m > 0.0
                                      ? limits.smallest_element_size_m
                                      : limits.global_element_size_m;
    return plateRotationDisplacement(plate) <=
           rotation_displacement_fraction_of_element * element_size_m;
}

// Registers one detail. The box is grown by a single refined element so the
// small cells surround the detail instead of stopping on its faces. Details
// that the global size already resolves are dropped, so a model without small
// features produces no size field at all and the script stays as it was.
void appendRefinementBox(std::vector<MeshRefinementRegion> &regions,
                         const Vec3 &minimum_m,
                         const Vec3 &maximum_m,
                         double feature_size_m,
                         const MeshRefinementLimits &limits)
{
    if (!(feature_size_m > 0.0) || !(limits.elements_across_feature > 0.0)) {
        return;
    }
    const double element_size_m = std::max(feature_size_m / limits.elements_across_feature,
                                           limits.smallest_element_size_m);
    if (element_size_m >= limits.global_element_size_m) {
        return;
    }
    MeshRefinementRegion region;
    region.minimum_m = {minimum_m.x - element_size_m,
                        minimum_m.y - element_size_m,
                        minimum_m.z - element_size_m};
    region.maximum_m = {maximum_m.x + element_size_m,
                        maximum_m.y + element_size_m,
                        maximum_m.z + element_size_m};
    region.element_size_m = element_size_m;
    regions.push_back(region);
}

// One refinement box hugging a straight edge of the plate outline: the band is
// added on both sides of the edge and on both ends, and the z span covers the
// plate thickness.
void appendEdgeBandBox(std::vector<MeshRefinementRegion> &regions,
                       double from_x_m,
                       double from_y_m,
                       double to_x_m,
                       double to_y_m,
                       double band_m,
                       double minimum_z_m,
                       double maximum_z_m,
                       double feature_size_m,
                       const MeshRefinementLimits &limits)
{
    appendRefinementBox(regions,
                        {std::min(from_x_m, to_x_m) - band_m,
                         std::min(from_y_m, to_y_m) - band_m,
                         minimum_z_m},
                        {std::max(from_x_m, to_x_m) + band_m,
                         std::max(from_y_m, to_y_m) + band_m,
                         maximum_z_m},
                        feature_size_m,
                        limits);
}

// A round rim is sampled as a polygon with a multiple of four sides, so that
// every chord stays inside one quadrant of the circle: there both coordinates
// run monotonically along the arc, and the box built on the two ends of the
// chord contains the arc exactly. The number of sides follows from the bulge
// allowed on one chord, half the band.
constexpr double rim_polygon_sagitta_fraction_of_band = 0.5;

// Clearance between each outer edge of the plate and the waveguide wall it
// faces. The walls are the faces of the fluid box the script actually builds -
// this file has no cylinder in it, so the fluid is a rectangular tube whatever
// the cross-section says, and a plate in a round guide never gets here anyway:
// the dispatcher refuses it. A clearance at or below the geometric tolerance
// means the boolean welds the plate onto the wall and leaves no fluid beside
// that edge at all.
struct PlateWallClearances
{
    double minimum_x_m = 0.0;
    double maximum_x_m = 0.0;
    double minimum_y_m = 0.0;
    double maximum_y_m = 0.0;
};

PlateWallClearances plateWallClearances(const PecPlateGeometry &plate,
                                        const WaveguideGeometry &waveguide)
{
    const double half_width_m = 0.5 * waveguide.inner_width_m;
    const double half_height_m = 0.5 * waveguide.inner_height_m;
    PlateWallClearances clearances;
    clearances.minimum_x_m = plate.center_m.x - 0.5 * plate.size_m.x + half_width_m;
    clearances.maximum_x_m = half_width_m - plate.center_m.x - 0.5 * plate.size_m.x;
    clearances.minimum_y_m = plate.center_m.y - 0.5 * plate.size_m.y + half_height_m;
    clearances.maximum_y_m = half_height_m - plate.center_m.y - 0.5 * plate.size_m.y;
    return clearances;
}

// The fluid squeezed between an outer edge of the plate and the wall in front
// of it. This is a small detail in its own right and it is its OWN width that
// sets the cell size in it - the narrower the gap, the harder the field is
// pressed through it, exactly as beside the stub. Sizing this fluid by the
// plate thickness instead inverted the rule: the narrower the gap, the coarser
// the cell asked for, so a 0.95 mm gap in a 3 mm thick plate demanded 1 mm
// cells, was rejected as "already resolved" and got no refinement at all.
void appendWallGapRegions(const PecPlateGeometry &plate,
                          const PlateWallClearances &clearances,
                          const MeshRefinementLimits &limits,
                          double minimum_z_m,
                          double maximum_z_m,
                          std::vector<MeshRefinementRegion> &regions)
{
    const double plate_minimum_x_m = plate.center_m.x - 0.5 * plate.size_m.x;
    const double plate_maximum_x_m = plate.center_m.x + 0.5 * plate.size_m.x;
    const double plate_minimum_y_m = plate.center_m.y - 0.5 * plate.size_m.y;
    const double plate_maximum_y_m = plate.center_m.y + 0.5 * plate.size_m.y;
    if (clearances.minimum_x_m > limits.geometry_tolerance_m) {
        appendRefinementBox(regions,
                            {plate_minimum_x_m - clearances.minimum_x_m, plate_minimum_y_m,
                             minimum_z_m},
                            {plate_minimum_x_m, plate_maximum_y_m, maximum_z_m},
                            clearances.minimum_x_m, limits);
    }
    if (clearances.maximum_x_m > limits.geometry_tolerance_m) {
        appendRefinementBox(regions,
                            {plate_maximum_x_m, plate_minimum_y_m, minimum_z_m},
                            {plate_maximum_x_m + clearances.maximum_x_m, plate_maximum_y_m,
                             maximum_z_m},
                            clearances.maximum_x_m, limits);
    }
    if (clearances.minimum_y_m > limits.geometry_tolerance_m) {
        appendRefinementBox(regions,
                            {plate_minimum_x_m, plate_minimum_y_m - clearances.minimum_y_m,
                             minimum_z_m},
                            {plate_maximum_x_m, plate_minimum_y_m, maximum_z_m},
                            clearances.minimum_y_m, limits);
    }
    if (clearances.maximum_y_m > limits.geometry_tolerance_m) {
        appendRefinementBox(regions,
                            {plate_minimum_x_m, plate_maximum_y_m, minimum_z_m},
                            {plate_maximum_x_m,
                             plate_maximum_y_m + clearances.maximum_y_m, maximum_z_m},
                            clearances.maximum_y_m, limits);
    }
}

// The plate volume is subtracted from the fluid, so the metal itself is never
// meshed: fine cells spread over the whole plate face would resolve nothing but
// the air on both sides of a blind wall, where the field is zero. What the
// MESHED fluid does see is every edge the metal ends on, because the field of a
// conducting edge is singular there:
//   - the rim of the window, where the fluid crosses the plate through a tunnel
//     of length t and turns around a 90 degree metal edge;
//   - an outer edge that stops short of the waveguide wall, where the fluid
//     bends around the end face of the plate.
// Both get a band `edge_feature_size_m` wide on each side of the edge, holding
// `elements_across_feature` cells across it. For a solid plate that scale is
// the thickness: the field of an edge decays on the scale of the feature
// itself, and beyond about a thickness the slab is indistinguishable from a
// zero-thickness sheet. For the 0.5 mm reference plate this is 0.5 mm plus the
// one-element growth of appendRefinementBox, i.e. 0.667 mm or four fine cells
// on each side of the rim, instead of the 11.33 mm half-width of the waveguide
// that the plate face used to claim. Measured on the reference iris (22.66 x
// 9.96 x 50 mm, 0.5 mm plate, 4.6 mm round window, stub): 186386 tetrahedra for
// the plate-face box against 73435 for the bands, with the element count around
// the stub just as steady (0.55% against 0.56% of spread over the element sizes
// 2.800 ... 2.828 mm).
void appendPlateEdgeRegions(const PecPlateGeometry &plate,
                            const PlateWallClearances &clearances,
                            const MeshRefinementLimits &limits,
                            double edge_feature_size_m,
                            double minimum_z_m,
                            double maximum_z_m,
                            std::vector<MeshRefinementRegion> &regions)
{
    if (!(edge_feature_size_m > 0.0) || !(limits.elements_across_feature > 0.0)) {
        return;
    }
    const double band_m = edge_feature_size_m;
    const double plate_minimum_x_m = plate.center_m.x - 0.5 * plate.size_m.x;
    const double plate_maximum_x_m = plate.center_m.x + 0.5 * plate.size_m.x;
    const double plate_minimum_y_m = plate.center_m.y - 0.5 * plate.size_m.y;
    const double plate_maximum_y_m = plate.center_m.y + 0.5 * plate.size_m.y;

    if (plateHasOpening(plate)) {
        const double window_center_x_m = plate.center_m.x + plate.aperture_offset_x_m;
        const double window_center_y_m = plate.center_m.y + plate.aperture_offset_y_m;
        if (plate.aperture_shape == PlateApertureShape::Circular) {
            const double radius_m = plate.aperture_radius_m;
            const double sagitta_m = rim_polygon_sagitta_fraction_of_band * band_m;
            // A window far wider than the plate is thick drives the allowed bulge
            // below the resolution of acos(), which would return a flat zero, so
            // the angle is floored at the value that already gives the cap.
            constexpr int maximum_quadrant_segments = 16;
            const double half_angle_rad =
                std::max(std::acos(std::clamp(1.0 - sagitta_m / radius_m, -1.0, 1.0)),
                         0.25 * pi / maximum_quadrant_segments);
            const int quadrant_segments =
                std::min(static_cast<int>(std::ceil(0.25 * pi / half_angle_rad)),
                         maximum_quadrant_segments);
            const int segment_count = 4 * quadrant_segments;
            for (int segment = 0; segment < segment_count; ++segment) {
                const double from_rad = 2.0 * pi * segment / segment_count;
                const double to_rad = 2.0 * pi * (segment + 1) / segment_count;
                appendEdgeBandBox(regions,
                                  window_center_x_m + radius_m * std::cos(from_rad),
                                  window_center_y_m + radius_m * std::sin(from_rad),
                                  window_center_x_m + radius_m * std::cos(to_rad),
                                  window_center_y_m + radius_m * std::sin(to_rad),
                                  band_m, minimum_z_m, maximum_z_m, edge_feature_size_m,
                                  limits);
            }
        } else {
            const double window_minimum_x_m = window_center_x_m - 0.5 * plate.aperture_width_m;
            const double window_maximum_x_m = window_center_x_m + 0.5 * plate.aperture_width_m;
            const double window_minimum_y_m = window_center_y_m - 0.5 * plate.aperture_height_m;
            const double window_maximum_y_m = window_center_y_m + 0.5 * plate.aperture_height_m;
            appendEdgeBandBox(regions, window_minimum_x_m, window_minimum_y_m,
                              window_minimum_x_m, window_maximum_y_m, band_m,
                              minimum_z_m, maximum_z_m, edge_feature_size_m, limits);
            appendEdgeBandBox(regions, window_maximum_x_m, window_minimum_y_m,
                              window_maximum_x_m, window_maximum_y_m, band_m,
                              minimum_z_m, maximum_z_m, edge_feature_size_m, limits);
            appendEdgeBandBox(regions, window_minimum_x_m, window_minimum_y_m,
                              window_maximum_x_m, window_minimum_y_m, band_m,
                              minimum_z_m, maximum_z_m, edge_feature_size_m, limits);
            appendEdgeBandBox(regions, window_minimum_x_m, window_maximum_y_m,
                              window_maximum_x_m, window_maximum_y_m, band_m,
                              minimum_z_m, maximum_z_m, edge_feature_size_m, limits);
        }
    }

    // An outer edge is free unless the plate ends flush against the wall: any
    // fluid at all beside the edge exposes the end face of the plate and the
    // singular field on it. The width of that fluid is a separate detail, sized
    // by appendWallGapRegions; here only its existence matters, so the test is
    // the geometric tolerance and not an element size. Testing against an
    // element size made the criterion self-defeating - the narrower (and hence
    // the more singular) the gap, the more surely the edge was declared blind.
    if (clearances.minimum_x_m > limits.geometry_tolerance_m) {
        appendEdgeBandBox(regions, plate_minimum_x_m, plate_minimum_y_m,
                          plate_minimum_x_m, plate_maximum_y_m, band_m,
                          minimum_z_m, maximum_z_m, edge_feature_size_m, limits);
    }
    if (clearances.maximum_x_m > limits.geometry_tolerance_m) {
        appendEdgeBandBox(regions, plate_maximum_x_m, plate_minimum_y_m,
                          plate_maximum_x_m, plate_maximum_y_m, band_m,
                          minimum_z_m, maximum_z_m, edge_feature_size_m, limits);
    }
    if (clearances.minimum_y_m > limits.geometry_tolerance_m) {
        appendEdgeBandBox(regions, plate_minimum_x_m, plate_minimum_y_m,
                          plate_maximum_x_m, plate_minimum_y_m, band_m,
                          minimum_z_m, maximum_z_m, edge_feature_size_m, limits);
    }
    if (clearances.maximum_y_m > limits.geometry_tolerance_m) {
        appendEdgeBandBox(regions, plate_minimum_x_m, plate_maximum_y_m,
                          plate_maximum_x_m, plate_maximum_y_m, band_m,
                          minimum_z_m, maximum_z_m, edge_feature_size_m, limits);
    }
}

// Collects the small details of one plate. The caller guarantees the plate is
// axis aligned: Field[Box] cannot follow a rotation, and a box in the wrong
// place is worse than no box.
void appendPlateRefinementRegions(const PecPlateGeometry &plate,
                                  const WaveguideGeometry &waveguide,
                                  bool meshed_as_thin_sheet,
                                  double sheet_substitute_thickness_m,
                                  const MeshRefinementLimits &limits,
                                  std::vector<MeshRefinementRegion> &regions)
{
    const double plate_minimum_x_m = plate.center_m.x - 0.5 * plate.size_m.x;
    const double plate_maximum_x_m = plate.center_m.x + 0.5 * plate.size_m.x;
    const double plate_minimum_y_m = plate.center_m.y - 0.5 * plate.size_m.y;
    const double plate_maximum_y_m = plate.center_m.y + 0.5 * plate.size_m.y;
    // A sheet has no thickness to resolve, but its edges still need the fine
    // cells, so every detail below keeps the same zero-thickness z span.
    const double half_thickness_m = meshed_as_thin_sheet ? 0.0 : 0.5 * plate.size_m.z;
    const double minimum_z_m = plate.center_m.z - half_thickness_m;
    const double maximum_z_m = plate.center_m.z + half_thickness_m;

    // A sheet has no thickness of its own to set the width of the edge band, yet
    // its edge carries the strongest singularity of the whole family: E ~
    // rho^(-1/2) on a zero-thickness rim against rho^(-1/3) on a right-angled
    // one. Leaving the sheet out entirely, as the code used to, refined every
    // edge of the family except the sharpest. The length to build the band on is
    // the thickness the sheet MODEL stands for, i.e. the threshold below which a
    // plate is turned into a surface: it keeps the mesh continuous across that
    // switch, where 0.31 mm and 0.29 mm at 10 GHz used to mean a 0.31 mm band
    // and no band at all.
    const double edge_feature_size_m =
        meshed_as_thin_sheet ? sheet_substitute_thickness_m : plate.size_m.z;
    const PlateWallClearances clearances = plateWallClearances(plate, waveguide);
    appendPlateEdgeRegions(plate, clearances, limits, edge_feature_size_m, minimum_z_m,
                           maximum_z_m, regions);
    appendWallGapRegions(plate, clearances, limits, minimum_z_m, maximum_z_m, regions);
    if (!plateHasOpening(plate)) {
        return;
    }

    const bool circular_window = plate.aperture_shape == PlateApertureShape::Circular;
    const double window_half_width_m = circular_window ? plate.aperture_radius_m
                                                       : 0.5 * plate.aperture_width_m;
    const double window_half_height_m = circular_window ? plate.aperture_radius_m
                                                        : 0.5 * plate.aperture_height_m;
    const double window_center_x_m = plate.center_m.x + plate.aperture_offset_x_m;
    const double window_center_y_m = plate.center_m.y + plate.aperture_offset_y_m;
    const double window_minimum_x_m = window_center_x_m - window_half_width_m;
    const double window_maximum_x_m = window_center_x_m + window_half_width_m;
    const double window_minimum_y_m = window_center_y_m - window_half_height_m;
    const double window_maximum_y_m = window_center_y_m + window_half_height_m;
    const Vec3 window_minimum_m{window_minimum_x_m, window_minimum_y_m, minimum_z_m};
    const Vec3 window_maximum_m{window_maximum_x_m, window_maximum_y_m, maximum_z_m};

    appendRefinementBox(regions, window_minimum_m, window_maximum_m,
                        2.0 * window_half_width_m, limits);
    appendRefinementBox(regions, window_minimum_m, window_maximum_m,
                        2.0 * window_half_height_m, limits);

    // The metal bridges left between the window and the plate edge: a narrow
    // bridge crowds the induced current and is the detail most often lost.
    appendRefinementBox(regions,
                        {plate_minimum_x_m, window_minimum_y_m, minimum_z_m},
                        {window_minimum_x_m, window_maximum_y_m, maximum_z_m},
                        window_minimum_x_m - plate_minimum_x_m, limits);
    appendRefinementBox(regions,
                        {window_maximum_x_m, window_minimum_y_m, minimum_z_m},
                        {plate_maximum_x_m, window_maximum_y_m, maximum_z_m},
                        plate_maximum_x_m - window_maximum_x_m, limits);
    appendRefinementBox(regions,
                        {window_minimum_x_m, plate_minimum_y_m, minimum_z_m},
                        {window_maximum_x_m, window_minimum_y_m, maximum_z_m},
                        window_minimum_y_m - plate_minimum_y_m, limits);
    appendRefinementBox(regions,
                        {window_minimum_x_m, window_maximum_y_m, minimum_z_m},
                        {window_maximum_x_m, plate_maximum_y_m, maximum_z_m},
                        plate_maximum_y_m - window_maximum_y_m, limits);

    if (!plateHasPost(plate)) {
        return;
    }
    const double stub_minimum_x_m = window_center_x_m - 0.5 * plate.post_width_m;
    const double stub_maximum_x_m = window_center_x_m + 0.5 * plate.post_width_m;
    const double stub_maximum_y_m = plate_minimum_y_m + plate.post_height_m;
    const Vec3 stub_minimum_m{stub_minimum_x_m, plate_minimum_y_m, minimum_z_m};
    const Vec3 stub_maximum_m{stub_maximum_x_m, stub_maximum_y_m, maximum_z_m};
    appendRefinementBox(regions, stub_minimum_m, stub_maximum_m,
                        plate.post_width_m, limits);
    appendRefinementBox(regions, stub_minimum_m, stub_maximum_m,
                        plate.post_height_m, limits);

    // Gaps left free on both sides of the stub: this is where the field squeezes
    // through, so it is the size that actually sets the reflection.
    appendRefinementBox(regions,
                        {window_minimum_x_m, plate_minimum_y_m, minimum_z_m},
                        {stub_minimum_x_m, stub_maximum_y_m, maximum_z_m},
                        stub_minimum_x_m - window_minimum_x_m, limits);
    appendRefinementBox(regions,
                        {stub_maximum_x_m, plate_minimum_y_m, minimum_z_m},
                        {window_maximum_x_m, stub_maximum_y_m, maximum_z_m},
                        window_maximum_x_m - stub_maximum_x_m, limits);
}

// Turns the collected details into gmsh size fields. Field[Box] holds VIn
// inside the box and blends to VOut across Thickness; Field[Min] combines them.
void appendMeshSizeFields(std::ostringstream &script,
                          const std::vector<MeshRefinementRegion> &regions,
                          double global_element_size_m,
                          double smallest_element_size_m)
{
    if (regions.empty()) {
        return;
    }
    for (std::size_t index = 0; index < regions.size(); ++index) {
        const MeshRefinementRegion &region = regions[index];
        const std::string field = "Field[" + std::to_string(index + 1) + "].";
        script << "Field[" << index + 1 << "] = Box;\n"
               << field << "XMin = " << region.minimum_m.x << ";\n"
               << field << "XMax = " << region.maximum_m.x << ";\n"
               << field << "YMin = " << region.minimum_m.y << ";\n"
               << field << "YMax = " << region.maximum_m.y << ";\n"
               << field << "ZMin = " << region.minimum_m.z << ";\n"
               << field << "ZMax = " << region.maximum_m.z << ";\n"
               << field << "VIn = " << region.element_size_m << ";\n"
               << field << "VOut = " << global_element_size_m << ";\n"
               << field << "Thickness = " << global_element_size_m << ";\n";
    }
    const std::size_t minimum_field = regions.size() + 1;
    script << "Field[" << minimum_field << "] = Min;\n"
           << "Field[" << minimum_field << "].FieldsList = {";
    for (std::size_t index = 0; index < regions.size(); ++index) {
        script << (index == 0 ? "" : ",") << index + 1;
    }
    // The sizes interpolated from the CAD vertices would otherwise override the
    // background field and wash the refinement out again.
    script << "};\n"
           << "Background Field = " << minimum_field << ";\n"
           << "Mesh.MeshSizeExtendFromBoundary = 0;\n"
           << "Mesh.MeshSizeFromPoints = 0;\n"
           << "Mesh.MeshSizeFromCurvature = 0;\n"
           << "Mesh.MeshSizeMin = " << smallest_element_size_m << ";\n";
}
}

GmshTetrahedralMesher::GmshTetrahedralMesher(std::filesystem::path gmsh_executable)
    : gmsh_executable_(std::move(gmsh_executable))
{
}

std::filesystem::path GmshTetrahedralMesher::resolveExecutable() const
{
    if (!gmsh_executable_.empty()) {
        return gmsh_executable_;
    }
#ifdef _MSC_VER
    char *gmsh_path = nullptr;
    std::size_t path_length = 0;
    if (_dupenv_s(&gmsh_path, &path_length, "GMSH_EXECUTABLE") == 0 && gmsh_path) {
        const std::filesystem::path result(gmsh_path);
        std::free(gmsh_path);
        // An empty variable names no executable: taking it built a command line
        // with an empty program name, which the shell answers with its own error
        // instead of anything about gmsh.
        if (!result.empty()) {
            return result;
        }
    }
#else
    if (const char *gmsh_path = std::getenv("GMSH_EXECUTABLE")) {
        if (*gmsh_path != '\0') {
            return gmsh_path;
        }
    }
#endif
#ifdef _WIN32
    // Order of the fallbacks. The environment variable above always wins: it is
    // the only way to point the program at one particular build. Next comes the
    // gmsh shipped beside the program, because that is the one the kit was put
    // together and tested with; only then a gmsh that happens to be installed on
    // this machine, and last whatever the PATH resolves - the least controlled
    // of all, and the only one that can silently change between two runs.
    const std::filesystem::path own_directory = executableDirectory();
    if (!own_directory.empty()) {
        for (const std::filesystem::path &candidate : {own_directory / "gmsh.exe",
                                                       own_directory / "gmsh" / "gmsh.exe",
                                                       own_directory / "gmsh" / "bin" /
                                                           "gmsh.exe"}) {
            if (std::filesystem::exists(candidate)) {
                return candidate;
            }
        }
    }
    const std::filesystem::path cst_gmsh =
        R"(C:\Program Files (x86)\CST Studio Suite 2025\WASP-NET\bin\gmsh.exe)";
    if (std::filesystem::exists(cst_gmsh)) {
        return cst_gmsh;
    }
#endif
    return "gmsh";
}

FemMeshFiles GmshTetrahedralMesher::generate(
    const SimulationRequest &request,
    const std::filesystem::path &working_directory) const
{
    FemMeshFiles files;
    files.geometry_path = working_directory / "waveguide_fem.geo";
    files.mesh_path = working_directory / "waveguide_fem.msh";

    std::error_code error;
    std::filesystem::create_directories(working_directory, error);
    if (error) {
        files.error_message = "Cannot create FEM working directory: " + error.message();
        return files;
    }

    std::ofstream geometry(files.geometry_path, std::ios::binary);
    if (!geometry) {
        files.error_message = "Cannot write Gmsh geometry file.";
        return files;
    }
    geometry << buildGeometryScript(request);
    geometry.close();

    std::string command = quoted(resolveExecutable()) + " " +
                          quoted(files.geometry_path) + " -3 -format msh2 -o " +
                          quoted(files.mesh_path) + " -v 2";
#ifdef _WIN32
    command = "\"" + command + "\"";
#endif
    files.gmsh_exit_code = std::system(command.c_str());
    if (files.gmsh_exit_code != 0 || !std::filesystem::exists(files.mesh_path)) {
        files.error_message = "Gmsh failed to create a tetrahedral mesh (exit code " +
                              std::to_string(files.gmsh_exit_code) + ").";
    }
    return files;
}

std::string GmshTetrahedralMesher::buildGeometryScript(const SimulationRequest &request)
{
    const auto &waveguide = request.model.waveguide;
    const auto &mesh = request.settings.fem.mesh;
    const double mesh_size = mesh.maximum_element_size_m > 0.0
                                 ? mesh.maximum_element_size_m
                                 : automaticMeshSize(request);

    std::ostringstream script;
    script << std::setprecision(17);
    script << "SetFactory(\"OpenCASCADE\");\n"
           << "Geometry.OCCBooleanPreserveNumbering = 1;\n"
           << "Geometry.Tolerance = " << mesh.geometry_tolerance_m << ";\n"
           << "Mesh.MshFileVersion = 2.2;\n"
           << "Mesh.CharacteristicLengthMax = " << mesh_size << ";\n";
    if (mesh.minimum_element_size_m > 0.0) {
        script << "Mesh.CharacteristicLengthMin = " << mesh.minimum_element_size_m << ";\n";
    }

    const double wavelength = speed_of_light_m_per_s / request.frequency_hz;
    const double pml_thickness = request.settings.fem.pml.thickness_m > 0.0
                                     ? request.settings.fem.pml.thickness_m
                                     : 0.35 * wavelength;
    const bool has_slots = std::any_of(request.model.slot_geometries.begin(),
                                       request.model.slot_geometries.end(),
                                       [](const SlotGeometry &slot) { return slot.enabled; });
    const double exterior_padding = has_slots && request.settings.fem.pml.enabled
                                        ? 2.0 * pml_thickness
                                        : 0.0;

    if (has_slots && !(waveguide.wall_thickness_m > 0.0)) {
        script << "Error(\"A positive wall thickness is required for slot FEM\");\n";
        return script.str();
    }

    if (has_slots) {
        const double wall = waveguide.wall_thickness_m;
        script << "Box(1) = {" << -0.5 * waveguide.inner_width_m - exterior_padding - wall << ","
               << -0.5 * waveguide.inner_height_m - exterior_padding - wall << ","
               << -0.5 * waveguide.length_m << ","
               << waveguide.inner_width_m + 2.0 * (exterior_padding + wall) << ","
               << waveguide.inner_height_m + 2.0 * (exterior_padding + wall) << ","
               << waveguide.length_m << "};\n"
               << "Box(2) = {" << -0.5 * waveguide.inner_width_m - wall << ","
               << -0.5 * waveguide.inner_height_m - wall << ","
               << -0.5 * waveguide.length_m << ","
               << waveguide.inner_width_m + 2.0 * wall << ","
               << waveguide.inner_height_m + 2.0 * wall << ","
               << waveguide.length_m << "};\n"
               << "Box(3) = {" << -0.5 * waveguide.inner_width_m << ","
               << -0.5 * waveguide.inner_height_m << ","
               << -0.5 * waveguide.length_m << "," << waveguide.inner_width_m << ","
               << waveguide.inner_height_m << "," << waveguide.length_m << "};\n"
               << "metal[] = BooleanDifference{ Volume{2}; Delete; }{ Volume{3}; Delete; };\n";

        std::vector<int> slot_tags;
        int slot_tag = 20;
        for (const SlotGeometry &slot : request.model.slot_geometries) {
            if (slot.enabled) {
                appendSlotCutter(script, slot_tag, slot, waveguide);
                slot_tags.push_back(slot_tag++);
            }
        }
        script << "perforatedMetal[] = BooleanDifference{ Volume{metal[]}; Delete; }{ Volume{";
        for (std::size_t i = 0; i < slot_tags.size(); ++i) {
            script << (i == 0 ? "" : ",") << slot_tags[i];
        }
        script << "}; Delete; };\n"
               << "fluid[] = BooleanDifference{ Volume{1}; Delete; }{ Volume{perforatedMetal[]}; Delete; };\n";
    } else {
        script << "Box(1) = {" << -0.5 * waveguide.inner_width_m << ","
               << -0.5 * waveguide.inner_height_m << ","
               << -0.5 * waveguide.length_m << "," << waveguide.inner_width_m << ","
               << waveguide.inner_height_m << "," << waveguide.length_m << "};\n"
               << "fluid[] = {1};\n";
    }

    MeshRefinementLimits refinement_limits;
    refinement_limits.elements_across_feature = mesh.elements_across_smallest_feature;
    refinement_limits.global_element_size_m = mesh_size;
    refinement_limits.smallest_element_size_m = mesh.minimum_element_size_m;
    refinement_limits.geometry_tolerance_m = std::max(mesh.geometry_tolerance_m, 0.0);
    if (mesh.minimum_size_ratio > 1.0) {
        refinement_limits.smallest_element_size_m =
            std::max(refinement_limits.smallest_element_size_m,
                     mesh_size / mesh.minimum_size_ratio);
    }
    std::vector<MeshRefinementRegion> refinement_regions;

    int next_tag = 100;
    bool has_pec_bodies = false;
    bool has_pec_sheets = false;
    script << "pecBodies[] = {};\npecSheets[] = {};\n";
    for (const PecPlateGeometry &plate : request.model.pec_plates) {
        if (!plate.enabled) {
            continue;
        }
        const bool thin_sheet =
            plateIsThinSheet(plate, wavelength, mesh.sheet_thickness_threshold_m);
        if (plateIsAxisAligned(plate, refinement_limits)) {
            appendPlateRefinementRegions(
                plate, waveguide, thin_sheet,
                sheetSubstituteThickness(wavelength, mesh.sheet_thickness_threshold_m),
                refinement_limits, refinement_regions);
        } else if (refinement_limits.elements_across_feature > 0.0) {
            // Silently dropping the refinement of a rotated plate would leave the
            // user with a result that follows the element size instead of the
            // geometry (|S11| spread 81.6% over four neighbouring sizes on the
            // reference iris) and no hint why. Gmsh prints this at its default
            // verbosity, and the message stays in the .geo file next to the mesh.
            script << "// A rotated plate gets no local mesh refinement: Field[Box] is\n"
                      "// axis aligned and cannot follow the rotation.\n"
                      "Warning(\"Rotated PEC plate: no local mesh refinement is built for "
                      "it, so the result depends on the global element size\");\n";
        }
        if (thin_sheet) {
            appendSheetPlate(script, next_tag, plate);
            has_pec_sheets = true;
            continue;
        }
        const int plate_tag = next_tag++;
        appendRotatedBox(script, plate_tag, plate.center_m, plate.size_m, plate.rotation_rad);
        if (plateHasOpening(plate)) {
            // Window cutter: same orientation as the plate, offset inside its
            // local x/y, and deliberately longer than the plate thickness so the
            // boolean cuts cleanly all the way through.
            const int window_tag = next_tag++;
            const double cut_length = plate.size_m.z * 3.0 + 1.0e-6;
            const Vec3 window_center{plate.center_m.x + plate.aperture_offset_x_m,
                                     plate.center_m.y + plate.aperture_offset_y_m,
                                     plate.center_m.z};
            if (plate.aperture_shape == PlateApertureShape::Circular) {
                const Vec3 base{window_center.x,
                                window_center.y,
                                window_center.z - 0.5 * cut_length};
                appendRotatedCylinder(script,
                                      window_tag,
                                      base,
                                      cut_length,
                                      plate.aperture_radius_m,
                                      plate.rotation_rad,
                                      plate.center_m);
            } else {
                const Vec3 window_size{plate.aperture_width_m,
                                       plate.aperture_height_m,
                                       cut_length};
                appendRotatedBox(script,
                                 window_tag,
                                 window_center,
                                 window_size,
                                 plate.rotation_rad,
                                 plate.center_m);
            }
            script << "iris" << plate_tag << "[] = BooleanDifference{ Volume{" << plate_tag
                   << "}; Delete; }{ Volume{" << window_tag << "}; Delete; };\n";
            if (plateHasPost(plate)) {
                // The stub lies in the plate plane and grows out of its bottom
                // edge, so it is welded back onto the perforated plate rather
                // than added as a separate body.
                const int stub_tag = next_tag++;
                const double bottom_m = plate.center_m.y - 0.5 * plate.size_m.y;
                const Vec3 stub_center{plate.center_m.x + plate.aperture_offset_x_m,
                                       bottom_m + 0.5 * plate.post_height_m,
                                       plate.center_m.z};
                const Vec3 stub_size{plate.post_width_m,
                                     plate.post_height_m,
                                     plate.size_m.z};
                appendRotatedBox(script,
                                 stub_tag,
                                 stub_center,
                                 stub_size,
                                 plate.rotation_rad,
                                 plate.center_m);
                script << "irisStub" << plate_tag << "[] = BooleanUnion{ Volume{iris"
                       << plate_tag << "[]}; Delete; }{ Volume{" << stub_tag
                       << "}; Delete; };\n"
                       << "pecBodies[] += irisStub" << plate_tag << "[];\n";
            } else {
                script << "pecBodies[] += iris" << plate_tag << "[];\n";
            }
        } else {
            script << "pecBodies[] += {" << plate_tag << "};\n";
        }
        has_pec_bodies = true;
    }

    if (has_pec_bodies) {
        script << "fluidAfterPec[] = BooleanDifference{ Volume{fluid[]}; Delete; }"
                  "{ Volume{pecBodies[]}; Delete; };\n";
    } else {
        script << "fluidAfterPec[] = fluid[];\n";
    }

    std::vector<int> dielectric_tags;
    for (const DielectricBlockGeometry &block : request.model.dielectric_blocks) {
        if (!block.enabled) {
            continue;
        }
        appendRotatedBox(script, next_tag, block.center_m, block.size_m, block.rotation_rad);
        dielectric_tags.push_back(next_tag++);
    }
    if (!dielectric_tags.empty()) {
        script << "allVolumes[] = BooleanFragments{ Volume{fluidAfterPec[]}; Delete; }{ Volume{";
        for (std::size_t i = 0; i < dielectric_tags.size(); ++i) {
            script << (i == 0 ? "" : ",") << dielectric_tags[i];
        }
        script << "}; Delete; };\n";
    } else {
        script << "allVolumes[] = fluidAfterPec[];\n";
    }

    const double tolerance = std::max(mesh.geometry_tolerance_m * 1000.0, 1.0e-7);

    // Imprint the zero-thickness sheets into the cavity so the mesh conforms to
    // them, then recover the volumes and the sheet faces by bounding box: the
    // boolean returns a mixed-dimension list that cannot be used directly.
    std::ostringstream sheet_selection;
    if (has_pec_sheets) {
        const double domain_margin = std::max(exterior_padding + waveguide.wall_thickness_m,
                                              tolerance) +
                                     tolerance;
        const double half_x = 0.5 * waveguide.inner_width_m + domain_margin;
        const double half_y = 0.5 * waveguide.inner_height_m + domain_margin;
        const double half_z = 0.5 * waveguide.length_m + tolerance;
        script << "BooleanFragments{ Volume{allVolumes[]}; Delete; }"
                  "{ Surface{pecSheets[]}; Delete; }\n"
               << "allVolumes[] = Volume In BoundingBox{" << -half_x << "," << -half_y << ","
               << -half_z << "," << half_x << "," << half_y << "," << half_z << "};\n"
               << "irisSheets[] = {};\n";
        for (const PecPlateGeometry &plate : request.model.pec_plates) {
            if (!plate.enabled || !plateIsThinSheet(plate, wavelength, mesh.sheet_thickness_threshold_m)) {
                continue;
            }
            script << "irisSheets[] += Surface In BoundingBox{"
                   << plate.center_m.x - 0.5 * plate.size_m.x - tolerance << ","
                   << plate.center_m.y - 0.5 * plate.size_m.y - tolerance << ","
                   << plate.center_m.z - tolerance << ","
                   << plate.center_m.x + 0.5 * plate.size_m.x + tolerance << ","
                   << plate.center_m.y + 0.5 * plate.size_m.y + tolerance << ","
                   << plate.center_m.z + tolerance << "};\n";
        }
        sheet_selection << ", irisSheets[]";
    }

    script << "backgroundVolumes[] = allVolumes[];\n";
    for (std::size_t i = 0; i < dielectric_tags.size(); ++i) {
        script << "backgroundVolumes[] -= {" << dielectric_tags[i] << "};\n"
               << "Physical Volume(" << i + 2 << ") = {" << dielectric_tags[i] << "};\n";
    }
    script << "Physical Volume(1) = {backgroundVolumes[]};\n"
           << "portIn[] = Surface In BoundingBox{" << -0.5 * waveguide.inner_width_m - tolerance
           << "," << -0.5 * waveguide.inner_height_m - tolerance << ","
           << -0.5 * waveguide.length_m - tolerance << ","
           << 0.5 * waveguide.inner_width_m + tolerance << ","
           << 0.5 * waveguide.inner_height_m + tolerance << ","
           << -0.5 * waveguide.length_m + tolerance << "};\n"
           << "portOut[] = Surface In BoundingBox{" << -0.5 * waveguide.inner_width_m - tolerance
           << "," << -0.5 * waveguide.inner_height_m - tolerance << ","
           << 0.5 * waveguide.length_m - tolerance << "," << 0.5 * waveguide.inner_width_m + tolerance
           << "," << 0.5 * waveguide.inner_height_m + tolerance << ","
           << 0.5 * waveguide.length_m + tolerance << "};\n"
           << "allBoundary[] = CombinedBoundary{ Volume{allVolumes[]}; };\n"
           << "pecBoundary[] = Abs(allBoundary[]);\n"
           << "pecBoundary[] -= {portIn[]};\n"
           << "pecBoundary[] -= {portOut[]};\n"
           << "Physical Surface(103) = {pecBoundary[]" << sheet_selection.str() << "};\n"
           << "Physical Surface(101) = {portIn[]};\n"
           << "Physical Surface(102) = {portOut[]};\n";
    appendMeshSizeFields(script, refinement_regions, mesh_size,
                         refinement_limits.smallest_element_size_m);
    script << "Mesh 3;\n";
    return script.str();
}
}
