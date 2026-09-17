// Wiring and RMNM channel ordering verified against Waveshare's esp_sr_02 example.
#include "board_audio.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_io_expander_tca95xx_16bit.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static esp_codec_dev_handle_t adc, dac;
esp_err_t board_audio_volume(int volume) { return esp_codec_dev_set_out_vol(dac, volume); }
esp_err_t board_audio_gain(int gain) { return esp_codec_dev_set_in_gain(adc, gain); }

esp_err_t board_audio_init(void) {
    i2c_master_bus_handle_t bus;
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = 0, .sda_io_num = 11, .scl_io_num = 10,
        .clk_source = I2C_CLK_SRC_DEFAULT, .flags.enable_internal_pullup = true,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &bus));
    esp_io_expander_handle_t expander;
    ESP_ERROR_CHECK(esp_io_expander_new_i2c_tca95xx_16bit(bus,
        ESP_IO_EXPANDER_I2C_TCA9555_ADDRESS_000, &expander));
    ESP_ERROR_CHECK(esp_io_expander_set_dir(expander, IO_EXPANDER_PIN_NUM_8, IO_EXPANDER_OUTPUT));
    ESP_ERROR_CHECK(esp_io_expander_set_level(expander, IO_EXPANDER_PIN_NUM_8, 0));

    i2s_chan_handle_t tx, rx;
    i2s_chan_config_t channel = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    // Match one DMA block to one 20 ms application frame. The default 511-frame
    // blocks make consecutive 320-frame writes alternate between bursts and waits.
    channel.dma_frame_num = AUDIO_SAMPLES;
    channel.auto_clear = true;
    ESP_ERROR_CHECK(i2s_new_channel(&channel, &tx, &rx));
    i2s_std_config_t standard = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(16000),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(32, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {.mclk = 12, .bclk = 13, .ws = 14, .dout = 16, .din = 15},
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx, &standard));
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx, &standard));
    ESP_ERROR_CHECK(i2s_channel_enable(tx));
    ESP_ERROR_CHECK(i2s_channel_enable(rx));

    audio_codec_i2s_cfg_t input_data = {.port = I2S_NUM_1, .rx_handle = rx};
    audio_codec_i2c_cfg_t input_control = {.addr = ES7210_CODEC_DEFAULT_ADDR, .bus_handle = bus};
    es7210_codec_cfg_t input_codec = {
        .ctrl_if = audio_codec_new_i2c_ctrl(&input_control),
        .mic_selected = ES7210_SEL_MIC1 | ES7210_SEL_MIC2 | ES7210_SEL_MIC3 | ES7210_SEL_MIC4,
    };
    esp_codec_dev_cfg_t input = {.codec_if = es7210_codec_new(&input_codec),
        .data_if = audio_codec_new_i2s_data(&input_data), .dev_type = ESP_CODEC_DEV_TYPE_IN};
    adc = esp_codec_dev_new(&input);
    audio_codec_i2s_cfg_t output_data = {.port = I2S_NUM_1, .tx_handle = tx};
    audio_codec_i2c_cfg_t output_control = {.addr = ES8311_CODEC_DEFAULT_ADDR, .bus_handle = bus};
    es8311_codec_cfg_t output_codec = {
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        .ctrl_if = audio_codec_new_i2c_ctrl(&output_control),
        .gpio_if = audio_codec_new_gpio(), .pa_pin = -1, .use_mclk = false,
    };
    esp_codec_dev_cfg_t output = {.codec_if = es8311_codec_new(&output_codec),
        .data_if = audio_codec_new_i2s_data(&output_data), .dev_type = ESP_CODEC_DEV_TYPE_OUT};
    dac = esp_codec_dev_new(&output);
    if (!adc || !dac) return ESP_ERR_NO_MEM;
    esp_codec_dev_sample_info_t format = {.sample_rate = 16000, .channel = 2, .bits_per_sample = 32};
    ESP_ERROR_CHECK(esp_codec_dev_open(adc, &format));
    ESP_ERROR_CHECK(esp_codec_dev_open(dac, &format));
    ESP_ERROR_CHECK(esp_codec_dev_set_in_gain(adc, 24.0));
    ESP_ERROR_CHECK(esp_codec_dev_set_out_vol(dac, 40));
    ESP_ERROR_CHECK(esp_io_expander_set_level(expander, IO_EXPANDER_PIN_NUM_8, 1));
    vTaskDelay(pdMS_TO_TICKS(10));
    return ESP_OK;
}

esp_err_t board_audio_read(int16_t raw[AUDIO_SAMPLES * 4]) {
    return esp_codec_dev_read(adc, raw, AUDIO_SAMPLES * 4 * sizeof(int16_t));
}

esp_err_t board_audio_write(const int16_t mono[AUDIO_SAMPLES]) {
    int32_t stereo[AUDIO_SAMPLES * 2];
    for (int i = 0; i < AUDIO_SAMPLES; ++i)
        stereo[2*i] = stereo[2*i+1] = (int32_t)mono[i] * 65536;
    return esp_codec_dev_write(dac, stereo, sizeof(stereo));
}
