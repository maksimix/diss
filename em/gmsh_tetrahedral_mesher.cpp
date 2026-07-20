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
    return std::min({waveguide.inner_width_m / 8.0,
                     waveguide.inner_height_m / 5.0,
                     wavelength / 12.0});
}

void appendRotatedBox(std::ostringstream &script,
                      int tag,
                      const Vec3 &center,
                      const Vec3 &size,
                      const Vec3 &rotation)
{
    script << "Box(" << tag << ") = {"
           << center.x - 0.5 * size.x << ","
           << center.y - 0.5 * size.y << ","
           << center.z - 0.5 * size.z << ","
           << size.x << "," << size.y << "," << size.z << "};\n";
    if (rotation.x != 0.0) {
        script << "Rotate {{1,0,0},{" << center.x << "," << center.y << ","
               << center.z << "}," << rotation.x << "} { Volume{" << tag << "}; }\n";
    }
    if (rotation.y != 0.0) {
        script << "Rotate {{0,1,0},{" << center.x << "," << center.y << ","
               << center.z << "}," << rotation.y << "} { Volume{" << tag << "}; }\n";
    }
    if (rotation.z != 0.0) {
        script << "Rotate {{0,0,1},{" << center.x << "," << center.y << ","
               << center.z << "}," << rotation.z << "} { Volume{" << tag << "}; }\n";
    }
}

void appendSlotCutter(std::ostringstream &script,
                      int tag,
                      const SlotGeometry &slot,
                      const RectangularWaveguideGeometry &waveguide)
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
    std::vector<int> pec_tags;
    for (const PecPlateGeometry &plate : request.model.pec_plates) {
        if (!plate.enabled) {
            continue;
        }
        appendRotatedBox(script, next_tag, plate.center_m, plate.size_m, plate.rotation_rad);
        pec_tags.push_back(next_tag++);
    }

    if (!pec_tags.empty()) {
        script << "fluidAfterPec[] = BooleanDifference{ Volume{fluid[]}; Delete; }{ Volume{";
        for (std::size_t i = 0; i < pec_tags.size(); ++i) {
            script << (i == 0 ? "" : ",") << pec_tags[i];
        }
        script << "}; Delete; };\n";
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
