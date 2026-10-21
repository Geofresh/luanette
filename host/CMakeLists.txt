# The engine on a desktop: cartrun (runs a cart's frames, for check.py's previews and the parity
# test) and sfx_wav (the built-in sounds as WAV files).
#   cmake -S host -B host/build && cmake --build host/build
cmake_minimum_required(VERSION 3.16)
project(carts_host C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(ROOT "${CMAKE_CURRENT_LIST_DIR}/..")
file(GLOB LUA_SRCS "${ROOT}/engine/lua/*.c")
list(FILTER LUA_SRCS EXCLUDE REGEX ".*/(lua|luac|onelua)\\.c$")
file(GLOB JET_SRCS "${ROOT}/engine/jet/*.cpp")
file(READ "${ROOT}/engine/ui.lua" HG_UI_LUA)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${ROOT}/engine/ui.lua")
configure_file(${ROOT}/engine/ui_lua.hpp.in gen/ui_lua.hpp @ONLY)

if(NOT WIN32)  # cartrun's frame deadline is a POSIX timer signal
  add_executable(cartrun ${ROOT}/engine/host/cartrun.cpp ${ROOT}/engine/cart.cpp ${ROOT}/engine/cartsrc.cpp ${ROOT}/engine/synth.cpp ${ROOT}/engine/stage.cpp
                 ${ROOT}/engine/hd.cpp ${ROOT}/engine/hd_fonts.cpp ${ROOT}/support/src/json.cpp
                 ${ROOT}/support/src/crypto.cpp ${LUA_SRCS} ${JET_SRCS})
  target_include_directories(cartrun PRIVATE ${ROOT}/engine ${ROOT}/engine/lua ${ROOT}/engine/jet ${ROOT}/support
                             "${CMAKE_CURRENT_BINARY_DIR}/gen")
  target_compile_definitions(cartrun PRIVATE LUA_32BITS=1)
  target_link_libraries(cartrun PRIVATE m)
endif()

add_executable(sfx_wav ${ROOT}/engine/host/sfx_wav.cpp ${ROOT}/engine/sfx.cpp)
target_include_directories(sfx_wav PRIVATE ${ROOT}/engine)

# The host tests: the synth, comment stripping, and the battery gauge's percent and charging logic.
add_executable(host_tests ${ROOT}/tests/main.cpp ${ROOT}/tests/test_sfx.cpp ${ROOT}/tests/test_synth.cpp ${ROOT}/tests/test_battery_curve.cpp
               ${ROOT}/tests/test_cartsrc.cpp ${ROOT}/engine/sfx.cpp ${ROOT}/engine/synth.cpp ${ROOT}/engine/cartsrc.cpp ${ROOT}/board/axp2101.cpp)
target_include_directories(host_tests PRIVATE ${ROOT}/tests ${ROOT}/engine ${ROOT}/board ${ROOT}/support)

# The 3D stage's test scene on the desktop, band by band as the board draws it (stagerun [segs] [out.ppm]).
add_executable(stagerun ${ROOT}/engine/host/stagerun.cpp ${ROOT}/engine/stage.cpp ${JET_SRCS})
target_include_directories(stagerun PRIVATE ${ROOT}/engine ${ROOT}/engine/jet ${ROOT}/support)
