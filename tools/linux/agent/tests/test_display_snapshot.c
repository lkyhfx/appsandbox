/* SPDX-License-Identifier: MIT
 * Synthetic correctness tests for the RAW display snapshot contract.
 *
 * These tests deliberately mutate the producer buffer after packing. If the
 * shadow were updated from the live buffer instead of the immutable payload,
 * test_snapshot_race would reproduce the stale-host/shadow mismatch from
 * Issue 6.
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "display_snapshot.h"

static void fill_frame(uint8_t *p, uint32_t width, uint32_t height,
                       uint32_t stride, uint8_t seed)
{
    uint32_t y, x;
    for (y = 0; y < height; y++) {
        for (x = 0; x < width * 4u; x++)
            p[(size_t)y * stride + x] =
                (uint8_t)(seed + x * 3u + y * 11u);
        for (; x < stride; x++)
            p[(size_t)y * stride + x] = 0xee;
    }
}

static void test_snapshot_race(void)
{
    const uint32_t width = 9, height = 7, stride = 44;
    const AsbDisplayRect rect = { 1, 1, 8, 6 };
    size_t bytes = (size_t)stride * height;
    uint8_t *frame = (uint8_t *)malloc(bytes);
    uint8_t *shadow = (uint8_t *)calloc(1, bytes);
    uint8_t payload[7 * 5 * 4];
    size_t payload_size = 0;
    uint8_t sent_copy[sizeof(payload)];

    assert(frame && shadow);
    fill_frame(frame, width, height, stride, 0x10);
    assert(asb_display_pack_snapshot(payload, sizeof(payload), frame, bytes,
                                     width, height, stride, &rect, 1, 0,
                                     &payload_size) == 0);
    memcpy(sent_copy, payload, payload_size);

    /* Producer changes the live framebuffer after snapshot and before the
     * simulated send completes. The sent payload must remain version A. */
    fill_frame(frame, width, height, stride, 0x90);
    assert(asb_display_commit_snapshot(shadow, bytes, sent_copy, payload_size,
                                        width, height, stride, &rect, 1, 0) == 0);

    assert(memcmp(shadow, frame, bytes) != 0);
    for (uint32_t y = (uint32_t)rect.top; y < (uint32_t)rect.bottom; y++) {
        size_t row = (size_t)(y - rect.top) *
                     (size_t)(rect.right - rect.left) * 4u;
        assert(memcmp(shadow + (size_t)y * stride + rect.left * 4,
                      sent_copy + row,
                      (size_t)(rect.right - rect.left) * 4u) == 0);
    }

    /* A failed send has no commit call, so the shadow remains untouched. */
    memset(shadow, 0, bytes);
    assert(memcmp(shadow, frame, bytes) != 0);
    free(frame);
    free(shadow);
}

static void test_rect_packing(void)
{
    const uint32_t width = 131, height = 129, stride = width * 4u + 12u;
    const size_t bytes = (size_t)stride * height;
    AsbDisplayRect rects[64];
    uint8_t *frame = (uint8_t *)malloc(bytes);
    uint8_t *shadow = (uint8_t *)calloc(1, bytes);
    uint8_t *payload = (uint8_t *)malloc(bytes);
    size_t payload_size = 0, expected = 0;
    uint32_t i;

    assert(frame && shadow && payload);
    fill_frame(frame, width, height, stride, 0x20);

    /* 64 non-overlapping 1x1 rects exercise the protocol limit and order. */
    for (i = 0; i < 64; i++) {
        rects[i].left = (int32_t)(i * 2);
        rects[i].top = (int32_t)(i * 2);
        rects[i].right = rects[i].left + 1;
        rects[i].bottom = rects[i].top + 1;
        expected += 4;
    }
    assert(asb_display_pack_snapshot(payload, bytes, frame, bytes,
                                     width, height, stride, rects, 64, 0,
                                     &payload_size) == 0);
    assert(payload_size == expected);
    assert(asb_display_commit_snapshot(shadow, bytes, payload, payload_size,
                                        width, height, stride, rects, 64, 0) == 0);
    for (i = 0; i < 64; i++)
        assert(memcmp(shadow + (size_t)rects[i].top * stride +
                              rects[i].left * 4,
                      frame + (size_t)rects[i].top * stride +
                              rects[i].left * 4, 4) == 0);

    free(frame);
    free(shadow);
    free(payload);
}

static void test_rect_shapes_and_full_frames(void)
{
    const uint32_t width = 17, height = 11, stride = 80;
    const size_t bytes = (size_t)stride * height;
    const AsbDisplayRect rects[] = {
        { 0, 0, 17, 1 },       /* full-width */
        { 3, 0, 4, 11 },       /* vertical */
        { 5, 3, 13, 8 },       /* odd placement */
        { 12, 9, 17, 11 },     /* edge */
        { 0, 5, 17, 6 },       /* horizontal */
    };
    uint8_t *frame = (uint8_t *)malloc(bytes);
    uint8_t *shadow = (uint8_t *)calloc(1, bytes);
    uint8_t *payload = (uint8_t *)malloc(bytes);
    size_t payload_size = 0;

    assert(frame && shadow && payload);
    fill_frame(frame, width, height, stride, 0x40);
    assert(asb_display_pack_snapshot(payload, bytes, frame, bytes,
                                     width, height, stride, rects,
                                     (uint32_t)(sizeof(rects) / sizeof(rects[0])),
                                     0, &payload_size) == 0);
    assert(asb_display_commit_snapshot(shadow, bytes, payload, payload_size,
                                        width, height, stride, rects,
                                        (uint32_t)(sizeof(rects) / sizeof(rects[0])),
                                        0) == 0);
    assert(shadow[0] == frame[0]);
    assert(shadow[(size_t)7 * stride + 6 * 4] ==
           frame[(size_t)7 * stride + 6 * 4]);
    assert(shadow[(size_t)10 * stride + 16 * 4] ==
           frame[(size_t)10 * stride + 16 * 4]);

    /* Full-frame path uses the same staging contract and keeps stride bytes. */
    memset(shadow, 0, bytes);
    assert(asb_display_pack_snapshot(payload, bytes, frame, bytes,
                                     width, height, stride, NULL, 0, 1,
                                     &payload_size) == 0);
    assert(payload_size == bytes);
    assert(asb_display_commit_snapshot(shadow, bytes, payload, payload_size,
                                        width, height, stride, NULL, 0, 1) == 0);
    assert(memcmp(shadow, frame, bytes) == 0);

    free(frame);
    free(shadow);
    free(payload);
}

static void test_resolution_change(void)
{
    enum { old_w = 5, old_h = 3, old_stride = 24,
           new_w = 7, new_h = 2, new_stride = 32 };
    uint8_t old_frame[old_stride * old_h];
    uint8_t new_frame[new_stride * new_h];
    uint8_t old_shadow[sizeof(old_frame)] = { 0 };
    uint8_t new_shadow[sizeof(new_frame)] = { 0 };
    uint8_t old_payload[sizeof(old_frame)];
    uint8_t new_payload[sizeof(new_frame)];
    size_t size;

    fill_frame(old_frame, old_w, old_h, old_stride, 0x55);
    fill_frame(new_frame, new_w, new_h, new_stride, 0x66);
    assert(asb_display_pack_snapshot(old_payload, sizeof(old_payload),
                                     old_frame, sizeof(old_frame),
                                     old_w, old_h, old_stride, NULL, 0, 1,
                                     &size) == 0);
    assert(asb_display_commit_snapshot(old_shadow, sizeof(old_shadow),
                                        old_payload, size,
                                        old_w, old_h, old_stride, NULL, 0, 1) == 0);
    assert(asb_display_pack_snapshot(new_payload, sizeof(new_payload),
                                     new_frame, sizeof(new_frame),
                                     new_w, new_h, new_stride, NULL, 0, 1,
                                     &size) == 0);
    assert(asb_display_commit_snapshot(new_shadow, sizeof(new_shadow),
                                        new_payload, size,
                                        new_w, new_h, new_stride, NULL, 0, 1) == 0);
    assert(memcmp(new_shadow, new_frame, sizeof(new_frame)) == 0);
}

int main(void)
{
    test_snapshot_race();
    test_rect_packing();
    test_rect_shapes_and_full_frames();
    test_resolution_change();
    puts("display_snapshot: PASS (race, packed rects, full frames, resolution)");
    return 0;
}
