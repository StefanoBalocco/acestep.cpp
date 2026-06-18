// audio-io.cpp: WAV / MP3 / Opus / FLAC encode and decode implementation.
//
// Implementation of the public API declared in audio-io.h. Codec backends
// are gated by ACE_HAVE_MP3 (LAME + vendored minimp3), ACE_HAVE_FLAC
// (libFLAC) and ACE_HAVE_OPUS (libopusenc + libopusfile). The WAV path is
// always available.
//
// All public functions work on planar stereo float [L: T][R: T].
// Part of acestep.cpp. MIT license.

#include "audio-io.h"
#include "audio-resample.h"
#include "task-types.h"
#include "wav.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

// ============================================================================
// Format descriptors
// ============================================================================

bool audio_codec_available(AudioCodec codec) {
	switch (codec) {
		case CODEC_WAV:
			return true;
		case CODEC_MP3:
#ifdef ACE_HAVE_MP3
			return true;
#else
			return false;
#endif
		case CODEC_OPUS:
#ifdef ACE_HAVE_OPUS
			return true;
#else
			return false;
#endif
		case CODEC_FLAC:
#ifdef ACE_HAVE_FLAC
			return true;
#else
			return false;
#endif
	}
	return false;
}

bool audio_parse_format(const char * s, AudioFormat & out) {
	if (!s) {
		return false;
	}
	if (!strcmp(s, OUTPUT_FORMAT_WAV) || !strcmp(s, OUTPUT_FORMAT_WAV16)) {
		out.codec        = CODEC_WAV;
		out.wav_subformat = WAV_S16;
		return true;
	}
	if (!strcmp(s, OUTPUT_FORMAT_WAV24)) {
		out.codec        = CODEC_WAV;
		out.wav_subformat = WAV_S24;
		return true;
	}
	if (!strcmp(s, OUTPUT_FORMAT_WAV32)) {
		out.codec        = CODEC_WAV;
		out.wav_subformat = WAV_F32;
		return true;
	}
	if (!strcmp(s, OUTPUT_FORMAT_MP3)) {
		if (!audio_codec_available(CODEC_MP3)) {
			return false;
		}
		out.codec        = CODEC_MP3;
		out.wav_subformat = WAV_S16;
		return true;
	}
	if (!strcmp(s, OUTPUT_FORMAT_OPUS)) {
		if (!audio_codec_available(CODEC_OPUS)) {
			return false;
		}
		out.codec        = CODEC_OPUS;
		out.wav_subformat = WAV_S16;
		return true;
	}
	if (!strcmp(s, OUTPUT_FORMAT_FLAC)) {
		if (!audio_codec_available(CODEC_FLAC)) {
			return false;
		}
		out.codec        = CODEC_FLAC;
		out.wav_subformat = WAV_S16;
		return true;
	}
	return false;
}

const char * audio_format_extension(const AudioFormat & fmt) {
	switch (fmt.codec) {
		case CODEC_WAV:
			return ".wav";
		case CODEC_MP3:
			return ".mp3";
		case CODEC_OPUS:
			return ".opus";
		case CODEC_FLAC:
			return ".flac";
	}
	return ".bin";
}

const char * audio_format_mime(const AudioFormat & fmt) {
	switch (fmt.codec) {
		case CODEC_WAV:
			return "audio/wav";
		case CODEC_MP3:
			return "audio/mpeg";
		case CODEC_OPUS:
			return "audio/ogg";
		case CODEC_FLAC:
			return "audio/flac";
	}
	return "application/octet-stream";
}

std::string audio_available_formats_text() {
	std::string out = "wav, wav16, wav24, wav32";
	if (audio_codec_available(CODEC_MP3)) {
		out += ", mp3";
	}
	if (audio_codec_available(CODEC_OPUS)) {
		out += ", opus";
	}
	if (audio_codec_available(CODEC_FLAC)) {
		out += ", flac";
	}
	return out;
}

// ============================================================================
// Shared helpers
// ============================================================================

namespace {

uint8_t * audio_io_load_file(const char * path, size_t * size_out) {
	*size_out = 0;
	FILE * fp = fopen(path, "rb");
	if (!fp) {
		fprintf(stderr, "[Audio] Cannot open %s\n", path);
		return nullptr;
	}
	fseek(fp, 0, SEEK_END);
	long fsize = ftell(fp);
	fseek(fp, 0, SEEK_SET);

	uint8_t * buf = (uint8_t *) malloc((size_t) fsize);
	if (!buf) {
		fclose(fp);
		return nullptr;
	}
	size_t nr = fread(buf, 1, (size_t) fsize, fp);
	fclose(fp);
	if (nr != (size_t) fsize) {
		free(buf);
		return nullptr;
	}

	*size_out = (size_t) fsize;
	return buf;
}

float wav_clamp1(float x) {
	return x < -1.0f ? -1.0f : (x > 1.0f ? 1.0f : x);
}

float wav_sanitize(float x) {
	return std::isfinite(x) ? x : 0.0f;
}

void wav_write_u16le(char *& p, uint16_t x) {
	*p++ = (char) (x & 0xff);
	*p++ = (char) ((x >> 8) & 0xff);
}

void wav_write_u24le(char *& p, uint32_t x) {
	*p++ = (char) (x & 0xff);
	*p++ = (char) ((x >> 8) & 0xff);
	*p++ = (char) ((x >> 16) & 0xff);
}

void wav_write_u32le(char *& p, uint32_t x) {
	*p++ = (char) (x & 0xff);
	*p++ = (char) ((x >> 8) & 0xff);
	*p++ = (char) ((x >> 16) & 0xff);
	*p++ = (char) ((x >> 24) & 0xff);
}

// Classic RIFF header: fmt_tag 1 (PCM int) or 3 (IEEE float), 16-byte fmt chunk
void wav_write_header_basic(char *& p, int T_audio, int sr, int n_channels, int bits, uint16_t fmt_tag) {
	uint32_t bytes_per_sample = (uint32_t) bits / 8;
	uint32_t byte_rate        = (uint32_t) sr * (uint32_t) n_channels * bytes_per_sample;
	uint16_t block_align      = (uint16_t) (n_channels * (int) bytes_per_sample);
	uint32_t data_size        = (uint32_t) T_audio * (uint32_t) n_channels * bytes_per_sample;
	uint32_t file_size        = 36u + data_size;

	memcpy(p, "RIFF", 4);
	p += 4;
	wav_write_u32le(p, file_size);
	memcpy(p, "WAVE", 4);
	p += 4;

	memcpy(p, "fmt ", 4);
	p += 4;
	wav_write_u32le(p, 16);
	wav_write_u16le(p, fmt_tag);
	wav_write_u16le(p, (uint16_t) n_channels);
	wav_write_u32le(p, (uint32_t) sr);
	wav_write_u32le(p, byte_rate);
	wav_write_u16le(p, block_align);
	wav_write_u16le(p, (uint16_t) bits);

	memcpy(p, "data", 4);
	p += 4;
	wav_write_u32le(p, data_size);
}

}  // namespace

// ============================================================================
// Normalization + planar/interleaved (public API)
// ============================================================================

void audio_normalize(float * audio, int n_total, int peak_clip) {
	if (n_total <= 0) {
		return;
	}

	// clamp to valid range
	if (peak_clip < 0) {
		peak_clip = 0;
	}
	if (peak_clip > 999) {
		peak_clip = 999;
	}

	// sanitize non-finite samples and collect absolute values
	std::vector<float> absvals((size_t) n_total);
	float pre_peak = 0.0f;
	for (int i = 0; i < n_total; i++) {
		audio[i] = wav_sanitize(audio[i]);
		absvals[i] = audio[i] < 0.0f ? -audio[i] : audio[i];
		if (absvals[i] > pre_peak) {
			pre_peak = absvals[i];
		}
	}

	// partial sort to find the target percentile
	double pct = 1.0 - (double) peak_clip / 1000000.0;
	size_t idx = (size_t) ((double) (n_total - 1) * pct);
	std::nth_element(absvals.begin(), absvals.begin() + (long) idx, absvals.end());
	float ref = absvals[idx];

	if (ref < 1e-6f) {
		fprintf(stderr, "[Audio-Norm] peak %.6f, ref %.6f, gain %.3f, clipped %d/%d (%.6f%%), peak_clip %d\n",
		        (double) pre_peak, (double) ref, 1.0,
		        0, n_total, 0.0, peak_clip);
		return;
	}

	// scale so the target percentile hits 1.0, hard clip the rest
	float gain = 1.0f / ref;
	int n_clipped = 0;
	for (int i = 0; i < n_total; i++) {
		float v = audio[i] * gain;
		if (v > 1.0f || v < -1.0f) {
			n_clipped++;
		}
		if (v > 1.0f) {
			v = 1.0f;
		} else if (v < -1.0f) {
			v = -1.0f;
		}
		audio[i] = v;
	}

	fprintf(stderr, "[Audio-Norm] peak %.6f, ref %.6f, gain %.3f, clipped %d/%d (%.6f%%), peak_clip %d\n",
	        (double) pre_peak, (double) ref, (double) gain,
	        n_clipped, n_total,
	        (n_total > 0) ? (100.0 * n_clipped / n_total) : 0.0,
	        peak_clip);
}

float * audio_planar_to_interleaved(const float * planar, int T) {
	float * out = (float *) malloc((size_t) T * 2 * sizeof(float));
	if (!out) {
		fprintf(stderr, "[Audio] OOM allocating interleaved buffer for %d samples\n", T);
		return nullptr;
	}
	for (int t = 0; t < T; t++) {
		out[t * 2 + 0] = planar[t];
		out[t * 2 + 1] = planar[T + t];
	}
	return out;
}

// ============================================================================
// WAV encode (always available)
// ============================================================================

static std::string audio_encode_wav_s16(const float * audio, int T_audio, int sr) {
	int n_channels = 2;
	int data_size  = T_audio * n_channels * 2;

	std::string out;
	out.resize(44 + (size_t) data_size);
	char * p = &out[0];

	wav_write_header_basic(p, T_audio, sr, n_channels, 16, 1);

	const float * L = audio;
	const float * R = audio + T_audio;

	for (int t = 0; t < T_audio; t++) {
		int16_t l = (int16_t) (wav_clamp1(wav_sanitize(L[t])) * 32767.0f);
		int16_t r = (int16_t) (wav_clamp1(wav_sanitize(R[t])) * 32767.0f);
		wav_write_u16le(p, (uint16_t) l);
		wav_write_u16le(p, (uint16_t) r);
	}

	return out;
}

static std::string audio_encode_wav_s24(const float * audio, int T_audio, int sr) {
	int n_channels = 2;
	int data_size  = T_audio * n_channels * 3;

	std::string out;
	out.resize(44 + (size_t) data_size);
	char * p = &out[0];

	wav_write_header_basic(p, T_audio, sr, n_channels, 24, 1);

	const float * L = audio;
	const float * R = audio + T_audio;

	for (int t = 0; t < T_audio; t++) {
		int32_t l = (int32_t) (wav_clamp1(wav_sanitize(L[t])) * 8388607.0f);
		int32_t r = (int32_t) (wav_clamp1(wav_sanitize(R[t])) * 8388607.0f);
		wav_write_u24le(p, (uint32_t) l);
		wav_write_u24le(p, (uint32_t) r);
	}

	return out;
}

static std::string audio_encode_wav_f32(const float * audio, int T_audio, int sr) {
	int n_channels = 2;
	int data_size  = T_audio * n_channels * 4;

	std::string out;
	out.resize(44 + (size_t) data_size);
	char * p = &out[0];

	wav_write_header_basic(p, T_audio, sr, n_channels, 32, 3);

	const float * L = audio;
	const float * R = audio + T_audio;

	for (int t = 0; t < T_audio; t++) {
		float    lf = wav_sanitize(L[t]);
		float    rf = wav_sanitize(R[t]);
		uint32_t lu, ru;
		memcpy(&lu, &lf, 4);
		memcpy(&ru, &rf, 4);
		wav_write_u32le(p, lu);
		wav_write_u32le(p, ru);
	}

	return out;
}

std::string audio_encode_wav(const float * audio, int T_audio, int sr, WavFormat fmt) {
	switch (fmt) {
		case WAV_S16:
			return audio_encode_wav_s16(audio, T_audio, sr);
		case WAV_S24:
			return audio_encode_wav_s24(audio, T_audio, sr);
		case WAV_F32:
			return audio_encode_wav_f32(audio, T_audio, sr);
	}
	return audio_encode_wav_s16(audio, T_audio, sr);
}

// ============================================================================
// MP3 encode (libmp3lame) + decode (vendored minimp3)
// ============================================================================

#ifdef ACE_HAVE_MP3
// minimp3 implementation lives only here, only when MP3 is enabled.
#    define MINIMP3_IMPLEMENTATION
#    ifdef __GNUC__
#        pragma GCC diagnostic push
#        pragma GCC diagnostic ignored "-Wconversion"
#        pragma GCC diagnostic ignored "-Wsign-conversion"
#        pragma GCC diagnostic ignored "-Wshadow"
#    endif
#    include "vendor/minimp3/minimp3.h"
#    ifdef __GNUC__
#        pragma GCC diagnostic pop
#    endif
#    include <lame/lame.h>

static std::string audio_encode_mp3(const float * audio,
                                    int           T_audio,
                                    int           sr,
                                    int           quality,
                                    int           bitrate,
                                    bool (*cancel)(void *),
                                    void *        cancel_data) {
	// resample to a valid MPEG1 rate if needed
	const float * enc_audio = audio;
	int           enc_T     = T_audio;
	int           enc_sr    = sr;
	float *       resampled = nullptr;

	if (sr != 32000 && sr != 44100 && sr != 48000) {
		int T_rs  = 0;
		resampled = audio_resample(audio, T_audio, sr, 44100, 2, &T_rs);
		if (!resampled) {
			fprintf(stderr, "[Audio-Resample] Resample failed\n");
			return "";
		}
		fprintf(stderr, "[Audio-Resample] %d Hz -> 44100 Hz (%d -> %d samples)\n", sr, T_audio, T_rs);
		enc_audio = resampled;
		enc_T     = T_rs;
		enc_sr    = 44100;
	}

	float duration = (float) enc_T / (float) enc_sr;

	lame_global_flags * gfp = lame_init();
	if (!gfp) {
		free(resampled);
		fprintf(stderr, "[MP3] lame_init failed\n");
		return "";
	}

	lame_set_num_channels(gfp, 2);
	lame_set_in_samplerate(gfp, enc_sr);
	lame_set_quality(gfp, 0);  // 0 = best algorithm quality (-q0), not VBR quality

	if (bitrate > 0) {
		lame_set_VBR(gfp, vbr_abr);
		lame_set_VBR_mean_bitrate_kbps(gfp, bitrate);
	} else {
		int q = 2;
		if (quality >= 0) {
			q = quality;
			if (q < 0) {
				q = 0;
			}
			if (q > 9) {
				q = 9;
			}
		}
		lame_set_VBR(gfp, vbr_default);
		lame_set_VBR_quality(gfp, (float) q);
	}

	if (lame_init_params(gfp) < 0) {
		fprintf(stderr, "[MP3] lame_init_params failed\n");
		lame_close(gfp);
		free(resampled);
		return "";
	}

	fprintf(stderr, "[MP3] Encoding %.1fs @ %d Hz stereo\n", duration, enc_sr);

	std::string out;

	// LAME's mp3 buffer must be at least 1.25 * samples + 7200 bytes per chunk.
	const int chunk = 1152 * 32;  // ~32 frames per chunk
	std::vector<unsigned char> mp3buf((size_t) (1.25 * (chunk + 1) + 7200));

	const float * L = enc_audio;
	const float * R = enc_audio + enc_T;
	for (int off = 0; off < enc_T; off += chunk) {
		if (cancel && cancel(cancel_data)) {
			break;
		}
		int len = (off + chunk <= enc_T) ? chunk : (enc_T - off);
		// lame_encode_buffer_ieee_float takes planar L/R pointers.
		int n = lame_encode_buffer_ieee_float(gfp, L + off, R + off, len, mp3buf.data(), (int) mp3buf.size());
		if (n < 0) {
			fprintf(stderr, "[MP3] encode error (%d) at offset %d\n", n, off);
			lame_close(gfp);
			free(resampled);
			return "";
		}
		out.append((const char *) mp3buf.data(), (size_t) n);
	}

	int n = lame_encode_flush(gfp, mp3buf.data(), (int) mp3buf.size());
	if (n > 0) {
		out.append((const char *) mp3buf.data(), (size_t) n);
	}

	lame_close(gfp);
	free(resampled);

	if (cancel && cancel(cancel_data)) {
		fprintf(stderr, "[MP3] Cancelled\n");
		return "";
	}

	return out;
}

// Decode MP3 from memory buffer. Returns planar stereo float [L:T][R:T].
static float * audio_io_read_mp3_buf(const uint8_t * data, size_t size, int * T_out, int * sr_out) {
	*T_out  = 0;
	*sr_out = 0;

	mp3dec_t dec;
	mp3dec_init(&dec);

	short * pcm_buf   = nullptr;
	int     pcm_cap   = 0;
	int     pcm_count = 0;
	int     out_sr    = 0;
	int     out_nch   = 0;

	size_t offset = 0;
	while (offset < size) {
		mp3dec_frame_info_t info;
		short               pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
		int samples = mp3dec_decode_frame(&dec, data + offset, (int) (size - offset), pcm, &info);
		if (info.frame_bytes == 0) {
			break;
		}
		offset += (size_t) info.frame_bytes;

		if (samples > 0) {
			if (out_sr == 0) {
				out_sr  = info.hz;
				out_nch = info.channels;
			}
			int need = pcm_count + samples * out_nch;
			if (need > pcm_cap) {
				pcm_cap         = (need < 65536) ? 65536 : need * 2;
				short * new_buf = (short *) realloc(pcm_buf, (size_t) pcm_cap * sizeof(short));
				if (!new_buf) {
					fprintf(stderr, "[Audio] OOM while decoding MP3 frames\n");
					free(pcm_buf);
					return nullptr;
				}
				pcm_buf = new_buf;
			}
			memcpy(pcm_buf + pcm_count, pcm, (size_t) samples * (size_t) out_nch * sizeof(short));
			pcm_count += samples * out_nch;
		}
	}

	if (pcm_count == 0 || out_sr == 0) {
		fprintf(stderr, "[Audio] No audio decoded from buffer\n");
		free(pcm_buf);
		return nullptr;
	}

	int T = pcm_count / out_nch;

	float * planar = (float *) malloc((size_t) T * 2 * sizeof(float));
	if (!planar) {
		free(pcm_buf);
		return nullptr;
	}
	for (int t = 0; t < T; t++) {
		float l       = (float) pcm_buf[t * out_nch + 0] / 32768.0f;
		float r       = (out_nch >= 2) ? (float) pcm_buf[t * out_nch + 1] / 32768.0f : l;
		planar[t]     = l;
		planar[T + t] = r;
	}
	free(pcm_buf);

	*T_out  = T;
	*sr_out = out_sr;
	fprintf(stderr, "[MP3] Read buffer: %d samples, %d Hz, %d ch\n", T, out_sr, out_nch);
	return planar;
}
#endif  // ACE_HAVE_MP3

// ============================================================================
// FLAC encode/decode (libFLAC)
// ============================================================================

#ifdef ACE_HAVE_FLAC
#    include <FLAC/stream_decoder.h>
#    include <FLAC/stream_encoder.h>

static std::string audio_encode_flac(const float * audio,
                                     int           T_audio,
                                     int           sr,
                                     int           quality,
                                     int           bitrate,
                                     bool (*cancel)(void *),
                                     void *        cancel_data) {
	if (bitrate > 0) {
		fprintf(stderr, "[FLAC] bitrate is ignored (lossless codec)\n");
	}

	std::string out;

	FLAC__StreamEncoder * enc = FLAC__stream_encoder_new();
	if (!enc) {
		fprintf(stderr, "[FLAC] FLAC__stream_encoder_new failed\n");
		return "";
	}

	FLAC__stream_encoder_set_channels(enc, 2);
	FLAC__stream_encoder_set_bits_per_sample(enc, 24);
	FLAC__stream_encoder_set_sample_rate(enc, (uint32_t) sr);

	if (quality >= 0) {
		int lvl = quality;
		if (lvl < 0) {
			lvl = 0;
		}
		if (lvl > 8) {
			lvl = 8;
		}
		FLAC__stream_encoder_set_compression_level(enc, (uint32_t) lvl);
	}

	auto write_cb = [](const FLAC__StreamEncoder * encoder, const FLAC__byte buffer[], size_t bytes,
	                   uint32_t samples, uint32_t current_frame, void * client_data) -> FLAC__StreamEncoderWriteStatus {
		(void) encoder;
		(void) samples;
		(void) current_frame;
		auto * out_ptr = (std::string *) client_data;
		out_ptr->append((const char *) buffer, bytes);
		return FLAC__STREAM_ENCODER_WRITE_STATUS_OK;
	};

	FLAC__stream_encoder_init_stream(enc, write_cb, nullptr, nullptr, nullptr, &out);

	// Convert planar float -> interleaved int32 scaled to 24-bit.
	const int       chunk = 4096;
	std::vector<FLAC__int32> buf((size_t) chunk * 2);
	const float * L = audio;
	const float * R = audio + T_audio;

	for (int off = 0; off < T_audio; off += chunk) {
		if (cancel && cancel(cancel_data)) {
			break;
		}
		int len = (off + chunk <= T_audio) ? chunk : (T_audio - off);
		for (int i = 0; i < len; i++) {
			float lf = wav_clamp1(wav_sanitize(L[off + i]));
			float rf = wav_clamp1(wav_sanitize(R[off + i]));
			buf[i * 2 + 0] = (FLAC__int32) (lf * 8388607.0f);
			buf[i * 2 + 1] = (FLAC__int32) (rf * 8388607.0f);
		}
		if (!FLAC__stream_encoder_process_interleaved(enc, buf.data(), (uint32_t) len)) {
			fprintf(stderr, "[FLAC] process_interleaved failed\n");
			FLAC__stream_encoder_delete(enc);
			return "";
		}
	}

	FLAC__stream_encoder_finish(enc);
	FLAC__stream_encoder_delete(enc);

	if (cancel && cancel(cancel_data)) {
		fprintf(stderr, "[FLAC] Cancelled\n");
		return "";
	}

	return out;
}

// State carried through the FLAC stream decoder callbacks.
struct FlacDecodeState {
	const uint8_t *            data     = nullptr;
	size_t                     size     = 0;
	size_t                     pos      = 0;
	std::vector<FLAC__int32>   pcm;  // interleaved stereo int32
	uint32_t                   sample_rate = 0;
	unsigned                   channels    = 0;
	unsigned                   bps         = 0;
	bool                       ok          = true;
};

static FLAC__StreamDecoderReadStatus flac_read_cb(const FLAC__StreamDecoder * decoder, FLAC__byte buffer[],
                                                  size_t * bytes, void * client_data) {
	(void) decoder;
	auto * st = (FlacDecodeState *) client_data;
	if (*bytes == 0) {
		return FLAC__STREAM_DECODER_READ_STATUS_ABORT;
	}
	size_t avail = (st->pos < st->size) ? (st->size - st->pos) : 0;
	size_t n = (avail < *bytes) ? avail : *bytes;
	if (n == 0) {
		*bytes = 0;
		return FLAC__STREAM_DECODER_READ_STATUS_END_OF_STREAM;
	}
	memcpy(buffer, st->data + st->pos, n);
	st->pos += n;
	*bytes = n;
	return FLAC__STREAM_DECODER_READ_STATUS_CONTINUE;
}

static FLAC__StreamDecoderSeekStatus flac_seek_cb(const FLAC__StreamDecoder * decoder, FLAC__uint64 absolute_byte_offset,
                                                  void * client_data) {
	(void) decoder;
	auto * st = (FlacDecodeState *) client_data;
	st->pos = (size_t) absolute_byte_offset;
	return FLAC__STREAM_DECODER_SEEK_STATUS_OK;
}

static FLAC__StreamDecoderTellStatus flac_tell_cb(const FLAC__StreamDecoder * decoder, FLAC__uint64 * absolute_byte_offset,
                                                  void * client_data) {
	(void) decoder;
	auto * st = (FlacDecodeState *) client_data;
	*absolute_byte_offset = (FLAC__uint64) st->pos;
	return FLAC__STREAM_DECODER_TELL_STATUS_OK;
}

static FLAC__StreamDecoderLengthStatus flac_length_cb(const FLAC__StreamDecoder * decoder, FLAC__uint64 * stream_length,
                                                      void * client_data) {
	(void) decoder;
	auto * st = (FlacDecodeState *) client_data;
	*stream_length = (FLAC__uint64) st->size;
	return FLAC__STREAM_DECODER_LENGTH_STATUS_OK;
}

static FLAC__bool flac_eof_cb(const FLAC__StreamDecoder * decoder, void * client_data) {
	(void) decoder;
	auto * st = (FlacDecodeState *) client_data;
	return st->pos >= st->size;
}

static FLAC__StreamDecoderWriteStatus flac_write_cb(const FLAC__StreamDecoder * decoder, const FLAC__Frame * frame,
                                                    const FLAC__int32 * const buffer[], void * client_data) {
	(void) decoder;
	auto * st = (FlacDecodeState *) client_data;
	if (frame->header.channels < 1u) {
		st->ok = false;
		return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
	}
	unsigned nch = frame->header.channels;
	if (st->channels == 0) {
		st->channels = nch;
	}
	uint32_t bs = frame->header.blocksize;
	for (uint32_t i = 0; i < bs; i++) {
		st->pcm.push_back(buffer[0][i]);
		if (nch >= 2) {
			st->pcm.push_back(buffer[1][i]);
		} else {
			st->pcm.push_back(buffer[0][i]);
		}
	}
	return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
}

static void flac_meta_cb(const FLAC__StreamDecoder * decoder, const FLAC__StreamMetadata * metadata, void * client_data) {
	(void) decoder;
	auto * st = (FlacDecodeState *) client_data;
	if (metadata->type == FLAC__METADATA_TYPE_STREAMINFO) {
		st->sample_rate = metadata->data.stream_info.sample_rate;
		st->channels    = metadata->data.stream_info.channels;
		st->bps         = metadata->data.stream_info.bits_per_sample;
	}
}

static void flac_error_cb(const FLAC__StreamDecoder * decoder, FLAC__StreamDecoderErrorStatus status, void * client_data) {
	(void) decoder;
	(void) client_data;
	fprintf(stderr, "[FLAC] decode error: %s\n", FLAC__StreamDecoderErrorStatusString[status]);
}

// Decode native FLAC from memory buffer. Returns planar stereo float.
static float * audio_io_read_flac_buf(const uint8_t * data, size_t size, int * T_out, int * sr_out) {
	*T_out  = 0;
	*sr_out = 0;

	FlacDecodeState st;
	st.data = data;
	st.size = size;

	FLAC__StreamDecoder * dec = FLAC__stream_decoder_new();
	if (!dec) {
		fprintf(stderr, "[FLAC] FLAC__stream_decoder_new failed\n");
		return nullptr;
	}

	FLAC__StreamDecoderInitStatus rc =
	    FLAC__stream_decoder_init_stream(dec, flac_read_cb, flac_seek_cb, flac_tell_cb, flac_length_cb, flac_eof_cb,
	                                    flac_write_cb, flac_meta_cb, flac_error_cb, &st);
	if (rc != FLAC__STREAM_DECODER_INIT_STATUS_OK) {
		fprintf(stderr, "[FLAC] init_stream failed: %s\n", FLAC__StreamDecoderInitStatusString[rc]);
		FLAC__stream_decoder_delete(dec);
		return nullptr;
	}

	if (!FLAC__stream_decoder_process_until_end_of_stream(dec)) {
		fprintf(stderr, "[FLAC] process_until_end_of_stream failed\n");
		FLAC__stream_decoder_delete(dec);
		return nullptr;
	}
	FLAC__stream_decoder_delete(dec);

	if (!st.ok || st.pcm.empty() || st.sample_rate == 0) {
		fprintf(stderr, "[FLAC] No audio decoded\n");
		return nullptr;
	}

	if (0u != (st.pcm.size() % 2)) {
		fprintf(stderr, "[FLAC] Internal error: pcm buffer not stereo-aligned\n");
		return nullptr;
	}

	int T = (int) (st.pcm.size() / 2);

	float * planar = (float *) malloc((size_t) T * 2 * sizeof(float));
	if (!planar) {
		return nullptr;
	}

	float scale = 1.0f / 8388607.0f;  // default 24-bit
	if (1 == st.bps) {
		scale = 1.0f;
	} else if (st.bps > 1 && st.bps <= 32) {
		int64_t max_val = ((int64_t) 1 << (st.bps - 1)) - 1;
		scale = 1.0f / (float) max_val;
	}

	for (int t = 0; t < T; t++) {
		planar[t]     = (float) st.pcm[t * 2 + 0] * scale;
		planar[T + t] = (float) st.pcm[t * 2 + 1] * scale;
	}

	*T_out  = T;
	*sr_out = (int) st.sample_rate;
	fprintf(stderr, "[FLAC] Read buffer: %d samples, %u Hz, %u bps\n", T, st.sample_rate, st.bps);
	return planar;
}
#endif  // ACE_HAVE_FLAC

// ============================================================================
// Opus encode/decode (libopusenc + libopusfile)
// ============================================================================

#ifdef ACE_HAVE_OPUS
#    include <opus/opus.h>
#    include <opus/opusenc.h>
#    include <opus/opusfile.h>

static std::string audio_encode_opus(const float * audio,
                                     int           T_audio,
                                     int           sr,
                                     int           quality,
                                     int           bitrate,
                                     bool (*cancel)(void *),
                                     void *        cancel_data) {
	// libopusenc requires 48 kHz input
	const float * enc_audio = audio;
	int           enc_T     = T_audio;
	float *       resampled = nullptr;
	if (sr != 48000) {
		int T_rs  = 0;
		resampled = audio_resample(audio, T_audio, sr, 48000, 2, &T_rs);
		if (!resampled) {
			fprintf(stderr, "[Audio-Resample] Resample failed\n");
			return "";
		}
		fprintf(stderr, "[Audio-Resample] %d Hz -> 48000 Hz (%d -> %d samples)\n", sr, T_audio, T_rs);
		enc_audio = resampled;
		enc_T     = T_rs;
	}

	std::string out;

	// write/close callbacks: append chunks to the std::string
	auto write_cb = [](void * user_data, const unsigned char * ptr, opus_int32 len) -> int {
		auto * out_ptr = (std::string *) user_data;
		out_ptr->append((const char *) ptr, (size_t) len);
		return 0;
	};
	auto close_cb = [](void * user_data) -> int {
		(void) user_data;
		return 0;
	};

	OggOpusComments * comments = ope_comments_create();
	if (!comments) {
		free(resampled);
		fprintf(stderr, "[Opus] ope_comments_create failed\n");
		return "";
	}

	int           err = 0;
	OpusEncCallbacks cbs;
	cbs.write = write_cb;
	cbs.close = close_cb;
	OggOpusEnc *enc = ope_encoder_create_callbacks(&cbs, &out, comments, 48000, 2, 0, &err);
	if (!enc || err != OPE_OK) {
		fprintf(stderr, "[Opus] ope_encoder_create_callbacks failed: %d\n", err);
		ope_comments_destroy(comments);
		free(resampled);
		return "";
	}

	if (bitrate > 0) {
		ope_encoder_ctl(enc, OPUS_SET_BITRATE(bitrate * 1000));
	}
	if (quality >= 0) {
		opus_int32 c = quality;
		if (c < 0) {
			c = 0;
		}
		if (c > 10) {
			c = 10;
		}
		ope_encoder_ctl(enc, OPUS_SET_COMPLEXITY(c));
	}

	// encode interleaved in ~1s chunks; cancel responsive. ope_encoder_write_float
	// expects interleaved stereo, so deinterleave from the planar layout first.
	const int          chunk = 48000;  // 1s @ 48 kHz
	std::vector<float> inter((size_t) chunk * 2);
	for (int off = 0; off < enc_T; off += chunk) {
		if (cancel && cancel(cancel_data)) {
			break;
		}
		int len = (off + chunk <= enc_T) ? chunk : (enc_T - off);
		for (int i = 0; i < len; i++) {
			inter[i * 2 + 0] = enc_audio[off + i];
			inter[i * 2 + 1] = enc_audio[enc_T + off + i];
		}
		int n = ope_encoder_write_float(enc, inter.data(), len);
		if (n != OPE_OK) {
			fprintf(stderr, "[Opus] ope_encoder_write_float failed: %d\n", n);
			ope_encoder_destroy(enc);
			ope_comments_destroy(comments);
			free(resampled);
			return "";
		}
	}

	ope_encoder_drain(enc);
	ope_encoder_destroy(enc);
	ope_comments_destroy(comments);
	free(resampled);

	if (cancel && cancel(cancel_data)) {
		fprintf(stderr, "[Opus] Cancelled\n");
		return "";
	}

	return out;
}

// Decode Ogg Opus from memory buffer. Returns planar stereo float.
static float * audio_io_read_opus_buf(const uint8_t * data, size_t size, int * T_out, int * sr_out) {
	*T_out  = 0;
	*sr_out = 48000;

	int err = 0;
	OggOpusFile * of = op_open_memory(data, size, &err);
	if (!of) {
		fprintf(stderr, "[Opus] op_open_memory failed: %d (unsupported Ogg stream?)\n", err);
		return nullptr;
	}

	std::vector<float> interleaved;
	const int          chunk = 5760;  // 120 ms at 48 kHz, op_read_float_stereo max
	std::vector<float> buf((size_t) chunk * 2);
	for (;;) {
		int n = op_read_float_stereo(of, buf.data(), (int) buf.size());
		if (n == 0) {
			break;
		}
		if (n < 0) {
			fprintf(stderr, "[Opus] op_read_float_stereo error: %d\n", n);
			op_free(of);
			return nullptr;
		}
		interleaved.insert(interleaved.end(), buf.begin(), buf.begin() + (ptrdiff_t) (n * 2));
	}
	op_free(of);

	int T = (int) (interleaved.size() / 2);
	if (T == 0) {
		fprintf(stderr, "[Opus] No audio decoded\n");
		return nullptr;
	}

	float * planar = (float *) malloc((size_t) T * 2 * sizeof(float));
	if (!planar) {
		return nullptr;
	}
	for (int t = 0; t < T; t++) {
		planar[t]     = interleaved[(size_t) t * 2 + 0];
		planar[T + t] = interleaved[(size_t) t * 2 + 1];
	}

	*T_out = T;
	fprintf(stderr, "[Opus] Read buffer: %d samples, 48000 Hz, stereo\n", T);
	return planar;
}
#endif  // ACE_HAVE_OPUS

// ============================================================================
// WAV decode (always available)
// ============================================================================

static float * audio_io_read_wav_buf(const uint8_t * data, size_t size, int * T_out, int * sr_out) {
	*T_out  = 0;
	*sr_out = 0;

	int     T = 0, sr = 0;
	float * interleaved = read_wav_buf(data, size, &T, &sr);
	if (!interleaved) {
		return nullptr;
	}

	float * planar = (float *) malloc((size_t) T * 2 * sizeof(float));
	if (!planar) {
		free(interleaved);
		return nullptr;
	}
	for (int t = 0; t < T; t++) {
		planar[t]     = interleaved[t * 2 + 0];
		planar[T + t] = interleaved[t * 2 + 1];
	}
	free(interleaved);

	*T_out  = T;
	*sr_out = sr;
	return planar;
}

// ============================================================================
// Decode dispatch (by magic)
// ============================================================================

float * audio_read_buf(const uint8_t * data, size_t size, int * T_out, int * sr_out) {
	*T_out  = 0;
	*sr_out = 0;
	if (size < 4) {
		fprintf(stderr, "[Audio] Buffer too small (%zu bytes)\n", size);
		return nullptr;
	}

	if (memcmp(data, "RIFF", 4) == 0) {
		return audio_io_read_wav_buf(data, size, T_out, sr_out);
	}
	if (memcmp(data, "fLaC", 4) == 0) {
#ifdef ACE_HAVE_FLAC
		return audio_io_read_flac_buf(data, size, T_out, sr_out);
#else
		fprintf(stderr, "[Audio] FLAC input requires building with -DFLAC=ON (libFLAC)\n");
		return nullptr;
#endif
	}
	if (memcmp(data, "OggS", 4) == 0) {
#ifdef ACE_HAVE_OPUS
		return audio_io_read_opus_buf(data, size, T_out, sr_out);
#else
		fprintf(stderr, "[Audio] Ogg input requires building with -DOPUS=ON (libopusfile)\n");
		return nullptr;
#endif
	}

#ifdef ACE_HAVE_MP3
	return audio_io_read_mp3_buf(data, size, T_out, sr_out);
#else
	fprintf(stderr, "[Audio] MP3 input requires building with -DMP3=ON (libmp3lame + minimp3)\n");
	return nullptr;
#endif
}

float * audio_read_48k_buf(const uint8_t * data, size_t size, int * T_out) {
	int     T = 0, sr = 0;
	float * raw = audio_read_buf(data, size, &T, &sr);
	if (!raw) {
		*T_out = 0;
		return nullptr;
	}

	if (sr == 48000) {
		*T_out = T;
		return raw;
	}

	int T_rs = 0;
	fprintf(stderr, "[Audio-Resample] %d Hz -> 48000 Hz, %d samples...\n", sr, T);
	float * resampled = audio_resample(raw, T, sr, 48000, 2, &T_rs);
	free(raw);

	if (!resampled) {
		fprintf(stderr, "[Audio-Resample] Resample failed\n");
		*T_out = 0;
		return nullptr;
	}

	fprintf(stderr, "[Audio-Resample] Done: %d -> %d samples\n", T, T_rs);

	*T_out = T_rs;
	return resampled;
}

float * audio_read(const char * path, int * T_out, int * sr_out) {
	size_t    size = 0;
	uint8_t * buf  = audio_io_load_file(path, &size);
	if (!buf) {
		return nullptr;
	}
	float * result = audio_read_buf(buf, size, T_out, sr_out);
	free(buf);
	return result;
}

float * audio_read_48k(const char * path, int * T_out) {
	int     T = 0, sr = 0;
	float * raw = audio_read(path, &T, &sr);
	if (!raw) {
		*T_out = 0;
		return nullptr;
	}

	if (sr == 48000) {
		*T_out = T;
		return raw;
	}

	int T_rs = 0;
	fprintf(stderr, "[Audio-Resample] %d Hz -> 48000 Hz, %d samples...\n", sr, T);
	float * resampled = audio_resample(raw, T, sr, 48000, 2, &T_rs);
	free(raw);

	if (!resampled) {
		fprintf(stderr, "[Audio-Resample] Resample failed\n");
		*T_out = 0;
		return nullptr;
	}

	fprintf(stderr, "[Audio-Resample] Done: %d -> %d samples\n", T, T_rs);

	*T_out = T_rs;
	return resampled;
}

// ============================================================================
// Encode dispatch + audio_write
// ============================================================================

std::string audio_encode(const float * audio, int T, int sr,
                         const AudioFormat & fmt, int quality, int bitrate,
                         bool (*cancel)(void *), void * cancel_data) {
	switch (fmt.codec) {
		case CODEC_WAV:
			return audio_encode_wav(audio, T, sr, fmt.wav_subformat);
		case CODEC_MP3:
#ifdef ACE_HAVE_MP3
			return audio_encode_mp3(audio, T, sr, quality, bitrate, cancel, cancel_data);
#else
			(void) cancel;
			(void) cancel_data;
			fprintf(stderr, "[Audio] MP3 output requires building with -DMP3=ON (libmp3lame)\n");
			return "";
#endif
		case CODEC_OPUS:
#ifdef ACE_HAVE_OPUS
			return audio_encode_opus(audio, T, sr, quality, bitrate, cancel, cancel_data);
#else
			(void) cancel;
			(void) cancel_data;
			fprintf(stderr, "[Audio] Opus output requires building with -DOPUS=ON (libopusenc)\n");
			return "";
#endif
		case CODEC_FLAC:
#ifdef ACE_HAVE_FLAC
			return audio_encode_flac(audio, T, sr, quality, bitrate, cancel, cancel_data);
#else
			(void) cancel;
			(void) cancel_data;
			fprintf(stderr, "[Audio] FLAC output requires building with -DFLAC=ON (libFLAC)\n");
			return "";
#endif
	}
	return "";
}

bool audio_write(const char * path, const float * audio, int T, int sr,
                 const AudioFormat & fmt, int quality, int bitrate, int peak_clip) {
	// Copy const input and normalize the copy, except WAV_F32 (passthrough).
	std::vector<float> buf;
	const float *      enc = audio;
	if (!(fmt.codec == CODEC_WAV && fmt.wav_subformat == WAV_F32)) {
		buf.assign(audio, audio + (size_t) T * 2);
		audio_normalize(buf.data(), T * 2, peak_clip);
		enc = buf.data();
	}

	std::string encoded = audio_encode(enc, T, sr, fmt, quality, bitrate);
	if (encoded.empty()) {
		return false;
	}

	FILE * fp = fopen(path, "wb");
	if (!fp) {
		fprintf(stderr, "[Audio] Cannot open %s for writing\n", path);
		return false;
	}
	fwrite(encoded.data(), 1, encoded.size(), fp);
	fclose(fp);

	fprintf(stderr, "[Audio] Wrote %s: %d samples, %d Hz, %s\n", path, T, sr, audio_format_extension(fmt) + 1);
	return true;
}
