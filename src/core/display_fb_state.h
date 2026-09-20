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

static inline int asb_display_fb_layout_equal(const AsbDisplayFbLayout *a,
                                              const AsbDisplayFbLayout *b)
{
    return a && b && a->width == b->width && a->height == b->height &&
           a->stride == b->stride && a->pixel_format == b->pixel_format;
}

#endif /* ASB_DISPLAY_FB_STATE_H */
