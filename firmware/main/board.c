// Board layer: rail sequencing, the 74HC165 chain on SPI3, ADC1 oversampling,
// PWM outputs, and deep sleep. Pin numbers come from shared/pinmap.json
// through pins_gen.h. Nobody has run this on hardware.
#include <stdlib.h>
#include <string.h>

#include "app.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_continuous.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "pins_gen.h"
#include "soc/soc_caps.h"

static const char *TAG = "board";

// Swap these if a trigger motor turns the wrong way on the real mechanism.
#define MOTOR_INVERT_L 0
#define MOTOR_INVERT_R 0

#define ADC_CHANNELS 8
#define ADC_OVERSAMPLE 8 // samples per channel per millisecond
#define ADC_FRAME_BYTES (ADC_CHANNELS * ADC_OVERSAMPLE * SOC_ADC_DIGI_RESULT_BYTES)
#define ADC_SLOT_ISEN 6
#define ISEN_MA_PER_MV 5 // 0.2 ohm sense resistor

#define PWM_HZ 20000
#define PWM_FULL 256 // 8-bit timer: 256 holds the output high
enum { CH_L_IN1, CH_L_IN2, CH_R_IN1, CH_R_IN2, CH_RUMBLE_L, CH_RUMBLE_R, CH_BACKLIGHT, CH_COUNT };

// Sticks and triggers first, in KS_AX_* order, then the two current channels.
static const uint8_t adc_channel[ADC_CHANNELS] = {
    ADC_CH_STICK_LX,   ADC_CH_STICK_LY,   ADC_CH_STICK_RX,    ADC_CH_STICK_RY,
    ADC_CH_TRIG_L_POS, ADC_CH_TRIG_R_POS, ADC_CH_TRIG_L_ISEN, ADC_CH_TRIG_R_ISEN,
};

static spi_device_handle_t sr_dev;
static adc_continuous_handle_t adc;
static adc_cali_handle_t isen_cali;
static uint16_t adc_avg[ADC_CHANNELS]; // last 12-bit average times 16
static uint16_t pwm_now[CH_COUNT];
static uint8_t motor_duty[2];

static void pwm_set(int channel, uint16_t duty)
{
    if (pwm_now[channel] != duty) {
        pwm_now[channel] = duty;
        ledc_set_duty(LEDC_LOW_SPEED_MODE, channel, duty);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, channel);
    }
}

static void pwm_init(void)
{
    static const int gpio[CH_COUNT] = {
        PIN_TRIG_L_IN1, PIN_TRIG_L_IN2, PIN_TRIG_R_IN1, PIN_TRIG_R_IN2,
        PIN_RUMBLE_L,   PIN_RUMBLE_R,   PIN_LCD_BL,
    };
    ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .timer_num = LEDC_TIMER_0,
        .duty_resolution = LEDC_TIMER_8_BIT,
        .freq_hz = PWM_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&timer));
    for (int ch = 0; ch < CH_COUNT; ch++) {
        ledc_channel_config_t cfg = {
            .gpio_num = gpio[ch],
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .channel = ch,
            .timer_sel = LEDC_TIMER_0,
        };
        ESP_ERROR_CHECK(ledc_channel_config(&cfg));
    }
}

static void sr_init(void)
{
    spi_bus_config_t bus = {
        .mosi_io_num = -1,
        .miso_io_num = PIN_SR_DATA,
        .sclk_io_num = PIN_SR_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
    };
    // Mode 2 idles the clock high and samples on the falling edge. The 74HC165
    // shifts on the rising edge, so each bit has half a clock period to settle
    // and the first sample is input H, which is present before any clock.
    spi_device_interface_config_t dev = {
        .clock_speed_hz = 2 * 1000 * 1000,
        .mode = 2,
        .spics_io_num = -1,
        .queue_size = 1,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &bus, SPI_DMA_DISABLED));
    ESP_ERROR_CHECK(spi_bus_add_device(SPI3_HOST, &dev, &sr_dev));
    // The chain is the only device on the bus. Holding the bus keeps the
    // clock at its idle level between reads and skips the lock on each one.
    ESP_ERROR_CHECK(spi_device_acquire_bus(sr_dev, portMAX_DELAY));
}

static void sr_read(uint8_t out[3])
{
    spi_transaction_t t = {.length = 24, .rxlength = 24, .flags = SPI_TRANS_USE_RXDATA};
    gpio_set_level(PIN_SR_LOAD, 0); // latch the inputs
    esp_rom_delay_us(1);
    gpio_set_level(PIN_SR_LOAD, 1);
    esp_rom_delay_us(1);
    if (spi_device_polling_transmit(sr_dev, &t) == ESP_OK) {
        memcpy(out, t.rx_data, 3);
    } else {
        memset(out, 0xFF, 3); // nothing pressed
    }
}

static void adc_init(void)
{
    adc_continuous_handle_cfg_t handle_cfg = {
        .max_store_buf_size = ADC_FRAME_BYTES * 4,
        .conv_frame_size = ADC_FRAME_BYTES,
    };
    adc_digi_pattern_config_t pattern[ADC_CHANNELS];
    for (int i = 0; i < ADC_CHANNELS; i++) {
        // The pin map asks for 12 dB on the sticks (3.0 V rail) and trigger
        // Hall sensors (3.3 V rail), and 0 dB on the sense resistors, whose
        // signal stays under 0.2 V.
        pattern[i] = (adc_digi_pattern_config_t){
            .atten = i < ADC_SLOT_ISEN ? ADC_ATTEN_DB_12 : ADC_ATTEN_DB_0,
            .channel = adc_channel[i],
            .unit = ADC_UNIT_1,
            .bit_width = SOC_ADC_DIGI_MAX_BITWIDTH,
        };
        adc_avg[i] = 2048 * 16;
    }
    adc_avg[ADC_SLOT_ISEN] = adc_avg[ADC_SLOT_ISEN + 1] = 0;
    adc_continuous_config_t cfg = {
        .pattern_num = ADC_CHANNELS,
        .adc_pattern = pattern,
        .sample_freq_hz = ADC_CHANNELS * ADC_OVERSAMPLE * 1000,
        .conv_mode = ADC_CONV_SINGLE_UNIT_1,
        .format = ADC_DIGI_OUTPUT_FORMAT_TYPE2,
    };
    ESP_ERROR_CHECK(adc_continuous_new_handle(&handle_cfg, &adc));
    ESP_ERROR_CHECK(adc_continuous_config(adc, &cfg));
    ESP_ERROR_CHECK(adc_continuous_start(adc));

    adc_cali_curve_fitting_config_t cali = {
        .unit_id = ADC_UNIT_1,
        .atten = ADC_ATTEN_DB_0,
        .bitwidth = SOC_ADC_DIGI_MAX_BITWIDTH,
    };
    if (adc_cali_create_scheme_curve_fitting(&cali, &isen_cali) != ESP_OK) {
        ESP_LOGW(TAG, "no ADC calibration in eFuse, motor current will be approximate");
        isen_cali = NULL;
    }
}

// Averages every conversion that has arrived since the last call.
static void adc_read(void)
{
    static uint8_t buf[ADC_FRAME_BYTES * 2];
    uint32_t sum[ADC_CHANNELS] = {0}, count[ADC_CHANNELS] = {0}, got = 0;
    while (adc_continuous_read(adc, buf, sizeof buf, &got, 0) == ESP_OK && got > 0) {
        for (uint32_t i = 0; i + SOC_ADC_DIGI_RESULT_BYTES <= got; i += SOC_ADC_DIGI_RESULT_BYTES) {
            const adc_digi_output_data_t *d = (const adc_digi_output_data_t *)&buf[i];
            if (d->type2.unit != 0) {
                continue;
            }
            for (int slot = 0; slot < ADC_CHANNELS; slot++) {
                if (adc_channel[slot] == d->type2.channel) {
                    sum[slot] += d->type2.data;
                    count[slot]++;
                    break;
                }
            }
        }
    }
    for (int slot = 0; slot < ADC_CHANNELS; slot++) {
        if (count[slot]) {
            adc_avg[slot] = (uint16_t)(sum[slot] * 16 / count[slot]);
        }
    }
}

void board_init(void)
{
    // GPIO45 and GPIO46 are strapping pins with external pull-downs. Drive
    // them as plain outputs and never enable a pull-up on them.
    gpio_hold_dis(PIN_PERIPH_EN);
    gpio_hold_dis(PIN_MOTOR_NSLEEP);
    gpio_deep_sleep_hold_dis();
    gpio_config_t outputs = {
        .pin_bit_mask = (1ULL << PIN_PERIPH_EN) | (1ULL << PIN_MOTOR_NSLEEP) | (1ULL << PIN_SR_LOAD),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
    };
    gpio_set_level(PIN_PERIPH_EN, 0);
    gpio_set_level(PIN_MOTOR_NSLEEP, 0);
    gpio_set_level(PIN_SR_LOAD, 1);
    ESP_ERROR_CHECK(gpio_config(&outputs));

    // Both inputs have external pull-ups.
    gpio_config_t inputs = {
        .pin_bit_mask = (1ULL << PIN_BTN_HOME) | (1ULL << PIN_FUEL_ALRT_N),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&inputs));

    // PWM outputs start at zero, so the motor inputs and the rumble gates are
    // low before any rail comes up.
    pwm_init();

    // PERIPH_EN switches 3V3_PERIPH, the 3.0 V analogue LDO, and the LED
    // rail. Give them time to settle before sampling or talking to anything.
    gpio_set_level(PIN_PERIPH_EN, 1);
    vTaskDelay(pdMS_TO_TICKS(20));

    sr_init();
    adc_init();
    uint8_t discard[3];
    sr_read(discard); // the first read also sets the clock's idle level

    // Wake the DRV8833 last, with both of its bridges commanded off.
    gpio_set_level(PIN_MOTOR_NSLEEP, 1);
    vTaskDelay(pdMS_TO_TICKS(2));
    ESP_LOGI(TAG, "rails up");
}

void board_read(board_sample_t *s)
{
    sr_read(s->sr);
    s->home_low = gpio_get_level(PIN_BTN_HOME) == 0;
    adc_read();
    memcpy(s->analog, adc_avg, sizeof s->analog);
    for (int i = 0; i < 2; i++) {
        int raw = adc_avg[ADC_SLOT_ISEN + i] / 16, mv = raw * 950 / 4095;
        if (isen_cali) {
            adc_cali_raw_to_voltage(isen_cali, raw, &mv);
        }
        // Current flows through the sense resistor only while the bridge
        // drives, so the filtered reading is the motor current times the duty.
        int ma = mv * ISEN_MA_PER_MV;
        if (motor_duty[i] >= 32) {
            ma = ma * 255 / motor_duty[i];
        }
        s->isen_ma[i] = (uint16_t)(ma < 0 ? 0 : ma > 65535 ? 65535 : ma);
    }
}

void board_motor(int index, int16_t duty)
{
    int in1 = index ? CH_R_IN1 : CH_L_IN1, in2 = in1 + 1;
    if (index ? MOTOR_INVERT_R : MOTOR_INVERT_L) {
        duty = (int16_t)-duty;
    }
    duty = (int16_t)(duty > 255 ? 255 : duty < -255 ? -255 : duty);
    motor_duty[index] = (uint8_t)abs(duty);
    if (duty == 0) {
        // Both inputs low lets the bridge coast. The worm holds the arm.
        pwm_set(in1, 0);
        pwm_set(in2, 0);
    } else if (duty > 0) {
        // Slow decay: drive for the duty, brake for the rest of the period.
        pwm_set(in1, PWM_FULL);
        pwm_set(in2, (uint16_t)(PWM_FULL - duty));
    } else {
        pwm_set(in1, (uint16_t)(PWM_FULL + duty));
        pwm_set(in2, PWM_FULL);
    }
}

void board_rumble(ks_rumble_t rumble)
{
    pwm_set(CH_RUMBLE_L, rumble.left);
    pwm_set(CH_RUMBLE_R, rumble.right);
}

void board_backlight(uint8_t pct)
{
    pwm_set(CH_BACKLIGHT, (uint16_t)((pct > 100 ? 100 : pct) * PWM_FULL / 100));
}

void board_sleep(void)
{
    ESP_LOGI(TAG, "sleeping");
    board_motor(0, 0);
    board_motor(1, 0);
    board_rumble((ks_rumble_t){0, 0});
    board_backlight(0);
    gpio_set_level(PIN_MOTOR_NSLEEP, 0);
    vTaskDelay(pdMS_TO_TICKS(5));
    gpio_set_level(PIN_PERIPH_EN, 0);

    // A held Home button would wake the pad at once, so wait for its release.
    for (int i = 0; i < 300 && gpio_get_level(PIN_BTN_HOME) == 0; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    // Keep both enables low through deep sleep. Their pull-downs do the same
    // job if the hold is ever lost.
    gpio_hold_en(PIN_PERIPH_EN);
    gpio_hold_en(PIN_MOTOR_NSLEEP);
    gpio_deep_sleep_hold_en();
    // BTN_HOME is an RTC pin with an external pull-up, so ext1 can watch it
    // with the RTC peripherals powered down.
    esp_sleep_enable_ext1_wakeup(1ULL << PIN_BTN_HOME, ESP_EXT1_WAKEUP_ANY_LOW);
    esp_deep_sleep_start();
}
