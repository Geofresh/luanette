-- title: hb 3de
-- by: geo
-- about: the layer over a 3D scene and under its picture
-- api: 2
local o, a = nil, 0
function _init()
  o = obj(shape("torus", 90, 30, 24), 0xff8040, 120)
  sky(0x203060, 0x000010)
  hd.order("under")
end
function _update60()
  a = a + 1
  place(o, 0, 0, 0, a, a * 0.7, 0)
end
function _draw()
  cls(0)
  print("PIXEL HUD", 46, 118, 7)
end
