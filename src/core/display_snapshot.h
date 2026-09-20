/* SPDX-License-Identifier: MIT
 * Small, platform-independent helpers for the RAW display snapshot path.
 *
 * The capture daemon uses these helpers to make one immutable payload for a
 * frame.  The exact same bytes are sent to the host and copied into the
 * guest shadow only after the send succeeds.
 */
#ifndef ASB_DISPLAY_SNAPSHOT_H
#define ASB_DISPLAY_SNAPSHOT_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "display_protocol.h"

static inline int asb_display_snapshot_layout(uint32_t width, uint32_t height,
                                              uint32_t stride,
                                              uint64_t *bytes_out)
{
    uint64_t bytes;

    if (!width || !height || stride < (uint64_t)width * 4u)
        return -1;
    bytes = (uint64_t)stride * height;
    if (bytes > ASB_DISPLAY_MAX_FRAME_DATA_SIZE || bytes > SIZE_MAX)
        return -1;
    if (bytes_out) *bytes_out = bytes;
    return 0;
}

static inline int asb_display_snapshot_rect_valid(const AsbDisplayRect *r,
                                                  uint32_t width,
                                                  uint32_t height)
{
    return r && r->left >= 0 && r->top >= 0 &&
           r->right > r->left && r->bottom > r->top &&
           (uint32_t)r->right <= width && (uint32_t)r->bottom <= height;
}

/* Pack a full frame or the rows of dirty rectangles into one contiguous
 * payload.  No pointer into the live framebuffer escapes this function. */
static inline int asb_display_pack_snapshot(uint8_t *dst, size_t dst_capacity,
                                            const uint8_t *src,
                                            size_t src_size,
                                            uint32_t width, uint32_t height,
                                            uint32_t stride,
                                            const AsbDisplayRect *rects,
                                            uint32_t rect_count, int full,
                                            size_t *payload_size_out)
{
    uint64_t frame_bytes;
    size_t offset = 0;
    uint32_t i;

    if (!dst || !src || !payload_size_out ||
        asb_display_snapshot_layout(width, height, stride, &frame_bytes) < 0 ||
        frame_bytes > src_size)
        return -1;

    if (full) {
        if (frame_bytes > dst_capacity) return -1;
        memcpy(dst, src, (size_t)frame_bytes);
        *payload_size_out = (size_t)frame_bytes;
        return 0;
    }

    for (i = 0; i < rect_count; i++) {
        const AsbDisplayRect *r = &rects[i];
        uint32_t y;
        size_t row_bytes;
        uint64_t rect_bytes;

        if (!asb_display_snapshot_rect_valid(r, width, height)) return -1;
        row_bytes = (size_t)(uint32_t)(r->right - r->left) * 4u;
        rect_bytes = (uint64_t)row_bytes * (uint32_t)(r->bottom - r->top);
        if (rect_bytes > SIZE_MAX || (size_t)rect_bytes > dst_capacity - offset)
            return -1;

        for (y = (uint32_t)r->top; y < (uint32_t)r->bottom; y++) {
            uint64_t src_offset = (uint64_t)y * stride +
                                  (uint32_t)r->left * 4u;
            if (src_offset > src_size || row_bytes > src_size - (size_t)src_offset)
                return -1;
            memcpy(dst + offset, src + (size_t)src_offset, row_bytes);
            offset += row_bytes;
        }
    }

    *payload_size_out = offset;
    return 0;
}

/* Commit precisely the payload that was sent.  The live framebuffer is never
 * consulted here, which is the key snapshot/shadow consistency guarantee. */
static inline int asb_display_commit_snapshot(uint8_t *shadow,
                                              size_t shadow_size,
                                              const uint8_t *payload,
                                              size_t payload_size,
                                              uint32_t width, uint32_t height,
                                              uint32_t stride,
                                              const AsbDisplayRect *rects,
                                              uint32_t rect_count, int full)
{
    uint64_t frame_bytes;
    size_t offset = 0;
    uint32_t i;

    if (!shadow || !payload ||
        asb_display_snapshot_layout(width, height, stride, &frame_bytes) < 0 ||
        frame_bytes > shadow_size)
        return -1;

    if (full) {
        if (payload_size != (size_t)frame_bytes) return -1;
        memcpy(shadow, payload, payload_size);
        return 0;
    }

    for (i = 0; i < rect_count; i++) {
        const AsbDisplayRect *r = &rects[i];
        uint32_t y;
        size_t row_bytes;
        uint64_t rect_bytes;

        if (!asb_display_snapshot_rect_valid(r, width, height)) return -1;
        row_bytes = (size_t)(uint32_t)(r->right - r->left) * 4u;
        rect_bytes = (uint64_t)row_bytes * (uint32_t)(r->bottom - r->top);
        if (rect_bytes > SIZE_MAX || (size_t)rect_bytes > payload_size - offset)
            return -1;

        for (y = (uint32_t)r->top; y < (uint32_t)r->bottom; y++) {
            uint64_t dst_offset = (uint64_t)y * stride +
                                  (uint32_t)r->left * 4u;
            if (dst_offset > shadow_size || row_bytes > shadow_size - (size_t)dst_offset)
                return -1;
            memcpy(shadow + (size_t)dst_offset, payload + offset, row_bytes);
            offset += row_bytes;
        }
    }

    return offset == payload_size ? 0 : -1;
}

#endif /* ASB_DISPLAY_SNAPSHOT_H */
