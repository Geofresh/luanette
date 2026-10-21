-- title: Sounds
-- by: tests
-- about: its own sounds (SOUNDS) and a song (MUSIC), and the built-in sounds, in turn
-- api: 3

SOUNDS = {
  [8] = "10 0000 18350 1c350 1f350 24355",
  [9] = "08 0004 0c650 0c650 13650 13650",
  [10] = "04 0000 30776 2b776 24773 18775",
  [63] = "02 0000 3f417 30413",
}
MUSIC = {
  [0] = "01 08 09 -- --",
  [1] = "02 08 09 0a --",
}

local plays = {8, 0, 10, 4, 63, 7}
local i, t = 0, 0

function _init()
  music(0)
end

function _update()
  t = t + 1
  if t % 10 == 1 then
    i = i % #plays + 1
    sfx(plays[i])
  end
  if t == 80 then sfx(-1) end
  if t == 85 then music(-1) end
end

function _draw()
  cls(1)
  print("sfx " .. plays[math.max(i, 1)], 4, 4, 7)
  for k = 1, #plays do rectfill(4 + k * 12, 60, 12 + k * 12, 68, k == i and 10 or 5) end
end
