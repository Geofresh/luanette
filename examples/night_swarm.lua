-- title: Night Swarm
-- by: geo
-- about: survive the swarm: drag or d-pad to move, bolts fire on their own; A start, B pause
-- api: 2
-- canvas: full
-- A tiny survivors-like: you never attack, you only move. Bolts seek the nearest foe on their
-- own; the swarm thickens as the minutes pass. XP gems level you up: pick a boon, keep moving.
-- The 128x128 picture is the night (redrawn every frame); the HUD lives on the layer bands
-- above and below it, repainted only when what they show changes (docs/DESIGN.md).

SPRITES = {
  [1] = -- the hunter: hair, face, eyes, red-lined cape
    "05555550" .. "05ffff50" .. "051ff150" .. "05ffff50" ..
    "00222200" .. "02288220" .. "02888820" .. "00200200",
  [2] = -- bat
    "06000060" .. "06655660" .. "06588560" .. "00555500" ..
    "00000000" .. "00000000" .. "00000000" .. "00000000",
  [3] = -- zombie
    "00bbbb00" .. "0bbbbbb0" .. "0b8bb8b0" .. "0bbbbbb0" ..
    "00bbbb00" .. "00b00b00" .. "00000000" .. "00000000",
  [4] = -- skeleton
    "00777700" .. "07777770" .. "07577570" .. "00777700" ..
    "00077000" .. "00700700" .. "00000000" .. "00000000",
  [5] = -- xp gem
    "0000c000" .. "000ccc00" .. "00c7ccc0" .. "000ccc00" ..
    "0000c000" .. "00000000" .. "00000000" .. "00000000",
  [6] = -- heart
    "08800880" .. "88788888" .. "88888888" .. "08888880" ..
    "00888800" .. "00088000" .. "00000000" .. "00000000",
}

local c = caps()
local W, H = c.w, c.h
local PX = c.picture_x or 0
local PY = c.picture_y or 0
local TOP = PY                        -- band above the picture
local BOT = H - (PY + 384)            -- band below it

local BG    = 0x0b0f1a
local TXT   = 0xf2f4f8
local SOFT  = 0x8d97ab
local RED   = 0xff3b5c
local BLUE  = 0x4fc3f7
local GOLD  = 0xffd54a
local PINK  = 0xffb3c0
local PANEL = 0x1c2333

local KIND = {
  bat  = {spr = 2, hp = 1, spd = 30, xp = 1, r = 5},
  zom  = {spr = 3, hp = 3, spd = 18, xp = 2, r = 6},
  skel = {spr = 4, hp = 5, spd = 14, xp = 3, r = 6},
}

local POOL = {
  {k = "bolts",  name = "+1 BOLT",    desc = "one more a volley",  max = 5},
  {k = "dmg",    name = "DAMAGE +1",  desc = "bolts hit harder",   max = 6},
  {k = "rate",   name = "RAPID FIRE", desc = "volleys come faster", max = 6},
  {k = "pierce", name = "PIERCE",     desc = "pass through foes",  max = 3},
  {k = "spd",    name = "SWIFT",      desc = "move faster",        max = 4},
  {k = "mag",    name = "MAGNET",     desc = "gems fly to you",    max = 4},
  {k = "vit",    name = "VITALITY",   desc = "+1 heart, heal 2",   max = 5},
}

-- ---- state ----
local pl, st, counts      -- player, weapon stats, boon stacks taken
local foes, shots, gems
local cam
local state, paused
local run_t, kills, lv, xp, xpneed
local spawn_t, wave_t, fire_t, tspawn
local iframes, shake_t
local picks, sel
local over_on, epoch = false, 0       -- is the boon overlay up; bumped per choice set
local best_t, best_k
local last = 0
local TT = 0              -- time(), cached once a frame

local function ns(n) return string.format("%d", flr(n)) end
local function fmt_t(t)
  local m, s = flr(t / 60), flr(t) % 60
  return ns(m) .. ":" .. (s < 10 and "0" or "") .. ns(s)
end

local function reset_run()
  pl = {x = 0, y = 0, hp = 5, maxhp = 5}
  st = {bolts = 1, dmg = 1, rate = 0.6, spd = 40, mag = 16, pierce = 0}
  counts = {}
  foes, shots, gems = {}, {}, {}
  cam = {x = 0, y = 0}
  run_t, kills, lv, xp, xpneed = 0, 0, 1, 0, 10
  spawn_t, wave_t, fire_t = 0.8, 30, 0.4
  iframes, shake_t = 0, 0
  paused = false
end

local function spawn_foe(kind)
  if #foes >= 42 then return end          -- the cap is the design: a crowd, not a soup
  local k = KIND[kind]
  local a = rnd() * 6.28318
  local d = 92 + rnd(18)                  -- just off the window's far corner
  local hp = k.hp + (kind ~= "bat" and flr(run_t / 70) or 0)
  foes[#foes + 1] = {x = pl.x + math.cos(a) * d, y = pl.y + math.sin(a) * d,
                     kind = kind, hp = hp, maxhp = hp, spd = k.spd, xp = k.xp, flash = 0}
end

local function pick_kind()
  local r = rnd()
  if run_t < 20 then return "bat" end
  if run_t < 50 then return r < 0.6 and "bat" or "zom" end
  if run_t < 120 then
    if r < 0.45 then return "bat" elseif r < 0.78 then return "zom" end
    return "skel"
  end
  if r < 0.38 then return "bat" elseif r < 0.75 then return "zom" end
  return "skel"
end

local function make_picks()
  local avail = {}
  for _, u in ipairs(POOL) do
    if (counts[u.k] or 0) < u.max then avail[#avail + 1] = u end
  end
  picks = {}
  for _ = 1, 3 do
    if #avail == 0 then break end
    picks[#picks + 1] = table.remove(avail, flr(rnd(#avail)) + 1)
  end
  sel = 1
  epoch = epoch + 1
end

local function gain(v)
  xp = xp + v
  if xp >= xpneed then
    xp = xp - xpneed
    lv = lv + 1
    xpneed = 5 + lv * 5
    state = "level"
    make_picks()
    iframes = max(iframes, 0.8)           -- a moment of grace while you choose
    buzz(60)
    sfx(5)
    fps(30, 10)
  end
end

local function apply_pick()
  local u = picks and picks[sel]
  if u then
    counts[u.k] = (counts[u.k] or 0) + 1
    if u.k == "bolts" then st.bolts = st.bolts + 1
    elseif u.k == "dmg" then st.dmg = st.dmg + 1
    elseif u.k == "rate" then st.rate = max(0.16, st.rate * 0.82)
    elseif u.k == "pierce" then st.pierce = st.pierce + 1
    elseif u.k == "spd" then st.spd = st.spd * 1.12
    elseif u.k == "mag" then st.mag = st.mag + 10
    elseif u.k == "vit" then
      pl.maxhp = pl.maxhp + 1
      pl.hp = min(pl.maxhp, pl.hp + 2)
    end
  end
  picks = nil
  state = "play"
  fps(30)
  gain(0)                                 -- a queued level waits right here
end

local function fire()
  local n, bi, bd = #foes, 0, 3600      -- 60^2: only fire when a foe is truly close
  for i = 1, n do
    local f = foes[i]
    local dx, dy = f.x - pl.x, f.y - pl.y
    local d = dx * dx + dy * dy
    if d < bd then bd, bi = d, i end
  end
  if bi == 0 then return end
  local f = foes[bi]
  local base = math.atan(f.y - pl.y, f.x - pl.x)
  for k = 1, st.bolts do
    local a = base + (k - (st.bolts + 1) / 2) * 0.16
    shots[#shots + 1] = {x = pl.x, y = pl.y, vx = math.cos(a) * 95, vy = math.sin(a) * 95,
                         p = st.pierce, cool = 0, dist = 0}
  end
  while #shots > 30 do table.remove(shots, 1) end
end

local function kill_foe(j)
  local f = table.remove(foes, j)
  kills = kills + 1
  sfx(0)
  if f.kind ~= "bat" and rnd() < 0.06 and #gems < 40 then
    gems[#gems + 1] = {x = f.x, y = f.y, heart = true, age = 0}
  elseif #gems < 40 then
    gems[#gems + 1] = {x = f.x, y = f.y, v = f.xp, age = 0}
  else
    gain(f.xp)                            -- pockets full: cash it in on the spot
  end
end

local function move_foes(dt, hurt)
  for i = 1, #foes do
    local f = foes[i]
    local dx, dy = pl.x - f.x, pl.y - f.y
    local d = math.sqrt(dx * dx + dy * dy)
    if d > 160 then                       -- fell behind: rejoin the ring around the player
      local a = rnd() * 6.28318
      local nd = 92 + rnd(18)
      f.x = pl.x + math.cos(a) * nd
      f.y = pl.y + math.sin(a) * nd
    elseif d > 0.1 then
      f.x = f.x + dx / d * f.spd * dt
      f.y = f.y + dy / d * f.spd * dt
    end
    if f.flash > 0 then f.flash = f.flash - dt end
    if hurt and iframes <= 0 and d < 3 + KIND[f.kind].r then
      pl.hp = pl.hp - 1
      iframes = 1.0
      shake_t = 0.2
      buzz(200)
      sfx(2)
      if pl.hp <= 0 then
        state = "over"
        if run_t > best_t then best_t = run_t end
        if kills > best_k then best_k = kills end
        save({bt = best_t, bk = best_k})
        buzz(300)
        sfx(4)
        fps(30, 10)
      end
    end
  end
end

local function move_shots(dt)
  for i = #shots, 1, -1 do
    local s = shots[i]
    s.x = s.x + s.vx * dt
    s.y = s.y + s.vy * dt
    s.dist = s.dist + 95 * dt
    if s.cool > 0 then s.cool = s.cool - dt end
    local dead = s.dist > 90              -- bolts fizzle just past the screen's edge
    if not dead and s.cool <= 0 then
      for j = #foes, 1, -1 do
        local f = foes[j]
        local dx, dy = f.x - s.x, f.y - s.y
        if dx * dx + dy * dy < 36 then
          f.hp = f.hp - st.dmg
          f.flash = 0.08
          local kd = math.sqrt(dx * dx + dy * dy)
          if kd > 0.01 then
            f.x = f.x + dx / kd * 3
            f.y = f.y + dy / kd * 3
          end
          if f.hp <= 0 then kill_foe(j) end
          if s.p > 0 then s.p = s.p - 1 s.cool = 0.12 else dead = true end
          break
        end
      end
    end
    if dead then
      shots[i] = shots[#shots]
      shots[#shots] = nil
    end
  end
end

local function move_gems(dt)
  for i = #gems, 1, -1 do
    local g = gems[i]
    g.age = g.age + dt
    if g.age > 25 then
      gems[i] = gems[#gems] gems[#gems] = nil
    else
      local dx, dy = pl.x - g.x, pl.y - g.y
      local d2 = dx * dx + dy * dy
      if d2 < st.mag * st.mag then
        local d = math.sqrt(d2)
        if d < 6 then
          if g.heart then
            pl.hp = min(pl.maxhp, pl.hp + 2)
            buzz(20)
          end
          sfx(1)
          gain(g.heart and 0 or g.v)
          gems[i] = gems[#gems] gems[#gems] = nil
        else
          g.x = g.x + dx / d * 70 * dt
          g.y = g.y + dy / d * 70 * dt
        end
      end
    end
  end
end

local function start()
  reset_run()
  state = "play"
  fps(30)
end

function _init()
  hd.order("under")                       -- the picture's colour 0 shows the layer
  hd.clear(BG)
  palette(0, BG)                          -- the night matches the bands
  local s = load()
  best_t = s and s.bt or 0
  best_k = s and s.bk or 0
  reset_run()
  state = "title"
  tspawn = 0.5
  for _ = 1, 6 do spawn_foe("bat") end    -- the title screen's attract swarm
  fps(30, 10)
  last = time()
end

function _update()
  TT = time()
  local dt = min(TT - last, 0.1)          -- after a stall: one step, not a burst
  last = TT
  if state == "title" then
    tspawn = tspawn - dt
    if tspawn <= 0 and #foes < 10 then
      tspawn = 1.2
      spawn_foe("bat")
    end
    move_foes(dt, false)
    if btnp(4) or btnp(5) then start() end
  elseif state == "over" then
    if btnp(4) or btnp(5) then start() end
  elseif state == "level" then
    if btnp(0) or btnp(2) then sel = sel - 1 if sel < 1 then sel = #picks end end
    if btnp(1) or btnp(3) then sel = sel + 1 if sel > #picks then sel = 1 end end
    if btnp(4) or btnp(5) then apply_pick() end
  elseif state == "play" then
    if btnp(5) then paused = not paused end
    if not paused then
      local mx, my = 0, 0
      if btn(0) then mx = mx - 1 end
      if btn(1) then mx = mx + 1 end
      if btn(2) then my = my - 1 end
      if btn(3) then my = my + 1 end
      if mx ~= 0 or my ~= 0 then
        local n = math.sqrt(mx * mx + my * my)
        pl.x = pl.x + mx / n * st.spd * dt
        pl.y = pl.y + my / n * st.spd * dt
      end
      run_t = run_t + dt
      if iframes > 0 then iframes = iframes - dt end
      if shake_t > 0 then shake_t = shake_t - dt end
      cam.x = cam.x + (pl.x - cam.x) * min(1, dt * 10)
      cam.y = cam.y + (pl.y - cam.y) * min(1, dt * 10)
      spawn_t = spawn_t - dt
      if spawn_t <= 0 then
        spawn_t = spawn_t + max(0.16, 0.9 - run_t * 0.005)
        spawn_foe(pick_kind())
      end
      wave_t = wave_t - dt
      if wave_t <= 0 then
        wave_t = 30
        for _ = 1, 6 + flr(run_t / 40) do spawn_foe("bat") end
        sfx(4)
      end
      fire_t = fire_t - dt
      if fire_t <= 0 then
        fire_t = fire_t + st.rate
        fire()
      end
      move_shots(dt)
      move_foes(dt, true)
      if state == "play" then move_gems(dt) end
    end
  end
end

-- ---- the bands, on the layer ----

local function draw_top()
  hd.rect(0, 0, W, TOP, BG)
  if state == "title" then
    hd.text("NIGHT SWARM", W / 2, 10, TXT, 24, "bold", "center")
    hd.text("SURVIVE THE NIGHT", W / 2, 42, SOFT, 12, "mono", "center")
  elseif state == "over" then
    hd.text("GAME OVER", W / 2, 10, RED, 24, "bold", "center")
    hd.text("SURVIVED " .. fmt_t(run_t) .. "   BEST " .. fmt_t(best_t), W / 2, 42, SOFT, 12, "mono", "center")
  elseif state == "level" then
    hd.text("LEVEL " .. ns(lv), W / 2, 12, GOLD, 18, "bold", "center")
    hd.text("CHOOSE A BOON", W / 2, 40, SOFT, 12, "mono", "center")
  else
    local ins = hd.inset(24, 20) + 8
    hd.text("LV " .. ns(lv), ins, 24, TXT, 18, "bold")
    hd.text(fmt_t(run_t), W - ins, 24, TXT, 18, "bold", "right")
    local bx = hd.inset(46, 8) + 2
    local bw = W - bx * 2
    hd.rect(bx, 46, bw, 8, PANEL, 255, 4)
    local fw = flr(bw / 16 * xp / xpneed) * 16
    if fw > 0 then hd.rect(bx, 46, max(8, fw), 8, BLUE, 255, 4) end
  end
end

-- ---- the boon picker: cards on the layer OVER the picture, on a dimmed frozen game ----

local CARDS_X, CARDS_W, CARD_H, CARD_GAP = PX + 32, 320, 68, 14
local CARDS_Y = PY + 76               -- three 68 px cards with 14 px gaps, centred

local function draw_card(i)
  local u = picks and picks[i + 1]
  local x, y = CARDS_X, CARDS_Y + i * (CARD_H + CARD_GAP)
  local on = sel == i + 1
  hd.rect(x, y, CARDS_W, CARD_H, on and 0x43222f or PANEL, 255, 14)
  if on then hd.frame(x, y, CARDS_W, CARD_H, GOLD, 2, 14) end
  if u then
    hd.text(u.name, x + 20, y + 8, on and TXT or SOFT, 20, "mono")
    hd.text(u.desc, x + 20, y + 40, on and PINK or SOFT, 14, "mono")
  end
end

local function draw_bottom(blink)
  hd.rect(0, H - BOT, W, BOT, BG)
  if state == "title" then
    if blink then hd.text("A OR TAP TO START", W / 2, H - 46, TXT, 16, "bold", "center") end
    hd.text("DRAG TO MOVE - BOLTS FIRE ON THEIR OWN", W / 2, H - 24, SOFT, 12, "mono", "center")
  elseif state == "over" then
    hd.text(ns(kills) .. " SLAIN", W / 2, H - 46, TXT, 16, "bold", "center")
    if blink then hd.text("A OR TAP TO TRY AGAIN", W / 2, H - 24, SOFT, 12, "mono", "center") end
  else
    local ins = hd.inset(H - 48, 16) + 8
    hd.text("♥", ins, H - 48, RED, 14, "mono")
    local bx = ins + 16
    local kw = hd.measure(ns(kills) .. " SLAIN", 12, "mono")
    local bw = max(20, W - bx - kw - ins - 12)
    hd.rect(bx, H - 44, bw, 9, PANEL, 255, 4)
    local fw = flr(bw * pl.hp / pl.maxhp)
    if fw > 0 then hd.rect(bx, H - 44, max(6, fw), 9, RED, 255, 4) end
    hd.text(ns(kills) .. " SLAIN", W - ins, H - 48, SOFT, 12, "mono", "right")
    if paused then hd.text("PAUSED - B RESUMES", W / 2, H - 24, GOLD, 12, "mono", "center") end
  end
end

function _draw()
  -- the boon picker lives on the layer OVER the picture while it is up
  if state == "level" then
    if not over_on then
      over_on = true
      hd.order("over")
      hd.rect(PX, PY, 384, 384, 0x05070d, 215)
      hd.text("D-PAD TO CHOOSE - A TO PICK", W / 2, PY + 366, SOFT, 12, "mono", "center")
      for i = 0, #picks - 1 do
        hd.repaint(CARDS_X, CARDS_Y + i * (CARD_H + CARD_GAP), CARDS_W, CARD_H, draw_card, i)
      end
      ui.changed("cards", sel, epoch)   -- the paints above did the work; seed the memory
    elseif ui.changed("cards", sel, epoch) then
      for i = 0, #picks - 1 do
        hd.repaint(CARDS_X, CARDS_Y + i * (CARD_H + CARD_GAP), CARDS_W, CARD_H, draw_card, i)
      end
    end
  elseif over_on then
    over_on = false
    hd.order("under")                   -- back under: wipe the overlay off the canvas
    hd.clear(BG)
  end
  local blink = flr(TT * 3) % 2 == 0
  local bl = state == "title" or state == "over" and blink or false
  local xpq = state == "play" and flr(xp / xpneed * 22) or 0
  if ui.changed("top", state, ns(lv), fmt_t(run_t), xpq) then
    hd.repaint(0, 0, W, TOP, draw_top)
  end
  if ui.changed("bot", state, pl.hp, ns(kills), bl, paused) then
    hd.repaint(0, H - BOT, W, BOT, draw_bottom, blink)
  end

  cls(0)
  palette(0, BG)
  local shx, shy = 0, 0
  if shake_t > 0 then
    shx = flr((rnd() - 0.5) * 5)
    shy = flr((rnd() - 0.5) * 5)
  end
  camera(shx, shy)
  clip(1, 0, 126, 128)                  -- the picture's edge columns stretch out to the
                                        -- glass's sides: keep moving pixels out of them
  local cx, cy = cam.x, cam.y

  -- the night ground: a dim dot grid, with the odd gravestone
  local gx0 = flr((cx - 68) / 16) * 16
  local gy0 = flr((cy - 68) / 16) * 16
  for gx = gx0, gx0 + 128, 16 do
    for gy = gy0, gy0 + 128, 16 do
      local sx, sy = flr(gx - cx) + 64, flr(gy - cy) + 64
      if (gx * 31 + gy * 17) % 97 == 0 then
        rectfill(sx - 1, sy - 3, sx + 1, sy, 5)
        rectfill(sx - 2, sy, sx + 2, sy, 5)
      else
        pset(sx, sy, 5)
      end
    end
  end

  for i = 1, #gems do
    local g = gems[i]
    if g.age < 20 or flr(g.age * 6) % 2 == 0 then
      local sx, sy = flr(g.x - cx) + 64, flr(g.y - cy) + 64
      if sx > -6 and sx < 134 and sy > -6 and sy < 134 then
        spr(g.heart and 6 or 5, sx - 4, sy - 4)
      end
    end
  end

  for i = 1, #foes do
    local f = foes[i]
    local sx, sy = flr(f.x - cx) + 64, flr(f.y - cy) + 64
    if sx > -8 and sx < 136 and sy > -8 and sy < 136 then
      local bob = f.kind == "bat" and flr(math.sin(TT * 9 + i) * 2) or flr(math.sin(TT * 6 + i))
      if f.flash > 0 then pal(5, 7) pal(6, 7) pal(8, 7) pal(11, 7) end
      spr(KIND[f.kind].spr, sx - 4, sy - 4 + bob, pl.x > f.x)
      if f.flash > 0 then pal() end
      if f.hp < f.maxhp then
        rectfill(sx - 4, sy - 6, sx + 3, sy - 6, 5)
        local w = flr(8 * f.hp / f.maxhp)
        if w > 0 then rectfill(sx - 4, sy - 6, sx - 5 + w, sy - 6, 8) end
      end
    end
  end

  if iframes <= 0 or flr(TT * 12) % 2 == 0 then
    spr(1, flr(pl.x - cx) + 60, flr(pl.y - cy) + 60)
  end

  for i = 1, #shots do
    local s = shots[i]
    local sx, sy = flr(s.x - cx) + 64, flr(s.y - cy) + 64
    if sx > -4 and sx < 132 and sy > -4 and sy < 132 then
      line(sx - flr(s.vx * 0.04), sy - flr(s.vy * 0.04), sx, sy, 9)
      circfill(sx, sy, 2, 10)
    end
  end
  camera()
  clip()
end
