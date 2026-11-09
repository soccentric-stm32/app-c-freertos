/**
 * @file    led_task.c
 * @brief   Segment-based LED pattern engine and LD3 status indication.
 * @details Implementation of the pattern engine declared in @ref led_task.h
 *          (SRS section 6).  After start-up the LED task is the only writer of
 *          LD1 (green, PB0), LD2 (blue, PB7) and LD3 (red, PB14).
 *
 *          **Pattern model.**  Every pattern is expressed as a short, fixed
 *          list of on/off segments whose lengths add up to one cycle.  The
 *          state of one LED is a @ref led_cfg_t that holds the pattern, the
 *          configured period and the tick at which the current cycle phase
 *          was anchored.
 *          @ref evaluate derives the level from the elapsed ticks
 *          (@c now - start) modulo the cycle length and returns the ticks
 *          remaining until the next segment boundary, so the task sleeps
 *          exactly to the next edge instead of polling at a fixed rate
 *          (LED-006).  Durations are milliseconds and the tick rate is
 *          1000 Hz (DRT-001), so one tick equals one millisecond.
 *
 *          **Control path.**  Other tasks never touch the pattern state; they
 *          post a @ref led_msg_t to the module queue (DRT-011).  Button
 *          presses arrive on the same queue, and the outcome of a press is
 *          reported back to Comms as a @c ButtonEvent through
 *          @ref comms_post_button_event (BTN-002, BTN-003).
 *
 *          **LD3 priority.**  LD3 ignores its configured pattern while an
 *          error bit is pending, in this order: @ref APP_ERROR_HEALTH gives a
 *          fast blink (HLT-003), then @ref APP_ERROR_FAULT or
 *          @ref APP_ERROR_WATCHDOG holds LD3 solid (LED-005), and only
 *          otherwise does the pattern selected with @ref led_post_set and
 *          @p force apply.
 *
 *          **Loop shape.**  One iteration evaluates the three patterns,
 *          writes the pins, checks in with the health task every
 *          @ref LED_CHECKIN_MS, then blocks on the queue for the earliest of
 *          the next segment boundary and the next check-in.  A message wakes
 *          the task at once and the rest of the queue is drained without
 *          blocking, so a burst of requests is applied in one iteration.
 * @author  Sandesh Ghimire <sandesh@soccentric.com>
 * @date    2026-11-03
 * @target  NUCLEO-F767ZI
 * @srs     LED-001..LED-007, BTN-002, BTN-003
 * @ingroup led_task
 */

#include "stm32f7xx_hal.h"
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "app_board.h"
#include "app_error.h"
#include "comms_task.h"
#include "health_task.h"
#include "led_task.h"

/**
 * @brief Stack depth of the LED task in words (384 words, 1536 bytes).
 * @details Sized for the deepest call chain, @ref led_task ->
 *          @ref handle_message -> (@ref apply_set or
 *          @ref send_button_event), plus the 60 bytes of segment arrays that
 *          @ref evaluate keeps on the stack.
 * @note Passed as the @c usStackDepth argument of @c xTaskCreate in
 *       @ref led_task_create; FreeRTOS scales it by the 4-byte stack width.
 */
#define LED_TASK_STACK_WORDS    384U
/**
 * @brief LED task priority, @c tskIDLE_PRIORITY + 2.
 * @details The lowest of the application tasks: the Button task runs at
 *          @c + 3, Comms at @c + 4 and Health at @c + 5, so all three can
 *          preempt the LED task.  The pattern engine tolerates this because
 *          every wake-up recomputes the level from the absolute tick count
 *          instead of trusting a relative schedule.
 * @note Preemption only delays a pin update; it cannot desynchronise a
 *       pattern, because the phase is derived from @ref led_cfg_t::start.
 */
#define LED_TASK_PRIORITY       (tskIDLE_PRIORITY + 2U)
/**
 * @brief Depth of the LED message queue in @ref led_msg_t entries.
 * @details Eight entries absorb a HWLINK command burst plus a button press
 *          without any producer blocking: every posting function uses a zero
 *          timeout and reports a full queue as @c false, which Comms turns
 *          into a BUSY NAK.
 * @note A full queue drops the newest request; the LED state stays
 *       consistent because a message is either queued completely or not at
 *       all.
 */
#define LED_QUEUE_LENGTH        8U
/**
 * @brief Health check-in period in milliseconds (500 ms).
 * @details Serves two purposes: it is the deadline after which the task
 *          calls @ref health_checkin with @ref HEALTH_TASK_LED, and it is the
 *          upper bound for the queue wait, so the task never sleeps longer
 *          than this without reporting in.
 * @note Also used directly as a tick count, which is exact only because the
 *       tick rate is 1000 Hz (DRT-001).
 */
#define LED_CHECKIN_MS          500U

/**
 * @brief Full period of the LD3 health-failure blink in milliseconds.
 * @details 100 ms split into 50 ms on and 50 ms off by @ref led_task, that
 *          is, a 5 Hz blink with a 10 Hz edge rate.  The phase is taken from
 *          the absolute tick count (@c now % LED_ERROR_FAST_PERIOD), so the
 *          blink starts wherever the tick counter happens to be when the
 *          health bit appears and is not anchored to a pattern start.
 * @note This is a period, not an on time; the halves are derived from it.
 * @see APP_ERROR_HEALTH
 */
#define LED_ERROR_FAST_PERIOD   100U
/**
 * @brief Default period of LD1 in milliseconds (1000 ms).
 * @details The initial LD1 period, the period restored by a long press and
 *          the fallback applied when a short press finds the stored LD1
 *          period outside @ref APP_LED_MIN_PERIOD ..
 *          @ref APP_LED_MAX_PERIOD.
 * @note Duplicates @ref APP_LED_DEFAULT_PERIOD.  The values are equal today
 *       but nothing enforces that they stay equal.
 */
#define LED_DEFAULT_LED1_PERIOD 1000U
/**
 * @brief Default period of LD2 and LD3 in milliseconds (1000 ms).
 * @details LD2 starts in @ref APP_PATTERN_OFF with this period and LD3 starts
 *          with the same value, so the period is carried but dormant until a
 *          pattern is applied to the slot.
 * @note A long press restores LD1 to @ref LED_DEFAULT_LED1_PERIOD and LD2 to
 *       this value directly; a short press copies the LD1 period instead of
 *       using this macro.
 */
#define LED_DEFAULT_LED2_PERIOD 1000U

/**
 * @brief Sentinel delay meaning "no segment boundary to wait for".
 * @details Returned through @c next_ms by @ref evaluate for
 *          @ref APP_PATTERN_OFF, @ref APP_PATTERN_ON and for a cycle whose
 *          segments sum to zero, and used to seed the three delays in
 *          @ref led_task.  It is the largest @c uint32_t so that the
 *          @c if (next < wait) comparisons can never select it.
 * @warning It is not a duration: never pass it to a delay and never add to
 *          it.
 */
#define NO_TRANSITION           0xFFFFFFFFUL

/**
 * @brief Message opcodes understood by @ref handle_message.
 * @details Private to this file.  The opcode is carried in
 *          @ref led_msg_t::type and never appears on the wire, so the
 *          implicit values are free to change as long as the dispatcher
 *          matches.
 * @note The field is 8 bits wide, so new opcodes must stay below 256.
 */
typedef enum {
    LED_MSG_SET = 0,       /**< Set one LED, from @ref led_post_set. */
    LED_MSG_SET_ALL,       /**< Set LD1 and LD2, from @ref led_post_set_all. */
    LED_MSG_BUTTON,        /**< B1 press; press kind in @ref led_msg_t::press. */
    LED_MSG_CLEAR_ERROR,   /**< Release FAULT/WATCHDOG on LD3 (LED-005). */
    LED_MSG_HEALTH_FAILED  /**< Show the health failure on LD3 (HLT-003). */
} led_msg_type_t;

/**
 * @brief One queued LED command.
 * @details Copied by value into the queue, so a producer may reuse or discard
 *          its message as soon as the posting function returns.  The fields
 *          are interpreted per opcode, and the posting functions leave the
 *          fields that do not apply at zero:
 *
 *          - @ref LED_MSG_SET reads @c led, @c pattern, @c period and
 *            @c force.
 *          - @ref LED_MSG_SET_ALL reads @c pattern and @c period.
 *          - @ref LED_MSG_BUTTON reads @c press only.
 *          - @ref LED_MSG_CLEAR_ERROR and @ref LED_MSG_HEALTH_FAILED read
 *            nothing but @c type.
 *
 * @note The layout is private to this file; the queue stores
 *       @ref LED_QUEUE_LENGTH entries of this type by copy.
 * @warning Do not reorder or widen the fields without updating every
 *          compound literal in the posting functions below.
 */
typedef struct {
    uint8_t type;     /**< Opcode, one of @ref led_msg_type_t. */
    uint8_t led;      /**< APP_LED_ID_* for @ref LED_MSG_SET, else 0. */
    uint8_t pattern;  /**< APP_PATTERN_* for SET and SET_ALL, else 0. */
    uint8_t press;    /**< 1 = short, 2 = long for @ref LED_MSG_BUTTON. */
    uint16_t period;  /**< Period in ms for SET and SET_ALL, else 0. */
    bool force;       /**< Allow LD3 override for @ref LED_MSG_SET. */
} led_msg_t;

/**
 * @brief Pattern state of one LED.
 * @details Holds the configuration applied by @ref apply_set plus the tick
 *          that anchors the current cycle.  @ref evaluate never modifies the
 *          structure; it derives the level and the time to the next edge from
 *          @c start and the current tick count, so a pattern keeps its phase
 *          and only a new request rewrites @c pattern, @c period and
 *          @c start.
 * @note @c start is a raw tick count.  The unsigned difference
 *       @c now - start remains correct across the 32-bit tick wrap (about
 *       49.7 days) and also when @c start lies ahead of @c now.
 */
typedef struct {
    uint8_t pattern;   /**< One of the APP_PATTERN_* identifiers. */
    uint16_t period;   /**< Configured cycle length in milliseconds. */
    uint32_t start;    /**< Tick at which the current cycle phase began. */
} led_cfg_t;

/**
 * @brief Handle of the LED message queue.
 * @details Created by @ref led_task_create and @c NULL until then, which is
 *          what makes every posting function fail before the task exists.
 *          Written once at start-up and only read afterwards, so no lock is
 *          needed around the handle itself; FreeRTOS serialises the queue
 *          internals.
 * @warning @ref led_post tests the handle without synchronisation.  A
 *          failure of @ref led_task_create leaves it non-@c NULL with no
 *          consumer behind it.
 */
static QueueHandle_t led_queue;

/**
 * @brief Advance a pattern identifier to the next one in wrap order.
 * @details Implements the short-press cycle OFF -> ON -> BLINK -> HEARTBEAT
 *          -> FAST -> CHASE -> MORSE_SOS -> OFF that BTN-002 requires.
 *
 * @param[in] current Pattern identifier the LED currently uses.
 *
 * @return The next pattern identifier, wrapping to @ref APP_PATTERN_OFF.
 *
 * @note The modulo uses @ref APP_PATTERN_COUNT, so an out-of-range
 *       @p current still yields a valid identifier instead of being
 *       rejected.
 * @note Task context only; called from @ref handle_message.
 * @see led_post_button
 */
static uint32_t next_pattern(uint32_t current)
{
    return (current + 1U) % APP_PATTERN_COUNT;
}

/**
 * @brief Drive one LED pin through the port's BSRR register.
 * @details Writes the BSx bit for "on" and the BRx bit (@c pin << 16) for
 *          "off" in one 32-bit store.  The level change is therefore atomic
 *          with respect to the other writes to the same port and needs no
 *          read-modify-write of ODR; the HAL write helper is bypassed to keep
 *          the per-iteration cost at one store per LED.
 *
 * @param[in] port GPIO port of the LED, one of @ref APP_LED1_PORT,
 *                 @ref APP_LED2_PORT or @ref APP_LED3_PORT.
 * @param[in] pin  HAL pin mask of the LED, e.g. @ref APP_LED1_PIN.
 * @param[in] on   @c true lights the LED (pin high), @c false darkens it.
 *
 * @pre The pin is configured as a push-pull output by @ref app_leds_init.
 * @note Never blocks, needs no kernel service and allocates nothing, so it is
 *       safe before the scheduler; only the single-writer rule (DRT-011)
 *       restricts it to the LED task.
 * @see app_led3_set
 */
static void led_write(GPIO_TypeDef *port, uint32_t pin, bool on)
{
    port->BSRR = on ? pin : (pin << 16U);
}

/**
 * @brief Expand a pattern into its list of on/off segments.
 * @details Fills the two parallel arrays with the durations and levels of one
 *          complete cycle.  @p dur and @p on are written in lockstep, the
 *          segments are concatenated in array order and the cycle repeats
 *          from index 0.  The expansion is:
 *
 *          - @ref APP_PATTERN_OFF - no segment at all, the cycle is empty.
 *          - @ref APP_PATTERN_ON - one 65535 ms segment, always on.
 *          - @ref APP_PATTERN_BLINK - @c period/2 on, then the remainder off.
 *          - @ref APP_PATTERN_FAST - a cycle of @c period/5 (at least 2 ms)
 *            split into @c cycle/2 on and the remainder off.
 *          - @ref APP_PATTERN_HEARTBEAT - 50 ms on, 150 ms off, 50 ms on and
 *            then off for the rest of the cycle, where the cycle is at least
 *            250 ms because a shorter period is clamped up.
 *          - @ref APP_PATTERN_CHASE - one LED lit for half the period and
 *            dark for the other half; @p second_led selects which half LD2
 *            uses, so the two LEDs of the pair alternate.
 *          - @ref APP_PATTERN_MORSE_SOS and every unknown value - the fixed
 *            18-segment SOS rhythm with a 100 ms unit and a 700 ms word gap,
 *            3400 ms in total.
 *
 * @param[in]  cfg        LED configuration supplying pattern and period.
 * @param[in]  second_led @c false for the LD1/LD3 phase, @c true for LD2,
 *                        which inverts the CHASE half.
 * @param[out] dur        Segment durations in milliseconds; the first @c n
 *                        entries are written.
 * @param[out] on         Segment levels; the first @c n entries are written.
 *
 * @return Number of segments written, between 0 and 18.
 *
 * @pre Both arrays hold at least 20 entries; @ref evaluate supplies them as
 *      local @c uint16_t[20] and @c bool[20].
 * @post Entries at and beyond the return value are untouched.
 * @note Durations are truncated to @c uint16_t.  That is lossless for a valid
 *       period, because the largest derived value is
 *       @ref APP_LED_MAX_PERIOD and the single ON segment is 65535 ms.
 * @note The OFF and ON cases are only reachable when the function is called
 *       directly; @ref evaluate short-circuits both.  They exist so that the
 *       expansion is total for every pattern identifier.
 */
static uint32_t segments_for(const led_cfg_t *cfg, bool second_led,
                             uint16_t dur[20], bool on[20])
{
    uint32_t n = 0U;
    uint32_t period = cfg->period;

    switch (cfg->pattern) {
    case APP_PATTERN_OFF:
        break;

    case APP_PATTERN_ON:
        dur[n] = 0xFFFFU;
        on[n] = true;
        n++;
        break;

    case APP_PATTERN_BLINK:
        dur[n] = (uint16_t)(period / 2U);
        on[n] = true;
        n++;
        dur[n] = (uint16_t)(period - (period / 2U));
        on[n] = false;
        n++;
        break;

    case APP_PATTERN_FAST: {
        uint32_t fast = period / 5U;
        if (fast < 2U) {
            fast = 2U;
        }
        dur[n] = (uint16_t)(fast / 2U);
        on[n] = true;
        n++;
        dur[n] = (uint16_t)(fast - (fast / 2U));
        on[n] = false;
        n++;
        break;
    }

    case APP_PATTERN_HEARTBEAT: {
        uint32_t cycle = period < 250U ? 250U : period;
        dur[n] = 50U;
        on[n] = true;
        n++;
        dur[n] = 150U;
        on[n] = false;
        n++;
        dur[n] = 50U;
        on[n] = true;
        n++;
        if (cycle > 250U) {
            dur[n] = (uint16_t)(cycle - 250U);
            on[n] = false;
            n++;
        }
        break;
    }

    case APP_PATTERN_CHASE: {
        uint32_t half = period / 2U;
        if (second_led) {
            dur[n] = (uint16_t)half;
            on[n] = false;
            n++;
            dur[n] = (uint16_t)(period - half);
            on[n] = true;
        } else {
            dur[n] = (uint16_t)half;
            on[n] = true;
            n++;
            dur[n] = (uint16_t)(period - half);
            on[n] = false;
        }
        n++;
        break;
    }

    case APP_PATTERN_MORSE_SOS:
    default:
        /* ... --- ...  with a 100 ms unit. */
        dur[n] = 100U; on[n] = true;  n++;
        dur[n] = 100U; on[n] = false; n++;
        dur[n] = 100U; on[n] = true;  n++;
        dur[n] = 100U; on[n] = false; n++;
        dur[n] = 100U; on[n] = true;  n++;
        dur[n] = 300U; on[n] = false; n++;
        dur[n] = 300U; on[n] = true;  n++;
        dur[n] = 100U; on[n] = false; n++;
        dur[n] = 300U; on[n] = true;  n++;
        dur[n] = 100U; on[n] = false; n++;
        dur[n] = 300U; on[n] = true;  n++;
        dur[n] = 300U; on[n] = false; n++;
        dur[n] = 100U; on[n] = true;  n++;
        dur[n] = 100U; on[n] = false; n++;
        dur[n] = 100U; on[n] = true;  n++;
        dur[n] = 100U; on[n] = false; n++;
        dur[n] = 100U; on[n] = true;  n++;
        dur[n] = 700U; on[n] = false; n++;
        break;
    }

    return n;
}

/**
 * @brief Compute the LED level now and the delay to the next segment edge.
 * @details The cycle position is @c (now - cfg->start) % cycle, so the phase
 *          is a pure function of the tick count and never drifts: preemption,
 *          a late wake-up or a rolled-over tick counter changes when the
 *          function runs, not which segment is active.  The function walks the
 *          segment list, skipping zero-length entries, until it finds the
 *          segment that contains the position; the time left in that segment
 *          becomes the delay.  The cost is O(segments), at most 18 iterations.
 *
 *          OFF and ON are handled before the walk because neither has a
 *          boundary to wait for, and a pattern whose segments sum to zero
 *          falls back to dark plus @ref NO_TRANSITION.
 *
 * @param[in]  cfg        LED configuration, including the cycle anchor.
 * @param[in]  second_led @c false for the LD1/LD3 phase, @c true for LD2,
 *                        which inverts the CHASE half.
 * @param[in]  now        Current tick count from @c xTaskGetTickCount().
 * @param[out] level      @c true when the LED is lit in the current segment.
 * @param[out] next_ms    Delay from @p now to the next segment edge, in
 *                        ticks, or @ref NO_TRANSITION when there is none.
 *
 * @pre @c cfg::start was set to a tick count, normally the tick at which the
 *      pattern was applied.
 * @post Both output parameters are always written.
 * @note The unit is ticks; it equals milliseconds only because the tick rate
 *       is 1000 Hz (DRT-001).  The caller compares the value against a
 *       millisecond budget directly.
 * @note Task context only.
 * @see segments_for
 */
static void evaluate(const led_cfg_t *cfg, bool second_led, uint32_t now,
                     bool *level, uint32_t *next_ms)
{
    uint16_t dur[20];
    bool on[20];

    if (cfg->pattern == APP_PATTERN_OFF) {
        *level = false;
        *next_ms = NO_TRANSITION;
        return;
    }
    if (cfg->pattern == APP_PATTERN_ON) {
        *level = true;
        *next_ms = NO_TRANSITION;
        return;
    }

    uint32_t count = segments_for(cfg, second_led, dur, on);
    uint32_t cycle = 0U;
    for (uint32_t i = 0U; i < count; i++) {
        cycle += dur[i];
    }
    if (cycle == 0U) {
        *level = false;
        *next_ms = NO_TRANSITION;
        return;
    }

    uint32_t pos = (now - cfg->start) % cycle;
    uint32_t acc = 0U;
    for (uint32_t i = 0U; i < count; i++) {
        if (dur[i] == 0U) {
            continue;
        }
        if (pos < (acc + dur[i])) {
            *level = on[i];
            *next_ms = (acc + dur[i]) - pos;
            return;
        }
        acc += dur[i];
    }

    *level = false;
    *next_ms = cycle - pos;
}

/**
 * @brief Report a button-driven pattern change to the Comms task.
 * @details Thin wrapper around @ref comms_post_button_event that keeps the
 *          call sites in @ref handle_message readable.  Comms mirrors the new
 *          LD1/LD2 state for its @c Status reply and, once a @c Hello has
 *          opened the link, sends a single unsolicited @c ButtonEvent
 *          (BTN-002, BTN-003).
 *
 * @param[in] press   Press kind: 1 for a short press, 2 for a long press.
 * @param[in] pattern Pattern now applied to LD1 and LD2.
 * @param[in] period  Period now applied to LD1 and LD2, in milliseconds.
 *
 * @warning Blocks the caller for up to 20 ms when the Comms event queue is
 *          full, which delays the rest of the LED loop by that amount.  The
 *          pattern phase itself is unaffected because it is derived from the
 *          tick count.
 * @note The result is discarded: a dropped event affects only the mirrored
 *       @c Status, never the LEDs.
 * @note Task context, never an ISR.
 * @see led_post_button
 */
static void send_button_event(uint8_t press, uint8_t pattern, uint16_t period)
{
    comms_post_button_event(press, pattern, period);
}

/**
 * @brief Apply one pattern request to the configuration table.
 * @details Encodes the pairing rules of the demo:
 *          - @ref APP_LED_ID_3 addresses the LD3 slot (@c cfg[2]) and never
 *            touches LD1 or LD2, so the status display stays independent.
 *          - @ref APP_PATTERN_CHASE on either LD1 or LD2 configures both with
 *            the same period and the same anchor, which is what makes the
 *            alternation seamless.
 *          - Leaving CHASE switches the partner LED off so the pair cannot
 *            stay split; the addressed LED then receives the new pattern.
 *          - Any identifier other than 2 or 3 is treated as @ref APP_LED_ID_1.
 *
 *          Every write restarts the cycle phase by storing @p now in
 *          @c start.
 *
 * @param[in,out] cfg     Three-element table, index 0 = LD1, 1 = LD2,
 *                        2 = LD3.
 * @param[in]     led     Target LED, one of @ref APP_LED_ID_1 ..
 *                        @ref APP_LED_ID_3.
 * @param[in]     pattern Pattern to apply; not validated here.
 * @param[in]     period  Period in milliseconds, stored in the addressed slot
 *                        and in the partner slot when CHASE is broken; not
 *                        range-checked here.
 * @param[in]     now     Tick count used as the new phase anchor.
 *
 * @post The addressed slot carries @p pattern, @p period and @c start ==
 *       @p now.
 * @warning Validation is the producer's job: Comms rejects patterns outside
 *          @ref APP_PATTERN_OFF .. @ref APP_PATTERN_MORSE_SOS and periods
 *          outside @ref APP_LED_MIN_PERIOD .. @ref APP_LED_MAX_PERIOD, while
 *          @ref evaluate maps an unknown pattern to the SOS rhythm.
 * @note Task context only.
 */
static void apply_set(led_cfg_t cfg[3], uint8_t led, uint8_t pattern,
                      uint16_t period, uint32_t now)
{
    if (led == APP_LED_ID_3) {
        cfg[2].pattern = pattern;
        cfg[2].period = period;
        cfg[2].start = now;
        return;
    }

    if (pattern == APP_PATTERN_CHASE) {
        cfg[0].pattern = APP_PATTERN_CHASE;
        cfg[0].period = period;
        cfg[0].start = now;
        cfg[1].pattern = APP_PATTERN_CHASE;
        cfg[1].period = period;
        cfg[1].start = now;
        return;
    }

    /* Leaving CHASE breaks the pair. */
    if (cfg[0].pattern == APP_PATTERN_CHASE ||
        cfg[1].pattern == APP_PATTERN_CHASE) {
        if (led == APP_LED_ID_1) {
            cfg[1].pattern = APP_PATTERN_OFF;
            cfg[1].period = period;
            cfg[1].start = now;
        } else {
            cfg[0].pattern = APP_PATTERN_OFF;
            cfg[0].period = period;
            cfg[0].start = now;
        }
    }

    uint32_t index = (led == APP_LED_ID_2) ? 1U : 0U;
    cfg[index].pattern = pattern;
    cfg[index].period = period;
    cfg[index].start = now;
}

/**
 * @brief Apply one queued command to the configuration and error state.
 * @details Dispatcher for @ref led_msg_type_t.  The function only updates
 *          state: it writes the configuration table and the caller's error
 *          accumulator, never a pin, so the pins are refreshed once per
 *          iteration from the resulting configuration.
 *
 *          Behaviour per opcode:
 *          - @ref LED_MSG_SET applies one LED through @ref apply_set.  A
 *            request that targets LD3 without @c force is dropped here as a
 *            second line of defence after the HWLINK handler.
 *          - @ref LED_MSG_SET_ALL gives LD1 and LD2 the same pattern and
 *            period and restarts both phases together; LD3 is untouched.
 *          - @ref LED_MSG_BUTTON with @c press == 2 restores the defaults
 *            (LD1 heartbeat, LD2 off, both at their default periods) and
 *            reports the reset.  Any other value is a short press: LD1
 *            advances one pattern via @ref next_pattern, keeps its stored
 *            period when that period lies inside @ref APP_LED_MIN_PERIOD ..
 *            @ref APP_LED_MAX_PERIOD and otherwise falls back to
 *            @ref LED_DEFAULT_LED1_PERIOD, LD2 is given the same pattern and
 *            period, and the outcome is reported to Comms.
 *          - @ref LED_MSG_CLEAR_ERROR drops @ref APP_ERROR_FAULT and
 *            @ref APP_ERROR_WATCHDOG from the accumulator but keeps
 *            @ref APP_ERROR_HEALTH, so ClearFault cannot hide a pending-slot
 *            health failure (LED-005).
 *          - @ref LED_MSG_HEALTH_FAILED latches @ref APP_ERROR_HEALTH in the
 *            accumulator, which selects the fast LD3 blink (HLT-003).
 *
 * @param[in,out] cfg         Three-element configuration table.
 * @param[in]     msg         Message dequeued by @ref led_task.
 * @param[in]     now         Tick count used to re-anchor changed patterns.
 * @param[in,out] error_flags Local accumulator of APP_ERROR_* bits.
 *
 * @post Only the slots named by @p msg changed their phase anchor.
 * @note The accumulator is task-local on purpose: it is ORed with
 *       @ref app_error_flags at the top of every iteration, so a clear here
 *       lasts only until the next iteration unless the global latch was
 *       cleared as well.
 * @warning A clear is lost when the producer clears the global latch but the
 *          message cannot be queued: the bit stays in the accumulator and
 *          keeps driving LD3 until the next reset.
 * @note Task context only.  A button message may block for up to 20 ms inside
 *       @ref send_button_event.
 * @see apply_set
 */
static void handle_message(led_cfg_t cfg[3], const led_msg_t *msg,
                           uint32_t now, uint32_t *error_flags)
{
    switch (msg->type) {
    case LED_MSG_SET:
        if (msg->led == APP_LED_ID_3 && !msg->force) {
            return;
        }
        apply_set(cfg, msg->led, msg->pattern, msg->period, now);
        break;

    case LED_MSG_SET_ALL:
        apply_set(cfg, APP_LED_ID_1, msg->pattern, msg->period, now);
        cfg[1].pattern = msg->pattern;
        cfg[1].period = msg->period;
        cfg[1].start = now;
        break;

    case LED_MSG_BUTTON:
        if (msg->press == 2U) {   /* long: restore defaults */
            cfg[0].pattern = APP_PATTERN_HEARTBEAT;
            cfg[0].period = LED_DEFAULT_LED1_PERIOD;
            cfg[0].start = now;
            cfg[1].pattern = APP_PATTERN_OFF;
            cfg[1].period = LED_DEFAULT_LED2_PERIOD;
            cfg[1].start = now;
            send_button_event(2U, APP_PATTERN_HEARTBEAT,
                              LED_DEFAULT_LED1_PERIOD);
        } else {
            uint16_t period = cfg[0].period;
            if (period < APP_LED_MIN_PERIOD || period > APP_LED_MAX_PERIOD) {
                period = LED_DEFAULT_LED1_PERIOD;
            }
            uint8_t pattern = (uint8_t)next_pattern(cfg[0].pattern);
            apply_set(cfg, APP_LED_ID_1, pattern, period, now);
            cfg[1].pattern = pattern;
            cfg[1].period = period;
            cfg[1].start = now;
            send_button_event(1U, pattern, period);
        }
        break;

    case LED_MSG_CLEAR_ERROR:
        /* LED-005: keep the health bit while health is still failed. */
        *error_flags &= ~(APP_ERROR_FAULT | APP_ERROR_WATCHDOG);
        break;

    case LED_MSG_HEALTH_FAILED:
        *error_flags |= APP_ERROR_HEALTH;
        break;

    default:
        break;
    }
}

/**
 * @brief LED task body: evaluate the patterns, drive the pins, drain the
 *        queue.
 * @details The task starts with LD1 in @ref APP_PATTERN_HEARTBEAT and LD2 and
 *          LD3 in @ref APP_PATTERN_OFF, all phases anchored to the tick at
 *          which it first runs, then loops forever:
 *          -# Read the tick count and OR the global error latch into the
 *             local accumulator, so a bit set by another module becomes
 *             visible without a message.
 *          -# Evaluate LD1 (first half of the CHASE pair) and LD2 (second
 *             half) from their configuration slots.
 *          -# Select the LD3 level in priority order: @ref APP_ERROR_HEALTH
 *             blinks with phase @c now % @ref LED_ERROR_FAST_PERIOD and a
 *             50 ms on / 50 ms off split (HLT-003); @ref APP_ERROR_FAULT or
 *             @ref APP_ERROR_WATCHDOG holds LD3 solid (LED-005); otherwise
 *             the LD3 slot pattern is evaluated with the LD1 phase.
 *          -# Write all three pins through BSRR so the observable state
 *             changes in one place.
 *          -# Call @ref health_checkin with @ref HEALTH_TASK_LED once
 *             @ref LED_CHECKIN_MS has elapsed.
 *          -# Compute the wait as @ref LED_CHECKIN_MS minus the time already
 *             spent towards the check-in, then shorten it to the smallest of
 *             the three next-segment delays; a zero wait becomes one tick so
 *             the loop always yields.
 *          -# Block on the queue for that wait.  On a message, take a fresh
 *             tick count, apply the message and drain the remaining messages
 *             with a zero timeout.  All messages drained in the same pass
 *             share one tick count and therefore one phase anchor.
 *
 * @param[in] arg Unused task argument; @ref led_task_create passes @c NULL.
 *
 * @note Never returns; the loop runs until the board resets.
 *
 * @pre The queue exists, the three LED pins are configured as outputs and the
 *      scheduler is running.
 * @post The task is blocked on its queue or on the tick delay; it never
 *       suspends itself and never exits.
 *
 * @note The wait is bounded from above by @ref LED_CHECKIN_MS and clamped from
 *       below at one tick, so the task wakes at least twice per second and
 *       never spins on a zero timeout.
 * @note Because @ref evaluate derives the level from the absolute tick count,
 *       a long preemption or a late wake-up still shows the correct level; it
 *       only removes the overshoot from a segment.
 * @warning Handling a button message can block for up to 20 ms in
 *          @ref comms_post_button_event, which delays the pin update and the
 *          next check-in by that amount.
 * @see led_task_create
 * @see evaluate
 */
static void led_task(void *arg)
{
    (void)arg;
    led_cfg_t cfg[3] = {
        { APP_PATTERN_HEARTBEAT, LED_DEFAULT_LED1_PERIOD, 0U },
        { APP_PATTERN_OFF,       LED_DEFAULT_LED2_PERIOD, 0U },
        { APP_PATTERN_OFF,       LED_DEFAULT_LED2_PERIOD, 0U }
    };
    uint32_t now = xTaskGetTickCount();
    cfg[0].start = now;
    cfg[1].start = now;
    cfg[2].start = now;
    uint32_t error_flags = app_error_flags;
    uint32_t last_checkin = now;

    for (;;) {
        now = xTaskGetTickCount();
        error_flags |= app_error_flags;

        bool led1_on = false;
        bool led2_on = false;
        bool led3_on = false;
        uint32_t next1 = NO_TRANSITION;
        uint32_t next2 = NO_TRANSITION;
        uint32_t next3 = NO_TRANSITION;

        evaluate(&cfg[0], false, now, &led1_on, &next1);
        evaluate(&cfg[1], true, now, &led2_on, &next2);

        if ((error_flags & APP_ERROR_HEALTH) != 0U) {
            uint32_t pos = now % LED_ERROR_FAST_PERIOD;
            led3_on = pos < (LED_ERROR_FAST_PERIOD / 2U);
            next3 = (led3_on ? (LED_ERROR_FAST_PERIOD / 2U)
                             : LED_ERROR_FAST_PERIOD) - pos;
        } else if ((error_flags & (APP_ERROR_FAULT | APP_ERROR_WATCHDOG)) != 0U) {
            led3_on = true;
            next3 = NO_TRANSITION;
        } else {
            evaluate(&cfg[2], false, now, &led3_on, &next3);
        }

        led_write(APP_LED1_PORT, APP_LED1_PIN, led1_on);
        led_write(APP_LED2_PORT, APP_LED2_PIN, led2_on);
        led_write(APP_LED3_PORT, APP_LED3_PIN, led3_on);

        if ((now - last_checkin) >= LED_CHECKIN_MS) {
            health_checkin(HEALTH_TASK_LED);
            last_checkin = now;
        }

        uint32_t wait = LED_CHECKIN_MS - (now - last_checkin);
        if (next1 < wait) {
            wait = next1;
        }
        if (next2 < wait) {
            wait = next2;
        }
        if (next3 < wait) {
            wait = next3;
        }
        if (wait == 0U) {
            wait = 1U;
        }

        led_msg_t msg;
        if (xQueueReceive(led_queue, &msg, pdMS_TO_TICKS(wait)) == pdPASS) {
            now = xTaskGetTickCount();
            handle_message(cfg, &msg, now, &error_flags);
            while (xQueueReceive(led_queue, &msg, 0U) == pdPASS) {
                handle_message(cfg, &msg, now, &error_flags);
            }
        }
    }
}

/**
 * @brief Configure the three LED GPIOs and drive them dark.
 * @details Enables the GPIOB clock, resets LD1, LD2 and LD3 with a single
 *          @c HAL_GPIO_WritePin call and then configures the three pins as
 *          push-pull outputs without pull resistors at the low slew rate.  The
 *          ports and pins come from @ref app_board.h, so a board change is a
 *          header change plus a matching clock enable.
 *
 * @pre The HAL and the clock tree are initialised; the scheduler may still be
 *      stopped.
 * @post The three pins are outputs and all three LEDs are dark, which is the
 *       state @ref led_task expects when it takes over the pins.
 * @note Not reentrant and not guarded against a second call: call it exactly
 *       once from the start-up thread.
 * @note The button and UART pins are configured by their own modules, and the
 *       fault path in @ref app_fault.c drives LD3 at register level without
 *       this configuration.
 * @see led_task_create
 */
void app_leds_init(void)
{
    GPIO_InitTypeDef gpio = {0};

    __HAL_RCC_GPIOB_CLK_ENABLE();

    HAL_GPIO_WritePin(APP_LED1_PORT, APP_LED1_PIN | APP_LED2_PIN | APP_LED3_PIN,
                      GPIO_PIN_RESET);
    gpio.Pin   = APP_LED1_PIN | APP_LED2_PIN | APP_LED3_PIN;
    gpio.Mode  = GPIO_MODE_OUTPUT_PP;
    gpio.Pull  = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(APP_LED1_PORT, &gpio);
}

/**
 * @brief Force LD3 on or off through BSRR, bypassing the pattern engine.
 * @details The same register write as @ref led_write, but hard-coded to
 *          @ref APP_LED3_PORT and @ref APP_LED3_PIN, so it also works before
 *          the task exists and without the queue.
 *
 * @param[in] on @c true lights LD3, @c false darkens it.
 *
 * @post The pin matches @p on until the LED task next writes LD3, which is at
 *       most one segment or @ref LED_CHECKIN_MS later.
 * @warning Violates the single-writer rule of DRT-011.  The current firmware
 *          does not call it; it exists as an early-boot and fatal-path escape
 *          hatch.
 * @note Needs no kernel service, so it is safe before the scheduler.
 * @see app_leds_init
 */
void app_led3_set(bool on)
{
    APP_LED3_PORT->BSRR = on ? APP_LED3_PIN : ((uint32_t)APP_LED3_PIN << 16U);
}

/**
 * @brief Create the LED queue and then the LED task.
 * @details Creates a queue of @ref LED_QUEUE_LENGTH @ref led_msg_t entries and
 *          a task named @c "LED" with @ref LED_TASK_STACK_WORDS of stack at
 *          @ref LED_TASK_PRIORITY.  The task handle argument of @c xTaskCreate
 *          is @c NULL because nothing notifies the task directly: all control
 *          goes through the queue.  The queue is created first so the task can
 *          never observe a @c NULL handle.
 *
 * @return @c true when the queue and the task both exist.
 * @retval false Queue allocation failed, or @c xTaskCreate did not return
 *               @c pdPASS.
 *
 * @pre Called once from the start-up thread, after @ref app_leds_init and
 *      before the scheduler starts (DRT-004), while the FreeRTOS heap still
 *      has room for the stack and the queue.
 * @post On success every posting function works and the task appears in the
 *       health supervision set.
 * @warning A failed @c xTaskCreate leaves the queue allocated and
 *          @ref led_queue non-@c NULL, so the posting functions would accept
 *          requests that nobody consumes.  The application treats the failure
 *          as fatal and does not retry.
 * @note Not idempotent: a second call replaces the handle and leaks the first
 *       queue and task.
 * @see app_leds_init
 * @see led_post_set
 */
bool led_task_create(void)
{
    led_queue = xQueueCreate(LED_QUEUE_LENGTH, sizeof(led_msg_t));
    if (led_queue == NULL) {
        return false;
    }
    return xTaskCreate(led_task, "LED", LED_TASK_STACK_WORDS, NULL,
                       LED_TASK_PRIORITY, NULL) == pdPASS;
}

/**
 * @brief Queue one message with a zero timeout.
 * @details Common tail of every posting function.  It refuses the call when
 *          the queue does not exist yet and otherwise calls @c xQueueSend with
 *          a timeout of zero ticks, so the caller never blocks and never
 *          yields.
 *
 * @param[in] msg Message to copy into the queue.
 *
 * @return @c true when the message was queued.
 * @retval false @ref led_queue is @c NULL, or the queue is full.
 *
 * @pre @p msg is fully initialised; the unused fields are copied verbatim.
 * @post On success exactly one message is pending for the LED task; on failure
 *       the queue is unchanged.
 * @warning Not ISR-safe: it uses @c xQueueSend instead of the @c FromISR
 *          variant, so it must not be called from interrupt context.
 * @note Task context only.  The message is copied, so @p msg may live on the
 *       caller's stack.
 * @see led_post_set
 */
static bool led_post(const led_msg_t *msg)
{
    if (led_queue == NULL) {
        return false;
    }
    return xQueueSend(led_queue, msg, 0U) == pdPASS;
}

/**
 * @brief Queue a pattern change for one LED (DRT-011).
 * @details Builds a @ref LED_MSG_SET message and posts it.  The task applies it
 *          on its next iteration and re-anchors the phase to the tick at that
 *          moment, so the pattern always starts at its first segment.
 *
 * @param[in] led     Target LED: @ref APP_LED_ID_1, @ref APP_LED_ID_2 or
 *                    @ref APP_LED_ID_3.
 * @param[in] pattern One of @ref APP_PATTERN_OFF ..
 *                    @ref APP_PATTERN_MORSE_SOS.
 * @param[in] period  Pattern period in milliseconds.
 * @param[in] force   @c true allows overriding the reserved LD3 display.
 *
 * @return @c true when the message was queued.
 * @retval false The task is not created yet or the queue is full.
 *
 * @note No validation happens here.  The HWLINK handler checks the LED, the
 *       pattern and the period before calling, and an out-of-range @p led falls
 *       back to LD1 in @ref apply_set.
 * @warning Not ISR-safe, see @ref led_post.
 * @post On success the change is applied asynchronously and the LED may take up
 *       to one iteration to show it.
 * @see led_post_set_all
 */
bool led_post_set(uint8_t led, uint8_t pattern, uint16_t period, bool force)
{
    led_msg_t msg = {
        .type = LED_MSG_SET,
        .led = led,
        .pattern = pattern,
        .period = period,
        .force = force
    };
    return led_post(&msg);
}

/**
 * @brief Queue the same pattern for both LD1 and LD2.
 * @details Builds a @ref LED_MSG_SET_ALL message.  The task restarts both
 *          phases together, so the two LEDs stay in sync, and leaves LD3
 *          alone so the error display cannot be overwritten by a bulk request.
 *
 * @param[in] pattern One of @ref APP_PATTERN_OFF ..
 *                    @ref APP_PATTERN_MORSE_SOS.
 * @param[in] period  Pattern period in milliseconds.
 *
 * @return @c true when the message was queued.
 * @retval false The task is not created yet or the queue is full.
 *
 * @note Because both LEDs receive the same pattern with the same anchor, any
 *       pattern other than @ref APP_PATTERN_CHASE makes LD1 and LD2 look
 *       identical; only CHASE separates them.
 * @see led_post_set
 */
bool led_post_set_all(uint8_t pattern, uint16_t period)
{
    led_msg_t msg = {
        .type = LED_MSG_SET_ALL,
        .pattern = pattern,
        .period = period
    };
    return led_post(&msg);
}

/**
 * @brief Queue a button press for the pattern demonstration.
 * @details Builds a @ref LED_MSG_BUTTON message.  A short press advances LD1 to
 *          the next pattern and copies it to LD2; a long press restores the
 *          defaults (BTN-002, BTN-003).  The task also reports the result to
 *          Comms, which turns the press into an unsolicited @c ButtonEvent.
 *
 * @param[in] press 1 for a short press, 2 for a long press.
 *
 * @return @c true when the message was queued.
 * @retval false The task is not created yet or the queue is full; the press is
 *               then dropped and the pattern does not change.
 *
 * @note The Button task posts 1 on a release shorter than its short-press limit
 *       and 2 once when the press passes its long-press limit; a release after
 *       a long press does not produce a second message.
 * @note Any value other than 2 is handled as a short press.
 * @see handle_message
 * @see comms_post_button_event
 */
bool led_post_button(uint8_t press)
{
    led_msg_t msg = {
        .type = LED_MSG_BUTTON,
        .press = press
    };
    return led_post(&msg);
}

/**
 * @brief Queue a release of the latched FAULT/WATCHDOG display (LED-005).
 * @details Builds a @ref LED_MSG_CLEAR_ERROR message.  The task drops
 *          @ref APP_ERROR_FAULT and @ref APP_ERROR_WATCHDOG from its local
 *          accumulator, so LD3 returns to its configured pattern, while
 *          @ref APP_ERROR_HEALTH keeps blinking.
 *
 * @return @c true when the message was queued.
 * @retval false The task is not created yet or the queue is full.
 *
 * @warning The message alone does not clear the global latch.  Comms calls
 *          app_fault_clear() and app_error_clear_fault() first, because bits
 *          that are still set in @ref app_error_flags would otherwise be ORed
 *          back in on the next iteration.
 * @warning If the global latch was already cleared and this post fails, the
 *          bits stay in the local accumulator and LD3 stays solid until the
 *          next reset.
 * @post A latched FAULT/WATCHDOG no longer forces LD3 on; a pending HEALTH
 *       failure still does.
 * @see led_post_health_failed
 */
bool led_post_clear_error(void)
{
    led_msg_t msg = { .type = LED_MSG_CLEAR_ERROR };
    return led_post(&msg);
}

/**
 * @brief Queue the fast LD3 blink for a failed health check (HLT-003).
 * @details Builds a @ref LED_MSG_HEALTH_FAILED message.  The task latches
 *          @ref APP_ERROR_HEALTH in its local accumulator, which selects the
 *          @ref LED_ERROR_FAST_PERIOD blink.  The health task calls this
 *          together with app_error_set(APP_ERROR_HEALTH) so the latch and the
 *          display agree.
 *
 * @return @c true when the message was queued.
 * @retval false The task is not created yet or the queue is full; the
 *               indication then still appears through the global latch that
 *               the task ORs in on every iteration.
 *
 * @note Does not modify @ref app_error_flags; the producer is responsible for
 *       latching the bit.
 * @note The blink cannot be released by @ref led_post_clear_error.
 * @see led_post_clear_error
 */
bool led_post_health_failed(void)
{
    led_msg_t msg = { .type = LED_MSG_HEALTH_FAILED };
    return led_post(&msg);
}
