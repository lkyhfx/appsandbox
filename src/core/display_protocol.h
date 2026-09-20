/* SPDX-License-Identifier: MIT
 * Shared AppSandbox raw display wire protocol.
 *
 * All fields are little-endian. The packed structs are intentionally shared
 * by the Linux Guest capture daemon and the Windows Host receivers.
 */
#ifndef ASB_DISPLAY_PROTOCOL_H
#define ASB_DISPLAY_PROTOCOL_H

#include <stdint.h>

#define ASB_DISPLAY_RAW_MAGIC        0x52465341u /* ASFR */
#define ASB_DISPLAY_CURSOR_MAGIC     0x52435341u /* ASCR */
#define ASB_DISPLAY_MAX_DIRTY_RECTS 64u
#define ASB_DISPLAY_TILE_SIZE       64u

/* The virtual DRM driver currently exposes only the active primary FB. It
 * does not publish FB_DAMAGE_CLIPS or a trustworthy atomic damage sequence,
 * so the guest daemon must use its explicit tile-compare fallback. Keep this
 * flag visible in logs/tests rather than implying that damage metadata exists. */
#define ASB_DISPLAY_DRM_DAMAGE_AVAILABLE 0u

/* The DRM driver supports 7680x4320. This is deliberately separate from
 * negotiation limits used by other display protocols. */
#define ASB_DISPLAY_RAW_MAX_WIDTH   7680u
#define ASB_DISPLAY_RAW_MAX_HEIGHT  4320u
#define ASB_DISPLAY_MAX_FRAME_DATA_SIZE \
    (ASB_DISPLAY_RAW_MAX_WIDTH * ASB_DISPLAY_RAW_MAX_HEIGHT * 4u)
#define ASB_DISPLAY_DEFAULT_WIDTH   1920u
#define ASB_DISPLAY_DEFAULT_HEIGHT  1080u

#pragma pack(push, 1)
typedef struct AsbDisplayFrameHeader {
    uint32_t magic;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint64_t frame_seq;
    uint32_t dirty_rect_count;
} AsbDisplayFrameHeader;

typedef struct AsbDisplayRect {
    int32_t left;
    int32_t top;
    int32_t right;
    int32_t bottom;
} AsbDisplayRect;
#pragma pack(pop)

#if defined(__cplusplus)
static_assert(sizeof(AsbDisplayFrameHeader) == 28, "raw frame header ABI");
static_assert(sizeof(AsbDisplayRect) == 16, "raw rect ABI");
#elif defined(_WIN32)
static_assert(sizeof(AsbDisplayFrameHeader) == 28, "raw frame header ABI");
static_assert(sizeof(AsbDisplayRect) == 16, "raw rect ABI");
#else
_Static_assert(sizeof(AsbDisplayFrameHeader) == 28, "raw frame header ABI");
_Static_assert(sizeof(AsbDisplayRect) == 16, "raw rect ABI");
#endif

#endif /* ASB_DISPLAY_PROTOCOL_H */
