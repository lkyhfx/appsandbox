/* SPDX-License-Identifier: MIT
 *
 * Wire ABI shared by the synthetic D3D12 probes and the isolated Mutter
 * publisher.  Keep this header free of DirectX types: the Mutter/Mesa side
 * only needs the Linux socket ABI, while the encoder includes DirectX.
 */
#ifndef ASB_D3D12_SHARE_PROBE_PROTOCOL_H
#define ASB_D3D12_SHARE_PROBE_PROTOCOL_H

#include <cstdint>

namespace asb_d3d12_share_probe {

constexpr std::uint32_t kProtocolMagic = 0x41534433; /* "3DSA" */
constexpr std::uint32_t kSlotCount = 3;
constexpr std::uint32_t kWidth = 3840;
constexpr std::uint32_t kHeight = 2160;
constexpr std::uint32_t kFrameCount = 3600;
constexpr std::uint32_t kDiagnosticInterval = 120;
constexpr std::uint32_t kDxgiFormatB8G8R8A8Unorm = 87;

enum MessageType : std::uint32_t {
    kResourceBundle = 1,
    kConsumerReady = 2,
    kFrameInfo = 3,
    kConsumerResult = 4,
};

struct ResourceBundleMessage {
    std::uint32_t magic;
    std::uint32_t type;
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t slots;
    std::uint32_t frames;
    std::uint32_t format;
    std::uint32_t synthetic_source;
    std::uint32_t gpu_copy;
    std::uint32_t buffer_count;
};

struct ConsumerReadyMessage {
    std::uint32_t magic;
    std::uint32_t type;
    std::uint32_t status;
    std::uint32_t opened_resources;
    std::uint32_t done_eventfds;
};

struct FrameInfoMessage {
    std::uint32_t magic;
    std::uint32_t type;
    std::uint64_t frame;
    std::uint32_t slot;
    std::uint32_t diagnostic;
    std::uint64_t producer_signal_ns;
    std::uint8_t expected_bgra[4];
    std::uint32_t expected_valid;
};

struct ConsumerResultMessage {
    std::uint32_t magic;
    std::uint32_t type;
    std::uint32_t status;
    std::uint32_t reserved;
    std::uint64_t frames;
    std::uint64_t timeouts;
    std::uint64_t mismatches;
    std::uint64_t diagnostic_checks;
    std::uint64_t resource_reopen_failures;
};

static_assert(sizeof(ResourceBundleMessage) == 40,
              "resource protocol packing changed");
static_assert(sizeof(ConsumerReadyMessage) == 20,
              "ready protocol packing changed");
static_assert(sizeof(FrameInfoMessage) == 40,
              "frame protocol packing changed");
static_assert(sizeof(ConsumerResultMessage) == 56,
              "result protocol packing changed");

} // namespace asb_d3d12_share_probe

#endif
