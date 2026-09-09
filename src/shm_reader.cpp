/**
 * shm_reader.cpp  —  Out-of-process frame consumer
 *
 * Reads frames written by consumer_backend.cpp via shared memory and signals
 * the producer when each frame has been consumed.  Replace the body of the
 * processing loop with your encoder, socket sender, or display logic.
 *
 * Build:
 *   cl /nologo /W3 /O2 /MT /Fe:shm_reader.exe shm_reader.cpp
 *
 * Usage:
 *   shm_reader.exe [--save N [dir]]
 *
 * --save writes the next N delivered frames as BMPs, which is how you confirm
 * that what arrives over shared memory is the game's actual output rather than
 * a black or stale buffer.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>

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

int main(int argc, char* argv[])
{
    int  saveCount = 0;
    char saveDir[MAX_PATH] = ".\\";
    for (int i = 1; i < argc; ++i)
    {
        if (strcmp(argv[i], "--save") == 0 && i + 1 < argc)
        {
            saveCount = atoi(argv[++i]);
            if (i + 1 < argc && argv[i + 1][0] != '-')
                strncpy_s(saveDir, sizeof(saveDir), argv[++i], _TRUNCATE);
        }
    }
    int saved = 0;

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
