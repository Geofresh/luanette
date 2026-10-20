-- title: Bubbles
-- by: geo
-- about: classic bubble shooter: drag to aim, release to fire; BOOT swaps the bubble
-- api: 2
-- canvas: full
-- The 128x128 picture is the board; score, level and the next bubble live on the
-- layer bands above and below it (docs/DESIGN.md). Pop 3+, drop the loose ones.

local c = caps()
local W, H = c.w, c.h
local PX = c.picture_x or 0
local PY = c.picture_y or 0
local TOP = PY
local BOT = H - (PY + 384)

local BG = 0x0b0f1a
local TXT = 0xf2f4f8
local SOFT = 0x8d97ab
local GOLD = 0xffd54a
local RED = 0xff3b5c
local PANEL = 0x1c2333
local DIMC = 0x05070d

local COLS = {8, 12, 11, 10, 14, 9}
local LET = {["R"] = 8, ["B"] = 12, ["G"] = 11, ["Y"] = 10, ["P"] = 14, ["O"] = 9}
local CH = {}
for k, v in pairs(LET) do CH[v] = k end
local RGB = {[8] = 0xff004d, [12] = 0x29adff, [11] = 0x00e436,
             [10] = 0xffec27, [14] = 0xff77a8, [9] = 0xffa300}
local SHX, SHY = 64, 120

-- ---- state ----
local grid = {}
local cur, nxt, level, score, best = 8, 12, 1, 0, 0
local state = "title"
local bx, by, bvx, bvy = 0, 0, 0, 0
local flash, drop = {}, {}
local banner_t = 0
local aim, aiming, wasd, over_on, wasbusy = -math.pi / 2, false, false, false, false
local blink = false
local tt = 0
local last_tt = 0

local function cx(i, r) return (i * 2 + r % 2) * 7 + 7 end
local function cy(r) return 7 + 12 * r end

local ROWC = {}
for r = 0, 8 do
  local t = {}
  for i = 1, 9 - r % 2 do t[#t + 1] = i end
  ROWC[r + 1] = t
end
local RY = {}
for r = 0, 8 do RY[r + 1] = cy(r) end
local RX = {}
for r = 0, 8 do
  RX[r + 1] = {}
  for i = 0, 8 do RX[r + 1][i + 1] = cx(i, r) end
end
local function nrow() return math.min(4 + flr((level - 1) / 2), 7) end
local function ncol() return math.min(4 + flr(level / 2), 6) end
local function inb(i, r) return i >= 0 and i < 9 - r % 2 and r >= 0 and r <= 8 end
local function col(i, r)
  if not inb(i, r) then return 0 end
  local v = grid[r + 1][i + 1]
  return v or 0
end
local function setc(i, r, v) if inb(i, r) then grid[r + 1][i + 1] = v end end

local NB = {}                   -- static neighbour tables (no per-call garbage)
for r = 0, 8 do
  NB[r + 1] = {}
  for i = 0, 8 do
    local o = r % 2
    NB[r + 1][i + 1] = {{i - 1, r}, {i + 1, r}, {i, r - 1}, {i, r + 1},
                        {i + o, r - 1}, {i + o, r + 1}, {i + o - 1, r - 1}, {i + o - 1, r + 1}}
  end
end
local seen, stk = {}, {}     -- scratch marks for the floods
local mark = 0
local function cluster(i, r)        -- the same-color group containing (i, r), as a list
  local out = {}
  local c0 = col(i, r)
  if c0 == 0 then return out end
  mark = mark + 1
  local st, top = {{i, r}}, 1
  seen[r * 16 + i] = mark
  while top > 0 do
    local t = st[top]
    st[top] = nil
    top = top - 1
    out[#out + 1] = t
    for _, n in ipairs(NB[t[2] + 1][t[1] + 1]) do
      if n[2] >= 0 and n[2] <= 8 and n[1] >= 0 and n[1] <= 8 then
        local k = n[2] * 16 + n[1]
        if seen[k] ~= mark and grid[n[2] + 1][n[1] + 1] == c0 then
          seen[k] = mark top = top + 1 st[top] = n
        end
      end
    end
  end
  return out
end
local function connected()          -- marks every bubble tied to row 0; returns mark
  mark = mark + 1
  local top = 0
  for i = 0, 8 do
    if col(i, 0) > 0 then seen[i] = mark top = top + 1 stk[top] = {i, 0} end
  end
  while top > 0 do
    local t = stk[top]
    stk[top] = nil
    top = top - 1
    for _, n in ipairs(NB[t[2] + 1][t[1] + 1]) do
      if n[2] >= 0 and n[2] <= 8 and n[1] >= 0 and n[1] <= 8 then
        local k = n[2] * 16 + n[1]
        if seen[k] ~= mark and grid[n[2] + 1][n[1] + 1] > 0 then
          seen[k] = mark top = top + 1 stk[top] = n
        end
      end
    end
  end
  return mark
end
local function newrow(r)
  local t, nc = {}, ncol()
  local w = 9 - (r - 1) % 2        -- odd rows hold 8 cells: the 9th slot stays empty, never nil
  for i = 1, 9 do t[i] = (i > w) and 0 or COLS[flr(rnd(nc)) + 1] end
  return t
end
local function refill()
  grid = {}
  for r = 1, 9 do grid[r] = {} for i = 1, 9 do grid[r][i] = 0 end end
  for r = 1, nrow() do grid[r] = newrow(r) end
end
local function count()
  local n = 0
  for r = 1, 9 do for i = 1, 9 - (r - 1) % 2 do if grid[r][i] > 0 then n = n + 1 end end end
  return n
end

local function finish()                    -- animations ended: hand control back
  flash, drop = {}, {}
  if count() == 0 then
    state = "levelup"
    banner_t = tt + 1.6
    sfx(7) buzz(80)
    return
  end
  local lo = false
  local li = ROWC[9]
  for j = 1, #li do
    if grid[9][li[j]] > 0 then lo = true end
  end
  if lo then
    state = "gameover"
    if score > best then best = score end
    save({b = best})
    buzz(300) sfx(6)
  else
    state = "playing"
  end
end

-- ---- flight ----
local function nearhit(i, r)         -- cell empty but touches the top or a filled cell
  if r == 0 then return true end
  for _, n in ipairs(NB[r + 1][i + 1]) do
    if n[2] < 0 then return true end
    local row = grid[n[2] + 1]
    local v = row and row[n[1] + 1]
    if v and v > 0 then return true end
  end
  return false
end
local function hit(x, y)
  for r = 1, 9 do
    local li = ROWC[r]
    local dy0 = y - RY[r]
    for j = 1, #li do
      local i = li[j]
      if grid[r][i] > 0 then
        local dx = x - RX[r][i]
        if dx * dx + dy0 * dy0 < 169 then return i - 1, r - 1 end
      end
    end
  end
  return -1, -1
end
local function attach(x, y)
  local bi, br, bd, ei, er, ed = -1, -1, 1e9, -1, -1, 1e9
  for r = 1, 9 do
    local ri = r - 1
    local li = ROWC[r]
    for j = 1, #li do
      local i = li[j] - 1
      if grid[r][i + 1] == 0 then
        local dx, dy = x - RX[r][i + 1], y - RY[r]
        local d = dx * dx + dy * dy
        if d < ed then ed, ei, er = d, i, ri end
        if d < bd and nearhit(i, ri) then bd, bi, br = d, i, ri end
      end
    end
  end
  if ei < 0 then return end                -- no room left: the bubble vanishes
  if bi < 0 then bi, br = ei, er end
  local cc = cur
  setc(bi, br, cc)
  cur, nxt = nxt, COLS[flr(rnd(ncol())) + 1]
  local cl = cluster(bi, br)          -- 3 or more of a color: the whole group goes
  if #cl >= 3 then
    for _, t in ipairs(cl) do
      flash[#flash + 1] = {x = RX[t[2] + 1][t[1] + 1], y = RY[t[2] + 1], t = tt}
      score = score + 10
      setc(t[1], t[2], 0)
    end
    local mk = connected()
    for r = 2, 9 do
      local li = ROWC[r]
      for j = 1, #li do
        local i = li[j] - 1
        local v = grid[r][i + 1]
        if v > 0 and seen[(r - 1) * 16 + i] ~= mk then
          drop[#drop + 1] = {x = RX[r][i + 1], y = RY[r], vy = -rnd(2), c = v}
          score = score + 20
          grid[r][i + 1] = 0
        end
      end
    end
    buzz(60) sfx(2)
  end
  if count() == 0 then
    for r = 1, 9 do for i = 1, #grid[r] do
      if grid[r][i] > 0 then
        drop[#drop + 1] = {x = cx(i - 1, r - 1), y = cy(r - 1), vy = -rnd(2), c = grid[r][i]}
        score = score + 20
        grid[r][i] = 0
      end
    end end
  end
  if #flash > 0 or #drop > 0 then
    state = "popping"
  else
    finish()                      -- nothing to animate: settle it now (a bubble on row 8 ends it)
  end
end
local function step(dt)
  local n = max(1, ceil((abs(bvx) + abs(bvy)) * dt / 4))   -- substeps: a late frame cannot tunnel
  for _ = 1, n do
    bx = bx + bvx * dt / n
    by = by + bvy * dt / n
    if bx < 7 then bx = 14 - bx bvx = -bvx sfx(0) end
    if bx > 121 then bx = 242 - bx bvx = -bvx sfx(0) end
    if by <= 7 then by = 7 attach(bx, by) return end
    local hi, hr = hit(bx, by)
    if hr >= 0 then attach(bx, by) return end
  end
end
local function path()       -- a free cursor: straight down the aim, stopping at the first wall, ceiling or bubble
  local x, y = SHX, SHY
  local vx, vy = math.cos(aim) * 4, math.sin(aim) * 4
  for _ = 1, 60 do
    x = x + vx y = y + vy
    if y <= 7 then y = 7 break end
    if x < 7 then x = 7 break end
    if x > 121 then x = 121 break end
    local _, hr = hit(x, y)
    if hr >= 0 then break end
  end
  return x, y
end
local function updaim(tx, ty)
  local a = math.atan((ty - PY) / 3 - SHY, (tx - PX) / 3 - SHX)
  if a > -0.06 and a < 3.2 then               -- points down: clamp near horizontal
    a = (tx - PX >= 3 * SHX) and -0.06 or (math.pi - 0.06)
  end
  aim = a
end

local function newgame()
  level, score = 1, 0
  refill()
  cur = COLS[flr(rnd(ncol())) + 1]
  nxt = COLS[flr(rnd(ncol())) + 1]
  flash, drop = {}, {}
  state = "playing"
end

function _init()
  hd.order("under")
  hd.clear(BG)
  palette(0, BG)
  local s = load()
  best = s and s.b or 0
  refill()
  fps(30, 10)
end

function _update()
  tt = time()
  blink = flr(tt * 3) % 2 == 0
  local busy = state == "flying" or state == "popping" or state == "falling" or state == "levelup"
  if busy ~= wasbusy then
    wasbusy = busy
    if busy then fps(30) else fps(30, 10) end   -- lazy: 10 fps while nothing moves
  end
  local dt = min(tt - last_tt, 0.1)
  last_tt = tt
  local tx, ty = touch(true)
  local down = tx ~= nil
  if btnp(5) and state == "playing" then
    cur, nxt = nxt, cur
    sfx(5) buzz(20)
  end

  if state == "title" or state == "gameover" then
    if down and not wasd then newgame() sfx(5) buzz(20) end
  elseif state == "levelup" then
    if tt >= banner_t then
      level = level + 1
      refill()
      state = "playing"
    end
  elseif state == "playing" then
    if down and not wasd then
      aiming = true
      updaim(tx, ty)
    elseif down and aiming then
      updaim(tx, ty)
    elseif aiming and not down then
      aiming = false
      bx, by = SHX, SHY
      bvx, bvy = math.cos(aim) * 120, math.sin(aim) * 120
      state = "flying"
      sfx(3) buzz(15)
    end
  elseif state == "flying" then
    step(dt)
  elseif state == "popping" or state == "falling" then
    if #drop > 0 then
      state = "falling"
      for i = #drop, 1, -1 do
        local d = drop[i]
        d.vy = d.vy + 300 * dt
        d.y = d.y + d.vy * dt
        if d.y > 140 then table.remove(drop, i) end
      end
    elseif #flash > 0 then
      state = "popping"
      for i = #flash, 1, -1 do
        if tt - flash[i].t > 0.27 then table.remove(flash, i) end
      end
    end
    if #drop == 0 and #flash == 0 then finish() end
  end
  wasd = down
end

-- ---- layer: bands, frame, popup cards ----
local function draw_top()
  hd.rect(0, 0, W, TOP, BG)
  local ins = hd.inset(20, 26) + 8
  if state == "title" then
    hd.text("BUBBLES", W / 2, 18, GOLD, 24, "bold", "center")
  else
    hd.text("SCORE " .. flr(score), ins, 24, TXT, 18, "bold")
    hd.text("LV " .. level, W - ins, 24, TXT, 18, "bold", "right")
  end
end
local function draw_bot()
  hd.rect(0, H - BOT, W, BOT, BG)
  local ins = hd.inset(H - 54, 46) + 8
  if state == "title" then
    if blink then hd.text("TAP TO START", W / 2, H - 48, TXT, 16, "bold", "center") end
    hd.text("DRAG TO AIM - RELEASE TO FIRE   BEST " .. flr(best), W / 2, H - 26, SOFT, 12, "mono", "center")
    return
  end
  hd.circle(ins + 10, H - 28, 9, RGB[cur] or TXT, 255)
  hd.circle(ins + 8, H - 31, 3, 0xffffff, 150)
  hd.text("NEXT", ins + 26, H - 34, SOFT, 12, "mono")
  hd.circle(ins + 74, H - 28, 9, RGB[nxt] or TXT, 255)
  hd.circle(ins + 72, H - 31, 3, 0xffffff, 150)
  hd.text("BOOT SWAPS", W - ins, H - 34, SOFT, 12, "mono", "right")
end
local function draw_card()
  hd.rect(PX + 24, PY + 88, 336, 208, 0x101627, 255, 18)
  if state == "title" then
    hd.text("BUBBLES", W / 2, PY + 112, GOLD, 24, "bold", "center")
    hd.text("DRAG TO AIM, RELEASE TO FIRE", W / 2, PY + 160, TXT, 12, "mono", "center")
    hd.text("BEST " .. flr(best), W / 2, PY + 186, SOFT, 12, "mono", "center")
    if blink then hd.text("TAP TO START", W / 2, PY + 226, TXT, 16, "bold", "center") end
  elseif state == "gameover" then
    hd.text("GAME OVER", W / 2, PY + 112, RED, 24, "bold", "center")
    hd.text("SCORE " .. flr(score), W / 2, PY + 160, TXT, 18, "bold", "center")
    hd.text("BEST " .. flr(best), W / 2, PY + 190, SOFT, 12, "mono", "center")
    if blink then hd.text("TAP TO RESTART", W / 2, PY + 230, TXT, 16, "bold", "center") end
  elseif state == "levelup" then
    hd.text("LEVEL " .. (level + 1), W / 2, PY + 150, GOLD, 24, "bold", "center")
    hd.text("SCORE " .. flr(score), W / 2, PY + 196, TXT, 16, "bold", "center")
  end
end

function _draw()
  if state == "title" or state == "gameover" or state == "levelup" then
    if not over_on then
      over_on = true
      hd.order("over")
      hd.rect(0, 0, W, H, BG, 200)
      hd.repaint(PX + 24, PY + 88, 336, 208, draw_card)
      ui.changed("card", state, blink)
    elseif ui.changed("card", state, blink) then
      hd.repaint(PX + 24, PY + 88, 336, 208, draw_card)
    end
  elseif over_on then
    over_on = false
    hd.order("under")
    hd.clear(BG)
    hd.frame(PX - 2, PY - 2, 388, 388, PANEL, 3, 6)
  end
  if ui.changed("top", state, score, level) then hd.repaint(0, 0, W, TOP, draw_top) end
  if ui.changed("bot", state, cur, nxt, blink) then hd.repaint(0, H - BOT, W, BOT, draw_bot) end

  cls(0)
  palette(0, BG)
  clip(1, 0, 126, 128)
  local lo = false
  for r = 1, 9 do
    local li = ROWC[r]
    for j = 1, #li do
      local i = li[j]
      local v = grid[r][i]
      if v and v > 0 then
        local px, py = RX[r][i], RY[r]
        circfill(px, py, 6, v)
        circfill(px - 2, py - 2, 2, 7)
        if r >= 9 then lo = true end
      end
    end
  end
  if (state == "playing" or state == "flying") and lo then
    for x = 2, 125, 5 do line(x, 99, x + 2, 99, 8) end
  end
  for _, f in ipairs(flash) do
    if flr((tt - f.t) * 30) % 2 == 0 then circfill(f.x, f.y, 6, 7) end
  end
  if state == "playing" and aiming then
    local px2, py2 = path()
    circ(px2, py2, 6, 6)
    local d = math.sqrt((px2 - SHX) ^ 2 + (py2 - SHY) ^ 2)
    local n = max(1, flr(d / 10))
    for k = 1, n do
      pset(SHX + (px2 - SHX) * k / n, SHY + (py2 - SHY) * k / n, 6)
    end
  end
  if state == "playing" or state == "levelup" then
    circfill(SHX, SHY, 6, cur)
    circfill(SHX - 2, SHY - 2, 2, 7)
  end
  if state == "flying" then
    circfill(bx, by, 6, cur)
    circfill(bx - 2, by - 2, 2, 7)
  end
  for _, d in ipairs(drop) do
    circfill(d.x, d.y, 6, d.c)
  end
  clip()
end

-- ---- test hooks ----
_G.__set_grid = function(spec, curchar)
  grid = {}
  for r = 1, 9 do grid[r] = {} for i = 1, 9 do grid[r][i] = 0 end end
  for r = 1, #spec do
    local s = spec[r]
    for i = 1, #s do
      local v = LET[string.sub(s, i, i)]
      if v and inb(i - 1, r - 1) then grid[r][i] = v end
    end
  end
  flash, drop = {}, {}
  bx, by = 0, 0
  if curchar then cur = LET[curchar] or cur end
  state = "playing"
end
_G.__dbg = function()
  local rowsout, cnt = {}, 0
  for r = 1, 9 do
    local s = ""
    for i = 1, 9 - (r - 1) % 2 do
      local v = grid[r][i]
      s = s .. (v and v > 0 and CH[v] or ".")
      if v and v > 0 then cnt = cnt + 1 end
    end
    rowsout[r] = s
  end
  local px, py = path()
  return {state = state, score = score, level = level, count = cnt, internal = count(),
          cur = cur, next = nxt, grid = rowsout, aim = aim, px = px, py = py}
end
