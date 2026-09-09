#pragma once
/**
 * recorder.h  —  Native in-game hardware-accelerated H.264 MP4 recorder
 *
 * Uses Windows Media Foundation IMFSinkWriter with an asynchronous frame queue
 * so encoding never stalls or stutters the game's render thread.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdint>

struct RecorderStats
{
    bool      isRecording;
    bool      isPaused;
    uint64_t  recordedFrames;
    uint64_t  durationMs;
    uint32_t  width;
    uint32_t  height;
    uint32_t  fps;
    uint32_t  bitrateKbps;
    char      outputPath[MAX_PATH];
    char      lastError[128];
};

void Recorder_Init();
void Recorder_Shutdown();

bool Recorder_Start(const char* customPath = nullptr, uint32_t fps = 30, uint32_t bitrateKbps = 8000);
void Recorder_Pause();
void Recorder_Resume();
void Recorder_Stop();

bool Recorder_IsRecording();
bool Recorder_IsPaused();
void Recorder_GetStats(RecorderStats* outStats);

bool Recorder_WantsFrame();
void Recorder_OnFrameReady(const void* pixels, uint32_t width, uint32_t height, uint32_t stride, uint64_t frameIdx);
