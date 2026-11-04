/*
 * identity.h - factory provisioning record reader (A-03)
 *
 * Author:  Sandesh Ghimire <sandesh@soccentric.com>
 * Date:    2026-11-04
 * Target:  STM32F767ZIT6
 * SRS:     identity contract PRV1, factory A-02/A-03/A-05/A-06
 */

#ifndef IDENTITY_H
#define IDENTITY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Reload and validate the record from the identity sector. */
bool app_identity_refresh(void);

/* True when a valid, UID-bound record is present. */
bool app_identity_valid(void);

/* Identity fields; empty strings when no record is present. */
const char *app_identity_serial(void);
const char *app_identity_product(void);
const char *app_identity_hw_revision(void);
const char *app_identity_profile(void);
uint16_t app_identity_revision(void);

/* Copy the provisioned EUI-48.  False when it is absent. */
bool app_identity_mac(uint8_t mac[6]);

/* Compact key=value detail for HelloReply (A-05 compatible). */
bool app_identity_detail(char *out, size_t capacity);

#endif /* IDENTITY_H */
