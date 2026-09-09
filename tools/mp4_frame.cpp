/**
 * mp4_frame.cpp — decode frames from an MP4 to BMP, using Media Foundation.
 *
 * Verification tool. There is no way to trust a recorded video without looking
 * at a decoded frame: a vertical flip or a red/blue swap produces a file that
 * plays perfectly and is still wrong. This is the other half of the check that
 * tools/d3d9_testapp.cpp's top-left marker exists for.
 *
 * Build:
 *   cl /nologo /W3 /O2 /MT /Fe:mp4_frame.exe mp4_frame.cpp
 *
 * Usage:
 *   mp4_frame.exe <in.mp4> <out-prefix> [count] [skip]
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <cstdint>
#include <cstdlib>

#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "ole32.lib")

// Writes a top-down BGRA buffer as a bottom-up 32-bpp BMP.
static bool SaveBmp(const char* path, const uint8_t* pixels,
                    uint32_t width, uint32_t height, int32_t stride)
{
    FILE* f = nullptr;
    if (fopen_s(&f, path, "wb") != 0 || !f) return false;

    const uint32_t rowBytes = width * 4;
    BITMAPFILEHEADER fh = {};
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
    fh.bfSize = fh.bfOffBits + rowBytes * height;

    BITMAPINFOHEADER ih = {};
    ih.biSize = sizeof(ih);
    ih.biWidth = static_cast<LONG>(width);
    ih.biHeight = static_cast<LONG>(height);   // positive: bottom-up
    ih.biPlanes = 1;
    ih.biBitCount = 32;
    ih.biCompression = BI_RGB;
    ih.biSizeImage = rowBytes * height;

    fwrite(&fh, sizeof(fh), 1, f);
    fwrite(&ih, sizeof(ih), 1, f);
    for (int y = static_cast<int>(height) - 1; y >= 0; --y)
        fwrite(pixels + static_cast<ptrdiff_t>(y) * stride, rowBytes, 1, f);

    fclose(f);
    return true;
}

int main(int argc, char* argv[])
{
    if (argc < 3)
    {
        printf("Usage: mp4_frame.exe <in.mp4> <out-prefix> [count] [skip]\n");
        return 1;
    }
    const char* inPath = argv[1];
    const char* prefix = argv[2];
    const int   count  = (argc > 3) ? atoi(argv[3]) : 1;
    const int   skip   = (argc > 4) ? atoi(argv[4]) : 0;

    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) return 1;
    if (FAILED(MFStartup(MF_VERSION))) return 1;

    wchar_t wpath[MAX_PATH] = {};
    MultiByteToWideChar(CP_ACP, 0, inPath, -1, wpath, MAX_PATH);

    // Without the video processor the reader only offers the decoder's native
    // output (NV12), and asking for RGB32 fails with MF_E_INVALIDMEDIATYPE.
    IMFAttributes* attrs = nullptr;
    MFCreateAttributes(&attrs, 1);
    attrs->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);

    IMFSourceReader* reader = nullptr;
    HRESULT hr = MFCreateSourceReaderFromURL(wpath, attrs, &reader);
    if (attrs) attrs->Release();
    if (FAILED(hr)) { printf("MFCreateSourceReaderFromURL failed: 0x%08lX\n", hr); return 1; }

    // Ask the reader to decode to BGRA for us.
    IMFMediaType* want = nullptr;
    MFCreateMediaType(&want);
    want->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    want->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
    hr = reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, want);
    want->Release();
    if (FAILED(hr)) { printf("SetCurrentMediaType failed: 0x%08lX\n", hr); return 1; }

    IMFMediaType* actual = nullptr;
    UINT32 width = 0, height = 0;
    if (SUCCEEDED(reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, &actual)))
    {
        MFGetAttributeSize(actual, MF_MT_FRAME_SIZE, &width, &height);
        actual->Release();
    }
    printf("[mp4] %ux%u\n", width, height);

    int emitted = 0, index = 0;
    while (emitted < count)
    {
        DWORD flags = 0;
        LONGLONG ts = 0;
        IMFSample* sample = nullptr;
        hr = reader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0,
                                nullptr, &flags, &ts, &sample);
        if (FAILED(hr)) { printf("[mp4] ReadSample failed: 0x%08lX\n", hr); break; }
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) { printf("[mp4] end of stream\n"); break; }
        if (!sample) continue;

        if (index++ >= skip)
        {
            IMFMediaBuffer* buffer = nullptr;
            if (SUCCEEDED(sample->ConvertToContiguousBuffer(&buffer)) && buffer)
            {
                BYTE* data = nullptr; DWORD len = 0;
                if (SUCCEEDED(buffer->Lock(&data, nullptr, &len)))
                {
                    // The decoder hands back RGB32 in the GDI bottom-up
                    // convention, so read rows from the bottom to recover a
                    // top-down image before writing it back out.
                    const int32_t stride = static_cast<int32_t>(width) * 4;
                    char path[MAX_PATH];
                    _snprintf_s(path, sizeof(path), _TRUNCATE, "%s_%03d.bmp", prefix, emitted);
                    if (SaveBmp(path, data, width, height, stride))
                        printf("[mp4] %s  (t=%.3fs)\n", path, double(ts) / 1e7);
                    ++emitted;
                    buffer->Unlock();
                }
                buffer->Release();
            }
        }
        sample->Release();
    }

    reader->Release();
    MFShutdown();
    CoUninitialize();
    return 0;
}
