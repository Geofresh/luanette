-- title: Overworld
-- by: geo
-- about: walk a world of 1,024 screens, stored as blocks the way Mother 1 stored its own; B shows the map
-- api: 5
-- canvas: full

-- A giant world in a few KB. The world is a grid of 64 x 64 blocks, a character each; a block is
-- 8 x 8 map cells, a sprite each. Only the 3 x 3 blocks around the kid are ever a map: MAP is
-- rebuilt from them when the kid walks into another block, and map() draws it under the camera.
-- So the world costs ~4.5 KB and the 25 blocks ~3.5 KB (as one map it would be 256 KB), and every
-- frame is one map() call however big the world is.

SPRITES = {
  [1] = "00000000 00000000 00030000 03030300 00303000 00000000 00000000 00000000",
  [2] = "00000111 00011333 0013333b 013333b3 01333b33 13333333 133b3333 1333333b",
  [3] = "11100000 33311000 33333100 333b3310 33b33310 33333331 b3333331 33333331",
  [4] = "13333333 0133b333 01333333 00113333 00001114 00000014 00000014 00000111",
  [5] = "333b3331 33333310 33333310 33331100 41110000 41000000 41000000 11100000",
  [6] = "ffffffff ffffffff fff4ffff ffffffff ffffffff ffffff4f ffffffff f4ffffff",
  [7] = "00001111 00018888 00188888 01882222 18888888 12222222 18888888 11111111",
  [8] = "11110000 88881000 88888100 22228810 88888881 22222221 88888881 11111111",
  [9] = "01777777 01766c67 0176cc67 01777774 01777774 01777774 01777774 01111111",
  [10] = "77777710 77766c10 777cc610 44777710 94777710 44777710 44777710 11111110",
  [11] = "cccccccc cccccccc cc7ccccc c7c7cccc cccccccc cccccc7c ccccc7c7 cccccccc",
  [12] = "00000000 0e000000 eae00000 0e0000a0 00300a9a 000000a0 00000030 00000000",
  [13] = "00000000 04000040 44444444 04000040 04000040 44444444 04000040 04000040",
  [14] = "00000000 00011100 00166610 01665661 01656561 01555551 00111110 00000000",
  [16] = "00888800 08888880 01ffff10 00f1f100 00ffff00 0dd77dd0 00d77d00 00100100",
  [17] = "00888800 08888880 01ffff10 00f1f100 00ffff00 0dd77dd0 00d77d00 00010010",
  [18] = "00888800 08888880 08888880 01888810 00ffff00 0dddddd0 00dddd00 00100100",
  [19] = "00888800 08888880 08888880 01888810 00ffff00 0dddddd0 00dddd00 00010010",
  [20] = "00888800 08888888 01fff100 001f1f00 00ffff00 00d77d00 00d77d00 00100100",
  [21] = "00888800 08888888 01fff100 001f1f00 00ffff00 00d77d00 00d77d00 01000010",
}

-- The blocks: 8 x 8 cells each (64 x 64 pixels), in MAP's characters ("." grass, 2-5 a tree, 6 path...).
-- The world below names one block a character, so each block's 64 cells are written once, however
-- many times the world uses it. Roads (A-P) join up with their neighbors: a bit each for N, E, S, W.
BLOCKS = {
  ["."] = { -- grass
    "....1...",
    ".1......",
    "........",
    ".....1..",
    "........",
    "........",
    "..1...1.",
    "........",
  },
  [","] = { -- grass
    "........",
    "........",
    "..1.....",
    "....c...",
    "......1.",
    ".1......",
    "........",
    "........",
  },
  [";"] = { -- grass
    "........",
    ".....1..",
    "........",
    "1.......",
    "........",
    ".....23.",
    "...1.45.",
    ".......1",
  },
  ["f"] = { -- forest
    "23232323",
    "45454545",
    "23232323",
    "45454545",
    "23232323",
    "45454545",
    "23232323",
    "45454545",
  },
  ["t"] = { -- trees
    ".....1..",
    ".23.....",
    ".45..23.",
    ".....45.",
    "........",
    "..23....",
    "..45..1.",
    "........",
  },
  ["w"] = { -- pond
    "........",
    "..bbbb..",
    ".bbbbbb.",
    ".bbbbbb.",
    ".bbbbbb.",
    ".bbbbbb.",
    "..bbbb..",
    "........",
  },
  ["n"] = { -- flowers
    "........",
    ".c....c.",
    "...c....",
    "........",
    "..c..c..",
    "...c....",
    ".c..c.c.",
    "........",
  },
  ["r"] = { -- rocks
    "........",
    ".....e..",
    ".e......",
    "........",
    ".1.e....",
    "......e.",
    "..e.....",
    "........",
  },
  ["h"] = { -- house
    "......23",
    ".1....45",
    "......c.",
    "...78...",
    "...9a...",
    "dd.66.dd",
    ".c.66...",
    "...66...",
  },
  ["A"] = { -- road
    "........",
    "........",
    "......1.",
    "...66...",
    "...66...",
    "........",
    "........",
    "........",
  },
  ["B"] = { -- road
    "...66...",
    "...66...",
    "...66.1.",
    "...66...",
    "...66...",
    "........",
    "........",
    "........",
  },
  ["C"] = { -- road
    "........",
    "........",
    "......1.",
    "...66666",
    "...66666",
    "........",
    "........",
    "........",
  },
  ["D"] = { -- road
    "...66...",
    "...66...",
    "...66.1.",
    "...66666",
    "...66666",
    "........",
    "........",
    "........",
  },
  ["E"] = { -- road
    "........",
    "........",
    "......1.",
    "...66...",
    "...66...",
    "...66...",
    "...66...",
    "...66...",
  },
  ["F"] = { -- road
    "...66...",
    "...66...",
    "...66.1.",
    "...66...",
    "...66...",
    "...66...",
    "...66...",
    "...66...",
  },
  ["G"] = { -- road
    "........",
    "........",
    "......1.",
    "...66666",
    "...66666",
    "...66...",
    "...66...",
    "...66...",
  },
  ["H"] = { -- road
    "...66...",
    "...66...",
    "...66.1.",
    "...66666",
    "...66666",
    "...66...",
    "...66...",
    "...66...",
  },
  ["I"] = { -- road
    "........",
    "........",
    "......1.",
    "66666...",
    "66666...",
    "........",
    "........",
    "........",
  },
  ["J"] = { -- road
    "...66...",
    "...66...",
    "...66.1.",
    "66666...",
    "66666...",
    "........",
    "........",
    "........",
  },
  ["K"] = { -- road
    "........",
    "........",
    "......1.",
    "66666666",
    "66666666",
    "........",
    "........",
    "........",
  },
  ["L"] = { -- road
    "...66...",
    "...66...",
    "...66.1.",
    "66666666",
    "66666666",
    "........",
    "........",
    "........",
  },
  ["M"] = { -- road
    "........",
    "........",
    "......1.",
    "66666...",
    "66666...",
    "...66...",
    "...66...",
    "...66...",
  },
  ["N"] = { -- road
    "...66...",
    "...66...",
    "...66.1.",
    "66666...",
    "66666...",
    "...66...",
    "...66...",
    "...66...",
  },
  ["O"] = { -- road
    "........",
    "........",
    "......1.",
    "66666666",
    "66666666",
    "...66...",
    "...66...",
    "...66...",
  },
  ["P"] = { -- road
    "...66...",
    "...66...",
    "...66.1.",
    "66666666",
    "66666666",
    "...66...",
    "...66...",
    "...66...",
  },
}

-- The world: 64 x 64 blocks, 4096 x 4096 pixels, 1024 screens. ~4 KB here; as one map it would be 256 KB.
WORLD = {
  "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff",
  "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff",
  "ff.;,..,....fffffffffft,.;t,.,,.;..ffffffffffftft.....;....,..ff",
  "ff....,,.,ffffffffffffttttt,;,.,...ffffffftft..nn..;.;...f,..,ff",
  "ff;....,..ffffffffEffft,ttt.,.,...;.,.ffffft.ttnnnf...f.fffff.ff",
  "ff..,,,.,.ffffw..,F;,.ftttt,,;,;,..,.f,ffftftf.nnnnf.ff.ffffffff",
  "ff..,....,ffffw;..F...t,ttt;.,.,..,.....;f.t..t.nnffffffffffffff",
  "ff.....,,.,.fwwhhhFhhhttt.tt.,f.f.f.....;.,t,..fnnffffffffffffff",
  "ff,.CKOKKKKKKKKLLLPLLLKKKKKKKKKKKKKKKKKKIw.wEww;fffffffffffffrff",
  "ff.;.tFt.t.,.fnnn.F,.....,,...ffffffnff;..wwFww;ffffffffffffffff",
  "ff...tFtttt.nfnnn,F.,,;.,.,.;ffnfffnfff,wwwwFw.fffffffffffffffff",
  "ff.,.tFttt,..n.n..F...;,;,,..fffnnffff,.;wwwFwwfff,fffffffffffff",
  "ff...nF..t;t..;.;.F.........fffnnffffff...wwF;,.....,.ff,ffff.ff",
  "ff,t.nFnnt;....n;,Fw.;..n...fffffftftft.,.wwF......f..,,f.;f,.ff",
  "ff....Fftnff.;....Fwww..nn...;fffftftt;tt..,F................nff",
  "ff..fnFnnf..t..,..Fwwwwwn;t..ffffffttt.t.t.,F....,,......,.;..ff",
  "ffwfffFffffftft,..Fwwww,;.t.,.f.fftftt,t....F..,f,fftf...f....ff",
  "ffw,.,F,.,fft.t,..F.wwww..tt..,.fft.ttttt...F,.,f;ftftt.fff..nff",
  "ffw.,.F...fff...,.F;wwwww..n,;.,.;;,t.tt,.;.F;;.fffffftfffff.fff",
  "ffw.,,F...ftftftntF,.wwww...,;.;...;.....hhhFh,hffftffttffffffff",
  "ffw...F...CKKKKKKKPKKKKKKKKKKKOKKKKKKKKKKLLLPLKLKKKKKKKKOKKIffff",
  "ffw.,.F..,fff,tnnwF;w.ww..n,.nFnn......;..;.F,.;fftttttfFfffffff",
  "ff...,F.,;ffffttn.Fw..,;,..;,nFn,n..;....;;,F...fffffftfFfffffff",
  "ff,.,,F...ffftt.,;Ftt...;;..,,Fnn.;....,...;F;..ffffffffFfffffff",
  "ff..ffFffffff...,.F....rr.,,,.Fn...,f,.f....F,.fffffffffFfffffff",
  "ff...fFfffffff.;.;F..,,...,...F.f;.fff;,f.,.F...;fffffffFfffffff",
  "ff,;,,Fffff,;.;,,.F.;..r.,.;,,Ff;fffffrff,.,F,..,.w.wfffFfffffff",
  "ff..,.Fffff,f....,F,,...,t,...F;fffnnfffff..F,,..,ww.f.fFfffffff",
  "ff....Fffffff...,;F,;.,t,t;...Fffffffffff...F;....www;frF.ff;.ff",
  "ff.,,.Ffffff,..;.,F;....t.;;,.Ffttffffffff..F..,,.www;..F...,.ff",
  "ff....Ffffff,.....F,.....,;...F.,.nnnfffff.,F....w;w..,.F;,;t.ff",
  "ff,.ffFffff.,;;;.,F.,,..;.,...F..,ffffffff.,F,...,......F..ttrff",
  "fffrffFffff..,r,,.F..,.f..fhhhFhhhnfffffff,,F.tt....,.,.F.....ff",
  "fffCKKPKKKKKOKKKKKPKKKKKKKKLLLPLLLKKKKKKKKKKPKKKKKI.....F.,,..ff",
  "ffrrffFfffffFr,,,.F.t;.ffff.;.F.n;ffffff.ff;Ftttt.;.,...F.,.,.ff",
  "ffffffFfffffF...,,F.,ttffff.,.Fnn;fffffffff.F.tt..,.,...F,.,..ff",
  "ffffffFfffffF,..;.F.,tfffff,.,Fnn.fff.f;ffffFft.,,..,...F,....ff",
  "ffffffFrffffF.,...F,;.f.fff,,,F;n......,ffffFf,;;..,.,.;F.tt.;ff",
  "ffffffFffff,F;.,..F.,....f....Fn,...,..fffffFf.......,,.F,.t,,ff",
  "ffffffFffff.F.,...F,.,...f..,.F...;..,;fffffFf....,..,,.F.tt,tff",
  "ffffffFffff,F.;,.,F....;,..;..F,......,fffffF,..,....,..Ftttttff",
  "ff.fffFffff.F,.;;.F......;.,,.F......,.f.fffF...,.;.....F.tttfff",
  "ffffffFffff,F,,...F..;.;;.,;..F..,,..,..f.f.F.....,..,..Fttftfff",
  "ffffffFff...F.,,.,F,...,;n,..;F,.;...;..,..,F....,..,..,F..,ftff",
  "ffffffFff.,.F....,F.;...n.nn,.F.....,,.n....F,.,...,f.,.F,..ffff",
  "ffffffFff...F,hh.fF.;..,.nnn.,F...,.........F..;fffffhhhF,hhftff",
  "ffffffFff...F.DLKKLKKKKKKKKKKKPKKKKKKKOKKKKKPKKKKKKKKLLLPKLLIfff",
  "ff;fffFff..;F...ftffff,fr,nn;,F,.....,Fn,.;,F..ffffff;..F.,.ftff",
  "ff.fffFff,.;F...ttffffrfnnnn..Frr..;..F.n...F...fffff.;,F.,,ffff",
  "ff.;ffFff,..F.,,tttfffff.n.f.,Frrrr,..Fnn,..F.,,fffff.,.F....;ff",
  "ffnn..F,;.ffFfttttfffff.ff.f,.F,.r...nFn.n..F.,fffftffftB...,.ff",
  "fftnt,F...ffFttttfffffff,fff.;F......,Fn..;,F...ff.ffttt.,.,.;ff",
  "ffn.ntF..,fnFfftffffff;.;ff..,F.r.,.;;F,.;.;F,.,..fft.;t.,..;.ff",
  "ffnt,,F,.ffnFfntffftff.,....r.F,rrr.;,F.....Fr;,,,,,.tt.tt,,..ff",
  "fft.n.F,..;nFnfffffftt.f,.rr.wF.r.....F,...,F.....;....,..tt;.ff",
  "ff,t..F..,..F.nfff,tf.f.ff..wwFwww..,.F;,...F......,..;.ttttt.ff",
  "ffttt.F.,;.;F.,f.ff.....fff..wFwww.hhhFhhh.;F.,,;;.....;.;t..nff",
  "ff,,.CLKKKKKLKKKKKKKKKKKKKKKKKPKKKKLLLPLLLKKJ....,.....;.,,..,ff",
  "ff...;.tt..nnn.,...,..f.f.,.,.Fw.w.;.,F,,....,...,,;,;.,..;...ff",
  "ff;,....ttnnnnnn.,...r.....,.,Fww,,,;.F.,;;;.........,;,.,...;ff",
  "ff,.;..t,,.nnnnnn..,r.;.,,,...B;......B....,...;;..,.;.;...,..ff",
  "ff..,;ttt.nn....n.,.......,;,..;.,,;..,...,...,,;......,...nn.ff",
  "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff",
  "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff",
}

local c = caps()
local W, H = c.w, c.h
local PX, PY = c.picture_x, c.picture_y
local TOP, BOT = PY, PY + 384
local BLOCK, CELLS = 64, 8                       -- a block's pixels and cells a side
local WW, WH = #WORLD[1], #WORLD                 -- the world in blocks
local SOLID = {}                                 -- the sprites the kid can't walk through
for _, n in ipairs({2, 3, 4, 5, 7, 8, 9, 10, 11, 13, 14}) do SOLID[n] = true end
local AREA = { ["."] = "fields", [","] = "fields", [";"] = "fields", f = "the woods", t = "a grove", w = "a pond", n = "a meadow",
               r = "the rocks", h = "a town" }

local kid = { x = 16 * BLOCK + 28, y = 8 * BLOCK + 28, face = "down", step = 0 }
local bx, by = -1, -1                            -- the block the window is built around
local ox, oy = 0, 0                              -- the window's top-left block
local cam_x, cam_y = 0, 0
local last, showing_map, blink = 0, false, 0

-- The block at block coordinates (x, y), the woods off the edge.
local function block_at(x, y)
  if x < 0 or y < 0 or x >= WW or y >= WH then return "f" end
  return WORLD[y + 1]:sub(x + 1, x + 1)
end

-- MAP: the 3 x 3 blocks around block (cx, cy), 24 x 24 cells; the next map() call reads it.
local function build(cx, cy)
  ox, oy = cx - 1, cy - 1
  local rows = {}
  for r = 0, 3 * CELLS - 1 do
    local by_ = oy + r // CELLS
    local line = {}
    for i = 0, 2 do line[i + 1] = BLOCKS[block_at(ox + i, by_)][r % CELLS + 1] end
    rows[r + 1] = table.concat(line)
  end
  MAP = rows
  bx, by = cx, cy
end

-- The sprite in the world's cell at pixel (x, y): read from the window (it always holds the kid).
local function cell(x, y)
  return mget(flr(x / 8) - ox * CELLS, flr(y / 8) - oy * CELLS)
end
local function blocked(x, y)  -- the kid's feet, 6 x 3 px, at (x, y) for its sprite's top left
  for _, p in ipairs({ {x + 1, y + 5}, {x + 6, y + 5}, {x + 1, y + 7}, {x + 6, y + 7} }) do
    if SOLID[cell(p[1], p[2]) or -1] then return true end
  end
  return false
end

-- ---- the bands: the title, where the kid is ----
local function where()
  hd.repaint(0, BOT, W, H - BOT, function()
    hd.gradient(0, BOT, W, H - BOT, 0x000000, 0x16301c)
    local x, y = flr(kid.x / 8), flr(kid.y / 8)
    hd.text(("%s  ·  block %d, %d  ·  cell %d, %d"):format(AREA[block_at(bx, by)] or "the road", bx, by, x, y),
      W / 2, BOT + 14, 0xcfe8cf, 14, "mono", "center")
    hd.text("walk  ·  hold A: run  ·  B: map", W / 2, BOT + 34, 0x7f9a80, 12, "mono", "center")
  end)
end

-- ---- the whole map, over the picture: a dot a block (4 x 4 px), drawn once each time it opens ----
local MAP_COLOR = { ["."] = 0x9fe39a, [","] = 0x9fe39a, [";"] = 0x9fe39a, f = 0x2f7d38, t = 0x58a85a, w = 0x3d7fe0, n = 0xe7a0c8, r = 0x8c8c8c,
                    h = 0xd8443c }
local MS = 4
local MX, MY = PX + (384 - WW * MS) // 2, PY + (384 - WH * MS) // 2
local runs = {}     -- the map's rects, worked out once: x, y, w, color, four numbers a run, by row
local drawn = 0    -- rows of it drawn since it opened: 16 a frame, so it sweeps in
local function plan_map()
  for y = 0, WH - 1 do  -- a run of the same color is one rect
    local row, x, list = WORLD[y + 1], 0, {}
    while x < WW do
      local col = MAP_COLOR[row:sub(x + 1, x + 1)] or 0xe8c98a  -- roads
      local run = 1
      while x + run < WW and (MAP_COLOR[row:sub(x + run + 1, x + run + 1)] or 0xe8c98a) == col do run = run + 1 end
      list[#list + 1] = { x, run, col }
      x = x + run
    end
    runs[y + 1] = list
  end
end
local function open_map()
  hd.rect(MX - 6, MY - 6, WW * MS + 12, WH * MS + 12, 0x0b1a0e, 235, 8)
  drawn = 0
end
local function more_map()
  for y = drawn, min(drawn + 15, WH - 1) do
    for _, r in ipairs(runs[y + 1]) do hd.rect(MX + r[1] * MS, MY + y * MS, r[2] * MS, MS, r[3]) end
  end
  drawn = min(drawn + 16, WH)
end
local function close_map()
  hd.clip(MX - 6, MY - 6, WW * MS + 12, WH * MS + 12)
  hd.clear()
  hd.clip()
end

function _init()
  -- Mother-ish colors: pale grass, a darker leaf, outlines near black (color 0 is see-through in map())
  palette(11, 0xb8ecb0)
  palette(3, 0x2f8f3a)
  palette(1, 0x101418)
  palette(15, 0xf0d8a0)
  fps(60, 10)
  hd.scanlines(false)
  hd.gradient(0, 0, W, TOP, 0x16301c, 0x000000)
  hd.text("OVERWORLD", W / 2, TOP / 2 - 22, 0xffffff, 24, "bold", "center")
  hd.text("4096 x 4096 px · 1,024 screens · ~8 KB of data", W / 2, TOP / 2 + 6, 0x9fcf9a, 12, "mono", "center")
  build(flr(kid.x / BLOCK), flr(kid.y / BLOCK))
  where()
  plan_map()
  last = time()
  log("world", WW .. " x " .. WH, "blocks; start block", bx, by)
end

function _update()
  local now = time()
  local dt = min(now - last, 0.1)
  last = now
  if btnp(5) then
    showing_map = not showing_map
    if showing_map then open_map() else close_map() end
  end
  if showing_map then  -- the kid's block blinks on the map
    if drawn < WH then more_map() return end
    blink = blink + dt
    hd.rect(MX + bx * MS, MY + by * MS, MS, MS, blink % 0.5 < 0.25 and 0xffffff or 0xff3030)
    return
  end
  local dx = (btn(1) and 1 or 0) - (btn(0) and 1 or 0)
  local dy = (btn(3) and 1 or 0) - (btn(2) and 1 or 0)
  if dx ~= 0 or dy ~= 0 then
    local speed = (btn(4) and 128 or 64) * dt * ((dx ~= 0 and dy ~= 0) and 0.707 or 1)  -- 64 px a second, A runs
    local nx, ny = kid.x + dx * speed, kid.y + dy * speed
    if not blocked(nx, kid.y) then kid.x = nx end  -- each axis on its own: slide along walls
    if not blocked(kid.x, ny) then kid.y = ny end
    kid.face = dy > 0 and "down" or dy < 0 and "up" or dx < 0 and "left" or "right"
    kid.step = kid.step + dt
  end
  kid.x = mid(0, kid.x, WW * BLOCK - 8)
  kid.y = mid(0, kid.y, WH * BLOCK - 8)
  local cx, cy = flr((kid.x + 4) / BLOCK), flr((kid.y + 4) / BLOCK)
  if cx ~= bx or cy ~= by then
    build(cx, cy)
    where()
  end
  cam_x = flr(mid(0, kid.x - 60, WW * BLOCK - 128))
  cam_y = flr(mid(0, kid.y - 60, WH * BLOCK - 128))
end

local KID = { down = 16, up = 18, left = 20, right = 20 }
function _draw()
  if showing_map then return end
  cls(11)
  clip(1, 0, 126, 128)  -- columns 0 and 127 carry on out to the glass's edges: keep them grass
  camera(cam_x, cam_y)
  map(0, 0, ox * BLOCK, oy * BLOCK)  -- the window, where its blocks are in the world
  local frame = KID[kid.face] + (flr(kid.step * 6) % 2)
  spr(frame, flr(kid.x), flr(kid.y), kid.face == "left")
  camera()
  clip()
end
