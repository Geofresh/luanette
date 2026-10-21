// The carts' own sounds and music (docs/api.md, "Sound and music"): a small tracker synth in the
// spirit of PICO-8's. A sound is up to 32 notes played at a speed, each note a pitch, one of eight
// instruments, a volume and an effect; a song is a list of patterns, each up to four sounds played
// together. Carts keep them as text (SOUNDS[8..63] and MUSIC[0..63]); the playground's editor
// writes it. Four channels play them, music and sounds sharing them. Portable: the board runs a
// Synth in its sound task (esp32/sound.cpp), the playground in the browser.
#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace carts {

constexpr int kSoundFirst = 8, kSoundLast = 63;  // a cart's sounds: SOUNDS[8..63] (0-7 are built in)
constexpr int kPatterns = 64;                    // a cart's song: MUSIC[0..63]
constexpr int kNotes = 32;                       // a sound's notes, at most
constexpr int kChannels = 4;
constexpr int kTickHz = 120;                     // a sound's speed: ticks of 1/120 s a note

// Instruments and effects, by their number in a note.
enum Instrument : uint8_t { kSquare, kPulse, kThinPulse, kTriangle, kSaw, kSine, kOrgan, kNoise, kInstruments };
enum Effect : uint8_t { kPlain, kSlide, kVibrato, kDrop, kFadeIn, kFadeOut, kArpFast, kArpSlow, kEffects };

struct Note {
  uint8_t pitch = 0;       // 0-63: C2 (65 Hz) to D#7
  uint8_t instrument = 0;  // Instrument
  uint8_t volume = 0;      // 0-7; 0 is a rest
  uint8_t effect = 0;      // Effect
};

struct Sound {
  std::vector<Note> notes;  // 1 to kNotes
  int speed = 16;           // ticks a note, 1-255
  int loop_start = 0, loop_end = 0;  // when end > start: after reaching `end`, play on from `start`, for good
  bool loops() const { return loop_end > loop_start; }
};

// A song's step: up to four sounds at once. It lasts as long as its longest sound's notes take.
// Its flags, as PICO-8's: 1 a loop starts here, 2 then go back to it, 4 stop after this one.
enum PatternFlag : uint8_t { kLoopStart = 1, kLoopBack = 2, kStopAfter = 4 };
struct Pattern {
  int sound[kChannels] = {-1, -1, -1, -1};  // a SOUNDS number per channel, or -1
  uint8_t flags = 0;
};

struct Song {
  Pattern patterns[kPatterns];
  bool has[kPatterns] = {};
  std::shared_ptr<const Sound> sounds[kSoundLast + 1];  // the ones the patterns play
  int start = 0;
};

// A sound or pattern from a cart's text: "" when it's fine, else what's wrong.
//   sound:   "SS LL EE" (speed, loop start, loop end), then a note per "PP I V E" (pitch,
//            instrument, volume, effect); hex digits, spaces anywhere. "10 0000 18350 1c350"
//   pattern: "FF" (flags), then a sound number per channel, or "--". "00 08 09 -- --"
std::string parse_sound(std::string_view text, Sound& out);
std::string parse_pattern(std::string_view text, Pattern& out);

// A song from MUSIC[start] on, its patterns and their sounds looked up as text (null: none).
// "" when it's fine, else what's wrong ("MUSIC[3] channel 2: SOUNDS[9] ...").
using TextOf = std::function<const char*(int)>;
std::string build_song(int start, const TextOf& pattern, const TextOf& sound, std::shared_ptr<const Song>& out);

// Plays sounds and a song on four channels. One thread at a time (the board's sound task).
class Synth {
 public:
  explicit Synth(int rate);
  // A sound on a channel the song isn't using if there is one, else one it is (the song is quiet
  // there until the sound ends). sfx(nullptr) stops the sounds; music(nullptr) the song.
  void sfx(std::shared_ptr<const Sound> sound);
  void music(std::shared_ptr<const Song> song);
  bool busy() const;  // anything still to play
  // Mixes the next n samples into `acc` (added; the caller clips).
  void render(int32_t* acc, int n);

  struct Voice {
    std::shared_ptr<const Sound> sound;
    int note = 0;
    int64_t ticks = 0;      // ticks before this note
    int64_t start = 0;      // the sample this note began at, counted from the sound's start
    int64_t pos = 0;        // samples since the sound's start
    int64_t end = 0;        // the sample this note ends at
    float prev = -1;        // the pitch before (slides start there)
    float phase = 0, vol = 0, level = 0;
    uint32_t lfsr = 0xACE1u;
    uint32_t order = 0;     // when it started (the oldest sound gives way)
  };

 private:
  void start_pattern(int index);
  void play_voice(Voice& v, int32_t* acc, int n);
  void next_note(Voice& v);

  int rate_;
  Voice music_[kChannels], sfx_[kChannels];
  std::shared_ptr<const Song> song_;
  int pattern_ = 0, loop_to_ = -1;
  int64_t pattern_left_ = 0;
  uint32_t count_ = 0;
  std::vector<int32_t> quiet_;  // where a song's voice plays while a sound has its channel
};

}  // namespace carts
