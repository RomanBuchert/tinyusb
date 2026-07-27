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
#include <string.h>

static void raw_gadget_tui_select_left(raw_gadget_tui_view_t *view) {
   if ((view->selected_button % RAW_GADGET_TUI_INDICATOR_COLUMNS) != 0u) {
      --view->selected_button;
   }
}

static void raw_gadget_tui_select_right(raw_gadget_tui_view_t *view) {
   uint32_t const next = (uint32_t) view->selected_button + 1u;

   if (((next % RAW_GADGET_TUI_INDICATOR_COLUMNS) != 0u) &&
       (next < RAW_GADGET_TUI_IO_COUNT)) {
      view->selected_button = (uint8_t) next;
   }
}

static void raw_gadget_tui_select_up(raw_gadget_tui_view_t *view) {
   if (view->selected_button >= RAW_GADGET_TUI_INDICATOR_COLUMNS) {
      view->selected_button -= RAW_GADGET_TUI_INDICATOR_COLUMNS;
   }
}

static void raw_gadget_tui_select_down(raw_gadget_tui_view_t *view) {
   uint32_t const next =
      (uint32_t) view->selected_button + RAW_GADGET_TUI_INDICATOR_COLUMNS;

   if (next < RAW_GADGET_TUI_IO_COUNT) {
      view->selected_button = (uint8_t) next;
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

void raw_gadget_tui_handle_key(
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
