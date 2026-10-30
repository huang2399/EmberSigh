idf_component_register(SRCS "pdm_mic_volume.c" "hal_hardware.c" "audio_player.c"
                     PRIV_REQUIRES esp32_s3_eye led_strip
                     INCLUDE_DIRS .
                     EMBED_FILES forest_audio.wav)