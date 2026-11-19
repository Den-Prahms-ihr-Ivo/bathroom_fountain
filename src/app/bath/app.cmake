# bath: bathroom node on the Louder-ESP32-S3 Plus (or the DevKit on the bench).
# Shared sources from src/platform/ that this firmware uses.
set(APP_PLATFORM_SRCS
    amp_io.c
    app_settings.c
    app_status.c
    console_task.c
    events.c
    hue_task.c
    input_task.c
    mic_task.c
    mp3_decoder.c
    player_task.c
    wifi.c
)
