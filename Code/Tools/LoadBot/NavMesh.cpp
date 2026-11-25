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

inline float triArea2XZ(const NavPoint& a, const NavPoint& b, const NavPoint& c) {
    const float ax = b.x - a.x;
    const float az = b.z - a.z;
    const float bx = c.x - a.x;
    const float bz = c.z - a.z;
    return bx * az - ax * bz;
}

inline bool pointsNearXZ(const NavPoint& a, const NavPoint& b) {
    const float dx = a.x - b.x;
    const float dz = a.z - b.z;
    return (dx * dx + dz * dz) < 1e-6f;
}

inline float diversityMultiplier(uint32_t polyIdx, uint32_t seed) {
    if (seed == 0u) {
        return 1.0f;
    }
    uint32_t h = (polyIdx * 2654435761u) ^ (seed * 2246822519u);
    h ^= (h >> 16);
    h *= 0x85EBCA6Bu;
    h ^= (h >> 13);
    const float unit = static_cast<float>(h & 0xFFFFu) / 65535.0f;
    return 1.0f + 0.18f * unit;
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

bool NavMesh::findCorridor(float startX, float startY, float startZ,
                           float goalX, float goalY, float goalZ,
                           uint32_t diversitySeed, uint32_t maxExpanded,
                           uint32_t& outStartPoly, uint32_t& outGoalPoly,
                           std::vector<CorridorStep>& outSteps) const {
    outSteps.clear();
    outStartPoly = INVALID_POLYGON;
    outGoalPoly = INVALID_POLYGON;
    if (!m_loaded) {
        return false;
    }

    const uint32_t startPoly = findPolygon(startX, startY, startZ);
    const uint32_t goalPoly = findPolygon(goalX, goalY, goalZ);
    if (startPoly == INVALID_POLYGON || goalPoly == INVALID_POLYGON) {
        return false;
    }
    outStartPoly = startPoly;
    outGoalPoly = goalPoly;

    if (startPoly == goalPoly) {
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
            const float divMult = diversityMultiplier(nbrPolyIdx, diversitySeed);
            const float tentativeG = curG + stepDist * nbrPoly.cost * divMult;

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

    uint32_t tracePoly = goalPoly;
    while (tracePoly != startPoly) {
        const AStarNodeState& st = states[stateIndexByPoly[tracePoly]];
        if (st.parent == INVALID_POLYGON) {
            return false;
        }
        CorridorStep se;
        se.poly = st.parent;
        se.edge = st.parentEdge;
        se.nextPoly = tracePoly;
        outSteps.push_back(se);
        tracePoly = st.parent;
    }
    std::reverse(outSteps.begin(), outSteps.end());
    return true;
}

bool NavMesh::findPath(float startX, float startY, float startZ,
                       float goalX, float goalY, float goalZ,
                       std::vector<NavPoint>& outWaypoints,
                       uint32_t maxExpanded) const {
    outWaypoints.clear();
    uint32_t startPoly = INVALID_POLYGON;
    uint32_t goalPoly = INVALID_POLYGON;
    std::vector<CorridorStep> steps;
    if (!findCorridor(startX, startY, startZ, goalX, goalY, goalZ,
                      0u, maxExpanded, startPoly, goalPoly, steps)) {
        return false;
    }

    const NavPoint goalPoint = {goalX, goalY, goalZ};
    if (startPoly == goalPoly) {
        outWaypoints.push_back(goalPoint);
        return true;
    }

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

bool NavMesh::findSmoothedPath(float startX, float startY, float startZ,
                               float goalX, float goalY, float goalZ,
                               std::vector<NavPoint>& outWaypoints,
                               uint32_t diversitySeed,
                               uint32_t maxExpanded) const {
    outWaypoints.clear();
    uint32_t startPoly = INVALID_POLYGON;
    uint32_t goalPoly = INVALID_POLYGON;
    std::vector<CorridorStep> steps;
    if (!findCorridor(startX, startY, startZ, goalX, goalY, goalZ,
                      diversitySeed, maxExpanded, startPoly, goalPoly, steps)) {
        return false;
    }

    const float startH = getPolygonHeight(startPoly, startX, startZ);
    const float goalH = getPolygonHeight(goalPoly, goalX, goalZ);
    const NavPoint startPt = {startX, startH, startZ};
    const NavPoint goalPt = {goalX, goalH, goalZ};

    if (startPoly == goalPoly || steps.empty()) {
        outWaypoints.push_back(goalPt);
        return true;
    }

    // Build ordered portal list (left, right) with slight corner inset (~0.45m soldier clearance)
    // so bots cut clean lines across open squares without clipping building edges.
    std::vector<PortalEdge> portals;
    portals.reserve(steps.size() + 2u);

    PortalEdge startPortal;
    startPortal.left = startPt;
    startPortal.right = startPt;
    portals.push_back(startPortal);

    for (size_t i = 0; i < steps.size(); ++i) {
        const NavPolygon& fromPoly = m_polygons[steps[i].poly];
        const NavPolygon& toPoly = m_polygons[steps[i].nextPoly];
        const uint8_t e = static_cast<uint8_t>(steps[i].edge);
        const uint8_t eNext = static_cast<uint8_t>((e + 1u) % fromPoly.numVertices);
        NavPoint vA = m_vertices[m_vertexIndices[fromPoly.firstIndex + e]];
        NavPoint vB = m_vertices[m_vertexIndices[fromPoly.firstIndex + eNext]];

        // Inset portal slightly inward along the shared edge to maintain wall clearance.
        const float edx = vB.x - vA.x;
        const float edy = vB.y - vA.y;
        const float edz = vB.z - vA.z;
        const float eLen = std::sqrt(edx * edx + edz * edz);
        if (eLen > 0.25f) {
            const float inset = (eLen * 0.22f < 0.45f) ? (eLen * 0.22f) : 0.45f;
            const float t = inset / eLen;
            const NavPoint aIn = { vA.x + edx * t, vA.y + edy * t, vA.z + edz * t };
            const NavPoint bIn = { vB.x - edx * t, vB.y - edy * t, vB.z - edz * t };
            vA = aIn;
            vB = bIn;
        }

        PortalEdge pe;
        // Winding-independent left/right determination relative to travel direction (from -> to).
        if (triArea2XZ(fromPoly.centroid, toPoly.centroid, vA) >
            triArea2XZ(fromPoly.centroid, toPoly.centroid, vB)) {
            pe.right = vA;
            pe.left = vB;
        } else {
            pe.left = vA;
            pe.right = vB;
        }
        portals.push_back(pe);
    }

    PortalEdge endPortal;
    endPortal.left = goalPt;
    endPortal.right = goalPt;
    portals.push_back(endPortal);

    // Simple Stupid Funnel Algorithm (2D XZ string-pulling).
    NavPoint portalApex = portals[0].left;
    NavPoint portalLeft = portals[0].left;
    NavPoint portalRight = portals[0].right;
    size_t apexIndex = 0u;
    size_t leftIndex = 0u;
    size_t rightIndex = 0u;

    const size_t maxIters = portals.size() * 4u + 64u;
    size_t iters = 0u;

    for (size_t i = 1u; i < portals.size() && iters < maxIters; ++i, ++iters) {
        const NavPoint& left = portals[i].left;
        const NavPoint& right = portals[i].right;

        // Update right boundary.
        if (triArea2XZ(portalApex, portalRight, right) <= 0.0f) {
            if (pointsNearXZ(portalApex, portalRight) ||
                triArea2XZ(portalApex, portalLeft, right) > 0.0f) {
                portalRight = right;
                rightIndex = i;
            } else {
                // Right crossed over left -> commit portalLeft corner.
                if (outWaypoints.empty() || dist3D(outWaypoints.back(), portalLeft) > 0.25f) {
                    outWaypoints.push_back(portalLeft);
                }
                portalApex = portalLeft;
                apexIndex = leftIndex;
                portalLeft = portalApex;
                portalRight = portalApex;
                leftIndex = apexIndex;
                rightIndex = apexIndex;
                i = apexIndex;
                continue;
            }
        }

        // Update left boundary.
        if (triArea2XZ(portalApex, portalLeft, left) >= 0.0f) {
            if (pointsNearXZ(portalApex, portalLeft) ||
                triArea2XZ(portalApex, portalRight, left) < 0.0f) {
                portalLeft = left;
                leftIndex = i;
            } else {
                // Left crossed over right -> commit portalRight corner.
                if (outWaypoints.empty() || dist3D(outWaypoints.back(), portalRight) > 0.25f) {
                    outWaypoints.push_back(portalRight);
                }
                portalApex = portalRight;
                apexIndex = rightIndex;
                portalLeft = portalApex;
                portalRight = portalApex;
                leftIndex = apexIndex;
                rightIndex = apexIndex;
                i = apexIndex;
                continue;
            }
        }
    }

    if (outWaypoints.empty() || dist3D(outWaypoints.back(), goalPt) > 0.25f) {
        outWaypoints.push_back(goalPt);
    }
    return true;
}

bool NavMesh::isPointWalkable(float x, float y, float z) const {
    if (!m_loaded) {
        return true;
    }
    return findPolygonExact(x, y, z) != INVALID_POLYGON;
}

bool NavMesh::hasNavLineOfSight(float ax, float ay, float az,
                                float bx, float by, float bz,
                                float maxGapMeters) const {
    if (!m_loaded) {
        return true;
    }
    const float dx = bx - ax;
    const float dy = by - ay;
    const float dz = bz - az;
    const float len = std::sqrt(dx * dx + dz * dz);
    if (len < 2.0f) {
        return true;
    }

    const float stepSize = 2.0f;
    const int steps = static_cast<int>(len / stepSize);
    float currentGap = 0.0f;
    for (int i = 1; i < steps; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(steps);
        const float sx = ax + dx * t;
        const float sy = ay + dy * t;
        const float sz = az + dz * t;
        if (findPolygonExact(sx, sy, sz) == INVALID_POLYGON) {
            currentGap += stepSize;
            if (currentGap >= maxGapMeters) {
                return false;
            }
        } else {
            currentGap = 0.0f;
        }
    }
    return true;
}

bool NavMesh::raycastLineOfSight(float ax, float ay, float az,
                                 float bx, float by, float bz,
                                 float eyeHeightA, float targetHeightB) const {
    if (!m_loaded) {
        return true;
    }

    const float dx = bx - ax;
    const float dz = bz - az;
    const float distXZ = std::sqrt(dx * dx + dz * dz);
    if (distXZ < 1.2f) {
        // Point-blank: only reject if separated vertically by a solid floor/ceiling.
        return std::fabs(by - ay) < 2.8f;
    }

    const float eyeAY = ay + eyeHeightA;
    const float tgtBY = by + targetHeightB;

    // Locate starting and target walkable polygons (tight 0.65m curb-erosion snap only so
    // an enemy inside an un-navmeshed room behind a wall cannot snap onto the outer street).
    uint32_t startPoly = findPolygonExact(ax, ay, az);
    if (startPoly == INVALID_POLYGON) {
        startPoly = findNearestPolygon(ax, ay, az, 0.65f);
    }
    const uint32_t exactEndPoly = findPolygonExact(bx, by, bz);
    const bool endExact = (exactEndPoly != INVALID_POLYGON);
    uint32_t endPoly = exactEndPoly;
    if (endPoly == INVALID_POLYGON) {
        endPoly = findNearestPolygon(bx, by, bz, 0.65f);
    }

    // Fallback when either soldier is off-mesh (e.g. rooftop/crate or inside un-navmeshed room):
    // dense 0.60m probe blocks if any interior sample (>1.2m from endpoints) crosses an off-mesh
    // building/wall footprint below roof height or a terrain crest rises above the sightline.
    if (startPoly == INVALID_POLYGON || endPoly == INVALID_POLYGON) {
        const float stepSize = 0.60f;
        const int steps = static_cast<int>(distXZ / stepSize);
        if (steps <= 1) {
            return std::fabs(by - ay) < 2.8f;
        }
        for (int i = 1; i < steps; ++i) {
            const float t = static_cast<float>(i) / static_cast<float>(steps);
            const float sx = ax + dx * t;
            const float sz = az + dz * t;
            const float chordGroundY = ay + (by - ay) * t;
            const float rayY = eyeAY + (tgtBY - eyeAY) * t;
            const float distFromA = t * distXZ;
            const float distFromB = (1.0f - t) * distXZ;

            const uint32_t p = findPolygonExact(sx, chordGroundY, sz);
            if (p == INVALID_POLYGON) {
                // Allow the first/last 1.2m (roof ledge / crate edge) for off-mesh endpoints,
                // but block any wall/building footprint in the middle below aerial roof height.
                if (distFromA > 1.2f && distFromB > 1.2f) {
                    const float ahmY = sampleAerialHeight(sx, sz, rayY + 10.0f);
                    if (rayY < ahmY + 1.5f) {
                        return false;
                    }
                }
            } else {
                const float polyY = getPolygonHeight(p, sx, sz);
                if (polyY > rayY + 0.15f) {
                    return false;
                }
            }
        }
        return true;
    }

    if (startPoly == endPoly && endExact) {
        return std::fabs(getPolygonHeight(startPoly, bx, bz) - by) < 2.8f;
    }

    // Exact polygon-by-polygon 2D corridor traversal along segment A -> B.
    uint32_t curPoly = startPoly;
    uint32_t prevPoly = INVALID_POLYGON;
    float curT = 0.0f;

    for (int step = 0; step < 192; ++step) {
        if (endExact && curPoly == endPoly) {
            return std::fabs(getPolygonHeight(curPoly, bx, bz) - by) < 2.8f;
        }
        if (curPoly >= m_polygons.size()) {
            return false;
        }

        const NavPolygon& poly = m_polygons[curPoly];
        int bestEdge = -1;
        float bestTRay = -1.0f;
        float bestTEdge = 0.5f;

        for (uint8_t e = 0u; e < poly.numVertices; ++e) {
            const uint8_t eNext = static_cast<uint8_t>((e + 1u) % poly.numVertices);
            const NavPoint& v0 = m_vertices[m_vertexIndices[poly.firstIndex + e]];
            const NavPoint& v1 = m_vertices[m_vertexIndices[poly.firstIndex + eNext]];

            const float ex = v1.x - v0.x;
            const float ez = v1.z - v0.z;

            // Inward normal in BF2 winding is (+ez, -ex); ray exits when dot(dir, N_in) < 0.
            const float dotIn = dx * ez - dz * ex;
            if (dotIn >= -1e-6f) {
                continue;
            }

            // Solve A + tRay * (dx, dz) = v0 + tEdge * (ex, ez).
            // det = dx * ez - dz * ex = dotIn (< 0).
            const float rx = v0.x - ax;
            const float rz = v0.z - az;
            const float tRay = (rx * ez - rz * ex) / dotIn;
            const float tEdge = (rx * dz - rz * dx) / dotIn;

            if (tEdge < -0.01f || tEdge > 1.01f) {
                continue;
            }
            if (tRay < curT - 0.002f) {
                continue;
            }

            // Prefer the furthest valid exit edge (handles vertex-grazing cleanly).
            if (bestEdge < 0 || tRay > bestTRay) {
                bestEdge = static_cast<int>(e);
                bestTRay = tRay;
                bestTEdge = tEdge;
            }
        }

        if (bestEdge < 0) {
            return false;
        }

        // Segment A -> B terminates inside curPoly before crossing its exit edge.
        if (bestTRay >= 1.0f - 1e-4f) {
            return std::fabs(getPolygonHeight(curPoly, bx, bz) - by) < 2.8f;
        }

        const float clampedTRay = (bestTRay < 0.0f) ? 0.0f : bestTRay;
        const float clampedTEdge = (bestTEdge < 0.0f) ? 0.0f : ((bestTEdge > 1.0f) ? 1.0f : bestTEdge);

        // Terrain crest occlusion check at the crossed polygon edge.
        const uint8_t e0 = static_cast<uint8_t>(bestEdge);
        const uint8_t e1 = static_cast<uint8_t>((e0 + 1u) % poly.numVertices);
        const NavPoint& v0 = m_vertices[m_vertexIndices[poly.firstIndex + e0]];
        const NavPoint& v1 = m_vertices[m_vertexIndices[poly.firstIndex + e1]];
        const float edgeY = v0.y + (v1.y - v0.y) * clampedTEdge;
        const float rayY = eyeAY + (tgtBY - eyeAY) * clampedTRay;
        if (edgeY > rayY + 0.15f) {
            return false; // Hill / ramp crest rises above eye-to-target sightline.
        }

        const uint32_t nextPoly = m_neighbours[poly.firstIndex + e0];
        if (nextPoly == INVALID_POLYGON || nextPoly >= m_polygons.size() ||
            m_polygons[nextPoly].cost < 0.0f) {
            // Hit a solid navmesh wall / building / obstacle boundary edge!
            // If target stands on a walkable polygon on the far side of this wall, NEVER allow.
            // Only allow if target is off-mesh hugging the near curb face (<= 0.45m erosion margin).
            if (endExact) {
                return false;
            }
            const float remainingDist = (1.0f - clampedTRay) * distXZ;
            return remainingDist <= 0.45f && std::fabs(edgeY - by) < 2.0f;
        }

        if (nextPoly == prevPoly) {
            return false;
        }

        prevPoly = curPoly;
        curPoly = nextPoly;
        curT = clampedTRay;
    }

    return false;
}

bool NavMesh::findCoverStep(float ownX, float ownY, float ownZ,
                            float threatX, float threatZ,
                            float searchRadius,
                            NavPoint& outCover) const {
    if (!m_loaded) {
        return false;
    }
    const float tx = threatX - ownX;
    const float tz = threatZ - ownZ;
    const float tLen = std::sqrt(tx * tx + tz * tz);
    if (tLen < 0.5f) {
        return false;
    }
    const float dirX = tx / tLen;
    const float dirZ = tz / tLen;
    const float rightX = -dirZ;
    const float rightZ = dirX;

    // Candidate angles relative to threat direction: lateral flanks, rear-diagonals, retreat.
    static const float kDirs[10][2] = {
        {  1.00f,  0.00f }, // pure right
        { -1.00f,  0.00f }, // pure left
        {  0.86f, -0.50f }, // back-right
        { -0.86f, -0.50f }, // back-left
        {  0.50f, -0.86f }, // rear-right
        { -0.50f, -0.86f }, // rear-left
        {  0.00f, -1.00f }, // straight back
        {  0.86f,  0.50f }, // forward-right (corner peek)
        { -0.86f,  0.50f }, // forward-left (corner peek)
        {  0.35f, -0.93f }  // angled retreat
    };
    const float radii[2] = { searchRadius, searchRadius * 0.55f };

    float bestScore = -1e9f;
    bool found = false;
    NavPoint bestPt = {ownX, ownY, ownZ};

    for (int r = 0; r < 2; ++r) {
        const float rad = radii[r];
        if (rad < 1.5f) continue;
        for (int i = 0; i < 10; ++i) {
            const float cx = ownX + (rightX * kDirs[i][0] + dirX * kDirs[i][1]) * rad;
            const float cz = ownZ + (rightZ * kDirs[i][0] + dirZ * kDirs[i][1]) * rad;
            const uint32_t poly = findPolygonExact(cx, ownY, cz);
            if (poly == INVALID_POLYGON) {
                continue;
            }
            const float cy = getPolygonHeight(poly, cx, cz);
            // Make sure we can walk straight there without a wall between us and the cover spot.
            if (!raycastLineOfSight(ownX, ownY, ownZ, cx, cy, cz)) {
                continue;
            }

            float score = 0.0f;
            // Big bonus if a building/wall blocks direct line from threat to candidate!
            if (!raycastLineOfSight(cx, cy, cz, threatX, ownY, threatZ)) {
                score += 12.0f;
            }
            // Bonus if candidate sits near a wall/boundary on the threat-facing side (hugging cover).
            const float probeX = cx + dirX * 2.2f;
            const float probeZ = cz + dirZ * 2.2f;
            if (findPolygonExact(probeX, cy, probeZ) == INVALID_POLYGON) {
                score += 6.0f;
            }
            // Prefer lateral/retreat movement over running directly toward threat.
            score -= kDirs[i][1] * 2.0f;
            // Slight preference for full searchRadius when equally good.
            score += (r == 0) ? 1.0f : 0.0f;

            if (score > bestScore) {
                bestScore = score;
                bestPt.x = cx;
                bestPt.y = cy;
                bestPt.z = cz;
                found = true;
            }
        }
    }

    if (found) {
        outCover = bestPt;
    }
    return found;
}

bool NavMesh::findFlankWaypoint(float startX, float startY, float startZ,
                                float goalX, float goalY, float goalZ,
                                float sideSign, float offsetDist,
                                NavPoint& outFlank) const {
    if (!m_loaded) {
        return false;
    }
    const float dx = goalX - startX;
    const float dz = goalZ - startZ;
    const float dist = std::sqrt(dx * dx + dz * dz);
    if (dist < 22.0f) {
        return false;
    }
    const float perpX = (-dz / dist) * (sideSign >= 0.0f ? 1.0f : -1.0f);
    const float perpZ = ( dx / dist) * (sideSign >= 0.0f ? 1.0f : -1.0f);

    const float midX = startX + dx * 0.55f;
    const float midY = startY + (goalY - startY) * 0.55f;
    const float midZ = startZ + dz * 0.55f;

    const float scales[3] = { 1.0f, 0.65f, 0.35f };
    for (int i = 0; i < 3; ++i) {
        const float off = offsetDist * scales[i];
        const float fx = midX + perpX * off;
        const float fz = midZ + perpZ * off;
        const uint32_t poly = findPolygonExact(fx, midY, fz);
        if (poly != INVALID_POLYGON) {
            outFlank.x = fx;
            outFlank.y = getPolygonHeight(poly, fx, fz);
            outFlank.z = fz;
            return true;
        }
        const uint32_t nearPoly = findNearestPolygon(fx, midY, fz, 18.0f);
        if (nearPoly != INVALID_POLYGON) {
            outFlank = getPolygonCentroid(nearPoly);
            return true;
        }
    }
    return false;
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
    const float fx = (x - m_ahmOriginX) / m_ahmCellSize;
    const float fz = (z - m_ahmOriginZ) / m_ahmCellSize;
    if (fx < 0.0f || fz < 0.0f ||
        fx >= static_cast<float>(m_ahmWidth) || fz >= static_cast<float>(m_ahmHeight)) {
        return fallbackY;
    }
    const int32_t c0 = static_cast<int32_t>(fx);
    const int32_t r0 = static_cast<int32_t>(fz);
    const int32_t c1 = (c0 + 1 < m_ahmWidth) ? (c0 + 1) : c0;
    const int32_t r1 = (r0 + 1 < m_ahmHeight) ? (r0 + 1) : r0;
    const float tx = fx - static_cast<float>(c0);
    const float tz = fz - static_cast<float>(r0);

    const float h00 = static_cast<float>(m_ahmHeights[static_cast<size_t>(c0) + static_cast<size_t>(m_ahmWidth) * static_cast<size_t>(r0)]);
    const float h10 = static_cast<float>(m_ahmHeights[static_cast<size_t>(c1) + static_cast<size_t>(m_ahmWidth) * static_cast<size_t>(r0)]);
    const float h01 = static_cast<float>(m_ahmHeights[static_cast<size_t>(c0) + static_cast<size_t>(m_ahmWidth) * static_cast<size_t>(r1)]);
    const float h11 = static_cast<float>(m_ahmHeights[static_cast<size_t>(c1) + static_cast<size_t>(m_ahmWidth) * static_cast<size_t>(r1)]);

    const float h0 = h00 + (h10 - h00) * tx;
    const float h1 = h01 + (h11 - h01) * tx;
    return h0 + (h1 - h0) * tz;
}

} // namespace bf2bot
