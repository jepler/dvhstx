#pragma once

#include <string.h>

#include "drivers/dvhstx/dvhstx.h"

#include "hardware/gpio.h"
#include "pico/stdlib.h"
#include "pico/util/queue.h"

// DVI HSTX driver for use with Pimoroni PicoGraphics

namespace pimoroni {

  // Digital Video using HSTX
  // Valid screen modes are:
  //   Pixel doubled: 640x480 (60Hz), 720x480 (60Hz), 720x400 (70Hz), 720x576 (50Hz), 
  //                  800x600 (60Hz), 800x480 (60Hz), 800x450 (60Hz), 960x540 (60Hz), 1024x768 (60Hz)
  //   Pixel doubled or quadrupled: 1280x720 (50Hz)
  //
  // Giving valid resolutions:
  //   320x180, 640x360 (well supported, square pixels on a 16:9 display)
  //   480x270, 400x225 (sometimes supported, square pixels on a 16:9 display)
  //   320x240, 360x240, 360x200, 360x288, 400x300, 512x384 (well supported, but pixels aren't square)
  //   400x240 (sometimes supported, pixels aren't square)
  //
  // Note that the double buffer is in RAM, so 640x360 uses almost all of the available RAM.
  class DVHSTX {
  public:
    static constexpr int PALETTE_SIZE = 256;

    struct Pinout {
        uint8_t clk_p, rgb_p[3];
    };

    enum Mode {
      MODE_RGB565_H2X, // pixels are horizontally doubled
      MODE_RGB888,
    };

    enum TextColour {
      TEXT_BLACK   = 0,
      TEXT_RED     = 0b1000000,
      TEXT_GREEN   = 0b0001000,
      TEXT_BLUE    = 0b0000001,
      TEXT_YELLOW  = 0b1001000,
      TEXT_MAGENTA = 0b1000001,
      TEXT_CYAN    = 0b0001001,
      TEXT_WHITE   = 0b1001001,
    };    

    //--------------------------------------------------
    // Variables
    //--------------------------------------------------
  protected:
    friend void vsync_callback();

    uint16_t display_width = 320;
    uint16_t display_height = 180;
    uint16_t frame_width = 320;
    uint16_t frame_height = 180;
    uint8_t h_repeat = 4;
    uint8_t v_repeat = 4;
    Mode mode = MODE_RGB565_H2X;

  public:
    DVHSTX();

    using line_fun_t = dvhstx_line_fun_t;
    using line_data_t = dvhstx_line_data_t;
    //--------------------------------------------------
    // Methods
    //--------------------------------------------------
    public:
      bool init(uint16_t width, uint16_t height, Mode mode, Pinout pinout);
      void reset();

      int get_h_repeat_shift() const { return h_repeat_shift; }
      int get_h_active_pixels() const;

      // DMA handlers, should not be called externally
      void gfx_dma_handler(); 

private:
      line_data_t lines[5];
      int queue_physical_line, queue_logical_line;
      bool started;
      volatile int underflow_count;

      line_data_t *cur_line, *old_line;

      uint32_t dma_ctrl_meta;
      uint32_t dma_ctrl_data;
public:
      line_data_t *try_get_empty_line();

      void put_filled_line(line_data_t *line);

    private:
      line_data_t *try_get_filled_line();

      void put_empty_line(line_data_t *line) {
        uint8_t result = line - lines;
        queue_add_blocking(&empty_line_queue, &result);
      }
      queue_t empty_line_queue;
      queue_t filled_line_queue;

      void display_setup_clock();

      // DMA scanline filling
      uint ch_num = 0;
      int line_num = -1;

      bool inited = false;

      uint32_t* line_buffers;
      const struct dvi_timing* timing_mode;
      int v_inactive_total;
      int v_total_lines;
      int v_active_lines;
      volatile int v_scanline;

      uint h_repeat_shift;
      uint v_repeat_shift;
      int line_bytes_per_pixel;

      uint32_t* display_palette = nullptr;

      line_fun_t callback = nullptr;
      void *cb_data = nullptr;
  };
}
