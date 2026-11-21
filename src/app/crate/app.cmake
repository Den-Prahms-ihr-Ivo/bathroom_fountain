# crate: the holy crate (Louder-ESP32-S3 Plus, battery). Wi-Fi only while
# awake (lid open, or a music sync finishing with the lid closed).
# Shared sources from src/platform/ that this firmware uses.
set(APP_PLATFORM_SRCS
    amp_io.c
    app_settings.c
    app_status.c
    cfg_store.c
    enroll_task.c
    events.c
    input_task.c
    mp3_decoder.c
    mqtt_link.c
    music_sync.c
    player_task.c
    wifi.c
)
