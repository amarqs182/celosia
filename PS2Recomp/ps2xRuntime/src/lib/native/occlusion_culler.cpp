// Dedicated background-thread CPU Hi-Z software occlusion culler.
//
// Why this exists on the R36S (4x Cortex-A35):
// In Shadow of the Colossus, the scene graph walks interior rooms, stairs,
// and distant props behind the Shrine of Worship walls with only frustum
// culling and distance LOD fade. Those occluded objects still consume cpu0
// matrix/light/VIF work and add thousands of native draw calls inside
// native_renderer.
//
// How it stays lock-free on the hot path and conservative:
// 1. Game thread (cpu0) tracks compact xyz-only occluder meshes (8 MiB cap)
//    and opaque-alpha textures, and appends (meshId, clip[16]) commands to
//    staging vectors with zero mutexes during draw()/drawMulti().
// 2. At endFrame(), cpu0 transfers staged data in a single mutex swap to a
//    dedicated worker thread pinned to cpu1/cpu2 (leaving cpu0 free for the
//    game loop and avoiding core 3 where audio/render threads often sit).
// 3. The worker rasterizes occluder triangles (near-clipped at w >= 0.1) into
//    an 80x64 max(invW) buffer and builds a 4-level min(invW) Hi-Z pyramid,
//    publishing into a wait-free triple buffer.
// 4. On the next frame, setFrameCamera() latches the latest published Hi-Z
//    buffer (built under VP_prev) and isBoxOccluded() projects the model AABB
//    through (VP_prev * M) after expansion by `margin` and comparison with
//    `maxObjInvW * (1 + bias) < tileMinInvW`. Sudden camera cuts invalidate
//    reprojection for one frame so no ghost culling can occur.

#include "runtime/occlusion_culler.h"
#include "runtime/native_renderer.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#endif

namespace native_renderer
{
    namespace occlusion
    {
        namespace
        {
            constexpr int kW0 = 80;
            constexpr int kH0 = 64;
            constexpr int kW1 = 40;
            constexpr int kH1 = 32;
            constexpr int kW2 = 20;
            constexpr int kH2 = 16;
            constexpr int kW3 = 10;
            constexpr int kH3 = 8;

            constexpr size_t kMaxMeshVerts = 18000u;
            constexpr size_t kMinMeshVerts = 3u;
            constexpr size_t kMaxMeshCacheBytes = 8u * 1024u * 1024u;
            constexpr uint32_t kMaxStageDraws = 2500u;

            struct Vec3
            {
                float x, y, z;
            };

            struct Vec4
            {
                float x, y, z, w;
            };

            struct OccluderMesh
            {
                std::vector<Vec3> verts;
            };

            struct PendingMesh
            {
                uint64_t id = 0;
                std::vector<Vec3> verts;
            };

            struct DrawCmd
            {
                uint64_t meshId = 0;
                float clip[16] = {};
            };

            struct HiZBuffer
            {
                bool valid = false;
                float vp[16] = {};
                float l0[kW0 * kH0] = {};
                float l1[kW1 * kH1] = {};
                float l2[kW2 * kH2] = {};
                float l3[kW3 * kH3] = {};
            };

            // Game-thread only state (zero locks during upload/draw/query).
            IdSet g_knownMeshes;
            IdSet g_opaqueTextures;
            size_t g_meshCacheBytes = 0;
            std::vector<PendingMesh> g_pendingMeshes;
            std::vector<DrawCmd> g_stagedDraws;
            float g_curVp[16] = {};
            bool g_hasCurVp = false;
            bool g_cutInvalidated = true;
            const HiZBuffer *g_activeHiZ = nullptr;
            uint64_t g_queryTested = 0;
            uint64_t g_queryCulled = 0;
            uint64_t g_statFrames = 0;

            // Triple-buffered published Hi-Z pyramids shared wait-free with cpu0.
            HiZBuffer g_buffers[3];
            std::atomic<int> g_readyIdx{0};
            std::atomic<int> g_readingIdx{-1};
            int g_writeIdx = 1;

            // Worker synchronization (touched once per frame in endFrame).
            std::mutex g_mutex;
            std::condition_variable g_cv;
            std::thread g_worker;
            bool g_workerRunning = false;
            bool g_stopRequested = false;
            bool g_hasJob = false;
            bool g_workerBusy = false;

            std::vector<PendingMesh> g_inboxMeshes;
            std::vector<DrawCmd> g_inboxDraws;
            float g_inboxVp[16] = {};
            bool g_inboxHasVp = false;

            // Worker-only persistent state.
            std::unordered_map<uint64_t, OccluderMesh> g_workerMeshes;
            size_t g_workerMeshBytes = 0;
            std::atomic<uint64_t> g_lastRasterUs{0};
            std::atomic<uint64_t> g_lastTriCount{0};

            inline Vec4 transformClip(const float m[16], float x, float y, float z)
            {
                return {
                    m[0] * x + m[4] * y + m[8] * z + m[12],
                    m[1] * x + m[5] * y + m[9] * z + m[13],
                    m[2] * x + m[6] * y + m[10] * z + m[14],
                    m[3] * x + m[7] * y + m[11] * z + m[15],
                };
            }

            inline bool vpContinuous(const float a[16], const float b[16])
            {
                // Detect camera cuts or teleport transitions so reprojection through
                // the previous frame's VP never culls the new camera's view.
                float maxDelta = 0.0f;
                for (int i = 0; i < 16; ++i)
                {
                    const float d = std::fabs(a[i] - b[i]);
                    if (d > maxDelta)
                        maxDelta = d;
                }
                return maxDelta < 25.0f;
            }

            void rasterizeProjectedTri(float *l0, const Vec4 &v0, const Vec4 &v1, const Vec4 &v2)
            {
                const float invW0 = 1.0f / v0.w;
                const float invW1 = 1.0f / v1.w;
                const float invW2 = 1.0f / v2.w;

                const float sx0 = (0.5f * v0.x * invW0 + 0.5f) * static_cast<float>(kW0);
                const float sy0 = (0.5f * v0.y * invW0 + 0.5f) * static_cast<float>(kH0);
                float sx1 = (0.5f * v1.x * invW1 + 0.5f) * static_cast<float>(kW0);
                float sy1 = (0.5f * v1.y * invW1 + 0.5f) * static_cast<float>(kH0);
                float sx2 = (0.5f * v2.x * invW2 + 0.5f) * static_cast<float>(kW0);
                float sy2 = (0.5f * v2.y * invW2 + 0.5f) * static_cast<float>(kH0);
                float z0 = invW0, z1 = invW1, z2 = invW2;

                float area = (sx1 - sx0) * (sy2 - sy0) - (sy1 - sy0) * (sx2 - sx0);
                if (std::fabs(area) < 1e-4f)
                    return;
                if (area < 0.0f)
                {
                    // Swap v1/v2 so both winding sides of solid walls act as occluders.
                    std::swap(sx1, sx2);
                    std::swap(sy1, sy2);
                    std::swap(z1, z2);
                    area = -area;
                }

                const int minX = std::max(0, static_cast<int>(std::floor(std::min({sx0, sx1, sx2}))));
                const int maxX = std::min(kW0 - 1, static_cast<int>(std::floor(std::max({sx0, sx1, sx2}))));
                const int minY = std::max(0, static_cast<int>(std::floor(std::min({sy0, sy1, sy2}))));
                const int maxY = std::min(kH0 - 1, static_cast<int>(std::floor(std::max({sy0, sy1, sy2}))));
                if (minX > maxX || minY > maxY)
                    return;

                const float invArea = 1.0f / area;
                const float dx01 = sx0 - sx1, dy01 = sy1 - sy0;
                const float dx12 = sx1 - sx2, dy12 = sy2 - sy1;
                const float dx20 = sx2 - sx0, dy20 = sy0 - sy2;

                for (int y = minY; y <= maxY; ++y)
                {
                    const float py = static_cast<float>(y) + 0.5f;
                    float *row = l0 + y * kW0;
                    for (int x = minX; x <= maxX; ++x)
                    {
                        const float px = static_cast<float>(x) + 0.5f;
                        const float w0 = dy12 * (px - sx1) + dx12 * (py - sy1);
                        const float w1 = dy20 * (px - sx2) + dx20 * (py - sy2);
                        const float w2 = dy01 * (px - sx0) + dx01 * (py - sy0);
                        if (w0 < 0.0f || w1 < 0.0f || w2 < 0.0f)
                            continue;
                        const float b0 = w0 * invArea;
                        const float b1 = w1 * invArea;
                        const float b2 = w2 * invArea;
                        const float z = b0 * z0 + b1 * z1 + b2 * z2;
                        if (z > row[x])
                            row[x] = z;
                    }
                }
            }

            void clipAndRasterizeTri(float *l0, const Vec4 &a, const Vec4 &b, const Vec4 &c)
            {
                constexpr float kNearW = 0.1f;
                const bool inA = a.w >= kNearW;
                const bool inB = b.w >= kNearW;
                const bool inC = c.w >= kNearW;
                if (inA && inB && inC)
                {
                    rasterizeProjectedTri(l0, a, b, c);
                    return;
                }
                if (!inA && !inB && !inC)
                    return;

                // Sutherland-Hodgman clip against w >= kNearW -> 3 or 4 vertices.
                const Vec4 in[3] = {a, b, c};
                Vec4 poly[4];
                int count = 0;
                for (int i = 0; i < 3; ++i)
                {
                    const Vec4 &p0 = in[i];
                    const Vec4 &p1 = in[(i + 1) % 3];
                    const bool ok0 = p0.w >= kNearW;
                    const bool ok1 = p1.w >= kNearW;
                    if (ok0)
                        poly[count++] = p0;
                    if (ok0 != ok1)
                    {
                        const float t = (kNearW - p0.w) / (p1.w - p0.w);
                        poly[count++] = {
                            p0.x + (p1.x - p0.x) * t,
                            p0.y + (p1.y - p0.y) * t,
                            p0.z + (p1.z - p0.z) * t,
                            kNearW,
                        };
                    }
                }
                if (count >= 3)
                    rasterizeProjectedTri(l0, poly[0], poly[1], poly[2]);
                if (count == 4)
                    rasterizeProjectedTri(l0, poly[0], poly[2], poly[3]);
            }

            void buildMip(const float *src, int sw, float *dst, int dw, int dh)
            {
                for (int y = 0; y < dh; ++y)
                {
                    const float *r0 = src + (y * 2) * sw;
                    const float *r1 = r0 + sw;
                    float *out = dst + y * dw;
                    for (int x = 0; x < dw; ++x)
                    {
                        const int sx = x * 2;
                        out[x] = std::min(std::min(r0[sx], r0[sx + 1]), std::min(r1[sx], r1[sx + 1]));
                    }
                }
            }

            void workerMain()
            {
#if defined(__linux__)
                pthread_setname_np(pthread_self(), "sotc-hiz");
                cpu_set_t cpuset;
                CPU_ZERO(&cpuset);
                CPU_SET(1, &cpuset);
                CPU_SET(2, &cpuset);
                pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);
#endif
                std::vector<PendingMesh> localNewMeshes;
                std::vector<DrawCmd> localDraws;
                float localVp[16] = {};
                bool localHasVp = false;

                for (;;)
                {
                    {
                        std::unique_lock<std::mutex> lock(g_mutex);
                        g_workerBusy = false;
                        g_cv.wait(lock, [] { return g_stopRequested || g_hasJob; });
                        if (g_stopRequested)
                            return;
                        g_hasJob = false;
                        g_workerBusy = true;
                        localNewMeshes.swap(g_inboxMeshes);
                        localDraws.swap(g_inboxDraws);
                        localHasVp = g_inboxHasVp;
                        std::memcpy(localVp, g_inboxVp, sizeof(localVp));
                    }

                    for (PendingMesh &pm : localNewMeshes)
                    {
                        const size_t bytes = pm.verts.size() * sizeof(Vec3);
                        if (g_workerMeshBytes + bytes <= kMaxMeshCacheBytes && g_workerMeshes.find(pm.id) == g_workerMeshes.end())
                        {
                            g_workerMeshBytes += bytes;
                            OccluderMesh &om = g_workerMeshes[pm.id];
                            om.verts = std::move(pm.verts);
                        }
                    }
                    localNewMeshes.clear();

                    if (!localHasVp || localDraws.empty())
                        continue;

                    // Pick the slot not currently held by g_readyIdx or g_readingIdx.
                    const int ready = g_readyIdx.load(std::memory_order_relaxed);
                    const int reading = g_readingIdx.load(std::memory_order_acquire);
                    for (int cand = 0; cand < 3; ++cand)
                    {
                        if (cand != ready && cand != reading)
                        {
                            g_writeIdx = cand;
                            break;
                        }
                    }

                    const auto t0 = std::chrono::steady_clock::now();
                    HiZBuffer &dst = g_buffers[g_writeIdx];
                    std::memset(dst.l0, 0, sizeof(dst.l0));
                    std::memcpy(dst.vp, localVp, sizeof(dst.vp));

                    uint64_t tris = 0;
                    for (const DrawCmd &cmd : localDraws)
                    {
                        const auto it = g_workerMeshes.find(cmd.meshId);
                        if (it == g_workerMeshes.end())
                            continue;
                        const std::vector<Vec3> &v = it->second.verts;
                        const size_t n = v.size();
                        for (size_t i = 0; i + 2 < n; i += 3)
                        {
                            const Vec4 p0 = transformClip(cmd.clip, v[i].x, v[i].y, v[i].z);
                            const Vec4 p1 = transformClip(cmd.clip, v[i + 1].x, v[i + 1].y, v[i + 1].z);
                            const Vec4 p2 = transformClip(cmd.clip, v[i + 2].x, v[i + 2].y, v[i + 2].z);

                            // Coarse clip-plane reject when all 3 verts lie outside one same plane.
                            const float w0 = std::fabs(p0.w), w1 = std::fabs(p1.w), w2 = std::fabs(p2.w);
                            if ((p0.x > w0 && p1.x > w1 && p2.x > w2) || (p0.x < -w0 && p1.x < -w1 && p2.x < -w2) ||
                                (p0.y > w0 && p1.y > w1 && p2.y > w2) || (p0.y < -w0 && p1.y < -w1 && p2.y < -w2) ||
                                (p0.w < 0.1f && p1.w < 0.1f && p2.w < 0.1f))
                            {
                                continue;
                            }
                            clipAndRasterizeTri(dst.l0, p0, p1, p2);
                            ++tris;
                        }
                    }

                    buildMip(dst.l0, kW0, dst.l1, kW1, kH1);
                    buildMip(dst.l1, kW1, dst.l2, kW2, kH2);
                    buildMip(dst.l2, kW2, dst.l3, kW3, kH3);
                    dst.valid = true;

                    const auto t1 = std::chrono::steady_clock::now();
                    g_lastRasterUs.store(static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count()), std::memory_order_relaxed);
                    g_lastTriCount.store(tris, std::memory_order_relaxed);

                    g_readyIdx.store(g_writeIdx, std::memory_order_release);
                }
            }

            void ensureWorkerStarted()
            {
                if (g_workerRunning)
                    return;
                g_stopRequested = false;
                g_workerRunning = true;
                g_worker = std::thread(workerMain);
            }
        }

        void registerMesh(uint64_t id, const Vertex *vertices, uint32_t count, bool perFrame)
        {
            if (perFrame || !vertices || count < kMinMeshVerts || count > kMaxMeshVerts)
                return;
            if (g_knownMeshes.count(id))
                return;
            const size_t bytes = static_cast<size_t>(count) * sizeof(Vec3);
            if (g_meshCacheBytes + bytes > kMaxMeshCacheBytes)
                return;
            for (uint32_t i = 0; i < count; ++i)
            {
                if (vertices[i].a < 124u)
                    return;
            }
            g_knownMeshes.insert(id);
            g_meshCacheBytes += bytes;

            PendingMesh pm;
            pm.id = id;
            pm.verts.resize(count);
            for (uint32_t i = 0; i < count; ++i)
                pm.verts[i] = {vertices[i].x, vertices[i].y, vertices[i].z};
            g_pendingMeshes.push_back(std::move(pm));
        }

        void registerTexture(uint64_t id, uint32_t width, uint32_t height, const uint8_t *rgba)
        {
            if (!id || !rgba || width == 0u || height == 0u)
                return;
            const size_t pixels = static_cast<size_t>(width) * height;
            for (size_t i = 0; i < pixels; ++i)
            {
                if (rgba[i * 4u + 3u] < 120u)
                    return;
            }
            g_opaqueTextures.insert(id);
        }

        void setFrameCamera(const float vpRowMajor[16])
        {
            if (g_hasCurVp && std::memcmp(g_curVp, vpRowMajor, sizeof(g_curVp)) == 0)
                return;
            std::memcpy(g_curVp, vpRowMajor, sizeof(g_curVp));
            g_hasCurVp = true;

            const int idx = g_readyIdx.load(std::memory_order_acquire);
            g_readingIdx.store(idx, std::memory_order_release);
            const HiZBuffer &buf = g_buffers[idx];
            if (buf.valid && vpContinuous(g_curVp, buf.vp))
            {
                g_activeHiZ = &buf;
                g_cutInvalidated = false;
            }
            else
            {
                g_activeHiZ = nullptr;
                g_cutInvalidated = true;
            }
        }

        void submitDraw(uint64_t meshId, const float clip[16])
        {
            if (g_stagedDraws.size() >= kMaxStageDraws || !g_knownMeshes.count(meshId))
                return;
            DrawCmd cmd;
            cmd.meshId = meshId;
            std::memcpy(cmd.clip, clip, sizeof(cmd.clip));
            g_stagedDraws.push_back(cmd);
        }

        void submitDrawMulti(const uint64_t *meshIds, uint32_t count, const float clip[16])
        {
            for (uint32_t i = 0; i < count; ++i)
                submitDraw(meshIds[i], clip);
        }

        void endFrame(bool enabled, bool logStats)
        {
            // Release reading pin at frame end so worker can freely rotate triple buffer.
            g_readingIdx.store(-1, std::memory_order_release);
            g_activeHiZ = nullptr;

            if (!enabled)
            {
                g_stagedDraws.clear();
                g_hasCurVp = false;
                g_queryTested = g_queryCulled = 0;
                return;
            }

            if (logStats && (++g_statFrames % 120u == 0u))
            {
                const uint64_t rus = g_lastRasterUs.load(std::memory_order_relaxed);
                const uint64_t tris = g_lastTriCount.load(std::memory_order_relaxed);
                std::fprintf(stderr, "[occ:hiz] tested=%llu culled=%llu worker=%.2fms tris=%llu meshes=%zu\n",
                             static_cast<unsigned long long>(g_queryTested / 120u),
                             static_cast<unsigned long long>(g_queryCulled / 120u),
                             static_cast<double>(rus) / 1000.0,
                             static_cast<unsigned long long>(tris),
                             g_knownMeshes.size());
                g_queryTested = g_queryCulled = 0;
            }

            ensureWorkerStarted();

            {
                std::lock_guard<std::mutex> lock(g_mutex);
                if (!g_pendingMeshes.empty())
                {
                    g_inboxMeshes.insert(g_inboxMeshes.end(),
                                         std::make_move_iterator(g_pendingMeshes.begin()),
                                         std::make_move_iterator(g_pendingMeshes.end()));
                    g_pendingMeshes.clear();
                }
                if (!g_workerBusy)
                {
                    g_inboxDraws.swap(g_stagedDraws);
                    g_stagedDraws.clear();
                    g_inboxHasVp = g_hasCurVp;
                    if (g_hasCurVp)
                        std::memcpy(g_inboxVp, g_curVp, sizeof(g_inboxVp));
                    g_hasJob = true;
                }
                else
                {
                    // Worker took longer than 1 frame; keep latest scene draws for next kick.
                    g_stagedDraws.clear();
                }
            }
            g_hasCurVp = false;
            g_cv.notify_one();
        }

        bool isBoxOccluded(const float modelRowMajor[16], const float lo[3], const float hi[3], float bias, float margin)
        {
            const HiZBuffer *hz = g_activeHiZ;
            if (!hz || g_cutInvalidated)
                return false;

            ++g_queryTested;

            // Compute PM = VP_prev * M (4 V4 rows, matching decomp_walk::rows convention).
            float pm[16];
            const float *vp = hz->vp;
            for (int r = 0; r < 4; ++r)
            {
                const float mx = modelRowMajor[r * 4 + 0];
                const float my = modelRowMajor[r * 4 + 1];
                const float mz = modelRowMajor[r * 4 + 2];
                const float mw = modelRowMajor[r * 4 + 3];
                for (int c = 0; c < 4; ++c)
                    pm[r * 4 + c] = vp[0 + c] * mx + vp[4 + c] * my + vp[8 + c] * mz + vp[12 + c] * mw;
            }

            float minNdcX = 1e9f, maxNdcX = -1e9f;
            float minNdcY = 1e9f, maxNdcY = -1e9f;
            float maxInvW = 0.0f;

            for (int k = 0; k < 8; ++k)
            {
                const float x = (k & 4) ? hi[0] : lo[0];
                const float y = (k & 2) ? hi[1] : lo[1];
                const float z = (k & 1) ? hi[2] : lo[2];
                const Vec4 cp = transformClip(pm, x, y, z);
                if (cp.w <= 0.25f)
                    return false;
                const float invW = 1.0f / cp.w;
                const float nx = cp.x * invW;
                const float ny = cp.y * invW;
                if (nx < minNdcX) minNdcX = nx;
                if (nx > maxNdcX) maxNdcX = nx;
                if (ny < minNdcY) minNdcY = ny;
                if (ny > maxNdcY) maxNdcY = ny;
                if (invW > maxInvW) maxInvW = invW;
            }

            // Reject giant boxes spanning most of the viewport so macro structures stay safe.
            if ((maxNdcX - minNdcX) > 1.8f || (maxNdcY - minNdcY) > 1.8f)
                return false;

            const float u0 = std::clamp(0.5f * minNdcX + 0.5f - margin, 0.0f, 1.0f);
            const float u1 = std::clamp(0.5f * maxNdcX + 0.5f + margin, 0.0f, 1.0f);
            const float v0 = std::clamp(0.5f * minNdcY + 0.5f - margin, 0.0f, 1.0f);
            const float v1 = std::clamp(0.5f * maxNdcY + 0.5f + margin, 0.0f, 1.0f);
            if (u0 >= u1 || v0 >= v1)
                return false;

            const int x0 = std::clamp(static_cast<int>(std::floor(u0 * static_cast<float>(kW0))), 0, kW0 - 1);
            const int x1 = std::clamp(static_cast<int>(std::floor(u1 * static_cast<float>(kW0))), x0, kW0 - 1);
            const int y0 = std::clamp(static_cast<int>(std::floor(v0 * static_cast<float>(kH0))), 0, kH0 - 1);
            const int y1 = std::clamp(static_cast<int>(std::floor(v1 * static_cast<float>(kH0))), y0, kH0 - 1);

            const float *levels[4] = {hz->l0, hz->l1, hz->l2, hz->l3};
            const int widths[4] = {kW0, kW1, kW2, kW3};
            int mip = 3;
            for (int l = 0; l < 3; ++l)
            {
                if (((x1 >> l) - (x0 >> l)) <= 1 && ((y1 >> l) - (y0 >> l)) <= 1)
                {
                    mip = l;
                    break;
                }
            }

            const int tx0 = x0 >> mip;
            const int tx1 = x1 >> mip;
            const int ty0 = y0 >> mip;
            const int ty1 = y1 >> mip;
            const int mw = widths[mip];
            const float *buf = levels[mip];
            const float cmpInvW = maxInvW * (1.0f + bias);

            for (int ty = ty0; ty <= ty1; ++ty)
            {
                const float *row = buf + ty * mw;
                for (int tx = tx0; tx <= tx1; ++tx)
                {
                    if (cmpInvW >= row[tx])
                        return false;
                }
            }

            ++g_queryCulled;
            return true;
        }

        void shutdown()
        {
            if (!g_workerRunning)
                return;
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                g_stopRequested = true;
            }
            g_cv.notify_one();
            if (g_worker.joinable())
                g_worker.join();
            g_workerRunning = false;
        }

        bool isOpaqueTexture(uint64_t id)
        {
            return id == 0ull || g_opaqueTextures.count(id);
        }
    }
}
