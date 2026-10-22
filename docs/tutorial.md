# Your first cart

This makes a small game from nothing: stars fall, you catch them in a basket. It takes about half an
hour, all of it in the [playground](https://geofresh.github.io/luanette/) in your browser, and at the
end it has its own sounds and a song, and it runs on a board if you have one.

You don't need to know Lua already. Each step shows the code to type and says what it does. If a step
won't work, its "stuck?" link opens that step finished, in the playground, next to yours.

## 1. Something on the screen

In the playground, open the **File** menu, press **New cart**, call it `stars`, and press Create. The editor fills with a
small example cart; select all of it and replace it with this:

```lua
-- title: Stars
-- by: you
-- about: catch the falling stars

x = 56  -- the basket's left edge, in picture pixels (0 to 127)

function _draw()
  cls(1)                             -- clear the picture to color 1, dark blue
  rectfill(x, 118, x + 15, 121, 9)   -- the basket: 16 pixels wide, orange (9)
end
```

Press **Run** (or Ctrl+Enter). An orange bar sits near the bottom of the screen: the basket.

The screen is a picture 128 pixels wide and 128 tall, with (0, 0) at the top left. It has 16 colors,
numbered 0 to 15. `_draw` runs every frame and draws the picture again from nothing: `cls` clears
it, `rectfill` fills a box from one corner to the other.

Stuck? [Open step 1 in the playground](https://geofresh.github.io/luanette/#cart=dfU9Na8MwDP0rD1-WsBTmQrs1sF-xa6A4jpKaanaxFZoy9t-ndPSyw3SR9CTex5eJ7pNMa4q4XI5F6GJNY0r2im02kCBMLT7WaxcV6G8tbmm-z65Ps7TwTvwJciKMjjnECeX3vYsL3rHbAyuT3ntXziRPBUyjgIaJGoSIS_AyZ9K-EBdUL5AEu32tV4pxjl5CijgO2V0rxQDPpbI1_itV9Ewu33UfAkrrE6cM22Bw-YyeZ1oJM3kZA3O1NLD2rcGCZ9idLlt9PdT4E6GF3T_sXsOgMVJ2cSJUBzVIceii-f4B) (`tests/tutorial/step1.lua`).

## 2. Moving the basket

A cart's `_update` runs every frame before `_draw`: it's where things change. Add this above
`_draw`:

```lua
function _update()
  if btn(0) then x = x - 2 end       -- left
  if btn(1) then x = x + 2 end       -- right
  local tx = touch()                 -- a finger on the screen: the basket goes under it
  if tx then x = tx - 8 end
  x = mid(0, x, 112)                 -- keep it on the screen
end
```

Run it. The arrow keys move the basket, and so does a finger (or the mouse) on the screen. `btn(0)` is
left and `btn(1)` right; on a board, dragging is the d-pad. `touch()` says where the screen is
touched, and `mid` keeps a number between two others.

Stuck? [Open step 2 in the playground](https://geofresh.github.io/luanette/#cart=dfVHRasJAEPyVIS9NaAQT0GqgX9FXQS6XjR6ed-Fug0rpv3cv1qJSui_J7Q4zszufmVNHypossgpxG5mGOiuzGLT0ZjOwYUsNPtJ046TRXhpc_Dj9q9aP3EAr1nvwntAra43bIV7hG3fGOxZLIDHJvFXxQPwSYalnULejEsZhMJrHQPI9k43I52CPqn4rEkU_Os3GO2zHoVNMuXQB06Nll8-LxOuQdM6YoQa5DtcSzSRzh64e0K_P6GB2-wluvVYWnGDsR73PCzxX2h697EoB4i0tF3Ugcs3doth5ihhdJyBzMyK0vyY4eV4lF2mYOkfT5fMS5xJVVf8peyAahO1RdeMmjvtrdUGdrrfSNqbV_yvh1ZZUmChvcUgI2lsfUJXoVDigtSMlwkCae2NtPtlcldMxq4U8aoGuCzwF3qBa3sI9mU5C90HJ6ZCvix_n2dc3) (`tests/tutorial/step2.lua`).

## 3. A star and a score

Now a star that falls, and a score for each one you catch. Replace the line `x = 56` with:

```lua
x = 56                   -- the basket's left edge, in picture pixels (0 to 127)
star = {x = 64, y = 0}   -- where the star is
score = 0

function new_star()
  star.x = flr(rnd(120)) + 4
  star.y = 0
end
```

Add these lines at the end of `_update`, before its `end`:

```lua
  star.y = star.y + 1 + score / 10   -- it falls, faster as the score goes up
  if star.y >= 116 and star.x >= x and star.x <= x + 15 then
    score = score + 1                -- caught
    new_star()
  elseif star.y > 128 then
    score = 0                        -- missed: start again
    new_star()
  end
```

And draw the star and the score in `_draw`, after `cls(1)`:

```lua
  circfill(star.x, star.y, 2, 10)    -- the star: yellow (10)
  print("score " .. score, 2, 2, 7)
```

Run it and catch a few. Each catch makes the stars fall a little faster.

Stuck? [Open step 3 in the playground](https://geofresh.github.io/luanette/#cart=ddVPtjtowEHyVUf40EYEm6R2lqHcv0b9IyDgOWGccZDsCdLp3766du_LVKJEd7-x4drx-z6zYq2yZ-SCcX_ugDj-yMvNO0tp0iqCDUUv84ejK0sLmvMS5H-JcbPohLCFFkDuEnUInjNF2C5_gK3vCC57nuH-YmhI2wr-p8M3DqC5AtVtVQlsctAyDUzSelPHIK4QedfOzWFmmJtJ3Zp4_lTjTWH0kxuNOURLzRpQmCV72tEQQVtMNVgbdW1h1XDMkJ0JE8Iz5OuNyZ9u8bqqiwARPX9FzolC2veJZD4dWBJVodIdNsHlVsAILJjxhigaU9a9qLvQCXV-hJ7dop7e7CDe9FAaBYaEf5C4vHlkq0JH9yoG0RRukU8ouL6zGtlceg20JpD-FEO2XiMCaF4iVIq7sdZtXJU4l6rp5uO2bUgdiu96Vjbpwb5xMUNOXTuU76ioRUC63ji9poBZ0EH5kYlySfBjFjkSvLyRnDkFujef3yg5e_P9OjtbPsTjOBj7bIY0s5r4aKYbRdNw0CvWiulBAHbl4wF3hPw9x77X3ql1GigCxFdo-3Ijdv2u21oljikvjqXPiTDvZaWPyVHQ5iivR0HFVxcVV4wBdXWVMf0ROMU53SoaYHk93UY6O0U9Tl_hV3N5Uzjk4bUO-ylK9qwyzWao9bkovX9MoPvv4Cw) (`tests/tutorial/step3.lua`).

## 4. Sounds

Open the **Sounds** tab. This is where a cart's own sounds are made; it writes them into your code.

1. Pick **New from… → Coin**. That's sound 8. Press **Play** (or Space) to hear it, and type
   `catch` in the name box.
2. Make one for a miss: press **+ New sound** (that's sound 9) and name it `miss`. It starts as
   two short notes, the second fading out. Make it fall: click the second note a few rows lower in
   the top area of the grid. Then set **Speed** to 6 to slow it down, and play it.

Back in the **Code** tab, the editor has added a `SOUNDS` table near the top, and `-- api: 3` to the
header, which tells a board the cart needs sound and music. Now play them: add `sfx(8)` next to
`score = score + 1`, and `sfx(9)` next to `score = 0`.

Stuck? [Open step 4 in the playground](https://geofresh.github.io/luanette/#cart=ddVTRbpswFP2VI14GKu2AJlkarX3a8_ZQ7WlMlQMmseoaZBslUdV_3702bZM2QwkY-9xzzz2-5jkx4kkmq8R5Yd2D83KYJXnibENzl5fwymu5wj2v1oYm1ocVDv0YxmLdj36FRvhmC7-V6ITWymzg3uBiUCtc1ya8uH40rUPK0EGLw8byxBcXFyBb5XuLnVVeOuJTdBNrLbPa3P_6_fPHPW7xXBvgz_IvDeukuEZBF6quXBS4npWLeZ3koExBUoDeTNBFhJZNMaf7spi_Qp-UI6kvLHFP0PkCny82gjSvhXuUnvRq2XmSu5E5lMGgGj9aKkntpabyCvgeZfWNdLMRrJqZF7McB3oWL5Fxt5UUxLwBpUiFa3qaIgir6UbTeNUbGLl7YEiacUU8umK-TtvUmjYtqyLLcIHZ2-ohUkjTnvA8jEMrvIw0qsPam7TIWIEBE-5xiQoU9V41F3qELk_QFx_RVm22Aa77Rmh4hvl-bLZpds5SgY6aRVqQtmBDY6U0qyOrsempE6g3CKRehRDtmwjPmpcIlSLMPKk2LXLsc5RldTbto5QDsZ1mZaOO3JsGFyjpH3flK8oiElAsN7rL6UEHxkK4iYlxUfIwiZ2I7m5JzgKC3Jr2744dPHr_Hh0t56E4jgZe2yE-WcznahoxTqYTvtunyyyOT5uG-lIeqaHuXJ7JU-A_13RMZLsKFB5iI5R5T3pzPinvyqcmbK3YxfVGO-qoMFK26ZTWaTQjn4TmqGgbi-zoCPICfYCk1v0OKa1xuJWND-Fh15f55CS9VGWOm-zjCeaYwSrj0zqJtdcJrq6iDyEp_fj4BvHJyz8) (`tests/tutorial/step4.lua`).

## 5. A song

A song is made of sounds too: a few play together, and the song steps through them.

1. In the Sounds tab, pick **New from… → Bass line**. That's sound 10.
2. Press **+ New sound** for sound 11, the tune. Pick **pulse**, set **Notes** to 8 and **Speed**
   to 16, and draw eight notes. Anything you like: play it as you go.
3. Open the **Music** tab and press **+ Pattern** twice. In pattern 0 pick sound 10 for the first
   channel and tick **loop start**. In pattern 1 pick 10 and 11, and tick **loop back**.
   **Play the song**: the bass on its own, then the tune over it, round and round.

Then start the song when the cart starts. Add this to your code:

```lua
function _init()
  music(0)                           -- the song, from pattern 0, round and round
end
```

Stuck? [Open step 5 in the playground](https://geofresh.github.io/luanette/#cart=dfVXfb9owEP5XTnlZoqadHUgKaO3L9rKHbQ-oT8tUGccBq8GJbEeAJv73ne1AQ8sawXHxfb777ofN30ixrYgWkbFMm2djRZdHaWQ0x7XbW7DSNmIBS2ctFS6sDgs4tL3X2art7QI4s3wDdiOgZk0j1RrMGc46uYBJqfyLaXtVGYgdtGvYYa3dwicTDCAqaVsNOy2tMOhPomCrRiSlWv56-vltCQ_wt1QAv2d_UC0jMgGCD2Q1LQhMprTIyygFjOQpeeh8gBYBSjnJUc5IfoJupTEeSUmAUhKghE_ys7wfyflI5uf1wd2KndzRS3fZjE5RrrwM-tTLYrQS9AnKwZ3tlSjV0ZfPlc20WN73Bdz2RvKPCvjjafn967l-Q6aEAmEuiv9gxEB8MGbOSFYj49H1cY_WvID3z8AQC_AiLHJqRG2R0lqkIBV0ktteI225Fw3OAAHbAs3ukZubFkfNeS6mKRzwlxyDx91G4CafuUNJrK3hLS4hxLGpe8WtbBUosXt2kDhxSTjtzvmrGx1rVcU0I0kCNzA9Ww_BhVDVhZ9nqaQNTnxRY5LA_59RV1KodbuFjlkrtAKSgm8OMPx67UqsvquYFSGarGFllQuHHhU48nu4hQxw12s0V9QRml6gb96itVxvPLxpOWvAOphte76Jk2upMKjx9AoNyM2nxbUQajFqK6xbnCxMBkHyRATdnklYx3kGPlPwK1tZxViMfQqUZlfDvgjRobfLqK5Qo04Nyg1Q_IYJ-Ax4tLwD3OtuHoMtYHiDaWBm8ORwgXI3kB0cPT4gncI3Z5iVR1fB0fuXUFGa--TcboDT6IVfR-Z9Npz1Q9ERX-_jWRL0ywHFMyBGbPAkzK7EIR_Mnbu3RLXwLiywNZPqNej8etCrQ1hptgt23hicKK9JzWvZNHEoRjoQTSHDNoYTcRp9NOA_gmiadgcx2tx2Lbj1233XZ-lQSXzJaArz5O1t4fZ0Wiobl1HIvYzg7i7UwQfFj7sqPPno-A8) (`tests/tutorial/step5.lua`).

## 6. On a board

If you have a Waveshare ESP32-S3-Touch-AMOLED-2.06, plug it in with a USB cable. In Chrome or Edge,
open the **Board** menu and press **Install Luanette** (leave "Erase device" unticked), then **Send
this cart to the board**.
Stars is on the board, with its sounds and song, and it stays there.

## 7. Show it to someone

**Share, Copy a share link** copies a link with your whole cart in it. Whoever opens it gets Stars in their own
playground, to play or change. **Share, Submit to the gallery** puts it forward for the
[gallery](../gallery/), where everyone who opens the playground can find it.

## Where to go next

- [`api.md`](api.md) lists everything a cart can do: the full-color layer, a 3D scene, images,
  saves. The editor shows the same help as you type.
- [`DESIGN.md`](DESIGN.md) covers making a cart that runs well and uses the whole screen.
- The examples and the gallery are complete carts to read. Open one and change something.
