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

#include <stdlib.h>
#include <time.h>

raw_gadget_tui_context_t raw_gadget_tui_context = {
   .lifecycle_mutex = PTHREAD_MUTEX_INITIALIZER,
   .condition = PTHREAD_COND_INITIALIZER,
   .io_mutex = PTHREAD_MUTEX_INITIALIZER,
   .run_requested = false,
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
#ifdef KEY_RESIZE
         if (key == KEY_RESIZE) {
            (void) raw_gadget_tui_windows_create(&windows);
         } else
#endif
         {
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

   if ((raw_gadget_tui_context.state == RAW_GADGET_TUI_STATE_STARTING) ||
       (raw_gadget_tui_context.state == RAW_GADGET_TUI_STATE_STOPPING) ||
       raw_gadget_tui_context.thread_created) {
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

   if (raw_gadget_tui_context.state == RAW_GADGET_TUI_STATE_STOPPING) {
      (void) pthread_mutex_unlock(&raw_gadget_tui_context.lifecycle_mutex);
      return;
   }

   if (raw_gadget_tui_context.thread_created) {
      raw_gadget_tui_context.state = RAW_GADGET_TUI_STATE_STOPPING;
      raw_gadget_tui_context.thread_created = false;
      atomic_store_explicit(&raw_gadget_tui_context.run_requested,
                            false,
                            memory_order_release);
      thread = raw_gadget_tui_context.thread;
      join_required = true;
   } else {
      raw_gadget_tui_context.state = RAW_GADGET_TUI_STATE_STOPPED;
   }

   (void) pthread_mutex_unlock(&raw_gadget_tui_context.lifecycle_mutex);

   if (join_required) {
      (void) pthread_join(thread, NULL);

      (void) pthread_mutex_lock(&raw_gadget_tui_context.lifecycle_mutex);
      raw_gadget_tui_context.state = RAW_GADGET_TUI_STATE_STOPPED;
      (void) pthread_mutex_unlock(&raw_gadget_tui_context.lifecycle_mutex);
   }
}

