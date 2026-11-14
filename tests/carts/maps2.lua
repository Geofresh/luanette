-- title: Maps 2
-- by: tests
-- about: API 8: hex maps of sprites past 63, a second map drawn behind, a 200 x 150 map, mset into rows, tline on another map, the errors
-- api: 8

SPRITES = {
  [1] = "11111111 12222221 12333321 12344321 12344321 12333321 12222221 11111111",
  [2] = "00000000 0aaaaaa0 0a0000a0 0a0bb0a0 0a0bb0a0 0a0000a0 0aaaaaa0 00000000",
  [70] = "88888888 80000008 80888808 80800808 80800808 80888808 80000008 88888888",
  [200] = "cccccccc c000000c c0cccc0c c0c00c0c c0c00c0c c0cccc0c c000000c cccccccc",
  [255] = "e0e0e0e0 0e0e0e0e e0e0e0e0 0e0e0e0e e0e0e0e0 0e0e0e0e e0e0e0e0 0e0e0e0e",
}

-- the main map, in hex: two digits a cell, ".." empty
MAP = {
  hex = true,
  "0146c8ff..01",
  "..ff46c801..",
  "c8c8",
}
-- a background map behind it, a character a cell
BG = {
  "1212121212121212",
  "2121212121212121",
  "1212121212121212",
  "2121212121212121",
}
-- a big one: 200 x 150
BIG = {hex = true}
for r = 1, 150 do
  local row = {}
  for c = 1, 200 do row[c] = (r + c) % 7 == 0 and "46" or (r * c) % 11 == 0 and "c8" or ".." end
  BIG[r] = table.concat(row)
end

local t = 0
function _update()
  t = t + 1
  if t == 1 then
    local w, h = msize()
    local bw, bh = msize(BG)
    local gw, gh = msize(BIG)
    log(w, h, bw, bh, gw, gh, mget(2, 0), mget(4, 0), mget(5, 0), mget(6, 0), mget(1, 1, BG), mget(199, 149, BIG))
    log(pcall(mset, 0, 0, 256))
    log(pcall(mset, 0, 0, 64, BG))
    log(pcall(mset, 99, 0, 1))
    log(pcall(map, 0, 0, 0, 0, 1, 1, 0, 5))
    log(pcall(mget, 0, 0, {hex = true, "0g"}))
    log(pcall(mget, 0, 0, {hex = true, "012"}))
    log(pcall(mget, 0, 0, {"1#"}))
  end
  if t == 5 then
    mset(1, 0, 200)
    mset(4, 2, 255)  -- past the end of a short row: it grows
    mset(0, 0)
    mset(3, 3, 1, BG)
    log(MAP[1], MAP[3], BG[4])
  end
  if t == 10 then MAP[2] = "c8c8c8c8c8c8" end  -- a row changed in place: read again next frame
end

function _draw()
  cls(5)
  map(0, 0, 0, 0, 16, 4, 0, BG)
  map(0, 0, 4, 4)
  for y = 0, 15 do tline(0, 40 + y, 127, 40 + y, (t % 64) / 8, y / 8, 1 / 8, 0, 0, BG) end
  camera(t * 3 % 1400, t % 900)
  map(0, 0, 0, 60, 200, 150, 0, BIG)
  camera()
  print(mget(1, 0) or "-", 100, 4, 7)
end
