// wxl-graphics-extend: entry points, the published service table and the device lifecycle.
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

#include "core/Extension.hpp"
#include "assets/Assets.hpp"
#include "frame/Scheduler.hpp"
#include "io/ClientFile.hpp"
#include "shaders/Bls.hpp"
#include "shaders/Compiler.hpp"
#include "shaders/Fullscreen.hpp"
#include "targets/TargetPool.hpp"
#include "textures/Dds.hpp"
#include "textures/Neutral.hpp"
#include "ui/Panel.hpp"
#include "vulkan/Vulkan.hpp"

#include "wxl/EventScript.hpp"
#include "wxl/GraphicsExtendApi.h"

#include <cstring>
#include <new>

namespace wxl::gfx
{
    const WXL_Api* g_api = nullptr;
}

namespace
{
    namespace gfx = wxl::gfx;
    namespace ev  = wxl::events;

    /**
     * @brief Copies a struct the service filled into the caller's, which may be an older (shorter)
     *        version of it.
     *
     * The caller states its size in structSize; 0 (never set) is read as this version's size. Only
     * what fits is written, and structSize says how much that was.
     */
    template <class T>
    void CopyOut(T* out, T& full)
    {
        const uint32_t asked = out->structSize;
        if (asked != 0 && asked < sizeof(uint32_t)) return;   // not even room for structSize
        const uint32_t n = (asked == 0 || asked > sizeof(T)) ? uint32_t(sizeof(T)) : asked;
        full.structSize = n;
        std::memcpy(out, &full, n);
    }

    /**
     * @brief Runs one service call so that no exception leaves it.
     *
     * Nothing with a C++ ABI may cross into the caller's module (PluginApi.h), and an allocation
     * failing on a large DDS or file in a 32-bit process must cost that one call, not the client.
     */
    template <class R, class F>
    R Guarded(const char* what, R fallback, F&& call) noexcept
    {
        try
        {
            return call();
        }
        catch (const std::bad_alloc&)
        {
            GFX_LOG_ERROR("%s: out of memory", what);
        }
        catch (...)
        {
            GFX_LOG_ERROR("%s: unexpected exception", what);
        }
        return fallback;
    }

    template <class F>
    void GuardedVoid(const char* what, F&& call) noexcept
    {
        Guarded(what, 0, [&] { call(); return 0; });
    }

    // --- the C ABI: every entry __cdecl, as PluginApi.h requires of anything crossing -------------

    uint32_t __cdecl ApiAddPass(const WXL_GfxPassDesc* desc)
    {
        return Guarded("AddPass", 0u, [&] { return gfx::frame::AddPass(desc); });
    }

    int __cdecl ApiCurrentFrame(WXL_GfxFrame* out)
    {
        if (!out) return 0;
        WXL_GfxFrame full{};
        full.structSize = sizeof full;
        if (!gfx::frame::CurrentFrame(&full)) return 0;
        CopyOut(out, full);
        return 1;
    }

    float __cdecl ApiPassGpuMs(uint32_t passId) { return gfx::frame::PassGpuMs(passId); }

    int __cdecl ApiSetEngineLightBuffer(void* texture, const float rows[16], const float params[4])
    {
        return gfx::frame::SetEngineLightBuffer(texture, rows, params);
    }

    void __cdecl ApiGetStatus(WXL_GfxStatus* out)
    {
        if (!out) return;
        WXL_GfxStatus full{};
        full.structSize = sizeof full;
        gfx::frame::FillStatus(&full);
        gfx::targets::Stats(full.targetCount, full.targetBytes);
        gfx::shaders::Stats(full.shadersCompiled, full.shadersCached);
        CopyOut(out, full);
    }

    uint32_t __cdecl ApiCreateTarget(const WXL_GfxTargetDesc* desc)
    {
        return Guarded("CreateTarget", 0u, [&] { return gfx::targets::Create(desc); });
    }

    int __cdecl ApiGetTarget(uint32_t handle, void* device, WXL_GfxTarget* out)
    {
        return gfx::targets::Get(handle, device, out);
    }

    void __cdecl ApiDestroyTarget(uint32_t handle)
    {
        GuardedVoid("DestroyTarget", [&] { gfx::targets::Destroy(handle); });
    }

    void* __cdecl ApiTexture(void* device, uint32_t which) { return gfx::textures::Texture(device, which); }

    int __cdecl ApiDdsParse(const void* bytes, size_t size, WXL_GfxDdsInfo* out)
    {
        if (!out) return 0;
        WXL_GfxDdsInfo full{};
        full.structSize = sizeof full;
        if (!gfx::dds::Parse(bytes, size, &full)) return 0;
        CopyOut(out, full);
        return 1;
    }

    void* __cdecl ApiDdsCreateTexture(void* device, const void* bytes, size_t size, uint32_t pool, const char* name)
    {
        return Guarded("DdsCreateTexture", static_cast<void*>(nullptr),
                       [&] { return gfx::dds::CreateTexture(device, bytes, size, pool, name); });
    }

    void* __cdecl ApiDdsLoadTexture(void* device, const char* path, uint32_t pool)
    {
        return Guarded("DdsLoadTexture", static_cast<void*>(nullptr),
                       [&] { return gfx::dds::LoadTexture(device, path, pool); });
    }

    int __cdecl ApiDdsDecodeBgra(const void* bytes, size_t size, uint32_t faceOrSlice, uint32_t level,
                                 WXL_ByteSink* out, uint32_t* width, uint32_t* height)
    {
        return Guarded("DdsDecodeBgra", 0,
                       [&] { return gfx::dds::DecodeBgra(bytes, size, faceOrSlice, level, out, width, height); });
    }

    int __cdecl ApiDdsDecodeLuminance(const void* bytes, size_t size, uint32_t faceOrSlice, uint32_t level,
                                      WXL_ByteSink* out, uint32_t* width, uint32_t* height)
    {
        return Guarded("DdsDecodeLuminance", 0,
                       [&] { return gfx::dds::DecodeLuminance(bytes, size, faceOrSlice, level, out, width, height); });
    }

    int __cdecl ApiReadClientFile(const char* path, WXL_ByteSink* out)
    {
        return Guarded("ReadClientFile", 0, [&] { return gfx::io::ReadToSink(path, out); });
    }

    int __cdecl ApiBlsCount(const void* bytes, size_t size) { return gfx::bls::Count(bytes, size); }

    int __cdecl ApiBlsPermutation(const void* bytes, size_t size, uint32_t index, WXL_GfxBlsPermutation* out)
    {
        return gfx::bls::Permutation(bytes, size, index, out);
    }

    int __cdecl ApiBlsWrite(const WXL_GfxBlsPermutation* permutations, uint32_t count, WXL_ByteSink* out)
    {
        return Guarded("BlsWrite", 0, [&] { return gfx::bls::Write(permutations, count, out); });
    }

    int __cdecl ApiCompileShader(const WXL_GfxShaderDesc* desc, WXL_ByteSink* out)
    {
        return Guarded("CompileShader", 0, [&] { return gfx::shaders::Compile(desc, out); });
    }

    void* __cdecl ApiCreatePixelShader(void* device, const WXL_GfxShaderDesc* desc)
    {
        return Guarded("CreatePixelShader", static_cast<void*>(nullptr),
                       [&] { return gfx::shaders::CreatePixelShader(device, desc); });
    }

    void* __cdecl ApiCreateVertexShader(void* device, const WXL_GfxShaderDesc* desc)
    {
        return Guarded("CreateVertexShader", static_cast<void*>(nullptr),
                       [&] { return gfx::shaders::CreateVertexShader(device, desc); });
    }

    const char* __cdecl ApiIncludeFile(const char* name, size_t* size) { return gfx::shaders::IncludeFile(name, size); }

    void __cdecl ApiClearShaderCache(void) { gfx::shaders::ClearCache(); }

    void* __cdecl ApiFullscreenVertexShader(void* device)
    {
        return Guarded("FullscreenVertexShader", static_cast<void*>(nullptr),
                       [&] { return gfx::fullscreen::VertexShader(device); });
    }

    void __cdecl ApiDrawFullscreen(void* device, uint32_t width, uint32_t height)
    {
        gfx::fullscreen::Draw(device, width, height);
    }

    // --- baked assets ---------------------------------------------------------------------------

    const WXL_GfxManifest* __cdecl ApiManifestLoad(const char* folder, const char* file)
    {
        return Guarded("ManifestLoad", static_cast<const WXL_GfxManifest*>(nullptr),
                       [&] { return gfx::assets::ManifestLoad(folder, file); });
    }

    int __cdecl ApiManifestVersion(const WXL_GfxManifest* m) { return gfx::assets::ManifestVersion(m); }
    int __cdecl ApiManifestCount(const WXL_GfxManifest* m) { return gfx::assets::ManifestCount(m); }

    int __cdecl ApiManifestColumn(const WXL_GfxManifest* m, const char* name)
    {
        return Guarded("ManifestColumn", -1, [&] { return gfx::assets::ManifestColumn(m, name); });
    }

    int __cdecl ApiManifestFind(const WXL_GfxManifest* m, const char* key)
    {
        return Guarded("ManifestFind", -1, [&] { return gfx::assets::ManifestFind(m, key); });
    }

    const char* __cdecl ApiManifestField(const WXL_GfxManifest* m, int row, int column)
    {
        return gfx::assets::ManifestField(m, row, column);
    }

    float __cdecl ApiManifestNumber(const WXL_GfxManifest* m, int row, int column, float fallback)
    {
        return gfx::assets::ManifestNumber(m, row, column, fallback);
    }

    const char* __cdecl ApiManifestFolder(const WXL_GfxManifest* m) { return gfx::assets::ManifestFolder(m); }

    uint32_t __cdecl ApiAssetRequest(const char* path, int priority, uint32_t want)
    {
        return Guarded("AssetRequest", 0u, [&] { return gfx::assets::Request(path, priority, want); });
    }

    void* __cdecl ApiAssetTexture(uint32_t id) { return gfx::assets::Texture(id); }

    const void* __cdecl ApiAssetBytes(uint32_t id, size_t* size) { return gfx::assets::Bytes(id, size); }

    uint32_t __cdecl ApiAssetState(uint32_t id) { return gfx::assets::State(id); }

    void __cdecl ApiAssetRelease(uint32_t id) { gfx::assets::Release(id); }

    const char* __cdecl ApiAssetStatus(void) { return gfx::assets::Status(); }

    const WXL_GraphicsExtendApi kApi = {
        .structSize             = sizeof(WXL_GraphicsExtendApi),
        .apiVersion             = WXL_GRAPHICS_EXTEND_API_VERSION,
        .AddPass                = &ApiAddPass,
        .CurrentFrame           = &ApiCurrentFrame,
        .PassGpuMs              = &ApiPassGpuMs,
        .SetEngineLightBuffer   = &ApiSetEngineLightBuffer,
        .GetStatus              = &ApiGetStatus,
        .CreateTarget           = &ApiCreateTarget,
        .GetTarget              = &ApiGetTarget,
        .DestroyTarget          = &ApiDestroyTarget,
        .Texture                = &ApiTexture,
        .DdsParse               = &ApiDdsParse,
        .DdsCreateTexture       = &ApiDdsCreateTexture,
        .DdsLoadTexture         = &ApiDdsLoadTexture,
        .DdsDecodeBgra          = &ApiDdsDecodeBgra,
        .DdsDecodeLuminance     = &ApiDdsDecodeLuminance,
        .ReadClientFile         = &ApiReadClientFile,
        .BlsCount               = &ApiBlsCount,
        .BlsPermutation         = &ApiBlsPermutation,
        .BlsWrite               = &ApiBlsWrite,
        .CompileShader          = &ApiCompileShader,
        .CreatePixelShader      = &ApiCreatePixelShader,
        .CreateVertexShader     = &ApiCreateVertexShader,
        .IncludeFile            = &ApiIncludeFile,
        .ClearShaderCache       = &ApiClearShaderCache,
        .FullscreenVertexShader = &ApiFullscreenVertexShader,
        .DrawFullscreen         = &ApiDrawFullscreen,
        .ManifestLoad           = &ApiManifestLoad,
        .ManifestVersion        = &ApiManifestVersion,
        .ManifestCount          = &ApiManifestCount,
        .ManifestColumn         = &ApiManifestColumn,
        .ManifestFind           = &ApiManifestFind,
        .ManifestField          = &ApiManifestField,
        .ManifestNumber         = &ApiManifestNumber,
        .ManifestFolder         = &ApiManifestFolder,
        .AssetRequest           = &ApiAssetRequest,
        .AssetTexture           = &ApiAssetTexture,
        .AssetBytes             = &ApiAssetBytes,
        .AssetState             = &ApiAssetState,
        .AssetRelease           = &ApiAssetRelease,
        .AssetStatus            = &ApiAssetStatus,
    };

    /// DEFAULT-pool resources go before the engine resets the device; each module re-creates its own
    /// lazily afterwards, so nothing is needed on OnDeviceReset.
    class Lifecycle final : public wxl::ext::EventScript
    {
    public:
        Lifecycle()
        {
            on<&Lifecycle::OnDeviceLost>(ev::Event::OnDeviceLost);
            on<&Lifecycle::OnEndScene>(ev::Event::OnEndScene);
        }

        void OnDeviceLost(const ev::DeviceResetArgs&)
        {
            gfx::vulkan::OnDeviceLost();
            gfx::frame::OnDeviceLost();
            gfx::targets::OnDeviceLost();
            gfx::assets::OnDeviceLost();
        }

        /// Streams baked assets: finished reads become textures a few at a time, on the render thread.
        void OnEndScene(const ev::EndSceneArgs& a) { gfx::assets::Pump(a.device, ++frames_); }

    private:
        uint32_t frames_ = 0;
    };
}

const WXL_PluginInfo* __cdecl WXL_Query(void)
{
    static const WXL_PluginInfo info = {
        sizeof(WXL_PluginInfo),
        WXL_API_VERSION,
        "wxl-graphics-extend",
        1,
        WXL_CLIENT_BUILD,
    };
    return &info;
}

int __cdecl WXL_Load(const WXL_Api* api)
{
    if (!api || api->apiVersion != WXL_API_VERSION) return 0;

    gfx::g_api = api;

    // Bind before the first subclass exists: EventScript has no table until it is handed one.
    wxl::ext::EventScript::Bind(api);

    gfx::io::Install();
    gfx::assets::Install();
    gfx::vulkan::Install();
    gfx::frame::Install();
    static Lifecycle lifecycle;
    gfx::ui::Install();

    // Published last, once every module the table reaches is installed. Consumers that loaded
    // earlier find it on their next lookup (include/wxl/gfx/Client.hpp retries).
    api->PublishInterface(WXL_GRAPHICS_EXTEND_API_NAME, WXL_GRAPHICS_EXTEND_API_VERSION,
                          const_cast<WXL_GraphicsExtendApi*>(&kApi));
    // Always published: its Available() says whether DXVK answered on this device.
    api->PublishInterface(WXL_GRAPHICS_VULKAN_API_NAME, WXL_GRAPHICS_VULKAN_API_VERSION,
                          const_cast<WXL_GfxVulkanApi*>(gfx::vulkan::Table()));

    api->Log(WXL_LOG_INFO, gfx::kTag, "ready: published %s v%d and %s v%d (depth, g-buffer, hdr, passes, targets, dds, bls, shaders, assets, vulkan)",
             WXL_GRAPHICS_EXTEND_API_NAME, WXL_GRAPHICS_EXTEND_API_VERSION, WXL_GRAPHICS_VULKAN_API_NAME,
             WXL_GRAPHICS_VULKAN_API_VERSION);
    return 1;
}
