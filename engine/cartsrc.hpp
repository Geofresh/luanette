// A cart's source as it ships: comments out, the header lines kept (-- title:, by:, about:, api:,
// canvas:), trailing spaces off. Line breaks stay, so an error's line number is the source's.
// Strings, long [[...]] ones included, are left alone. The same rules as tools/cartsrc.py, which
// push.py and check.py use; the runtime applies it to a source over the size limit, so the limit
// never counts comments, whichever way a cart arrives.
#pragma once

#include <string>
#include <string_view>

namespace carts {

std::string shipped(std::string_view src);

}  // namespace carts
