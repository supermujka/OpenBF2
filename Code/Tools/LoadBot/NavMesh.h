#ifndef BF2BOT_NAVMESH_H
#define BF2BOT_NAVMESH_H

#include <cstdint>
#include <string>
#include <vector>

namespace bf2bot {

struct NavPoint {
    float x;
    float y;
    float z;
};

// Standalone reader and A* pathfinder for BF2 AIPathFinding v3 .qtr navmeshes
// (and optional AerialHeighMap.ahm). Thread-safe for concurrent reads after load().
class NavMesh {
public:
    static const uint32_t INVALID_POLYGON = 0xFFFFFFFFu;

    NavMesh();

    // Loads <path>/Infantry.qtr (if path is a directory) or path directly (if .qtr file).
    // Also loads sibling AerialHeighMap.ahm if present.
    bool load(const std::string& path);
    void clear();

    bool isLoaded() const { return m_loaded; }
    float worldSize() const { return m_size; }
    float boundsMinX() const { return m_minX; }
    float boundsMinZ() const { return m_minZ; }
    float boundsMaxX() const { return m_maxX; }
    float boundsMaxZ() const { return m_maxZ; }
    uint32_t polygonCount() const { return static_cast<uint32_t>(m_polygons.size()); }
    uint32_t vertexCount() const { return static_cast<uint32_t>(m_vertices.size()); }
    bool hasAerialHeightMap() const { return m_ahmLoaded; }

    // Finds the walkable polygon containing (x, y, z) via QuadTree descent + plane/edge
    // checks (+2.5m above / -1.0m below), falling back to nearest walkable polygon within 64m.
    uint32_t findPolygon(float x, float y, float z) const;

    // Exact QuadTree polygon lookup without off-mesh fallback.
    uint32_t findPolygonExact(float x, float y, float z) const;

    // Nearest walkable polygon centroid within maxRadius in XZ.
    uint32_t findNearestPolygon(float x, float y, float z, float maxRadius = 64.0f) const;

    // Polygon-graph A* search across neighbour edges, emitting edge-midpoint waypoints.
    bool findPath(float startX, float startY, float startZ,
                  float goalX, float goalY, float goalZ,
                  std::vector<NavPoint>& outWaypoints,
                  uint32_t maxExpanded = 4096) const;

    bool isWalkablePolygon(uint32_t polyIdx) const;
    float getPolygonCost(uint32_t polyIdx) const;
    NavPoint getPolygonCentroid(uint32_t polyIdx) const;
    float getPolygonHeight(uint32_t polyIdx, float x, float z) const;
    float sampleAerialHeight(float x, float z, float fallbackY = 0.0f) const;

private:
    struct QuadNode {
        uint32_t children[4];
        uint8_t isLeaf;
        uint8_t polygonCount;
        uint32_t firstPolygon;
    };

    struct NavPolygon {
        uint8_t numVertices;
        uint8_t material;
        uint16_t cluster;
        uint32_t firstIndex;
        NavPoint normal;
        NavPoint centroid;
        float cost;
    };

    bool loadQtrFile(const std::string& qtrPath);
    bool loadAhmFile(const std::string& ahmPath);
    bool pointWithinPolygonEdges(const NavPolygon& poly, float x, float z) const;
    bool withinPolygon(const NavPolygon& poly, float x, float y, float z, float& outDistSq) const;
    uint32_t findPolygonInNode(uint32_t nodeIdx, float minX, float minZ, float size,
                               float x, float y, float z) const;

    bool m_loaded;
    float m_minX;
    float m_minZ;
    float m_maxX;
    float m_maxZ;
    float m_size;

    std::vector<QuadNode> m_nodes;
    std::vector<NavPolygon> m_polygons;
    std::vector<uint32_t> m_nodePolygons;
    std::vector<NavPoint> m_vertices;
    std::vector<uint32_t> m_vertexIndices;
    std::vector<uint32_t> m_neighbours;

    bool m_ahmLoaded;
    float m_ahmCellSize;
    int32_t m_ahmWidth;
    int32_t m_ahmHeight;
    float m_ahmOriginX;
    float m_ahmOriginZ;
    std::vector<uint16_t> m_ahmHeights;
};

} // namespace bf2bot

#endif // BF2BOT_NAVMESH_H
