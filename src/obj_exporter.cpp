#include "obj_exporter.h"

#include <fstream>
#include <unordered_map>
#include <vector>

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
    file << "# Indexed terrain mesh topology (top surface only)\n";
    file << "o terrain_mesh\n";

    const auto isExportSurfaceVertex = [](const MeshVertex& vertex) {
        const bool wallSentinel = vertex.mask > 1.5f;
        const bool bottomFace = vertex.ny < -0.5f;
        return !wallSentinel && !bottomFace;
    };

    std::vector<uint32_t> exportedVertexIndices;
    std::unordered_map<uint32_t, uint32_t> objIndexByMeshIndex;
    exportedVertexIndices.reserve(mesh.vertices.size());

    for (const MeshTriangle& triangle : mesh.triangles)
    {
        const uint32_t indices[3] = {triangle.a, triangle.b, triangle.c};
        bool exportTriangle = true;
        for (const uint32_t index : indices)
        {
            if (index >= mesh.vertices.size() || !isExportSurfaceVertex(mesh.vertices[index]))
            {
                exportTriangle = false;
                break;
            }
        }
        if (!exportTriangle)
        {
            continue;
        }

        for (const uint32_t index : indices)
        {
            if (!objIndexByMeshIndex.contains(index))
            {
                const uint32_t objIndex = static_cast<uint32_t>(exportedVertexIndices.size() + 1u);
                objIndexByMeshIndex.emplace(index, objIndex);
                exportedVertexIndices.push_back(index);
            }
        }
    }

    if (exportedVertexIndices.empty())
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = "No exportable top surface triangles found.";
        }
        return false;
    }

    for (const uint32_t index : exportedVertexIndices)
    {
        const MeshVertex& vertex = mesh.vertices[index];
        file << "v " << vertex.x << ' ' << vertex.y << ' ' << vertex.z << '\n';
    }
    for (const uint32_t index : exportedVertexIndices)
    {
        const MeshVertex& vertex = mesh.vertices[index];
        file << "vn " << vertex.nx << ' ' << vertex.ny << ' ' << vertex.nz << '\n';
    }

    file << "s 1\n";
    for (const MeshTriangle& triangle : mesh.triangles)
    {
        const auto aIt = objIndexByMeshIndex.find(triangle.a);
        const auto bIt = objIndexByMeshIndex.find(triangle.b);
        const auto cIt = objIndexByMeshIndex.find(triangle.c);
        if (aIt == objIndexByMeshIndex.end() ||
            bIt == objIndexByMeshIndex.end() ||
            cIt == objIndexByMeshIndex.end())
        {
            continue;
        }

        const uint32_t a = aIt->second;
        const uint32_t b = bIt->second;
        const uint32_t c = cIt->second;
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
