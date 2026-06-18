#pragma once
// audio-io.h: unified audio read/write for WAV, MP3, Opus and FLAC.
//
// Reads any WAV (PCM16/24/float32, mono/stereo, any rate), MP3 (minimp3),
// FLAC (libFLAC when ACE_HAVE_FLAC) or Ogg Opus (libopusfile when
// ACE_HAVE_OPUS). Writes WAV (16/24/32-bit), MP3 (libmp3lame when
// ACE_HAVE_MP3), Ogg Opus (.opus, libopusenc when ACE_HAVE_OPUS) or native
// FLAC (.flac, libFLAC when ACE_HAVE_FLAC).
//
// All public functions work on planar stereo float: [L: T samples][R: T].
// Part of acestep.cpp. MIT license.

#include "task-types.h"

#include <cstdint>
#include <string>

// Container/format descriptor used by audio_encode/audio_write and the
// request handlers. `wav_subformat` is only meaningful when codec == WAV.
enum AudioCodec {
	CODEC_WAV,   // classic RIFF, 16/24/32-bit PCM
	CODEC_MP3,   // MPEG1 Layer III (.mp3)
	CODEC_OPUS,  // Ogg Opus (.opus)
	CODEC_FLAC,  // native FLAC (.flac)
};

enum WavFormat {
	WAV_S16,  // 16-bit signed integer PCM (classic RIFF, default)
	WAV_S24,  // 24-bit signed integer PCM (classic RIFF)
	WAV_F32,  // 32-bit IEEE 754 float (classic RIFF, fmt_tag=3)
};

struct AudioFormat {
	AudioCodec codec;
	WavFormat  wav_subformat;
};

// Parse a request `output_format` string into an AudioFormat descriptor.
// Returns false on unknown strings or codecs that are not compiled in
// (callers should surface a 400/error with audio_available_formats_text()).
// Accepts: wav, wav16, wav24, wav32 (always), mp3 (ACE_HAVE_MP3),
// opus (ACE_HAVE_OPUS), flac (ACE_HAVE_FLAC).
bool audio_parse_format(const char * s, AudioFormat & out);

// True if the codec was compiled into this build.
bool audio_codec_available(AudioCodec codec);

// File extension (with leading dot) for the format's container.
const char * audio_format_extension(const AudioFormat & fmt);

// HTTP MIME type for the format's container.
const char * audio_format_mime(const AudioFormat & fmt);

// Human-readable list of compiled-in format names (e.g. "wav16, wav24,
// wav32, mp3, opus"). Used in error messages and CLI help.
std::string audio_available_formats_text();

// Encode planar stereo float [L:T][R:T] -> container bytes. The encoder
// assumes already-normalized float input; it does NOT normalize.
// quality/bitrate: -1 = library default, otherwise codec-specific
// (see docs/ARCHITECTURE.md). `cancel` is polled between encoder chunks
// and aborts early when it returns true. Returns empty string on failure.
std::string audio_encode(const float * audio, int T, int sr,
                         const AudioFormat & fmt, int quality, int bitrate,
                         bool (*cancel)(void *) = nullptr, void * cancel_data = nullptr);

// Encode and write to disk. Copies the const input internally and
// normalizes the copy unless fmt is {CODEC_WAV, WAV_F32}. Returns false
// on failure (file open or encoder error).
bool audio_write(const char * path, const float * audio, int T, int sr,
                 const AudioFormat & fmt, int quality, int bitrate,
                 int peak_clip = 10);

// WAV-only convenience: encode planar stereo to a RIFF container in memory.
// Does NOT normalize. NaN/Inf coerced to zero; S16/S24 clamp to [-1, +1].
std::string audio_encode_wav(const float * audio, int T, int sr, WavFormat fmt = WAV_S16);

// --- Decode (input) -------------------------------------------------------

// Decode an audio buffer (auto-detected from magic bytes) into planar
// stereo float [L:T][R:T] at the source sample rate. Caller frees.
float * audio_read_buf(const uint8_t * data, size_t size, int * T_out, int * sr_out);

// Decode an audio buffer and resample to 48000 Hz stereo. Caller frees.
float * audio_read_48k_buf(const uint8_t * data, size_t size, int * T_out);

// File-based wrappers around audio_read_buf / audio_read_48k_buf.
float * audio_read(const char * path, int * T_out, int * sr_out);
float * audio_read_48k(const char * path, int * T_out);

// --- Shared helpers -------------------------------------------------------

// Percentile normalization. peak_clip trades loudness for clipping:
//   0   = peak normalization (no clipping)
//   10  = default (clips top 0.001%)
//   999 = max (clips top 0.1%)
// n_total is the total sample count across both channels.
void audio_normalize(float * audio, int n_total, int peak_clip = 10);

// Convert planar [L:T][R:T] to interleaved [L0,R0,L1,R1,...].
// Returns malloc'd buffer of 2*T floats, caller frees. NULL on OOM.
float * audio_planar_to_interleaved(const float * planar, int T);
