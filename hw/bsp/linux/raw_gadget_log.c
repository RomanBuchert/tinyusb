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
#include <stdio.h>
#include <stdlib.h>

#define RAW_GADGET_LOG_STACK_BUFFER_SIZE 512u

int raw_gadget_log_printf(char const *format, ...)
   RAW_GADGET_PRINTF_FORMAT(1, 2);

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
   for (size_t index = 0; index < length; ++index) {
      size_t write_index;

      if (raw_gadget_log_context.length < RAW_GADGET_LOG_CAPACITY) {
         write_index =
            (raw_gadget_log_context.start + raw_gadget_log_context.length) %
            RAW_GADGET_LOG_CAPACITY;
         ++raw_gadget_log_context.length;
      } else {
         write_index = raw_gadget_log_context.start;
         raw_gadget_log_context.start =
            (raw_gadget_log_context.start + 1u) % RAW_GADGET_LOG_CAPACITY;
      }

      raw_gadget_log_context.data[write_index] = data[index];
   }
}

int raw_gadget_log_backend_vprintf(char const *format, va_list arguments) {
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
   result = raw_gadget_log_backend_vprintf(format, arguments);
   va_end(arguments);

   return result;
}

size_t raw_gadget_log_snapshot(uint8_t *buffer, size_t capacity) {
   size_t length;

   if ((buffer == NULL) || (capacity == 0u)) {
      return 0u;
   }

   (void) pthread_mutex_lock(&raw_gadget_log_context.mutex);

   length = raw_gadget_log_context.length;
   if (length > capacity) {
      length = capacity;
   }

   for (size_t index = 0; index < length; ++index) {
      buffer[index] =
         raw_gadget_log_context.data[
            (raw_gadget_log_context.start + index) %
            RAW_GADGET_LOG_CAPACITY];
   }

   (void) pthread_mutex_unlock(&raw_gadget_log_context.mutex);

   return length;
}

void raw_gadget_log_clear(void) {
   (void) pthread_mutex_lock(&raw_gadget_log_context.mutex);
   raw_gadget_log_context.start = 0u;
   raw_gadget_log_context.length = 0u;
   (void) pthread_mutex_unlock(&raw_gadget_log_context.mutex);
}
