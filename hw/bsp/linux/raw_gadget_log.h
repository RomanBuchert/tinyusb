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

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RAW_GADGET_LOG_CAPACITY 65536u

#if defined(__GNUC__) || defined(__clang__)
#define RAW_GADGET_PRINTF_FORMAT(format_index, argument_index) \
   __attribute__((format(gnu_printf, format_index, argument_index)))
#else
#define RAW_GADGET_PRINTF_FORMAT(format_index, argument_index)
#endif

int raw_gadget_log_backend_vprintf(char const *format, va_list arguments)
   RAW_GADGET_PRINTF_FORMAT(1, 0);

size_t raw_gadget_log_snapshot(uint8_t *buffer, size_t capacity);
void raw_gadget_log_clear(void);

#ifdef __cplusplus
}
#endif
