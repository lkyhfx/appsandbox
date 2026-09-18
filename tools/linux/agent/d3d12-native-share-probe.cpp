/* SPDX-License-Identifier: MIT
 *
 * D3D12 native shared-resource + shared-fence round-trip probe for Linux
 * GPU-PV guests.  The handles returned by libd3d12 are dxgkrnl NT-object
 * file descriptors.  They are intentionally passed only to
 * ID3D12Device::OpenSharedHandle; they are not dma-buf/DRM PRIME FDs.
 *
 * Build (DirectX-Headers and the WSL D3D12/DXCore libraries are required):
 *   c++ -std=c++17 -O2 -Wall -Wextra -Werror d3d12-native-share-probe.cpp \
 *       $(pkg-config --cflags --libs DirectX-Headers) -ld3d12 -ldxcore \
 *       -o d3d12-native-share-probe
 */

#include <wsl/winadapter.h>

/* DirectX-Headers releases newer than some distro winadapter.h packages use
 * these SAL annotations.  They are documentation-only for this build. */
#ifndef _In_count_
#define _In_count_(count)
#endif
#ifndef _In_opt_count_
#define _In_opt_count_(count)
#endif

#include <directx/d3d12.h>
#include <directx/dxcore.h>
#include <dxguids/dxguids.h>
#include <wrl/client.h>

#include <cerrno>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <unistd.h>

using Microsoft::WRL::ComPtr;

static bool hr_ok(HRESULT hr, const char *stage)
{
    if (SUCCEEDED(hr))
        return true;
    std::fprintf(stderr, "FAIL stage=%s HRESULT=0x%08x\n", stage,
                 static_cast<unsigned>(hr));
    return false;
}

static int shared_handle_fd(HANDLE handle, const char *kind)
{
    const intptr_t raw = reinterpret_cast<intptr_t>(handle);
    if (raw < 0 || raw > INT_MAX) {
        std::fprintf(stderr, "FAIL stage=%s-shared-handle value=%p\n", kind,
                     handle);
        return -1;
    }
    const int fd = static_cast<int>(raw);
#ifndef _WIN32
    if (fcntl(fd, F_GETFD) < 0) {
        std::perror(kind);
        return -1;
    }
#endif
    std::printf("PASS stage=%s-shared-handle fd=%d\n", kind, fd);
    return fd;
}

static bool wait_fence_eventfd(ID3D12Fence *fence, UINT64 value,
                               const char *stage)
{
    int completion_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (completion_fd < 0) {
        std::fprintf(stderr, "FAIL stage=%s-eventfd errno=%d (%s)\n", stage,
                     errno, std::strerror(errno));
        return false;
    }

    const HRESULT hr = fence->SetEventOnCompletion(
        value, reinterpret_cast<HANDLE>(static_cast<intptr_t>(completion_fd)));
    if (FAILED(hr)) {
        close(completion_fd);
        return hr_ok(hr, stage);
    }

    struct pollfd wait_fd = {};
    wait_fd.fd = completion_fd;
    wait_fd.events = POLLIN;
    int poll_result;
    do {
        poll_result = poll(&wait_fd, 1, 10000);
    } while (poll_result < 0 && errno == EINTR);

    if (poll_result == 0) {
        std::fprintf(stderr,
                     "FAIL stage=%s-timeout completed=%llu expected=%llu\n",
                     stage,
                     static_cast<unsigned long long>(fence->GetCompletedValue()),
                     static_cast<unsigned long long>(value));
        close(completion_fd);
        return false;
    }
    if (poll_result < 0) {
        std::fprintf(stderr, "FAIL stage=%s-poll errno=%d (%s)\n", stage,
                     errno, std::strerror(errno));
        close(completion_fd);
        return false;
    }
    if ((wait_fd.revents & POLLIN) == 0) {
        std::fprintf(stderr, "FAIL stage=%s-poll revents=0x%x\n", stage,
                     wait_fd.revents);
        close(completion_fd);
        return false;
    }

    eventfd_t notifications = 0;
    int read_result;
    do {
        read_result = eventfd_read(completion_fd, &notifications);
    } while (read_result < 0 && errno == EINTR);
    if (read_result < 0) {
        std::fprintf(stderr, "FAIL stage=%s-read errno=%d (%s)\n", stage,
                     errno, std::strerror(errno));
        close(completion_fd);
        return false;
    }
    close(completion_fd);

    const UINT64 completed = fence->GetCompletedValue();
    if (completed < value) {
        std::fprintf(stderr,
                     "FAIL stage=%s-premature completed=%llu expected=%llu\n",
                     stage, static_cast<unsigned long long>(completed),
                     static_cast<unsigned long long>(value));
        return false;
    }
    std::printf("PASS stage=%s eventfd_notifications=%llu completed=%llu\n",
                stage, static_cast<unsigned long long>(notifications),
                static_cast<unsigned long long>(completed));
    return true;
}

static D3D12_RESOURCE_DESC buffer_desc(UINT64 size)
{
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Alignment = 0;
    desc.Width = size;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = D3D12_RESOURCE_FLAG_NONE;
    return desc;
}

int main(int argc, char **argv)
{
    constexpr UINT width = 64;
    constexpr UINT height = 64;
    constexpr UINT64 shared_value = 1;
    constexpr UINT64 done_value = 1;
    constexpr float clear[4] = {0.25f, 0.50f, 0.75f, 1.0f};

    bool cross_adapter_fence = false;
    bool cpu_sync_fallback = false;
    if (argc == 2 && std::strcmp(argv[1], "--cross-adapter-fence") == 0)
        cross_adapter_fence = true;
    else if (argc == 2 && std::strcmp(argv[1], "--cpu-sync-fallback") == 0)
        cpu_sync_fallback = true;
    else if (argc != 1) {
        std::fprintf(stderr,
                     "Usage: %s [--cross-adapter-fence|--cpu-sync-fallback]\n",
                     argv[0]);
        return 2;
    }

    setvbuf(stdout, nullptr, _IONBF, 0);

    ComPtr<IDXCoreAdapterFactory> factory;
    ComPtr<IDXCoreAdapterList> adapters;
    const GUID attributes[] = {DXCORE_ADAPTER_ATTRIBUTE_D3D12_CORE_COMPUTE};
    if (!hr_ok(DXCoreCreateAdapterFactory(IID_PPV_ARGS(&factory)),
               "dxcore-factory") ||
        !hr_ok(factory->CreateAdapterList(1, attributes,
                                          IID_PPV_ARGS(&adapters)),
               "dxcore-adapter-list"))
        return 1;
    if (adapters->GetAdapterCount() == 0) {
        std::fputs("FAIL stage=dxcore-no-adapter\n", stderr);
        return 1;
    }

    ComPtr<IDXCoreAdapter> adapter;
    if (!hr_ok(adapters->GetAdapter(0, IID_PPV_ARGS(&adapter)),
               "dxcore-get-adapter"))
        return 1;

    char description[256] = {};
    size_t description_size = 0;
    if (adapter->IsPropertySupported(DXCoreAdapterProperty::DriverDescription) &&
        SUCCEEDED(adapter->GetPropertySize(
            DXCoreAdapterProperty::DriverDescription, &description_size)) &&
        description_size > 0 && description_size <= sizeof(description)) {
        (void)adapter->GetProperty(DXCoreAdapterProperty::DriverDescription,
                                   description_size, description);
    }
    std::printf("adapter=%s\n", description[0] ? description : "(unknown)");

    ComPtr<ID3D12Device> producer_device;
    ComPtr<ID3D12Device> consumer_device;
    if (!hr_ok(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0,
                                  IID_PPV_ARGS(&producer_device)),
               "producer-device") ||
        !hr_ok(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0,
                                  IID_PPV_ARGS(&consumer_device)),
               "consumer-device"))
        return 1;

    const LUID producer_luid = producer_device->GetAdapterLuid();
    const LUID consumer_luid = consumer_device->GetAdapterLuid();
    if (std::memcmp(&producer_luid, &consumer_luid, sizeof(LUID)) != 0) {
        std::fputs("FAIL stage=device-adapter-mismatch\n", stderr);
        return 1;
    }
    std::printf("PASS stage=two-devices adapter_luid=%08x:%08x\n",
                static_cast<unsigned>(producer_luid.HighPart),
                static_cast<unsigned>(producer_luid.LowPart));

    D3D12_COMMAND_QUEUE_DESC queue_desc = {};
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> producer_queue;
    ComPtr<ID3D12CommandQueue> consumer_queue;
    if (!hr_ok(producer_device->CreateCommandQueue(
                   &queue_desc, IID_PPV_ARGS(&producer_queue)),
               "producer-queue") ||
        !hr_ok(consumer_device->CreateCommandQueue(
                   &queue_desc, IID_PPV_ARGS(&consumer_queue)),
               "consumer-queue"))
        return 1;

    D3D12_HEAP_PROPERTIES default_heap = {};
    default_heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    default_heap.CreationNodeMask = 1;
    default_heap.VisibleNodeMask = 1;

    D3D12_RESOURCE_DESC texture_desc = {};
    texture_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texture_desc.Width = width;
    texture_desc.Height = height;
    texture_desc.DepthOrArraySize = 1;
    texture_desc.MipLevels = 1;
    texture_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    texture_desc.SampleDesc.Count = 1;
    texture_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    texture_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    D3D12_CLEAR_VALUE clear_value = {};
    clear_value.Format = texture_desc.Format;
    std::memcpy(clear_value.Color, clear, sizeof(clear));

    ComPtr<ID3D12Resource> producer_texture;
    if (!hr_ok(producer_device->CreateCommittedResource(
                   &default_heap, D3D12_HEAP_FLAG_SHARED, &texture_desc,
                   D3D12_RESOURCE_STATE_RENDER_TARGET, &clear_value,
                   IID_PPV_ARGS(&producer_texture)),
               "create-shared-texture"))
        return 1;

    ComPtr<ID3D12Fence> producer_fence;
    const D3D12_FENCE_FLAGS fence_flags = cross_adapter_fence
        ? D3D12_FENCE_FLAG_SHARED_CROSS_ADAPTER
        : D3D12_FENCE_FLAG_SHARED;
    std::printf("fence_flags=%s\n",
                cross_adapter_fence ? "SHARED_CROSS_ADAPTER" : "SHARED");
    if (!hr_ok(producer_device->CreateFence(
                   0, fence_flags,
                   IID_PPV_ARGS(&producer_fence)),
               "create-shared-fence"))
        return 1;

    HANDLE resource_handle = nullptr;
    HANDLE fence_handle = nullptr;
    if (!hr_ok(producer_device->CreateSharedHandle(
                   producer_texture.Get(), nullptr, GENERIC_ALL, nullptr,
                   &resource_handle),
               "export-resource"))
        return 1;

    const int resource_fd = shared_handle_fd(resource_handle, "resource");
    if (resource_fd < 0)
        return 1;
    if (!hr_ok(producer_device->CreateSharedHandle(
                   producer_fence.Get(), nullptr, GENERIC_ALL, nullptr,
                   &fence_handle),
               "export-fence")) {
        close(resource_fd);
        return 1;
    }
    const int fence_fd = shared_handle_fd(fence_handle, "fence");
    if (fence_fd < 0) {
        close(resource_fd);
        return 1;
    }

    ComPtr<ID3D12Resource> consumer_texture;
    ComPtr<ID3D12Fence> consumer_shared_fence;
    ComPtr<ID3D12Fence> producer_reopened_fence;
    const HRESULT open_resource_hr = consumer_device->OpenSharedHandle(
        reinterpret_cast<HANDLE>(static_cast<intptr_t>(resource_fd)),
        IID_PPV_ARGS(&consumer_texture));
    const HRESULT reopen_fence_hr = producer_device->OpenSharedHandle(
        reinterpret_cast<HANDLE>(static_cast<intptr_t>(fence_fd)),
        IID_PPV_ARGS(&producer_reopened_fence));
    std::printf("diagnostic_same_device_open_fence=0x%08x\n",
                static_cast<unsigned>(reopen_fence_hr));
    const HRESULT open_fence_hr = consumer_device->OpenSharedHandle(
        reinterpret_cast<HANDLE>(static_cast<intptr_t>(fence_fd)),
        IID_PPV_ARGS(&consumer_shared_fence));
    close(resource_fd);
    close(fence_fd);
    if (!hr_ok(open_resource_hr, "open-shared-resource"))
        return 1;
    if (FAILED(open_fence_hr)) {
        if (!cpu_sync_fallback) {
            (void)hr_ok(open_fence_hr, "open-shared-fence");
            return 1;
        }
        std::printf("BLOCKED stage=open-shared-fence HRESULT=0x%08x; "
                    "continuing with diagnostic CPU wait\n",
                    static_cast<unsigned>(open_fence_hr));
    } else {
        std::puts("PASS stage=native-handle-open "
                  "resource=ID3D12Resource fence=ID3D12Fence");
    }

    D3D12_DESCRIPTOR_HEAP_DESC rtv_heap_desc = {};
    rtv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtv_heap_desc.NumDescriptors = 1;
    ComPtr<ID3D12DescriptorHeap> rtv_heap;
    if (!hr_ok(producer_device->CreateDescriptorHeap(
                   &rtv_heap_desc, IID_PPV_ARGS(&rtv_heap)),
               "producer-rtv-heap"))
        return 1;
    const D3D12_CPU_DESCRIPTOR_HANDLE rtv =
        rtv_heap->GetCPUDescriptorHandleForHeapStart();
    producer_device->CreateRenderTargetView(producer_texture.Get(), nullptr, rtv);

    ComPtr<ID3D12CommandAllocator> producer_allocator;
    ComPtr<ID3D12GraphicsCommandList> producer_list;
    if (!hr_ok(producer_device->CreateCommandAllocator(
                   D3D12_COMMAND_LIST_TYPE_DIRECT,
                   IID_PPV_ARGS(&producer_allocator)),
               "producer-command-allocator") ||
        !hr_ok(producer_device->CreateCommandList(
                   0, D3D12_COMMAND_LIST_TYPE_DIRECT, producer_allocator.Get(),
                   nullptr, IID_PPV_ARGS(&producer_list)),
               "producer-command-list"))
        return 1;
    producer_list->ClearRenderTargetView(rtv, clear, 0, nullptr);
    D3D12_RESOURCE_BARRIER producer_barrier = {};
    producer_barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    producer_barrier.Transition.pResource = producer_texture.Get();
    producer_barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    producer_barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    producer_barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
    producer_list->ResourceBarrier(1, &producer_barrier);
    if (!hr_ok(producer_list->Close(), "producer-command-close"))
        return 1;

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
    UINT rows = 0;
    UINT64 row_size = 0;
    UINT64 readback_size = 0;
    consumer_device->GetCopyableFootprints(&texture_desc, 0, 1, 0, &footprint,
                                            &rows, &row_size, &readback_size);
    (void)rows;
    (void)row_size;

    D3D12_HEAP_PROPERTIES readback_heap = {};
    readback_heap.Type = D3D12_HEAP_TYPE_READBACK;
    readback_heap.CreationNodeMask = 1;
    readback_heap.VisibleNodeMask = 1;
    const D3D12_RESOURCE_DESC readback_desc = buffer_desc(readback_size);
    ComPtr<ID3D12Resource> readback;
    if (!hr_ok(consumer_device->CreateCommittedResource(
                   &readback_heap, D3D12_HEAP_FLAG_NONE, &readback_desc,
                   D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                   IID_PPV_ARGS(&readback)),
               "consumer-readback"))
        return 1;

    ComPtr<ID3D12CommandAllocator> consumer_allocator;
    ComPtr<ID3D12GraphicsCommandList> consumer_list;
    if (!hr_ok(consumer_device->CreateCommandAllocator(
                   D3D12_COMMAND_LIST_TYPE_DIRECT,
                   IID_PPV_ARGS(&consumer_allocator)),
               "consumer-command-allocator") ||
        !hr_ok(consumer_device->CreateCommandList(
                   0, D3D12_COMMAND_LIST_TYPE_DIRECT, consumer_allocator.Get(),
                   nullptr, IID_PPV_ARGS(&consumer_list)),
               "consumer-command-list"))
        return 1;

    D3D12_RESOURCE_BARRIER consumer_barrier = {};
    consumer_barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    consumer_barrier.Transition.pResource = consumer_texture.Get();
    consumer_barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    consumer_barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    consumer_barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    consumer_list->ResourceBarrier(1, &consumer_barrier);

    D3D12_TEXTURE_COPY_LOCATION source = {};
    source.pResource = consumer_texture.Get();
    source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    source.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION destination = {};
    destination.pResource = readback.Get();
    destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    destination.PlacedFootprint = footprint;
    consumer_list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    if (!hr_ok(consumer_list->Close(), "consumer-command-close"))
        return 1;

    ComPtr<ID3D12Fence> done_fence;
    if (!hr_ok(consumer_device->CreateFence(
                   0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&done_fence)),
               "consumer-done-fence"))
        return 1;

    /* Enqueue the consumer wait before producer submission.  This proves that
     * the opened fence, rather than submission order or a CPU wait, gates the
     * second device's texture access. */
    ID3D12CommandList *consumer_lists[] = {consumer_list.Get()};
    ID3D12CommandList *producer_lists[] = {producer_list.Get()};
    if (consumer_shared_fence) {
        if (!hr_ok(consumer_queue->Wait(consumer_shared_fence.Get(), shared_value),
                   "consumer-queue-wait"))
            return 1;
        consumer_queue->ExecuteCommandLists(1, consumer_lists);
        if (!hr_ok(consumer_queue->Signal(done_fence.Get(), done_value),
                   "consumer-signal"))
            return 1;
        producer_queue->ExecuteCommandLists(1, producer_lists);
        if (!hr_ok(producer_queue->Signal(producer_fence.Get(), shared_value),
                   "producer-signal"))
            return 1;
    } else {
        producer_queue->ExecuteCommandLists(1, producer_lists);
        if (!hr_ok(producer_queue->Signal(producer_fence.Get(), shared_value),
                   "producer-signal-cpu-fallback") ||
            !wait_fence_eventfd(producer_fence.Get(), shared_value,
                                "producer-eventfd-notification"))
            return 1;
        consumer_queue->ExecuteCommandLists(1, consumer_lists);
        if (!hr_ok(consumer_queue->Signal(done_fence.Get(), done_value),
                   "consumer-signal-cpu-fallback"))
            return 1;
    }
    if (!wait_fence_eventfd(done_fence.Get(), done_value,
                            "consumer-eventfd-notification"))
        return 1;
    if (consumer_shared_fence)
        std::puts("PASS stage=shared-fence-cross-device-wait");
    else
        std::puts("DIAGNOSTIC stage=cpu-fence-wait-before-consumer");

    const D3D12_RANGE read_range = {0, 4};
    void *mapped = nullptr;
    if (!hr_ok(readback->Map(0, &read_range, &mapped), "readback-map"))
        return 1;
    const auto *pixel = static_cast<const uint8_t *>(mapped) + footprint.Offset;
    const uint8_t expected[4] = {191, 128, 64, 255}; /* BGRA byte order */
    bool pixel_ok = true;
    for (unsigned i = 0; i < 4; ++i) {
        const int delta = static_cast<int>(pixel[i]) - expected[i];
        if (delta < -1 || delta > 1)
            pixel_ok = false;
    }
    std::printf("pixel_bgra=%u,%u,%u,%u expected=%u,%u,%u,%u\n",
                pixel[0], pixel[1], pixel[2], pixel[3], expected[0], expected[1],
                expected[2], expected[3]);
    const D3D12_RANGE written_range = {0, 0};
    readback->Unmap(0, &written_range);
    if (!pixel_ok) {
        std::fputs("FAIL stage=native-shared-pixel-roundtrip\n", stderr);
        return 1;
    }

    std::puts("PASS stage=native-shared-pixel-roundtrip");

    /* Reuse the same opened resource and fences with a second color/value.
     * Waiting for done_value above makes allocator/list reuse safe. */
    constexpr float clear_reuse[4] = {0.80f, 0.20f, 0.40f, 1.0f};
    if (!hr_ok(producer_allocator->Reset(), "producer-allocator-reuse") ||
        !hr_ok(producer_list->Reset(producer_allocator.Get(), nullptr),
               "producer-list-reuse"))
        return 1;
    producer_barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    producer_barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    producer_list->ResourceBarrier(1, &producer_barrier);
    producer_list->ClearRenderTargetView(rtv, clear_reuse, 0, nullptr);
    producer_barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    producer_barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
    producer_list->ResourceBarrier(1, &producer_barrier);
    if (!hr_ok(producer_list->Close(), "producer-command-close-reuse"))
        return 1;

    if (!hr_ok(consumer_allocator->Reset(), "consumer-allocator-reuse") ||
        !hr_ok(consumer_list->Reset(consumer_allocator.Get(), nullptr),
               "consumer-list-reuse"))
        return 1;
    consumer_list->ResourceBarrier(1, &consumer_barrier);
    consumer_list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    if (!hr_ok(consumer_list->Close(), "consumer-command-close-reuse"))
        return 1;

    if (consumer_shared_fence) {
        if (!hr_ok(consumer_queue->Wait(consumer_shared_fence.Get(), 2),
                   "consumer-queue-wait-reuse"))
            return 1;
        consumer_queue->ExecuteCommandLists(1, consumer_lists);
        if (!hr_ok(consumer_queue->Signal(done_fence.Get(), 2),
                   "consumer-signal-reuse"))
            return 1;
        producer_queue->ExecuteCommandLists(1, producer_lists);
        if (!hr_ok(producer_queue->Signal(producer_fence.Get(), 2),
                   "producer-signal-reuse"))
            return 1;
    } else {
        producer_queue->ExecuteCommandLists(1, producer_lists);
        if (!hr_ok(producer_queue->Signal(producer_fence.Get(), 2),
                   "producer-signal-reuse-cpu-fallback") ||
            !wait_fence_eventfd(producer_fence.Get(), 2,
                                "producer-eventfd-notification-reuse"))
            return 1;
        consumer_queue->ExecuteCommandLists(1, consumer_lists);
        if (!hr_ok(consumer_queue->Signal(done_fence.Get(), 2),
                   "consumer-signal-reuse-cpu-fallback"))
            return 1;
    }
    if (!wait_fence_eventfd(done_fence.Get(), 2,
                            "consumer-eventfd-notification-reuse"))
        return 1;

    mapped = nullptr;
    if (!hr_ok(readback->Map(0, &read_range, &mapped), "readback-map-reuse"))
        return 1;
    pixel = static_cast<const uint8_t *>(mapped) + footprint.Offset;
    const uint8_t expected_reuse[4] = {102, 51, 204, 255};
    pixel_ok = true;
    for (unsigned i = 0; i < 4; ++i) {
        const int delta = static_cast<int>(pixel[i]) - expected_reuse[i];
        if (delta < -1 || delta > 1)
            pixel_ok = false;
    }
    std::printf("reuse_pixel_bgra=%u,%u,%u,%u expected=%u,%u,%u,%u\n",
                pixel[0], pixel[1], pixel[2], pixel[3], expected_reuse[0],
                expected_reuse[1], expected_reuse[2], expected_reuse[3]);
    readback->Unmap(0, &written_range);
    if (!pixel_ok) {
        std::fputs("FAIL stage=native-shared-reuse\n", stderr);
        return 1;
    }

    std::puts("PASS stage=native-shared-reuse");
    if (consumer_shared_fence) {
        std::puts("PASS d3d12-native-share resource+fence+pixel+reuse");
        return 0;
    }
    std::puts("PASS diagnostic=native-shared-resource+pixel+reuse");
    std::puts("BLOCKED d3d12-native-share shared-fence-open");
    return 3;
}
