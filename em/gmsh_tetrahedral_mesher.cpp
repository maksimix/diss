#include "gmsh_tetrahedral_mesher.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace em
{
namespace
{
std::string quoted(const std::filesystem::path &path)
{
    return "\"" + path.string() + "\"";
}

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
    const double factor = std::clamp(request.settings.fem.mesh.refinement_factor, 0.2, 4.0);
    return base_size_m * factor;
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
        return result;
    }
#else
    if (const char *gmsh_path = std::getenv("GMSH_EXECUTABLE")) {
        return gmsh_path;
    }
#endif
#ifdef _WIN32
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

    int next_tag = 100;
    bool has_pec_bodies = false;
    script << "pecBodies[] = {};\n";
    for (const PecPlateGeometry &plate : request.model.pec_plates) {
        if (!plate.enabled) {
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
           << "Physical Surface(103) = {pecBoundary[]};\n"
           << "Physical Surface(101) = {portIn[]};\n"
           << "Physical Surface(102) = {portOut[]};\n"
           << "Mesh 3;\n";
    return script.str();
}
}
