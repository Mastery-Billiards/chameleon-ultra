#include "lf_tag_em.h"

#include <stdint.h>

#include "app_timer.h"
#include "app_util_platform.h"
#include "bsp_delay.h"
#include "fds_util.h"
#include "nrf_gpio.h"
#include "nrf_soc.h"
#include "nrfx_lpcomp.h"
#include "nrfx_pwm.h"
#include "nrfx_saadc.h"
#include "protocols/em410x.h"
#include "protocols/hidprox.h"
#include "protocols/idteck.h"
#include "protocols/ioprox.h"
#include "protocols/jablotron.h"
#include "protocols/pac.h"
#include "protocols/viking.h"
#include "syssleep.h"
#include "tag_emulation.h"
#include "tag_persistence.h"

#define NRF_LOG_MODULE_NAME tag_em410x
#include "nrf_log.h"
#include "nrf_log_ctrl.h"
#include "nrf_log_default_backends.h"
NRF_LOG_MODULE_REGISTER();

#define ANT_NO_MOD() nrf_gpio_pin_clear(LF_MOD)

// Whether the USB light effect is allowed to enable
extern bool g_usb_led_marquee_enable;

// Whether it is currently in the low -frequency card number of broadcasting
static volatile bool m_is_lf_emulating = false;
// Cache tag type
static tag_specific_type_t m_tag_type = TAG_TYPE_UNDEFINED;

/* ------------------------------------------------------------------
 * Reader-read detection for LF (locker "tap" acknowledgement)
 *
 * See lf_tag_em.h for why a field-appearance count is not enough on its own and
 * what the envelope measurement adds. The mechanics:
 *
 * Every emulation burst ends in pwm_handler with the modulator released and a
 * 2ms settle — the one moment in the cycle when LF_RSSI carries the reader's
 * carrier and nothing of ours. is_lf_field_exists() already samples there. We
 * take an ADC reading at the same point, so measuring costs no extra airtime
 * and cannot disturb the id stream the reader is trying to decode.
 *
 * That puts a sample every ~328ms (ten EM4100 frames per burst). Coarse, but it
 * is the resolution the physics allows: sampling more often would mean cutting
 * the burst shorter, and a reader that wants two or three *consecutive* clean
 * frames would start missing them. A one-second tap still yields three samples,
 * which is enough to tell a sustained strong field from a glancing one.
 *
 * Concurrency: the LPCOMP, PWM and SAADC interrupts all sit at
 * APP_IRQ_PRIORITY_LOW, so none of them preempts another and a blocking
 * conversion inside a handler is safe. The host-facing snapshot spans several
 * words, so it reads under CRITICAL_REGION the way the HF one does.
 * ------------------------------------------------------------------ */

// SAADC channel 0 is the battery monitor's (ble_main.c). Tag mode borrows 1 and
// gives it back when LF sensing stops.
#define LF_RSSI_ADC_CHANNEL     1
// 14-bit resolution (NRFX_SAADC_CONFIG_RESOLUTION=3) at the default single-ended
// gain of 1/6 against the 0.6V internal reference — full scale is 3.6V.
#define LF_RSSI_ADC_FULL_SCALE  16384
#define LF_RSSI_ADC_FULL_MV     3600
// Frames per emulation burst. Shared with the playback calls below so the frame
// tally and the airtime can never drift apart.
#define LF_EMU_BURST_FRAMES     10
// Default "strong enough that the reader can demodulate us" level. A starting
// point, not a law of nature: the host can retune it per deployment over
// DATA_CMD_LF_SET_STRONG_MV without reflashing, which is the whole reason the
// threshold lives in a variable.
#define LF_STRONG_MV_DEFAULT    600

// Returned when no reading could be taken at all. Distinct from 0 mV, which is
// a real measurement meaning "no field": a skipped conversion must not be
// scored as a weak sample, or a busy ADC would break the run of a good tap.
#define LF_RSSI_NO_READING      0xFFFF

static volatile uint32_t m_lf_field_count = 0;
static volatile uint32_t m_lf_frames = 0;
static volatile uint32_t m_lf_session_ms_max = 0;
static volatile uint32_t m_lf_session_start_tk = 0;
static volatile uint32_t m_lf_strong_ms_max = 0;
static volatile uint32_t m_lf_strong_start_tk = 0;
static volatile uint16_t m_lf_rssi_last_mv = 0;
static volatile uint16_t m_lf_rssi_peak_mv = 0;
static volatile uint16_t m_lf_samples = 0;
static volatile uint16_t m_lf_strong_samples = 0;
static volatile uint16_t m_lf_strong_run = 0;
static volatile uint16_t m_lf_strong_run_max = 0;
static volatile uint16_t m_lf_strong_mv = LF_STRONG_MV_DEFAULT;
static volatile uint16_t m_lf_missed_samples = 0;
// Set once LF sensing has claimed its ADC channel, so a sample is never
// attempted against a channel that was never initialised. Reported to the host,
// which refuses to arm without it: a device that cannot measure would otherwise
// reject every tap forever, and that is indistinguishable from a bad locker.
static volatile bool m_lf_rssi_adc_ready = false;
// A sample pinned to ADC full scale. If this sets on good taps, the envelope is
// saturating and any apparent strong/weak gap is an artefact of the clip, so the
// host should be told rather than left to trust the number.
static volatile bool m_lf_rssi_clipped = false;

/** Milliseconds elapsed since an app_timer tick reading. */
static uint32_t lf_ms_since(uint32_t start_tk) {
    uint32_t ticks = app_timer_cnt_diff_compute(app_timer_cnt_get(), start_tk);
    // APP_TIMER_TICKS(1) is the tick count in one millisecond; dividing by it
    // converts back without assuming a particular RTC prescaler.
    return ticks / APP_TIMER_TICKS(1);
}

/**
 * @brief Read the LF envelope in millivolts. Returns 0 if the ADC is unavailable.
 *
 * Only meaningful when called with the modulator released and settled, i.e. from
 * pwm_handler after ANT_NO_MOD(), or at field-up before playback starts.
 */
static uint16_t lf_rssi_sample_mv(void) {
    if (!m_lf_rssi_adc_ready) {
        return LF_RSSI_NO_READING;
    }
    nrf_saadc_value_t raw = 0;
    // Busy rather than broken: the LF *reader* drives continuous SAADC buffer
    // conversions, and battery sampling borrows the peripheral periodically.
    // Neither should coincide with tag mode, but if one does, report "no
    // reading" rather than a fabricated 0 mV that would look like a weak tap.
    if (nrfx_saadc_sample_convert(LF_RSSI_ADC_CHANNEL, &raw) != NRFX_SUCCESS) {
        return LF_RSSI_NO_READING;
    }
    if (raw < 0) {
        raw = 0;  // single-ended conversions can read slightly negative at 0V
    }
    if (raw >= (LF_RSSI_ADC_FULL_SCALE - 1)) {
        m_lf_rssi_clipped = true;
    }
    return (uint16_t)(((uint32_t)raw * LF_RSSI_ADC_FULL_MV) / LF_RSSI_ADC_FULL_SCALE);
}

/** Fold one envelope sample into the running profile. */
static void lf_rssi_record(uint16_t mv) {
    if (mv == LF_RSSI_NO_READING) {
        // Nothing was measured, so nothing is known. Leaving the run alone is
        // the point: scoring an unavailable conversion as weak would punish a
        // perfectly good tap for a passing ADC conflict.
        if (m_lf_missed_samples < UINT16_MAX) {
            m_lf_missed_samples++;
        }
        return;
    }

    m_lf_rssi_last_mv = mv;
    if (mv > m_lf_rssi_peak_mv) {
        m_lf_rssi_peak_mv = mv;
    }
    if (m_lf_samples < UINT16_MAX) {
        m_lf_samples++;
    }

    if (mv >= m_lf_strong_mv) {
        if (m_lf_strong_samples < UINT16_MAX) {
            m_lf_strong_samples++;
        }
        if (m_lf_strong_run < UINT16_MAX) {
            m_lf_strong_run++;
        }
        if (m_lf_strong_run > m_lf_strong_run_max) {
            m_lf_strong_run_max = m_lf_strong_run;
        }
        // Time the stretch rather than counting samples. A burst is ten frames,
        // which is ~328ms for EM410X but ~655ms for Electra's double-length
        // frame, so a sample count means different things on different cards
        // while milliseconds mean the same thing on both.
        if (m_lf_strong_run == 1) {
            m_lf_strong_start_tk = app_timer_cnt_get();
        } else {
            uint32_t held = lf_ms_since(m_lf_strong_start_tk);
            if (held > m_lf_strong_ms_max) {
                m_lf_strong_ms_max = held;
            }
        }
    } else {
        // A weak sample breaks the run. This is the line that refuses a tap
        // which drifts out of range.
        m_lf_strong_run = 0;
    }
}

uint32_t lf_tag_em_field_count(void) {
    return m_lf_field_count;
}

void lf_tag_em_field_clear(void) {
    CRITICAL_REGION_ENTER();
    m_lf_field_count = 0;
    m_lf_frames = 0;
    m_lf_session_ms_max = 0;
    m_lf_strong_ms_max = 0;
    m_lf_rssi_last_mv = 0;
    m_lf_rssi_peak_mv = 0;
    m_lf_samples = 0;
    m_lf_strong_samples = 0;
    m_lf_strong_run = 0;
    m_lf_strong_run_max = 0;
    m_lf_missed_samples = 0;
    m_lf_rssi_clipped = false;
    // A session already in flight keeps running, but its clock restarts here so
    // the reported duration only ever covers time the host asked about.
    m_lf_session_start_tk = app_timer_cnt_get();
    CRITICAL_REGION_EXIT();
}

void lf_tag_em_field_get(lf_tag_em_field_info_t *out) {
    if (out == NULL) {
        return;
    }
    CRITICAL_REGION_ENTER();
    out->field_count = m_lf_field_count;
    out->frames = m_lf_frames;
    out->session_ms_max = m_lf_session_ms_max;
    out->strong_ms_max = m_lf_strong_ms_max;
    out->rssi_last_mv = m_lf_rssi_last_mv;
    out->rssi_peak_mv = m_lf_rssi_peak_mv;
    out->samples = m_lf_samples;
    out->strong_samples = m_lf_strong_samples;
    out->strong_run_max = m_lf_strong_run_max;
    out->strong_mv = m_lf_strong_mv;
    out->missed_samples = m_lf_missed_samples;
    out->emulating = m_is_lf_emulating;
    out->adc_ok = m_lf_rssi_adc_ready;
    out->clipped = m_lf_rssi_clipped;
    if (m_is_lf_emulating) {
        // Report the session in progress too, otherwise a host polling during a
        // long steady tap would keep seeing whatever the previous tap managed.
        uint32_t open_ms = lf_ms_since(m_lf_session_start_tk);
        if (open_ms > out->session_ms_max) {
            out->session_ms_max = open_ms;
        }
        // Likewise for a strong stretch still running: without this a host
        // polling during a steady hold would keep seeing the value frozen at
        // the last sample rather than the hold in progress.
        if (m_lf_strong_run > 0) {
            uint32_t held = lf_ms_since(m_lf_strong_start_tk);
            if (held > out->strong_ms_max) {
                out->strong_ms_max = held;
            }
        }
    }
    CRITICAL_REGION_EXIT();
}

void lf_tag_em_field_set_strong_mv(uint16_t mv) {
    m_lf_strong_mv = mv;
}

// The pwm to broadcast modulated card id
const nrfx_pwm_t m_broadcast = NRFX_PWM_INSTANCE(0);
const nrf_pwm_sequence_t *m_pwm_seq = NULL;

static void lf_field_lost(void) {
    // Close the exposure session before dropping the emulation flags, so a host
    // polling after the reader has gone still sees how long the tap lasted.
    uint32_t session_ms = lf_ms_since(m_lf_session_start_tk);
    if (session_ms > m_lf_session_ms_max) {
        m_lf_session_ms_max = session_ms;
    }
    // Bank any strong stretch that was still running when the field went, then
    // end it — a run must never bridge two separate taps.
    if (m_lf_strong_run > 0) {
        uint32_t held = lf_ms_since(m_lf_strong_start_tk);
        if (held > m_lf_strong_ms_max) {
            m_lf_strong_ms_max = held;
        }
    }
    m_lf_strong_run = 0;

    // Open the incident interruption, so that the next event can be in and out normally
    g_is_tag_emulating = false;  // Reset the flag in the emulation
    m_is_lf_emulating = false;
    TAG_FIELD_LED_OFF()  // Make sure the indicator light of the LF field status
    // Re-arm LPCOMP so the next field appearance triggers lpcomp_event_handler.
    NRF_LPCOMP->INTENSET = LPCOMP_INTENSET_UP_Msk;
    // call sleep_timer_start *after* unsetting g_is_tag_emulating
    sleep_timer_start(SLEEP_DELAY_MS_FIELD_125KHZ_LOST);  // Start the timer to enter the sleep
    NRF_LOG_INFO("LF FIELD LOST");
}

/**
 * @brief Judge field status
 */
bool is_lf_field_exists(void) {
    nrfx_lpcomp_enable();
    bsp_delay_us(30);  // Display for a period of time and sampling to avoid misjudgment
    nrf_lpcomp_task_trigger(NRF_LPCOMP_TASK_SAMPLE);
    return nrf_lpcomp_result_get() == 1;  // Determine the sampling results of the LF field status
}

/**
 * @brief LPCOMP event handler is called when LPCOMP detects voltage drop.
 *
 * This function is called from interrupt context so it is very important
 * to return quickly. Don't put busy loops or any other CPU intensive actions here.
 * It is also not allowed to call soft device functions from it (if LPCOMP IRQ
 * priority is set to APP_IRQ_PRIORITY_HIGH).
 */
static void lpcomp_event_handler(nrf_lpcomp_event_t event) {
    // Only when the lf-frequency emulation is not launched, and the analog card is started
    if (m_is_lf_emulating || event != NRF_LPCOMP_EVENT_UP) {
        return;
    }

    // Reader-read detection: a 125kHz reader just energised the antenna while we
    // are emulating. On its own this says nothing about how well we are coupled
    // — that is what the envelope samples taken in pwm_handler are for — so open
    // a session and let the burst cycle fill in the evidence.
    if (m_lf_field_count != 0xFFFFFFFF) {
        m_lf_field_count++;
    }
    m_lf_session_start_tk = app_timer_cnt_get();
    // Runs of strong samples must never bridge two separate taps.
    m_lf_strong_run = 0;

    sleep_timer_stop();  // turn off dormant delay
    // Disable LPCOMP during emulation — LF_RSSI fluctuates during load
    // modulation and would trigger spurious DOWN events with DETECT_CROSS.
    // Field-loss is checked periodically via EVT_END_SEQ0 in pwm_handler.
    nrfx_lpcomp_disable();

    // set the emulation status logo bit
    m_is_lf_emulating = true;
    g_is_tag_emulating = true;
    // turn off USB light effect when emulating cards
    g_usb_led_marquee_enable = false;

    // LED status update
    set_slot_light_color(RGB_BLUE);
    TAG_FIELD_LED_ON()

    // Play a finite burst then stop — field check happens in EVT_STOPPED after
    // PWM has fully released LF_MOD, so ANT_NO_MOD() and the settle delay are
    // effective. NRFX_PWM_FLAG_LOOP kept the pin owned by the peripheral,
    // making the field check always read "present" due to self-drive on LF_RSSI.
    nrfx_pwm_simple_playback(&m_broadcast, m_pwm_seq, LF_EMU_BURST_FRAMES, NRFX_PWM_FLAG_STOP);

    NRF_LOG_INFO("LF FIELD DETECTED");
}

/**
 * @brief Claim a SAADC channel on the LF envelope pin for the duration of LF sensing.
 *
 * The driver itself is brought up once at boot for the battery monitor, so this
 * only adds a channel. If that fails — most plausibly because the LF reader has
 * torn the driver down and rebuilt it for its own streaming use — envelope
 * measurement simply stays off and lf_rssi_sample_mv() returns 0.
 */
static void lf_rssi_adc_enable(void) {
    nrf_saadc_channel_config_t ch = NRFX_SAADC_DEFAULT_CHANNEL_CONFIG_SE(LF_RSSI_ADC);
    // The pin is a peak-detector output, so it is a slow-moving DC level rather
    // than the 125kHz carrier. A longer acquisition window suits its source
    // impedance and costs nothing at one sample per burst.
    ch.acq_time = NRF_SAADC_ACQTIME_40US;
    m_lf_rssi_adc_ready = (nrfx_saadc_channel_init(LF_RSSI_ADC_CHANNEL, &ch) == NRFX_SUCCESS);
    if (!m_lf_rssi_adc_ready) {
        NRF_LOG_WARNING("LF RSSI ADC channel unavailable; read detection degraded");
    }
}

static void lf_rssi_adc_disable(void) {
    if (m_lf_rssi_adc_ready) {
        m_lf_rssi_adc_ready = false;
        nrfx_saadc_channel_uninit(LF_RSSI_ADC_CHANNEL);
    }
}

static void lpcomp_init(void) {
    nrfx_lpcomp_config_t cfg = NRFX_LPCOMP_DEFAULT_CONFIG;
    cfg.input = LF_RSSI;
    cfg.hal.reference = NRF_LPCOMP_REF_SUPPLY_1_16;
    cfg.hal.detection = NRF_LPCOMP_DETECT_UP;
    cfg.hal.hyst = NRF_LPCOMP_HYST_50mV;

    ret_code_t err_code = nrfx_lpcomp_init(&cfg, lpcomp_event_handler);
    APP_ERROR_CHECK(err_code);
}

static void pwm_handler(nrfx_pwm_evt_type_t event_type) {
    if (event_type != NRFX_PWM_EVT_STOPPED) {
        return;
    }
    // PWM has fully stopped — LF_MOD is released back to GPIO.
    // Now ANT_NO_MOD() and the settle delay are effective.
    ANT_NO_MOD();
    bsp_delay_ms(2);  // let peak detector drain: ~2 ms time constant on LF_RSSI

    // A burst just finished, so its frames went out into whatever field was
    // there. Count them before deciding whether to continue.
    m_lf_frames += LF_EMU_BURST_FRAMES;

    // The modulator is released and settled: LF_RSSI now carries the reader's
    // carrier alone. This is the only point in the cycle where the envelope
    // means "how hard is the reader driving us", which is the quantity that
    // decides whether our load modulation is legible to it.
    //
    // LPCOMP is taken off the pin first. is_lf_field_exists() leaves it enabled
    // from the previous iteration, and an enabled comparator on the same analog
    // input loads the SAADC's acquisition. The check below re-enables it, and
    // nothing is missed meanwhile: UP events are ignored while emulating.
    nrfx_lpcomp_disable();
    lf_rssi_record(lf_rssi_sample_mv());

    if (is_lf_field_exists()) {
        // Field still present — play another finite burst then check again.
        nrfx_pwm_simple_playback(&m_broadcast, m_pwm_seq, LF_EMU_BURST_FRAMES, NRFX_PWM_FLAG_STOP);
    } else {
        // Field gone — clean up.
        lf_field_lost();
    }
}

static void pwm_init(void) {
    nrfx_pwm_config_t cfg = NRFX_PWM_DEFAULT_CONFIG;
    cfg.output_pins[0] = LF_MOD;
    for (uint8_t i = 1; i < NRF_PWM_CHANNEL_COUNT; i++) {
        cfg.output_pins[i] = NRFX_PWM_PIN_NOT_USED;
    }
    cfg.irq_priority = APP_IRQ_PRIORITY_LOW;
    // Base clock depends on the currently-loaded tag type. Legacy ASK/FSK
    // protocols (EM410x, HID, ioProx, Viking, PAC) use 125kHz base so that
    // their hardcoded counter_top values (8-64 range) produce the correct
    // absolute timing. PSK1 protocols need finer resolution for the 16us
    // subcarrier period, so pwm_init uses 1MHz base with counter_top=16.
    // See tag_base_type.h IS_PSK1_TYPE for the list of qualifying types.
    cfg.base_clock = IS_PSK1_TYPE(m_tag_type) ? NRF_PWM_CLK_1MHz : NRF_PWM_CLK_125kHz;
    cfg.count_mode = NRF_PWM_MODE_UP;
    cfg.load_mode = NRF_PWM_LOAD_WAVE_FORM;
    cfg.step_mode = NRF_PWM_STEP_AUTO;

    nrfx_err_t err_code = nrfx_pwm_init(&m_broadcast, &cfg, pwm_handler);
    APP_ERROR_CHECK(err_code);
}

static void lf_sense_enable(void) {
    // PWM bit timing divides HFCLK by a fixed ratio. On HFINT (64 MHz RC,
    // ±1.5% at 25°C after factory trim, wider over temperature) this gives a
    // chip-to-chip spread that NRZ readers — which see cumulative error across
    // runs of same-polarity bits with no intra-run resync — reject even when
    // Manchester/FSK readers don't. Holding HFXO brings the PWM clock to
    // ±40 ppm, which is also tight enough for differential PSK encodings
    // (e.g. IDTECK) where what the reader decodes are bit-to-bit phase
    // transitions, so absolute phase lock to the reader's carrier is not
    // required. The tag-mode antenna taps on this board are envelope-only,
    // which rules out coherent demodulation or phase-lock-based approaches,
    // but does not preclude the differential-phase encodings supported here.
    //
    // Paired release in lf_sense_disable(). SD reference-counts HFXO requests,
    // so this coexists with BLE. Both functions run from thread context
    // (tag_mode_enter/tag_emulation_sense_end) where SVCs are safe.
    sd_clock_hfclk_request();
    uint32_t hfclk_running = 0;
    while (!hfclk_running) {
        sd_clock_hfclk_is_running(&hfclk_running);
    }

    lpcomp_init();
    lf_rssi_adc_enable();
    pwm_init();  // use precise hardware pwm to broadcast card id
    if (is_lf_field_exists()) {
        lpcomp_event_handler(NRF_LPCOMP_EVENT_UP);
    }
}

static void lf_sense_disable(void) {
    nrfx_pwm_uninit(&m_broadcast);
    nrfx_lpcomp_uninit();
    lf_rssi_adc_disable();
    m_pwm_seq = NULL;
    m_is_lf_emulating = false;
    sd_clock_hfclk_release();
}

static enum {
    LF_SENSE_STATE_NONE,
    LF_SENSE_STATE_DISABLE,
    LF_SENSE_STATE_ENABLE,
} m_lf_sense_state = LF_SENSE_STATE_NONE;

static uint16_t lf_em410x_id_size(tag_specific_type_t type) {
    return type == TAG_TYPE_EM410X_ELECTRA ? LF_EM410X_ELECTRA_TAG_ID_SIZE : LF_EM410X_TAG_ID_SIZE;
}

/**
 * @brief switchLfFieldInductionToEnableTheState
 */
void lf_tag_125khz_sense_switch(bool enable) {
    // init modulation PIN as output PIN
    nrf_gpio_cfg_output(LF_MOD);
    // turn off mod, otherwise its hard to judge RSSI
    ANT_NO_MOD();

    if ((m_lf_sense_state == LF_SENSE_STATE_NONE || m_lf_sense_state == LF_SENSE_STATE_DISABLE) && enable) {
        // switch from disable -> enable
        m_lf_sense_state = LF_SENSE_STATE_ENABLE;
        lf_sense_enable();
    } else if (m_lf_sense_state == LF_SENSE_STATE_ENABLE && !enable) {
        // switch from enable -> disable
        m_lf_sense_state = LF_SENSE_STATE_DISABLE;
        lf_sense_disable();
    }
}

/** @brief lf card data loader
 * @param type     Refined tag type
 * @param buffer   Data buffer
 */
int lf_tag_data_loadcb(tag_specific_type_t type, tag_data_buffer_t *buffer) {
    // ensure buffer size is large enough for specific tag type,
    // so that tag data (e.g., card numbers) can be converted to corresponding pwm sequence here.
    if ((type == TAG_TYPE_EM410X || type == TAG_TYPE_EM410X_ELECTRA) && buffer->length >= lf_em410x_id_size(type)) {
        const protocol *p = type == TAG_TYPE_EM410X_ELECTRA ? &em410x_electra : &em410x_64;
        m_tag_type = type;
        void *codec = p->alloc();
        m_pwm_seq = p->modulator(codec, buffer->buffer);
        p->free(codec);
        NRF_LOG_INFO("load lf em410x%s data finish.", type == TAG_TYPE_EM410X_ELECTRA ? " electra" : "");
        return lf_em410x_id_size(type);
    }

    if (type == TAG_TYPE_HID_PROX && buffer->length >= LF_HIDPROX_TAG_ID_SIZE) {
        m_tag_type = type;
        void *codec = hidprox.alloc();
        m_pwm_seq = hidprox.modulator(codec, buffer->buffer);
        hidprox.free(codec);
        NRF_LOG_INFO("load lf hidprox data finish.");
        return LF_HIDPROX_TAG_ID_SIZE;
    }

    if (type == TAG_TYPE_IOPROX && buffer->length >= LF_IOPROX_TAG_ID_SIZE) {
        m_tag_type = type;
        void *codec = ioprox.alloc();
        m_pwm_seq = ioprox.modulator(codec, buffer->buffer);
        ioprox.free(codec);
        NRF_LOG_INFO("load lf ioprox data finish.");
        return LF_IOPROX_TAG_ID_SIZE;
    }

    if (type == TAG_TYPE_VIKING && buffer->length >= LF_VIKING_TAG_ID_SIZE) {
        m_tag_type = type;
        void *codec = viking.alloc();
        m_pwm_seq = viking.modulator(codec, buffer->buffer);
        viking.free(codec);
        NRF_LOG_INFO("load lf viking data finish.");
        return LF_VIKING_TAG_ID_SIZE;
    }

    if (type == TAG_TYPE_PAC && buffer->length >= LF_PAC_TAG_ID_SIZE) {
        m_tag_type = type;
        void *codec = pac.alloc();
        m_pwm_seq = pac.modulator(codec, buffer->buffer);
        pac.free(codec);
        NRF_LOG_INFO("load lf pac data finish.");
        return LF_PAC_TAG_ID_SIZE;
    }

    if (type == TAG_TYPE_JABLOTRON && buffer->length >= LF_JABLOTRON_TAG_ID_SIZE) {
        m_tag_type = type;
        void *codec = jablotron.alloc();
        m_pwm_seq = jablotron.modulator(codec, buffer->buffer);
        jablotron.free(codec);
        NRF_LOG_INFO("load lf jablotron data finish.");
        return LF_JABLOTRON_TAG_ID_SIZE;
    }

    if (type == TAG_TYPE_IDTECK && buffer->length >= LF_IDTECK_TAG_ID_SIZE) {
        m_tag_type = type;
        void *codec = idteck.alloc();
        m_pwm_seq = idteck.modulator(codec, buffer->buffer);
        idteck.free(codec);
        NRF_LOG_INFO("load lf idteck data finish.");
        return LF_IDTECK_TAG_ID_SIZE;
    }

    NRF_LOG_ERROR("no valid data exists in buffer for tag type: %d.", type);
    return 0;
}

/** @brief Id card deposit card number before callback
 * @param type      Refined tag type
 * @param buffer    Data buffer
 * @return The length of the data that needs to be saved is that it does not save when 0
 */
int lf_tag_em410x_data_savecb(tag_specific_type_t type, tag_data_buffer_t *buffer) {
    // Make sure to load this tag before allowing saving
    // Just save the original card package directly
    if (m_tag_type == TAG_TYPE_EM410X) {
        return LF_EM410X_TAG_ID_SIZE;
    }
    if (m_tag_type == TAG_TYPE_EM410X_ELECTRA) {
        return LF_EM410X_ELECTRA_TAG_ID_SIZE;
    }
    return 0;
}

/** @brief Id card deposit card number before callback
 * @param type      Refined tag type
 * @param buffer    Data buffer
 * @return The length of the data that needs to be saved is that it does not save when 0
 */
int lf_tag_hidprox_data_savecb(tag_specific_type_t type, tag_data_buffer_t *buffer) {
    // Make sure to load this tag before allowing saving
    // Just save the original card package directly
    return m_tag_type == TAG_TYPE_HID_PROX ? LF_HIDPROX_TAG_ID_SIZE : 0;
}

/** @brief Id card deposit card number before callback
 * @param type      Refined tag type
 * @param buffer    Data buffer
 * @return The length of the data that needs to be saved is that it does not save when 0
 */
int lf_tag_ioprox_data_savecb(tag_specific_type_t type, tag_data_buffer_t *buffer) {
    // Make sure to load this tag before allowing saving
    // Just save the original card package directly
    return m_tag_type == TAG_TYPE_IOPROX ? LF_IOPROX_TAG_ID_SIZE : 0;
}

/** @brief Id card deposit card number before callback
 * @param type      Refined tag type
 * @param buffer    Data buffer
 * @return The length of the data that needs to be saved is that it does not save when 0
 */
int lf_tag_viking_data_savecb(tag_specific_type_t type, tag_data_buffer_t *buffer) {
    // Make sure to load this tag before allowing saving
    // Just save the original card package directly
    return m_tag_type == TAG_TYPE_VIKING ? LF_VIKING_TAG_ID_SIZE : 0;
}

bool lf_tag_data_factory(uint8_t slot, tag_specific_type_t tag_type, uint8_t *tag_id, uint16_t length) {
    // write data to flash
    tag_sense_type_t sense_type = get_sense_type_from_tag_type(tag_type);
    fds_slot_record_map_t map_info;  // Get the special card slot FDS record information
    get_fds_map_by_slot_sense_type_for_dump(slot, sense_type, &map_info);
    // Call the blocked FDS to write the function, and write the data of the specified field type of the card slot into the Flash
    bool ret = fds_write_sync(map_info.id, map_info.key, length, (uint8_t *)tag_id);
    if (ret) {
        NRF_LOG_INFO("Factory slot data success.");
    } else {
        NRF_LOG_ERROR("Factory slot data error.");
    }
    return ret;
}

/** @brief Id card deposit card number before callback
 * @param slot      Card slot number
 * @param tag_type  Refined tag type
 * @return Whether the format is successful, if the formatting is successful, it will return to True, otherwise False will be returned
 */
bool lf_tag_em410x_data_factory(uint8_t slot, tag_specific_type_t tag_type) {
    static const uint8_t tag_id_base[LF_EM410X_TAG_ID_SIZE] = {0xDE, 0xAD, 0xBE, 0xEF, 0x88};
    static const uint8_t tag_id_electra[LF_EM410X_ELECTRA_TAG_ID_SIZE] = {0xDE, 0xAD, 0xBE, 0xEF, 0x88,
                                                                          0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
                                                                         };

    switch (tag_type) {
        case TAG_TYPE_EM410X_ELECTRA:
            return lf_tag_data_factory(slot, tag_type, (uint8_t *)tag_id_electra, sizeof(tag_id_electra));
        case TAG_TYPE_EM410X:
            return lf_tag_data_factory(slot, tag_type, (uint8_t *)tag_id_base, sizeof(tag_id_base));
        default:
            return false;
    }
}

/** @brief Id card deposit card number before callback
 * @param slot      Card slot number
 * @param tag_type  Refined tag type
 * @return Whether the format is successful, if the formatting is successful, it will return to True, otherwise False will be returned
 */
bool lf_tag_hidprox_data_factory(uint8_t slot, tag_specific_type_t tag_type) {
    // default id, must to align(4), more word...
    uint8_t tag_id[13] = {0x01, 0x00, 0x00, 0x00, 0x64, 0x00, 0x00, 0x00, 0x51, 0x45, 0x00, 0x00, 0x00};
    return lf_tag_data_factory(slot, tag_type, tag_id, sizeof(tag_id));
}

/** @brief Id card deposit card number before callback
 * @param slot      Card slot number
 * @param tag_type  Refined tag type
 * @return Whether the format is successful, if the formatting is successful, it will return to True, otherwise False will be returned
 */
bool lf_tag_ioprox_data_factory(uint8_t slot, tag_specific_type_t tag_type) {
    uint8_t tag_id[16] = {
        0x01, 0xAA, 0x30, 0x39, 0x00, 0x78, 0x6A, 0xA0, 0x33, 0x09, 0xCF, 0xEF, 0x00, 0x00, 0x00, 0x00
    };
    return lf_tag_data_factory(slot, tag_type, tag_id, sizeof(tag_id));
}

/** @brief Id card deposit card number before callback
 * @param slot      Card slot number
 * @param tag_type  Refined tag type
 * @return Whether the format is successful, if the formatting is successful, it will return to True, otherwise False will be returned
 */
bool lf_tag_viking_data_factory(uint8_t slot, tag_specific_type_t tag_type) {
    // default id
    uint8_t tag_id[4] = {0xDE, 0xAD, 0xBE, 0xEF};
    return lf_tag_data_factory(slot, tag_type, tag_id, sizeof(tag_id));
}

int lf_tag_pac_data_savecb(tag_specific_type_t type, tag_data_buffer_t *buffer) {
    return m_tag_type == TAG_TYPE_PAC ? LF_PAC_TAG_ID_SIZE : 0;
}

bool lf_tag_pac_data_factory(uint8_t slot, tag_specific_type_t tag_type) {
    // default id: 8 ASCII bytes
    uint8_t tag_id[8] = {'C', 'A', 'R', 'D', '0', '0', '0', '1'};
    return lf_tag_data_factory(slot, tag_type, tag_id, sizeof(tag_id));
}

int lf_tag_jablotron_data_savecb(tag_specific_type_t type, tag_data_buffer_t *buffer) {
    return m_tag_type == TAG_TYPE_JABLOTRON ? LF_JABLOTRON_TAG_ID_SIZE : 0;
}

bool lf_tag_jablotron_data_factory(uint8_t slot, tag_specific_type_t tag_type) {
    // default id: 5 bytes (top bit must be 0)
    uint8_t tag_id[5] = {0x01, 0xB6, 0x69, 0x00, 0x00};
    return lf_tag_data_factory(slot, tag_type, tag_id, sizeof(tag_id));
}

/** @brief IDTECK data save callback. */
int lf_tag_idteck_data_savecb(tag_specific_type_t type, tag_data_buffer_t *buffer) {
    return m_tag_type == TAG_TYPE_IDTECK ? LF_IDTECK_TAG_ID_SIZE : 0;
}

/** @brief IDTECK default frame: preamble "IDTK" + 32-bit placeholder card data. */
bool lf_tag_idteck_data_factory(uint8_t slot, tag_specific_type_t tag_type) {
    uint8_t tag_id[LF_IDTECK_TAG_ID_SIZE] = {
        0x49, 0x44, 0x54, 0x4B,   // "IDTK" preamble (MSB first)
        0xDE, 0xAD, 0xBE, 0xEF,   // default card data
    };
    return lf_tag_data_factory(slot, tag_type, tag_id, sizeof(tag_id));
}
