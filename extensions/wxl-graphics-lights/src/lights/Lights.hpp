// wxl-graphics-lights: the light service inside the extension, the one list every consumer lights with.
// Copyright (C) 2026 WarcraftXL
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program. If not, see <https://www.gnu.org/licenses/>.

#pragma once

#include "wxl/GraphicsLightsApi.h"
#include "wxl/GraphicsLightsSourcesApi.h"

#include <cstddef>
#include <cstdint>

struct IDirect3DDevice9;
struct IDirect3DTexture9;

// One list, two views. Light carries the legacy fields the fog was tuned on (the working colour,
// intensity and radius of GraphicsLightsApi.h) and the HDR model of docs/design.md, section 3 (family,
// linear chroma, scene-referred intensity, reach, soft core). The C ABI (src/Module.cpp) is a thin skin:
// WXL_GfxLight and WXL_GfxLightSource are converted at the boundary (ToAbi, ToSource, FromGiven).
namespace wxl::gfx::lights
{
    constexpr int kMaxLights = WXL_GFX_LIGHTS_MAX;   // raise with kLightCapacity in shaders/wxl/lights/lights.hlsli
    constexpr int kLightRows = WXL_GFX_LIGHT_ROWS;   // texel rows per light in the legacy light texture
    constexpr int kSourceRows = WXL_GFX_LIGHT_SOURCE_ROWS;

    enum class Kind : uint8_t { M2 = WXL_GFX_LIGHT_KIND_M2, Wmo = WXL_GFX_LIGHT_KIND_WMO, Table = WXL_GFX_LIGHT_KIND_TABLE, Given = WXL_GFX_LIGHT_KIND_GIVEN };

    /// How a light flickers: fire breathes with layered noise, a candle trembles fast and small, a
    /// lantern barely moves, the rest are steady.
    enum class Flicker : uint8_t { None = 0, Fire = 1, Candle = 2, Lantern = 3 };

    /// How a light's output varies with direction (lights.hlsli, LightProfile).
    enum class Profile : uint8_t { None = 0, Cage = 1, Downlight = 2, Flame = 3, Grille = 4 };

    struct Light
    {
        // --- legacy (GraphicsLightsApi.h) -------------------------------------------------------------
        float position[3];     // world space, the flicker's jitter included once animated
        float radius;          // legacy reach, yards
        float color[3];        // the client's working (gamma) colour
        float intensity;       // multiplies color (legacy, the fade and flicker folded in)
        float direction[3];    // spot axis; ignored for a point
        float cosCone;         // cosine of the spot half-angle; <= -1 makes it a point light
        float innerRadius;     // legacy reference radius
        int   interior;        // 1 for a light inside an interior space (with the interior gate on)
        Kind  kind;
        Flicker flicker;
        Profile profile;
        float size;            // radius of the glowing source, yards
        float extent[3];       // half the length of a tube light, world space; zero for a point
        uint8_t carried;       // 1 when its model rides another (a torch in a hand)
        const void* owner;     // identity for fading; null for a given light
        uint32_t    index;
        uint32_t id = 0;       // LightId, stable while its owner lives (0 for a given light)
        float    rest[3] = {}; // position before the flicker moved it
        uint64_t cookieSource; // table::StemHash of the file the light belongs to (0 = unknown)
        float    cookieRotation[4];
        uint16_t cookieCell;   // 1-based atlas cell + 128 x its faded-in share (0 = none)
        float    cookieOpen = -1.0f;
        float    cookieTint[3] = { 1.0f, 1.0f, 1.0f };
        int      room = -1;    // the interior room holding it, in rooms::Rows order
        float    roomFloor, roomCeiling;
        float    haloRadius = 0.0f, haloInner = 0.0f;

        // --- HDR (GraphicsLightsSourcesApi.h), set by the gather and the frame -------------------------
        uint8_t  family = WXL_GFX_LIGHT_FAMILY_TINT;
        float    chroma[3] = { 1.0f, 1.0f, 1.0f };   // linear, luminance 1 (Kelvin, adapted, or a tint)
        float    kelvin = 0.0f;                       // 0 for a tint
        float    power = 0.0f;                        // scene units at one yard, before fade and flicker
        float    fade = 1.0f;                         // 0..1 importance fade
        float    flickerGain = 1.0f;                  // this frame's flicker factor
        float    reach = 0.0f;                        // HDR window end, yards
        float    softRadius = 0.1f;                   // yards
        float    emissiveRadius = 0.1f;               // yards around the source that glow
        float    roomGate = 0.0f;                     // 0..1 how much its room gate applies
        uint32_t roomMask = 0;                        // rooms it may light (bit b = rooms::Rows room b)
        int      shadowSlot = -1;                     // wxl-graphics-shadow's slot as last known
    };

    struct Options
    {
        bool  engine = true;    // gather the client's lights at all
        bool  wmo    = true;    // include the WMO lights files author
        bool  table  = true;    // include the model table's lights
        float radius = 70.0f;   // around the camera, yards
        float flicker = 1.0f;   // strength of every light's flicker; 0 steady
        float carriedCore = 1.0f;  // soft core of a carried light (a held torch), yards
        float nearCap = 2.5f;      // legacy: most any light gives near its source (fog only)
        float hotCore = 0.25f;     // how much a flame's light whitens near its source
        float maxPeak = 3.0f;      // legacy: brightest linear channel published to the fog
        float maxRadius = 30.0f;   // widest reach any light may publish, yards (legacy and HDR)
        float roomLeak = 0.1f;     // share of a room's light that reaches another floor
        bool  rooms = true;        // keep a light in its room (off: the isolate)
        bool  merge = true;        // one light per fixture
        bool  mergeReach = true;   // the merged light takes the larger legacy reach of the two
        // The HDR model.
        float gain = 1.0f;         // every lamp's intensity times this
        float adaptation = 0.35f;  // share a Kelvin colour moves towards white (the eye's adaptation)
        float cutoff = 0.01f;      // irradiance where a light's reach ends, scene units
        bool  legacyChroma = true; // the legacy texture's colours take the families' chroma
    };

    /**
     * @brief Builds this frame's list.
     * @param given       lights that always go first, unfaded (a debug lantern, lights fed by others).
     * @param dt          seconds since the last call, for the fades.
     * @return how many lights were written to out, at most kMaxLights.
     */
    int Gather(const float eye[3], const Light* given, int givenCount, float dt, const Options& options,
               Light out[kMaxLights]);

    /// One line: found, of which WMO, table and carried, and used.
    const char* Status();

    /// One line: the collected light nearest the camera.
    const char* Nearest();

    /// Sets the HDR model of a light from its family: chroma, power, soft core, reach.
    void ApplyFamily(Light& l, const Options& options);

    // --- clusters ---------------------------------------------------------------------------------

    constexpr int kClustersX = WXL_GFX_CLUSTERS_X, kClustersY = WXL_GFX_CLUSTERS_Y, kClustersZ = WXL_GFX_CLUSTERS_Z;
    constexpr int kClusterTexW = WXL_GFX_CLUSTER_TEX_W;

    /// A camera frustum sliced exponentially in depth, as the clusters divide it.
    struct ClusterSpace
    {
        const float* viewProjRel;   // camera-relative view * projection, row vectors
        float        eye[3];
        float        nearDistance;  // first slice
        float        logRatio;      // log(far / near)
    };

    /**
     * @brief Writes a list to the legacy light and cluster textures and the HDR source texture.
     *
     * The legacy layouts are GraphicsLightsApi.h's. A light joins every cluster its larger reach (legacy
     * or HDR) touches, so every consumer finds it. The source texture follows
     * shaders/wxl/lights/sources.hlsli. All are D3DPOOL_DEFAULT, filled from a system-memory twin.
     * @return false when the textures cannot be created on this device.
     */
    bool Publish(IDirect3DDevice9* dev, const Light* lights, int count, const ClusterSpace& space);

    IDirect3DTexture9* LightTexture();
    IDirect3DTexture9* ClusterTexture();
    IDirect3DTexture9* SourceTexture();
    int ClusterTextureWidth();
    int ClusterTextureHeight();
    int Published();            // lights in the last Publish
    int ClusterPeak();          // the longest cluster list of the last Publish
    int ClusterEntries();       // light indices in all the lists of the last Publish

    /// Publishes this frame's omni slots (the legacy table, GraphicsLightsApi.h OmniTexture).
    void PublishOmni(IDirect3DDevice9* dev, const WXL_GfxOmniSlot* slots, int count, const float* housing,
                     const float* receiverSkip, uint32_t frame);

    IDirect3DTexture9* OmniTexture();
    const WXL_GfxOmniSlot* CurrentOmni(int& count, uint32_t& frame);

    /// The self-check: light i as a shader reads it back from the last Publish, beside its source.
    void DescribePublished(int i, const Light* source, char* out, size_t size);

    /// Frees the light, cluster, source and omni textures before a device reset.
    void ReleaseTextures();

    // --- the frame's list, shared by every consumer -------------------------------------------------

    /// Reads the service's config (WXL_GFX_LIGHTS_*). Called once at load.
    void Install();

    Options& Settings();

    /// The legacy linear colour and reach a light is published with; returns the brightest channel it
    /// had unbounded.
    float PublishedColour(const Light& l, float rgb[3], float& radius);

    /// The HDR intensity rgb of a light this frame: chroma x power x gain x fade x flicker.
    void SourceIntensity(const Light& l, float rgb[3]);

    /// The luminance of a light's head (its glass or flame), scene units: what bloom finds later.
    float SourceEmissive(const Light& l);

    void SetGiven(const void* key, const WXL_GfxLight* lights, int count);
    void SetInteriorGate(bool on);

    /// A consumer will read the list this frame and the next.
    void Want();

    /// Polled once per world frame: whether Want() was called during this frame or the previous one.
    bool PollWanted();

    /// Gathers and publishes the list of frame `index` once (a second call in the same frame is a
    /// no-op). False when the device cannot take the textures.
    bool Frame(IDirect3DDevice9* dev, uint32_t index);

    const Light* Current(int& count);
    const WXL_GfxLight* CurrentAbi(int& count, uint32_t& frame);
    const WXL_GfxLightSource* CurrentSources(int& count, uint32_t& frame);

    /// The eye the last Frame published around (the textures' positions are relative to it).
    const float* PublishedEye();

    using Moving = WXL_GfxMovingLight;
    int MovingLights(Moving* out, int max, bool withCarried);

    void CarriedConstants(float out[4]);
    float RoomCross();

    /// A stable nonzero id for a light, from its owner, index and kind (0 for a light without an
    /// owner). The id space wxl-graphics-shadow keys its slots on, and the core's omni slots carry.
    inline uint32_t LightId(const Light& l)
    {
        if (!l.owner) return 0;
        const uint32_t id = uint32_t(reinterpret_cast<uintptr_t>(l.owner) >> 2) * 2654435761u ^ (l.index + 1u) * 40503u
                          ^ (uint32_t(l.kind) + 1u) * 0x9E3779B1u;
        return id ? id : 1u;
    }

    constexpr float kClusterNear = WXL_GFX_CLUSTER_NEAR;
    constexpr float kClusterFar  = WXL_GFX_CLUSTER_FAR;

    void ClusterConstants(float clusterC[4], float clusterD[4]);

    void ToAbi(const Light& l, WXL_GfxLight& out);
    void ToSource(const Light& l, int index, WXL_GfxLightSource& out);
    Light FromGiven(const WXL_GfxLight& in);

    void OnDeviceLost();
}
