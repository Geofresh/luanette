-- title: Map memory
-- by: tests
-- about: API 8: a map's cells count against the cart's 1 MB, as its Lua memory does: 256 x 256 maps, until one is refused; then a level copied twelve times, the old copies let go
-- api: 8

SPRITES = { [1] = "11111111 12222221 12333321 12344321 12344321 12333321 12222221 11111111" }

local function big(fill)  -- 256 x 256 cells: 128 KB once read
  local row = string.rep(fill, 256)
  local t = {}
  for r = 1, 256 do t[r] = row end
  return t
end
local done = false
function _update()
  if done then return end
  done = true
  log(msize(big("1")))
  -- more 256 x 256 maps, kept alive, until one is refused: well before 16 (2 MB). (check.py counts with
  -- some room, so where it stops differs; that it stops, and what it says, don't.)
  local kept, why = {}, nil
  for i = 1, 16 do
    local m = big(i % 2 == 0 and "1" or ".")
    kept[i] = m
    local ok, err = pcall(msize, m)
    if not ok then why = err break end
  end
  log(why and (tostring(why):gsub("a 256 x 256 map", "a map")) or "never refused")
  -- levels started over and over: each a fresh copy, the last let go: the maps gone give their room back
  kept = nil
  local level = big("1")
  local n = 0
  for i = 1, 12 do
    MAP = {table.unpack(level)}
    if pcall(msize) then n = n + 1 end
  end
  log("levels read", n)
end

function _draw()
  cls(1)
  print("maps", 2, 2, 7)
end
