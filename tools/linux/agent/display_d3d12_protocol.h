/* SPDX-License-Identifier: MIT
 * Private SOCK_SEQPACKET protocol between Mesa's publisher and the D3D12
 * encoder helper. Descriptor order for RESOURCE_SET is resource, ready for
 * each slot. RESOURCE_REPLY carries one done eventfd for each slot.
 */
#ifndef ASB_DISPLAY_D3D12_PROTOCOL_H
#define ASB_DISPLAY_D3D12_PROTOCOL_H

#include <stdint.h>

#define ASB_D3D12_PROTOCOL_MAGIC   0x50334441u /* AD3P */
#define ASB_D3D12_PROTOCOL_VERSION 1u
#define ASB_D3D12_SLOT_COUNT       3u
#define ASB_D3D12_MAX_SLOTS        3u

#define ASB_D3D12_MSG_RESOURCE_SET   1u
#define ASB_D3D12_MSG_RESOURCE_REPLY 2u
#define ASB_D3D12_MSG_FRAME          3u

#define ASB_D3D12_RESOURCE_GPU_COPY (1u << 0)

#pragma pack(push, 1)
typedef struct AsbD3D12ResourceSet {
    uint32_t magic;
    uint16_t version;
    uint16_t header_size;
    uint32_t type;
    uint32_t reserved0;
    uint64_t generation;
    uint32_t width;
    uint32_t height;
    uint32_t dxgi_format;
    uint32_t slot_count;
    uint32_t flags;
    uint32_t reserved;
} AsbD3D12ResourceSet;

typedef struct AsbD3D12ResourceReply {
    uint32_t magic;
    uint16_t version;
    uint16_t header_size;
    uint32_t type;
    uint32_t status;
    uint64_t generation;
    uint32_t slot_count;
    uint32_t reserved;
} AsbD3D12ResourceReply;

typedef struct AsbD3D12Frame {
    uint32_t magic;
    uint16_t version;
    uint16_t header_size;
    uint32_t type;
    uint32_t reserved0;
    uint64_t generation;
    uint64_t frame_seq;
    uint64_t capture_time_ns;
    uint32_t slot;
    uint32_t flags;
} AsbD3D12Frame;
#pragma pack(pop)

#if defined(__cplusplus)
static_assert(sizeof(AsbD3D12ResourceSet) == 48, "resource set ABI");
static_assert(sizeof(AsbD3D12ResourceReply) == 32, "resource reply ABI");
static_assert(sizeof(AsbD3D12Frame) == 48, "frame ABI");
#else
_Static_assert(sizeof(AsbD3D12ResourceSet) == 48, "resource set ABI");
_Static_assert(sizeof(AsbD3D12ResourceReply) == 32, "resource reply ABI");
_Static_assert(sizeof(AsbD3D12Frame) == 48, "frame ABI");
#endif

#endif
