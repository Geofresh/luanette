-- title: Stars
-- by: you
-- about: catch the falling stars

x = 56  -- the basket's left edge, in picture pixels (0 to 127)

function _draw()
  cls(1)                             -- clear the picture to color 1, dark blue
  rectfill(x, 118, x + 15, 121, 9)   -- the basket: 16 pixels wide, orange (9)
end
