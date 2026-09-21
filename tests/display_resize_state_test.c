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
    bool send_completed;
    bool runtime_active;
    uint64_t runtime_deadline;
    enum {
        PHASE_NONE = 0,
        PHASE_WAIT_ACK,
        PHASE_WAIT_FRAME,
    } phase;
    bool queued;
} ResizeModel;

static void model_runtime_begin(ResizeModel *s, uint32_t width,
                                uint32_t height, uint64_t now)
{
    s->runtime_active = true;
    s->runtime_deadline = now + ASB_DISPLAY_RUNTIME_REQUEST_TIMEOUT_MS;
    s->desired_width = width;
    s->desired_height = height;
}

static bool model_runtime_terminal(ResizeModel *s, uint64_t now)
{
    if (!s->runtime_active || !s->runtime_deadline ||
        now < s->runtime_deadline)
        return false;
    s->runtime_active = false;
    s->runtime_deadline = 0;
    s->pending_id = 0;
    s->pending_generation = 0;
    s->pending_width = 0;
    s->pending_height = 0;
    s->deadline = 0;
    s->phase = PHASE_NONE;
    s->queued = false;
    return true;
}

static bool model_runtime_abort(ResizeModel *s)
{
    if (!s->runtime_active) return false;
    s->runtime_active = false;
    s->runtime_deadline = 0;
    s->pending_id = 0;
    s->pending_generation = 0;
    s->pending_width = 0;
    s->pending_height = 0;
    s->deadline = 0;
    s->phase = PHASE_NONE;
    s->queued = false;
    return true;
}

static void model_disconnect(ResizeModel *s)
{
    s->actual_valid = false;
    s->pending_generation = 0;
    s->pending_id = 0;
    s->pending_width = 0;
    s->pending_height = 0;
    s->retry_count = 0;
    s->deadline = 0;
    s->phase = PHASE_NONE;
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
                      uint64_t generation, uint64_t now)
{
    if (request_id != s->pending_id ||
        generation != s->pending_generation ||
        s->phase != PHASE_WAIT_ACK)
        return false; /* stale ACK does not mutate current request */

    if (status == ASB_DISPLAY_CONTROL_STATUS_ACCEPTED) {
        s->pending_width = width;
        s->pending_height = height;
        s->phase = PHASE_WAIT_FRAME;
        s->deadline = now + ASB_DISPLAY_RESIZE_COMPLETION_TIMEOUT_MS;
        return true;
    }

    s->pending_id = 0;
    s->pending_generation = 0;
    s->pending_width = 0;
    s->pending_height = 0;
    s->deadline = 0;
    s->phase = PHASE_NONE;
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
        s->phase == PHASE_WAIT_FRAME &&
        s->pending_width == width && s->pending_height == height) {
        s->pending_id = 0;
        s->pending_generation = 0;
        s->pending_width = 0;
        s->pending_height = 0;
        s->deadline = 0;
        s->phase = PHASE_NONE;
        s->retry_count = 0;
        return true;
    }
    return false;
}

static bool model_timeout(ResizeModel *s, uint64_t now)
{
    if (!s->pending_id || s->phase == PHASE_NONE || !s->deadline ||
        now < s->deadline)
        return false;
    s->pending_id = 0;
    s->pending_generation = 0;
    s->pending_width = 0;
    s->pending_height = 0;
    s->deadline = 0;
    s->phase = PHASE_NONE;
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
    s->phase = PHASE_NONE;
    s->deadline = 0;
    s->send_completed = false;
}

/* Mirrors the Windows sender's prepare step. This deliberately happens before
 * the simulated send completes so a guest ACK can race with the final write. */
static bool model_prepare_send(ResizeModel *s, uint64_t now)
{
    if (!s->pending_id || s->phase != PHASE_NONE)
        return false;
    s->phase = PHASE_WAIT_ACK;
    s->deadline = now + ASB_DISPLAY_RESIZE_ACK_TIMEOUT_MS;
    return true;
}

static void model_send_complete(ResizeModel *s)
{
    s->send_completed = true;
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
    assert(model_prepare_send(&s, 0));
    assert(!model_ack(&s, 10, ASB_DISPLAY_CONTROL_STATUS_MODE_FAILED,
                      1920, 1080, 1, 1));
    assert(s.pending_id == 0 && s.queued && s.retry_count == 1);

    /* No ACK: old-size frames do not keep a sent request pending forever. */
    s.retry_count = 0;
    begin_request(&s, 20, 1, 1920, 1080);
    assert(model_prepare_send(&s, 100));
    model_send_complete(&s);
    assert(!model_frame(&s, 1280, 720, 1));
    assert(model_timeout(&s, 100 + ASB_DISPLAY_RESIZE_ACK_TIMEOUT_MS));
    assert(s.pending_id == 0 && s.queued && s.retry_count == 1);
    begin_request(&s, 21, 1, 1920, 1080);
    assert(model_prepare_send(&s, 1500));
    /* A late ACK for id=20 cannot resurrect the timed-out request. */
    assert(!model_ack(&s, 20, ASB_DISPLAY_CONTROL_STATUS_ACCEPTED,
                      1920, 1080, 1, 1600));
    assert(s.pending_id == 21 && s.phase == PHASE_WAIT_ACK);
    assert(model_timeout(&s, 1500 + ASB_DISPLAY_RESIZE_ACK_TIMEOUT_MS));
    assert(s.pending_id == 0 && s.queued && s.retry_count == 2);
    begin_request(&s, 22, 1, 1920, 1080);
    assert(model_prepare_send(&s, 3000));
    assert(model_timeout(&s, 3000 + ASB_DISPLAY_RESIZE_ACK_TIMEOUT_MS));
    assert(s.pending_id == 0 && !s.queued && s.retry_count == 0);

    /* A later request remains usable after the bounded abandon. */
    begin_request(&s, 23, 1, 1920, 1080);
    assert(model_prepare_send(&s, 10));
    assert(s.pending_id == 23 && s.phase == PHASE_WAIT_ACK);

    /* Completion uses the normalized ACK target, not the raw request. */
    begin_request(&s, 24, 1, 1573, 887);
    assert(model_prepare_send(&s, 100));
    assert(model_ack(&s, 24, ASB_DISPLAY_CONTROL_STATUS_ACCEPTED,
                     1576, 888, 1, 100));
    assert(model_frame(&s, 1576, 888, 1));
    assert(s.pending_id == 0 && s.actual_width == 1576 &&
           s.actual_height == 888);

    /* Accepted ACK without ASFR times out and retries, then abandons. */
    begin_request(&s, 25, 1, 1920, 1080);
    assert(model_prepare_send(&s, 200));
    assert(model_ack(&s, 25, ASB_DISPLAY_CONTROL_STATUS_ACCEPTED,
                     1920, 1080, 1, 200));
    assert(model_timeout(&s, 200 + ASB_DISPLAY_RESIZE_COMPLETION_TIMEOUT_MS));
    assert(s.pending_id == 0 && s.queued && s.retry_count == 1);
    begin_request(&s, 26, 1, 1920, 1080);
    assert(model_prepare_send(&s, 500));
    assert(model_ack(&s, 26, ASB_DISPLAY_CONTROL_STATUS_ACCEPTED,
                     1920, 1080, 1, 500));
    assert(model_timeout(&s, 500 + ASB_DISPLAY_RESIZE_COMPLETION_TIMEOUT_MS));
    begin_request(&s, 27, 1, 1920, 1080);
    assert(model_prepare_send(&s, 800));
    assert(model_ack(&s, 27, ASB_DISPLAY_CONTROL_STATUS_ACCEPTED,
                     1920, 1080, 1, 800));
    assert(model_timeout(&s, 800 + ASB_DISPLAY_RESIZE_COMPLETION_TIMEOUT_MS));
    assert(s.pending_id == 0 && !s.queued && s.retry_count == 0);

    /* Disconnect while pending invalidates the old generation completely. */
    begin_request(&s, 28, 1, 2560, 1440);
    assert(model_prepare_send(&s, 900));
    model_disconnect(&s);
    s.connection_generation = 2;
    model_schedule(&s, 2560, 1440);
    assert(s.pending_id == 0 && s.queued && !s.actual_valid);

    /* ACK race: WAIT_ACK is armed before the guest can observe the request.
     * The guest replies before send() returns; the ACK must be accepted and
     * the later send completion must not overwrite WAIT_FRAME. */
    s = (ResizeModel){0};
    s.connection_generation = 7;
    begin_request(&s, 29, 7, 1920, 1080);
    assert(model_prepare_send(&s, 1000));
    assert(s.phase == PHASE_WAIT_ACK);
    assert(model_ack(&s, 29, ASB_DISPLAY_CONTROL_STATUS_ACCEPTED,
                     1920, 1080, 7, 1001));
    assert(s.phase == PHASE_WAIT_FRAME);
    model_send_complete(&s);
    assert(s.send_completed && s.phase == PHASE_WAIT_FRAME);
    assert(model_frame(&s, 1920, 1080, 7));
    assert(s.pending_id == 0 && s.phase == PHASE_NONE);

    /* A request made before HELLO must have an independent lifecycle deadline;
     * resize ACK/frame timers alone cannot expire it. */
    s = (ResizeModel){0};
    model_runtime_begin(&s, 1920, 1080, 100);
    assert(s.runtime_active);
    assert(!model_runtime_terminal(&s,
                                   100 + ASB_DISPLAY_RUNTIME_REQUEST_TIMEOUT_MS - 1));
    assert(model_runtime_terminal(&s,
                                  100 + ASB_DISPLAY_RUNTIME_REQUEST_TIMEOUT_MS));
    assert(!s.runtime_active && s.pending_id == 0 && s.phase == PHASE_NONE);

    /* A frame disconnect is terminal for the explicit UI request. A later
     * reconnect may synchronize the persisted value, but cannot revive the
     * failed request. */
    s = (ResizeModel){0};
    model_runtime_begin(&s, 2560, 1440, 200);
    begin_request(&s, 30, 2, 2560, 1440);
    assert(model_prepare_send(&s, 201));
    assert(model_runtime_abort(&s)); /* disconnect during WAIT_ACK */
    model_disconnect(&s);
    assert(s.pending_id == 0 && s.phase == PHASE_NONE);

    s = (ResizeModel){0};
    model_runtime_begin(&s, 3840, 2160, 300);
    begin_request(&s, 31, 3, 3840, 2160);
    assert(model_prepare_send(&s, 301));
    assert(model_ack(&s, 31, ASB_DISPLAY_CONTROL_STATUS_ACCEPTED,
                     3840, 2160, 3, 302));
    assert(s.phase == PHASE_WAIT_FRAME);
    assert(model_runtime_abort(&s)); /* disconnect during WAIT_FRAME */
    assert(!s.runtime_active && s.pending_id == 0);

    s = (ResizeModel){0};
    model_runtime_begin(&s, 1920, 1080, 400);
    assert(model_runtime_abort(&s)); /* IDD destroy / VM stop */
    assert(!s.runtime_active && s.pending_id == 0);

    return 0;
}
