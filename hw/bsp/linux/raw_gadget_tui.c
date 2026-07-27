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

#include <curses.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <time.h>

#define RAW_GADGET_TUI_MINIMUM_COLUMNS 80
#define RAW_GADGET_TUI_MINIMUM_ROWS    24
#define RAW_GADGET_TUI_REFRESH_MS      20

#define RAW_GADGET_TUI_INDICATOR_COLUMNS    8u
#define RAW_GADGET_TUI_INDICATOR_CELL_WIDTH 8

typedef enum {
   RAW_GADGET_TUI_PAGE_HELP,
   RAW_GADGET_TUI_PAGE_BOARD,
   RAW_GADGET_TUI_PAGE_LOG,
} raw_gadget_tui_page_t;

typedef enum {
   RAW_GADGET_TUI_STATE_STOPPED,
   RAW_GADGET_TUI_STATE_STARTING,
   RAW_GADGET_TUI_STATE_RUNNING,
   RAW_GADGET_TUI_STATE_FAILED,
} raw_gadget_tui_state_t;

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
   bool thread_created;
   bool atexit_registered;
} raw_gadget_tui_context_t;

typedef struct {
   raw_gadget_tui_page_t page;
   uint8_t selected_button;
   bool hidden;
} raw_gadget_tui_view_t;

typedef struct {
   uint32_t button_states;
   uint32_t led_states;
} raw_gadget_tui_snapshot_t;

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

static void raw_gadget_tui_take_snapshot(raw_gadget_tui_snapshot_t *snapshot) {
   (void) pthread_mutex_lock(&raw_gadget_tui_context.io_mutex);
   snapshot->button_states =
      raw_gadget_tui_context.button_events | raw_gadget_tui_context.button_toggles;
   snapshot->led_states = raw_gadget_tui_context.led_states;
   (void) pthread_mutex_unlock(&raw_gadget_tui_context.io_mutex);
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

static void raw_gadget_tui_draw_frame(WINDOW *window, char const *title) {
   (void) box(window, 0, 0);
   (void) mvwprintw(window, 0, 2, " %s ", title);
}

static void raw_gadget_tui_draw_footer(void) {
   int const row = LINES - 2;

   (void) mvhline(row - 1, 1, ACS_HLINE, COLS - 2);
   (void) mvprintw(row,
                   2,
                   "F1 Help   F2 Board   F3 Log   F10 Hide   "
                   "Arrows Select   Space Pulse   T Toggle");
}

static void raw_gadget_tui_draw_help(void) {
   (void) box(stdscr, 0, 0);
   (void) mvprintw(0, 2, " Help ");

   (void) mvprintw(2, 3, "Pages");
   (void) mvprintw(3, 5, "F1          Help");
   (void) mvprintw(4, 5, "F2          Board");
   (void) mvprintw(5, 5, "F3          Log");
   (void) mvprintw(6, 5, "F10         Hide or restore the TUI");

   (void) mvprintw(8, 3, "Board controls");
   (void) mvprintw(9, 5, "Arrow keys  Select a button");
   (void) mvprintw(10, 5, "Home / End  Select first or last button");
   (void) mvprintw(11, 5, "Space       Generate one cached button event");
   (void) mvprintw(12, 5, "T           Toggle selected button state");

   (void) mvprintw(14, 3, "Display");
   (void) mvprintw(15, 5, "[NN]        Selected button");
   (void) mvprintw(16, 5, "Inverse     Active button or LED");

   raw_gadget_tui_draw_footer();
}

static void raw_gadget_tui_delete_window(WINDOW **window) {
   if (*window != NULL) {
      (void) delwin(*window);
      *window = NULL;
   }
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
                                        uint8_t selected_button,
                                        uint32_t states) {
   for (uint8_t button = 0; button < RAW_GADGET_TUI_IO_COUNT; ++button) {
      int const row = 1 + (int) (button / RAW_GADGET_TUI_INDICATOR_COLUMNS);
      int const column =
         2 + ((int) (button % RAW_GADGET_TUI_INDICATOR_COLUMNS) *
              RAW_GADGET_TUI_INDICATOR_CELL_WIDTH);
      uint32_t const mask = UINT32_C(1) << button;

      raw_gadget_tui_draw_indicator(window,
                                    row,
                                    column,
                                    button,
                                    button == selected_button,
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

static void raw_gadget_tui_draw_board(raw_gadget_tui_view_t const *view,
                                      raw_gadget_tui_snapshot_t const *snapshot) {
   int const content_width = COLS - 4;
   int const buttons_height = 6;
   int const leds_height = 6;
   int const uart_input_height = 3;
   int const buttons_row = 1;
   int const leds_row = buttons_row + buttons_height + 1;
   int const uart_input_row = leds_row + leds_height + 1;
   int const uart_output_row = uart_input_row + uart_input_height + 1;
   int const uart_output_height = (LINES - 3) - uart_output_row;
   WINDOW *buttons_window = NULL;
   WINDOW *leds_window = NULL;
   WINDOW *uart_input_window = NULL;
   WINDOW *uart_output_window = NULL;

   (void) box(stdscr, 0, 0);
   (void) mvprintw(0, 2, " Board ");

   buttons_window = newwin(buttons_height, content_width, buttons_row, 2);
   leds_window = newwin(leds_height, content_width, leds_row, 2);
   uart_input_window = newwin(uart_input_height, content_width, uart_input_row, 2);
   uart_output_window = newwin(uart_output_height, content_width, uart_output_row, 2);

   if ((buttons_window == NULL) || (leds_window == NULL) ||
       (uart_input_window == NULL) || (uart_output_window == NULL)) {
      (void) mvprintw(2, 3, "Unable to create the Board windows.");
   } else {
      raw_gadget_tui_draw_frame(buttons_window, "Buttons");
      raw_gadget_tui_draw_buttons(buttons_window,
                                  view->selected_button,
                                  snapshot->button_states);

      raw_gadget_tui_draw_frame(leds_window, "LEDs");
      raw_gadget_tui_draw_leds(leds_window, snapshot->led_states);

      raw_gadget_tui_draw_frame(uart_input_window, "UART input");
      raw_gadget_tui_draw_frame(uart_output_window, "UART output");
   }

   raw_gadget_tui_draw_footer();
   (void) wnoutrefresh(stdscr);

   if ((buttons_window != NULL) && (leds_window != NULL) &&
       (uart_input_window != NULL) && (uart_output_window != NULL)) {
      (void) wnoutrefresh(buttons_window);
      (void) wnoutrefresh(leds_window);
      (void) wnoutrefresh(uart_input_window);
      (void) wnoutrefresh(uart_output_window);
   }

   (void) doupdate();

   raw_gadget_tui_delete_window(&buttons_window);
   raw_gadget_tui_delete_window(&leds_window);
   raw_gadget_tui_delete_window(&uart_input_window);
   raw_gadget_tui_delete_window(&uart_output_window);
}

static void raw_gadget_tui_draw_log(void) {
   int const window_height = LINES - 4;
   int const window_width = COLS - 4;
   WINDOW *log_window = NULL;

   (void) box(stdscr, 0, 0);
   (void) mvprintw(0, 2, " Log ");

   log_window = newwin(window_height, window_width, 1, 2);
   if (log_window == NULL) {
      (void) mvprintw(2, 3, "Unable to create the Log window.");
   } else {
      raw_gadget_tui_draw_frame(log_window, "TinyUSB log");
   }

   raw_gadget_tui_draw_footer();
   (void) wnoutrefresh(stdscr);

   if (log_window != NULL) {
      (void) wnoutrefresh(log_window);
   }

   (void) doupdate();

   raw_gadget_tui_delete_window(&log_window);
}

static void raw_gadget_tui_draw_hidden(void) {
   (void) erase();
   (void) mvprintw(0, 0, "Raw Gadget TUI hidden. Press F10 to restore.");
   (void) refresh();
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
}

static void raw_gadget_tui_draw(raw_gadget_tui_view_t const *view) {
   raw_gadget_tui_snapshot_t snapshot;

   if (view->hidden) {
      raw_gadget_tui_draw_hidden();
      return;
   }

   if ((COLS < RAW_GADGET_TUI_MINIMUM_COLUMNS) ||
       (LINES < RAW_GADGET_TUI_MINIMUM_ROWS)) {
      raw_gadget_tui_draw_too_small();
      return;
   }

   raw_gadget_tui_take_snapshot(&snapshot);
   (void) erase();

   switch (view->page) {
      case RAW_GADGET_TUI_PAGE_HELP:
         raw_gadget_tui_draw_help();
         (void) refresh();
         break;

      case RAW_GADGET_TUI_PAGE_LOG:
         raw_gadget_tui_draw_log();
         break;

      case RAW_GADGET_TUI_PAGE_BOARD:
      default:
         raw_gadget_tui_draw_board(view, &snapshot);
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

static void raw_gadget_tui_handle_board_key(int key,
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

static void raw_gadget_tui_handle_key(int key, raw_gadget_tui_view_t *view) {
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

   if (!view->hidden && (view->page == RAW_GADGET_TUI_PAGE_BOARD)) {
      raw_gadget_tui_handle_board_key(key, view);
   }
}

static void *raw_gadget_tui_thread(void *argument) {
   raw_gadget_tui_view_t view = {
      .page = RAW_GADGET_TUI_PAGE_BOARD,
      .selected_button = 0u,
      .hidden = false,
   };
   WINDOW *screen;

   (void) argument;

   screen = initscr();
   if (screen == NULL) {
      raw_gadget_tui_signal_startup(RAW_GADGET_TUI_STATE_FAILED);
      return NULL;
   }

   if ((cbreak() == ERR) || (noecho() == ERR) ||
       (keypad(stdscr, true) == ERR) || (nodelay(stdscr, true) == ERR)) {
      (void) endwin();
      raw_gadget_tui_signal_startup(RAW_GADGET_TUI_STATE_FAILED);
      return NULL;
   }

   (void) curs_set(0);
   raw_gadget_tui_signal_startup(RAW_GADGET_TUI_STATE_RUNNING);

   while (atomic_load_explicit(&raw_gadget_tui_context.run_requested,
                               memory_order_acquire)) {
      int key;

      while ((key = getch()) != ERR) {
         raw_gadget_tui_handle_key(key, &view);
      }

      raw_gadget_tui_draw(&view);
      raw_gadget_tui_sleep();
   }

   (void) erase();
   (void) refresh();
   (void) endwin();

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
