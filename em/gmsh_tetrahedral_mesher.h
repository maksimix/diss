#pragma once

#include "em_model.h"

#include <filesystem>
#include <string>

namespace em
{
struct FemMeshFiles
{
    std::filesystem::path geometry_path;
    std::filesystem::path mesh_path;
    int gmsh_exit_code = -1;
    std::string error_message;
};

class GmshTetrahedralMesher
{
public:
    explicit GmshTetrahedralMesher(std::filesystem::path gmsh_executable = {});

    FemMeshFiles generate(const SimulationRequest &request,
                          const std::filesystem::path &working_directory) const;

    static std::string buildGeometryScript(const SimulationRequest &request);

private:
    std::filesystem::path resolveExecutable() const;

    std::filesystem::path gmsh_executable_;
};
}
