# crate: the holy crate (Louder-ESP32-S3 Plus, battery, no Wi-Fi).
# Shared sources from src/platform/ that this firmware uses.
set(APP_PLATFORM_SRCS
    amp_io.c
    app_status.c
    events.c
    input_task.c
    mp3_decoder.c
    player_task.c
)
