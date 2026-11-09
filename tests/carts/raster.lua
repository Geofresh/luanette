-- title: Raster
-- by: tests
-- about: API 7: tline across, down and slanting, turned and zoomed, with a layer and palt; rowshift both ways; rowpal
-- api: 7

SPRITES = {
  [1] = "11111111 12222221 12333321 12344321 12344321 12333321 12222221 11111111",
  [2] = "00000000 0aaaaaa0 0a0000a0 0a0bb0a0 0a0bb0a0 0a0000a0 0aaaaaa0 00000000",
  [3] = "89abcdef 9abcdef8 abcdef89 bcdef89a cdef89ab def89abc ef89abcd f89abcde",
}
FLAGS = { [1] = 1, [2] = 2 }

MAP = {
  "12312312",
  "23.23.23",
  "3123..12",
  "12312312",
}

local t = 0
function _update()
  t = t + 1
  if t == 1 then
    log(pcall(rowpal, 3, 16, 0xff0000))
    log(pcall(rowpal, 3, 2, 0x1000000))
    log(pcall(rowpal, 3, nil, 0xff))
  end
end

function _draw()
  cls(5)
  palt()
  -- across, at the map's size and stretched
  for y = 0, 31 do
    tline(0, y, 63, y, 0, y / 8)
    tline(64, y, 127, y, t / 16, y / 16, 1 / 16, 0)
  end
  -- turned and zoomed: each row a line across the map at an angle
  -- (the turn from a rational point on the circle, not sin and cos: those differ in their last
  -- bits between the runtime and check.py, and this checks tline, not them)
  local u = (t % 40 - 20) / 20
  local ca, sa = (1 - u * u) / (1 + u * u), 2 * u / (1 + u * u)
  local z = 1 / 8 * (0.8 + (t % 30) / 30)
  for y = 0, 31 do
    local dy = y - 16
    local mx, my = 4 + (-32 * ca - dy * sa) * z, 2 + (-32 * sa + dy * ca) * z
    tline(0, 34 + y, 63, 34 + y, mx, my, ca * z, sa * z)
  end
  -- down, slanting, backwards, off the map, with a layer, with palt and the camera
  for x = 64, 95 do tline(x, 34, x, 65, (x - 64) / 8, 0, 0, 1 / 8) end
  tline(96, 34, 127, 65, 0, 0, 1 / 8, 1 / 16)
  tline(127, 40, 96, 40, 0, 1, 1 / 8, 0)
  tline(96, 44, 127, 44, -1, -0.5, 1 / 4, 0)
  for y = 66, 81 do tline(0, y, 127, y, 0, (y - 66) / 8, 1 / 16, 0, 2) end
  palt(1, true)
  palt(0, false)
  for y = 82, 97 do tline(0, y, 127, y, 0.5, (y - 82) / 8, 1 / 16, 0) end
  palt()
  camera(4, -2)
  for y = 98, 105 do tline(0, y, 200, y, 0, (y - 98) / 8 + 1) end
  camera()
  -- rows moved both ways, and rows with colors of their own
  rowshift()
  for y = 106, 127 do rowshift(y, (y + t) % 13 - 6) end
  rowshift(0, 300)
  rowshift(1, -300)
  rowshift(2, 3)
  rowshift(2)
  rowshift(500, 4)
  for y = 106, 127 do
    rectfill(0, y, 127, y, 12)
    pset(y - 100, y, 8)
    rowpal(y, 12, (y - 106) * 0x0a0a + 0x40)
  end
  rowpal(110)
  rowpal(111, 12)
  if t > 3 then rowpal() end
  print(pget(2, 2), 100, 100, 7)
end
