// mp3-codec.cpp: standalone audio transcoder.
//
// Encode: mp3-codec -i input.<fmt> -o output.<fmt> [-q <q>] [-b <kbps>] [--format wav16|wav24|wav32]
// Decode: mp3-codec -i input.<fmt> -o output.<fmt>
//
// Container is auto-detected from output extension. WAV is always available;
// MP3/Opus/FLAC require the matching CMake flag (-DMP3/-DOPUS/-DFLAC).
// The binary keeps its historical name; "codec" no longer implies MP3.
//
// Input formats: WAV (always), MP3 (-DMP3), FLAC (-DFLAC), Ogg Opus (-DOPUS).
// Output formats: WAV variants (always), MP3 (-DMP3), Opus (-DOPUS), FLAC (-DFLAC).

#include "audio-io.h"
#include "task-types.h"
#include "version.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

static bool ends_with(const char * str, const char * suffix) {
	int slen = (int) strlen(str);
	int xlen = (int) strlen(suffix);
	if (slen < xlen) {
		return false;
	}
	for (int i = 0; i < xlen; i++) {
		char a = str[slen - xlen + i];
		char b = suffix[i];
		if (a >= 'A' && a <= 'Z') {
			a = (char) (a + 32);
		}
		if (b >= 'A' && b <= 'Z') {
			b = (char) (b + 32);
		}
		if (a != b) {
			return false;
		}
	}
	return true;
}

static void usage(const char * prog) {
	fprintf(stderr, "acestep.cpp %s\n\n", ACE_VERSION);
	fprintf(stderr,
            "Usage: %s -i <input> -o <output> [options]\n"
            "\n"
            "  -i <path>          Input file (WAV always; MP3/FLAC/Opus when built)\n"
            "  -o <path>          Output file (.wav/.mp3/.opus/.flac)\n"
            "  -q <quality>       Codec quality (codec-specific scale, default: -1)\n"
            "                     MP3 0..9 (VBR -V), Opus 0..10 (complexity), FLAC 0..8 (level)\n"
            "  -b <kbps>          Bitrate for lossy codecs (default: -1 = codec default)\n"
            "  --format <fmt>     WAV sub-format: wav16, wav24, wav32 (default: wav16).\n"
            "                     Ignored when -o is not a .wav file.\n"
            "\n"
            "Direction is auto-detected from output extension. Compiled-in output\n"
            "formats: %s.\n"
            "\n"
            "Examples:\n"
            "  %s -i song.wav -o song.mp3\n"
            "  %s -i song.wav -o song.mp3 -b 192\n"
            "  %s -i song.wav -o song.opus -q 8\n"
            "  %s -i song.flac -o song.wav --format wav32\n",
            prog, audio_available_formats_text().c_str(), prog, prog, prog, prog);
}

int main(int argc, char ** argv) {
	const char * input   = nullptr;
	const char * output  = nullptr;
	int          quality = -1;
	int          bitrate = -1;
	WavFormat    wav_fmt = WAV_S16;

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-i") == 0 && i + 1 < argc) {
			input = argv[++i];
		} else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
			output = argv[++i];
		} else if (strcmp(argv[i], "-q") == 0 && i + 1 < argc) {
			quality = atoi(argv[++i]);
		} else if (strcmp(argv[i], "-b") == 0 && i + 1 < argc) {
			bitrate = atoi(argv[++i]);
		} else if (strcmp(argv[i], "--format") == 0 && i + 1 < argc) {
			AudioFormat parsed{ CODEC_WAV, WAV_S16 };
			if (!audio_parse_format(argv[++i], parsed)) {
				fprintf(stderr, "[mp3-codec] Unknown WAV format: %s\n", argv[i]);
				return 1;
			}
			wav_fmt = parsed.wav_subformat;
		} else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
			usage(argv[0]);
			return 0;
		} else {
			fprintf(stderr, "[mp3-codec] Unknown option: %s\n", argv[i]);
			return 1;
		}
	}

	if (!input || !output) {
		usage(argv[0]);
		return 1;
	}

	// read input (auto-detect by magic)
	int     T = 0, sr = 0;
	float * audio = audio_read(input, &T, &sr);
	if (!audio) {
		return 1;
	}

	// resolve output codec from extension
	AudioFormat out_fmt{ CODEC_WAV, wav_fmt };
	bool        ok = true;
	if (ends_with(output, ".wav")) {
		out_fmt.codec        = CODEC_WAV;
		out_fmt.wav_subformat = wav_fmt;
	} else if (ends_with(output, ".mp3")) {
		if (!audio_codec_available(CODEC_MP3)) {
			fprintf(stderr, "[mp3-codec] MP3 output requires -DMP3=ON (libmp3lame). Compiled formats: %s\n",
			        audio_available_formats_text().c_str());
			free(audio);
			return 1;
		}
		out_fmt.codec = CODEC_MP3;
	} else if (ends_with(output, ".opus")) {
		if (!audio_codec_available(CODEC_OPUS)) {
			fprintf(stderr, "[mp3-codec] Opus output requires -DOPUS=ON (libopusenc). Compiled formats: %s\n",
			        audio_available_formats_text().c_str());
			free(audio);
			return 1;
		}
		out_fmt.codec = CODEC_OPUS;
	} else if (ends_with(output, ".flac")) {
		if (!audio_codec_available(CODEC_FLAC)) {
			fprintf(stderr, "[mp3-codec] FLAC output requires -DFLAC=ON (libFLAC). Compiled formats: %s\n",
			        audio_available_formats_text().c_str());
			free(audio);
			return 1;
		}
		out_fmt.codec = CODEC_FLAC;
	} else {
		fprintf(stderr, "[mp3-codec] Cannot determine format from output extension: %s\n", output);
		fprintf(stderr, "  use .wav, .mp3, .opus or .flac\n");
		free(audio);
		return 1;
	}

	ok = audio_write(output, audio, T, sr, out_fmt, quality, bitrate);

	free(audio);
	return ok ? 0 : 1;
}
