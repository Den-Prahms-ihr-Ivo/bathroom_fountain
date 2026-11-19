# One codebase, one firmware per node. APP picks the node:
#
#   bath | bedroom_speaker | bedside | gun | target
#
# PlatformIO passes it per environment (board_build.cmake_extra_args = -DAPP=...
# in platformio.ini). Each app lives in src/app/<APP>/ and lists the shared
# ESP-IDF sources it uses from src/platform/ in its app.cmake.
if(NOT DEFINED APP)
    if(DEFINED ENV{BAS_APP})
        set(APP $ENV{BAS_APP})
    else()
        set(APP bath)
    endif()
endif()

set(SRC_DIR ${CMAKE_CURRENT_LIST_DIR})
set(APP_DIR ${SRC_DIR}/app/${APP})
if(NOT EXISTS ${APP_DIR}/app.cmake)
    message(FATAL_ERROR "unknown APP '${APP}': ${APP_DIR}/app.cmake not found")
endif()
message(STATUS "bathroom-ambience app: ${APP}")

set(APP_PLATFORM_SRCS "")
include(${APP_DIR}/app.cmake)

file(GLOB app_sources ${APP_DIR}/*.c)
foreach(src ${APP_PLATFORM_SRCS})
    list(APPEND app_sources ${SRC_DIR}/platform/${src})
endforeach()

idf_component_register(SRCS ${app_sources}
                       INCLUDE_DIRS ${SRC_DIR}/platform ${APP_DIR})
