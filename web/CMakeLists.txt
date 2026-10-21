# Luanette in a browser: the engine as WebAssembly, for the playground page (index.html).
#   emcmake cmake -S web -B web/build && cmake --build web/build    (web/build.sh does it all)
cmake_minimum_required(VERSION 3.16)
project(luanette_web C CXX)
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

add_executable(luanette ${CMAKE_CURRENT_LIST_DIR}/luanette_web.cpp ${ROOT}/engine/cart.cpp ${ROOT}/engine/cartsrc.cpp
               ${ROOT}/engine/stage.cpp ${ROOT}/engine/hd.cpp ${ROOT}/engine/hd_fonts.cpp ${ROOT}/engine/sfx.cpp ${ROOT}/engine/synth.cpp
               ${ROOT}/support/src/json.cpp ${ROOT}/support/src/crypto.cpp ${LUA_SRCS} ${JET_SRCS})
target_include_directories(luanette PRIVATE ${ROOT}/engine ${ROOT}/engine/lua ${ROOT}/engine/jet ${ROOT}/support
                           "${CMAKE_CURRENT_BINARY_DIR}/gen")
target_compile_definitions(luanette PRIVATE LUA_32BITS=1)
target_compile_options(luanette PRIVATE -O3)
target_link_options(luanette PRIVATE -O3 -sMODULARIZE=1 -sEXPORT_NAME=Luanette -sALLOW_MEMORY_GROWTH=1
                    -sENVIRONMENT=web "-sEXPORTED_RUNTIME_METHODS=['ccall','cwrap','HEAPU8','HEAP16','HEAPF32']"
                    "-sDEFAULT_LIBRARY_FUNCS_TO_INCLUDE=['$stringToNewUTF8','$UTF8ToString']")
