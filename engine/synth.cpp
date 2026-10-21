#include "synth.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace carts {
namespace {

constexpr float kPeak = 0.25f * 32767;  // a channel at full volume: four together still fit, mostly
constexpr int kBlock = 32;              // samples between updates of pitch and volume (2 ms at 16 kHz)

// The hex digits in `text` (spaces skipped); false at anything else.
bool digits(std::string_view text, std::vector<uint8_t>& out, bool dashes = false) {
  for (const char c : text) {
    if (c == ' ' || c == '\t' || c == '\n') continue;
    if (c >= '0' && c <= '9') out.push_back(static_cast<uint8_t>(c - '0'));
    else if (c >= 'a' && c <= 'f') out.push_back(static_cast<uint8_t>(c - 'a' + 10));
    else if (c >= 'A' && c <= 'F') out.push_back(static_cast<uint8_t>(c - 'A' + 10));
    else if (dashes && c == '-') out.push_back(0xFF);
    else return false;
  }
  return true;
}

float hz(float pitch) { return 65.406f * std::exp2(pitch / 12.0f); }  // pitch 0: C2

// One period of a wave, at phase 0-1.
float wave(int instrument, float p, Synth::Voice& v, bool wrapped) {
  switch (instrument) {
    case kSquare: return p < 0.5f ? 1.0f : -1.0f;
    case kPulse: return p < 0.25f ? 1.0f : -1.0f;
    case kThinPulse: return p < 0.125f ? 1.0f : -1.0f;
    case kTriangle: return p < 0.5f ? 4 * p - 1 : 3 - 4 * p;
    case kSaw: return 2 * p - 1;
    case kSine: {
      const float x = p < 0.5f ? p : p - 0.5f;  // a parabola per half: close to a sine, no table
      const float y = 16 * x * (0.5f - x);
      return p < 0.5f ? y : -y;
    }
    case kOrgan: {
      const float q = p * 2 - std::floor(p * 2);
      const float a = p < 0.5f ? 4 * p - 1 : 3 - 4 * p, b = q < 0.5f ? 4 * q - 1 : 3 - 4 * q;
      return (a + 0.5f * b) / 1.5f;
    }
    default:  // noise: a new random level each period
      if (wrapped) {
        v.lfsr = (v.lfsr >> 1) ^ (-(v.lfsr & 1u) & 0xB400u);
        v.level = (v.lfsr & 0xFFFF) / 32767.5f - 1.0f;
      }
      return v.level;
  }
}

}  // namespace

std::string parse_sound(std::string_view text, Sound& out) {
  std::vector<uint8_t> d;
  if (!digits(text, d)) return "should be hex digits (the sound editor writes them)";
  if (d.size() < 11 || (d.size() - 6) % 5 != 0) return "should be 6 digits (speed, loop) and then 5 for each note";
  out = Sound{};
  out.speed = d[0] * 16 + d[1];
  out.loop_start = d[2] * 16 + d[3];
  out.loop_end = d[4] * 16 + d[5];
  const size_t count = (d.size() - 6) / 5;
  char why[80];
  if (count > static_cast<size_t>(kNotes)) {
    std::snprintf(why, sizeof why, "has %u notes; a sound has at most %d", static_cast<unsigned>(count), kNotes);
    return why;
  }
  if (out.speed < 1) return "has speed 0; it's 1 to 255";
  for (size_t i = 0; i < count; ++i) {
    const uint8_t* n = &d[6 + i * 5];
    Note note;
    note.pitch = static_cast<uint8_t>(n[0] * 16 + n[1]);
    note.instrument = n[2];
    note.volume = n[3];
    note.effect = n[4];
    if (note.pitch > 63 || note.instrument >= kInstruments || note.volume > 7 || note.effect >= kEffects) {
      std::snprintf(why, sizeof why, "note %u: pitch 0-3f, instrument 0-7, volume 0-7, effect 0-7", static_cast<unsigned>(i + 1));
      return why;
    }
    out.notes.push_back(note);
  }
  if (out.loops() && out.loop_end > static_cast<int>(count)) {
    std::snprintf(why, sizeof why, "loops to note %d; it has %u", out.loop_end, static_cast<unsigned>(count));
    return why;
  }
  return "";
}

std::string parse_pattern(std::string_view text, Pattern& out) {
  std::vector<uint8_t> d;
  if (!digits(text, d, true) || d.size() != 2 + 2 * kChannels)
    return "should be 2 digits of flags and then a sound per channel (\"00 08 09 -- --\")";
  out = Pattern{};
  if (d[0] == 0xFF || d[1] == 0xFF) return "should start with 2 digits of flags";
  out.flags = static_cast<uint8_t>(d[0] * 16 + d[1]);
  for (int c = 0; c < kChannels; ++c) {
    const uint8_t a = d[2 + c * 2], b = d[3 + c * 2];
    if (a == 0xFF && b == 0xFF) continue;
    const int n = a * 16 + b;
    if (a == 0xFF || b == 0xFF || n < kSoundFirst || n > kSoundLast) {
      char why[64];
      std::snprintf(why, sizeof why, "channel %d: a sound is 08-3f (hex), or -- for none", c + 1);
      return why;
    }
    out.sound[c] = n;
  }
  return "";
}

std::string build_song(int start, const TextOf& pattern, const TextOf& sound, std::shared_ptr<const Song>& out) {
  auto song = std::make_shared<Song>();
  song->start = start;
  char at[64];
  for (int i = 0; i < kPatterns; ++i) {
    const char* text = pattern(i);
    if (!text) continue;
    std::snprintf(at, sizeof at, "MUSIC[%d] ", i);
    std::string why = parse_pattern(text, song->patterns[i]);
    if (!why.empty()) return at + why;
    song->has[i] = true;
    for (int c = 0; c < kChannels; ++c) {
      const int s = song->patterns[i].sound[c];
      if (s < 0 || song->sounds[s]) continue;
      const char* st = sound(s);
      std::snprintf(at, sizeof at, "MUSIC[%d] channel %d: SOUNDS[%d] ", i, c + 1, s);
      if (!st) return std::string(at) + "isn't there";
      auto snd = std::make_shared<Sound>();
      why = parse_sound(st, *snd);
      if (!why.empty()) return at + why;
      song->sounds[s] = std::move(snd);
    }
  }
  if (start < 0 || start >= kPatterns || !song->has[start]) {
    std::snprintf(at, sizeof at, "MUSIC[%d] isn't there", start);
    return at;
  }
  out = std::move(song);
  return "";
}

Synth::Synth(int rate) : rate_(rate > 0 ? rate : 16000) {}

void Synth::sfx(std::shared_ptr<const Sound> sound) {
  if (!sound) {
    for (Voice& v : sfx_) v.sound.reset();
    return;
  }
  if (sound->notes.empty()) return;
  // A channel with nothing on it, else one the song leaves quiet, else the oldest sound's, else the last.
  auto score = [&](int c) {
    const bool music = music_[c].sound != nullptr, mine = sfx_[c].sound != nullptr;
    return !music && !mine ? 0 : !mine ? 1 : 2;
  };
  int best = kChannels - 1;
  for (int c = kChannels - 1; c >= 0; --c) {
    if (score(c) < score(best) || (score(c) == score(best) && score(c) == 2 && sfx_[c].order < sfx_[best].order)) best = c;
  }
  Voice& v = sfx_[best];
  v = Voice{};
  v.sound = std::move(sound);
  v.order = ++count_;
  v.ticks = 0;
  v.end = static_cast<int64_t>(v.sound->speed) * rate_ / kTickHz;
  v.prev = v.sound->notes[0].pitch;
}

void Synth::music(std::shared_ptr<const Song> song) {
  song_ = std::move(song);
  for (Voice& v : music_) v.sound.reset();
  loop_to_ = -1;
  if (song_) start_pattern(song_->start);
}

void Synth::start_pattern(int index) {
  if (!song_ || index < 0 || index >= kPatterns || !song_->has[index]) {
    song_.reset();
    for (Voice& v : music_) v.sound.reset();
    return;
  }
  pattern_ = index;
  const Pattern& p = song_->patterns[index];
  if (p.flags & kLoopStart) loop_to_ = index;
  int64_t longest = 0;
  for (int c = 0; c < kChannels; ++c) {
    Voice& v = music_[c];
    const float prev = v.sound ? v.prev : -1;
    v = Voice{};
    if (p.sound[c] < 0) continue;
    v.sound = song_->sounds[p.sound[c]];
    if (!v.sound || v.sound->notes.empty()) continue;
    v.end = static_cast<int64_t>(v.sound->speed) * rate_ / kTickHz;
    v.prev = prev >= 0 ? prev : v.sound->notes[0].pitch;
    longest = std::max<int64_t>(longest, static_cast<int64_t>(v.sound->notes.size()) * v.sound->speed);
  }
  pattern_left_ = longest * rate_ / kTickHz;
  if (pattern_left_ <= 0) pattern_left_ = rate_ / 4;  // an empty pattern: a short rest
}

bool Synth::busy() const {
  if (song_) return true;
  for (const Voice& v : sfx_) if (v.sound) return true;
  return false;
}

void Synth::next_note(Voice& v) {
  const Sound& s = *v.sound;
  v.prev = s.notes[static_cast<size_t>(v.note)].pitch;
  v.ticks += s.speed;
  v.start = v.end;
  int next = v.note + 1;
  if (s.loops() && next == s.loop_end) next = s.loop_start;
  if (next >= static_cast<int>(s.notes.size())) {
    v.sound.reset();
    return;
  }
  v.note = next;
  v.end = (v.ticks + s.speed) * rate_ / kTickHz;
}

void Synth::play_voice(Voice& v, int32_t* acc, int n) {
  int done = 0;
  while (done < n && v.sound) {
    if (v.pos >= v.end) {
      next_note(v);
      continue;
    }
    const Sound& s = *v.sound;
    const Note& note = s.notes[static_cast<size_t>(v.note)];
    const int len = static_cast<int>(std::min<int64_t>({n - done, kBlock, v.end - v.pos}));
    // Pitch and volume for this block, from how far into the note it is.
    const float t = static_cast<float>(v.pos - v.start) / static_cast<float>(std::max<int64_t>(1, v.end - v.start));
    const float secs = static_cast<float>(v.pos - v.start) / rate_;
    float pitch = note.pitch, vol = note.volume / 7.0f, f;
    switch (note.effect) {
      case kSlide: pitch = v.prev + (note.pitch - v.prev) * t; break;
      case kVibrato: pitch += 0.5f * std::sin(6.2831853f * 7.0f * secs); break;
      case kFadeIn: vol *= t; break;
      case kFadeOut: vol *= 1 - t; break;
      case kArpFast:
      case kArpSlow: {
        const int group = v.note & ~3, size = std::min<int>(4, static_cast<int>(s.notes.size()) - group);
        const int every = note.effect == kArpFast ? 2 : 4;  // ticks per step
        const int step = static_cast<int>(secs * kTickHz) / every % std::max(1, size);
        pitch = s.notes[static_cast<size_t>(group + step)].pitch;
        break;
      }
      default: break;
    }
    f = hz(pitch);
    if (note.effect == kDrop) f *= 1 - t;
    if (note.instrument == kNoise) f *= 8;  // noise's pitch is how often it changes: higher than a tone's
    const float step = std::max(0.0f, f) / rate_;
    const float target = note.volume ? vol * kPeak : 0.0f;
    for (int k = 0; k < len; ++k) {
      v.vol += (target - v.vol) * 0.06f;  // about 1 ms to a new volume: no clicks
      const float before = v.phase;
      v.phase += step;
      if (v.phase >= 1.0f) v.phase -= std::floor(v.phase);
      acc[done + k] += static_cast<int32_t>(wave(note.instrument, v.phase, v, v.phase < before) * v.vol);
    }
    v.pos += len;
    done += len;
    if (v.pos >= v.end) next_note(v);  // so a sound that's done stops counting as busy at once
  }
}

void Synth::render(int32_t* acc, int n) {
  int done = 0;
  while (done < n) {
    int len = n - done;
    if (song_) len = static_cast<int>(std::min<int64_t>(len, pattern_left_));
    for (int c = 0; c < kChannels; ++c) {
      if (sfx_[c].sound) {
        play_voice(sfx_[c], acc + done, len);
        // the song's voice keeps time underneath, unheard
        if (music_[c].sound) {
          quiet_.assign(static_cast<size_t>(len), 0);
          play_voice(music_[c], quiet_.data(), len);
        }
      } else if (music_[c].sound) {
        play_voice(music_[c], acc + done, len);
      }
    }
    done += len;
    if (song_ && (pattern_left_ -= len) <= 0) {
      const Pattern& p = song_->patterns[pattern_];
      if (p.flags & kStopAfter) start_pattern(-1);
      else if ((p.flags & kLoopBack) && loop_to_ >= 0) start_pattern(loop_to_);
      else start_pattern(pattern_ + 1);
    }
  }
}

}  // namespace carts
