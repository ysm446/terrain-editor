#include "obj_exporter.h"

#include <fstream>

namespace rock
{
bool ExportMeshObj(const MeshData& mesh, const std::filesystem::path& path, std::string* errorMessage)
{
    if (mesh.vertices.empty() || mesh.triangles.empty())
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = "No mesh topology to export. Evaluate the graph first.";
        }
        return false;
    }

    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec)
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = "Failed to create export directory: " + ec.message();
        }
        return false;
    }

    std::ofstream file(path);
    if (!file)
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = "Failed to open OBJ file for writing.";
        }
        return false;
    }

    file << "# Terrain Editor OBJ\n";
    file << "# Indexed terrain mesh topology\n";
    file << "o terrain_mesh\n";

    for (const MeshVertex& vertex : mesh.vertices)
    {
        file << "v " << vertex.x << ' ' << vertex.y << ' ' << vertex.z << '\n';
    }
    for (const MeshVertex& vertex : mesh.vertices)
    {
        file << "vn " << vertex.nx << ' ' << vertex.ny << ' ' << vertex.nz << '\n';
    }

    file << "s 1\n";
    for (const MeshTriangle& triangle : mesh.triangles)
    {
        const uint32_t a = triangle.a + 1;
        const uint32_t b = triangle.b + 1;
        const uint32_t c = triangle.c + 1;
        file << "f " << a << "//" << a << ' ' << b << "//" << b << ' ' << c << "//" << c << '\n';
    }

    if (!file)
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = "Failed while writing OBJ file.";
        }
        return false;
    }

    if (errorMessage != nullptr)
    {
        errorMessage->clear();
    }
    return true;
}

} // namespace rock
