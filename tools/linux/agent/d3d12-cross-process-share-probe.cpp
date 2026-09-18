/* SPDX-License-Identifier: MIT
 *
 * Cross-process D3D12 native shared-resource probe for Linux GPU-PV guests.
 *
 * The default invocation is a launcher.  It creates two independent
 * fork+exec children; neither child inherits a D3D12 device.  The producer
 * creates three 4K BGRA8 resources and sends their dxg file descriptors to
 * the consumer with SCM_RIGHTS over a Unix SOCK_SEQPACKET socket.  The
 * producer also sends one ready eventfd per slot.  The consumer opens the
 * resources on its own device, creates one done eventfd per slot, and sends
 * those descriptors back with SCM_RIGHTS.
 *
 * The frame path has no framebuffer mmap, CPU framebuffer memcpy, or
 * GPU-to-CPU-to-GPU copy.  The consumer copies each shared texture into a
 * private GPU-only sink texture.  A readback is allocated and used only on
 * every 120th frame for sequence diagnostics.
 *
 * Build (DirectX-Headers and the WSL D3D12/DXCore libraries are required):
 *   c++ -std=c++17 -O2 -Wall -Wextra -Werror \
 *       d3d12-cross-process-share-probe.cpp \
 *       $(pkg-config --cflags --libs DirectX-Headers) -ld3d12 -ldxcore \
 *       -o d3d12-cross-process-share-probe
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

#include <algorithm>
#include <array>
#include <cerrno>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <utility>
#include <vector>

#include "d3d12-share-probe-protocol.h"

using Microsoft::WRL::ComPtr;

namespace {

using namespace asb_d3d12_share_probe;
constexpr int kPollTimeoutMs = 10000;
constexpr int kSocketTimeoutMs = 30000;
constexpr std::size_t kMaxTransferFds = kSlotCount * 2;

static std::uint64_t monotonic_ns()
{
    struct timespec ts = {};
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;
    return static_cast<std::uint64_t>(ts.tv_sec) * 1000000000ULL +
           static_cast<std::uint64_t>(ts.tv_nsec);
}

static void sleep_until_ns(std::uint64_t target_ns)
{
    for (;;) {
        const std::uint64_t now = monotonic_ns();
        if (now >= target_ns)
            return;
        const std::uint64_t remaining = target_ns - now;
        struct timespec delay = {};
        delay.tv_sec = static_cast<time_t>(remaining / 1000000000ULL);
        delay.tv_nsec = static_cast<long>(remaining % 1000000000ULL);
        if (nanosleep(&delay, nullptr) == 0 || errno != EINTR)
            return;
    }
}

static void close_fd(int *fd)
{
    if (*fd >= 0) {
        close(*fd);
        *fd = -1;
    }
}

static void close_fd_vector(std::vector<int> *fds)
{
    for (int &fd : *fds)
        close_fd(&fd);
    fds->clear();
}

static bool set_fd_cloexec(int fd, bool enabled)
{
    const int flags = fcntl(fd, F_GETFD);
    if (flags < 0)
        return false;
    const int updated = enabled ? (flags | FD_CLOEXEC) : (flags & ~FD_CLOEXEC);
    return fcntl(fd, F_SETFD, updated) == 0;
}

static bool send_packet(int socket_fd, const void *data, std::size_t size,
                        const std::vector<int> &fds)
{
    if (fds.size() > kMaxTransferFds) {
        std::fprintf(stderr, "FAIL stage=scm-rights-too-many-fds count=%zu\n",
                     fds.size());
        return false;
    }

    struct msghdr message = {};
    struct iovec iov = {};
    iov.iov_base = const_cast<void *>(data);
    iov.iov_len = size;
    message.msg_iov = &iov;
    message.msg_iovlen = 1;

    std::array<unsigned char, CMSG_SPACE(sizeof(int) * kMaxTransferFds)> control = {};
    if (!fds.empty()) {
        message.msg_control = control.data();
        message.msg_controllen = CMSG_SPACE(sizeof(int) * fds.size());
        struct cmsghdr *cmsg = CMSG_FIRSTHDR(&message);
        cmsg->cmsg_level = SOL_SOCKET;
        cmsg->cmsg_type = SCM_RIGHTS;
        cmsg->cmsg_len = CMSG_LEN(sizeof(int) * fds.size());
        std::memcpy(CMSG_DATA(cmsg), fds.data(), sizeof(int) * fds.size());
    }

    const ssize_t sent = sendmsg(socket_fd, &message, MSG_NOSIGNAL);
    if (sent != static_cast<ssize_t>(size)) {
        std::fprintf(stderr, "FAIL stage=scm-rights-send errno=%d (%s)\n",
                     errno, std::strerror(errno));
        return false;
    }
    return true;
}

static bool receive_packet(int socket_fd, void *data, std::size_t capacity,
                           std::size_t *received_size, std::vector<int> *fds,
                           int timeout_ms)
{
    fds->clear();
    struct pollfd wait_fd = {};
    wait_fd.fd = socket_fd;
    wait_fd.events = POLLIN;
    int poll_result;
    do {
        poll_result = poll(&wait_fd, 1, timeout_ms);
    } while (poll_result < 0 && errno == EINTR);
    if (poll_result == 0) {
        std::fprintf(stderr, "FAIL stage=control-socket-timeout\n");
        return false;
    }
    if (poll_result < 0 || (wait_fd.revents & POLLIN) == 0) {
        std::fprintf(stderr, "FAIL stage=control-socket-poll errno=%d (%s)\n",
                     errno, std::strerror(errno));
        return false;
    }

    struct msghdr message = {};
    struct iovec iov = {};
    iov.iov_base = data;
    iov.iov_len = capacity;
    message.msg_iov = &iov;
    message.msg_iovlen = 1;
    std::array<unsigned char, CMSG_SPACE(sizeof(int) * kMaxTransferFds)> control = {};
    message.msg_control = control.data();
    message.msg_controllen = control.size();

    const ssize_t got = recvmsg(socket_fd, &message, 0);
    if (got < 0) {
        std::fprintf(stderr, "FAIL stage=scm-rights-receive errno=%d (%s)\n",
                     errno, std::strerror(errno));
        return false;
    }
    if (got == 0 || (message.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) != 0) {
        std::fputs("FAIL stage=scm-rights-receive-truncated\n", stderr);
        return false;
    }

    for (struct cmsghdr *cmsg = CMSG_FIRSTHDR(&message); cmsg != nullptr;
         cmsg = CMSG_NXTHDR(&message, cmsg)) {
        if (cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS)
            continue;
        if (cmsg->cmsg_len < CMSG_LEN(0)) {
            std::fputs("FAIL stage=scm-rights-invalid-control\n", stderr);
            return false;
        }
        const std::size_t bytes = cmsg->cmsg_len - CMSG_LEN(0);
        if (bytes % sizeof(int) != 0 ||
            fds->size() + bytes / sizeof(int) > kMaxTransferFds) {
            std::fputs("FAIL stage=scm-rights-invalid-fd-count\n", stderr);
            return false;
        }
        const int *received = reinterpret_cast<const int *>(CMSG_DATA(cmsg));
        for (std::size_t i = 0; i < bytes / sizeof(int); ++i)
            fds->push_back(received[i]);
    }

    *received_size = static_cast<std::size_t>(got);
    return true;
}

static bool wait_eventfd(int fd, const char *stage, std::uint64_t *wake_ns,
                         std::uint64_t *timeouts)
{
    struct pollfd wait_fd = {};
    wait_fd.fd = fd;
    wait_fd.events = POLLIN;
    int poll_result;
    do {
        poll_result = poll(&wait_fd, 1, kPollTimeoutMs);
    } while (poll_result < 0 && errno == EINTR);
    if (poll_result == 0) {
        ++*timeouts;
        std::fprintf(stderr, "FAIL stage=%s-timeout\n", stage);
        return false;
    }
    if (poll_result < 0 || (wait_fd.revents & POLLIN) == 0) {
        std::fprintf(stderr, "FAIL stage=%s-poll errno=%d (%s)\n", stage,
                     errno, std::strerror(errno));
        return false;
    }

    eventfd_t notifications = 0;
    int read_result;
    do {
        read_result = eventfd_read(fd, &notifications);
    } while (read_result < 0 && errno == EINTR);
    if (read_result < 0 || notifications == 0) {
        std::fprintf(stderr, "FAIL stage=%s-read errno=%d (%s)\n", stage,
                     errno, std::strerror(errno));
        return false;
    }
    *wake_ns = monotonic_ns();
    return true;
}

static bool register_event(ID3D12Fence *fence, std::uint64_t value, int fd,
                           const char *stage)
{
    const HRESULT hr = fence->SetEventOnCompletion(
        static_cast<UINT64>(value),
        reinterpret_cast<HANDLE>(static_cast<intptr_t>(fd)));
    if (FAILED(hr)) {
        std::fprintf(stderr, "FAIL stage=%s HRESULT=0x%08x\n", stage,
                     static_cast<unsigned>(hr));
        return false;
    }
    return true;
}

static bool hr_ok(HRESULT hr, const char *stage)
{
    if (SUCCEEDED(hr))
        return true;
    std::fprintf(stderr, "FAIL stage=%s HRESULT=0x%08x\n", stage,
                 static_cast<unsigned>(hr));
    return false;
}

static int make_eventfd(const char *stage)
{
    const int fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (fd < 0)
        std::fprintf(stderr, "FAIL stage=%s errno=%d (%s)\n", stage, errno,
                     std::strerror(errno));
    return fd;
}

struct DeviceContext {
    ComPtr<IDXCoreAdapterFactory> factory;
    ComPtr<IDXCoreAdapterList> adapters;
    ComPtr<IDXCoreAdapter> adapter;
    ComPtr<ID3D12Device> device;
};

static bool create_device(const char *role, DeviceContext *context)
{
    const GUID attributes[] = {DXCORE_ADAPTER_ATTRIBUTE_D3D12_CORE_COMPUTE};
    if (!hr_ok(DXCoreCreateAdapterFactory(IID_PPV_ARGS(&context->factory)),
               "dxcore-factory") ||
        !hr_ok(context->factory->CreateAdapterList(
                   1, attributes, IID_PPV_ARGS(&context->adapters)),
               "dxcore-adapter-list"))
        return false;
    if (context->adapters->GetAdapterCount() == 0) {
        std::fprintf(stderr, "FAIL stage=%s-no-adapter\n", role);
        return false;
    }
    if (!hr_ok(context->adapters->GetAdapter(0, IID_PPV_ARGS(&context->adapter)),
               "dxcore-get-adapter") ||
        !hr_ok(D3D12CreateDevice(context->adapter.Get(), D3D_FEATURE_LEVEL_11_0,
                                 IID_PPV_ARGS(&context->device)),
               role))
        return false;

    char description[256] = {};
    std::size_t description_size = 0;
    if (context->adapter->IsPropertySupported(
            DXCoreAdapterProperty::DriverDescription) &&
        SUCCEEDED(context->adapter->GetPropertySize(
            DXCoreAdapterProperty::DriverDescription, &description_size)) &&
        description_size > 0 && description_size <= sizeof(description)) {
        (void)context->adapter->GetProperty(
            DXCoreAdapterProperty::DriverDescription, description_size,
            description);
    }
    std::printf("role=%s pid=%ld adapter=%s\n", role,
                static_cast<long>(getpid()),
                description[0] ? description : "(unknown)");
    return true;
}

static D3D12_RESOURCE_DESC texture_desc()
{
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = kWidth;
    desc.Height = kHeight;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    return desc;
}

static D3D12_HEAP_PROPERTIES default_heap_properties()
{
    D3D12_HEAP_PROPERTIES properties = {};
    properties.Type = D3D12_HEAP_TYPE_DEFAULT;
    properties.CreationNodeMask = 1;
    properties.VisibleNodeMask = 1;
    return properties;
}

static D3D12_RESOURCE_DESC buffer_desc(UINT64 size)
{
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = size;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return desc;
}

static void frame_color(std::uint64_t frame, float color[4])
{
    color[0] = static_cast<float>(31U + ((frame * 3U) % 96U)) / 255.0f;
    color[1] = static_cast<float>(127U + ((frame * 5U) % 64U)) / 255.0f;
    color[2] = static_cast<float>(223U - ((frame * 7U) % 64U)) / 255.0f;
    color[3] = 1.0f;
}

static std::array<std::uint8_t, 4> expected_rgba(std::uint64_t frame)
{
    return {static_cast<std::uint8_t>(31U + ((frame * 3U) % 96U)),
            static_cast<std::uint8_t>(127U + ((frame * 5U) % 64U)),
            static_cast<std::uint8_t>(223U - ((frame * 7U) % 64U)), 255};
}

static bool expected_pixel_for_format(
    std::uint32_t format, const std::array<std::uint8_t, 4> &semantic,
    std::array<std::uint8_t, 4> *pixel)
{
    switch (format) {
    case kDxgiFormatR8G8B8A8Unorm:
        *pixel = semantic;
        return true;
    case kDxgiFormatB8G8R8A8Unorm:
        *pixel = {semantic[2], semantic[1], semantic[0], semantic[3]};
        return true;
    default:
        return false;
    }
}

static bool close_enough(const std::uint8_t *actual,
                         const std::array<std::uint8_t, 4> &expected)
{
    for (unsigned i = 0; i < 4; ++i) {
        const int delta = static_cast<int>(actual[i]) - expected[i];
        if (delta < -1 || delta > 1)
            return false;
    }
    return true;
}

static double percentile_us(std::vector<std::uint64_t> values, double q)
{
    if (values.empty())
        return 0.0;
    std::sort(values.begin(), values.end());
    const double position = q * static_cast<double>(values.size() - 1);
    const std::size_t index = static_cast<std::size_t>(position + 0.5);
    return static_cast<double>(values[index]) / 1000.0;
}

static double mean_us(const std::vector<std::uint64_t> &values)
{
    if (values.empty())
        return 0.0;
    long double total = 0.0;
    for (const std::uint64_t value : values)
        total += static_cast<long double>(value);
    return static_cast<double>(total / values.size() / 1000.0L);
}

static double min_us(const std::vector<std::uint64_t> &values)
{
    if (values.empty())
        return 0.0;
    return static_cast<double>(*std::min_element(values.begin(), values.end())) /
           1000.0;
}

static double max_us(const std::vector<std::uint64_t> &values)
{
    if (values.empty())
        return 0.0;
    return static_cast<double>(*std::max_element(values.begin(), values.end())) /
           1000.0;
}

struct ProducerSlot {
    ComPtr<ID3D12Resource> texture;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    int resource_fd = -1;
    int ready_fd = -1;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = {};
};

struct ConsumerSlot {
    ComPtr<ID3D12Resource> texture;
    ComPtr<ID3D12Resource> sink;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    int resource_fd = -1;
    int ready_fd = -1;
    int done_fd = -1;
};

static bool producer_main(int control_fd)
{
    DeviceContext context;
    if (!create_device("producer-device", &context))
        return false;

    D3D12_COMMAND_QUEUE_DESC queue_desc = {};
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> queue;
    if (!hr_ok(context.device->CreateCommandQueue(
                   &queue_desc, IID_PPV_ARGS(&queue)),
               "producer-queue"))
        return false;

    const D3D12_RESOURCE_DESC desc = texture_desc();
    const D3D12_HEAP_PROPERTIES heap = default_heap_properties();
    const float initial_color[4] = {0, 0, 0, 1};
    D3D12_CLEAR_VALUE clear_value = {};
    clear_value.Format = desc.Format;
    std::memcpy(clear_value.Color, initial_color, sizeof(initial_color));

    D3D12_DESCRIPTOR_HEAP_DESC rtv_heap_desc = {};
    rtv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtv_heap_desc.NumDescriptors = kSlotCount;
    ComPtr<ID3D12DescriptorHeap> rtv_heap;
    if (!hr_ok(context.device->CreateDescriptorHeap(
                   &rtv_heap_desc, IID_PPV_ARGS(&rtv_heap)),
               "producer-rtv-heap"))
        return false;
    const UINT rtv_increment = context.device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    D3D12_CPU_DESCRIPTOR_HANDLE rtv_start =
        rtv_heap->GetCPUDescriptorHandleForHeapStart();

    std::array<ProducerSlot, kSlotCount> slots;
    for (std::uint32_t slot = 0; slot < kSlotCount; ++slot) {
        if (!hr_ok(context.device->CreateCommittedResource(
                       &heap, D3D12_HEAP_FLAG_SHARED, &desc,
                       D3D12_RESOURCE_STATE_COMMON, &clear_value,
                       IID_PPV_ARGS(&slots[slot].texture)),
                   "create-shared-texture"))
            return false;
        slots[slot].rtv = rtv_start;
        slots[slot].rtv.ptr += static_cast<std::size_t>(slot) * rtv_increment;
        context.device->CreateRenderTargetView(slots[slot].texture.Get(),
                                                nullptr, slots[slot].rtv);
        if (!hr_ok(context.device->CreateCommandAllocator(
                       D3D12_COMMAND_LIST_TYPE_DIRECT,
                       IID_PPV_ARGS(&slots[slot].allocator)),
                   "producer-command-allocator") ||
            !hr_ok(context.device->CreateCommandList(
                       0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                       slots[slot].allocator.Get(), nullptr,
                       IID_PPV_ARGS(&slots[slot].list)),
                   "producer-command-list") ||
            !hr_ok(slots[slot].list->Close(), "producer-command-close"))
            return false;

        HANDLE shared_handle = nullptr;
        if (!hr_ok(context.device->CreateSharedHandle(
                       slots[slot].texture.Get(), nullptr, GENERIC_ALL, nullptr,
                       &shared_handle),
                   "export-resource"))
            return false;
        const intptr_t raw = reinterpret_cast<intptr_t>(shared_handle);
        if (raw < 0 || raw > INT_MAX) {
            std::fprintf(stderr, "FAIL stage=export-resource-handle value=%p\n",
                         shared_handle);
            return false;
        }
        slots[slot].resource_fd = static_cast<int>(raw);
        if (slots[slot].resource_fd < 0 ||
            fcntl(slots[slot].resource_fd, F_GETFD) < 0) {
            std::fprintf(stderr, "FAIL stage=export-resource-fd errno=%d (%s)\n",
                         errno, std::strerror(errno));
            return false;
        }
        slots[slot].ready_fd = make_eventfd("producer-ready-eventfd");
        if (slots[slot].ready_fd < 0)
            return false;
    }

    ComPtr<ID3D12Fence> producer_fence;
    if (!hr_ok(context.device->CreateFence(
                   0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&producer_fence)),
               "producer-fence"))
        return false;

    std::vector<int> transfer_fds;
    transfer_fds.reserve(kMaxTransferFds);
    for (const ProducerSlot &slot : slots) {
        transfer_fds.push_back(slot.resource_fd);
        transfer_fds.push_back(slot.ready_fd);
    }
    const ResourceBundleMessage bundle = {
        kProtocolMagic, kResourceBundle, kWidth, kHeight, kSlotCount,
        kFrameCount, kDxgiFormatB8G8R8A8Unorm, 1, 0, kSlotCount};
    if (!send_packet(control_fd, &bundle, sizeof(bundle), transfer_fds))
        return false;
    std::printf("PASS stage=cross-process-resource-fd-transfer slots=%u "
                "scm_rights=1 resource_fds=%u\n",
                kSlotCount, kSlotCount);

    ConsumerReadyMessage ready = {};
    std::vector<int> done_fds;
    std::size_t ready_size = 0;
    if (!receive_packet(control_fd, &ready, sizeof(ready), &ready_size,
                        &done_fds, kSocketTimeoutMs) ||
        ready_size != sizeof(ready) || ready.magic != kProtocolMagic ||
        ready.type != kConsumerReady || ready.status != 0 ||
        ready.opened_resources != kSlotCount ||
        ready.done_eventfds != kSlotCount || done_fds.size() != kSlotCount) {
        close_fd_vector(&done_fds);
        std::fputs("FAIL stage=consumer-open-shared-resource-ack\n", stderr);
        return false;
    }
    std::array<int, kSlotCount> producer_done_fds = {};
    for (std::uint32_t slot = 0; slot < kSlotCount; ++slot)
        producer_done_fds[slot] = done_fds[slot];
    done_fds.clear();

    for (ProducerSlot &slot : slots)
        close(slot.resource_fd), slot.resource_fd = -1;
    std::puts("PASS stage=resource-transport-fd-close producer=3 consumer=3");

    std::uint64_t producer_timeouts = 0;
    const std::uint64_t run_start_ns = monotonic_ns();
    constexpr std::uint64_t frame_period_ns = 1000000000ULL / 60ULL;
    bool ready_stage_logged = false;
    bool reuse_stage_logged = false;

    for (std::uint64_t frame = 0; frame < kFrameCount; ++frame) {
        const std::uint32_t slot_index =
            static_cast<std::uint32_t>(frame % kSlotCount);
        if (frame >= kSlotCount) {
            std::uint64_t wake_ns = 0;
            if (!wait_eventfd(producer_done_fds[slot_index],
                              "producer-done-eventfd", &wake_ns,
                              &producer_timeouts))
                return false;
            if (!reuse_stage_logged) {
                std::puts("PASS stage=triple-buffer-reuse slots=3");
                reuse_stage_logged = true;
            }
        }
        sleep_until_ns(run_start_ns + frame * frame_period_ns);

        ProducerSlot &slot = slots[slot_index];
        if (!hr_ok(slot.allocator->Reset(), "producer-allocator-reset") ||
            !hr_ok(slot.list->Reset(slot.allocator.Get(), nullptr),
                   "producer-list-reset"))
            return false;
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = slot.texture.Get();
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        slot.list->ResourceBarrier(1, &barrier);
        float color[4] = {};
        frame_color(frame, color);
        slot.list->ClearRenderTargetView(slot.rtv, color, 0, nullptr);
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
        slot.list->ResourceBarrier(1, &barrier);
        if (!hr_ok(slot.list->Close(), "producer-list-close"))
            return false;

        ID3D12CommandList *lists[] = {slot.list.Get()};
        queue->ExecuteCommandLists(1, lists);
        const std::uint64_t fence_value = frame + 1;
        if (!hr_ok(queue->Signal(producer_fence.Get(), fence_value),
                   "producer-fence-signal"))
            return false;
        const std::uint64_t signal_ns = monotonic_ns();
        if (!register_event(producer_fence.Get(), fence_value, slot.ready_fd,
                            "producer-ready-eventfd"))
            return false;
        if (!ready_stage_logged) {
            std::puts("PASS stage=producer-ready-eventfd slots=3 "
                       "set_event_on_completion=1");
            ready_stage_logged = true;
        }

        const auto expected = expected_rgba(frame);
        const FrameInfoMessage frame_info = {
            kProtocolMagic, kFrameInfo, frame, slot_index,
            ((frame + 1) % kDiagnosticInterval) == 0 ? 1U : 0U, signal_ns,
            expected[0], expected[1], expected[2], expected[3], 1};
        if (!send_packet(control_fd, &frame_info, sizeof(frame_info), {}))
            return false;
    }

    for (std::uint32_t slot = 0; slot < kSlotCount; ++slot) {
        std::uint64_t wake_ns = 0;
        if (!wait_eventfd(producer_done_fds[slot], "producer-final-done-eventfd",
                          &wake_ns, &producer_timeouts))
            return false;
    }
    const std::uint64_t run_end_ns = monotonic_ns();
    const double elapsed_seconds =
        static_cast<double>(run_end_ns - run_start_ns) / 1000000000.0;
    const double fps = static_cast<double>(kFrameCount) / elapsed_seconds;

    ConsumerResultMessage result = {};
    std::vector<int> unexpected_fds;
    std::size_t result_size = 0;
    if (!receive_packet(control_fd, &result, sizeof(result), &result_size,
                        &unexpected_fds, kSocketTimeoutMs) ||
        result_size != sizeof(result) || result.magic != kProtocolMagic ||
        result.type != kConsumerResult) {
        close_fd_vector(&unexpected_fds);
        std::fputs("FAIL stage=consumer-result\n", stderr);
        return false;
    }
    close_fd_vector(&unexpected_fds);

    const bool fps_ok = fps >= 59.0 && fps <= 61.0;
    const bool result_ok = result.status == 0 && result.frames == kFrameCount &&
                           result.timeouts == 0 && result.mismatches == 0 &&
                           result.resource_reopen_failures == 0 && fps_ok &&
                           producer_timeouts == 0;
    std::printf("frames=%u fps=%.2f timeouts=%llu mismatches=%llu "
                "resource_reopen_failures=%llu\n",
                kFrameCount, fps,
                static_cast<unsigned long long>(result.timeouts +
                                                producer_timeouts),
                static_cast<unsigned long long>(result.mismatches),
                static_cast<unsigned long long>(
                    result.resource_reopen_failures));
    if (!result_ok) {
        std::fputs("FAIL stage=4k60-sustained\n", stderr);
        return false;
    }
    std::puts("PASS stage=4k60-sustained");
    std::puts("PASS stage=throughput-zero-copy framebuffer_mmap=0 "
               "cpu_memcpy_framebuffer=0 gpu_cpu_gpu=0");

    for (std::uint32_t slot = 0; slot < kSlotCount; ++slot) {
        close_fd(&slots[slot].ready_fd);
        close_fd(&producer_done_fds[slot]);
    }
    return true;
}

static bool consumer_main(int control_fd)
{
    DeviceContext context;
    if (!create_device("consumer-device", &context))
        return false;

    ResourceBundleMessage bundle = {};
    std::vector<int> received_fds;
    std::size_t bundle_size = 0;
    if (!receive_packet(control_fd, &bundle, sizeof(bundle), &bundle_size,
                        &received_fds, kSocketTimeoutMs) ||
        bundle_size != sizeof(bundle) || bundle.magic != kProtocolMagic ||
        bundle.type != kResourceBundle || bundle.width != kWidth ||
        bundle.height != kHeight || bundle.slots != kSlotCount ||
        bundle.frames != kFrameCount || received_fds.size() != kMaxTransferFds) {
        close_fd_vector(&received_fds);
        std::fputs("FAIL stage=cross-process-resource-fd-transfer\n", stderr);
        return false;
    }
    if (bundle.format != kDxgiFormatR8G8B8A8Unorm &&
        bundle.format != kDxgiFormatB8G8R8A8Unorm) {
        std::fprintf(stderr,
                     "BLOCKED stage=resource-format format=%u "
                     "reason=unsupported-dxgi-format\n",
                     bundle.format);
        close_fd_vector(&received_fds);
        return false;
    }

    std::array<ConsumerSlot, kSlotCount> slots;
    for (std::uint32_t slot = 0; slot < kSlotCount; ++slot) {
        slots[slot].resource_fd = received_fds[slot * 2];
        slots[slot].ready_fd = received_fds[slot * 2 + 1];
    }
    received_fds.clear();

    std::uint64_t reopen_failures = 0;
    for (std::uint32_t slot = 0; slot < kSlotCount; ++slot) {
        const HRESULT hr = context.device->OpenSharedHandle(
            reinterpret_cast<HANDLE>(
                static_cast<intptr_t>(slots[slot].resource_fd)),
            IID_PPV_ARGS(&slots[slot].texture));
        close_fd(&slots[slot].resource_fd);
        if (FAILED(hr)) {
            ++reopen_failures;
            std::fprintf(stderr,
                         "FAIL stage=open-shared-resource slot=%u "
                         "HRESULT=0x%08x\n",
                         slot, static_cast<unsigned>(hr));
        }
    }
    if (reopen_failures != 0) {
        ConsumerReadyMessage failure = {
            kProtocolMagic, kConsumerReady, 1, 0, 0};
        (void)send_packet(control_fd, &failure, sizeof(failure), {});
        for (ConsumerSlot &slot : slots)
            close_fd(&slot.ready_fd);
        return false;
    }
    std::puts("PASS stage=cross-process-open-shared-resource slots=3 "
               "consumer_device=independent");
    std::puts("PASS stage=resource-transport-fd-close side=consumer count=3");

    D3D12_COMMAND_QUEUE_DESC queue_desc = {};
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> queue;
    if (!hr_ok(context.device->CreateCommandQueue(
                   &queue_desc, IID_PPV_ARGS(&queue)),
               "consumer-queue"))
        return false;

    const D3D12_RESOURCE_DESC desc = texture_desc();
    const D3D12_HEAP_PROPERTIES heap = default_heap_properties();
    D3D12_RESOURCE_DESC sink_desc = desc;
    sink_desc.Flags = D3D12_RESOURCE_FLAG_NONE;
    for (std::uint32_t slot = 0; slot < kSlotCount; ++slot) {
        if (!hr_ok(context.device->CreateCommittedResource(
                       &heap, D3D12_HEAP_FLAG_NONE, &sink_desc,
                       D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                       IID_PPV_ARGS(&slots[slot].sink)),
                   "consumer-gpu-sink") ||
            !hr_ok(context.device->CreateCommandAllocator(
                       D3D12_COMMAND_LIST_TYPE_DIRECT,
                       IID_PPV_ARGS(&slots[slot].allocator)),
                   "consumer-command-allocator") ||
            !hr_ok(context.device->CreateCommandList(
                       0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                       slots[slot].allocator.Get(), nullptr,
                       IID_PPV_ARGS(&slots[slot].list)),
                   "consumer-command-list") ||
            !hr_ok(slots[slot].list->Close(), "consumer-command-close"))
            return false;
    }

    UINT rows = 0;
    UINT64 row_size = 0;
    UINT64 readback_size = 0;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
    context.device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, &rows,
                                          &row_size, &readback_size);
    (void)rows;
    (void)row_size;
    D3D12_HEAP_PROPERTIES readback_heap = {};
    readback_heap.Type = D3D12_HEAP_TYPE_READBACK;
    readback_heap.CreationNodeMask = 1;
    readback_heap.VisibleNodeMask = 1;
    const D3D12_RESOURCE_DESC readback_desc = buffer_desc(readback_size);
    ComPtr<ID3D12Resource> readback;
    if (!hr_ok(context.device->CreateCommittedResource(
                   &readback_heap, D3D12_HEAP_FLAG_NONE, &readback_desc,
                   D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                   IID_PPV_ARGS(&readback)),
               "consumer-diagnostic-readback"))
        return false;

    ComPtr<ID3D12Fence> done_fence;
    ComPtr<ID3D12Fence> profile_fence;
    if (!hr_ok(context.device->CreateFence(
                   0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&done_fence)),
               "consumer-done-fence") ||
        !hr_ok(context.device->CreateFence(
                   0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&profile_fence)),
               "consumer-profile-fence"))
        return false;
    const int profile_eventfd = make_eventfd("consumer-profile-eventfd");
    if (profile_eventfd < 0)
        return false;

    std::array<int, kSlotCount> done_fds = {};
    std::vector<int> done_transfer_fds;
    for (std::uint32_t slot = 0; slot < kSlotCount; ++slot) {
        done_fds[slot] = make_eventfd("consumer-done-eventfd");
        if (done_fds[slot] < 0) {
            close(profile_eventfd);
            for (std::uint32_t previous = 0; previous < slot; ++previous)
                close_fd(&done_fds[previous]);
            return false;
        }
        slots[slot].done_fd = done_fds[slot];
        done_transfer_fds.push_back(done_fds[slot]);
    }
    const ConsumerReadyMessage ready = {
        kProtocolMagic, kConsumerReady, 0, kSlotCount, kSlotCount};
    if (!send_packet(control_fd, &ready, sizeof(ready), done_transfer_fds)) {
        close(profile_eventfd);
        for (int &fd : done_fds)
            close_fd(&fd);
        for (ConsumerSlot &slot : slots)
            close_fd(&slot.ready_fd);
        return false;
    }
    std::puts("PASS stage=consumer-done-eventfd slots=3 "
               "set_event_on_completion=1 scm_rights=1");

    std::vector<std::uint64_t> wake_latencies;
    std::vector<std::uint64_t> gpu_work_times;
    std::uint64_t timeouts = 0;
    std::uint64_t mismatches = 0;
    std::uint64_t diagnostic_checks = 0;
    bool gpu_stage_logged = false;

    for (std::uint64_t expected_frame = 0; expected_frame < kFrameCount;
         ++expected_frame) {
        FrameInfoMessage frame_info = {};
        std::vector<int> unexpected_fds;
        std::size_t frame_size = 0;
        if (!receive_packet(control_fd, &frame_info, sizeof(frame_info),
                            &frame_size, &unexpected_fds, kSocketTimeoutMs) ||
            frame_size != sizeof(frame_info) || frame_info.magic != kProtocolMagic ||
            frame_info.type != kFrameInfo || frame_info.frame != expected_frame ||
            frame_info.slot >= kSlotCount || !unexpected_fds.empty()) {
            close_fd_vector(&unexpected_fds);
            std::fputs("FAIL stage=frame-control-sequence\n", stderr);
            close(profile_eventfd);
            return false;
        }
        close_fd_vector(&unexpected_fds);

        const std::uint32_t slot_index = frame_info.slot;
        std::uint64_t ready_ns = 0;
        if (!wait_eventfd(slots[slot_index].ready_fd,
                          "consumer-ready-eventfd", &ready_ns, &timeouts)) {
            close(profile_eventfd);
            return false;
        }
        if (ready_ns >= frame_info.producer_signal_ns)
            wake_latencies.push_back(ready_ns - frame_info.producer_signal_ns);

        ConsumerSlot &slot = slots[slot_index];
        if (!hr_ok(slot.allocator->Reset(), "consumer-allocator-reset") ||
            !hr_ok(slot.list->Reset(slot.allocator.Get(), nullptr),
                   "consumer-list-reset")) {
            close(profile_eventfd);
            return false;
        }
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = slot.texture.Get();
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        slot.list->ResourceBarrier(1, &barrier);
        D3D12_TEXTURE_COPY_LOCATION source = {};
        source.pResource = slot.texture.Get();
        source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        source.SubresourceIndex = 0;
        D3D12_TEXTURE_COPY_LOCATION sink_destination = {};
        sink_destination.pResource = slot.sink.Get();
        sink_destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        sink_destination.SubresourceIndex = 0;
        slot.list->CopyTextureRegion(&sink_destination, 0, 0, 0, &source,
                                     nullptr);
        if (frame_info.diagnostic != 0) {
            D3D12_TEXTURE_COPY_LOCATION readback_destination = {};
            readback_destination.pResource = readback.Get();
            readback_destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            readback_destination.PlacedFootprint = footprint;
            slot.list->CopyTextureRegion(&readback_destination, 0, 0, 0,
                                         &source, nullptr);
        }
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
        slot.list->ResourceBarrier(1, &barrier);
        if (!hr_ok(slot.list->Close(), "consumer-list-close")) {
            close(profile_eventfd);
            return false;
        }

        ID3D12CommandList *lists[] = {slot.list.Get()};
        const std::uint64_t fence_value = expected_frame + 1;
        const std::uint64_t gpu_start_ns = monotonic_ns();
        queue->ExecuteCommandLists(1, lists);
        if (!hr_ok(queue->Signal(done_fence.Get(), fence_value),
                   "consumer-done-fence-signal") ||
            !hr_ok(queue->Signal(profile_fence.Get(), fence_value),
                   "consumer-profile-fence-signal") ||
            !register_event(done_fence.Get(), fence_value, slot.done_fd,
                            "consumer-done-eventfd") ||
            !register_event(profile_fence.Get(), fence_value, profile_eventfd,
                            "consumer-profile-eventfd")) {
            close(profile_eventfd);
            return false;
        }
        if (!gpu_stage_logged) {
            std::puts("PASS stage=consumer-gpu-operation copy=GPU-only "
                       "cpu_framebuffer_copy=0");
            gpu_stage_logged = true;
        }
        std::uint64_t gpu_end_ns = 0;
        if (!wait_eventfd(profile_eventfd, "consumer-gpu-operation",
                          &gpu_end_ns, &timeouts)) {
            close(profile_eventfd);
            return false;
        }
        if (gpu_end_ns >= gpu_start_ns)
            gpu_work_times.push_back(gpu_end_ns - gpu_start_ns);

        if (frame_info.diagnostic != 0) {
            void *mapped = nullptr;
            const D3D12_RANGE read_range = {0, readback_size};
            if (!hr_ok(readback->Map(0, &read_range, &mapped),
                       "diagnostic-readback-map")) {
                close(profile_eventfd);
                return false;
            }
            const auto *bytes = static_cast<const std::uint8_t *>(mapped) +
                                footprint.Offset;
            const auto semantic = expected_rgba(expected_frame);
            std::array<std::uint8_t, 4> expected = {};
            if (!expected_pixel_for_format(bundle.format, semantic, &expected)) {
                std::fprintf(stderr,
                             "BLOCKED stage=diagnostic-format format=%u\n",
                             bundle.format);
                readback->Unmap(0, nullptr);
                close(profile_eventfd);
                return false;
            }
            const std::array<std::pair<UINT, UINT>, 3> points = {
                std::make_pair(0U, 0U),
                std::make_pair(kWidth / 2, kHeight / 2),
                std::make_pair(kWidth - 1, kHeight - 1)};
            bool frame_ok = true;
            for (const auto &point : points) {
                const std::size_t offset =
                    static_cast<std::size_t>(point.second) *
                        footprint.Footprint.RowPitch +
                    static_cast<std::size_t>(point.first) * 4;
                if (!close_enough(bytes + offset, expected))
                    frame_ok = false;
            }
            const D3D12_RANGE written_range = {0, 0};
            readback->Unmap(0, &written_range);
            ++diagnostic_checks;
            if (!frame_ok) {
                ++mismatches;
                std::fprintf(stderr,
                             "FAIL stage=diagnostic-frame-sequence frame=%llu\n",
                             static_cast<unsigned long long>(expected_frame));
            }
        }
    }

    std::printf("wake_latency_us p50=%.3f p95=%.3f p99=%.3f samples=%zu\n",
                percentile_us(wake_latencies, 0.50),
                percentile_us(wake_latencies, 0.95),
                percentile_us(wake_latencies, 0.99), wake_latencies.size());
    std::printf("gpu_work_us min=%.3f mean=%.3f max=%.3f p50=%.3f "
                "p95=%.3f p99=%.3f samples=%zu\n",
                min_us(gpu_work_times), mean_us(gpu_work_times),
                max_us(gpu_work_times),
                percentile_us(gpu_work_times, 0.50),
                percentile_us(gpu_work_times, 0.95),
                percentile_us(gpu_work_times, 0.99), gpu_work_times.size());
    if (mismatches == 0)
        std::printf("PASS stage=diagnostic-frame-sequence mismatches=0 checks=%llu\n",
                    static_cast<unsigned long long>(diagnostic_checks));

    const ConsumerResultMessage result = {
        kProtocolMagic, kConsumerResult,
        (timeouts == 0 && mismatches == 0 &&
         diagnostic_checks == kFrameCount / kDiagnosticInterval)
            ? 0U
            : 1U,
        0,
        kFrameCount,
        timeouts,
        mismatches,
        diagnostic_checks,
        reopen_failures};
    const bool sent = send_packet(control_fd, &result, sizeof(result), {});

    for (ConsumerSlot &slot : slots) {
        close_fd(&slot.ready_fd);
        close_fd(&slot.done_fd);
    }
    close(profile_eventfd);
    if (!sent)
        return false;
    return result.status == 0;
}

static void exec_role(const char *self, const char *role, int control_fd)
{
    char fd_text[32] = {};
    std::snprintf(fd_text, sizeof(fd_text), "%d", control_fd);
    (void)set_fd_cloexec(control_fd, false);
    if (std::strchr(self, '/') != nullptr)
        execl(self, self, role, fd_text, static_cast<char *>(nullptr));
    else
        execlp(self, self, role, fd_text, static_cast<char *>(nullptr));
    std::fprintf(stderr, "FAIL stage=exec-%s errno=%d (%s)\n", role, errno,
                 std::strerror(errno));
    _exit(127);
}

static int launch_children(const char *self)
{
    int sockets[2] = {-1, -1};
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sockets) != 0) {
        std::fprintf(stderr, "FAIL stage=control-socket errno=%d (%s)\n", errno,
                     std::strerror(errno));
        return 1;
    }

    pid_t consumer_pid = fork();
    if (consumer_pid == 0) {
        close(sockets[0]);
        exec_role(self, "--consumer", sockets[1]);
    }
    if (consumer_pid < 0) {
        std::fprintf(stderr, "FAIL stage=fork-consumer errno=%d (%s)\n", errno,
                     std::strerror(errno));
        close(sockets[0]);
        close(sockets[1]);
        return 1;
    }

    pid_t producer_pid = fork();
    if (producer_pid == 0) {
        close(sockets[1]);
        exec_role(self, "--producer", sockets[0]);
    }
    if (producer_pid < 0) {
        std::fprintf(stderr, "FAIL stage=fork-producer errno=%d (%s)\n", errno,
                     std::strerror(errno));
        close(sockets[0]);
        close(sockets[1]);
        (void)waitpid(consumer_pid, nullptr, 0);
        return 1;
    }
    close(sockets[0]);
    close(sockets[1]);
    std::printf("PASS stage=independent-processes producer_pid=%ld "
                "consumer_pid=%ld exec=1 inherited_d3d12_device=0\n",
                static_cast<long>(producer_pid), static_cast<long>(consumer_pid));

    int producer_status = 0;
    int consumer_status = 0;
    (void)waitpid(producer_pid, &producer_status, 0);
    (void)waitpid(consumer_pid, &consumer_status, 0);
    const bool producer_ok = WIFEXITED(producer_status) &&
                             WEXITSTATUS(producer_status) == 0;
    const bool consumer_ok = WIFEXITED(consumer_status) &&
                             WEXITSTATUS(consumer_status) == 0;
    if (!producer_ok || !consumer_ok) {
        std::fprintf(stderr,
                     "FAIL stage=child-exit producer_status=%d "
                     "consumer_status=%d\n",
                     producer_status, consumer_status);
        return 1;
    }
    std::puts("PASS d3d12-cross-process-zero-copy");
    return 0;
}

} // namespace

int main(int argc, char **argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    (void)signal(SIGPIPE, SIG_IGN);

    if (argc == 3 && std::strcmp(argv[1], "--producer") == 0) {
        char *end = nullptr;
        const long fd = std::strtol(argv[2], &end, 10);
        if (end == argv[2] || *end != '\0' || fd < 0 || fd > INT_MAX)
            return 2;
        return producer_main(static_cast<int>(fd)) ? 0 : 1;
    }
    if (argc == 3 && std::strcmp(argv[1], "--consumer") == 0) {
        char *end = nullptr;
        const long fd = std::strtol(argv[2], &end, 10);
        if (end == argv[2] || *end != '\0' || fd < 0 || fd > INT_MAX)
            return 2;
        return consumer_main(static_cast<int>(fd)) ? 0 : 1;
    }
    if (argc != 1) {
        std::fprintf(stderr, "Usage: %s\n", argv[0]);
        return 2;
    }
    return launch_children(argv[0]);
}
