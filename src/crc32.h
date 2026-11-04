/*
 * crc32.h - CRC-32/ISO-HDLC (the zlib polynomial), no table in RAM
 *
 * Author:  Sandesh Ghimire <sandesh@soccentric.com>
 * Date:    2026-11-03
 * Target:  STM32F767ZIT6
 * SRS:     HWLINK LNK-003/004, BOOT Appendix B
 */

#ifndef CRC32_H
#define CRC32_H

#include <stdint.h>
#include <stddef.h>

uint32_t app_crc32(const void *data, size_t len);

#endif /* CRC32_H */
