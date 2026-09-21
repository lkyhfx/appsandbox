/* Build with:
 *   gcc -std=c11 -Wall -Wextra -I src tests/display_resize_state_test.c \
 *       -o display_resize_state_test
 *
 * This is a platform-independent contract test for the host resize state
 * machine. The Windows implementation uses the same generation, ACK,
 * normalized-target, timeout, and retry rules exercised here.
 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>

#include "../src/core/display_protocol.h"

typedef struct ResizeModel {
    uint32_t desired_width;
    uint32_t desired_height;
    uint32_t actual_width;
    uint32_t actual_height;
    bool actual_valid;
    uint64_t connection_generation;
    uint64_t pending_generation;
    uint32_t pending_id;
    uint32_t pending_width;
    uint32_t pending_height;
    uint32_t retry_count;
    uint64_t deadline;
    bool queued;
} ResizeModel;

static void model_disconnect(ResizeModel *s)
{
    s->actual_valid = false;
    s->pending_generation = 0;
    s->pending_id = 0;
    s->pending_width = 0;
    s->pending_height = 0;
    s->retry_count = 0;
    s->deadline = 0;
    s->queued = false;
}

static void model_schedule(ResizeModel *s, uint32_t width, uint32_t height)
{
    uint32_t old_width = s->desired_width;
    uint32_t old_height = s->desired_height;
    asb_display_normalize_resolution(&width, &height);
    if (old_width != width || old_height != height)
        s->retry_count = 0;
    s->desired_width = width;
    s->desired_height = height;
    s->queued = !s->actual_valid || s->actual_width != width ||
                s->actual_height != height;
}

static bool model_ack(ResizeModel *s, uint32_t request_id,
                      uint32_t status, uint32_t width, uint32_t height,
                      uint64_t now)
{
    if (request_id != s->pending_id)
        return false; /* stale ACK does not mutate current request */

    if (status == ASB_DISPLAY_CONTROL_STATUS_ACCEPTED) {
        s->pending_width = width;
        s->pending_height = height;
        s->deadline = now + ASB_DISPLAY_RESIZE_COMPLETION_TIMEOUT_MS;
        return true;
    }

    s->pending_id = 0;
    s->pending_generation = 0;
    s->pending_width = 0;
    s->pending_height = 0;
    s->deadline = 0;
    if (status == ASB_DISPLAY_CONTROL_STATUS_MODE_FAILED &&
        s->retry_count < ASB_DISPLAY_RESIZE_RETRY_LIMIT) {
        s->retry_count++;
        s->queued = true;
    } else {
        s->retry_count = 0;
        s->queued = false;
    }
    return false;
}

static bool model_frame(ResizeModel *s, uint32_t width, uint32_t height,
                        uint64_t generation)
{
    s->actual_width = width;
    s->actual_height = height;
    s->actual_valid = true;
    s->connection_generation = generation;
    if (s->pending_id && s->pending_generation == generation &&
        s->pending_width == width && s->pending_height == height) {
        s->pending_id = 0;
        s->pending_generation = 0;
        s->pending_width = 0;
        s->pending_height = 0;
        s->deadline = 0;
        s->retry_count = 0;
        return true;
    }
    return false;
}

static bool model_timeout(ResizeModel *s, uint64_t now)
{
    if (!s->pending_id || !s->deadline || now < s->deadline)
        return false;
    s->pending_id = 0;
    s->pending_generation = 0;
    s->pending_width = 0;
    s->pending_height = 0;
    s->deadline = 0;
    if (s->retry_count < ASB_DISPLAY_RESIZE_RETRY_LIMIT) {
        s->retry_count++;
        s->queued = true;
    } else {
        s->retry_count = 0;
        s->queued = false;
    }
    return true;
}

static void begin_request(ResizeModel *s, uint32_t request_id,
                          uint64_t generation, uint32_t width,
                          uint32_t height)
{
    s->queued = false;
    s->pending_id = request_id;
    s->pending_generation = generation;
    s->pending_width = width;
    s->pending_height = height;
    s->deadline = 0;
}

int main(void)
{
    ResizeModel s = {0};

    /* Reconnect: stale actual must not suppress the new sync request. */
    s.desired_width = 1920;
    s.desired_height = 1080;
    s.actual_width = 1920;
    s.actual_height = 1080;
    s.actual_valid = true;
    model_disconnect(&s);
    model_schedule(&s, 1920, 1080);
    assert(s.queued);

    /* MODE_FAILED clears pending and permits only bounded retries. */
    s.connection_generation = 1;
    begin_request(&s, 10, 1, 1920, 1080);
    assert(!model_ack(&s, 10, ASB_DISPLAY_CONTROL_STATUS_MODE_FAILED,
                      1920, 1080, 0));
    assert(s.pending_id == 0 && s.queued && s.retry_count == 1);

    /* Stale ACK leaves the new request untouched. */
    begin_request(&s, 11, 1, 1920, 1080);
    assert(!model_ack(&s, 10, ASB_DISPLAY_CONTROL_STATUS_ACCEPTED,
                      1920, 1080, 1));
    assert(s.pending_id == 11);

    /* Completion uses the normalized ACK target, not the raw request. */
    begin_request(&s, 12, 1, 1573, 887);
    assert(model_ack(&s, 12, ASB_DISPLAY_CONTROL_STATUS_ACCEPTED,
                     1576, 888, 100));
    assert(model_frame(&s, 1576, 888, 1));
    assert(s.pending_id == 0 && s.actual_width == 1576 &&
           s.actual_height == 888);

    /* Accepted ACK without ASFR times out and retries, then abandons. */
    begin_request(&s, 13, 1, 1920, 1080);
    assert(model_ack(&s, 13, ASB_DISPLAY_CONTROL_STATUS_ACCEPTED,
                     1920, 1080, 200));
    assert(model_timeout(&s, 200 + ASB_DISPLAY_RESIZE_COMPLETION_TIMEOUT_MS));
    assert(s.pending_id == 0 && s.queued && s.retry_count == 1);
    begin_request(&s, 14, 1, 1920, 1080);
    assert(model_ack(&s, 14, ASB_DISPLAY_CONTROL_STATUS_ACCEPTED,
                     1920, 1080, 500));
    assert(model_timeout(&s, 500 + ASB_DISPLAY_RESIZE_COMPLETION_TIMEOUT_MS));
    begin_request(&s, 15, 1, 1920, 1080);
    assert(model_ack(&s, 15, ASB_DISPLAY_CONTROL_STATUS_ACCEPTED,
                     1920, 1080, 800));
    assert(model_timeout(&s, 800 + ASB_DISPLAY_RESIZE_COMPLETION_TIMEOUT_MS));
    assert(s.pending_id == 0 && !s.queued && s.retry_count == 0);

    /* Disconnect while pending invalidates the old generation completely. */
    begin_request(&s, 16, 1, 2560, 1440);
    model_disconnect(&s);
    s.connection_generation = 2;
    model_schedule(&s, 2560, 1440);
    assert(s.pending_id == 0 && s.queued && !s.actual_valid);

    return 0;
}
