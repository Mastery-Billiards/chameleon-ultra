#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "rfid_main.h"
#include "tag_emulation.h"

#define LF_EM410X_TAG_ID_SIZE 5
#define LF_EM410X_ELECTRA_TAG_ID_SIZE 13
#define LF_IOPROX_TAG_ID_SIZE 16
#define LF_HIDPROX_TAG_ID_SIZE 13
#define LF_VIKING_TAG_ID_SIZE 4
#define LF_PAC_TAG_ID_SIZE 8
#define LF_JABLOTRON_TAG_ID_SIZE 5
#define LF_IDTECK_TAG_ID_SIZE 8

void lf_tag_125khz_sense_switch(bool enable);
int lf_tag_data_loadcb(tag_specific_type_t type, tag_data_buffer_t *buffer);
int lf_tag_em410x_data_savecb(tag_specific_type_t type, tag_data_buffer_t *buffer);
bool lf_tag_em410x_data_factory(uint8_t slot, tag_specific_type_t tag_type);
int lf_tag_hidprox_data_savecb(tag_specific_type_t type, tag_data_buffer_t *buffer);
bool lf_tag_hidprox_data_factory(uint8_t slot, tag_specific_type_t tag_type);
int lf_tag_ioprox_data_savecb(tag_specific_type_t type, tag_data_buffer_t *buffer);
bool lf_tag_ioprox_data_factory(uint8_t slot, tag_specific_type_t tag_type);
int lf_tag_viking_data_savecb(tag_specific_type_t type, tag_data_buffer_t *buffer);
bool lf_tag_viking_data_factory(uint8_t slot, tag_specific_type_t tag_type);
int lf_tag_pac_data_savecb(tag_specific_type_t type, tag_data_buffer_t *buffer);
bool lf_tag_pac_data_factory(uint8_t slot, tag_specific_type_t tag_type);
int lf_tag_jablotron_data_savecb(tag_specific_type_t type, tag_data_buffer_t *buffer);
bool lf_tag_jablotron_data_factory(uint8_t slot, tag_specific_type_t tag_type);
int lf_tag_idteck_data_savecb(tag_specific_type_t type, tag_data_buffer_t *buffer);
bool lf_tag_idteck_data_factory(uint8_t slot, tag_specific_type_t tag_type);
bool is_lf_field_exists(void);

/* ------------------------------------------------------------------
 * Reader-read detection for LF.
 *
 * The original signal here was a bare count of 125kHz field-appearance events:
 * "a reader energised us". That turned out to be far too weak a claim to hang a
 * locker on. A reader whose field merely reaches the antenna trips it just as
 * readily as one that is close enough to decode us, because the LPCOMP that
 * raises the event fires at VDD/16 — a few hundred millivolts. Tapping the
 * wrong spot on the locker, or the wrong face of the device, produced exactly
 * that: a counted "read" and a door that stayed shut.
 *
 * What separates the two is coupling strength, not the mere presence of a
 * field. Coupling is reciprocal, so the envelope amplitude the reader's carrier
 * induces on our antenna is also what decides whether our load modulation is
 * strong enough for the reader to demodulate. Sampling that envelope with the
 * SAADC turns a yes/no into millivolts, and millivolts can carry a threshold.
 *
 * So the firmware now reports a small profile of the tap and lets the host
 * decide. Duration alone would not have been enough: one emulation burst is ten
 * EM4100 frames (~328ms), so even the 1-2 second wrong-side taps pushed 30+
 * frames at the reader — many times what one needs to decode — and still failed.
 * The frames were plentiful; they were just too faint.
 * ------------------------------------------------------------------ */

/** Snapshot of LF reader-exposure since the last clear. */
typedef struct {
    uint32_t field_count;      /**< 125kHz field appearances (the legacy signal) */
    uint32_t frames;           /**< tag frames pushed into a live field */
    uint32_t session_ms_max;   /**< longest unbroken field session, milliseconds */
    uint32_t strong_ms_max;    /**< longest unbroken *strong* stretch, milliseconds */
    uint16_t rssi_last_mv;     /**< most recent envelope sample */
    uint16_t rssi_peak_mv;     /**< strongest envelope sample */
    uint16_t samples;          /**< envelope samples taken */
    uint16_t strong_samples;   /**< samples at or above the strong threshold */
    uint16_t strong_run_max;   /**< longest unbroken run of strong samples */
    uint16_t strong_mv;        /**< the strong threshold currently in force */
    uint16_t missed_samples;   /**< conversions skipped because the ADC was busy */

    /* ---- actuation evidence: looking for a dip, not a peak ---------------
     *
     * Everything above this line is a maximum or a monotonic count, which is
     * the right shape for the question "was the reader ever coupled well enough
     * to read us" and the wrong shape for every other question. In particular a
     * *drop* in the carrier is invisible to all of it by construction:
     * rssi_peak_mv only ever rises, strong_samples only ever counts up, and a
     * strong run that briefly dips and recovers still leaves strong_ms_max at
     * its old value.
     *
     * That matters because the one physical event which distinguishes a reader
     * that accepted our id from one that decoded it and threw it away is the
     * bolt: firing it draws 0.3-3A for tens to hundreds of milliseconds out of
     * the same cells that generate the 125kHz carrier, and drags a ferrous
     * armature past the reader's coil. Both push the carrier *down*. A refusal
     * draws almost nothing. So the accept signature, if it exists on a given
     * lock, is a trough — and nothing here could see one.
     *
     * These four fields are the cheapest possible instrument for that question.
     * They add no command, no buffer and no airtime; they are two extra
     * comparisons per sample. If the trough never moves on a door that visibly
     * opens, the whole idea is dead for a tenth of the cost of finding out any
     * other way.
     */
    uint16_t rssi_min_mv;      /**< weakest sample taken while a field was present */
    uint16_t weak_run_max;     /**< longest unbroken run of below-threshold samples */

    /* The idle sample saturates exactly where the answer would live. A correct
     * tap reads 3599mV, and 3599 is the largest number the conversion can
     * produce: raw codes 16380-16383 all map to it at 14-bit resolution and 1/6
     * gain. There is no headroom above it in which a sag could show, and the
     * clip flag sits one code away without ever tripping.
     *
     * Sampling a second time with the modulator *held on* fixes that for free.
     * Load modulation damps the antenna, so the loaded envelope sits well below
     * the unloaded one — back inside the converter's range — while still moving
     * monotonically with it. It costs no airtime, because it happens in the same
     * inter-burst gap the idle sample already uses, and it is the only reading
     * of the two that can show a change at the top of the coupling range.
     */
    uint16_t rssi_loaded_peak_mv; /**< strongest loaded sample (unsaturated) */
    uint16_t rssi_loaded_min_mv;  /**< weakest loaded sample while a field was present */

    bool emulating;            /**< a field session is open right now */
    bool adc_ok;               /**< the envelope channel was claimed successfully */
    bool clipped;              /**< a sample hit ADC full scale (3.6V) */
} lf_tag_em_field_info_t;

/** Legacy count-only accessor, kept so DATA_CMD_LF_GET_FIELD_COUNT still works. */
uint32_t lf_tag_em_field_count(void);
/** Reset every counter and envelope statistic. Leaves the threshold alone. */
void lf_tag_em_field_clear(void);
/** Coherent snapshot of all of the above, safe against the sampling interrupts. */
void lf_tag_em_field_get(lf_tag_em_field_info_t *out);
/** Set the envelope level, in millivolts, at or above which a sample is "strong". */
void lf_tag_em_field_set_strong_mv(uint16_t mv);
