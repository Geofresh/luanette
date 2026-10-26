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
