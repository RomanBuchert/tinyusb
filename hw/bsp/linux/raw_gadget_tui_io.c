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

#include <string.h>

static void raw_gadget_tui_fifo_write(raw_gadget_tui_fifo_t *fifo,
                                      uint8_t const *data,
                                      size_t length) {
   size_t available = RAW_GADGET_TUI_UART_RX_CAPACITY - fifo->count;
   size_t first_length;

   if (length > available) {
      length = available;
   }

   first_length = RAW_GADGET_TUI_UART_RX_CAPACITY - fifo->write_index;
   if (first_length > length) {
      first_length = length;
   }

   memcpy(fifo->data + fifo->write_index, data, first_length);
   memcpy(fifo->data, data + first_length, length - first_length);

   fifo->write_index =
      (fifo->write_index + length) % RAW_GADGET_TUI_UART_RX_CAPACITY;
   fifo->count += length;
}

static size_t raw_gadget_tui_fifo_read(raw_gadget_tui_fifo_t *fifo,
                                       uint8_t *data,
                                       size_t length) {
   size_t first_length;

   if (length > fifo->count) {
      length = fifo->count;
   }

   first_length = RAW_GADGET_TUI_UART_RX_CAPACITY - fifo->read_index;
   if (first_length > length) {
      first_length = length;
   }

   memcpy(data, fifo->data + fifo->read_index, first_length);
   memcpy(data + first_length, fifo->data, length - first_length);

   fifo->read_index =
      (fifo->read_index + length) % RAW_GADGET_TUI_UART_RX_CAPACITY;
   fifo->count -= length;

   return length;
}

static void raw_gadget_tui_ring_write(raw_gadget_tui_ring_t *ring,
                                      uint8_t const *data,
                                      size_t length) {
   size_t write_index;
   size_t first_length;
   size_t overflow;

   if (length >= RAW_GADGET_TUI_UART_TX_CAPACITY) {
      memcpy(ring->data,
             data + length - RAW_GADGET_TUI_UART_TX_CAPACITY,
             RAW_GADGET_TUI_UART_TX_CAPACITY);
      ring->start = 0u;
      ring->length = RAW_GADGET_TUI_UART_TX_CAPACITY;
      return;
   }

   write_index =
      (ring->start + ring->length) % RAW_GADGET_TUI_UART_TX_CAPACITY;
   first_length = RAW_GADGET_TUI_UART_TX_CAPACITY - write_index;
   if (first_length > length) {
      first_length = length;
   }

   memcpy(ring->data + write_index, data, first_length);
   memcpy(ring->data, data + first_length, length - first_length);

   overflow = 0u;
   if (length > RAW_GADGET_TUI_UART_TX_CAPACITY - ring->length) {
      overflow = length - (RAW_GADGET_TUI_UART_TX_CAPACITY - ring->length);
   }

   ring->start = (ring->start + overflow) % RAW_GADGET_TUI_UART_TX_CAPACITY;
   ring->length += length - overflow;
}

static void raw_gadget_tui_ring_copy(raw_gadget_tui_ring_t const *ring,
                                     uint8_t *data,
                                     size_t *length) {
   size_t first_length = RAW_GADGET_TUI_UART_TX_CAPACITY - ring->start;

   if (first_length > ring->length) {
      first_length = ring->length;
   }

   memcpy(data, ring->data + ring->start, first_length);
   memcpy(data + first_length, ring->data, ring->length - first_length);
   *length = ring->length;
}

void raw_gadget_tui_take_snapshot(raw_gadget_tui_snapshot_t *snapshot) {
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

void raw_gadget_tui_button_pulse(uint8_t button) {
   uint32_t mask;

   if (button >= RAW_GADGET_TUI_IO_COUNT) {
      return;
   }

   mask = UINT32_C(1) << button;

   (void) pthread_mutex_lock(&raw_gadget_tui_context.io_mutex);
   raw_gadget_tui_context.button_events |= mask;
   (void) pthread_mutex_unlock(&raw_gadget_tui_context.io_mutex);
}

void raw_gadget_tui_button_toggle(uint8_t button) {
   uint32_t mask;

   if (button >= RAW_GADGET_TUI_IO_COUNT) {
      return;
   }

   mask = UINT32_C(1) << button;

   (void) pthread_mutex_lock(&raw_gadget_tui_context.io_mutex);
   raw_gadget_tui_context.button_toggles ^= mask;
   (void) pthread_mutex_unlock(&raw_gadget_tui_context.io_mutex);
}

void raw_gadget_tui_uart_submit(raw_gadget_tui_view_t *view) {
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
