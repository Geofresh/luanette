-- title: Hang
-- by: tests
-- about: a pcall loop around an endless loop: the frame must still stop (tests/hang.py)

function _update()
  while true do
    pcall(function() while true do end end)
  end
end

function _draw()
  cls(1)
end
