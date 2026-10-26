-- title: state demo
-- by: luanette
-- about: a title, a game of catching dots, and game over, as three scenes (the state library)
-- api: 1

-- lib: state 1, api 1: scenes (a title, the game, game over), each with its own update and draw
-- A scene is a table with any of enter(...), update(), draw() and leave(). state.go(scene, ...)
-- switches; call state.update() in _update and state.draw() in _draw.
local state = {}
local current, since = nil, 0

-- Leaves the scene there is and enters this one (enter gets the other arguments).
function state.go(scene, ...)
  if current and current.leave then current.leave() end
  current, since = scene, time()
  if scene.enter then scene.enter(...) end
end

function state.update()
  if current and current.update then current.update() end
end

function state.draw()
  if current and current.draw then current.draw() end
end

-- Whether this is the scene now.
function state.is(scene)
  return current == scene
end

-- Seconds since this scene began (for a title that fades in, a "press A" that waits a moment).
function state.time()
  return time() - since
end
-- end lib: state

local title, game, over = {}, {}, {}
local score

function title.draw()
  cls(1)
  print("catch the dots", 36, 50, 7)
  if state.time() % 1 < 0.6 then print("press a", 50, 70, 6) end
end
function title.update()
  if btnp(4) then state.go(game) end
end

function game.enter()
  score = 0
  game.x, game.dot = 64, {x = rnd(120), y = 0}
end
function game.update()
  if btn(0) then game.x = game.x - 2 end
  if btn(1) then game.x = game.x + 2 end
  local d = game.dot
  d.y = d.y + 1.5
  if d.y > 110 and math.abs(d.x - game.x) < 10 then
    score = score + 1
    game.dot = {x = rnd(120), y = 0}
  elseif d.y > 128 then
    state.go(over, score)
  end
end
function game.draw()
  cls(0)
  circfill(game.dot.x, game.dot.y, 3, 10)
  rectfill(game.x - 8, 114, game.x + 8, 117, 12)
  print(score, 2, 2, 7)
end

function over.enter(s)
  over.score = s
end
function over.update()
  if state.time() > 1 and btnp(4) then state.go(title) end
end
function over.draw()
  cls(2)
  print("game over: " .. over.score, 38, 56, 7)
end

function _init()
  state.go(title)
end

function _update()
  state.update()
end

function _draw()
  state.draw()
end
