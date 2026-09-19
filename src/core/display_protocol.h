/* SPDX-License-Identifier: MIT
 * Versioned AppSandbox display wire protocol shared by host and guest.
 * All fields are little-endian; Windows and the supported Linux guests are
 * little-endian today. Keep the structs packed and append-only.
 */
#ifndef ASB_DISPLAY_PROTOCOL_H
#define ASB_DISPLAY_PROTOCOL_H

#include <stdint.h>

#define ASB_DISPLAY_PROTOCOL_VERSION 2u

#define ASB_DISPLAY_HOST_HELLO_MAGIC 0x48445341u /* ASDH */
#define ASB_DISPLAY_RAW_MAGIC        0x52465341u /* ASFR */
#define ASB_DISPLAY_CURSOR_MAGIC     0x52435341u /* ASCR */
#define ASB_DISPLAY_VIDEO_CONFIG_MAGIC 0x43565341u /* ASVC */
#define ASB_DISPLAY_VIDEO_FRAME_MAGIC  0x45565341u /* ASVE */

#define ASB_DISPLAY_CAP_RAW_ASFR              (1u << 0)
/* Bit 1 was the original generic HEVC capability. Preserve it as 420. */
#define ASB_DISPLAY_CAP_HEVC420_D3D11_HW_DECODE (1u << 1)
#define ASB_DISPLAY_CAP_HEVC_D3D11_HW_DECODE    ASB_DISPLAY_CAP_HEVC420_D3D11_HW_DECODE
#define ASB_DISPLAY_CAP_HEVC444_D3D11_HW_DECODE (1u << 2)

#define ASB_DISPLAY_CODEC_HEVC 1u
#define ASB_DISPLAY_VIDEO_FLAG_IDR           (1u << 0)
#define ASB_DISPLAY_VIDEO_FLAG_DISCONTINUITY (1u << 1)

/* Explicit data-plane profile flags carried in AsbEncodedVideoConfig.flags.
   Keeping these in the existing field preserves the v2 packed ABI. */
#define ASB_DISPLAY_VIDEO_CONFIG_HEVC420     (1u << 8)
#define ASB_DISPLAY_VIDEO_CONFIG_HEVC444     (1u << 9)

#define ASB_DISPLAY_MAX_WIDTH       16384u
#define ASB_DISPLAY_MAX_HEIGHT      16384u
#define ASB_DISPLAY_MAX_EXTRADATA   (256u * 1024u)
#define ASB_DISPLAY_MAX_VIDEO_FRAME (16u * 1024u * 1024u)

#pragma pack(push, 1)
typedef struct AsbDisplayHostHello {
    uint32_t magic;
    uint16_t version;
    uint16_t header_size;
    uint32_t capabilities;
    uint32_t max_width;
    uint32_t max_height;
} AsbDisplayHostHello;

typedef struct AsbEncodedVideoConfig {
    uint32_t magic;
    uint16_t version;
    uint16_t header_size;
    uint64_t generation;
    uint32_t codec;
    uint32_t width;
    uint32_t height;
    uint32_t fps_num;
    uint32_t fps_den;
    uint32_t flags;
    uint32_t extradata_size;
} AsbEncodedVideoConfig;

typedef struct AsbEncodedVideoFrame {
    uint32_t magic;
    uint16_t version;
    uint16_t header_size;
    uint64_t generation;
    uint64_t frame_seq;
    uint64_t capture_time_ns;
    uint32_t flags;
    uint32_t payload_size;
} AsbEncodedVideoFrame;
#pragma pack(pop)

#if defined(__cplusplus)
static_assert(sizeof(AsbDisplayHostHello) == 20, "display hello ABI");
static_assert(sizeof(AsbEncodedVideoConfig) == 44, "video config ABI");
static_assert(sizeof(AsbEncodedVideoFrame) == 40, "video frame ABI");
#elif defined(_WIN32)
static_assert(sizeof(AsbDisplayHostHello) == 20, "display hello ABI");
static_assert(sizeof(AsbEncodedVideoConfig) == 44, "video config ABI");
static_assert(sizeof(AsbEncodedVideoFrame) == 40, "video frame ABI");
#else
_Static_assert(sizeof(AsbDisplayHostHello) == 20, "display hello ABI");
_Static_assert(sizeof(AsbEncodedVideoConfig) == 44, "video config ABI");
_Static_assert(sizeof(AsbEncodedVideoFrame) == 40, "video frame ABI");
#endif

#endif
