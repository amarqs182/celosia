#pragma once

// Background-thread CPU Hi-Z software occlusion culler (80x64 invW depth buffer
// with a 4-level min-invW pyramid). Built for the RK3326 (R36S) so cpu1/cpu2
// can rasterize solid opaque occluders from the previous frame asynchronously
// and let cpu0 (the Game Thread in decomp_walk.cpp) skip occluded objects
// before touching VU1/display-list construction or draw submission.

#include <cstdint>
#include <vector>

namespace native_renderer
{
    struct Vertex;

    namespace occlusion
    {
        // Registers a static opaque-candidate mesh's vertex positions (called
        // from uploadMesh / uploadIndexedMesh on the game thread).
        void registerMesh(uint64_t id, const Vertex *vertices, uint32_t count, bool perFrame);
        void registerIndexedMesh(uint64_t id, const Vertex *vertices, uint32_t count, const uint32_t *indices, uint32_t indexCount);

        // Records whether an uploaded texture has fully opaque mip-0 alpha so
        // foliage/cutout cards are excluded from acting as solid occluders.
        void registerTexture(uint64_t id, uint32_t width, uint32_t height, const uint8_t *rgba);

        // Captures the current frame's world->clip matrix (4 V4 rows, float[16])
        // and latches the latest completed previous-frame Hi-Z buffer for queries.
        void setFrameCamera(const float vpRowMajor[16]);

        // Queues solid opaque draws on the game thread to rasterize on endFrame().
        void submitDraw(uint64_t meshId, const float clip[16]);
        void submitDrawMulti(const uint64_t *meshIds, uint32_t count, const float clip[16]);
        bool isOpaqueTexture(uint64_t id);

        // Flushes staged meshes and occluder draws to the background thread.
        void endFrame(bool enabled, bool logStats);

        // Tests a model-space AABB (lo[3], hi[3]) transformed by model-to-world
        // M (4 V4 rows, float[16]) against the previous frame's Hi-Z pyramid.
        // Returns true ONLY when conservatively guaranteed occluded behind solids.
        bool isBoxOccluded(const float modelRowMajor[16], const float lo[3], const float hi[3], float bias, float margin);

        // Fast-path helpers for BVH sub-part traversal inside modelDisp: compute
        // outPmPrev = VP_prev * model once per model (returning false if Hi-Z is
        // inactive or currentCamera doesn't match the captured main camera), then
        // test individual BVH node AABBs in O(1) via isBoxOccludedClip().
        bool prepareModelPrevClip(const float currentCamera[16], const float modelRowMajor[16], float outPmPrev[16]);
        bool isBoxOccludedClip(const float pmPrev[16], const float lo[3], const float hi[3], float minDist, float bias, float margin);

        // Per-draw mesh AABB culling against the previous frame's Hi-Z pyramid
        // using reprojected clip_prev = (VP_prev * inv(VP_cur)) * clip.
        bool isMeshOccluded(uint64_t meshId, const float clip[16], float minDist, float bias, float margin);
        uint32_t filterOccludedMeshes(const uint64_t *meshes, uint32_t count, const float clip[16],
                                      float minDist, float bias, float margin, std::vector<uint64_t> &outKept);

        void shutdown();
    }
}
