# Doxygen style guide

This project documents every hand-written C source and header with Doxygen.
Generated FlatBuffers files (`src/generated/`) and vendored flatcc sources
(`third_party/`) are excluded.

## Ground rules

- Documentation comments only. Never change code, identifiers, include
  order, macro values, formatting of code, or runtime behaviour.
- Keep lines at or below 90 columns, matching `.clang-format`.
- Write in the present tense and the imperative mood for functions.
- Prefer ASCII. Use `->` rather than Unicode arrows.
- Keep existing requirement/SRS references; add `@srs` or `@note` where they
  help explain why code exists.
- Do not invent requirements. If the reason for a symbol is unclear, describe
  what the code does and add a note that the rationale is inherited from the
  bootloader contract or SRS.

## File header

Every `.c` and `.h` starts with a `@file` block. Keep the author/date/target/SRS
metadata that already exists and move it into the block.

```c
/**
 * @file    app_error.c
 * @brief   Latched application error state implementation.
 * @details Each module owns one or more bits in @ref app_error_flags.  The
 *          latch is read by the LED task (LD3) and reported in Status.
 * @author  Sandesh Ghimire <sandesh@soccentric.com>
 * @date    2026-11-03
 * @target  NUCLEO-F767ZI (STM32F767ZIT6)
 * @srs     LED-005, DGEN-004, DGEN-005, HLT-003
 */
```

Headers may add `@ingroup` to attach the file to a module group.

## Modules and groups

Give each module a `@defgroup` in its public header and close it with `@}`.
Use the group for the public API. Use `@ingroup` on related declarations.

```c
/**
 * @defgroup app_error Application error latch
 * @brief Latched error bits reported on LD3 and in Status.
 * @{
 */

/* declarations... */

/** @} */
```

Sub-groups are appropriate for flag bits, register layouts, or command sets.

## Macros and constants

Document every macro, including configuration knobs and bit masks. A leading
block comment is best for multi-line explanations; a trailing `/**< ... */`
is best for short single-line constants.

```c
/** Saved fault or assert (LED-005). */
#define APP_ERROR_FAULT (1UL << 0U)

#define APP_SLOT_BASE 0x0800C000UL /**< Application link address in bank 1. */
```

Function-like macros need `@param` and `@return`.

## Enumerations

Document the enum type and every enumerator. Add explicit values only when the
code already has them (never reorder or renumber).

```c
/**
 * @brief Reason recorded in the no-init fault record.
 */
typedef enum
{
    APP_FAULT_NONE = 0,   /**< No fault recorded. */
    APP_FAULT_HARDFAULT,  /**< HardFault handler. */
    /* ... */
} app_fault_reason_t;
```

## Structures, unions and typedefs

Document the type and every field. Mention units, valid ranges, ownership and
lifetime where they matter. Use `@note` for ABI or persistence constraints.

```c
/**
 * @brief Persistent fault record stored in the .noinit section.
 * @note The layout is read by the debugger and must not be reordered.
 */
typedef struct
{
    uint32_t magic;                    /**< APP_FAULT_MAGIC when valid. */
    uint32_t reason;                   /**< One of @ref app_fault_reason_t. */
    char     task[APP_FAULT_TASK_LEN]; /**< NUL-terminated task name. */
} app_fault_record_t;
```

## Variables

Document every global and file-scope static variable. Use `/**< ... */` for
short descriptions and a leading block for longer ownership/locking notes.

```c
/** Read-only latch shared by the LED task and Status. */
extern volatile uint32_t app_error_flags;

static TaskHandle_t s_led_task; /**< LED task handle, created at start-up. */
```

## Functions

Document every function, including `static` helpers in `.c` files. Use `@brief`
for a one-line summary and `@details` for behaviour that is not obvious from
the signature. Always document parameters and return values. State units,
ownership, blocking behaviour and ISR safety.

```c
/**
 * @brief Build an unencoded HWLINK frame.
 * @details Layout is proto, flags, sequence, payload, then CRC-32.  The
 *          function never blocks and performs no allocation.
 *
 * @param[in]  payload      Pointer to the payload bytes.
 * @param[in]  payload_len  Payload length in bytes.
 * @param[in]  flags        HWLINK_FLAG_* bit mask.
 * @param[in]  seq          Sequence number copied into the header.
 * @param[out] raw          Destination buffer for the frame.
 * @param[in]  raw_capacity Capacity of @p raw in bytes.
 * @param[out] raw_len      Number of bytes written.
 *
 * @return true on success, false when the frame does not fit.
 * @retval false @p raw_capacity is too small or an argument is invalid.
 *
 * @note Not thread-safe when the same output buffer is shared.
 * @see hwlink_cobs_encode
 */
bool hwlink_build_raw(const uint8_t *payload, size_t payload_len,
                      uint8_t flags, uint16_t seq,
                      uint8_t *raw, size_t raw_capacity, size_t *raw_len);
```

Use the parameter direction that matches the API:

- `@param[in]` - read by the function.
- `@param[out]` - written by the function.
- `@param[in,out]` - read and updated.

Additional tags to use when they add value:

- `@pre` / `@post` - required state before/after the call.
- `@warning` - misuse, concurrency or safety hazard.
- `@note` - design rationale, SRS clause, non-obvious behaviour.
- `@retval` - specific return values.
- `@see` - related functions, groups or documents.
- `@deprecated` - only if the code already says so.
- `@todo` - only for a real, acknowledged gap; never invent work.

For `static` helpers, add a `@internal` note only when the file has several
layers and the distinction helps a reader.

## Callback and ISR documentation

State the execution context explicitly: task, ISR, or either. For callbacks,
document who invokes them and whether they may block.

## Cross-references

- Reference symbols with `@ref name` or `@p param`.
- Reference modules with `@ref group_name`.
- Reference files with `@ref file_name.h`.
- Keep SRS identifiers in backticks or plain text so they survive search.

## Verification

After editing a file:

1. Run `git diff --check -- <file>` to catch whitespace damage.
2. Re-read the diff and confirm every changed line is inside a comment.
3. Confirm the file still compiles as part of the normal build.
