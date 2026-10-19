#include <string>

#include "cartsrc.hpp"
#include "check.hpp"

TEST("cartsrc: comments out, header lines and line breaks kept, strings untouched") {
  const std::string src =
      "-- title: T\n-- by: x\n-- a plain comment\nlocal s = \"a--b\" -- c\n"
      "local t = [==[x]]--]==] --[[ block\nline]] y = 1  \n--[=[\n]=]z=2";
  CHECK_EQ(carts::shipped(src), std::string("-- title: T\n-- by: x\n\nlocal s = \"a--b\"\nlocal t = [==[x]]--]==]\n y = 1\n\nz=2"));
}

TEST("cartsrc: a header key only counts at the start of a line") {
  CHECK_EQ(carts::shipped("x = 1 -- title: no\n  -- about: yes"), std::string("x = 1\n  -- about: yes"));
}

TEST("cartsrc: unterminated strings and comments run to the end") {
  CHECK_EQ(carts::shipped("a = 'x -- y"), std::string("a = 'x -- y"));
  CHECK_EQ(carts::shipped("a = 1 --[[ open\nstill"), std::string("a = 1\n"));
}
