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
