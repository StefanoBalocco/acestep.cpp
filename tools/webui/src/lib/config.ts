// UI constants
export const PROPS_POLL_MS = 10000;
export const FETCH_TIMEOUT_MS = 2000;
export const JOB_POLL_MS = 2000;
export const SSE_RECONNECT_MS = 2000;
export const LOG_MAX_LINES = 50;
export const WAVEFORM_HEIGHT = 64;
export const WAVEFORM_BINS = 4096;

// task types (mirrors task-types.h)
export const TASK_TEXT2MUSIC = 'text2music';
export const TASK_COVER = 'cover';
export const TASK_COVER_NOFSQ = 'cover-nofsq';
export const TASK_REPAINT = 'repaint';
export const TASK_LEGO = 'lego';
export const TASK_EXTRACT = 'extract';
export const TASK_COMPLETE = 'complete';

// Solver names (mirrors task-types.h SOLVER_* and the C++ solver registry).
// The string is the canonical solver key resolved by solver_lookup().
export const SOLVER_EULER = 'euler';
export const SOLVER_SDE = 'sde';
export const SOLVER_DPM3M = 'dpm3m';
export const SOLVER_STORK4 = 'stork4';

// DCW modes (mirrors task-types.h DCW_MODE_*)
export const DCW_MODE_LOW = 'low';
export const DCW_MODE_HIGH = 'high';
export const DCW_MODE_DOUBLE = 'double';
export const DCW_MODE_PIX = 'pix';

// Output format keys (mirrors task-types.h OUTPUT_FORMAT_*). Sent as the
// `output_format` field on AceRequest.
export const OUTPUT_FORMAT_WAV16 = 'wav16';
export const OUTPUT_FORMAT_WAV24 = 'wav24';
export const OUTPUT_FORMAT_WAV32 = 'wav32';
export const OUTPUT_FORMAT_MP3 = 'mp3';
export const OUTPUT_FORMAT_OPUS = 'opus';
export const OUTPUT_FORMAT_FLAC = 'flac';

// Canonical list of all output formats the UI knows about.
export const OUTPUT_FORMATS = [
	OUTPUT_FORMAT_WAV16,
	OUTPUT_FORMAT_WAV24,
	OUTPUT_FORMAT_WAV32,
	OUTPUT_FORMAT_MP3,
	OUTPUT_FORMAT_OPUS,
	OUTPUT_FORMAT_FLAC
] as const;

// Display name per format key, used to populate the format <select>.
export const OUTPUT_FORMAT_LABELS: Record<string, string> = {
	wav16: 'WAV 16-bit',
	wav24: 'WAV 24-bit',
	wav32: 'WAV 32-bit float',
	mp3: 'MP3',
	opus: 'Opus',
	flac: 'FLAC'
};

export const TRACK_NAMES = [
	'vocals',
	'backing_vocals',
	'drums',
	'bass',
	'guitar',
	'keyboard',
	'percussion',
	'strings',
	'synth',
	'fx',
	'brass',
	'woodwinds'
] as const;
