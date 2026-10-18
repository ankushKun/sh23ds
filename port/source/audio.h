#pragma once
#include <3ds.h>

// Streaming music: CRI ADX (version 3/4, standard encoding) and AIX containers (every layer
// decoded and mixed with its own volume), decoded on the CPU and played through ndsp channel 0.
// Real hardware needs sdmc:/3ds/dspfirm.cdc.

bool audio_init(void);
void audio_exit(void);

// Starts a music stream (loops at the ADX loop points when present). Returns false if unreadable.
// AIX layer 0 starts at full volume and the others silent.
bool audio_play_music(const char* path, float volume);
// The options' volumes, 0..1: music (channel 0) and the cutscene voices (channel 1).
void audio_set_gains(float music, float voice);
void audio_set_output(int mode);          // 0 mono, 1 stereo (default), 2 surround
bool audio_play_layers(const char* path, float volume);     // same, every layer silent
void audio_stop_music(void);

// AIX layer gain 0..1 (ADX streams have one layer). Changes ramp over the next decoded buffer.
void audio_set_layer_volume(int layer, float volume);
int audio_layer_count(void);

// Cutscene voice (demo.c) on its own ndsp channel, mixed over the music. Plays once.
bool audio_play_voice(const char* path, float volume);
void audio_stop_voice(void);
bool audio_voice_playing(void);
float audio_voice_position(void);   // seconds heard, < 0 before it starts

// Call once per frame: refills the ndsp wave buffers (wakes the refill thread when there is one).
void audio_update(void);

const char* audio_status(void);
// Percent of one core the music decoding took since the last call (-1: nothing decoded).
int audio_decode_load(void);
