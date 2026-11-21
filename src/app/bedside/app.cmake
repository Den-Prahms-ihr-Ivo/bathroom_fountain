# bedside: bedside node (ESP32-S3): owns time, alarms, lights, buttons, display; game master.
# Skeleton: buttons, modes, candles, encoder volume, speaker link, server link.
# Alarm, light ramp, display, game: TODO (docs/ARCHITECTURE.md).
set(APP_PLATFORM_SRCS
    app_settings.c
    app_status.c
    cfg_store.c
    enroll_task.c
    espnow_link.c
    events.c
    input_task.c
    mqtt_link.c
    wifi.c
)
