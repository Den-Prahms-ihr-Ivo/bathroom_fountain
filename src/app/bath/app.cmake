# bath: bathroom node on the Louder-ESP32-S3 Plus (or the DevKit on the bench).
# Shared sources from src/platform/ that this firmware uses.
set(APP_PLATFORM_SRCS
    amp_io.c
    app_settings.c
    app_status.c
    cfg_store.c
    console_task.c
    enroll_task.c
    events.c
    expander_io.c
    hue_task.c
    input_task.c
    mic_task.c
    mp3_decoder.c
    mqtt_link.c
    music_sync.c
    player_task.c
    server_link.c
    wifi.c
)
