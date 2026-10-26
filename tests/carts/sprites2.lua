-- title: Sprites 2
-- by: tests
-- about: API 6: sprites past 63, spr's blocks and flips, sspr stretched and shrunk, palt, flags and map layers
-- api: 6

SPRITES = {
  [1] = "11111111 12222221 12333321 12344321 12344321 12333321 12222221 11111111",
  [2] = "00000000 0aaaaaa0 0a0000a0 0a0bb0a0 0a0bb0a0 0a0000a0 0aaaaaa0 00000000",
  [3] = "88000000 88000000 00000000 00000000 00000000 00000000 00000000 0000000c",
  [4] = "eeeeeeee e000000e e0e00e0e e000000e e0e00e0e e00ee00e e000000e eeeeeeee",
  [100] = "77777777 70000007 70777707 70700707 70700707 70777707 70000007 77777777",
  [101] = "99999999 90000009 90999909 90900909 90900909 90999909 90000009 99999999",
  [102] = "cccccccc c000000c c0cccc0c c0c00c0c c0c00c0c c0cccc0c c000000c cccccccc",
  [103] = "56565656 65656565 56565656 65656565 56565656 65656565 56565656 65656565",
}
FLAGS = { [1] = 1, [2] = 2 }

MAP = {
  "1234.12",
  "2.3412.",
}

local t = 0
function _update()
  t = t + 1
  if t == 1 then
    log(fget(1), fget(1, 0), fget(1, 1), fget(2), fget(3), fget(200))
    fset(3, 2, true)
    fset(3, 0, true)
    fset(3, 2, false)
    fset(4, 6)
    log(fget(3), fget(4), fget(4, 1), FLAGS[3], FLAGS[4])
  end
end

function _draw()
  cls(1)
  palt()
  -- blocks, plain and flipped both ways
  spr(100, 2, 2, 2, 2)
  spr(100, 22, 2, 2, 2, true, false)
  spr(100, 42, 2, 2, 2, false, true)
  spr(100, 62, 2, 2, 1, true, true)
  spr(3, 82, 2, true)
  spr(3, 92, 2, false, true)
  -- stretched, shrunk, flipped, a block, and partly off the left edge and the clip
  sspr(3, 2, 22, 24, 16)
  sspr(4, 30, 22, 5, 7)
  sspr(3, 40, 22, 16, 16, true, true)
  sspr(100, 60, 22, 30, 14, false, false, 2, 1)
  sspr(4, -6, 44, 20, 20)
  clip(100, 20, 20, 20)
  sspr(102, 96, 18, 30, 30)
  clip()
  -- palt: 0 drawn, a and e left out
  palt(0, false)
  palt(10, true)
  palt(14, true)
  spr(2, 2, 70)
  spr(4, 12, 70)
  sspr(2, 22, 70, 12, 12)
  palt()
  spr(2, 40, 70)
  -- the map: everything, layer 1, layer 2, layer 4, with the camera moved
  map(0, 0, 2, 90)
  map(0, 0, 62, 90, 7, 2, 1)
  map(0, 0, 2, 108, 7, 2, 2)
  camera(-3, -2)
  map(0, 0, 62, 106, 7, 2, 4 | 1)
  camera()
  print(t % 10, 120, 120, 7)
end
