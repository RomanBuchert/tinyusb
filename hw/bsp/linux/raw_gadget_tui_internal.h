/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2026 TinyUSB contributors
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */


#pragma once

#include "raw_gadget_tui.h"
#include "raw_gadget_log.h"

#include <curses.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

#define RAW_GADGET_TUI_MINIMUM_COLUMNS 80
#define RAW_GADGET_TUI_MINIMUM_ROWS    24
#define RAW_GADGET_TUI_REFRESH_MS      20

#define RAW_GADGET_TUI_INDICATOR_COLUMNS    8u
#define RAW_GADGET_TUI_INDICATOR_CELL_WIDTH 8
#define RAW_GADGET_TUI_INDICATOR_ROWS                                      \
   ((RAW_GADGET_TUI_IO_COUNT + RAW_GADGET_TUI_INDICATOR_COLUMNS - 1u) /   \
    RAW_GADGET_TUI_INDICATOR_COLUMNS)

#define RAW_GADGET_TUI_UART_INPUT_LENGTH  256u
#define RAW_GADGET_TUI_UART_RX_CAPACITY   4096u
#define RAW_GADGET_TUI_UART_TX_CAPACITY   65536u

typedef enum {
   RAW_GADGET_TUI_PAGE_HELP,
   RAW_GADGET_TUI_PAGE_BOARD,
   RAW_GADGET_TUI_PAGE_LOG,
} raw_gadget_tui_page_t;

typedef enum {
   RAW_GADGET_TUI_FOCUS_BUTTONS,
   RAW_GADGET_TUI_FOCUS_UART_INPUT,
   RAW_GADGET_TUI_FOCUS_UART_OUTPUT,
} raw_gadget_tui_focus_t;

typedef enum {
   RAW_GADGET_TUI_STATE_STOPPED,
   RAW_GADGET_TUI_STATE_STARTING,
   RAW_GADGET_TUI_STATE_RUNNING,
   RAW_GADGET_TUI_STATE_STOPPING,
   RAW_GADGET_TUI_STATE_FAILED,
} raw_gadget_tui_state_t;

typedef struct {
   uint8_t data[RAW_GADGET_TUI_UART_RX_CAPACITY];
   size_t read_index;
   size_t write_index;
   size_t count;
} raw_gadget_tui_fifo_t;

typedef struct {
   uint8_t data[RAW_GADGET_TUI_UART_TX_CAPACITY];
   size_t start;
   size_t length;
} raw_gadget_tui_ring_t;

typedef struct {
   size_t scroll_lines;
   bool follow;
} raw_gadget_tui_text_view_t;

typedef struct {
   WINDOW *buttons;
   WINDOW *leds;
   WINDOW *uart_input;
   WINDOW *uart_output;
   WINDOW *log;
   int rows;
   int columns;
   bool valid;
} raw_gadget_tui_windows_t;

typedef struct {
   raw_gadget_tui_page_t page;
   raw_gadget_tui_focus_t focus;
   raw_gadget_tui_text_view_t uart_output_view;
   raw_gadget_tui_text_view_t log_view;
   uint8_t selected_button;
   char uart_input[RAW_GADGET_TUI_UART_INPUT_LENGTH];
   size_t uart_input_length;
   size_t uart_input_cursor;
   bool hidden;
} raw_gadget_tui_view_t;

typedef struct {
   uint32_t button_states;
   uint32_t led_states;
   uint8_t uart_tx[RAW_GADGET_TUI_UART_TX_CAPACITY];
   size_t uart_tx_length;
   uint8_t log[RAW_GADGET_LOG_CAPACITY];
   size_t log_length;
} raw_gadget_tui_snapshot_t;

typedef struct {
   pthread_t thread;
   pthread_mutex_t lifecycle_mutex;
   pthread_cond_t condition;
   pthread_mutex_t io_mutex;
   atomic_bool run_requested;
   raw_gadget_tui_state_t state;
   uint32_t button_events;
   uint32_t button_toggles;
   uint32_t led_states;
   raw_gadget_tui_fifo_t uart_rx;
   raw_gadget_tui_ring_t uart_tx;
   bool thread_created;
   bool atexit_registered;
} raw_gadget_tui_context_t;

extern raw_gadget_tui_context_t raw_gadget_tui_context;

void raw_gadget_tui_take_snapshot(raw_gadget_tui_snapshot_t *snapshot);
void raw_gadget_tui_button_pulse(uint8_t button);
void raw_gadget_tui_button_toggle(uint8_t button);
void raw_gadget_tui_uart_submit(raw_gadget_tui_view_t *view);

void raw_gadget_tui_windows_destroy(raw_gadget_tui_windows_t *windows);
bool raw_gadget_tui_windows_create(raw_gadget_tui_windows_t *windows);
bool raw_gadget_tui_windows_need_recreate(raw_gadget_tui_windows_t const *windows);
void raw_gadget_tui_draw(raw_gadget_tui_windows_t *windows,
                         raw_gadget_tui_view_t *view,
                         raw_gadget_tui_snapshot_t const *snapshot);

void raw_gadget_tui_handle_key(int key,
                               raw_gadget_tui_view_t *view,
                               raw_gadget_tui_windows_t const *windows);
