/* Build with: gcc -std=c11 -Wall -Wextra -I src tests/display_protocol_test.c -o display_protocol_test */
#include <assert.h>
#include <stdint.h>

#include "../src/core/display_protocol.h"

int main(void)
{
    uint32_t width = 1573;
    uint32_t height = 887;

    assert(sizeof(AsbDisplayControl) == 28);
    assert(asb_display_is_preset(1280, 720));
    assert(asb_display_is_preset(1920, 1080));
    assert(asb_display_is_preset(2560, 1440));
    assert(asb_display_is_preset(3840, 2160));
    assert(!asb_display_is_preset(1576, 888));
    asb_display_normalize_resolution(&width, &height);
    assert(width == 1576 && height == 888);

    width = 1;
    height = 99999;
    asb_display_normalize_resolution(&width, &height);
    assert(width == ASB_DISPLAY_CONTROL_MIN_WIDTH &&
           height == ASB_DISPLAY_CONTROL_MAX_HEIGHT);
    return 0;
}
