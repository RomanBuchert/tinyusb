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

#include "raw_gadget_tui.h"
#include "raw_gadget_log.h"

#include <curses.h>
#include <ctype.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define RAW_GADGET_TUI_MINIMUM_COLUMNS 80
#define RAW_GADGET_TUI_MINIMUM_ROWS    24
#define RAW_GADGET_TUI_REFRESH_MS      20

#define RAW_GADGET_TUI_INDICATOR_COLUMNS    8u
#define RAW_GADGET_TUI_INDICATOR_CELL_WIDTH 8

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

static raw_gadget_tui_context_t raw_gadget_tui_context = {
   .lifecycle_mutex = PTHREAD_MUTEX_INITIALIZER,
   .condition = PTHREAD_COND_INITIALIZER,
   .io_mutex = PTHREAD_MUTEX_INITIALIZER,
   .run_requested = ATOMIC_VAR_INIT(false),
   .state = RAW_GADGET_TUI_STATE_STOPPED,
};

static void raw_gadget_tui_signal_startup(raw_gadget_tui_state_t state) {
   (void) pthread_mutex_lock(&raw_gadget_tui_context.lifecycle_mutex);
   raw_gadget_tui_context.state = state;
   (void) pthread_cond_broadcast(&raw_gadget_tui_context.condition);
   (void) pthread_mutex_unlock(&raw_gadget_tui_context.lifecycle_mutex);
}

static void raw_gadget_tui_sleep(void) {
   struct timespec delay = {
      .tv_sec = 0,
      .tv_nsec = RAW_GADGET_TUI_REFRESH_MS * 1000000L,
   };

   (void) nanosleep(&delay, NULL);
}

static void raw_gadget_tui_fifo_write(raw_gadget_tui_fifo_t *fifo,
                                      uint8_t const *data,
                                      size_t length) {
   for (size_t index = 0; index < length; ++index) {
      if (fifo->count == RAW_GADGET_TUI_UART_RX_CAPACITY) {
         break;
      }

      fifo->data[fifo->write_index] = data[index];
      fifo->write_index = (fifo->write_index + 1u) % RAW_GADGET_TUI_UART_RX_CAPACITY;
      ++fifo->count;
   }
}

static size_t raw_gadget_tui_fifo_read(raw_gadget_tui_fifo_t *fifo,
                                       uint8_t *data,
                                       size_t length) {
   size_t count = 0u;

   while ((count < length) && (fifo->count > 0u)) {
      data[count] = fifo->data[fifo->read_index];
      fifo->read_index = (fifo->read_index + 1u) % RAW_GADGET_TUI_UART_RX_CAPACITY;
      --fifo->count;
      ++count;
   }

   return count;
}

static void raw_gadget_tui_ring_write(raw_gadget_tui_ring_t *ring,
                                      uint8_t const *data,
                                      size_t length) {
   for (size_t index = 0; index < length; ++index) {
      size_t write_index;

      if (ring->length < RAW_GADGET_TUI_UART_TX_CAPACITY) {
         write_index =
            (ring->start + ring->length) % RAW_GADGET_TUI_UART_TX_CAPACITY;
         ++ring->length;
      } else {
         write_index = ring->start;
         ring->start = (ring->start + 1u) % RAW_GADGET_TUI_UART_TX_CAPACITY;
      }

      ring->data[write_index] = data[index];
   }
}

static void raw_gadget_tui_ring_copy(raw_gadget_tui_ring_t const *ring,
                                     uint8_t *data,
                                     size_t *length) {
   *length = ring->length;

   for (size_t index = 0; index < ring->length; ++index) {
      data[index] =
         ring->data[(ring->start + index) % RAW_GADGET_TUI_UART_TX_CAPACITY];
   }
}

static void raw_gadget_tui_take_snapshot(raw_gadget_tui_snapshot_t *snapshot) {
   (void) pthread_mutex_lock(&raw_gadget_tui_context.io_mutex);

   snapshot->button_states =
      raw_gadget_tui_context.button_events | raw_gadget_tui_context.button_toggles;
   snapshot->led_states = raw_gadget_tui_context.led_states;
   raw_gadget_tui_ring_copy(&raw_gadget_tui_context.uart_tx,
                            snapshot->uart_tx,
                            &snapshot->uart_tx_length);

   (void) pthread_mutex_unlock(&raw_gadget_tui_context.io_mutex);

   snapshot->log_length =
      raw_gadget_log_snapshot(snapshot->log, sizeof(snapshot->log));
}

static void raw_gadget_tui_button_pulse(uint8_t button) {
   uint32_t mask;

   if (button >= RAW_GADGET_TUI_IO_COUNT) {
      return;
   }

   mask = UINT32_C(1) << button;

   (void) pthread_mutex_lock(&raw_gadget_tui_context.io_mutex);
   raw_gadget_tui_context.button_events |= mask;
   (void) pthread_mutex_unlock(&raw_gadget_tui_context.io_mutex);
}

static void raw_gadget_tui_button_toggle(uint8_t button) {
   uint32_t mask;

   if (button >= RAW_GADGET_TUI_IO_COUNT) {
      return;
   }

   mask = UINT32_C(1) << button;

   (void) pthread_mutex_lock(&raw_gadget_tui_context.io_mutex);
   raw_gadget_tui_context.button_toggles ^= mask;
   (void) pthread_mutex_unlock(&raw_gadget_tui_context.io_mutex);
}

static void raw_gadget_tui_uart_submit(raw_gadget_tui_view_t *view) {
   static uint8_t const newline = '\n';

   if (view->uart_input_length == 0u) {
      return;
   }

   (void) pthread_mutex_lock(&raw_gadget_tui_context.io_mutex);
   raw_gadget_tui_fifo_write(&raw_gadget_tui_context.uart_rx,
                             (uint8_t const *) view->uart_input,
                             view->uart_input_length);
   raw_gadget_tui_fifo_write(&raw_gadget_tui_context.uart_rx, &newline, 1u);
   (void) pthread_mutex_unlock(&raw_gadget_tui_context.io_mutex);

   view->uart_input_length = 0u;
   view->uart_input_cursor = 0u;
   view->uart_input[0] = '\0';
}

static void raw_gadget_tui_windows_destroy(raw_gadget_tui_windows_t *windows) {
   if (windows->buttons != NULL) {
      (void) delwin(windows->buttons);
   }
   if (windows->leds != NULL) {
      (void) delwin(windows->leds);
   }
   if (windows->uart_input != NULL) {
      (void) delwin(windows->uart_input);
   }
   if (windows->uart_output != NULL) {
      (void) delwin(windows->uart_output);
   }
   if (windows->log != NULL) {
      (void) delwin(windows->log);
   }

   memset(windows, 0, sizeof(*windows));
}

static bool raw_gadget_tui_windows_create(raw_gadget_tui_windows_t *windows) {
   int const content_width = COLS - 4;
   int const buttons_height = 6;
   int const leds_height = 6;
   int const uart_input_height = 3;
   int const buttons_row = 1;
   int const leds_row = buttons_row + buttons_height + 1;
   int const uart_input_row = leds_row + leds_height + 1;
   int const uart_output_row = uart_input_row + uart_input_height + 1;
   int const uart_output_height = (LINES - 3) - uart_output_row;
   int const log_height = LINES - 4;

   raw_gadget_tui_windows_destroy(windows);

   windows->rows = LINES;
   windows->columns = COLS;

   if ((COLS < RAW_GADGET_TUI_MINIMUM_COLUMNS) ||
       (LINES < RAW_GADGET_TUI_MINIMUM_ROWS)) {
      return false;
   }

   windows->buttons = newwin(buttons_height, content_width, buttons_row, 2);
   windows->leds = newwin(leds_height, content_width, leds_row, 2);
   windows->uart_input =
      newwin(uart_input_height, content_width, uart_input_row, 2);
   windows->uart_output =
      newwin(uart_output_height, content_width, uart_output_row, 2);
   windows->log = newwin(log_height, content_width, 1, 2);

   windows->valid = (windows->buttons != NULL) && (windows->leds != NULL) &&
                    (windows->uart_input != NULL) &&
                    (windows->uart_output != NULL) && (windows->log != NULL);

   if (!windows->valid) {
      raw_gadget_tui_windows_destroy(windows);
   }

   return windows->valid;
}

static bool raw_gadget_tui_windows_need_recreate(
   raw_gadget_tui_windows_t const *windows) {
   return !windows->valid || (windows->rows != LINES) ||
          (windows->columns != COLS);
}

static void raw_gadget_tui_draw_frame(WINDOW *window,
                                      char const *title,
                                      bool focused) {
   (void) werase(window);
   (void) box(window, 0, 0);

   if (focused) {
      (void) wattron(window, A_BOLD);
      (void) mvwprintw(window, 0, 2, " %s * ", title);
      (void) wattroff(window, A_BOLD);
   } else {
      (void) mvwprintw(window, 0, 2, " %s ", title);
   }
}

static void raw_gadget_tui_draw_footer(raw_gadget_tui_view_t const *view) {
   int const row = LINES - 2;

   (void) mvhline(row - 1, 1, ACS_HLINE, COLS - 2);

   if (view->page == RAW_GADGET_TUI_PAGE_BOARD) {
      (void) mvprintw(row,
                      2,
                      "F1 Help  F2 Board  F3 Log  F10 Hide  Tab Focus  "
                      "Space Pulse  T Toggle");
   } else if (view->page == RAW_GADGET_TUI_PAGE_LOG) {
      (void) mvprintw(row,
                      2,
                      "F1 Help  F2 Board  F3 Log  F10 Hide  "
                      "Up/Down  PgUp/PgDn  Home/End");
   } else {
      (void) mvprintw(row, 2, "F1 Help   F2 Board   F3 Log   F10 Hide");
   }
}

static void raw_gadget_tui_draw_help(raw_gadget_tui_view_t const *view) {
   (void) box(stdscr, 0, 0);
   (void) mvprintw(0, 2, " Help ");

   (void) mvprintw(2, 3, "Pages");
   (void) mvprintw(3, 5, "F1             Help");
   (void) mvprintw(4, 5, "F2             Board");
   (void) mvprintw(5, 5, "F3             Log");
   (void) mvprintw(6, 5, "F10            Hide or restore the TUI");

   (void) mvprintw(8, 3, "Board");
   (void) mvprintw(9, 5, "Tab            Change focus");
   (void) mvprintw(10, 5, "Arrow keys     Select button or edit input");
   (void) mvprintw(11, 5, "Space          Generate a button pulse");
   (void) mvprintw(12, 5, "T              Toggle selected button");
   (void) mvprintw(13, 5, "Enter          Submit UART input line");

   (void) mvprintw(15, 3, "UART output");
   (void) mvprintw(16, 5, "Up / Down      Scroll one line");
   (void) mvprintw(17, 5, "Page Up/Down   Scroll one page");
   (void) mvprintw(18, 5, "Home           Oldest available output");
   (void) mvprintw(19, 5, "End            Latest output and follow mode");

   (void) mvprintw(21, 3, "The same scrolling keys apply to the Log page.");

   raw_gadget_tui_draw_footer(view);
}

static void raw_gadget_tui_draw_indicator(WINDOW *window,
                                          int row,
                                          int column,
                                          uint8_t index,
                                          bool selected,
                                          bool active) {
   if (selected) {
      (void) mvwaddch(window, row, column, '[');
      (void) mvwaddch(window, row, column + 3, ']');
   } else {
      (void) mvwaddch(window, row, column, ' ');
      (void) mvwaddch(window, row, column + 3, ' ');
   }

   if (active) {
      (void) wattron(window, A_REVERSE);
   }

   (void) mvwprintw(window, row, column + 1, "%02u", (unsigned int) index);

   if (active) {
      (void) wattroff(window, A_REVERSE);
   }
}

static void raw_gadget_tui_draw_buttons(WINDOW *window,
                                        raw_gadget_tui_view_t const *view,
                                        uint32_t states) {
   for (uint8_t button = 0; button < RAW_GADGET_TUI_IO_COUNT; ++button) {
      int const row = 1 + (int) (button / RAW_GADGET_TUI_INDICATOR_COLUMNS);
      int const column =
         2 + ((int) (button % RAW_GADGET_TUI_INDICATOR_COLUMNS) *
              RAW_GADGET_TUI_INDICATOR_CELL_WIDTH);
      uint32_t const mask = UINT32_C(1) << button;
      bool const selected =
         (view->focus == RAW_GADGET_TUI_FOCUS_BUTTONS) &&
         (button == view->selected_button);

      raw_gadget_tui_draw_indicator(window,
                                    row,
                                    column,
                                    button,
                                    selected,
                                    (states & mask) != 0u);
   }
}

static void raw_gadget_tui_draw_leds(WINDOW *window, uint32_t states) {
   for (uint8_t led = 0; led < RAW_GADGET_TUI_IO_COUNT; ++led) {
      int const row = 1 + (int) (led / RAW_GADGET_TUI_INDICATOR_COLUMNS);
      int const column =
         2 + ((int) (led % RAW_GADGET_TUI_INDICATOR_COLUMNS) *
              RAW_GADGET_TUI_INDICATOR_CELL_WIDTH);
      uint32_t const mask = UINT32_C(1) << led;

      raw_gadget_tui_draw_indicator(window,
                                    row,
                                    column,
                                    led,
                                    false,
                                    (states & mask) != 0u);
   }
}

static size_t raw_gadget_tui_text_line_count(uint8_t const *data,
                                             size_t length,
                                             size_t columns) {
   size_t lines = 1u;
   size_t column = 0u;

   if (columns == 0u) {
      return 0u;
   }

   for (size_t index = 0; index < length; ++index) {
      uint8_t const value = data[index];

      if (value == '\n') {
         ++lines;
         column = 0u;
      } else if (value == '\r') {
         column = 0u;
      } else {
         size_t width = value == '\t' ? 4u - (column % 4u) : 1u;

         while (width > 0u) {
            size_t const remaining = columns - column;
            size_t const advance = width < remaining ? width : remaining;

            column += advance;
            width -= advance;

            if (column == columns) {
               ++lines;
               column = 0u;
            }
         }
      }
   }

   return lines;
}

static size_t raw_gadget_tui_text_find_line(uint8_t const *data,
                                            size_t length,
                                            size_t columns,
                                            size_t target_line) {
   size_t line = 0u;
   size_t column = 0u;

   if ((columns == 0u) || (target_line == 0u)) {
      return 0u;
   }

   for (size_t index = 0; index < length; ++index) {
      uint8_t const value = data[index];

      if (value == '\n') {
         ++line;
         column = 0u;
         if (line == target_line) {
            return index + 1u;
         }
      } else if (value == '\r') {
         column = 0u;
      } else {
         size_t width = value == '\t' ? 4u - (column % 4u) : 1u;

         while (width > 0u) {
            size_t const remaining = columns - column;
            size_t const advance = width < remaining ? width : remaining;

            column += advance;
            width -= advance;

            if (column == columns) {
               ++line;
               column = 0u;
               if (line == target_line) {
                  return index + 1u;
               }
            }
         }
      }
   }

   return length;
}

static void raw_gadget_tui_text_view_limit(
   raw_gadget_tui_text_view_t *view,
   size_t total_lines,
   size_t visible_rows) {
   size_t const maximum_scroll =
      total_lines > visible_rows ? total_lines - visible_rows : 0u;

   if (view->follow) {
      view->scroll_lines = 0u;
   } else if (view->scroll_lines > maximum_scroll) {
      view->scroll_lines = maximum_scroll;
   }
}

static void raw_gadget_tui_draw_text_view(
   WINDOW *window,
   raw_gadget_tui_text_view_t *view,
   uint8_t const *data,
   size_t length) {
   int rows;
   int columns;
   size_t visible_rows;
   size_t visible_columns;
   size_t total_lines;
   size_t maximum_scroll;
   size_t first_line;
   size_t start;
   int row = 1;
   int column = 1;

   getmaxyx(window, rows, columns);

   if ((rows <= 2) || (columns <= 2)) {
      return;
   }

   visible_rows = (size_t) (rows - 2);
   visible_columns = (size_t) (columns - 2);
   total_lines =
      raw_gadget_tui_text_line_count(data, length, visible_columns);

   raw_gadget_tui_text_view_limit(view, total_lines, visible_rows);

   maximum_scroll =
      total_lines > visible_rows ? total_lines - visible_rows : 0u;
   first_line = maximum_scroll - view->scroll_lines;
   start =
      raw_gadget_tui_text_find_line(data, length, visible_columns, first_line);

   for (size_t index = start; (index < length) && (row < rows - 1); ++index) {
      uint8_t value = data[index];

      if (value == '\r') {
         column = 1;
         continue;
      }

      if (value == '\n') {
         ++row;
         column = 1;
         continue;
      }

      if (value == '\t') {
         int spaces = 4 - ((column - 1) % 4);

         while ((spaces > 0) && (row < rows - 1)) {
            (void) mvwaddch(window, row, column, ' ');
            ++column;
            --spaces;

            if (column >= columns - 1) {
               ++row;
               column = 1;
            }
         }

         continue;
      }

      if (!isprint((int) value)) {
         value = '.';
      }

      (void) mvwaddch(window, row, column, (chtype) value);
      ++column;

      if (column >= columns - 1) {
         ++row;
         column = 1;
      }
   }
}

static void raw_gadget_tui_draw_uart_input(WINDOW *window,
                                           raw_gadget_tui_view_t const *view) {
   int rows;
   int columns;
   size_t available;
   size_t first = 0u;
   size_t cursor_column;

   getmaxyx(window, rows, columns);
   (void) rows;

   if (columns <= 5) {
      return;
   }

   available = (size_t) (columns - 5);

   if (view->uart_input_cursor > available) {
      first = view->uart_input_cursor - available;
   }

   (void) mvwprintw(window,
                    1,
                    2,
                    "> %.*s",
                    (int) available,
                    view->uart_input + first);

   if (view->focus == RAW_GADGET_TUI_FOCUS_UART_INPUT) {
      cursor_column = 4u + view->uart_input_cursor - first;
      if (cursor_column < (size_t) (columns - 1)) {
         (void) wmove(window, 1, (int) cursor_column);
      }
   }
}

static void raw_gadget_tui_draw_board(
   raw_gadget_tui_windows_t *windows,
   raw_gadget_tui_view_t *view,
   raw_gadget_tui_snapshot_t const *snapshot) {
   char uart_output_title[64];

   (void) erase();
   (void) box(stdscr, 0, 0);
   (void) mvprintw(0, 2, " Board ");

   raw_gadget_tui_draw_frame(
      windows->buttons,
      "Buttons",
      view->focus == RAW_GADGET_TUI_FOCUS_BUTTONS);
   raw_gadget_tui_draw_buttons(windows->buttons,
                               view,
                               snapshot->button_states);

   raw_gadget_tui_draw_frame(windows->leds, "LEDs", false);
   raw_gadget_tui_draw_leds(windows->leds, snapshot->led_states);

   raw_gadget_tui_draw_frame(
      windows->uart_input,
      "UART input",
      view->focus == RAW_GADGET_TUI_FOCUS_UART_INPUT);
   raw_gadget_tui_draw_uart_input(windows->uart_input, view);

   if (view->uart_output_view.follow) {
      (void) snprintf(uart_output_title,
                      sizeof(uart_output_title),
                      "UART output [FOLLOW]");
   } else {
      (void) snprintf(uart_output_title,
                      sizeof(uart_output_title),
                      "UART output [-%zu lines]",
                      view->uart_output_view.scroll_lines);
   }

   raw_gadget_tui_draw_frame(
      windows->uart_output,
      uart_output_title,
      view->focus == RAW_GADGET_TUI_FOCUS_UART_OUTPUT);
   raw_gadget_tui_draw_text_view(windows->uart_output,
                                 &view->uart_output_view,
                                 snapshot->uart_tx,
                                 snapshot->uart_tx_length);

   raw_gadget_tui_draw_footer(view);

   (void) wnoutrefresh(stdscr);
   (void) wnoutrefresh(windows->buttons);
   (void) wnoutrefresh(windows->leds);
   (void) wnoutrefresh(windows->uart_input);
   (void) wnoutrefresh(windows->uart_output);
   (void) doupdate();

   if (view->focus == RAW_GADGET_TUI_FOCUS_UART_INPUT) {
      (void) curs_set(1);
   } else {
      (void) curs_set(0);
   }
}

static void raw_gadget_tui_draw_log(
   raw_gadget_tui_windows_t *windows,
   raw_gadget_tui_view_t *view,
   raw_gadget_tui_snapshot_t const *snapshot) {
   char title[64];

   (void) erase();
   (void) box(stdscr, 0, 0);
   (void) mvprintw(0, 2, " Log ");

   if (view->log_view.follow) {
      (void) snprintf(title, sizeof(title), "TinyUSB log [FOLLOW]");
   } else {
      (void) snprintf(title,
                      sizeof(title),
                      "TinyUSB log [-%zu lines]",
                      view->log_view.scroll_lines);
   }

   raw_gadget_tui_draw_frame(windows->log, title, true);
   raw_gadget_tui_draw_text_view(windows->log,
                                 &view->log_view,
                                 snapshot->log,
                                 snapshot->log_length);
   raw_gadget_tui_draw_footer(view);

   (void) wnoutrefresh(stdscr);
   (void) wnoutrefresh(windows->log);
   (void) doupdate();
   (void) curs_set(0);
}

static void raw_gadget_tui_draw_hidden(void) {
   (void) erase();
   (void) mvprintw(0, 0, "Raw Gadget TUI hidden. Press F10 to restore.");
   (void) refresh();
   (void) curs_set(0);
}

static void raw_gadget_tui_draw_too_small(void) {
   (void) erase();
   (void) mvprintw(0,
                   0,
                   "Terminal too small: need at least %dx%d, current %dx%d",
                   RAW_GADGET_TUI_MINIMUM_COLUMNS,
                   RAW_GADGET_TUI_MINIMUM_ROWS,
                   COLS,
                   LINES);
   (void) refresh();
   (void) curs_set(0);
}

static void raw_gadget_tui_draw(raw_gadget_tui_windows_t *windows,
                                raw_gadget_tui_view_t *view,
                                raw_gadget_tui_snapshot_t const *snapshot) {
   if (view->hidden) {
      raw_gadget_tui_draw_hidden();
      return;
   }

   if ((COLS < RAW_GADGET_TUI_MINIMUM_COLUMNS) ||
       (LINES < RAW_GADGET_TUI_MINIMUM_ROWS)) {
      raw_gadget_tui_draw_too_small();
      return;
   }

   switch (view->page) {
      case RAW_GADGET_TUI_PAGE_HELP:
         (void) erase();
         raw_gadget_tui_draw_help(view);
         (void) refresh();
         (void) curs_set(0);
         break;

      case RAW_GADGET_TUI_PAGE_LOG:
         raw_gadget_tui_draw_log(windows, view, snapshot);
         break;

      case RAW_GADGET_TUI_PAGE_BOARD:
      default:
         raw_gadget_tui_draw_board(windows, view, snapshot);
         break;
   }
}

static void raw_gadget_tui_select_left(raw_gadget_tui_view_t *view) {
   if ((view->selected_button % RAW_GADGET_TUI_INDICATOR_COLUMNS) > 0u) {
      --view->selected_button;
   }
}

static void raw_gadget_tui_select_right(raw_gadget_tui_view_t *view) {
   if (((view->selected_button % RAW_GADGET_TUI_INDICATOR_COLUMNS) <
        (RAW_GADGET_TUI_INDICATOR_COLUMNS - 1u)) &&
       (view->selected_button < (RAW_GADGET_TUI_IO_COUNT - 1u))) {
      ++view->selected_button;
   }
}

static void raw_gadget_tui_select_up(raw_gadget_tui_view_t *view) {
   if (view->selected_button >= RAW_GADGET_TUI_INDICATOR_COLUMNS) {
      view->selected_button -= RAW_GADGET_TUI_INDICATOR_COLUMNS;
   }
}

static void raw_gadget_tui_select_down(raw_gadget_tui_view_t *view) {
   if (view->selected_button <
       (RAW_GADGET_TUI_IO_COUNT - RAW_GADGET_TUI_INDICATOR_COLUMNS)) {
      view->selected_button += RAW_GADGET_TUI_INDICATOR_COLUMNS;
   }
}

static void raw_gadget_tui_focus_next(raw_gadget_tui_view_t *view) {
   switch (view->focus) {
      case RAW_GADGET_TUI_FOCUS_BUTTONS:
         view->focus = RAW_GADGET_TUI_FOCUS_UART_INPUT;
         break;

      case RAW_GADGET_TUI_FOCUS_UART_INPUT:
         view->focus = RAW_GADGET_TUI_FOCUS_UART_OUTPUT;
         break;

      case RAW_GADGET_TUI_FOCUS_UART_OUTPUT:
      default:
         view->focus = RAW_GADGET_TUI_FOCUS_BUTTONS;
         break;
   }
}

static void raw_gadget_tui_focus_previous(raw_gadget_tui_view_t *view) {
   switch (view->focus) {
      case RAW_GADGET_TUI_FOCUS_BUTTONS:
         view->focus = RAW_GADGET_TUI_FOCUS_UART_OUTPUT;
         break;

      case RAW_GADGET_TUI_FOCUS_UART_INPUT:
         view->focus = RAW_GADGET_TUI_FOCUS_BUTTONS;
         break;

      case RAW_GADGET_TUI_FOCUS_UART_OUTPUT:
      default:
         view->focus = RAW_GADGET_TUI_FOCUS_UART_INPUT;
         break;
   }
}

static void raw_gadget_tui_handle_buttons_key(int key,
                                              raw_gadget_tui_view_t *view) {
   switch (key) {
      case KEY_LEFT:
         raw_gadget_tui_select_left(view);
         break;

      case KEY_RIGHT:
         raw_gadget_tui_select_right(view);
         break;

      case KEY_UP:
         raw_gadget_tui_select_up(view);
         break;

      case KEY_DOWN:
         raw_gadget_tui_select_down(view);
         break;

      case KEY_HOME:
         view->selected_button = 0u;
         break;

      case KEY_END:
         view->selected_button = RAW_GADGET_TUI_IO_COUNT - 1u;
         break;

      case ' ':
         raw_gadget_tui_button_pulse(view->selected_button);
         break;

      case 't':
      case 'T':
         raw_gadget_tui_button_toggle(view->selected_button);
         break;

      default:
         break;
   }
}

static void raw_gadget_tui_input_insert(raw_gadget_tui_view_t *view,
                                        char character) {
   if (view->uart_input_length >= RAW_GADGET_TUI_UART_INPUT_LENGTH - 1u) {
      return;
   }

   memmove(view->uart_input + view->uart_input_cursor + 1u,
           view->uart_input + view->uart_input_cursor,
           view->uart_input_length - view->uart_input_cursor + 1u);

   view->uart_input[view->uart_input_cursor] = character;
   ++view->uart_input_cursor;
   ++view->uart_input_length;
}

static void raw_gadget_tui_input_backspace(raw_gadget_tui_view_t *view) {
   if (view->uart_input_cursor == 0u) {
      return;
   }

   memmove(view->uart_input + view->uart_input_cursor - 1u,
           view->uart_input + view->uart_input_cursor,
           view->uart_input_length - view->uart_input_cursor + 1u);

   --view->uart_input_cursor;
   --view->uart_input_length;
}

static void raw_gadget_tui_input_delete(raw_gadget_tui_view_t *view) {
   if (view->uart_input_cursor >= view->uart_input_length) {
      return;
   }

   memmove(view->uart_input + view->uart_input_cursor,
           view->uart_input + view->uart_input_cursor + 1u,
           view->uart_input_length - view->uart_input_cursor);

   --view->uart_input_length;
}

static void raw_gadget_tui_handle_uart_input_key(
   int key,
   raw_gadget_tui_view_t *view) {
   switch (key) {
      case KEY_LEFT:
         if (view->uart_input_cursor > 0u) {
            --view->uart_input_cursor;
         }
         break;

      case KEY_RIGHT:
         if (view->uart_input_cursor < view->uart_input_length) {
            ++view->uart_input_cursor;
         }
         break;

      case KEY_HOME:
         view->uart_input_cursor = 0u;
         break;

      case KEY_END:
         view->uart_input_cursor = view->uart_input_length;
         break;

      case KEY_BACKSPACE:
      case 0x7f:
      case '\b':
         raw_gadget_tui_input_backspace(view);
         break;

      case KEY_DC:
         raw_gadget_tui_input_delete(view);
         break;

      case KEY_ENTER:
      case '\n':
      case '\r':
         raw_gadget_tui_uart_submit(view);
         break;

      default:
         if ((key >= 0x20) && (key <= 0x7e)) {
            raw_gadget_tui_input_insert(view, (char) key);
         }
         break;
   }
}

static void raw_gadget_tui_handle_uart_output_key(
   int key,
   raw_gadget_tui_text_view_t *view,
   size_t visible_rows) {
   size_t const page_lines = visible_rows > 1u ? visible_rows - 1u : 1u;

   switch (key) {
      case KEY_UP:
         view->follow = false;
         ++view->scroll_lines;
         break;

      case KEY_DOWN:
         if (view->scroll_lines > 0u) {
            --view->scroll_lines;
         }
         if (view->scroll_lines == 0u) {
            view->follow = true;
         }
         break;

      case KEY_PPAGE:
         view->follow = false;
         view->scroll_lines += page_lines;
         break;

      case KEY_NPAGE:
         if (view->scroll_lines > page_lines) {
            view->scroll_lines -= page_lines;
         } else {
            view->scroll_lines = 0u;
            view->follow = true;
         }
         break;

      case KEY_HOME:
         view->follow = false;
         view->scroll_lines = SIZE_MAX;
         break;

      case KEY_END:
         view->scroll_lines = 0u;
         view->follow = true;
         break;

      default:
         break;
   }
}

static void raw_gadget_tui_handle_board_key(
   int key,
   raw_gadget_tui_view_t *view,
   raw_gadget_tui_windows_t const *windows) {
   if (key == '\t') {
      raw_gadget_tui_focus_next(view);
      return;
   }

   if (key == KEY_BTAB) {
      raw_gadget_tui_focus_previous(view);
      return;
   }

   switch (view->focus) {
      case RAW_GADGET_TUI_FOCUS_BUTTONS:
         raw_gadget_tui_handle_buttons_key(key, view);
         break;

      case RAW_GADGET_TUI_FOCUS_UART_INPUT:
         raw_gadget_tui_handle_uart_input_key(key, view);
         break;

      case RAW_GADGET_TUI_FOCUS_UART_OUTPUT: {
         int rows;
         int columns;

         getmaxyx(windows->uart_output, rows, columns);
         (void) columns;
         raw_gadget_tui_handle_uart_output_key(
            key,
            &view->uart_output_view,
            rows > 2 ? (size_t) (rows - 2) : 1u);
         break;
      }

      default:
         break;
   }
}

static void raw_gadget_tui_handle_log_key(
   int key,
   raw_gadget_tui_view_t *view,
   raw_gadget_tui_windows_t const *windows) {
   int rows;
   int columns;

   getmaxyx(windows->log, rows, columns);
   (void) columns;

   raw_gadget_tui_handle_uart_output_key(
      key,
      &view->log_view,
      rows > 2 ? (size_t) (rows - 2) : 1u);
}

static void raw_gadget_tui_handle_key(
   int key,
   raw_gadget_tui_view_t *view,
   raw_gadget_tui_windows_t const *windows) {
   switch (key) {
      case KEY_F(1):
         view->page = RAW_GADGET_TUI_PAGE_HELP;
         view->hidden = false;
         return;

      case KEY_F(2):
         view->page = RAW_GADGET_TUI_PAGE_BOARD;
         view->hidden = false;
         return;

      case KEY_F(3):
         view->page = RAW_GADGET_TUI_PAGE_LOG;
         view->hidden = false;
         return;

      case KEY_F(10):
         view->hidden = !view->hidden;
         return;

      case KEY_RESIZE:
         return;

      default:
         break;
   }

   if (view->hidden || !windows->valid) {
      return;
   }

   if (view->page == RAW_GADGET_TUI_PAGE_BOARD) {
      raw_gadget_tui_handle_board_key(key, view, windows);
   } else if (view->page == RAW_GADGET_TUI_PAGE_LOG) {
      raw_gadget_tui_handle_log_key(key, view, windows);
   }
}

static void *raw_gadget_tui_thread(void *argument) {
   raw_gadget_tui_view_t view = {
      .page = RAW_GADGET_TUI_PAGE_BOARD,
      .focus = RAW_GADGET_TUI_FOCUS_BUTTONS,
      .uart_output_view = {
         .scroll_lines = 0u,
         .follow = true,
      },
      .log_view = {
         .scroll_lines = 0u,
         .follow = true,
      },
      .selected_button = 0u,
      .hidden = false,
   };
   raw_gadget_tui_windows_t windows = {0};
   raw_gadget_tui_snapshot_t *snapshot;
   WINDOW *screen;

   (void) argument;

   snapshot = malloc(sizeof(*snapshot));
   if (snapshot == NULL) {
      raw_gadget_tui_signal_startup(RAW_GADGET_TUI_STATE_FAILED);
      return NULL;
   }

   screen = initscr();
   if (screen == NULL) {
      free(snapshot);
      raw_gadget_tui_signal_startup(RAW_GADGET_TUI_STATE_FAILED);
      return NULL;
   }

   if ((cbreak() == ERR) || (noecho() == ERR) ||
       (keypad(stdscr, true) == ERR) || (nodelay(stdscr, true) == ERR)) {
      (void) endwin();
      free(snapshot);
      raw_gadget_tui_signal_startup(RAW_GADGET_TUI_STATE_FAILED);
      return NULL;
   }

   (void) curs_set(0);
   (void) raw_gadget_tui_windows_create(&windows);
   raw_gadget_tui_signal_startup(RAW_GADGET_TUI_STATE_RUNNING);

   while (atomic_load_explicit(&raw_gadget_tui_context.run_requested,
                               memory_order_acquire)) {
      int key;

      if (raw_gadget_tui_windows_need_recreate(&windows)) {
         (void) raw_gadget_tui_windows_create(&windows);
      }

      while ((key = getch()) != ERR) {
         if (key == KEY_RESIZE) {
            (void) raw_gadget_tui_windows_create(&windows);
         } else {
            raw_gadget_tui_handle_key(key, &view, &windows);
         }
      }

      raw_gadget_tui_take_snapshot(snapshot);
      raw_gadget_tui_draw(&windows, &view, snapshot);
      raw_gadget_tui_sleep();
   }

   raw_gadget_tui_windows_destroy(&windows);
   (void) erase();
   (void) refresh();
   (void) endwin();
   free(snapshot);

   return NULL;
}

static void raw_gadget_tui_atexit(void) {
   raw_gadget_tui_deinit();
}

bool raw_gadget_tui_init(void) {
   int result;
   bool initialized;

   (void) pthread_mutex_lock(&raw_gadget_tui_context.lifecycle_mutex);

   if (raw_gadget_tui_context.state == RAW_GADGET_TUI_STATE_RUNNING) {
      (void) pthread_mutex_unlock(&raw_gadget_tui_context.lifecycle_mutex);
      return true;
   }

   if (raw_gadget_tui_context.thread_created) {
      (void) pthread_mutex_unlock(&raw_gadget_tui_context.lifecycle_mutex);
      return false;
   }

   raw_gadget_tui_context.state = RAW_GADGET_TUI_STATE_STARTING;
   atomic_store_explicit(&raw_gadget_tui_context.run_requested,
                         true,
                         memory_order_release);

   result = pthread_create(&raw_gadget_tui_context.thread,
                           NULL,
                           raw_gadget_tui_thread,
                           NULL);
   if (result != 0) {
      atomic_store_explicit(&raw_gadget_tui_context.run_requested,
                            false,
                            memory_order_release);
      raw_gadget_tui_context.state = RAW_GADGET_TUI_STATE_FAILED;
      (void) pthread_mutex_unlock(&raw_gadget_tui_context.lifecycle_mutex);
      return false;
   }

   raw_gadget_tui_context.thread_created = true;

   while (raw_gadget_tui_context.state == RAW_GADGET_TUI_STATE_STARTING) {
      (void) pthread_cond_wait(&raw_gadget_tui_context.condition,
                               &raw_gadget_tui_context.lifecycle_mutex);
   }

   initialized = raw_gadget_tui_context.state == RAW_GADGET_TUI_STATE_RUNNING;

   if (initialized && !raw_gadget_tui_context.atexit_registered) {
      if (atexit(raw_gadget_tui_atexit) == 0) {
         raw_gadget_tui_context.atexit_registered = true;
      }
   }

   (void) pthread_mutex_unlock(&raw_gadget_tui_context.lifecycle_mutex);

   if (!initialized) {
      raw_gadget_tui_deinit();
   }

   return initialized;
}

void raw_gadget_tui_deinit(void) {
   pthread_t thread;
   bool join_required = false;

   (void) pthread_mutex_lock(&raw_gadget_tui_context.lifecycle_mutex);

   if (raw_gadget_tui_context.thread_created) {
      atomic_store_explicit(&raw_gadget_tui_context.run_requested,
                            false,
                            memory_order_release);
      thread = raw_gadget_tui_context.thread;
      join_required = true;
   }

   (void) pthread_mutex_unlock(&raw_gadget_tui_context.lifecycle_mutex);

   if (join_required) {
      (void) pthread_join(thread, NULL);
   }

   (void) pthread_mutex_lock(&raw_gadget_tui_context.lifecycle_mutex);
   raw_gadget_tui_context.thread_created = false;
   raw_gadget_tui_context.state = RAW_GADGET_TUI_STATE_STOPPED;
   (void) pthread_mutex_unlock(&raw_gadget_tui_context.lifecycle_mutex);
}

uint32_t raw_gadget_tui_button_read(void) {
   uint32_t states;

   (void) pthread_mutex_lock(&raw_gadget_tui_context.io_mutex);
   states =
      raw_gadget_tui_context.button_events | raw_gadget_tui_context.button_toggles;
   raw_gadget_tui_context.button_events = 0u;
   (void) pthread_mutex_unlock(&raw_gadget_tui_context.io_mutex);

   return states;
}

void raw_gadget_tui_led_write(uint8_t index, bool state) {
   uint32_t mask;

   if (index >= RAW_GADGET_TUI_IO_COUNT) {
      return;
   }

   mask = UINT32_C(1) << index;

   (void) pthread_mutex_lock(&raw_gadget_tui_context.io_mutex);

   if (state) {
      raw_gadget_tui_context.led_states |= mask;
   } else {
      raw_gadget_tui_context.led_states &= ~mask;
   }

   (void) pthread_mutex_unlock(&raw_gadget_tui_context.io_mutex);
}

void raw_gadget_tui_led_write_all(uint32_t states) {
   (void) pthread_mutex_lock(&raw_gadget_tui_context.io_mutex);
   raw_gadget_tui_context.led_states = states;
   (void) pthread_mutex_unlock(&raw_gadget_tui_context.io_mutex);
}

uint32_t raw_gadget_tui_led_read_all(void) {
   uint32_t states;

   (void) pthread_mutex_lock(&raw_gadget_tui_context.io_mutex);
   states = raw_gadget_tui_context.led_states;
   (void) pthread_mutex_unlock(&raw_gadget_tui_context.io_mutex);

   return states;
}

int raw_gadget_tui_uart_read(uint8_t *buffer, int length) {
   size_t count;

   if ((buffer == NULL) || (length <= 0)) {
      return -1;
   }

   (void) pthread_mutex_lock(&raw_gadget_tui_context.io_mutex);
   count = raw_gadget_tui_fifo_read(&raw_gadget_tui_context.uart_rx,
                                    buffer,
                                    (size_t) length);
   (void) pthread_mutex_unlock(&raw_gadget_tui_context.io_mutex);

   return count > 0u ? (int) count : -1;
}

int raw_gadget_tui_uart_write(void const *buffer, int length) {
   if ((buffer == NULL) || (length <= 0)) {
      return -1;
   }

   (void) pthread_mutex_lock(&raw_gadget_tui_context.io_mutex);
   raw_gadget_tui_ring_write(&raw_gadget_tui_context.uart_tx,
                             buffer,
                             (size_t) length);
   (void) pthread_mutex_unlock(&raw_gadget_tui_context.io_mutex);

   return length;
}
