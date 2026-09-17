idf_component_register(SRCS "main.c" "board_audio.c" "setup_portal.c" "credentials.c" "live_voice.c" "assistant.c" "delegation.c" "wake_word.c" "activity_leds.c" "hangup_phrase.c" INCLUDE_DIRS "."
    REQUIRES esp_driver_i2s esp_driver_i2c esp_driver_usb_serial_jtag esp_timer esp_driver_gpio esp_driver_rmt bootloader_support
             esp_wifi esp_event esp_netif esp_http_server tcp_transport nvs_flash nvs_sec_provider efuse json mbedtls esp_partition spi_flash
    EMBED_FILES "setup.html")
