// Event bus: the readable hook surface modules subscribe to. The core owns the detours.
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

#include <cstdint>

/// Event bus: the core installs the detours and republishes them as named events. A module
/// Subscribe()s and never sees an address. A subscriber is a plain function pointer plus an opaque
/// user, so Emit() walks a flat vector with no std::function or vtable on the path. Each event
/// carries a typed POD args struct by const pointer.
namespace wxl::events
{
    /// Named events a module may subscribe to; each carries the args struct noted alongside it.
    enum class Event : uint32_t
    {
        OnModelLoadPre,  // a model's raw bytes are read, before parse (ModelLoadArgs)
        OnModelLoad,     // a model finished loading and is parsed     (ModelLoadArgs)
        OnM2SkinFinalize,// a model's skin profile is being finalized  (M2SkinFinalizeArgs)
        OnM2PerFrameUpdate, // per-render-ctx scene-graph update tick  (M2PerFrameUpdateArgs)
        OnBuildBonePalette, // bone-palette fill for one instance, post-engine (BuildBonePaletteArgs)
        OnFrame,         // per-frame Present                          (FrameArgs)
        OnEndScene,      // end of the frame, before present           (EndSceneArgs)
        OnDeviceLost,    // before IDirect3DDevice9::Reset frees DEFAULT resources (DeviceResetArgs)
        OnDeviceReset,   // after a successful IDirect3DDevice9::Reset  (DeviceResetArgs)
        OnUpdate,        // once-per-frame logic tick, with delta time (UpdateArgs)
        OnWorldRender,   // per-frame world draw pass                  (WorldRenderArgs)
        OnWorldRenderEnd,// world -> UI boundary, the post-fx slot     (WorldRenderEndArgs)
        OnWorldSceneEnd, // world drawn, camera matrices still current (WorldSceneEndArgs)
        OnLiquidRender,  // a liquid render pass is about to draw       (LiquidRenderArgs)
        OnM2BatchDraw,   // one M2 triangle batch is drawing           (M2BatchDrawArgs)
        OnM2SetupBatchAlpha, // an M2 batch's alpha/material is set up (M2SetupBatchAlphaArgs)
        OnRibbonDraw,    // a ribbon emitter is about to draw          (RibbonDrawArgs)
        OnInput,         // window input message (swallowable)         (InputArgs)
        OnWorldClick,    // a world click resolved to a point/object   (WorldClickArgs)
        OnAdtChunkBuild, // an ADT map chunk is being built            (AdtChunkArgs)
        OnAdtSplitTileLoad, // a split ADT tile finished its 3-file load (AdtSplitTileLoadArgs)
        OnWmoRootLoad,   // a WMO root buffer is read, before the walk (WmoRootLoadArgs)
        OnWmoGroupLoad,  // a WMO group buffer is read, before the walk(WmoGroupLoadArgs)
        OnTextureUpload, // a texture is about to upload to the device (TextureUploadArgs)
        OnBlpLoad,       // a BLP texture was requested by name           (BlpLoadArgs)
        OnObjectUpdate,  // a server object create/update batch parsed     (ObjectUpdateArgs)
        OnObjectDestroy, // an object is about to despawn                  (ObjectDestroyArgs)
        OnTargetChanged, // the player's target was set via the API        (TargetChangedArgs)
        OnSoundPlay,     // a UI/world sound is about to play              (SoundPlayArgs)
        OnDoodadSpawn,   // a placed map doodad object was built       (DoodadSpawnArgs)
        OnItemSlotChange,// a character model slot received an item    (ItemSlotChangeArgs)
        OnItemSlotClear, // a character model equipment slot was cleared(ItemSlotClearArgs)
        OnWorldEnter,    // the world/map finished loading, in-world   (WorldEnterArgs)
        OnWorldLeave,    // the world/map is being torn down           (WorldLeaveArgs)
        OnGrassWind,     // grass wind integrator advanced this frame (GrassWindArgs)
        OnAdtHeightBlend,// a terrain PS permutation was patched for height blending (AdtHeightBlendArgs)
        OnM2NativeLoad,  // a modern MD21 model was direct-filled by the native reader (M2NativeLoadArgs)
        OnPacketReceived,// an inbound server message reached the dispatcher (PacketReceivedArgs)
        OnWorldSceneBegin, // world pass about to draw; depth may be redirected (WorldSceneBeginArgs)
        Count
    };

    // Typed args, passed by const void* and reinterpreted by the subscriber for its event.

    /** @brief Args for OnModelLoadPre and OnModelLoad. */
    struct ModelLoadArgs      { void* model; };
    /**
     * @brief Args for OnM2SkinFinalize, fired before the native finalize sizes its parallel batch
     *        blocks; the window to rebuild a material/texunit contract a modern skin omits (read the
     *        header and skin via wxl::game::m2::Header / Skin). The header arrays are raw pointers here.
     */
    struct M2SkinFinalizeArgs { void* model; };
    /**
     * @brief Args for OnM2NativeLoad, fired after the native MD21 reader (features/m2native)
     *        direct-filled a modern model through the stock runtime. version is the model's modern
     *        inner version (272-274, kept resident); texturesResolved/-Unresolved count the TXID
     *        FileDataID resolutions of this load; skipMask flags the parked modern payloads
     *        (see features/m2native/M2Native.cpp kSkip* bits).
     */
    struct M2NativeLoadArgs
    {
        void*    model;
        uint32_t version;
        uint32_t texturesResolved;
        uint32_t texturesUnresolved;
        uint32_t skipMask;
    };
    /** @brief Args for OnFrame. */
    struct FrameArgs          { void* device; };
    /** @brief Args for OnUpdate, fired once per frame with the frame delta and timestamp. */
    struct UpdateArgs         { float dt; uint32_t timeMs; };
    /** @brief Args for OnEndScene. */
    struct EndSceneArgs       { void* device; };
    /**
     * @brief Args for OnDeviceLost (fired before the engine resets the D3D9 device -- e.g. a window resize --
     *        while every D3DPOOL_DEFAULT resource must be released) and OnDeviceReset (fired after a successful
     *        reset, so a subscriber recreates them). params is the D3DPRESENT_PARAMETERS* the reset (re)creates with.
     */
    struct DeviceResetArgs    { void* device; void* params; };
    /** @brief Args for OnWorldRender. */
    struct WorldRenderArgs    { void* device; };
    /**
     * @brief Args for OnWorldRenderEnd: the world -> UI boundary of the frame. A subscriber draws
     *        post-world effects here, before the client renders the interface on top.
     */
    struct WorldRenderEndArgs { void* device; };
    /**
     * @brief Args for OnWorldSceneEnd: the world is drawn and the camera matrices that drew it are
     *        still on the device. Its caller puts the pre-world projection and view back immediately
     *        afterwards, so this is the only point at which a subscriber can place geometry by world
     *        coordinate and have it land where those coordinates say. OnWorldRenderEnd is past that
     *        restore, and is the slot for screen-space work instead.
     *
     *        sceneDepth is the depth-stencil surface the world was drawn into, captured before the
     *        pass ran; it may be null when none was bound. Whatever is bound by the time this fires is
     *        not reliably that surface -- a post-process pass inside the call can leave its own -- and
     *        depth-testing against the wrong one rejects every pixel without reporting an error. When an
     *        OnWorldSceneBegin subscriber redirected the pass, sceneDepth is that override, and it is no
     *        longer bound, so it can be sampled.
     *
     *        normalTarget is the render-target 1 surface a Begin subscriber supplied, when the core bound
     *        it for this pass (cleared to 0, then written by the shaders that output oC1); null when none
     *        was supplied or the core declined it. It is unbound by now.
     *
     *        sceneColor is the colour override a Begin subscriber supplied, when the core used it (unbound
     *        by now; null otherwise). The world then never reached the back buffer: a subscriber must
     *        write it (tonemap) during this event and set *colorResolved = 1, or the core copies the
     *        override across itself (a plain StretchRect, no tonemap). colorResolved is never null.
     */
    struct WorldSceneEndArgs  { void* device; void* sceneDepth; void* normalTarget; void* sceneColor; int* colorResolved; };
    /**
     * @brief Args for OnWorldSceneBegin: the world pass is about to draw.
     *
     *        sceneDepth is the depth-stencil surface bound before the pass (never null here: the event
     *        does not fire without one). A subscriber that needs the world's depth in a surface of its
     *        own -- one it can sample afterwards -- writes that surface to *depthOverride. The core
     *        then runs the whole pass against it, including every render-target bind the engine makes
     *        inside the pass, clears it the way the engine clears its own (Z 1.0, stencil 0) and puts
     *        the engine's surface back once the pass returns. The override must match sceneDepth's
     *        size and multisample type. One owner: the last subscriber to write wins.
     *
     *        OnWorldSceneEnd then reports the override as its sceneDepth, already unbound.
     *        depthOverride is never null and starts null.
     *
     *        normalTarget: a subscriber may write an IDirect3DSurface9* render target (A8R8G8B8, the
     *        size of the world's render target 0, not multisampled) to it. The core clears it to 0 and
     *        binds it as render target 1 whenever the world's render target 0 is bound during the pass
     *        (never while the engine draws into another target), and lets a draw write it only when the
     *        bound pixel shader outputs oC1 and alpha blending is off. Never null, starts null.
     *
     *        colorOverride: a subscriber may write an IDirect3DSurface9* render target (typically
     *        A16B16G16R16F, the back buffer's size and multisample type). The core then runs the whole
     *        world pass into it instead of the back buffer, the way depthOverride works, and hands it
     *        back in OnWorldSceneEnd, where the subscriber must resolve it to the back buffer. Never null.
     */
    struct WorldSceneBeginArgs { void* device; void* sceneDepth; void** depthOverride; void** normalTarget; void** colorOverride; };
    /**
     * @brief Args for OnLiquidRender, fired before the native liquid pass draws. passType is 0 for the
     *        main pass, 1 for the secondary; instanceCount is the visible liquid instances in this pass;
     *        transform is the shared liquid transform forwarded to each instance. Read-only.
     */
    struct LiquidRenderArgs  { void* bank; void* transform; int passType; uint32_t instanceCount; };
    /**
     * @brief Args for OnM2BatchDraw, fired just after the native draw with the same draw parameters,
     *        so a subscriber can re-issue the draw while the vertex/index buffers are still bound.
     */
    struct M2BatchDrawArgs
    {
        void*    device;
        void*    model;
        int      primType;
        int      baseVertex;
        uint32_t minIndex;
        uint32_t numVerts;
        uint32_t startIndex;
        uint32_t primCount;
    };
    /**
     * @brief Args for OnM2SetupBatchAlpha, fired just after the native setter chose the alpha-test
     *        reference from the blend mode; a subscriber may re-push the reference
     *        (wxl::game::m2::PushAlphaRef) for the model and blend mode it recognizes. blendMode 1 is
     *        alpha key. model may be null, in which case the subscriber skips.
     */
    struct M2SetupBatchAlphaArgs { void* model; uint16_t blendMode; };
    /**
     * @brief Args for OnRibbonDraw, fired before the native draw; a subscriber sets *useMultiTexture
     *        true to request the single-pass multi-texture combine for a ribbon of >= 3 layers (the
     *        core binds the extra layers and folds the passes into one). useMultiTexture is never null
     *        and starts false.
     */
    struct RibbonDrawArgs { void* emitter; uint32_t layerCount; bool* useMultiTexture; };
    /**
     * @brief Args for OnInput; a subscriber that consumes the message sets *handled true, which makes
     *        the core swallow it so the game does not also react. handled is never null. Args are
     *        otherwise read-only.
     */
    struct InputArgs         { uint32_t message; uintptr_t wparam; uintptr_t lparam; bool* handled; };
    /**
     * @brief Args for OnWorldClick, fired on a mouse button when the cursor ray hits the world (not when
     *        the click was consumed by a UI subscriber). hitType is 2 for an M2/doodad, 3 for terrain or
     *        WMO; x/y/z is the world hit point; objLo/objHi is the engine object handle (zero for terrain).
     */
    struct WorldClickArgs    { uint32_t message; int hitType; float x; float y; float z; void* objLo; void* objHi; };
    /** @brief Args for OnAdtChunkBuild. */
    struct AdtChunkArgs      { void* chunk; uint32_t layerCount; };
    /**
     * @brief Args for OnAdtSplitTileLoad, fired on the main thread after a split ADT tile's
     *        root/_tex0/_obj0 trio finished loading and the stock tile parser ran over the direct-fill
     *        state. tileFirst/tileSecond are the two %d of the "<Map>_%d_%d.adt" name; sizes are the
     *        resident raw file buffer sizes (0 when that split file was absent); chunkCount is the
     *        number of MCNKs indexed from the root (256 on a well-formed tile). Read-only.
     */
    struct AdtSplitTileLoadArgs
    {
        int      tileFirst;
        int      tileSecond;
        uint32_t rootSize;
        uint32_t texSize;
        uint32_t objSize;
        uint32_t chunkCount;
    };
    /**
     * @brief Args for OnWmoRootLoad, fired after the WMO root buffer is read and before the native chunk
     *        walk; the window to reshape the root in place (read/replace the buffer via wxl::game::wmo).
     */
    struct WmoRootLoadArgs   { void* root; };
    /**
     * @brief Args for OnWmoGroupLoad, fired after a WMO group buffer is read and before the native
     *        sub-chunk walk; the window to reshape the group in place (read/replace via wxl::game::wmo).
     */
    struct WmoGroupLoadArgs  { void* group; };
    /** @brief Args for OnTextureUpload. */
    struct TextureUploadArgs { void* texture; uint32_t width; uint32_t height; };
    /**
     * @brief Args for OnBlpLoad, fired after a by-name texture request resolves. name is the full virtual
     *        path requested (match case-insensitively, slash-normalized); handle is the resolved texture
     *        handle (may be null on failure). Fires on every reference, cached or not. Read-only.
     */
    struct BlpLoadArgs       { const char* name; void* handle; };
    /**
     * @brief Args for OnObjectUpdate, fired after a server update message is parsed (a batch of objects
     *        created or field-updated). packet is the inbound message reader (cursor already consumed);
     *        opcode is the message opcode. Read-only.
     */
    struct ObjectUpdateArgs  { void* packet; int opcode; };
    /**
     * @brief Args for OnObjectDestroy, fired before an object despawns while it is still resident. packet
     *        holds the object GUID and an on-death flag; opcode is the message opcode. Read-only.
     */
    struct ObjectDestroyArgs { void* packet; int opcode; };
    /**
     * @brief Args for OnTargetChanged, fired after the target-set API applied a new target. scriptState is
     *        the script state the call ran on; read the applied target GUID via wxl::game (kTargetGuid).
     */
    struct TargetChangedArgs { void* scriptState; };
    /**
     * @brief Args for OnSoundPlay, fired before a UI/world sound plays. scriptState is the script state
     *        the call ran on; the sound id/name is on its stack. Read-only.
     */
    struct SoundPlayArgs     { void* scriptState; };
    /** @brief Args for OnDoodadSpawn; read the transform via wxl::game::doodad. */
    struct DoodadSpawnArgs   { void* doodad; };
    /** @brief Args for OnItemSlotChange; charModelObj is the CharModelObject, modelSlot is the internal
     *         model slot index (maps to an equipment category), itemDataPtr points to the item data block. */
    struct ItemSlotChangeArgs { void* charModelObj; uint32_t modelSlot; void* itemDataPtr; };
    /** @brief Args for OnItemSlotClear; charModelObj is the CharModelObject, equipSlotWow is the
     *         WoW equipment slot index (EQUIPMENT_SLOT_* constants, 0-18). */
    struct ItemSlotClearArgs  { void* charModelObj; uint32_t equipSlotWow; };
    /** @brief Args for OnM2PerFrameUpdate; renderCtx is the per-instance render context that the
     *         scene graph is updating -- fires once per visible M2 instance per frame. */
    struct M2PerFrameUpdateArgs { void* renderCtx; };
    /** @brief Args for OnBuildBonePalette; fires after the engine fills the per-instance bone palette
     *         from the current animation pose, immediately before the batch draw uploads it to the
     *         vertex shader. renderCtx is the M2Instance whose bone palette was just written.
     *         Subscribers may overwrite bone matrices here to override the engine's pose. */
    struct BuildBonePaletteArgs { void* renderCtx; };
    /** @brief Args for OnWorldEnter. */
    struct WorldEnterArgs    { uint32_t mapId; };
    /** @brief Args for OnWorldLeave. */
    struct WorldLeaveArgs    { uint32_t mapId; };
    /**
     * @brief Args for OnGrassWind, emitted once per frame when the grass-wind integrator advances (only
     *        while grass is drawing -- the tick lives on the detail-doodad chunk pass). dirX/dirY is the
     *        damped current wind vector, strength its magnitude scalar, phase the accumulated sway phase
     *        in radians. Read-only observation; runtime tuning goes through wxl.wind.* (methods).
     */
    struct GrassWindArgs     { float dirX; float dirY; float strength; float phase; };
    /**
     * @brief Args for OnAdtHeightBlend, fired on the draw thread the first time a stock terrain
     *        pixel-shader permutation is patched with the height-blend formula (once per stock
     *        permutation per session). layerCount is the permutation's texture layer count (2..4);
     *        stockBytes/patchedBytes are the bytecode sizes before/after the injection. Read-only.
     */
    struct AdtHeightBlendArgs { uint32_t layerCount; uint32_t stockBytes; uint32_t patchedBytes; };
    /**
     * @brief Args for OnPacketReceived: one inbound server message, before the client dispatches it.
     *
     * Published from NetClient::ProcessMessage, which NETEVENTQUEUE::Poll drains on the main thread,
     * so a handler runs on the game thread with the message still intact.
     *
     * It fires for EVERY opcode, including the ones the client has no handler for. That matters: the
     * dispatcher only consults its handler table for opcodes below 0x51F, and quietly discards
     * everything else through CDataStore::Reset. A custom opcode a server and a patched client agree
     * on is only reachable here.
     *
     * packet is the message's CDataStore with its read cursor positioned past the opcode, so a
     * subscriber reads the payload from byte zero through wxl::game::net. The cursor is restored
     * before the client's own handler runs, so reading here costs the stock path nothing.
     *
     * Setting *handled true suppresses the client's own dispatch of this message and nothing else.
     * handled is never null and starts false. connection is the WowConnection it arrived on, which is
     * not necessarily the active one.
     */
    struct PacketReceivedArgs
    {
        uint32_t opcode;
        void*    packet;
        void*    connection;
        void*    client;      ///< the NetClient/ClientConnection the message is being dispatched on
        bool*    handled;
    };

    using Handler = void (*)(void* user, const void* args);

    /**
     * @brief Subscribes a handler to an event, called cold at startup.
     * @param e        event to subscribe to.
     * @param handler  function pointer invoked on Emit.
     * @param user     opaque pointer passed back to the handler.
     */
    void Subscribe(Event e, Handler handler, void* user);

    /**
     * @brief Publishes an event to its subscribers in subscription order, called by the core's detours.
     * @param e     event to publish.
     * @param args  typed args struct for the event, passed by const pointer.
     */
    void Emit(Event e, const void* args);

    /**
     * @brief Reports whether an event has at least one subscriber.
     *
     * For a per-draw detour, checking this before building the args struct skips the whole emission
     * when nothing listens. Subscription happens at module load, before the render hooks run, so the
     * unsynchronized read is stable by the time a hot path consults it.
     * @param e  event to test.
     * @return true when Emit(e, ...) would invoke at least one handler.
     */
    bool Any(Event e);
}
