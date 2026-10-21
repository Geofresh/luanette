set(ROOT "${CMAKE_CURRENT_LIST_DIR}/../..")

if(NOT CMAKE_SCRIPT_MODE_FILE)
    # The helpers loaded before every cart (engine/ui.lua), as a string in ui_lua.hpp.
    file(READ "${ROOT}/engine/ui.lua" HG_UI_LUA)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${ROOT}/engine/ui.lua")
    configure_file(${ROOT}/engine/ui_lua.hpp.in ui_lua.hpp @ONLY)
endif()

# Lua 5.4 with 32-bit numbers (the S3's FPU is single precision), and Jet for the 3D stage.
file(GLOB LUA_SRCS "${ROOT}/engine/lua/*.c")
file(GLOB JET_SRCS "${ROOT}/engine/jet/*.cpp")
# luac.c and lua.c are Lua's own programs, not the library.
list(FILTER LUA_SRCS EXCLUDE REGEX ".*/(lua|luac|onelua)\\.c$")

idf_component_register(
    SRCS "main.cpp" "console.cpp" "usb_link.cpp" "builtin_carts.cpp"
         "${ROOT}/board/board.cpp" "${ROOT}/board/panel.cpp" "${ROOT}/board/touch.cpp" "${ROOT}/board/buttons.cpp"
         "${ROOT}/board/speaker.cpp" "${ROOT}/board/axp2101.cpp"
         "${ROOT}/engine/cart.cpp" "${ROOT}/engine/cartsrc.cpp" "${ROOT}/engine/hd.cpp" "${ROOT}/engine/hd_fonts.cpp" "${ROOT}/engine/stage.cpp"
         "${ROOT}/engine/sfx.cpp" "${ROOT}/engine/synth.cpp" "${ROOT}/engine/hd_pie.S" "${ROOT}/engine/stage_pie.S"
         "${ROOT}/engine/esp32/cart_player.cpp" "${ROOT}/engine/esp32/cart_store.cpp"
         "${ROOT}/engine/esp32/scan_sync.cpp" "${ROOT}/engine/esp32/sound.cpp"
         "${ROOT}/support/src/json.cpp" "${ROOT}/support/src/crypto.cpp"
         ${LUA_SRCS} ${JET_SRCS}
    INCLUDE_DIRS "." "${ROOT}/board" "${ROOT}/engine" "${ROOT}/engine/esp32" "${ROOT}/support" "${CMAKE_CURRENT_BINARY_DIR}"
    PRIV_INCLUDE_DIRS "${ROOT}/engine/lua" "${ROOT}/engine/jet"
    EMBED_TXTFILES "${ROOT}/engine/builtin/launcher.lua"
                   "${ROOT}/examples/lockload.lua" "${ROOT}/examples/bubbles.lua" "${ROOT}/examples/night_swarm.lua"
                   "${ROOT}/examples/dice.lua" "${ROOT}/examples/mochi.lua" "${ROOT}/examples/overdrive.lua"
    PRIV_REQUIRES
        driver esp_driver_gpio esp_driver_i2c esp_driver_i2s esp_driver_spi esp_driver_gptimer
        esp_driver_usb_serial_jtag esp_lcd esp_partition esp_timer esp_pm esp_app_format nvs_flash
        perfmon esp_psram
)
target_compile_definitions(${COMPONENT_LIB} PRIVATE LUA_32BITS=1 HD_PIE=1)
# The 3D stage's per-frame pipeline and span loop: speed over size.
set_source_files_properties("${ROOT}/engine/stage.cpp" PROPERTIES COMPILE_OPTIONS "-O2")
