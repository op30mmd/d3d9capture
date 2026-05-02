/**
 * shm_reader.cpp  —  Out-of-process frame consumer
 *
 * Reads frames written by consumer_backend.cpp via shared memory and signals
 * the producer when each frame has been consumed.  Replace the body of the
 * processing loop with your encoder, socket sender, or display logic.
 *
 * Build:
 *   cl /nologo /W3 /O2 /MT /Fe:shm_reader.exe shm_reader.cpp
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <cstdint>

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
    uint32_t dataOffset;
};
#pragma pack(pop)

int main()
{
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

    printf("[reader] Connected.  Consuming frames (Ctrl-C to stop)...\n");

    uint64_t lastIdx = UINT64_MAX;

    while (true)
    {
        // Block until the producer signals a new frame is ready.
        DWORD w = WaitForSingleObject(hReady, 2000);
        if (w == WAIT_TIMEOUT) { printf("[reader] Waiting...\n"); continue; }
        if (w != WAIT_OBJECT_0) break;

        const ShmHeader* hdr  = static_cast<const ShmHeader*>(pView);
        const uint8_t*   data = static_cast<const uint8_t*>(pView)
                                + hdr->dataOffset;

        if (hdr->frameIdx != lastIdx)
        {
            lastIdx = hdr->frameIdx;

            printf("[reader] Frame %6llu  |  %ux%u  fmt=%u  stride=%u\n",
                   (unsigned long long)hdr->frameIdx,
                   hdr->width, hdr->height, hdr->format, hdr->stride);

            //
            // ── INSERT YOUR PROCESSING HERE ───────────────────────────────────
            //
            // data  = pointer to top-left pixel (tightly-packed rows, 4 bpp)
            // hdr-> = metadata
            //
            // Examples:
            //   encode_nvenc(data, hdr->width, hdr->height);
            //   send_udp(data, hdr->width * hdr->height * 4);
            //   display_opengl_texture(data, hdr->width, hdr->height);
            //
        }

        // Signal the producer we're done so it can overwrite the buffer.
        SetEvent(hDone);
    }

    UnmapViewOfFile(pView);
    CloseHandle(hMap);
    CloseHandle(hReady);
    CloseHandle(hDone);
    return 0;
}
