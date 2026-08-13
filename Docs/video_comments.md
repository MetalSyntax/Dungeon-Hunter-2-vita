# Technical Documentation: `source/video.cpp`

This document details hardware video playback using `SceAvPlayer` implemented in [`source/video.cpp`](file:///Volumes/Seagate/PSVITA%20Develop/Dungeon-Hunter-2-vita/source/video.cpp).

---

## 1. Video Playback Architecture

`source/video.cpp` enables hardware-accelerated MP4/H.264 video playback using the PS Vita's native decoder module (`SCE_SYSMODULE_AVPLAYER`). Key components include:

1. **Input File Handling (`AvFileCtx`):**
   - Custom file I/O callbacks (`av_file_open`, `av_file_read`, `av_file_size`, `av_file_close`) built on `sceIoPread`. Provides full visibility into read offsets and buffer sizes requested by `SceAvPlayer`.
2. **CDRAM & PHYCONT Memory Allocation (`av_alloc_texture`):**
   - Allocates video texture framebuffers in physical **CDRAM** (`SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW`) aligned to 256KB (`0x40000`).
   - Automatically falls back to **PHYCONT** memory (`SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_RW`) if CDRAM is full.
   - Maps memory into the graphics pipeline using `sceGxmMapMemory`.
3. **GPU-Accelerated YUV to RGB Conversion (`VIDEO_GPU_YUV_CONVERT`):**
   - **Performance Optimization:** Software CPU color conversion (NV12 to RGB565 via ARM NEON) consumed ~228 ms per frame on 1280x720 video (resulting in ~2.6 FPS).
   - **GPU Solution:** Replaced with an OpenGL ES 2.0 fragment shader that samples the Y plane as a `GL_LUMINANCE` texture and the UV plane as `GL_LUMINANCE_ALPHA`. The BT.601 color matrix runs directly on the GPU, raising framerates to ~14-25 FPS.
   - **Upload Downsampling (`VIDEO_DOWNSAMPLE_UPLOAD`):** Halves uploaded texture dimensions when source resolution exceeds native Vita screen size, optimizing bus bandwidth.

---

## 2. Fullscreen OpenGL ES 2.0 Rendering (`draw_video_frame`)

Unlike `vitaGL` ports using GLES1 fixed-function pipelines (`glOrthof`, `glVertexPointer`), this port uses pure OpenGL ES 2.0 over `PVR_PSP2`:
- Compiles and links a dedicated GLSL ES shader program (`gVideoProgram`).
- Computes an adaptive fullscreen quad in NDC coordinates (`-1.0` to `1.0`) maintaining the video's original aspect ratio (letterboxed on 960x544).
- Saves and restores existing OpenGL context states (VBO bindings, blend, depth test, scissor test, viewport) so video playback does not corrupt main game rendering.

---

## 3. Cutscene Audio Output

- Audio frames decoded by `SceAvPlayer` are submitted to a dedicated voice port (`SCE_AUDIO_OUT_PORT_TYPE_VOICE`) via `sceAudioOutOpenPort`.
- A dedicated thread (`cutscene_audio_thread`) asynchronously processes and outputs audio blocks using a double buffer (`gCutAudioBuf`), avoiding stuttering during disk reads.
