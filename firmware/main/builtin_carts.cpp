// The carts this firmware carries: the examples (examples/*.lua, embedded by the build), listed in
// the launcher after any sent ones. A sent cart of the same name takes its place.
#include "cart_store.hpp"

extern "C" const char lockload_lua[] asm("_binary_lockload_lua_start");
extern "C" const char bubbles_lua[] asm("_binary_bubbles_lua_start");
extern "C" const char night_swarm_lua[] asm("_binary_night_swarm_lua_start");
extern "C" const char dice_lua[] asm("_binary_dice_lua_start");
extern "C" const char mochi_lua[] asm("_binary_mochi_lua_start");

namespace carts::store::builtin {
const Builtin kAll[] = {{"lockload", lockload_lua},
                        {"bubbles", bubbles_lua},
                        {"night_swarm", night_swarm_lua},
                        {"dice", dice_lua},
                        {"mochi", mochi_lua}};
const size_t kCount = sizeof(kAll) / sizeof(kAll[0]);
}  // namespace carts::store::builtin
