// wxl-forever lights: the suite's light service, the one list every render feature lights with.
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

#include <cstddef>
#include <cstdint>

struct IDirect3DDevice9;
struct IDirect3DTexture9;

namespace wxl::forever::passes { struct Frame; }

// Where the lights come from: the engine's M2 lights and the WMO lights files author (core's
// wxl::game::lights), the model table, which gives light to models that carry none (torches,
// lamp posts, candles: data/model-lights.csv, deployed as Extensions/wxl-forever/model-lights.csv),
// and lights a caller passes in. Gather scores them all, keeps the best kMaxLights and fades each in
// and out; Publish writes a list to two textures a shader reads (shaders/lights.hlsli): every light,
// and per coarse cluster of a camera frustum the indices of the lights that reach it. Nothing here
// depends on any feature; the fog is the first consumer.
namespace wxl::forever::lights
{
    constexpr int kMaxLights = 128;   // raise with kLightCapacity in shaders/lights.hlsli
    constexpr int kLightRows = 4;     // texel rows per light, with kLightRows in shaders/lights.hlsli

    enum class Kind : uint8_t { M2 = 0, Wmo = 1, Table = 2, Given = 3 };

    /// How a light flickers: fire breathes with layered noise, a candle trembles fast and small, a
    /// lantern barely moves, the rest are steady.
    enum class Flicker : uint8_t { None = 0, Fire = 1, Candle = 2, Lantern = 3 };

    /// How a light's output varies with direction: a lantern's cage bars, a street lamp's downward
    /// hood, a flame rising, a window's grille. Shaders apply it (lights.hlsli, LightProfile).
    enum class Profile : uint8_t { None = 0, Cage = 1, Downlight = 2, Flame = 3, Grille = 4 };

    struct Light
    {
        float position[3];     // world space
        float radius;          // nothing past it, yards
        float color[3];        // the client's working (gamma) colour
        float intensity;       // multiplies color
        float direction[3];    // spot axis; ignored for a point
        float cosCone;         // cosine of the spot half-angle; <= -1 makes it a point light
        float innerRadius;     // reference radius: where the light has its own colour
        int   interior;        // 1 for a light inside an interior space (the caller decides)
        Kind  kind;
        Flicker flicker;       // its animation, from the model table's family or its colour
        Profile profile;       // its angular profile
        float size;            // radius of the glowing source, yards: how soft its shadows are
        float extent[3];       // half the length of a tube light, world space; zero for a point
        uint8_t carried;       // 1 when its model rides another (a torch in a hand, a glow on a unit)
        const void* owner;     // identity for fading; null for a given light
        uint32_t    index;
        // The service's identity for the light, stable while its owner lives (LightId; 0 for a
        // given light), and its position before the flicker moved it, both set each frame. The
        // published list is sorted by id, so the same lights keep the same order.
        uint32_t id = 0;
        float    rest[3] = {};
        // Cookies (Cookies.hpp): the hash of the file the light belongs to (ModelTable::StemHash of
        // the M2 or WMO root stem, 0 = unknown) with which index (a table row, an M2 light, a MOLT
        // entry) names its cookie; the rotation taking a world direction into that file's frame
        // (quaternion xyzw); and its cell code once resident (0 = none): the 1-based atlas cell plus
        // 128 times the share its pattern has faded in, on 8 bits; set each frame by the service.
        uint64_t cookieSource;
        float    cookieRotation[4];
        uint16_t cookieCell;
        // What its cookie averages to, from the bake's manifest and then the file itself (set by the
        // service each frame, resident or not): the mean transmittance (-1 unknown) and the glass's tint,
        // luma-preserving, by the tint setting's share. The colour stays untinted: shaders take the
        // glass colour from the cookie, or from this mean wherever they cannot sample the cookie
        // itself, so the light keeps its colour and energy.
        float    cookieOpen = -1.0f;
        float    cookieTint[3] = { 1.0f, 1.0f, 1.0f };
        // The interior room holding it (Rooms.hpp, set each frame by the service; -1 outside every
        // room) and that room's floor and ceiling heights at the light, world z.
        int      room = -1;
        float    roomFloor, roomCeiling;
        // The reach and reference radius its fog halo keeps when a merge gave it a larger reach
        // (Gather: one light per fixture); 0 = the halo uses radius and innerRadius.
        float    haloRadius = 0.0f, haloInner = 0.0f;
    };

    struct Options
    {
        bool  engine = true;    // gather the client's lights at all
        bool  wmo    = true;    // include the WMO lights files author
        bool  table  = true;    // include the model table's lights
        float radius = 60.0f;   // around the camera, yards
        float flicker = 1.0f;   // strength of every light's flicker; 0 steady
        float carriedCore = 0.8f;  // soft core of a carried light (a held torch), yards
        float nearCap = 2.5f;      // most any light gives near its source, times its colour (5 = uncapped)
        float hotCore = 0.35f;     // how much a flame's light whitens near its source (0 off), on surfaces and in the fog
        float maxPeak = 3.0f;      // brightest linear channel any light may publish (its colour times intensity)
        float maxRadius = 30.0f;   // widest reach any light may publish, yards
        float roomLeak = 0.1f;     // share of a room's light that reaches another floor of the building
        bool  rooms = true;        // keep a light in its room (off: the isolate)
        bool  merge = true;        // one light per fixture: lights of different sources at one lamp merge
        bool  mergeReach = true;   // the merged light takes the larger reach and brightness of the two
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

    /// One line: the collected light nearest the camera, its kind, radius, colour, and whether it was chosen.
    const char* Nearest();

    // --- clusters ---------------------------------------------------------------------------------

    constexpr int kClustersX = 16, kClustersY = 9, kClustersZ = 16;
    constexpr int kClusterTexW = 256;   // texels across the cluster texture, clusterC.w in shaders/lights.hlsli

    /// A camera frustum sliced exponentially in depth, as the clusters divide it.
    struct ClusterSpace
    {
        const float* viewProjRel;   // camera-relative view * projection, row vectors
        float        eye[3];
        float        nearDistance;  // first slice
        float        logRatio;      // log(far / near)
    };

    /**
     * @brief Writes a list to the light and cluster textures.
     *
     * The light texture is kMaxLights x kLightRows texels (A32B32G32R32F), one column per light:
     * (position - eye, radius, negative for an interior light) (linear colour * intensity, cosCone)
     * (spot axis, or a tube's half length when the fourth row says so, or for a point light its fog
     * halo's reach and reference radius, 0 when the halo takes the light's own; reference radius)
     * (source size, profile, 1 = tube, flags: 1 an engine model light the engine already applies to
     * models, + 2 a light carried by another model, + 4 times the mask of the rooms it may light:
     * bit b for room b of rooms::Rows, see shaders/lights.hlsli RoomGate). The cluster texture
     * (A32B32G32R32F, kClusterTexW wide, every value exact) holds, in rows: one texel per cluster
     * (x + y * kClustersX + z * kClustersX * kClustersY): its list's offset and length; then the
     * cookie rows (shaders/cookies.hlsli): per light, its rotation quaternion (xyzw), then its cell
     * code and its cookie's mean transmittance rgb (0 = no cookie known), light i at column i % width,
     * one block of ceil(kMaxLights / width) rows per attribute; then the pool of every cluster's
     * light indices, four to a texel, sized so that every light fits in every cluster: no list is
     * ever cut.
     * @return false when the textures cannot be created on this device.
     */
    bool Publish(IDirect3DDevice9* dev, const Light* lights, int count, const ClusterSpace& space);

    IDirect3DTexture9* LightTexture();
    IDirect3DTexture9* ClusterTexture();
    int ClusterTextureWidth();
    int ClusterTextureHeight();
    int Published();            // lights in the last Publish
    int ClusterPeak();          // the longest cluster list of the last Publish
    int ClusterEntries();       // light indices in all the lists of the last Publish

    /// One omni shadow map, as surface lighting chose and the core rendered it this frame: the index
    /// in this frame's list of the light it belongs to, its shadow's share (it fades as a slot
    /// changes hands), where it was rendered from (camera-relative) and its radius, its atlas and
    /// face size, and per face three rows taking a camera-relative (x, y, z, 1) to (u * w, v * w, w)
    /// (zero for a face not rendered).
    struct OmniSlot
    {
        int                lightIndex;
        float              weight;
        float              position[3];
        float              radius;
        float              faceSize;
        IDirect3DTexture9* texture;
        float              rows[6][3][4];
    };

    /**
     * @brief Publishes this frame's omni slots: the one table every consumer reads a light's slot from.
     *
     * The table is kMaxLights x 2 texels (A32B32G32R32F): row 0, per light index, (slot + 1, the slot's
     * share, 0, 0), zero for a light without a map (shaders/lights.hlsli, OmniSlotOf); row 1, 20 texels a
     * slot: (position, radius) (1 / atlas width, 1 / atlas height, face size, own-housing yards) then
     * 3 rows a face. count 0 clears it.
     */
    void PublishOmni(IDirect3DDevice9* dev, const OmniSlot* slots, int count, float selfSkip, uint32_t frame);

    /// The table of the last PublishOmni, the slots and the frame they were published for.
    IDirect3DTexture9* OmniTexture();
    const OmniSlot* CurrentOmni(int& count, uint32_t& frame);

    /// The self-check: light i as a shader reads it back from the last Publish (its columns, its
    /// cluster at its own place on screen, the texture layout) beside source, the light it came from.
    void DescribePublished(int i, const Light* source, char* out, size_t size);

    /// Frees the DEFAULT-pool textures before a device reset.
    void ReleaseTextures();

    /// Reads the service's config, loads nothing yet, registers the lights panel. Called once at load.
    void Install();

    /// The feature's parts of the Forever window (core/ForeverUi.cpp): its line in Overview, its own
    /// tab, and its group in Debug.
    void UiOverview();
    void UiSettings();
    void UiDebug();

    // --- the frame's list, shared by every consumer -------------------------------------------------

    /// What the service gathers, from WXL_FOREVER_LIGHTS_GATHER_*.
    Options& Settings();

    /// The linear colour and reach a light is published with (the brightest channel and the reach
    /// bounded by the options); returns the brightest channel it had unbounded, as before the bound.
    float PublishedColour(const Light& l, float rgb[3], float& radius);

    /// Lights given by a feature (a debug lantern, lights another extension fed), replacing the
    /// last set. They go first every frame until replaced. Call on the tick.
    void SetGiven(const Light* lights, int count);

    /// Whether a light in a room counts as interior (Light::interior = its room >= 0). The fog turns it
    /// on while it separates indoor from outdoor; off, every light is outdoor.
    void SetInteriorGate(bool on);

    /// Gathers and publishes this frame's list once, whichever consumer asks first; the others get
    /// the same list. False when the device cannot take the textures.
    bool Frame(const passes::Frame& frame);

    /// This frame's list, as the last Frame published it.
    const Light* Current(int& count);

    /// A light that moved lately (a torch carried, a missile's glow), measured before the flicker:
    /// world position and radius. Consumers shorten their histories around it.
    struct Moving
    {
        float position[3];
        float radius;
    };

    /// The lights that moved more than a few centimetres within the last few frames, nearest the
    /// camera first (by distance less radius); at most max. withCarried adds every carried light,
    /// moving or not: what it lights changes with the carrier's pose and the view.
    int MovingLights(Moving* out, int max, bool withCarried);

    /// Shader constants for a carried light's core, the near cap and the hot core: (core yards, cap, hot, 0).
    void CarriedConstants(float out[4]);

    /// The share of a room's light that reaches another floor (lights.hlsli, RoomGate); 1 with the
    /// room gating off.
    float RoomCross();

    /// A stable nonzero id for a light, from its owner, index and kind (0 for a light without an
    /// owner); the list carries it in Light::id.
    uint32_t LightId(const Light& l);

    /// The fixed space the clusters divide, the same for every consumer: from kClusterNear to
    /// kClusterFar yards, exponential in depth.
    constexpr float kClusterNear = 0.5f;
    constexpr float kClusterFar  = 250.0f;

    /// Shader constants for lights.hlsli: clusterC (clusters across, down, deep, texture width) and clusterD
    /// (1 / texture width, 1 / texture height, near distance, 1 / log(far / near)).
    void ClusterConstants(float clusterC[4], float clusterD[4]);
}
