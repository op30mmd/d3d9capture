/**
 * shm_reader.cpp  —  Out-of-process frame consumer
 *
 * Reads frames written by consumer_backend.cpp via shared memory, and
 * optionally encodes them to an H.264 MP4 using Media Foundation.
 *
 * Media Foundation is used rather than a bundled encoder because it ships with
 * Windows: no third-party dependency, no static blob, no effect on this
 * project's Apache-2.0 licensing, and it picks up a hardware encoder where one
 * is available. Statically linking FFmpeg would drag in either the LGPL relink
 * obligation or, with --enable-gpl, a licence change for the whole binary.
 *
 * Build:
 *   cl /nologo /W3 /O2 /MT /Fe:shm_reader.exe shm_reader.cpp
 *   (the Media Foundation libraries are pulled in by #pragma comment below)
 *
 * Usage:
 *   shm_reader.exe [--record out.mp4] [--fps N] [--bitrate KBPS]
 *                  [--save N [dir]] [--seconds S]
 *
 *   --record   encode delivered frames to an H.264 MP4
 *   --fps      frames per second to request and encode (default 30)
 *   --bitrate  target bitrate in kbit/s (default 8000)
 *   --save     also write the first N delivered frames as BMPs
 *   --seconds  stop after S seconds (otherwise run until Ctrl-C)
 *
 * On the capture rate: the producer only captures a frame while this reader is
 * waiting for one, so pacing here throttles the *game's* cost, not just ours.
 * Asking for 30 fps means the game pays the readback 30 times a second instead
 * of on every presented frame. These knobs are deliberately parameters rather
 * than constants so a control panel can drive them later.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>

#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "ole32.lib")

// Must match consumer_backend.cpp exactly.
static constexpr DWORD SHM_MAX_BYTES     = 7680 * 4320 * 4 + 64;
static constexpr char  SHM_NAME[]        = "Local\\D3D9CaptureShm";
static constexpr char  EVT_FRAME_READY[] = "Local\\D3D9CaptureReady";
static constexpr char  EVT_FRAME_DONE[]  = "Local\\D3D9CaptureDone";

#pragma pack(push, 1)
struct ShmHeader
{
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t format;
    uint64_t frameIdx;
    uint64_t timestampQpc;   // QueryPerformanceCounter at capture time
    uint32_t dataOffset;
};
#pragma pack(pop)

// ── BMP writer ───────────────────────────────────────────────────────────────

// Minimal 32-bpp BMP writer. Rows arrive tightly packed and top-down; BMP is
// bottom-up, so they are written in reverse.
static bool SaveBmp(const char* path, const uint8_t* pixels,
                    uint32_t width, uint32_t height, uint32_t stride)
{
    FILE* f = nullptr;
    if (fopen_s(&f, path, "wb") != 0 || !f) return false;

    const uint32_t rowBytes = width * 4;
    const uint32_t dataSize = rowBytes * height;

    BITMAPFILEHEADER fh = {};
    fh.bfType = 0x4D42;                                   // 'BM'
    fh.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
    fh.bfSize = fh.bfOffBits + dataSize;

    BITMAPINFOHEADER ih = {};
    ih.biSize = sizeof(ih);
    ih.biWidth = static_cast<LONG>(width);
    ih.biHeight = static_cast<LONG>(height);            // positive: bottom-up
    ih.biPlanes = 1;
    ih.biBitCount = 32;
    ih.biCompression = BI_RGB;
    ih.biSizeImage = dataSize;

    fwrite(&fh, sizeof(fh), 1, f);
    fwrite(&ih, sizeof(ih), 1, f);
    for (int y = static_cast<int>(height) - 1; y >= 0; --y)
        fwrite(pixels + static_cast<size_t>(y) * stride, rowBytes, 1, f);

    fclose(f);
    return true;
}

// ── Media Foundation H.264 recorder ──────────────────────────────────────────

class Recorder
{
public:
    bool Start(const char* path, uint32_t width, uint32_t height,
               uint32_t fps, uint32_t bitrateKbps);
    bool WriteFrame(const uint8_t* bgra, uint32_t srcStride, uint64_t qpc);
    void Finish();
    bool Active() const { return m_writer != nullptr; }
    uint64_t Frames() const { return m_frames; }

private:
    IMFSinkWriter* m_writer = nullptr;
    DWORD          m_stream = 0;
    uint32_t       m_width = 0, m_height = 0, m_fps = 30;
    LONGLONG       m_qpcFreq = 0;
    uint64_t       m_baseQpc = 0;
    uint64_t       m_frames = 0;
};

bool Recorder::Start(const char* path, uint32_t width, uint32_t height,
                     uint32_t fps, uint32_t bitrateKbps)
{
    m_width = width; m_height = height; m_fps = fps ? fps : 30;

    LARGE_INTEGER freq = {};
    QueryPerformanceFrequency(&freq);
    m_qpcFreq = freq.QuadPart;

    wchar_t wpath[MAX_PATH] = {};
    MultiByteToWideChar(CP_ACP, 0, path, -1, wpath, MAX_PATH);

    // Let the sink writer use a hardware encoder MFT when the GPU provides one.
    IMFAttributes* attrs = nullptr;
    if (FAILED(MFCreateAttributes(&attrs, 2))) return false;
    attrs->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
    attrs->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE);

    HRESULT hr = MFCreateSinkWriterFromURL(wpath, nullptr, attrs, &m_writer);
    attrs->Release();
    if (FAILED(hr))
    {
        printf("[rec] MFCreateSinkWriterFromURL failed: 0x%08lX\n", hr);
        m_writer = nullptr;
        return false;
    }

    // Output: H.264 in an MP4 container.
    IMFMediaType* outType = nullptr;
    if (FAILED(MFCreateMediaType(&outType))) return false;
    outType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    outType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    outType->SetUINT32(MF_MT_AVG_BITRATE, bitrateKbps * 1000);
    outType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    MFSetAttributeSize(outType, MF_MT_FRAME_SIZE, m_width, m_height);
    MFSetAttributeRatio(outType, MF_MT_FRAME_RATE, m_fps, 1);
    MFSetAttributeRatio(outType, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    hr = m_writer->AddStream(outType, &m_stream);
    outType->Release();
    if (FAILED(hr)) { printf("[rec] AddStream failed: 0x%08lX\n", hr); return false; }

    // Input: the BGRA we receive over shared memory. The sink writer inserts a
    // colour converter to feed the encoder's NV12 input.
    IMFMediaType* inType = nullptr;
    if (FAILED(MFCreateMediaType(&inType))) return false;
    inType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    inType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
    inType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    // Positive stride declares a top-down image, which is how frames arrive.
    // Left unset, Media Foundation assumes the bottom-up GDI convention and the
    // video comes out vertically flipped.
    inType->SetUINT32(MF_MT_DEFAULT_STRIDE, m_width * 4);
    MFSetAttributeSize(inType, MF_MT_FRAME_SIZE, m_width, m_height);
    MFSetAttributeRatio(inType, MF_MT_FRAME_RATE, m_fps, 1);
    MFSetAttributeRatio(inType, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    hr = m_writer->SetInputMediaType(m_stream, inType, nullptr);
    inType->Release();
    if (FAILED(hr)) { printf("[rec] SetInputMediaType failed: 0x%08lX\n", hr); return false; }

    hr = m_writer->BeginWriting();
    if (FAILED(hr)) { printf("[rec] BeginWriting failed: 0x%08lX\n", hr); return false; }

    printf("[rec] Recording %ux%u @ %u fps, %u kbit/s -> %s\n",
           m_width, m_height, m_fps, bitrateKbps, path);
    return true;
}

bool Recorder::WriteFrame(const uint8_t* bgra, uint32_t srcStride, uint64_t qpc)
{
    if (!m_writer) return false;

    const DWORD frameBytes = m_width * m_height * 4;

    IMFMediaBuffer* buffer = nullptr;
    if (FAILED(MFCreateMemoryBuffer(frameBytes, &buffer))) return false;

    BYTE* dst = nullptr;
    if (FAILED(buffer->Lock(&dst, nullptr, nullptr))) { buffer->Release(); return false; }
    MFCopyImage(dst, m_width * 4, bgra, srcStride, m_width * 4, m_height);
    buffer->Unlock();
    buffer->SetCurrentLength(frameBytes);

    IMFSample* sample = nullptr;
    if (FAILED(MFCreateSample(&sample))) { buffer->Release(); return false; }
    sample->AddBuffer(buffer);

    // Real capture timestamps, so playback speed is correct even though frames
    // are produced on demand rather than at a fixed cadence.
    if (m_baseQpc == 0) m_baseQpc = qpc;
    const LONGLONG t = static_cast<LONGLONG>(
        ((qpc - m_baseQpc) * 10000000ULL) / static_cast<uint64_t>(m_qpcFreq));
    sample->SetSampleTime(t);
    sample->SetSampleDuration(10000000LL / m_fps);

    HRESULT hr = m_writer->WriteSample(m_stream, sample);
    sample->Release();
    buffer->Release();

    if (FAILED(hr))
    {
        printf("[rec] WriteSample failed: 0x%08lX\n", hr);
        return false;
    }
    ++m_frames;
    return true;
}

void Recorder::Finish()
{
    if (!m_writer) return;
    HRESULT hr = m_writer->Finalize();
    if (FAILED(hr)) printf("[rec] Finalize failed: 0x%08lX\n", hr);
    m_writer->Release();
    m_writer = nullptr;
    printf("[rec] Wrote %llu frames.\n", (unsigned long long)m_frames);
}

// ── main ─────────────────────────────────────────────────────────────────────

static volatile bool g_stop = false;
static BOOL WINAPI CtrlHandler(DWORD) { g_stop = true; return TRUE; }

int main(int argc, char* argv[])
{
    int      saveCount   = 0;
    char     saveDir[MAX_PATH] = ".\\";
    const char* recordPath = nullptr;
    uint32_t fps         = 30;
    uint32_t bitrateKbps = 8000;
    double   seconds     = 0.0;

    for (int i = 1; i < argc; ++i)
    {
        if (strcmp(argv[i], "--save") == 0 && i + 1 < argc)
        {
            saveCount = atoi(argv[++i]);
            if (i + 1 < argc && argv[i + 1][0] != '-')
                strncpy_s(saveDir, sizeof(saveDir), argv[++i], _TRUNCATE);
        }
        else if (strcmp(argv[i], "--record") == 0 && i + 1 < argc) recordPath = argv[++i];
        else if (strcmp(argv[i], "--fps") == 0 && i + 1 < argc) fps = (uint32_t)atoi(argv[++i]);
        else if (strcmp(argv[i], "--bitrate") == 0 && i + 1 < argc) bitrateKbps = (uint32_t)atoi(argv[++i]);
        else if (strcmp(argv[i], "--seconds") == 0 && i + 1 < argc) seconds = atof(argv[++i]);
    }
    if (fps == 0) fps = 30;
    int saved = 0;

    SetConsoleCtrlHandler(CtrlHandler, TRUE);

    // Open the shared objects created by the injected DLL.
    HANDLE hMap = OpenFileMappingA(FILE_MAP_READ, FALSE, SHM_NAME);
    if (!hMap) { printf("[reader] No shared memory yet — is the DLL injected?\n"); return 1; }

    const void* pView = MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, 0);
    if (!pView) { CloseHandle(hMap); return 1; }

    HANDLE hReady = OpenEventA(EVENT_ALL_ACCESS, FALSE, EVT_FRAME_READY);
    HANDLE hDone  = OpenEventA(EVENT_ALL_ACCESS, FALSE, EVT_FRAME_DONE);
    if (!hReady || !hDone)
    {
        printf("[reader] Could not open sync events.\n");
        return 1;
    }

    Recorder recorder;
    bool mfStarted = false;
    if (recordPath)
    {
        if (SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) &&
            SUCCEEDED(MFStartup(MF_VERSION)))
        {
            mfStarted = true;
        }
        else
        {
            printf("[reader] Media Foundation startup failed; not recording.\n");
            recordPath = nullptr;
        }
    }

    LARGE_INTEGER qpf = {}; QueryPerformanceFrequency(&qpf);
    LARGE_INTEGER startQpc = {}; QueryPerformanceCounter(&startQpc);

    // Announce readiness. The producer only captures while a reader is waiting,
    // and the "done" event is auto-reset, so a reader that connects after an
    // earlier one exited would otherwise wait forever for a frame that is never
    // produced.
    SetEvent(hDone);

    printf("[reader] Connected.  Consuming frames (Ctrl-C to stop)...\n");

    uint64_t lastIdx = UINT64_MAX;
    DWORD    frameIntervalMs = 1000 / fps;
    DWORD    nextDueTick = GetTickCount();

    while (!g_stop)
    {
        DWORD w = WaitForSingleObject(hReady, 2000);
        if (w == WAIT_TIMEOUT) { printf("[reader] Waiting...\n"); SetEvent(hDone); continue; }
        if (w != WAIT_OBJECT_0) break;

        const ShmHeader* hdr  = static_cast<const ShmHeader*>(pView);
        const uint8_t*   data = static_cast<const uint8_t*>(pView) + hdr->dataOffset;

        if (hdr->frameIdx != lastIdx)
        {
            lastIdx = hdr->frameIdx;

            if (recordPath && !recorder.Active())
            {
                if (!recorder.Start(recordPath, hdr->width, hdr->height, fps, bitrateKbps))
                    recordPath = nullptr;
            }
            if (recorder.Active())
                recorder.WriteFrame(data, hdr->stride, hdr->timestampQpc);

            if (saved < saveCount)
            {
                char path[MAX_PATH];
                _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\shm_%05llu.bmp",
                            saveDir, (unsigned long long)hdr->frameIdx);
                if (SaveBmp(path, data, hdr->width, hdr->height, hdr->stride))
                {
                    printf("[reader] saved %s\n", path);
                    ++saved;
                }
                else
                {
                    printf("[reader] FAILED to save %s\n", path);
                    saveCount = 0;
                }
            }

            if (!recorder.Active() && saveCount == 0)
                printf("[reader] Frame %6llu  |  %ux%u  fmt=%u  stride=%u\n",
                       (unsigned long long)hdr->frameIdx,
                       hdr->width, hdr->height, hdr->format, hdr->stride);

            //
            // ── INSERT YOUR PROCESSING HERE ───────────────────────────────────
            //
            // data  = pointer to top-left pixel (tightly-packed rows, 4 bpp)
            // hdr-> = metadata, including timestampQpc
            //
        }

        if (seconds > 0.0)
        {
            LARGE_INTEGER nowQpc = {}; QueryPerformanceCounter(&nowQpc);
            if (double(nowQpc.QuadPart - startQpc.QuadPart) / double(qpf.QuadPart) >= seconds)
                break;
        }

        // Pace the request for the next frame. The producer captures only when
        // it sees this signal, so holding it back throttles the game's readback
        // cost rather than merely discarding work already paid for.
        nextDueTick += frameIntervalMs;
        const DWORD nowTick = GetTickCount();
        if (static_cast<int>(nextDueTick - nowTick) > 0)
            Sleep(nextDueTick - nowTick);
        else
            nextDueTick = nowTick;    // fell behind; don't accumulate debt

        SetEvent(hDone);
    }

    if (recorder.Active()) recorder.Finish();
    if (mfStarted) { MFShutdown(); CoUninitialize(); }

    UnmapViewOfFile(pView);
    CloseHandle(hMap);
    CloseHandle(hReady);
    CloseHandle(hDone);
    return 0;
}
