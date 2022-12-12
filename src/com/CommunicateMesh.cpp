#include <Eigen/Core>
#include <algorithm>
#include <boost/container/flat_map.hpp>
#include <cstddef>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <ostream>
#include <utility>
#include <vector>

#include "CommunicateMesh.hpp"
#include "Communication.hpp"
#include "com/SharedPointer.hpp"
#include "logging/LogMacros.hpp"
#include "mesh/Edge.hpp"
#include "mesh/Mesh.hpp"
#include "mesh/Vertex.hpp"
#include "precice/types.hpp"
#include "utils/assertion.hpp"

namespace precice::com {
CommunicateMesh::CommunicateMesh(
    com::PtrCommunication communication)
    : _communication(std::move(communication))
{
}

namespace {

class SerializedMesh {
public:
  void assertValid() const
  {
    PRECICE_ASSERT(sizes.size() == 5);
    auto dim = sizes[0];
    PRECICE_ASSERT(0 < dim && dim <= 3);
    auto nVertices = sizes[1];
    PRECICE_ASSERT(0 <= nVertices);

    auto nEdges      = sizes[2];
    auto nTriangles  = sizes[3];
    auto nTetrahedra = sizes[4];

    if (nVertices == 0) {
      PRECICE_ASSERT(nEdges == 0);
      PRECICE_ASSERT(nTriangles == 0);
      PRECICE_ASSERT(nTetrahedra == 0);
      PRECICE_ASSERT(ids.empty());
      PRECICE_ASSERT(coords.empty());
      return;
    }
    PRECICE_ASSERT(static_cast<std::size_t>(nVertices * dim) == coords.size());
    bool hasConnectivity = (nEdges + nTriangles + nTetrahedra) > 0;
    // Global IDs are allowed to have duplicates as they may not be initialized
    if (hasConnectivity) {
      PRECICE_ASSERT(ids.size() == static_cast<std::size_t>(2 * nVertices + 2 * nEdges + 3 * nTriangles + 4 * nTetrahedra));
      std::set<int> validIDs;
      for (int vertex = 0; vertex < nVertices; ++vertex) {
        PRECICE_ASSERT(validIDs.count(ids[2 * vertex + 1]) == 0, "Duplicate IDs");
        validIDs.insert(ids[2 * vertex + 1]);
      }
      for (std::size_t idx = 2 * nVertices; idx < ids.size(); ++idx) {
        PRECICE_ASSERT(validIDs.count(ids[idx]) == 1, "Unknown ID");
      }
    } else {
      PRECICE_ASSERT(ids.size() == static_cast<std::size_t>(nVertices));
    }
  }

  void sendTo(Communication &communication, int rankReceiver)
  {
    communication.sendRange(sizes, rankReceiver);
    if (sizes[1] > 0) {
      communication.sendRange(coords, rankReceiver);
      communication.sendRange(ids, rankReceiver);
    }
  }

  static SerializedMesh receiveFrom(Communication &communication, int rankSender)
  {
    SerializedMesh sm;
    sm.sizes = communication.receiveRange(rankSender, AsVectorTag<int>{});
    PRECICE_ASSERT(sm.sizes.size() == 5);
    auto nVertices = sm.sizes[1];
    if (nVertices > 0) {
      sm.coords = communication.receiveRange(rankSender, AsVectorTag<double>{});
      sm.ids    = communication.receiveRange(rankSender, AsVectorTag<int>{});
    }
    sm.assertValid();
    return sm;
  }

  void broadcastSend(Communication &communication)
  {
    communication.broadcast(sizes);
    if (sizes[1] > 0) {
      communication.broadcast(coords);
      communication.broadcast(ids);
    }
  }

  static SerializedMesh broadcastReceive(Communication &communication)
  {
    constexpr int  broadcasterRank{0};
    SerializedMesh sm;
    communication.broadcast(sm.sizes, broadcasterRank);
    PRECICE_ASSERT(sm.sizes.size() == 5);
    auto nVertices = sm.sizes[1];
    if (nVertices > 0) {
      communication.broadcast(sm.coords, broadcasterRank);
      communication.broadcast(sm.ids, broadcasterRank);
    }
    sm.assertValid();
    return sm;
  }

  void addToMesh(mesh::Mesh &mesh) const
  {
    PRECICE_ASSERT(sizes[0] == mesh.getDimensions());

    const auto numberOfVertices = sizes[1];
    if (numberOfVertices == 0) {
      return;
    }
    const auto numberOfEdges      = sizes[2];
    const auto numberOfTriangles  = sizes[3];
    const auto numberOfTetrahedra = sizes[4];

    const bool hasConnectivity = (numberOfEdges + numberOfTriangles + numberOfTetrahedra) > 0;

    const auto dim = mesh.getDimensions();

    std::map<int, mesh::Vertex *> vertices;
    {
      Eigen::VectorXd coord(dim);
      for (int i = 0; i < numberOfVertices; i++) {
        std::copy_n(&coords[i * dim], dim, coord.data());
        mesh::Vertex &v = mesh.createVertex(coord);

        if (hasConnectivity) {
          v.setGlobalIndex(ids[i * 2]);
          vertices.emplace(ids[i * 2 + 1], &v);
        } else {
          v.setGlobalIndex(ids[i]);
        }
      }
    }

    if (!hasConnectivity) {
      return;
    }

    const size_t offsetEdge        = numberOfVertices * 2;
    const size_t offsetTriangle    = offsetEdge + 2 * numberOfEdges;
    const size_t offsetTetrahedron = offsetTriangle + 3 * numberOfTriangles;

    for (size_t idx = offsetEdge; idx != offsetTriangle; idx += 2) {
      mesh.createEdge(
          *vertices.at(ids[idx]),
          *vertices.at(ids[idx + 1]));
    }
    for (size_t idx = offsetTriangle; idx != offsetTetrahedron; idx += 3) {
      mesh.createTriangle(
          *vertices.at(ids[idx]),
          *vertices.at(ids[idx + 1]),
          *vertices.at(ids[idx + 2]));
    }
    for (size_t idx = offsetTetrahedron; idx != ids.size(); idx += 4) {
      mesh.createTetrahedron(
          *vertices.at(ids[idx]),
          *vertices.at(ids[idx + 1]),
          *vertices.at(ids[idx + 2]),
          *vertices.at(ids[idx + 3]));
    }
  }

  static SerializedMesh serialize(const mesh::Mesh &mesh)
  {
    const auto &meshVertices   = mesh.vertices();
    const auto &meshEdges      = mesh.edges();
    const auto &meshTriangles  = mesh.triangles();
    const auto &meshTetrahedra = mesh.tetrahedra();

    const auto numberOfVertices   = meshVertices.size();
    const auto numberOfEdges      = meshEdges.size();
    const auto numberOfTriangles  = meshTriangles.size();
    const auto numberOfTetrahedra = meshTetrahedra.size();

    SerializedMesh result;

    result.sizes = {
        mesh.getDimensions(),
        static_cast<int>(numberOfVertices),
        static_cast<int>(numberOfEdges),
        static_cast<int>(numberOfTriangles),
        static_cast<int>(numberOfTetrahedra)};

    // Empty mesh
    if (numberOfVertices == 0) {
      return result;
    }

    auto dim = static_cast<size_t>(mesh.getDimensions());

    // we always need to send globalIDs
    auto       totalIDs        = numberOfVertices;
    const bool hasConnectivity = mesh.hasConnectivity();
    if (hasConnectivity) {
      totalIDs += numberOfVertices          // ids for reconstruction, then connectivity
                  + numberOfEdges * 2       // 2 vertices per edge
                  + numberOfTriangles * 3   // 3 vertices per triangle
                  + numberOfTetrahedra * 4; // 4 vertices per tetrahedron
    }
    result.ids.reserve(totalIDs);

    result.coords.resize(numberOfVertices * dim);
    for (size_t i = 0; i < numberOfVertices; ++i) {
      auto &v = meshVertices[i];
      std::copy_n(v.rawCoords().begin(), dim, &result.coords[i * dim]);
      result.ids.push_back(v.getGlobalIndex());
      // local ids are only interleaved if required
      if (hasConnectivity) {
        result.ids.push_back(v.getID());
      }
    }

    // Mesh without connectivity information
    if (!hasConnectivity) {
      PRECICE_ASSERT(result.ids.size() == numberOfVertices);
      return result;
    }

    auto pushVID = [&result](const auto &element, auto... id) {
      (result.ids.push_back(element.vertex(id).getID()), ...);
    };

    for (const auto &e : meshEdges) {
      pushVID(e, 0, 1);
    }

    for (const auto &e : meshTriangles) {
      pushVID(e, 0, 1, 2);
    }

    for (const auto &e : meshTetrahedra) {
      pushVID(e, 0, 1, 2, 3);
    }

    result.assertValid();

    // Mesh with connectivity information
    return result;
  }

private:
  SerializedMesh() = default;

  /// contains the dimension, followed by the numbers of vertices, edges, triangles, and tetrahedra
  std::vector<int> sizes;

  /// sizes[0] * dimension coordinates for vertices
  std::vector<double> coords;

  // if no connectivity ( sum(sizes[1-3]) == 0)
  // then contains sizes[0] global IDs
  // else contains sizes[0] pairs of (global id, local id)
  //      followed by sizes[1] pairs of local ids defining edges
  //      followed by sizes[2] triples of local ids defining triangles
  //      followed by sizes[3] quadruples of local ids defining tetrahedra
  std::vector<int> ids;
};

SerializedMesh serialize(const mesh::Mesh &mesh)
{
  return SerializedMesh::serialize(mesh);
}

} // namespace

void CommunicateMesh::sendMesh(
    const mesh::Mesh &mesh,
    int               rankReceiver)
{
  PRECICE_TRACE(mesh.getName(), rankReceiver);

  serialize(mesh).sendTo(*_communication, rankReceiver);
}

void CommunicateMesh::receiveMesh(
    mesh::Mesh &mesh,
    int         rankSender)
{
  PRECICE_TRACE(mesh.getName(), rankSender);
  SerializedMesh::receiveFrom(*_communication, rankSender).addToMesh(mesh);
}

void CommunicateMesh::broadcastSendMesh(const mesh::Mesh &mesh)
{
  PRECICE_TRACE(mesh.getName());

  serialize(mesh).broadcastSend(*_communication);
}

void CommunicateMesh::broadcastReceiveMesh(
    mesh::Mesh &mesh)
{
  PRECICE_TRACE(mesh.getName());
  SerializedMesh::broadcastReceive(*_communication).addToMesh(mesh);
}

} // namespace precice::com
