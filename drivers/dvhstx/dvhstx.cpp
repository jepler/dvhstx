#include <string.h>
#include <pico/stdlib.h>

extern "C" {
#include <pico/lock_core.h>
}

#include <algorithm>
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/structs/bus_ctrl.h"
#include "hardware/structs/hstx_ctrl.h"
#include "hardware/structs/hstx_fifo.h"
#include "hardware/structs/sio.h"

#include "hardware/structs/ioqspi.h"
#include "hardware/vreg.h"
#include "hardware/structs/qmi.h"
#include "hardware/pll.h"
#include "hardware/clocks.h"

#include "dvi.hpp"
#include "dvhstx.hpp"

using namespace pimoroni;

#ifdef MICROPY_BUILD_TYPE
#define FRAME_BUFFER_SIZE (640*360)
__attribute__((section(".uninitialized_data"))) static uint8_t frame_buffer_a[FRAME_BUFFER_SIZE];
__attribute__((section(".uninitialized_data"))) static uint8_t frame_buffer_b[FRAME_BUFFER_SIZE];
__attribute__((section(".uninitialized_data"))) RGB888 *global_palette[PALETTE_SIZE];
#endif

#include "font.h"

// If changing the font, note this code will not handle glyphs wider than 13 pixels
#define FONT (&intel_one_mono)

#ifdef MICROPY_BUILD_TYPE
extern "C" {
void dvhstx_debug(const char *fmt, ...);
}
#else
#include <stdio.h>
#define dvhstx_debug printf
#endif

// ----------------------------------------------------------------------------
// HSTX command lists

// Lists are padded with NOPs to be >= HSTX FIFO size, to avoid DMA rapidly
// pingponging and tripping up the IRQs.

static const uint32_t vblank_line_vsync_off_src[] = {
    HSTX_CMD_RAW_REPEAT,
    SYNC_V1_H1,
    HSTX_CMD_RAW_REPEAT,
    SYNC_V1_H0,
    HSTX_CMD_RAW_REPEAT,
    SYNC_V1_H1
};
static uint32_t vblank_line_vsync_off[count_of(vblank_line_vsync_off_src)];

static const uint32_t vblank_line_vsync_on_src[] = {
    HSTX_CMD_RAW_REPEAT,
    SYNC_V0_H1,
    HSTX_CMD_RAW_REPEAT,
    SYNC_V0_H0,
    HSTX_CMD_RAW_REPEAT,
    SYNC_V0_H1
};
static uint32_t vblank_line_vsync_on[count_of(vblank_line_vsync_on_src)];

static const uint32_t vactive_line_header_src[] = {
    HSTX_CMD_RAW_REPEAT,
    SYNC_V1_H1,
    HSTX_CMD_RAW_REPEAT,
    SYNC_V1_H0,
    HSTX_CMD_RAW_REPEAT,
    SYNC_V1_H1,
    HSTX_CMD_TMDS      
};
static uint32_t vactive_line_header[count_of(vactive_line_header_src)];

#define NUM_FRAME_LINES 2
#define NUM_CHANS 2

static DVHSTX* display = nullptr;

// ----------------------------------------------------------------------------
// DMA logic

void __scratch_x("display") dma_irq_handler() {
    display->gfx_dma_handler();
}

#define ch1_num (1)
#define ch1 (&dma_hw->ch[1])
#define ch0 (&dma_hw->ch[0])

void __scratch_x("display") DVHSTX::gfx_dma_handler() {
    // we trigger on completion of channel 1 (which may be pixel data or control data)
    dma_hw->intr = 1u << ch1_num;

    if (v_scanline >= timing_mode->v_front_porch && v_scanline < (timing_mode->v_front_porch + timing_mode->v_sync_width)) {
        // control only
        ch1->read_addr = (uintptr_t)vblank_line_vsync_on;
        ch1->transfer_count = count_of(vblank_line_vsync_on);
        ch1->ctrl_trig = dma_ctrl_meta;
    } else if (v_scanline < v_inactive_total) {
        // control only
        ch1->read_addr = (uintptr_t)vblank_line_vsync_off;
        ch1->transfer_count = count_of(vblank_line_vsync_off);
        ch1->ctrl_trig = dma_ctrl_meta;
    }  else {
        // we have data and control
        ch1->read_addr = (uintptr_t)cur_line->data;
        ch1->transfer_count = timing_mode->h_active_pixels/2;
        ch0->al1_ctrl = dma_ctrl_data;
        ch0->read_addr = (uintptr_t)vactive_line_header;
        ch0->transfer_count = count_of(vactive_line_header);
        ch0->ctrl_trig = dma_ctrl_meta;

    }

    if (++v_scanline == v_total_lines) {
        v_scanline = 0;
        //__sev();
    }

    const int y = v_scanline - v_inactive_total;
    while (y == 0 || y >= cur_line->physical_end_line) {
        auto new_line = try_get_filled_line();
        if (new_line) {
            if (cur_line)
                put_empty_line(cur_line);
            cur_line = new_line;
            if (y == 0) break;
        } else {
            underflow_count++;
            break;
        }
    }
}

// ----------------------------------------------------------------------------
// Experimental clock config

#ifndef MICROPY_BUILD_TYPE
static void __no_inline_not_in_flash_func(set_qmi_timing)() {
    // Make sure flash is deselected - QMI doesn't appear to have a busy flag(!)
    while ((ioqspi_hw->io[1].status & IO_QSPI_GPIO_QSPI_SS_STATUS_OUTTOPAD_BITS) != IO_QSPI_GPIO_QSPI_SS_STATUS_OUTTOPAD_BITS)
        ;

    qmi_hw->m[0].timing = 0x40000202;
    //qmi_hw->m[0].timing = 0x40000101;
    // Force a read through XIP to ensure the timing is applied
    volatile uint32_t* ptr = (volatile uint32_t*)0x14000000;
    (void) *ptr;
}
#endif

extern "C" void __no_inline_not_in_flash_func(display_setup_clock_preinit)() {
    uint32_t intr_stash = save_and_disable_interrupts();

    // Before messing with clock speeds ensure QSPI clock is nice and slow
    hw_write_masked(&qmi_hw->m[0].timing, 6, QMI_M0_TIMING_CLKDIV_BITS);

    // We're going to go fast, boost the voltage a little
    vreg_set_voltage(VREG_VOLTAGE_1_15);

    // Force a read through XIP to ensure the timing is applied before raising the clock rate
    volatile uint32_t* ptr = (volatile uint32_t*)0x14000000;
    (void) *ptr;

    // Before we touch PLLs, switch sys and ref cleanly away from their aux sources.
    hw_clear_bits(&clocks_hw->clk[clk_sys].ctrl, CLOCKS_CLK_SYS_CTRL_SRC_BITS);
    while (clocks_hw->clk[clk_sys].selected != 0x1)
        tight_loop_contents();
    hw_write_masked(&clocks_hw->clk[clk_ref].ctrl, CLOCKS_CLK_REF_CTRL_SRC_VALUE_XOSC_CLKSRC, CLOCKS_CLK_REF_CTRL_SRC_BITS);
    while (clocks_hw->clk[clk_ref].selected != 0x4)
        tight_loop_contents();

    // Stop the other clocks so we don't worry about overspeed
    clock_stop(clk_usb);
    clock_stop(clk_adc);
    clock_stop(clk_peri);
    clock_stop(clk_hstx);

    // Set USB PLL to 528MHz
    pll_init(pll_usb, PLL_COMMON_REFDIV, 1584 * MHZ, 3, 1);

    const uint32_t usb_pll_freq = 528 * MHZ;

    // CLK SYS = PLL USB 528MHz / 2 = 264MHz
    clock_configure(clk_sys,
                    CLOCKS_CLK_SYS_CTRL_SRC_VALUE_CLKSRC_CLK_SYS_AUX,
                    CLOCKS_CLK_SYS_CTRL_AUXSRC_VALUE_CLKSRC_PLL_USB,
                    usb_pll_freq, usb_pll_freq / 2);

    // CLK PERI = PLL USB 528MHz / 4 = 132MHz
    clock_configure(clk_peri,
                    0, // Only AUX mux on ADC
                    CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLKSRC_PLL_USB,
                    usb_pll_freq, usb_pll_freq / 4);

    // CLK USB = PLL USB 528MHz / 11 = 48MHz
    clock_configure(clk_usb,
                    0, // No GLMUX
                    CLOCKS_CLK_USB_CTRL_AUXSRC_VALUE_CLKSRC_PLL_USB,
                    usb_pll_freq,
                    USB_CLK_KHZ * KHZ);

    // CLK ADC = PLL USB 528MHz / 11 = 48MHz
    clock_configure(clk_adc,
                    0, // No GLMUX
                    CLOCKS_CLK_ADC_CTRL_AUXSRC_VALUE_CLKSRC_PLL_USB,
                    usb_pll_freq,
                    USB_CLK_KHZ * KHZ);

    // Now we are running fast set fast QSPI clock and read delay
    // On MicroPython this is setup by main.
#ifndef MICROPY_BUILD_TYPE
    set_qmi_timing();
#endif

    restore_interrupts(intr_stash);
}

#ifndef MICROPY_BUILD_TYPE
// Trigger clock setup early - on MicroPython this is done by a hook in main.
namespace {
    class DV_preinit {
        public:
        DV_preinit() {
            display_setup_clock_preinit();
        }
    };
    DV_preinit dv_preinit __attribute__ ((init_priority (101))) ;
}
#endif

void DVHSTX::display_setup_clock() {
    const uint32_t dvi_clock_khz = timing_mode->bit_clk_khz >> 1;
    uint vco_freq, post_div1, post_div2;
    if (!check_sys_clock_khz(dvi_clock_khz, &vco_freq, &post_div1, &post_div2))
        panic("System clock of %u kHz cannot be exactly achieved", dvi_clock_khz);
    const uint32_t freq = vco_freq / (post_div1 * post_div2);

    // Set the sys PLL to the requested freq
    pll_init(pll_sys, PLL_COMMON_REFDIV, vco_freq, post_div1, post_div2);

    // CLK HSTX = Requested freq
    clock_configure(clk_hstx,
                    0,
                    CLOCKS_CLK_HSTX_CTRL_AUXSRC_VALUE_CLKSRC_PLL_SYS,
                    freq, freq);
}

DVHSTX::DVHSTX()
{
    // Always use the bottom channels
    dma_claim_mask((1 << NUM_CHANS) - 1);
}

bool DVHSTX::init(uint16_t width, uint16_t height, Mode mode_, Pinout pinout)
{
    if (inited) reset();

    display_width = width;
    display_height = height;
    frame_width = width;
    frame_height = height;
    mode = mode_;

    timing_mode = nullptr;
    if (width == 320 && height == 180) {
        h_repeat_shift = 2;
        v_repeat_shift = 2;
        timing_mode = &dvi_timing_1280x720p_rb_50hz;
    }
    else if (width == 640 && height == 360) {
        h_repeat_shift = 1;
        v_repeat_shift = 1;
        timing_mode = &dvi_timing_1280x720p_rb_50hz;
    }
    else if (width == 480 && height == 270) {
        h_repeat_shift = 2;
        v_repeat_shift = 2;
        timing_mode = &dvi_timing_1920x1080p_rb2_30hz;
    }
    else
    {
        volatile uint16_t full_width = display_width;
        volatile uint16_t full_height = display_height;
        h_repeat_shift = 0;
        v_repeat_shift = 0;

        if (display_width < 640) {
            h_repeat_shift = 1;
            full_width *= 2;
        }

        if (display_height < 400) {
            v_repeat_shift = 1;
            full_height *= 2;
        }

        if (full_width == 640) {
            if (full_height == 480) timing_mode = &dvi_timing_640x480p_60hz;
        }
        else if (full_width == 1280 && full_height == 720) {
            timing_mode = &dvi_timing_1280x720p_rb_50hz;
        }
        else if (full_width == 720) {
            if (full_height == 480) timing_mode = &dvi_timing_720x480p_60hz;
            else if (full_height == 400) timing_mode = &dvi_timing_720x400p_70hz;
            else if (full_height == 576) timing_mode = &dvi_timing_720x576p_50hz;
        }
        else if (full_width == 800) {
            if (full_height == 600) timing_mode = &dvi_timing_800x600p_60hz;
            else if (full_height == 480) timing_mode = &dvi_timing_800x480p_60hz;
            else if (full_height == 450) timing_mode = &dvi_timing_800x450p_60hz;
        }
        else if (full_width == 960) {
            if (full_height == 540) timing_mode = &dvi_timing_960x540p_60hz;
        }
        else if (full_width == 1024) {
            if (full_height == 768) timing_mode = &dvi_timing_1024x768_rb_60hz;
        }
    }

    if (!timing_mode) {
        dvhstx_debug("Unsupported resolution %dx%d", width, height);
        __builtin_trap();
        return false;
    }

    display = this;
    
    dvhstx_debug("Setup clock\n");
    display_setup_clock();

#ifndef MICROPY_BUILD_TYPE
    stdio_init_all();
#endif
    dvhstx_debug("Clock setup done\n");

    v_inactive_total = timing_mode->v_front_porch + timing_mode->v_sync_width + timing_mode->v_back_porch;
    v_total_lines = v_inactive_total + timing_mode->v_active_lines;
    v_active_lines = timing_mode->v_active_lines;

    v_repeat = 1 << v_repeat_shift;
    h_repeat = 1 << h_repeat_shift;

    memcpy(vblank_line_vsync_off, vblank_line_vsync_off_src, sizeof(vblank_line_vsync_off_src));
    vblank_line_vsync_off[0] |= timing_mode->h_front_porch;
    vblank_line_vsync_off[2] |= timing_mode->h_sync_width;
    vblank_line_vsync_off[4] |= timing_mode->h_back_porch + timing_mode->h_active_pixels;

    memcpy(vblank_line_vsync_on, vblank_line_vsync_on_src, sizeof(vblank_line_vsync_on_src));
    vblank_line_vsync_on[0] |= timing_mode->h_front_porch;
    vblank_line_vsync_on[2] |= timing_mode->h_sync_width;
    vblank_line_vsync_on[4] |= timing_mode->h_back_porch + timing_mode->h_active_pixels;

    memcpy(vactive_line_header, vactive_line_header_src, sizeof(vactive_line_header_src));
    vactive_line_header[0] |= timing_mode->h_front_porch;
    vactive_line_header[2] |= timing_mode->h_sync_width;
    vactive_line_header[4] |= timing_mode->h_back_porch;
    vactive_line_header[6] |= timing_mode->h_active_pixels;

    switch (mode) {
    case MODE_RGB565_H2X:
        line_bytes_per_pixel = 1; // 2BPP but DMA tricks are used to double the data
        break;
    case MODE_RGB888:
        line_bytes_per_pixel = 4;
        break;
    default:
        dvhstx_debug("Unsupported mode %d", (int)mode);
        return false;
    }

    const size_t line_bytes = frame_width * line_bytes_per_pixel;

    queue_init(&filled_line_queue, sizeof(uint8_t), count_of(lines));
    queue_init(&empty_line_queue, sizeof(uint8_t), count_of(lines));

    line_buffers = (uint32_t*)malloc(line_bytes * count_of(lines));

    for (uint8_t i = 0; i < count_of(lines); ++i)
    {
        memcpy(&line_buffers[line_bytes * i], vactive_line_header, count_of(vactive_line_header) * sizeof(uint32_t));
        lines[i].data = &line_buffers[line_bytes * i];
        queue_add_blocking(&empty_line_queue, &i);
    }

    cur_line = &lines[count_of(lines) - 1];

    // Ensure HSTX FIFO is clear
    reset_block_num(RESET_HSTX);
    sleep_us(10);
    unreset_block_num_wait_blocking(RESET_HSTX);
    sleep_us(10);

    switch (mode) {
    case MODE_RGB565_H2X:
        // Configure HSTX's TMDS encoder for RGB565
        hstx_ctrl_hw->expand_tmds =
            4  << HSTX_CTRL_EXPAND_TMDS_L2_NBITS_LSB |
            8 << HSTX_CTRL_EXPAND_TMDS_L2_ROT_LSB   |
            5  << HSTX_CTRL_EXPAND_TMDS_L1_NBITS_LSB |
            3  << HSTX_CTRL_EXPAND_TMDS_L1_ROT_LSB   |
            4  << HSTX_CTRL_EXPAND_TMDS_L0_NBITS_LSB |
            29 << HSTX_CTRL_EXPAND_TMDS_L0_ROT_LSB;

        // Pixels (TMDS) come in 2 16-bit chunks. Control symbols (RAW) are an
        // entire 32-bit word.
        hstx_ctrl_hw->expand_shift =
            2 << HSTX_CTRL_EXPAND_SHIFT_ENC_N_SHIFTS_LSB |
            16 << HSTX_CTRL_EXPAND_SHIFT_ENC_SHIFT_LSB |
            1 << HSTX_CTRL_EXPAND_SHIFT_RAW_N_SHIFTS_LSB |
            0 << HSTX_CTRL_EXPAND_SHIFT_RAW_SHIFT_LSB;
        break;

    case MODE_RGB888:
        // Configure HSTX's TMDS encoder for RGB888
        hstx_ctrl_hw->expand_tmds =
            7  << HSTX_CTRL_EXPAND_TMDS_L2_NBITS_LSB |
            16 << HSTX_CTRL_EXPAND_TMDS_L2_ROT_LSB   |
            7  << HSTX_CTRL_EXPAND_TMDS_L1_NBITS_LSB |
            8  << HSTX_CTRL_EXPAND_TMDS_L1_ROT_LSB   |
            7  << HSTX_CTRL_EXPAND_TMDS_L0_NBITS_LSB |
            0  << HSTX_CTRL_EXPAND_TMDS_L0_ROT_LSB;

        // Pixels and control symbols (RAW) are an
        // entire 32-bit word.
        hstx_ctrl_hw->expand_shift =
            1 << HSTX_CTRL_EXPAND_SHIFT_ENC_N_SHIFTS_LSB |
            0 << HSTX_CTRL_EXPAND_SHIFT_ENC_SHIFT_LSB |
            1 << HSTX_CTRL_EXPAND_SHIFT_RAW_N_SHIFTS_LSB |
            0 << HSTX_CTRL_EXPAND_SHIFT_RAW_SHIFT_LSB;
        break;

    default:
        dvhstx_debug("Unsupported mode %d", (int)mode);
        return false;
    }

    // disable hstx peripheral ... we'll enable it in a second
    hstx_ctrl_hw->csr = 0;

    // HSTX outputs 0 through 7 appear on GPIO 12 through 19.
    constexpr int HSTX_FIRST_PIN = 12;

    // Assign clock pair to two neighbouring pins:
    {
    int bit = pinout.clk_p - HSTX_FIRST_PIN;
    hstx_ctrl_hw->bit[bit    ] = HSTX_CTRL_BIT0_CLK_BITS;
    hstx_ctrl_hw->bit[bit ^ 1] = HSTX_CTRL_BIT0_CLK_BITS | HSTX_CTRL_BIT0_INV_BITS;
    }

    for (uint lane = 0; lane < 3; ++lane) {
        // For each TMDS lane, assign it to the correct GPIO pair based on the
        // desired pinout:
        int bit = pinout.rgb_p[lane] - HSTX_FIRST_PIN;
        // Output even bits during first half of each HSTX cycle, and odd bits
        // during second half. The shifter advances by two bits each cycle.
        uint32_t lane_data_sel_bits =
            (lane * 10    ) << HSTX_CTRL_BIT0_SEL_P_LSB |
            (lane * 10 + 1) << HSTX_CTRL_BIT0_SEL_N_LSB;
        // The two halves of each pair get identical data, but one pin is inverted.
        hstx_ctrl_hw->bit[bit    ] = lane_data_sel_bits;
        hstx_ctrl_hw->bit[bit ^ 1] = lane_data_sel_bits | HSTX_CTRL_BIT0_INV_BITS;
}

    for (int i = 12; i <= 19; ++i) {
        gpio_set_function(i, GPIO_FUNC_HSTX);
        gpio_set_drive_strength(i, GPIO_DRIVE_STRENGTH_4MA);
    }

    // Serial output config: clock period of 5 cycles, pop from command
    // expander every 5 cycles, shift the output shiftreg by 2 every cycle.
    hstx_ctrl_hw->csr = 0;
    hstx_ctrl_hw->csr =
        HSTX_CTRL_CSR_EXPAND_EN_BITS |
        5u << HSTX_CTRL_CSR_CLKDIV_LSB |
        5u << HSTX_CTRL_CSR_N_SHIFTS_LSB |
        2u << HSTX_CTRL_CSR_SHIFT_LSB |
        HSTX_CTRL_CSR_EN_BITS; 

    dvhstx_debug("GPIO configured\n");

    // This creates a dma_channel_config with chain_to=1. When loaded as the
    // ctrl register of dma channel 0, this will chain from the "metadata" part of the
    // data into the "data" part of the line (for active lines). When loaded
    // into the ctrl register of dma channel 1, this will not chain to anything.
    //
    // In either event, we have until the 8x32 data FIFO is cleared to respond to the DMA
    // request and start a fresh DMA transaction in the IRQ handler. (16 pixels
    // times in the case of DOOM)
    dma_channel_config c;
    c = dma_channel_get_default_config(1);
    channel_config_set_dreq(&c, DREQ_HSTX);
    dma_ctrl_meta = channel_config_get_ctrl_value(&c);

    c = dma_channel_get_default_config(1);
    channel_config_set_dreq(&c, DREQ_HSTX);
    channel_config_set_transfer_data_size(&c, mode == MODE_RGB565_H2X ? DMA_SIZE_16 : DMA_SIZE_32);
    dma_ctrl_data = channel_config_get_ctrl_value(&c);

    c = dma_channel_get_default_config(0);
    channel_config_set_dreq(&c, DREQ_HSTX);
    dma_channel_configure(
        0,
        &c,
        &hstx_fifo_hw->fifo,
        vblank_line_vsync_off,
        count_of(vblank_line_vsync_off),
        false
    );
    c = dma_channel_get_default_config(1);
    channel_config_set_dreq(&c, DREQ_HSTX);
    dma_channel_configure(
        1,
        &c,
        &hstx_fifo_hw->fifo,
        vblank_line_vsync_off,
        count_of(vblank_line_vsync_off),
        false
    );

    dvhstx_debug("DMA channels claimed\n");

    dma_hw->intr |= (1 << ch1_num);
    dma_hw->ints2 |= (1 << ch1_num);
    dma_hw->inte2 |= (1 << ch1_num);
    irq_set_exclusive_handler(DMA_IRQ_2, dma_irq_handler);
    irq_set_enabled(DMA_IRQ_2, true);

    dvhstx_debug("DVHSTX configured\n");

    inited = true;
    return true;
}

void DVHSTX::reset() {
    if (!inited) return;
    inited = false;

    hstx_ctrl_hw->csr = 0;

    irq_set_enabled(DMA_IRQ_2, false);
    irq_remove_handler(DMA_IRQ_2, irq_get_exclusive_handler(DMA_IRQ_2));

    for (int i = 0; i < NUM_CHANS; ++i)
        dma_channel_abort(i);

    free(line_buffers);
}

int DVHSTX::get_h_active_pixels() const {
    return timing_mode->h_active_pixels;
}


DVHSTX::line_data_t *DVHSTX::try_get_empty_line() {
    uint8_t result;
    bool status = queue_try_remove(&empty_line_queue, &result);
    if (!status) {
        return NULL;
    }
    auto line = &lines[result];
    line->physical_start_line = queue_physical_line;
    line->logical_line_number = queue_logical_line;
    return line;
}


DVHSTX::line_data_t *DVHSTX::try_get_filled_line() {
    uint8_t idx;
    if (!queue_try_remove(&filled_line_queue, &idx)) {
        return NULL;
    }
    return &lines[idx]; 
}

void DVHSTX::put_filled_line(DVHSTX::line_data_t *line) {
    uint8_t idx = line - lines;
    queue_physical_line = line->physical_end_line;
    if (queue_physical_line >= v_active_lines) {
        queue_physical_line = 0;
        queue_logical_line = 0;
    } else {
        queue_logical_line ++;
    }

    queue_add_blocking(&filled_line_queue, &idx);
    if (!started && queue_is_full(&filled_line_queue)) {
        started = true;
        gfx_dma_handler();
        dvhstx_debug("buffers full, DMA started\n");
    }
}

