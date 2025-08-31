#define PICO_DEFAULT_UART_RX_PIN 45 

#include <stdio.h>
#include "hardware/uart.h"
#include "pico/multicore.h"
#include "drivers/dvhstx/dvhstx.hpp"

extern "C" {
#include "mandelf.h"
}


using namespace pimoroni;

#define FRAME_WIDTH 640
#define FRAME_HEIGHT 480

static DVHSTX display;

inline constexpr uint32_t RGB_to_RGB888(const uint8_t r, const uint8_t g, const uint8_t b) {
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

static uint32_t palette[256];
static void init_palette() {
    for (int i = 0; i < 256; ++i) {
#if 0
        int h = i * (1.f / 255.f), s = 1.0f, v = 0.5f + (i & 7) * (0.5f / 7.f);
        RGB p = RGB::from_hsv(h, s, v);
        palette[i] = RGB_to_RGB888(p.r, p.g, p.b);
#endif
        palette[i] = RGB_to_RGB888(i, i, i);
    }
}

void gen_line() {
    auto d = display.try_get_empty_line();
    if (!d) return;
    d->physical_end_line = d->physical_start_line + 2;
    uint16_t *dest = (uint16_t*)d->data;
    int line_num = d->logical_line_number;
    int y1 = line_num - FRAME_HEIGHT / 2;
    int ysq = y1*y1 * 4;
    for(int h=0; h<FRAME_WIDTH; h++) {
        int x = h - FRAME_WIDTH / 2;
        int r2 = x*x + ysq;
        #define LIM (320*320)
        *dest++ = palette[r2 / 256 % 256];
    }
    display.put_filled_line(d);
}

int main() {
    stdio_init_all();
    display.init(FRAME_WIDTH, FRAME_HEIGHT, DVHSTX::MODE_RGB565_H2X, {13, 15, 17, 19});
    init_palette();

    while(true) {
        gen_line();
        tight_loop_contents();
    }
}
