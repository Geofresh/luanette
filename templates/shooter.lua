-- title: Shooter
-- by: you
-- about: steer the ship; it fires on its own
-- api: 4
-- canvas: full

-- sprites (the playground's sprite editor writes this table)
SPRITES = {
  [1] = "00077000 00077000 0077c700 007cc700 077cc770 77777777 78077087 80000008", -- ship
  [2] = "08000080 00800800 08888880 88188188 88888888 08088080 80000008 08000080", -- invader
}
-- sounds (the playground's sound editor writes this table)
SOUNDS = {
  [8] = "01 0000 30223", -- shot
  [9] = "04 0000 18770 0c755", -- boom
  [10] = "06 0000 10460 0c455", -- hurt
}

-- The whole screen: the 128x128 picture in the middle, and bands above and below it in the
-- full-color layer (hd) for the title and what to do. Drawn once; they stay.
local c = caps()
local W, H = c.w, c.h
local TOP, BOT = c.picture_y, c.picture_y + 384

local function bands(title, hint)
  hd.scanlines(false)
  hd.gradient(0, 0, W, TOP, 0x1b2440, 0x000000)
  hd.text(title, W / 2, TOP / 2 - 16, 0xffffff, 24, "bold", "center")
  hd.gradient(0, BOT, W, H - BOT, 0x000000, 0x1b2440)
  hd.text(hint, W / 2, BOT + 22, 0x9aa4b8, 14, "mono", "center")
end

local ship = {x = 60}
local shots, foes, sparks = {}, {}, {}
local score, lives, t = 0, 3, 0

function _init()
  bands("SHOOTER", "drag to steer  ·  tap to start again")
end

local function hit(f)  -- an enemy goes up in sparks
  for i = 1, 8 do
    sparks[#sparks + 1] = {x = f.x + 4, y = f.y + 4, vx = rnd(2) - 1, vy = rnd(2) - 1, life = 15}
  end
  sfx(9)
end

function _update()
  t = t + 1
  if lives == 0 then
    if btnp(4) then score, lives, foes, shots = 0, 3, {}, {} end
    return
  end
  local tx = touch()
  if tx then ship.x = tx - 4 end
  if btn(0) then ship.x = ship.x - 2 end
  if btn(1) then ship.x = ship.x + 2 end
  ship.x = mid(0, ship.x, 120)
  if t % 8 == 0 then
    shots[#shots + 1] = {x = ship.x + 3, y = 112}
    sfx(8)
  end
  if t % 30 == 0 then foes[#foes + 1] = {x = flr(rnd(120)), y = -8, phase = rnd(1)} end
  for i = #shots, 1, -1 do
    local s = shots[i]
    s.y = s.y - 4
    if s.y < -4 then table.remove(shots, i) end
  end
  for i = #foes, 1, -1 do
    local f = foes[i]
    f.y = f.y + 0.6 + score / 200
    f.x = f.x + sin(t / 60 + f.phase) * 0.5
    local gone = false
    for j = #shots, 1, -1 do
      local s = shots[j]
      if s.x >= f.x and s.x <= f.x + 7 and s.y >= f.y and s.y <= f.y + 7 then
        table.remove(shots, j)
        hit(f)
        score = score + 1
        gone = true
        break
      end
    end
    if not gone and (f.y > 128 or (f.y > 108 and abs(f.x - ship.x) < 7)) then
      lives = lives - 1
      sfx(10)
      log("lost a life at score", score)
      gone = true
    end
    if gone then table.remove(foes, i) end
  end
  for i = #sparks, 1, -1 do
    local p = sparks[i]
    p.x, p.y, p.life = p.x + p.vx, p.y + p.vy, p.life - 1
    if p.life <= 0 then table.remove(sparks, i) end
  end
end

function _draw()
  cls(0)
  for _, s in ipairs(shots) do rectfill(s.x, s.y, s.x + 1, s.y + 3, 10) end
  for _, f in ipairs(foes) do spr(2, f.x, f.y) end
  for _, p in ipairs(sparks) do pset(p.x, p.y, p.life > 8 and 10 or 9) end
  if lives > 0 then spr(1, ship.x, 116) end
  print("score " .. score, 2, 2, 7)
  print("lives " .. lives, 96, 2, 8)
  if lives == 0 then print("game over: tap", 36, 60, 7) end
end
