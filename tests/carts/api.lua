-- title: API
-- by: tests
-- about: logs the name of every global a cart sees, and every hd and ui function; parity.py compares the lists
-- api: 5

-- tools/api_check.py also reads this cart's log and checks every name against docs/api.md.
local names = {}
local function add(prefix, t)
  for k, v in pairs(t) do
    -- functions, and the tables of them (check.py's calls are Python's, so any type but plain values)
    local plain = type(v) == "number" or type(v) == "string" or type(v) == "boolean"
    if type(k) == "string" and not plain and (prefix == "" or type(v) ~= "table") then
      names[#names + 1] = prefix .. k
    end
  end
end
add("", _G)
add("hd.", hd)
add("ui.", ui)
table.sort(names)

-- log() takes 200 characters a line and 20 lines a frame: a few lines a frame, packed.
local lines, line = {}, ""
for _, n in ipairs(names) do
  if #line + #n + 1 > 190 then lines[#lines + 1] = line; line = "" end
  line = line == "" and n or line .. " " .. n
end
lines[#lines + 1] = line

local t = 0
function _update()
  t = t + 1
  for i = (t - 1) * 10 + 1, math.min(t * 10, #lines) do log(lines[i]) end
end

function _draw()
  cls(1)
  print(#names .. " names", 2, 2, 7)
end
