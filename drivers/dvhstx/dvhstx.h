#pragma once

#include <stdint.h>

typedef struct {
    uint32_t *data;
    uint16_t physical_start_line;
    uint16_t physical_end_line;
    uint16_t logical_line_number;
    uint16_t logical_frame_number;
} dvhstx_line_data_t;

typedef void(*dvhstx_line_fun_t)(void *cb_data, dvhstx_line_data_t *line_data);
