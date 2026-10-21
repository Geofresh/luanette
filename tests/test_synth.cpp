#include <cstdlib>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "check.hpp"
#include "synth.hpp"

namespace {

std::shared_ptr<const carts::Sound> sound(const char* text) {
  auto s = std::make_shared<carts::Sound>();
  return carts::parse_sound(text, *s).empty() ? s : nullptr;
}

// Samples until the synth goes quiet (at most `limit`), and the loudest one.
int64_t play_out(carts::Synth& synth, int64_t limit, int* peak = nullptr) {
  std::vector<int32_t> acc(160);
  int64_t n = 0;
  while (synth.busy() && n < limit) {
    std::fill(acc.begin(), acc.end(), 0);
    synth.render(acc.data(), 160);
    for (int32_t s : acc) if (peak) *peak = std::max(*peak, std::abs(s));
    n += 160;
  }
  return n;
}

}  // namespace

TEST("synth: a sound's text parses, with spaces anywhere; bad text says what's wrong") {
  carts::Sound s;
  CHECK(carts::parse_sound("10 0000 18350 1c350 1f355", s).empty());
  CHECK(s.speed == 16 && s.notes.size() == 3 && !s.loops());
  CHECK(s.notes[0].pitch == 0x18 && s.notes[0].instrument == 3 && s.notes[0].volume == 5 && s.notes[2].effect == 5);
  CHECK(carts::parse_sound("080103 18350 1c350 1f350", s).empty() && s.loops() && s.loop_start == 1 && s.loop_end == 3);
  CHECK(!carts::parse_sound("", s).empty());
  CHECK(!carts::parse_sound("10 0000 1835", s).empty());          // a note short
  CHECK(!carts::parse_sound("10 0000 40350", s).empty());         // pitch past 3f
  CHECK(!carts::parse_sound("10 0000 18850", s).empty());         // instrument 8
  CHECK(!carts::parse_sound("00 0000 18350", s).empty());         // speed 0
  CHECK(!carts::parse_sound("10 0005 18350", s).empty());         // loops past its notes
  CHECK(!carts::parse_sound("10 0000 1835g", s).empty());
  std::string many = "10 0000";
  for (int i = 0; i < 33; ++i) many += " 18350";
  CHECK(!carts::parse_sound(many, s).empty());
}

TEST("synth: a pattern's text parses; -- is no sound") {
  carts::Pattern p;
  CHECK(carts::parse_pattern("01 08 09 -- 3f", p).empty());
  CHECK(p.flags == 1 && p.sound[0] == 8 && p.sound[1] == 9 && p.sound[2] == -1 && p.sound[3] == 63);
  CHECK(!carts::parse_pattern("00 07 -- -- --", p).empty());  // 0-7 are the built-in ones, not for songs
  CHECK(!carts::parse_pattern("00 08 -- --", p).empty());
  CHECK(!carts::parse_pattern("-- 08 -- -- --", p).empty());
}

TEST("synth: a sound plays for its notes' time, audibly, and the same each time") {
  carts::Synth a(16000), b(16000);
  a.sfx(sound("10 0000 18350 1c350 1f350"));  // 3 notes of 16 ticks: 0.4 s
  b.sfx(sound("10 0000 18350 1c350 1f350"));
  int peak = 0;
  const int64_t n = play_out(a, 16000 * 5, &peak);
  CHECK(n >= 6400 && n < 6400 + 160);
  CHECK(peak > 3000 && peak < 32768);
  std::vector<int32_t> x(800, 0), y(800, 0);
  carts::Synth c(16000), d(16000);
  c.sfx(sound("10 0000 18750 2b711 3f736"));  // noise, slide, arpeggio
  d.sfx(sound("10 0000 18750 2b711 3f736"));
  c.render(x.data(), 800);
  d.render(y.data(), 800);
  CHECK(x == y);
}

TEST("synth: every instrument and effect sounds, and a volume-0 note is a rest") {
  for (int inst = 0; inst < carts::kInstruments; ++inst) {
    for (int fx = 0; fx < carts::kEffects; ++fx) {
      char text[32];
      std::snprintf(text, sizeof text, "08 0000 18%x7%x 24%x7%x", inst, fx, inst, fx);
      carts::Synth s(16000);
      s.sfx(sound(text));
      int peak = 0;
      play_out(s, 16000, &peak);
      CHECK(peak > 1000);
    }
  }
  carts::Synth s(16000);
  s.sfx(sound("08 0000 18300 18300"));
  int peak = 0;
  CHECK(play_out(s, 16000, &peak) > 0);
  CHECK(peak == 0);
}

TEST("synth: a looping sound plays until it's stopped") {
  carts::Synth s(16000);
  s.sfx(sound("04 0002 18350 1c350"));
  CHECK(play_out(s, 16000 * 3) >= 16000 * 3);
  s.sfx(nullptr);
  CHECK(!s.busy());
}

TEST("synth: a song steps through its patterns, loops back, and stops where it says") {
  std::map<int, std::string> music = {{0, "00 08 -- -- --"}, {1, "04 08 09 -- --"}};
  std::map<int, std::string> sounds = {{8, "10 0000 18350 1c350"}, {9, "10 0000 24350 28350 2b350 30350"}};
  auto text = [](std::map<int, std::string>& m) { return [&m](int i) { return m.count(i) ? m[i].c_str() : nullptr; }; };
  std::shared_ptr<const carts::Song> song;
  CHECK(carts::build_song(0, text(music), text(sounds), song).empty());
  carts::Synth s(16000);
  s.music(song);
  // Pattern 0: 2 notes of 16 ticks; pattern 1: its longest sound, 4 notes; then it stops.
  const int64_t n = play_out(s, 16000 * 10);
  CHECK(n >= 16000 * 96 / 120 && n < 16000 * 96 / 120 + 320);

  music[1] = "02 08 09 -- --";  // loop back, but no pattern starts a loop: it plays on past the end and stops
  CHECK(carts::build_song(0, text(music), text(sounds), song).empty());
  music[0] = "01 08 -- -- --";
  CHECK(carts::build_song(0, text(music), text(sounds), song).empty());
  s.music(song);
  CHECK(play_out(s, 16000 * 4) >= 16000 * 4);  // loops for good
  s.music(nullptr);
  CHECK(!s.busy());

  std::shared_ptr<const carts::Song> none;
  CHECK(!carts::build_song(5, text(music), text(sounds), none).empty());  // no MUSIC[5]
  music[2] = "00 0a -- -- --";
  CHECK(carts::build_song(0, text(music), text(sounds), none).find("SOUNDS[10]") != std::string::npos);
}

TEST("synth: a sound takes a channel the song leaves free, and the song keeps time under it") {
  std::map<int, std::string> music = {{0, "01 08 08 08 08"}, {1, "02 08 08 08 08"}};
  std::map<int, std::string> sounds = {{8, "10 0000 18350 1c350"}};
  auto text = [](std::map<int, std::string>& m) { return [&m](int i) { return m.count(i) ? m[i].c_str() : nullptr; }; };
  std::shared_ptr<const carts::Song> song;
  CHECK(carts::build_song(0, text(music), text(sounds), song).empty());
  carts::Synth s(16000);
  s.music(song);
  s.sfx(sound("10 0000 30350"));
  CHECK(s.busy());
  std::vector<int32_t> acc(16000 / 2, 0);
  s.render(acc.data(), static_cast<int>(acc.size()));  // past the sound's end: the song plays on
  CHECK(s.busy());
}
