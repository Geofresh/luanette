-- title: Stars
-- by: you
-- about: catch the falling stars

x = 56  -- the basket's left edge, in picture pixels (0 to 127)

function _update()
  if btn(0) then x = x - 2 end       -- left
  if btn(1) then x = x + 2 end       -- right
  local tx = touch()                 -- a finger on the screen: the basket goes under it
  if tx then x = tx - 8 end
  x = mid(0, x, 112)                 -- keep it on the screen
end

function _draw()
  cls(1)                             -- clear the picture to color 1, dark blue
  rectfill(x, 118, x + 15, 121, 9)   -- the basket: 16 pixels wide, orange (9)
end
