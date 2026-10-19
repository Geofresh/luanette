#include "cartsrc.hpp"

#include <cctype>

namespace carts {
namespace {

bool space(char c) { return std::isspace(static_cast<unsigned char>(c)) != 0; }

// "[" then any number of "=" then "[" at i: the level (number of '='), or -1.
int long_bracket(std::string_view s, size_t i) {
  if (i >= s.size() || s[i] != '[') return -1;
  size_t j = i + 1;
  while (j < s.size() && s[j] == '=') ++j;
  return j < s.size() && s[j] == '[' ? static_cast<int>(j - i - 1) : -1;
}

// Where the long bracket of this level closes, past its "]==]"; the end of s when it doesn't.
size_t long_end(std::string_view s, size_t from, int level) {
  const std::string close = "]" + std::string(static_cast<size_t>(level), '=') + "]";
  const size_t e = s.find(close, from);
  return e == std::string_view::npos ? s.size() : e + close.size();
}

// "--" then spaces then one of the header keys, spaces, ':'.
bool header_at(std::string_view s, size_t i) {
  size_t j = i + 2;
  while (j < s.size() && space(s[j])) ++j;
  for (std::string_view key : {"title", "by", "about", "api", "canvas"}) {
    if (s.substr(j, key.size()) != key) continue;
    size_t k = j + key.size();
    while (k < s.size() && space(s[k])) ++k;
    if (k < s.size() && s[k] == ':') return true;
  }
  return false;
}

}  // namespace

std::string shipped(std::string_view src) {
  std::string out;
  out.reserve(src.size());
  const size_t n = src.size();
  size_t i = 0;
  bool line_start = true;
  while (i < n) {
    const char c = src[i];
    if (c == '"' || c == '\'') {  // a quoted string, escapes included
      size_t j = i + 1;
      while (j < n && src[j] != c && src[j] != '\n') j += src[j] == '\\' ? 2 : 1;
      const size_t end = j + 1 < n ? j + 1 : n;
      out.append(src.substr(i, end - i));
      i = end;
      line_start = false;
      continue;
    }
    if (const int level = long_bracket(src, i); level >= 0) {  // a long string
      const size_t end = long_end(src, i, level);
      out.append(src.substr(i, end - i));
      i = end;
      line_start = false;
      continue;
    }
    if (src.substr(i, 2) == "--") {
      if (line_start && header_at(src, i)) {  // keep "-- title: ..." and friends
        size_t j = src.find('\n', i);
        if (j == std::string_view::npos) j = n;
        out.append(src.substr(i, j - i));
        i = j;
        continue;
      }
      if (const int level = long_bracket(src, i + 2); level >= 0) {  // a block comment: its line breaks stay
        const size_t end = long_end(src, i, level);
        for (size_t k = i; k < end; ++k)
          if (src[k] == '\n') out.push_back('\n');
        i = end;
      } else {
        const size_t j = src.find('\n', i);
        i = j == std::string_view::npos ? n : j;
      }
      continue;
    }
    out.push_back(c);
    if (c == '\n') line_start = true;
    else if (!space(c)) line_start = false;
    ++i;
  }
  // Trailing spaces off each line.
  std::string trimmed;
  trimmed.reserve(out.size());
  size_t start = 0;
  while (start <= out.size()) {
    size_t nl = out.find('\n', start);
    const bool last = nl == std::string::npos;
    if (last) nl = out.size();
    size_t e = nl;
    while (e > start && space(out[e - 1])) --e;
    trimmed.append(out, start, e - start);
    if (last) break;
    trimmed.push_back('\n');
    start = nl + 1;
  }
  return trimmed;
}

}  // namespace carts
