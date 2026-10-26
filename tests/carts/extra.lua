-- title: Extra
-- by: tests
-- about: a port's own call (Host::extra_api, check.py's EXTRA): tests/extra.py gives it twice(n)
-- api: 6

local logged = false

function _update()
  if not logged then
    logged = true
    local c = caps()
    log(#c.extras, c.extras[1], twice and twice(21))
  end
end

function _draw()
  cls(1)
end
