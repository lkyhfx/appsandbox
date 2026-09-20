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

#include "display_fb_state.h"
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

typedef struct SyntheticPageFlipState {
    uint32_t fb_id;
    AsbDisplayFbLayout layout;
    int have_layout;
    int shadow_valid;
    int force_full;
    uint32_t fb_id_changes;
    uint32_t layout_changes;
    uint32_t full_frames;
    uint32_t dirty_frames;
} SyntheticPageFlipState;

static int synthetic_acquire_fb(SyntheticPageFlipState *state,
                                uint32_t fb_id,
                                const AsbDisplayFbLayout *layout)
{
    int same_layout = state->have_layout &&
                      asb_display_fb_layout_equal(&state->layout, layout);
    int full = state->force_full;

    if (state->have_layout && state->fb_id != fb_id)
        state->fb_id_changes++;
    if (!state->have_layout || !same_layout) {
        if (state->have_layout) state->layout_changes++;
        state->shadow_valid = 0;
        full = 1;
    } else if (!state->shadow_valid) {
        full = 1;
    }

    state->fb_id = fb_id;
    state->layout = *layout;
    state->have_layout = 1;
    state->force_full = full;
    return full;
}

static void synthetic_commit(SyntheticPageFlipState *state, int full)
{
    if (full) state->full_frames++;
    else      state->dirty_frames++;
    state->shadow_valid = 1;
    state->force_full = 0;
}

static void test_page_flip_semantics(void)
{
    const AsbDisplayFbLayout layout = { 8, 8, 32, 1 };
    const AsbDisplayFbLayout larger_layout = { 10, 8, 40, 1 };
    const AsbDisplayRect first_rect = { 0, 0, 2, 2 };
    const AsbDisplayRect second_rect = { 4, 4, 6, 6 };
    uint8_t frame[8 * 32], next[8 * 32], shadow[8 * 32];
    uint8_t payload[10 * 8 * 4];
    size_t payload_size;
    SyntheticPageFlipState state = { 0 };
    uint32_t i;

    fill_frame(frame, layout.width, layout.height, layout.stride, 0x10);
    assert(synthetic_acquire_fb(&state, 1, &layout) == 1);
    assert(asb_display_pack_snapshot(payload, sizeof(payload), frame,
                                     sizeof(frame), layout.width, layout.height,
                                     layout.stride, NULL, 0, 1,
                                     &payload_size) == 0);
    assert(asb_display_commit_snapshot(shadow, sizeof(shadow), payload,
                                        payload_size, layout.width,
                                        layout.height, layout.stride, NULL, 0,
                                        1) == 0);
    synthetic_commit(&state, 1);

    /* B and C are different backing objects with the same layout. Only their
     * changed tiles are dirty; FB_ID churn must not force another full frame. */
    memcpy(next, frame, sizeof(next));
    memset(next, 0xa1, 2 * 4);
    assert(synthetic_acquire_fb(&state, 2, &layout) == 0);
    assert(asb_display_pack_snapshot(payload, sizeof(payload), next,
                                     sizeof(next), layout.width, layout.height,
                                     layout.stride, &first_rect, 1, 0,
                                     &payload_size) == 0);
    assert(asb_display_commit_snapshot(shadow, sizeof(shadow), payload,
                                        payload_size, layout.width,
                                        layout.height, layout.stride,
                                        &first_rect, 1, 0) == 0);
    memcpy(frame, next, sizeof(frame));
    synthetic_commit(&state, 0);

    memset(next + 4 * layout.stride + 4 * 4, 0xb2, 2 * 4);
    assert(synthetic_acquire_fb(&state, 3, &layout) == 0);
    assert(asb_display_pack_snapshot(payload, sizeof(payload), next,
                                     sizeof(next), layout.width, layout.height,
                                     layout.stride, &second_rect, 1, 0,
                                     &payload_size) == 0);
    assert(asb_display_commit_snapshot(shadow, sizeof(shadow), payload,
                                        payload_size, layout.width,
                                        layout.height, layout.stride,
                                        &second_rect, 1, 0) == 0);
    memcpy(frame, next, sizeof(frame));
    synthetic_commit(&state, 0);

    /* Exercise the requested A -> B -> C churn count at identical layout. */
    for (i = 0; i < 3600; i++) {
        AsbDisplayRect rect = {
            (int32_t)((i & 1) ? 4 : 0),
            (int32_t)((i & 1) ? 4 : 0),
            (int32_t)((i & 1) ? 6 : 2),
            (int32_t)((i & 1) ? 6 : 2),
        };
        memcpy(next, frame, sizeof(next));
        next[(size_t)rect.top * layout.stride + (size_t)rect.left * 4] ^= 1;
        assert(synthetic_acquire_fb(&state, 4 + (i % 3), &layout) == 0);
        assert(asb_display_pack_snapshot(payload, sizeof(payload), next,
                                         sizeof(next), layout.width,
                                         layout.height, layout.stride, &rect, 1,
                                         0, &payload_size) == 0);
        assert(asb_display_commit_snapshot(shadow, sizeof(shadow), payload,
                                            payload_size, layout.width,
                                            layout.height, layout.stride, &rect,
                                            1, 0) == 0);
        memcpy(frame, next, sizeof(frame));
        synthetic_commit(&state, 0);
    }

    assert(state.fb_id_changes == 3602);
    assert(state.layout_changes == 0);
    assert(state.full_frames == 1);
    assert(state.dirty_frames == 3602);
    assert(memcmp(shadow, frame, sizeof(shadow)) == 0);

    /* A layout change invalidates the shadow and forces exactly one next full. */
    {
        uint8_t larger_frame[8 * 40], larger_shadow[8 * 40];
        fill_frame(larger_frame, larger_layout.width, larger_layout.height,
                   larger_layout.stride, 0xc0);
        assert(synthetic_acquire_fb(&state, 4000, &larger_layout) == 1);
        assert(asb_display_pack_snapshot(payload, sizeof(payload),
                                         larger_frame, sizeof(larger_frame),
                                         larger_layout.width, larger_layout.height,
                                         larger_layout.stride, NULL, 0, 1,
                                         &payload_size) == 0);
        assert(asb_display_commit_snapshot(larger_shadow, sizeof(larger_shadow),
                                            payload, payload_size,
                                            larger_layout.width,
                                            larger_layout.height,
                                            larger_layout.stride, NULL, 0, 1) == 0);
        synthetic_commit(&state, 1);
        assert(state.layout_changes == 1);
        assert(state.full_frames == 2);
        (void)larger_shadow;
    }
}

int main(void)
{
    test_snapshot_race();
    test_rect_packing();
    test_rect_shapes_and_full_frames();
    test_resolution_change();
    test_page_flip_semantics();
    puts("display_snapshot: PASS (race, packing, page-flip, layout, churn)");
    return 0;
}
