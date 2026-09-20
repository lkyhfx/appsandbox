/* SPDX-License-Identifier: MIT
 * Shared framebuffer layout semantics for the RAW display capture path.
 *
 * A DRM FB object is only a backing store.  Its id may change on every
 * compositor page flip while the visible layout remains identical, so the id
 * must not be used as a display-content version or as a reason to invalidate
 * the host shadow.
 */
#ifndef ASB_DISPLAY_FB_STATE_H
#define ASB_DISPLAY_FB_STATE_H

#include <stdint.h>

typedef struct AsbDisplayFbLayout {
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t pixel_format;
} AsbDisplayFbLayout;

typedef enum AsbDisplayFbTransition {
    ASB_FB_TRANSITION_INITIAL = 0,
    ASB_FB_TRANSITION_SAME_LAYOUT,
    ASB_FB_TRANSITION_LAYOUT_CHANGE,
    ASB_FB_TRANSITION_RECOVERY,
} AsbDisplayFbTransition;

static inline int asb_display_fb_layout_equal(const AsbDisplayFbLayout *a,
                                              const AsbDisplayFbLayout *b)
{
    return a && b && a->width == b->width && a->height == b->height &&
           a->stride == b->stride && a->pixel_format == b->pixel_format;
}

/* Decide whether a newly acquired DRM framebuffer can continue using the
 * current host shadow. FB identity is intentionally absent: page-flipping
 * between backing objects with the same layout is not a display transition. */
static inline AsbDisplayFbTransition
asb_display_fb_transition(const AsbDisplayFbLayout *old_layout,
                          int old_layout_valid,
                          const AsbDisplayFbLayout *new_layout,
                          int shadow_valid)
{
    if (!old_layout || !new_layout || !old_layout_valid)
        return ASB_FB_TRANSITION_INITIAL;
    if (!asb_display_fb_layout_equal(old_layout, new_layout))
        return ASB_FB_TRANSITION_LAYOUT_CHANGE;
    return shadow_valid ? ASB_FB_TRANSITION_SAME_LAYOUT
                        : ASB_FB_TRANSITION_RECOVERY;
}

#endif /* ASB_DISPLAY_FB_STATE_H */
