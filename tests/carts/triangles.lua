-- title: Triangles
-- by: geo
-- about: trifill's edge cases for the parity test (subpixel, shared edges, off screen, camera, clip)
local f = 0
function _update() f = f + 1 end
function _draw()
  cls(0)
  -- a fan, every edge shared; vertices on sixteenths of a pixel (exact in 32 and 64 bits)
  local P = {{114, 64}, {107.375, 89}, {89, 107.25}, {64, 114}, {39, 107.3125}, {20.6875, 89}, {14, 64},
             {20.75, 39}, {39, 20.5}, {64, 14}, {89, 20.625}, {107.4375, 39}}
  local s = f % 12
  for i = 0, 11 do
    local a, b = P[(i + s) % 12 + 1], P[(i + s + 1) % 12 + 1]
    trifill(64.25, 64.75, a[1], a[2], b[1], b[2], 1 + i)
  end
  trifill(-20, -10, 40.5, 3.25, 10, 30.9, 8)
  trifill(100, 100, 100, 100, 120, 120, 9)    -- degenerate: nothing
  trifill(110.1, 90, 140, 130, 90, 140, 10)   -- off the edge
  camera(-3, 5) trifill(2, 110, 30, 112, 12, 126, 11) camera()
  clip(40, 100, 20, 20) trifill(30, 95, 70, 110, 35, 130, 14) clip()
  trifill(5.5, 5.5, 6.5, 5.5, 5.5, 6.5, 7)    -- a sliver
  pal(7, 12) trifill(120, 2, 127, 2, 124, 9) pal()
  print(flr(sin(0.25) * 100) .. " " .. flr(cos(0.5) * 100), 2, 2, 7)
end
