-- title: Stars
-- by: you
-- about: catch the falling stars
-- api: 3

-- sounds (the playground's sound editor writes this table)
SOUNDS = {
  [8] = "03 0000 2f160 34165", -- catch
  [9] = "06 0000 1c050 18055", -- miss
  [10] = "10 0000 0c350 0c350 07350 07350 09350 09350 05350 07350", -- bass
  [11] = "10 0000 28140 2b140 28140 24140 26140 28140 26140 23140", -- tune
}
-- the song (the playground's music editor writes this table)
MUSIC = {
  [0] = "01 0a -- -- --",
  [1] = "02 0a 0b -- --",
}

x = 56                   -- the basket's left edge, in picture pixels (0 to 127)
star = {x = 64, y = 0}   -- where the star is
score = 0

function new_star()
  star.x = flr(rnd(120)) + 4
  star.y = 0
end

function _init()
  music(0)                           -- the song, from pattern 0, round and round
end

function _update()
  if btn(0) then x = x - 2 end       -- left
  if btn(1) then x = x + 2 end       -- right
  local tx = touch()                 -- a finger on the screen: the basket goes under it
  if tx then x = tx - 8 end
  x = mid(0, x, 112)                 -- keep it on the screen

  star.y = star.y + 1 + score / 10   -- it falls, faster as the score goes up
  if star.y >= 116 and star.x >= x and star.x <= x + 15 then
    score = score + 1                -- caught
    sfx(8)
    new_star()
  elseif star.y > 128 then
    score = 0                        -- missed: start again
    sfx(9)
    new_star()
  end
end

function _draw()
  cls(1)
  circfill(star.x, star.y, 2, 10)    -- the star: yellow (10)
  rectfill(x, 118, x + 15, 121, 9)   -- the basket
  print("score " .. score, 2, 2, 7)
end
