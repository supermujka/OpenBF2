#include "NavMesh.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <queue>
#include <sys/stat.h>
#include <unordered_map>

namespace bf2bot {

const uint32_t NavMesh::INVALID_POLYGON;

namespace {

inline uint8_t readU8(const uint8_t* p) {
    return p[0];
}

inline uint16_t readU16LE(const uint8_t* p) {
    return static_cast<uint16_t>(p[0]) |
           (static_cast<uint16_t>(p[1]) << 8);
}

inline uint32_t readU32LE(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

inline int32_t readI32LE(const uint8_t* p) {
    uint32_t u = readU32LE(p);
    int32_t v = 0;
    std::memcpy(&v, &u, sizeof(v));
    return v;
}

inline float readF32LE(const uint8_t* p) {
    uint32_t u = readU32LE(p);
    float f = 0.0f;
    std::memcpy(&f, &u, sizeof(f));
    return f;
}

bool readFileBytes(const std::string& path, std::vector<uint8_t>& outBytes) {
    std::ifstream ifs(path.c_str(), std::ios::binary | std::ios::ate);
    if (!ifs.is_open()) {
        return false;
    }
    const std::ifstream::pos_type endPos = ifs.tellg();
    if (endPos <= 0) {
        return false;
    }
    const size_t size = static_cast<size_t>(endPos);
    outBytes.resize(size);
    ifs.seekg(0, std::ios::beg);
    if (!ifs.read(reinterpret_cast<char*>(&outBytes[0]), static_cast<std::streamsize>(size))) {
        outBytes.clear();
        return false;
    }
    return true;
}

bool isDirectoryPath(const std::string& path) {
#ifdef _WIN32
    struct _stat st;
    if (_stat(path.c_str(), &st) != 0) {
        return false;
    }
    return (st.st_mode & _S_IFDIR) != 0;
#else
    struct stat st;
    if (stat(path.c_str(), &st) != 0) {
        return false;
    }
    return S_ISDIR(st.st_mode);
#endif
}

std::string joinPath(const std::string& dir, const std::string& name) {
    if (dir.empty()) {
        return name;
    }
    const char last = dir[dir.size() - 1];
    if (last == '/' || last == '\\') {
        return dir + name;
    }
    return dir + "/" + name;
}

std::string parentDirectory(const std::string& filePath) {
    const size_t pos = filePath.find_last_of("/\\");
    if (pos == std::string::npos) {
        return std::string();
    }
    return filePath.substr(0, pos);
}

float dist3D(const NavPoint& a, const NavPoint& b) {
    const float dx = a.x - b.x;
    const float dy = a.y - b.y;
    const float dz = a.z - b.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

float computeMaterialCost(uint8_t material) {
    // Matches BF2 MaterialManager.cpp: 0 = Invalid (-1.0), 5/7 = Avoid (1000.0), others = 1.0.
    if (material == 0u) {
        return -1.0f;
    }
    if (material == 5u || material == 7u) {
        return 1000.0f;
    }
    return 1.0f;
}

struct AStarNodeState {
    uint32_t poly;
    uint32_t parent;
    uint16_t parentEdge;
    float g;
    bool closed;
};

struct OpenQueueEntry {
    float f;
    float g;
    uint32_t poly;
};

struct OpenQueueGreater {
    bool operator()(const OpenQueueEntry& a, const OpenQueueEntry& b) const {
        if (a.f != b.f) {
            return a.f > b.f;
        }
        return a.g < b.g;
    }
};

} // namespace

NavMesh::NavMesh()
    : m_loaded(false)
    , m_minX(0.0f)
    , m_minZ(0.0f)
    , m_maxX(0.0f)
    , m_maxZ(0.0f)
    , m_size(0.0f)
    , m_ahmLoaded(false)
    , m_ahmCellSize(16.0f)
    , m_ahmWidth(0)
    , m_ahmHeight(0)
    , m_ahmOriginX(0.0f)
    , m_ahmOriginZ(0.0f) {
}

void NavMesh::clear() {
    m_loaded = false;
    m_minX = 0.0f;
    m_minZ = 0.0f;
    m_maxX = 0.0f;
    m_maxZ = 0.0f;
    m_size = 0.0f;
    m_nodes.clear();
    m_polygons.clear();
    m_nodePolygons.clear();
    m_vertices.clear();
    m_vertexIndices.clear();
    m_neighbours.clear();

    m_ahmLoaded = false;
    m_ahmCellSize = 16.0f;
    m_ahmWidth = 0;
    m_ahmHeight = 0;
    m_ahmOriginX = 0.0f;
    m_ahmOriginZ = 0.0f;
    m_ahmHeights.clear();
}

bool NavMesh::load(const std::string& path) {
    clear();
    if (path.empty()) {
        return false;
    }

    std::string qtrPath = path;
    std::string dirPath;
    if (isDirectoryPath(path)) {
        dirPath = path;
        qtrPath = joinPath(path, "Infantry.qtr");
    } else {
        dirPath = parentDirectory(path);
    }

    if (!loadQtrFile(qtrPath)) {
        clear();
        return false;
    }

    // AerialHeighMap.ahm is optional; note BF2 spelling missing 't'.
    if (!dirPath.empty()) {
        loadAhmFile(joinPath(dirPath, "AerialHeighMap.ahm"));
    } else {
        loadAhmFile("AerialHeighMap.ahm");
    }

    return true;
}

bool NavMesh::loadQtrFile(const std::string& qtrPath) {
    std::vector<uint8_t> data;
    if (!readFileBytes(qtrPath, data)) {
        return false;
    }

    size_t offset = 0;
    const size_t totalSize = data.size();

    if (offset + 8u > totalSize) {
        return false;
    }
    const int32_t numberOfIslands = readI32LE(&data[offset]);
    offset += 4u;
    if (numberOfIslands < 1 || numberOfIslands > 64) {
        return false;
    }

    // Load island 0 (BF2 standard maps use 1 island).
    const int32_t version = readI32LE(&data[offset]);
    offset += 4u;
    if (version != 3) {
        return false;
    }

    if (offset + 20u > totalSize) {
        return false;
    }
    m_minX = readF32LE(&data[offset]);
    m_minZ = readF32LE(&data[offset + 4u]);
    m_maxX = readF32LE(&data[offset + 8u]);
    m_maxZ = readF32LE(&data[offset + 12u]);
    m_size = readF32LE(&data[offset + 16u]);
    offset += 20u;

    // Nodes: uint32 count + N * 24B
    if (offset + 4u > totalSize) {
        return false;
    }
    const uint32_t numNodes = readU32LE(&data[offset]);
    offset += 4u;
    if (numNodes == 0u || static_cast<size_t>(numNodes) > (totalSize - offset) / 24u) {
        return false;
    }
    m_nodes.resize(numNodes);
    for (uint32_t i = 0; i < numNodes; ++i) {
        const uint8_t* p = &data[offset + static_cast<size_t>(i) * 24u];
        QuadNode& node = m_nodes[i];
        node.children[0] = readU32LE(p + 0u);
        node.children[1] = readU32LE(p + 4u);
        node.children[2] = readU32LE(p + 8u);
        node.children[3] = readU32LE(p + 12u);
        node.isLeaf = readU8(p + 16u);
        node.polygonCount = readU8(p + 17u);
        node.firstPolygon = readU32LE(p + 20u);
    }
    offset += static_cast<size_t>(numNodes) * 24u;

    // Polygons: uint32 count + P * 20B
    if (offset + 4u > totalSize) {
        return false;
    }
    const uint32_t numPolygons = readU32LE(&data[offset]);
    offset += 4u;
    if (numPolygons == 0u || static_cast<size_t>(numPolygons) > (totalSize - offset) / 20u) {
        return false;
    }
    m_polygons.resize(numPolygons);
    for (uint32_t i = 0; i < numPolygons; ++i) {
        const uint8_t* p = &data[offset + static_cast<size_t>(i) * 20u];
        NavPolygon& poly = m_polygons[i];
        poly.numVertices = readU8(p + 0u);
        poly.material = readU8(p + 1u);
        poly.cluster = readU16LE(p + 2u);
        poly.firstIndex = readU32LE(p + 4u);
        poly.normal.x = readF32LE(p + 8u);
        poly.normal.y = readF32LE(p + 12u);
        poly.normal.z = readF32LE(p + 16u);
        poly.centroid.x = 0.0f;
        poly.centroid.y = 0.0f;
        poly.centroid.z = 0.0f;
        poly.cost = -1.0f;
    }
    offset += static_cast<size_t>(numPolygons) * 20u;

    // NodePolygons: uint32 count + NP * 4B
    if (offset + 4u > totalSize) {
        return false;
    }
    const uint32_t numNodePolygons = readU32LE(&data[offset]);
    offset += 4u;
    if (static_cast<size_t>(numNodePolygons) > (totalSize - offset) / 4u) {
        return false;
    }
    m_nodePolygons.resize(numNodePolygons);
    for (uint32_t i = 0; i < numNodePolygons; ++i) {
        m_nodePolygons[i] = readU32LE(&data[offset + static_cast<size_t>(i) * 4u]);
    }
    offset += static_cast<size_t>(numNodePolygons) * 4u;

    // Vertices: uint32 count + V * 12B
    if (offset + 4u > totalSize) {
        return false;
    }
    const uint32_t numVertices = readU32LE(&data[offset]);
    offset += 4u;
    if (numVertices < 3u || static_cast<size_t>(numVertices) > (totalSize - offset) / 12u) {
        return false;
    }
    m_vertices.resize(numVertices);
    for (uint32_t i = 0; i < numVertices; ++i) {
        const uint8_t* p = &data[offset + static_cast<size_t>(i) * 12u];
        m_vertices[i].x = readF32LE(p + 0u);
        m_vertices[i].y = readF32LE(p + 4u);
        m_vertices[i].z = readF32LE(p + 8u);
    }
    offset += static_cast<size_t>(numVertices) * 12u;

    // VertexIndices: uint32 count + VI * 4B
    if (offset + 4u > totalSize) {
        return false;
    }
    const uint32_t numVertexIndices = readU32LE(&data[offset]);
    offset += 4u;
    if (static_cast<size_t>(numVertexIndices) > (totalSize - offset) / 4u) {
        return false;
    }
    m_vertexIndices.resize(numVertexIndices);
    for (uint32_t i = 0; i < numVertexIndices; ++i) {
        const uint32_t vIdx = readU32LE(&data[offset + static_cast<size_t>(i) * 4u]);
        if (vIdx >= numVertices) {
            return false;
        }
        m_vertexIndices[i] = vIdx;
    }
    offset += static_cast<size_t>(numVertexIndices) * 4u;

    // Neighbours: uint32 count + N * 4B (parallel to vertexIndices; 0xFFFFFFFF = boundary)
    if (offset + 4u > totalSize) {
        return false;
    }
    const uint32_t numNeighbours = readU32LE(&data[offset]);
    offset += 4u;
    if (numNeighbours != numVertexIndices ||
        static_cast<size_t>(numNeighbours) > (totalSize - offset) / 4u) {
        return false;
    }
    m_neighbours.resize(numNeighbours);
    for (uint32_t i = 0; i < numNeighbours; ++i) {
        const uint32_t nbr = readU32LE(&data[offset + static_cast<size_t>(i) * 4u]);
        if (nbr != INVALID_POLYGON && nbr >= numPolygons) {
            return false;
        }
        m_neighbours[i] = nbr;
    }

    // Validate quadtree node indices and leaf polygon slices.
    for (uint32_t i = 0; i < numNodes; ++i) {
        const QuadNode& node = m_nodes[i];
        if (!node.isLeaf) {
            for (int c = 0; c < 4; ++c) {
                if (node.children[c] >= numNodes) {
                    return false;
                }
            }
        } else {
            const uint64_t endIdx = static_cast<uint64_t>(node.firstPolygon) +
                                    static_cast<uint64_t>(node.polygonCount);
            if (endIdx > static_cast<uint64_t>(numNodePolygons)) {
                return false;
            }
            for (uint32_t j = 0; j < node.polygonCount; ++j) {
                if (m_nodePolygons[node.firstPolygon + j] >= numPolygons) {
                    return false;
                }
            }
        }
    }

    // Validate polygons and precompute centroid and walkability cost.
    for (uint32_t i = 0; i < numPolygons; ++i) {
        NavPolygon& poly = m_polygons[i];
        const uint64_t endIdx = static_cast<uint64_t>(poly.firstIndex) +
                                static_cast<uint64_t>(poly.numVertices);
        if (endIdx > static_cast<uint64_t>(numVertexIndices)) {
            return false;
        }

        if (poly.numVertices > 0u) {
            float sx = 0.0f;
            float sy = 0.0f;
            float sz = 0.0f;
            for (uint8_t k = 0; k < poly.numVertices; ++k) {
                const NavPoint& v = m_vertices[m_vertexIndices[poly.firstIndex + k]];
                sx += v.x;
                sy += v.y;
                sz += v.z;
            }
            const float inv = 1.0f / static_cast<float>(poly.numVertices);
            poly.centroid.x = sx * inv;
            poly.centroid.y = sy * inv;
            poly.centroid.z = sz * inv;
        }

        if (poly.numVertices < 3u || !(poly.normal.y > 0.0f)) {
            poly.cost = -1.0f;
        } else {
            poly.cost = computeMaterialCost(poly.material);
        }
    }

    m_loaded = true;
    return true;
}

bool NavMesh::loadAhmFile(const std::string& ahmPath) {
    std::vector<uint8_t> data;
    if (!readFileBytes(ahmPath, data)) {
        return false;
    }
    if (data.size() < 20u) {
        return false;
    }

    const float cellSize = readF32LE(&data[0]);
    const int32_t width = readI32LE(&data[4]);
    const int32_t height = readI32LE(&data[8]);
    const float originX = readF32LE(&data[12]);
    const float originZ = readF32LE(&data[16]);

    if (cellSize <= 0.0f || width <= 0 || height <= 0) {
        return false;
    }
    const uint64_t count = static_cast<uint64_t>(width) * static_cast<uint64_t>(height);
    if (count > (data.size() - 20u) / 2u) {
        return false;
    }

    m_ahmCellSize = cellSize;
    m_ahmWidth = width;
    m_ahmHeight = height;
    m_ahmOriginX = originX;
    m_ahmOriginZ = originZ;
    m_ahmHeights.resize(static_cast<size_t>(count));
    for (size_t i = 0; i < static_cast<size_t>(count); ++i) {
        m_ahmHeights[i] = readU16LE(&data[20u + i * 2u]);
    }
    m_ahmLoaded = true;
    return true;
}

bool NavMesh::pointWithinPolygonEdges(const NavPolygon& poly, float x, float z) const {
    if (poly.numVertices == 0u) {
        return false;
    }
    for (uint8_t edge = 0; edge < poly.numVertices; ++edge) {
        const uint8_t next = static_cast<uint8_t>((edge + 1u) % poly.numVertices);
        const NavPoint& first = m_vertices[m_vertexIndices[poly.firstIndex + edge]];
        const NavPoint& second = m_vertices[m_vertexIndices[poly.firstIndex + next]];
        const float cross = (second.z - first.z) * (x - first.x) +
                            (z - first.z) * -(second.x - first.x);
        if (cross < -0.001f) {
            return false;
        }
    }
    return true;
}

bool NavMesh::withinPolygon(const NavPolygon& poly, float x, float y, float z,
                            float& outDistSq) const {
    if (poly.numVertices < 3u || std::fabs(poly.normal.y) <= 1e-6f) {
        return false;
    }

    const NavPoint& first = m_vertices[m_vertexIndices[poly.firstIndex]];
    const float iy = first.y - (poly.normal.x * (x - first.x) +
                                poly.normal.z * (z - first.z)) / poly.normal.y;

    if (!pointWithinPolygonEdges(poly, x, z)) {
        return false;
    }

    const float dy = y - iy;
    outDistSq = dy * dy;

    // BF2 QuadTree.cpp: 2.5f allowed above intersection, 1.0f below.
    const float limit = (y > iy) ? 2.5f : 1.0f;
    return outDistSq <= (limit * limit);
}

uint32_t NavMesh::findPolygonInNode(uint32_t nodeIdx, float minX, float minZ, float size,
                                    float x, float y, float z) const {
    float curMinX = minX;
    float curMinZ = minZ;
    float curSize = size;
    uint32_t curIdx = nodeIdx;

    // Iterative QuadTree descent matching BF2 QuadTreeNode.cpp.
    for (int depth = 0; depth < 64; ++depth) {
        if (curIdx >= m_nodes.size()) {
            return INVALID_POLYGON;
        }
        const QuadNode& node = m_nodes[curIdx];
        if (!node.isLeaf) {
            const float half = curSize * 0.5f;
            const float midX = curMinX + half;
            const float midZ = curMinZ + half;

            uint32_t child = 0u;
            if (midX > x) {
                if (midZ > z) {
                    child = node.children[0];
                } else {
                    curMinZ = midZ;
                    child = node.children[1];
                }
            } else {
                curMinX = midX;
                if (midZ > z) {
                    child = node.children[2];
                } else {
                    curMinZ = midZ;
                    child = node.children[3];
                }
            }

            if (child == 0u) {
                return INVALID_POLYGON;
            }
            curIdx = child;
            curSize = half;
            continue;
        }

        uint32_t bestPoly = INVALID_POLYGON;
        float bestDistSq = 100000000.0f;
        for (uint8_t i = 0; i < node.polygonCount; ++i) {
            const uint32_t candidateIdx = m_nodePolygons[node.firstPolygon + i];
            const NavPolygon& poly = m_polygons[candidateIdx];
            if (poly.cost < 0.0f) {
                continue;
            }
            float distSq = 0.0f;
            if (withinPolygon(poly, x, y, z, distSq) && distSq < bestDistSq) {
                bestDistSq = distSq;
                bestPoly = candidateIdx;
            }
        }
        return bestPoly;
    }

    return INVALID_POLYGON;
}

uint32_t NavMesh::findPolygonExact(float x, float y, float z) const {
    if (!m_loaded || m_nodes.empty()) {
        return INVALID_POLYGON;
    }
    if (x < m_minX || x > m_maxX || z < m_minZ || z > m_maxZ) {
        return INVALID_POLYGON;
    }
    return findPolygonInNode(0u, m_minX, m_minZ, m_size, x, y, z);
}

uint32_t NavMesh::findNearestPolygon(float x, float y, float z, float maxRadius) const {
    if (!m_loaded || m_polygons.empty()) {
        return INVALID_POLYGON;
    }

    const float maxRadiusSq = maxRadius * maxRadius;
    uint32_t bestPoly = INVALID_POLYGON;
    float bestScore = std::numeric_limits<float>::max();

    for (uint32_t i = 0; i < static_cast<uint32_t>(m_polygons.size()); ++i) {
        const NavPolygon& poly = m_polygons[i];
        if (poly.cost < 0.0f) {
            continue;
        }
        const float dx = poly.centroid.x - x;
        const float dz = poly.centroid.z - z;
        const float xzSq = dx * dx + dz * dz;
        if (xzSq > maxRadiusSq) {
            continue;
        }
        const float dy = poly.centroid.y - y;
        // Prefer low-cost normal walkable polygons near (x, y, z).
        const float score = (xzSq + 0.25f * dy * dy) * poly.cost;
        if (score < bestScore) {
            bestScore = score;
            bestPoly = i;
        }
    }
    return bestPoly;
}

uint32_t NavMesh::findPolygon(float x, float y, float z) const {
    const uint32_t exact = findPolygonExact(x, y, z);
    if (exact != INVALID_POLYGON) {
        return exact;
    }
    return findNearestPolygon(x, y, z, 64.0f);
}

bool NavMesh::findPath(float startX, float startY, float startZ,
                       float goalX, float goalY, float goalZ,
                       std::vector<NavPoint>& outWaypoints,
                       uint32_t maxExpanded) const {
    outWaypoints.clear();
    if (!m_loaded) {
        return false;
    }

    const uint32_t startPoly = findPolygon(startX, startY, startZ);
    const uint32_t goalPoly = findPolygon(goalX, goalY, goalZ);
    if (startPoly == INVALID_POLYGON || goalPoly == INVALID_POLYGON) {
        return false;
    }

    const NavPoint goalPoint = {goalX, goalY, goalZ};
    if (startPoly == goalPoly) {
        outWaypoints.push_back(goalPoint);
        return true;
    }

    const NavPoint& goalCentroid = m_polygons[goalPoly].centroid;

    std::vector<AStarNodeState> states;
    states.reserve(1024u);
    std::unordered_map<uint32_t, size_t> stateIndexByPoly;
    stateIndexByPoly.reserve(1024u);

    std::priority_queue<OpenQueueEntry, std::vector<OpenQueueEntry>, OpenQueueGreater> openQueue;

    AStarNodeState startState;
    startState.poly = startPoly;
    startState.parent = INVALID_POLYGON;
    startState.parentEdge = 0u;
    startState.g = 0.0f;
    startState.closed = false;
    stateIndexByPoly[startPoly] = states.size();
    states.push_back(startState);

    OpenQueueEntry startEntry;
    startEntry.f = dist3D(m_polygons[startPoly].centroid, goalCentroid);
    startEntry.g = 0.0f;
    startEntry.poly = startPoly;
    openQueue.push(startEntry);

    uint32_t expanded = 0u;
    bool foundGoal = false;

    while (!openQueue.empty() && expanded < maxExpanded) {
        const OpenQueueEntry top = openQueue.top();
        openQueue.pop();

        const size_t curIdx = stateIndexByPoly[top.poly];
        if (states[curIdx].closed) {
            continue;
        }
        states[curIdx].closed = true;

        if (top.poly == goalPoly) {
            foundGoal = true;
            break;
        }
        ++expanded;

        const float curG = states[curIdx].g;
        const NavPolygon& curPoly = m_polygons[top.poly];

        for (uint8_t edge = 0u; edge < curPoly.numVertices; ++edge) {
            const uint32_t nbrPolyIdx = m_neighbours[curPoly.firstIndex + edge];
            if (nbrPolyIdx == INVALID_POLYGON || nbrPolyIdx >= m_polygons.size()) {
                continue;
            }
            const NavPolygon& nbrPoly = m_polygons[nbrPolyIdx];
            if (nbrPoly.cost < 0.0f) {
                continue;
            }

            const std::unordered_map<uint32_t, size_t>::const_iterator existingIt =
                stateIndexByPoly.find(nbrPolyIdx);
            if (existingIt != stateIndexByPoly.end() && states[existingIt->second].closed) {
                continue;
            }

            const uint8_t nextEdge = static_cast<uint8_t>((edge + 1u) % curPoly.numVertices);
            const NavPoint& vA = m_vertices[m_vertexIndices[curPoly.firstIndex + edge]];
            const NavPoint& vB = m_vertices[m_vertexIndices[curPoly.firstIndex + nextEdge]];
            const NavPoint edgeMid = {
                0.5f * (vA.x + vB.x),
                0.5f * (vA.y + vB.y),
                0.5f * (vA.z + vB.z)
            };

            const float stepDist = dist3D(curPoly.centroid, edgeMid) +
                                   dist3D(edgeMid, nbrPoly.centroid);
            const float tentativeG = curG + stepDist * nbrPoly.cost;

            if (existingIt == stateIndexByPoly.end()) {
                AStarNodeState nextState;
                nextState.poly = nbrPolyIdx;
                nextState.parent = top.poly;
                nextState.parentEdge = edge;
                nextState.g = tentativeG;
                nextState.closed = false;
                stateIndexByPoly[nbrPolyIdx] = states.size();
                states.push_back(nextState);

                OpenQueueEntry nextEntry;
                nextEntry.g = tentativeG;
                nextEntry.f = tentativeG + dist3D(nbrPoly.centroid, goalCentroid);
                nextEntry.poly = nbrPolyIdx;
                openQueue.push(nextEntry);
            } else if (tentativeG < states[existingIt->second].g) {
                AStarNodeState& existing = states[existingIt->second];
                existing.parent = top.poly;
                existing.parentEdge = edge;
                existing.g = tentativeG;

                OpenQueueEntry nextEntry;
                nextEntry.g = tentativeG;
                nextEntry.f = tentativeG + dist3D(nbrPoly.centroid, goalCentroid);
                nextEntry.poly = nbrPolyIdx;
                openQueue.push(nextEntry);
            }
        }
    }

    if (!foundGoal) {
        return false;
    }

    // Reconstruct traversed (parentPoly, edge) transitions in reverse, then reverse them.
    struct StepEdge {
        uint32_t poly;
        uint16_t edge;
    };
    std::vector<StepEdge> steps;
    uint32_t tracePoly = goalPoly;
    while (tracePoly != startPoly) {
        const AStarNodeState& st = states[stateIndexByPoly[tracePoly]];
        if (st.parent == INVALID_POLYGON) {
            return false;
        }
        StepEdge se;
        se.poly = st.parent;
        se.edge = st.parentEdge;
        steps.push_back(se);
        tracePoly = st.parent;
    }
    std::reverse(steps.begin(), steps.end());

    outWaypoints.reserve(steps.size() + 1u);
    for (size_t i = 0; i < steps.size(); ++i) {
        const NavPolygon& p = m_polygons[steps[i].poly];
        const uint8_t e = static_cast<uint8_t>(steps[i].edge);
        const uint8_t eNext = static_cast<uint8_t>((e + 1u) % p.numVertices);
        const NavPoint& vA = m_vertices[m_vertexIndices[p.firstIndex + e]];
        const NavPoint& vB = m_vertices[m_vertexIndices[p.firstIndex + eNext]];
        const NavPoint mid = {
            0.5f * (vA.x + vB.x),
            0.5f * (vA.y + vB.y),
            0.5f * (vA.z + vB.z)
        };
        if (outWaypoints.empty() || dist3D(outWaypoints.back(), mid) > 0.05f) {
            outWaypoints.push_back(mid);
        }
    }

    if (outWaypoints.empty() || dist3D(outWaypoints.back(), goalPoint) > 0.05f) {
        outWaypoints.push_back(goalPoint);
    }
    return true;
}

bool NavMesh::isWalkablePolygon(uint32_t polyIdx) const {
    if (polyIdx >= m_polygons.size()) {
        return false;
    }
    return m_polygons[polyIdx].cost >= 0.0f;
}

float NavMesh::getPolygonCost(uint32_t polyIdx) const {
    if (polyIdx >= m_polygons.size()) {
        return -1.0f;
    }
    return m_polygons[polyIdx].cost;
}

NavPoint NavMesh::getPolygonCentroid(uint32_t polyIdx) const {
    if (polyIdx >= m_polygons.size()) {
        const NavPoint zero = {0.0f, 0.0f, 0.0f};
        return zero;
    }
    return m_polygons[polyIdx].centroid;
}

float NavMesh::getPolygonHeight(uint32_t polyIdx, float x, float z) const {
    if (polyIdx >= m_polygons.size()) {
        return 0.0f;
    }
    const NavPolygon& poly = m_polygons[polyIdx];
    if (poly.numVertices == 0u || std::fabs(poly.normal.y) <= 1e-6f) {
        return poly.centroid.y;
    }
    const NavPoint& first = m_vertices[m_vertexIndices[poly.firstIndex]];
    return first.y - (poly.normal.x * (x - first.x) +
                      poly.normal.z * (z - first.z)) / poly.normal.y;
}

float NavMesh::sampleAerialHeight(float x, float z, float fallbackY) const {
    if (!m_ahmLoaded || m_ahmWidth <= 0 || m_ahmHeight <= 0) {
        return fallbackY;
    }
    const int32_t col = static_cast<int32_t>((x - m_ahmOriginX) / m_ahmCellSize);
    const int32_t row = static_cast<int32_t>((z - m_ahmOriginZ) / m_ahmCellSize);
    if (col < 0 || col >= m_ahmWidth || row < 0 || row >= m_ahmHeight) {
        return fallbackY;
    }
    const size_t idx = static_cast<size_t>(col) +
                       static_cast<size_t>(m_ahmWidth) * static_cast<size_t>(row);
    return static_cast<float>(m_ahmHeights[idx]);
}

} // namespace bf2bot
