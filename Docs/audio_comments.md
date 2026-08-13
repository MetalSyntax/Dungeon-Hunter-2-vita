# Technical Documentation: `source/audio.cpp`

This document details the architecture and implementation of the audio subsystem in [`source/audio.cpp`](file:///Volumes/Seagate/PSVITA%20Develop/Dungeon-Hunter-2-vita/source/audio.cpp) for the PS Vita.

---

## 1. Overall Audio Architecture

The audio subsystem combines two primary audio pipelines targeting the PS Vita hardware (`sceAudioOut`):

1. **Generic `GLMediaPlayer` Software Mixer (Dedicated Thread):**
   - Handles short sound effects (SFX) and background music (BGM).
   - Manages up to 12 polyphonic voices simultaneously (`MAX_VOICES = 12`) plus 1 dedicated channel for large audio streams / BGM (`gBig`).
   - Performs bilinear resampling, pitch interpolation, volume gain ramping (fade in/out), and soft-clipping (`tanhf`) at 44,100 Hz stereo 16-bit PCM.

2. **`android/media/AudioTrack` Shim (Native Vox Middleware):**
   - Gameloft's native engine incorporates its own audio middleware (`vox::DriverAndroid`), which directly decodes and mixes `.wav` and `.vxn` sound files.
   - Instead of using Java `GLMediaPlayer` callbacks, the native engine routes mixed audio through `android.media.AudioTrack.write()` (intercepted in C++ by `Misc_AudioTrackWrite`).
   - AudioTrack PCM samples feed into a FIFO buffer (`gATFifo`) where the mixer thread (`mixer_thread`) drains and mixes them directly into the hardware output.

---

## 2. Audio Decoding

### 2.1 WAV Files (16-bit PCM and IMA-ADPCM)

- `decode_wav_file`: Generically parses RIFF/WAVE chunks while safely skipping non-standard metadata (such as Gameloft's proprietary `fact` and `voxu` chunks).
- `decode_ima_adpcm`: Self-contained 4-bit Microsoft IMA-ADPCM decoder without external library dependencies. Supports mono (`decode_ima_block_mono`) and stereo (`decode_ima_block_stereo`).

### 2.2 VoxN (`.vxn`) Containers

- `decode_vxn_file`: Decoder for Gameloft's proprietary audio container found in `data/sounds/m_level_*.vxn`. Parses `Afmt` and `Data` chunks, extracting level ambient audio.

---

## 3. Hardware Insights & Fixes

### 3.1 `SCE_AUDIO_OUT_ERROR_PORT_FULL` (`0x80260005`) Fix

- **Issue:** Initial implementations attempted to open a second BGM-type `sceAudioOut` port inside `Misc_AudioTrackWrite`, which consistently failed on PS Vita due to per-process hardware BGM port limits.
- **Solution:** Removed the second port allocation. All AudioTrack output is drained via `drain_audiotrack_into` directly inside the existing `mixer_thread`. Both `GLMediaPlayer` effects and `AudioTrack` streams now share **a single hardware port and a single output thread**.

### 3.2 Safe No-op Stub Interface Methods

- Methods such as `unloadSound`, `resetSound`, `destroySoundPool`, `initSoundPoolArray` are kept as harmless no-ops. Attempting to intercept them with unverified `va_list` signatures against the `.so` binary caused ARM stack misalignment and boot crashes.

# audio.cpp Pure comments

```cpp
// See audio.h for the full picture. Summary: a small sceAudioOut mixer
// (architecture proven by the sibling Prince-of-Persia-vita port, adapted
// here for plain/IMA-ADPCM WAV instead of mp3) backing GLMediaPlayer's
// previously-all-stub sound bridge in java.c.
//
// Catalog is shared between "Sound" (small, pooled, one-shot -- playSound
// has no loop argument, confirmed against java.c's existing, previously-
// verified stub signature) and "SoundBig" (long tracks, playSoundBig DOES
// take a loop flag) -- both draw from the SAME Sounddefs[] id space, so one
// cache indexed by sndId serves both.
//
// Scope deliberately NOT covered this pass (left as safe no-ops, matching
// their pre-existing behavior in java.c, since their real argument lists
// were never verified against the decompiled .so the way loadSound/
// playSound's were -- see java.c's own top-of-file methodology note):
// unloadSound/unloadSoundBig, pauseSound(Big), resumeSound(Big),
// stopSound(Big) (single-instance stop by id), setVolume(Big), setPitch,
// resetSound, destroySoundPool, initSoundPoolArray. Guessing a wrong
// va_arg() type/count here reads garbage off the real stack (see PoP's own
// Cocos2dxSound_playEffect fix comment for exactly this failure mode) --
// worth doing once someone can cross-reference the real nativeInit call
// sites in out_ghidra.c properly, not worth the crash risk to guess now.
// playSoundBig's "fadeIn" argument is consumed (so the vararg stack stays
// aligned for nothing after it) but not actually implemented as a ramp --
// starts at full target volume immediately.

// Frames per sceAudioOutOutput block (~46ms), same size proven on this
// project's other Vita audio references -- gives the mixer thread (12-voice
// resample + mix, not just a memcpy) slack before the hardware needs the
// next block.

// ---- WAV decode (self-contained, no external library) ----------------

// Standard ITU/IMA ADPCM step and index-adjustment tables (the same public
// tables used by every MS-IMA-ADPCM decoder -- this is a documented codec,
// not reverse-engineered).

// One block layout (Microsoft IMA-ADPCM, mono): 4-byte header (int16
// predictor, uint8 step index, uint8 reserved) then (blockAlign-4)*2 nibbles
// decoded sequentially -- samplesPerBlock total including the header sample.

// Generic RIFF chunk walk -- does NOT assume "fmt " is immediately followed
// by "data" (real files here have "fact" and a Gameloft-specific "voxu"
// metadata chunk in between for the IMA-ADPCM ones), so unknown chunks are
// skipped by their own declared size instead.

// ---- VoxN container decode (data/sounds/m_level_*.vxn) -----------------
//
// A plain chunked container, structurally identical in spirit to RIFF/WAV:
// [4-byte ASCII tag][4-byte LE length][length bytes of payload], repeated.
// Verified generically across 3 different files -- see audio.h. Only "Afmt"
// (same 3 fields as WAV's fmt: format tag/channels/rate, no block-align
// field though, so samplesPerBlock is computed the same standard way as for
// the .wav IMA-ADPCM files) and "Data" (the raw payload) are read; every
// other chunk (adaptive-music metadata: loop segments, transition rules,
// groups) is skipped by its own self-declared length, unparsed.

// Same defensive cap as the WAV parser's chunk walk: a real file only
// ever has ~8 metadata chunks before "Data" (confirmed by inspection),
// so this only ever protects against a corrupt/foreign file with no
// "Data" tag from looping until EOF one 8-byte read at a time.

// blockAlign isn't stored in Afmt (unlike WAV's extended fmt chunk) --
// every sample file inspected uses 1024, the standard block size this
// engine's own tools default to for 32kHz stereo IMA-ADPCM (matches the
// .wav music tracks exactly). samplesPerBlock is then the same fixed
// function of channels/blockAlign as everywhere else in this file.

// ---- catalog cache, shared by "Sound" and "SoundBig" (same Sounddefs id space) ----

// ---- android/media/AudioTrack shim -------------------------------------
//
// log_122.txt: audio_init() succeeded and the mixer thread ran, but
// GLMediaPlayer.loadSound/playSound were never called even once in a full
// session that reached real combat (confirmed by their total absence from
// the log, while data/sounds/*.wav files WERE being fopen'd the whole time).
// Traced the real path in out_ghidra.c: this engine embeds its own native
// audio middleware, "vox::DriverAndroid" (matches the ".vxn"/"VoxN"
// container name from audio.h's own notes) -- it decodes and mixes
// everything itself in native code (explaining the direct fopen() calls),
// confirmed running at 44100Hz stereo 16-bit PCM (SetDriverSampleRate(0xac44)
// and a getMinBufferSize(44100, CHANNEL_OUT_STEREO=12, ENCODING_PCM_16BIT=2)
// call, both in vox::DriverAndroid::_InitAT), and only touches Java at all
// to push the final mixed buffer out via android.media.AudioTrack.write() --
// which java.c's Misc_AudioTrackWrite stub has always read correctly (3 args:
// byte[] data, int offset, int sizeInBytes) but then just returned
// sizeInBytes without touching the actual bytes. THIS was the real silent
// culprit the whole time, not GLMediaPlayer -- that JNI surface (audio.cpp's
// GLMediaPlayer_* functions above) is very likely dead/legacy code in this
// particular build and is kept only in case something else does call it.
//
// log_124.txt update: the first version of this shim opened its OWN second
// sceAudioOut BGM port (audiotrack_ensure_port() below, now removed) and hit
// SCE_AUDIO_OUT_ERROR_PORT_FULL (0x80260005) on every single call, forever --
// audio_init()'s own mixer port above already holds one BGM port, and this
// device's sceAudioOut BGM-port budget doesn't stretch to a second one for
// this build/firmware combination (confirmed 100% reproducible on hardware).
// Since this write() path is the REAL active audio (per the note above) and
// GLMediaPlayer's own mixer is very likely dead code, the fix is to stop
// asking for a second port at all: this shim now just fills gATFifo, and
// mixer_thread's existing single sceAudioOutOutput(gPort, ...) call (the one
// serving the GLMediaPlayer voices) drains and sums it in every grain --
// ONE hardware port, ONE output thread, two input sources.
//
// write() is called repeatedly from Vox's own dedicated audio thread
// (vox::DriverAndroid::UpdateThreadedAT) with MODE_STREAM semantics (the
// real Android AudioTrack.write() blocks until there's room) -- mirrored
// here by blocking (briefly sleeping, not spinning) while the FIFO is full
// instead of silently dropping audio, so backpressure matches what Vox's own
// thread expects.
```
