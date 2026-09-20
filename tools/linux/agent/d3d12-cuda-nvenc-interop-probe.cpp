/* SPDX-License-Identifier: MIT
 *
 * Gate B diagnostic.  This probe intentionally stops at the first unsupported
 * interop boundary; it never inserts a CPU readback or upload as a fallback.
 * The resource is a synthetic D3D12 BGRA texture.  A real Mutter framebuffer
 * is exercised only by the separate B4 runner, and is never represented by a
 * synthetic PASS here.
 *
 * Build (nv-codec-headers, DirectX-Headers and WSL D3D12):
 *   c++ -std=c++17 -O2 -Wall -Wextra -Werror -D_GNU_SOURCE -DGUID_DEFINED \
 *     -I/usr/include/ffnvcodec \
 *     d3d12-cuda-nvenc-interop-probe.cpp $(pkg-config --cflags --libs DirectX-Headers) \
 *     -ld3d12 -ldxcore -ldl -o d3d12-cuda-nvenc-interop-probe
 */
#include <wsl/winadapter.h>
#include <directx/d3d12.h>
#include <directx/dxcore.h>
#include <dxguids/dxguids.h>
#include <wrl/client.h>
#include <dynlink_cuda.h>
#include <nvEncodeAPI.h>

#include <cstdint>
#include <cstdio>
#include <dlfcn.h>
#include <fcntl.h>
#include <unistd.h>

using Microsoft::WRL::ComPtr;

namespace {

using cu_init_fn = CUresult (*)(unsigned);
using cu_device_get_fn = CUresult (*)(CUdevice *, int);
using cu_ctx_create_fn = CUresult (*)(CUcontext *, unsigned, CUdevice);
using cu_ctx_destroy_fn = CUresult (*)(CUcontext);
using cu_import_external_memory_fn = CUresult (*)(
    CUexternalMemory *, const CUDA_EXTERNAL_MEMORY_HANDLE_DESC *);
using cu_destroy_external_memory_fn = CUresult (*)(CUexternalMemory);
using cu_external_memory_get_mipmap_fn = CUresult (*)(
    CUmipmappedArray *, CUexternalMemory,
    const CUDA_EXTERNAL_MEMORY_MIPMAPPED_ARRAY_DESC *);
using cu_mipmap_get_level_fn = CUresult (*)(CUarray *, CUmipmappedArray, unsigned);
using nv_create_instance_fn = NVENCSTATUS (*)(NV_ENCODE_API_FUNCTION_LIST *);

struct AdapterContext {
    ComPtr<IDXCoreAdapterFactory> factory;
    ComPtr<IDXCoreAdapterList> adapters;
    ComPtr<IDXCoreAdapter> adapter;
    ComPtr<ID3D12Device> device;
};

static void blocked(const char *stage, const char *reason)
{
    std::printf("BLOCKED stage=%s reason=%s\n", stage, reason);
}

static bool make_device(AdapterContext *out)
{
    const GUID attributes[] = {DXCORE_ADAPTER_ATTRIBUTE_D3D12_CORE_COMPUTE};
    if (FAILED(DXCoreCreateAdapterFactory(IID_PPV_ARGS(&out->factory))) ||
        FAILED(out->factory->CreateAdapterList(
            1, attributes, IID_PPV_ARGS(&out->adapters))) ||
        out->adapters->GetAdapterCount() == 0 ||
        FAILED(out->adapters->GetAdapter(0, IID_PPV_ARGS(&out->adapter))) ||
        FAILED(D3D12CreateDevice(out->adapter.Get(), D3D_FEATURE_LEVEL_11_0,
                                 IID_PPV_ARGS(&out->device)))) {
        return false;
    }
    return true;
}

static D3D12_RESOURCE_DESC texture_desc()
{
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = 3840;
    desc.Height = 2160;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    return desc;
}

} // namespace

int main()
{
    std::printf("real_mutter=0\n");
    std::printf("frames=0\n");
    std::printf("cpu_framebuffer_copy=0\n");
    std::printf("cpu_conversion=0\n");
    std::printf("cpu_upload=0\n");

    void *cuda_lib = dlopen("libcuda.so.1", RTLD_NOW | RTLD_LOCAL);
    void *nvenc_lib = dlopen("libnvidia-encode.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!cuda_lib || !nvenc_lib) {
        blocked("d3d12_cuda_import", "cuda-or-nvenc-runtime-not-found");
        blocked("gpu_rgb_to_yuv444", "not-run-after-import-block");
        blocked("cuda_nvenc_register", "not-run-after-conversion-block");
        blocked("cuda_nvenc_map", "not-run-after-register-block");
        blocked("real_mutter", "not-run-synthetic-probe");
        if (nvenc_lib) dlclose(nvenc_lib);
        if (cuda_lib) dlclose(cuda_lib);
        return 3;
    }

    auto cu_init = reinterpret_cast<cu_init_fn>(dlsym(cuda_lib, "cuInit"));
    auto cu_device_get = reinterpret_cast<cu_device_get_fn>(dlsym(cuda_lib, "cuDeviceGet"));
    auto cu_ctx_create = reinterpret_cast<cu_ctx_create_fn>(dlsym(cuda_lib, "cuCtxCreate_v2"));
    auto cu_ctx_destroy = reinterpret_cast<cu_ctx_destroy_fn>(dlsym(cuda_lib, "cuCtxDestroy_v2"));
    auto cu_import = reinterpret_cast<cu_import_external_memory_fn>(
        dlsym(cuda_lib, "cuImportExternalMemory"));
    auto cu_destroy_external = reinterpret_cast<cu_destroy_external_memory_fn>(
        dlsym(cuda_lib, "cuDestroyExternalMemory"));
    auto cu_get_mipmap = reinterpret_cast<cu_external_memory_get_mipmap_fn>(
        dlsym(cuda_lib, "cuExternalMemoryGetMappedMipmappedArray"));
    auto cu_get_level = reinterpret_cast<cu_mipmap_get_level_fn>(
        dlsym(cuda_lib, "cuMipmappedArrayGetLevel"));
    auto nv_create = reinterpret_cast<nv_create_instance_fn>(
        dlsym(nvenc_lib, "NvEncodeAPICreateInstance"));
    if (!cu_init || !cu_device_get || !cu_ctx_create || !cu_ctx_destroy ||
        !cu_import || !cu_destroy_external || !cu_get_mipmap || !cu_get_level ||
        !nv_create || cu_init(0) != CUDA_SUCCESS) {
        blocked("d3d12_cuda_import", "cuda-driver-api-incomplete");
        blocked("gpu_rgb_to_yuv444", "not-run-after-import-block");
        blocked("cuda_nvenc_register", "not-run-after-conversion-block");
        blocked("cuda_nvenc_map", "not-run-after-register-block");
        blocked("real_mutter", "not-run-synthetic-probe");
        dlclose(nvenc_lib);
        dlclose(cuda_lib);
        return 3;
    }

    CUdevice cuda_device = 0;
    CUcontext cuda_context = nullptr;
    if (cu_device_get(&cuda_device, 0) != CUDA_SUCCESS ||
        cu_ctx_create(&cuda_context, 0, cuda_device) != CUDA_SUCCESS) {
        blocked("d3d12_cuda_import", "cuda-context");
        blocked("gpu_rgb_to_yuv444", "not-run-after-import-block");
        blocked("cuda_nvenc_register", "not-run-after-conversion-block");
        blocked("cuda_nvenc_map", "not-run-after-register-block");
        blocked("real_mutter", "not-run-synthetic-probe");
        if (cuda_context) cu_ctx_destroy(cuda_context);
        dlclose(nvenc_lib);
        dlclose(cuda_lib);
        return 3;
    }

    AdapterContext adapter;
    ComPtr<ID3D12Resource> resource;
    HANDLE shared_handle = nullptr;
    int resource_fd = -1;
    bool imported = false;
    bool conversion_ready = false;
    bool later_stages_reported = false;
    CUexternalMemory external_memory = nullptr;
    CUmipmappedArray mipmapped = nullptr;
    CUarray level = nullptr;
    if (!make_device(&adapter)) {
        blocked("d3d12_cuda_import", "d3d12-device");
        goto cleanup;
    }
    {
        D3D12_HEAP_PROPERTIES heap = {};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        heap.CreationNodeMask = 1;
        heap.VisibleNodeMask = 1;
        const D3D12_RESOURCE_DESC desc = texture_desc();
        const float color[4] = {0.25f, 0.5f, 0.75f, 1.0f};
        D3D12_CLEAR_VALUE clear = {};
        clear.Format = desc.Format;
        for (unsigned i = 0; i < 4; ++i) clear.Color[i] = color[i];
        if (FAILED(adapter.device->CreateCommittedResource(
                &heap, D3D12_HEAP_FLAG_SHARED, &desc,
                D3D12_RESOURCE_STATE_COMMON, &clear,
                IID_PPV_ARGS(&resource))) ||
            FAILED(adapter.device->CreateSharedHandle(
                resource.Get(), nullptr, GENERIC_ALL, nullptr, &shared_handle))) {
            blocked("d3d12_cuda_import", "d3d12-shared-resource");
            goto cleanup;
        }
        resource_fd = static_cast<int>(reinterpret_cast<intptr_t>(shared_handle));
        shared_handle = nullptr;
        if (resource_fd < 0 || fcntl(resource_fd, F_GETFD) < 0) {
            blocked("d3d12_cuda_import", "shared-handle-not-linux-fd");
            goto cleanup;
        }
        const D3D12_RESOURCE_ALLOCATION_INFO allocation =
            adapter.device->GetResourceAllocationInfo(0, 1, &desc);
        CUDA_EXTERNAL_MEMORY_HANDLE_DESC import_desc = {};
        import_desc.type = CU_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD;
        import_desc.handle.fd = resource_fd;
        import_desc.size = allocation.SizeInBytes;
        if (cu_import(&external_memory, &import_desc) != CUDA_SUCCESS) {
            blocked("d3d12_cuda_import", "opaque-fd-texture-import");
            goto cleanup;
        }
        imported = true;
        close(resource_fd);
        resource_fd = -1;
        CUDA_EXTERNAL_MEMORY_MIPMAPPED_ARRAY_DESC array_desc = {};
        array_desc.numLevels = 1;
        array_desc.arrayDesc.Width = 3840;
        array_desc.arrayDesc.Height = 2160;
        array_desc.arrayDesc.Depth = 0;
        array_desc.arrayDesc.Format = CU_AD_FORMAT_UNSIGNED_INT8;
        array_desc.arrayDesc.NumChannels = 4;
        if (cu_get_mipmap(&mipmapped, external_memory, &array_desc) != CUDA_SUCCESS ||
            cu_get_level(&level, mipmapped, 0) != CUDA_SUCCESS) {
            blocked("d3d12_cuda_import", "opaque-fd-array-map");
            goto cleanup;
        }
        conversion_ready = true;
        std::printf("PASS stage=d3d12_cuda_import resource=BGRA8 width=3840 height=2160 cpu_framebuffer_copy=0\n");
    }

    /* CUDA has imported a BGRA array, but this probe intentionally has no
     * hidden CPU staging and no uncompiled conversion kernel.  Therefore the
     * correct result for B2/B3 is BLOCKED, never a synthetic PASS. */
    blocked("gpu_rgb_to_yuv444", "no-gpu-conversion-kernel");
    blocked("cuda_nvenc_register", "input-is-bgra-not-yuv444");
    blocked("cuda_nvenc_map", "register-blocked");
    blocked("real_mutter", "synthetic-resource-only");
    later_stages_reported = true;

cleanup:
    if (!later_stages_reported) {
        if (!conversion_ready)
            blocked("gpu_rgb_to_yuv444", "not-run-after-import-block");
        blocked("cuda_nvenc_register", "not-run-after-conversion-block");
        blocked("cuda_nvenc_map", "not-run-after-register-block");
        blocked("real_mutter", "not-run-synthetic-probe");
    }
    if (resource_fd >= 0) close(resource_fd);
    if (shared_handle)
        close(static_cast<int>(reinterpret_cast<intptr_t>(shared_handle)));
    if (imported && external_memory) cu_destroy_external(external_memory);
    (void)level;
    (void)mipmapped;
    if (cuda_context) cu_ctx_destroy(cuda_context);
    dlclose(nvenc_lib);
    dlclose(cuda_lib);
    return 3;
}
