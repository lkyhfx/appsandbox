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
#define ASB_DISPLAY_CONTROL_MAGIC    0x43445341u /* ASDC */
#define ASB_DISPLAY_CONTROL_VERSION  1u
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
#define ASB_DISPLAY_DEFAULT_WIDTH   1280u
#define ASB_DISPLAY_DEFAULT_HEIGHT  720u
#define ASB_DISPLAY_CONTROL_MIN_WIDTH  320u
#define ASB_DISPLAY_CONTROL_MIN_HEIGHT 180u
#define ASB_DISPLAY_CONTROL_MAX_WIDTH  ASB_DISPLAY_RAW_MAX_WIDTH
#define ASB_DISPLAY_CONTROL_MAX_HEIGHT ASB_DISPLAY_RAW_MAX_HEIGHT
#define ASB_DISPLAY_CONTROL_DEFAULT_REFRESH 60u

/* Issue #9 deliberately exposes a small, stable mode list. Keep this table
 * in the shared protocol header so the host UI, guest helper, and tests all
 * validate the same values. */
#define ASB_DISPLAY_PRESET_COUNT 4u

static inline int asb_display_is_preset(uint32_t width, uint32_t height)
{
    return (width == 1280u && height == 720u) ||
           (width == 1920u && height == 1080u) ||
           (width == 2560u && height == 1440u) ||
           (width == 3840u && height == 2160u);
}
/* Host-side state-machine policy shared with deterministic unit tests. */
#define ASB_DISPLAY_RESIZE_ACK_TIMEOUT_MS 1250u
#define ASB_DISPLAY_RESIZE_COMPLETION_TIMEOUT_MS 2500u
#define ASB_DISPLAY_RESIZE_RETRY_LIMIT 2u
#define ASB_DISPLAY_RUNTIME_REQUEST_TIMEOUT_MS 15000u

enum {
    ASB_DISPLAY_CONTROL_HELLO = 1,
    ASB_DISPLAY_CONTROL_RESIZE_REQUEST = 2,
    ASB_DISPLAY_CONTROL_RESIZE_ACK = 3,
};

enum {
    ASB_DISPLAY_CONTROL_FLAG_DYNAMIC_RESIZE = 1u << 0,
};

enum {
    ASB_DISPLAY_CONTROL_STATUS_ACCEPTED = 0,
    ASB_DISPLAY_CONTROL_STATUS_BAD_REQUEST = 1,
    ASB_DISPLAY_CONTROL_STATUS_UNSUPPORTED = 2,
    ASB_DISPLAY_CONTROL_STATUS_MODE_FAILED = 3,
    /* The guest already has one asynchronous Mutter modeset in flight. */
    ASB_DISPLAY_CONTROL_STATUS_BUSY = 4,
};

/* Resolution normalization is deliberately shared by the host and guest:
 * width is even on an 8-pixel boundary and height on a 2-pixel boundary. */
static inline void asb_display_normalize_resolution(uint32_t *width,
                                                    uint32_t *height)
{
    uint32_t w = *width;
    uint32_t h = *height;

    if (w < ASB_DISPLAY_CONTROL_MIN_WIDTH) w = ASB_DISPLAY_CONTROL_MIN_WIDTH;
    if (h < ASB_DISPLAY_CONTROL_MIN_HEIGHT) h = ASB_DISPLAY_CONTROL_MIN_HEIGHT;
    if (w > ASB_DISPLAY_CONTROL_MAX_WIDTH) w = ASB_DISPLAY_CONTROL_MAX_WIDTH;
    if (h > ASB_DISPLAY_CONTROL_MAX_HEIGHT) h = ASB_DISPLAY_CONTROL_MAX_HEIGHT;
    w = (w + 7u) & ~7u;
    h = (h + 1u) & ~1u;
    if (w > ASB_DISPLAY_CONTROL_MAX_WIDTH) w = ASB_DISPLAY_CONTROL_MAX_WIDTH;
    if (h > ASB_DISPLAY_CONTROL_MAX_HEIGHT) h = ASB_DISPLAY_CONTROL_MAX_HEIGHT;
    *width = w;
    *height = h;
}

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

/* ASDC is a fixed-size, little-endian, bidirectional control message. A HELLO
 * is sent by the guest after connection; resize requests travel host->guest
 * and ACKs travel guest->host. ACK means the request was accepted by the
 * guest, not that the compositor has completed its modeset. ASFR dimensions
 * remain the completion signal. */
typedef struct AsbDisplayControl {
    uint32_t magic;
    uint16_t version;
    uint16_t type;
    uint32_t request_id;
    uint32_t width;
    uint32_t height;
    uint32_t refresh_hz;
    uint32_t status_or_flags;
} AsbDisplayControl;
#pragma pack(pop)

#if defined(__cplusplus)
static_assert(sizeof(AsbDisplayFrameHeader) == 28, "raw frame header ABI");
static_assert(sizeof(AsbDisplayRect) == 16, "raw rect ABI");
static_assert(sizeof(AsbDisplayControl) == 28, "display control ABI");
#elif defined(_WIN32)
static_assert(sizeof(AsbDisplayFrameHeader) == 28, "raw frame header ABI");
static_assert(sizeof(AsbDisplayRect) == 16, "raw rect ABI");
static_assert(sizeof(AsbDisplayControl) == 28, "display control ABI");
#else
_Static_assert(sizeof(AsbDisplayFrameHeader) == 28, "raw frame header ABI");
_Static_assert(sizeof(AsbDisplayRect) == 16, "raw rect ABI");
_Static_assert(sizeof(AsbDisplayControl) == 28, "display control ABI");
#endif

#endif /* ASB_DISPLAY_PROTOCOL_H */
