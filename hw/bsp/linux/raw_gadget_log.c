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

#include "raw_gadget_log.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RAW_GADGET_LOG_STACK_BUFFER_SIZE 512u

#if defined(__GNUC__) || defined(__clang__)
#define RAW_GADGET_PRINTF_FORMAT(format_index, argument_index) \
   __attribute__((format(gnu_printf, format_index, argument_index)))
#else
#define RAW_GADGET_PRINTF_FORMAT(format_index, argument_index)
#endif

int raw_gadget_log_printf(char const *format, ...)
   RAW_GADGET_PRINTF_FORMAT(1, 2);

static int raw_gadget_log_vprintf(char const *format, va_list arguments)
   RAW_GADGET_PRINTF_FORMAT(1, 0);

typedef struct {
   pthread_mutex_t mutex;
   uint8_t data[RAW_GADGET_LOG_CAPACITY];
   size_t start;
   size_t length;
} raw_gadget_log_context_t;

static raw_gadget_log_context_t raw_gadget_log_context = {
   .mutex = PTHREAD_MUTEX_INITIALIZER,
};

static void raw_gadget_log_write_locked(uint8_t const *data, size_t length) {
   size_t write_index;
   size_t first_length;
   size_t overflow;

   if (length >= RAW_GADGET_LOG_CAPACITY) {
      memcpy(raw_gadget_log_context.data,
             data + length - RAW_GADGET_LOG_CAPACITY,
             RAW_GADGET_LOG_CAPACITY);
      raw_gadget_log_context.start = 0u;
      raw_gadget_log_context.length = RAW_GADGET_LOG_CAPACITY;
      return;
   }

   write_index =
      (raw_gadget_log_context.start + raw_gadget_log_context.length) %
      RAW_GADGET_LOG_CAPACITY;
   first_length = RAW_GADGET_LOG_CAPACITY - write_index;
   if (first_length > length) {
      first_length = length;
   }

   memcpy(raw_gadget_log_context.data + write_index, data, first_length);
   memcpy(raw_gadget_log_context.data,
          data + first_length,
          length - first_length);

   overflow = 0u;
   if (length > RAW_GADGET_LOG_CAPACITY - raw_gadget_log_context.length) {
      overflow =
         length - (RAW_GADGET_LOG_CAPACITY - raw_gadget_log_context.length);
   }

   raw_gadget_log_context.start =
      (raw_gadget_log_context.start + overflow) % RAW_GADGET_LOG_CAPACITY;
   raw_gadget_log_context.length += length - overflow;
}

static int raw_gadget_log_vprintf(char const *format, va_list arguments) {
   char stack_buffer[RAW_GADGET_LOG_STACK_BUFFER_SIZE];
   char *buffer = stack_buffer;
   va_list copy;
   int required;
   int result;

   if (format == NULL) {
      return -1;
   }

   va_copy(copy, arguments);
   required = vsnprintf(stack_buffer, sizeof(stack_buffer), format, copy);
   va_end(copy);

   if (required < 0) {
      return required;
   }

   if ((size_t) required >= sizeof(stack_buffer)) {
      buffer = malloc((size_t) required + 1u);
      if (buffer == NULL) {
         return -1;
      }

      va_copy(copy, arguments);
      result = vsnprintf(buffer, (size_t) required + 1u, format, copy);
      va_end(copy);

      if (result < 0) {
         free(buffer);
         return result;
      }
   }

   (void) pthread_mutex_lock(&raw_gadget_log_context.mutex);
   raw_gadget_log_write_locked((uint8_t const *) buffer, (size_t) required);
   (void) pthread_mutex_unlock(&raw_gadget_log_context.mutex);

   if (buffer != stack_buffer) {
      free(buffer);
   }

   return required;
}

int raw_gadget_log_printf(char const *format, ...) {
   va_list arguments;
   int result;

   va_start(arguments, format);
   result = raw_gadget_log_vprintf(format, arguments);
   va_end(arguments);

   return result;
}

size_t raw_gadget_log_snapshot(uint8_t *buffer, size_t capacity) {
   size_t length;
   size_t first_length;

   if ((buffer == NULL) || (capacity == 0u)) {
      return 0u;
   }

   (void) pthread_mutex_lock(&raw_gadget_log_context.mutex);

   length = raw_gadget_log_context.length;
   if (length > capacity) {
      length = capacity;
   }

   first_length = RAW_GADGET_LOG_CAPACITY - raw_gadget_log_context.start;
   if (first_length > length) {
      first_length = length;
   }

   memcpy(buffer,
          raw_gadget_log_context.data + raw_gadget_log_context.start,
          first_length);
   memcpy(buffer + first_length,
          raw_gadget_log_context.data,
          length - first_length);

   (void) pthread_mutex_unlock(&raw_gadget_log_context.mutex);

   return length;
}
