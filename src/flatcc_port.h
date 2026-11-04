/*
 * flatcc_port.h - flatcc runtime allocation hooks
 *
 * Author:  Sandesh Ghimire <sandesh@soccentric.com>
 * Date:    2026-11-03
 * Target:  STM32F767ZIT6
 * SRS:     DRT-004 (no allocation after start-up), DCON-002 (flatcc runtime)
 *
 * The flatcc runtime is compiled with FLATCC_ALLOC/FLATCC_FREE/... mapped to
 * these functions, which use a fixed static arena.  This header is force-
 * included into every translation unit in CMakeLists.txt so the macro
 * expansion sees the prototypes.
 */

#ifndef FLATCC_PORT_H
#define FLATCC_PORT_H

#include <stddef.h>
#include <stdint.h>

void *app_flatcc_alloc(size_t size);
void app_flatcc_free(void *ptr);
void *app_flatcc_realloc(void *ptr, size_t size);
void *app_flatcc_calloc(size_t count, size_t size);
void *app_flatcc_aligned_alloc(size_t alignment, size_t size);
void app_flatcc_aligned_free(void *ptr);

#endif /* FLATCC_PORT_H */
