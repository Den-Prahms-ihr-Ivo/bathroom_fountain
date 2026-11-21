# bedroom_speaker: bedroom speaker node (Louder-ESP32-S3 Plus): plays sleep sounds, music and the alarm; follower of the bedside node.
# Skeleton: link from the bedside -> speaker_session -> player; server link.
set(APP_PLATFORM_SRCS
    amp_io.c
    app_settings.c
    app_status.c
    cfg_store.c
    enroll_task.c
    espnow_link.c
    events.c
    input_task.c
    mp3_decoder.c
    mqtt_link.c
    music_sync.c
    player_task.c
    wifi.c
)
