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


#include "raw_gadget_tui_internal.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

void raw_gadget_tui_windows_destroy(raw_gadget_tui_windows_t *windows) {
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

bool raw_gadget_tui_windows_create(raw_gadget_tui_windows_t *windows) {
   int const content_width = COLS - 4;
   int const buttons_height = (int) RAW_GADGET_TUI_INDICATOR_ROWS + 2;
   int const leds_height = (int) RAW_GADGET_TUI_INDICATOR_ROWS + 2;
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

bool raw_gadget_tui_windows_need_recreate(
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

void raw_gadget_tui_draw(raw_gadget_tui_windows_t *windows,
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
