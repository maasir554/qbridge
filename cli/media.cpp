#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include "qbridge/media.h"

#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <objbase.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

static volatile LONG stop_requested;

template <typename T>
static void release(T *&object)
{
    if (object) {
        object->Release();
        object = nullptr;
    }
}

static void print_hresult(const char *operation, HRESULT hr)
{
    std::fprintf(stderr, "%s failed: HRESULT 0x%08lx\n", operation,
                 static_cast<unsigned long>(hr));
}

static HRESULT select_mjpeg_type(IMFSourceReader *reader, UINT32 width,
                                 UINT32 height, UINT32 fps)
{
    for (DWORD index = 0;; ++index) {
        IMFMediaType *type = nullptr;
        GUID subtype{};
        UINT32 candidate_width = 0;
        UINT32 candidate_height = 0;
        UINT32 numerator = 0;
        UINT32 denominator = 0;
        HRESULT hr = reader->GetNativeMediaType(
            MF_SOURCE_READER_FIRST_VIDEO_STREAM, index, &type);

        if (hr == MF_E_NO_MORE_TYPES) {
            break;
        }
        if (FAILED(hr)) {
            release(type);
            return hr;
        }

        if (SUCCEEDED(type->GetGUID(MF_MT_SUBTYPE, &subtype)) &&
            subtype == MFVideoFormat_MJPG &&
            SUCCEEDED(MFGetAttributeSize(type, MF_MT_FRAME_SIZE,
                                         &candidate_width,
                                         &candidate_height)) &&
            candidate_width == width && candidate_height == height) {
            MFGetAttributeRatio(type, MF_MT_FRAME_RATE, &numerator,
                                &denominator);
            if (!fps || !denominator || numerator / denominator == fps) {
                hr = reader->SetCurrentMediaType(
                    MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, type);
                release(type);
                return hr;
            }
        }
        release(type);
    }
    return MF_E_INVALIDMEDIATYPE;
}

static HRESULT open_camera(UINT32 camera_index, UINT32 width, UINT32 height,
                           UINT32 fps, IMFSourceReader **out_reader,
                           IMFMediaSource **out_source,
                           std::wstring *friendly_name)
{
    IMFAttributes *attributes = nullptr;
    IMFActivate **devices = nullptr;
    IMFMediaSource *source = nullptr;
    IMFAttributes *reader_attributes = nullptr;
    UINT32 count = 0;
    HRESULT hr = MFCreateAttributes(&attributes, 1);

    if (SUCCEEDED(hr)) {
        hr = attributes->SetGUID(
            MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
            MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    }
    if (SUCCEEDED(hr)) {
        hr = MFEnumDeviceSources(attributes, &devices, &count);
    }
    if (SUCCEEDED(hr) && camera_index >= count) {
        std::fprintf(stderr, "camera index %u does not exist; found %u camera(s)\n",
                     camera_index, count);
        hr = MF_E_NOT_FOUND;
    }
    if (SUCCEEDED(hr)) {
        WCHAR *name = nullptr;
        UINT32 name_length = 0;
        if (SUCCEEDED(devices[camera_index]->GetAllocatedString(
                MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &name, &name_length))) {
            *friendly_name = name;
            CoTaskMemFree(name);
        }
        hr = devices[camera_index]->ActivateObject(
            __uuidof(IMFMediaSource), reinterpret_cast<void **>(&source));
    }
    if (SUCCEEDED(hr)) {
        hr = MFCreateAttributes(&reader_attributes, 2);
    }
    if (SUCCEEDED(hr)) {
        hr = reader_attributes->SetUINT32(
            MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, FALSE);
    }
    if (SUCCEEDED(hr)) {
        hr = reader_attributes->SetUINT32(
            MF_READWRITE_DISABLE_CONVERTERS, TRUE);
    }
    if (SUCCEEDED(hr)) {
        hr = MFCreateSourceReaderFromMediaSource(source, reader_attributes,
                                                  out_reader);
    }
    if (SUCCEEDED(hr)) {
        hr = select_mjpeg_type(*out_reader, width, height, fps);
    }

    if (SUCCEEDED(hr)) {
        *out_source = source;
        source = nullptr;
    }
    release(source);
    release(reader_attributes);
    if (devices) {
        for (UINT32 i = 0; i < count; ++i) {
            release(devices[i]);
        }
        CoTaskMemFree(devices);
    }
    release(attributes);
    return hr;
}

static bool publish_sample(qb_media_frame *shared, IMFSample *sample,
                           UINT32 width, UINT32 height, uint64_t frame_number,
                           const char *snapshot_path, bool *snapshot_written)
{
    IMFMediaBuffer *buffer = nullptr;
    BYTE *bytes = nullptr;
    DWORD maximum = 0;
    DWORD length = 0;
    LONGLONG timestamp = 0;
    HRESULT hr = sample->ConvertToContiguousBuffer(&buffer);

    if (SUCCEEDED(hr)) {
        hr = buffer->Lock(&bytes, &maximum, &length);
    }
    if (FAILED(hr)) {
        release(buffer);
        return false;
    }
    sample->GetSampleTime(&timestamp);
    const bool jpeg = length >= 4 && bytes[0] == 0xff && bytes[1] == 0xd8 &&
                      bytes[length - 2] == 0xff && bytes[length - 1] == 0xd9;
    if (!jpeg || length > QB_MEDIA_MAX_FRAME) {
        buffer->Unlock();
        release(buffer);
        return false;
    }

    InterlockedIncrement(reinterpret_cast<volatile LONG *>(&shared->sequence));
    MemoryBarrier();
    shared->magic = QB_MEDIA_MAGIC;
    shared->version = QB_MEDIA_VERSION;
    shared->header_size = offsetof(qb_media_frame, data);
    shared->frame_size = length;
    shared->width = width;
    shared->height = height;
    shared->fourcc = static_cast<uint32_t>('M') |
                     (static_cast<uint32_t>('J') << 8) |
                     (static_cast<uint32_t>('P') << 16) |
                     (static_cast<uint32_t>('G') << 24);
    shared->timestamp_100ns = static_cast<uint64_t>(timestamp);
    shared->frame_number = frame_number;
    std::memcpy(shared->data, bytes, length);
    MemoryBarrier();
    InterlockedIncrement(reinterpret_cast<volatile LONG *>(&shared->sequence));

    if (snapshot_path && !*snapshot_written) {
        if (FILE *snapshot = std::fopen(snapshot_path, "wb")) {
            *snapshot_written =
                std::fwrite(bytes, 1, length, snapshot) == length;
            std::fclose(snapshot);
        }
    }

    buffer->Unlock();
    release(buffer);
    return true;
}

static BOOL WINAPI console_handler(DWORD event)
{
    if (event == CTRL_C_EVENT || event == CTRL_BREAK_EVENT ||
        event == CTRL_CLOSE_EVENT) {
        InterlockedExchange(&stop_requested, 1);
        return TRUE;
    }
    return FALSE;
}

int main(int argc, char **argv)
{
    UINT32 camera_index = 0;
    UINT32 width = 640;
    UINT32 height = 480;
    UINT32 fps = 30;
    uint64_t maximum_frames = 0;
    const char *log_path = nullptr;
    const char *snapshot_path = nullptr;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--camera") && i + 1 < argc) {
            camera_index = static_cast<UINT32>(std::strtoul(argv[++i], nullptr, 10));
        } else if (!std::strcmp(argv[i], "--width") && i + 1 < argc) {
            width = static_cast<UINT32>(std::strtoul(argv[++i], nullptr, 10));
        } else if (!std::strcmp(argv[i], "--height") && i + 1 < argc) {
            height = static_cast<UINT32>(std::strtoul(argv[++i], nullptr, 10));
        } else if (!std::strcmp(argv[i], "--fps") && i + 1 < argc) {
            fps = static_cast<UINT32>(std::strtoul(argv[++i], nullptr, 10));
        } else if (!std::strcmp(argv[i], "--frames") && i + 1 < argc) {
            maximum_frames = std::strtoull(argv[++i], nullptr, 10);
        } else if (!std::strcmp(argv[i], "--log") && i + 1 < argc) {
            log_path = argv[++i];
        } else if (!std::strcmp(argv[i], "--snapshot") && i + 1 < argc) {
            snapshot_path = argv[++i];
        } else {
            std::fprintf(stderr,
                         "usage: qbridge-media [--camera INDEX] "
                         "[--width N] [--height N] [--fps N] [--frames N] "
                         "[--log PATH] [--snapshot PATH]\n");
            return 2;
        }
    }
    if (log_path) {
        if (!std::freopen(log_path, "w", stdout) ||
            !std::freopen(log_path, "a", stderr)) {
            return 2;
        }
    }

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr)) {
        print_hresult("CoInitializeEx", hr);
        return 1;
    }
    hr = MFStartup(MF_VERSION);
    if (FAILED(hr)) {
        print_hresult("MFStartup", hr);
        CoUninitialize();
        return 1;
    }

    IMFSourceReader *reader = nullptr;
    IMFMediaSource *source = nullptr;
    std::wstring camera_name;
    hr = open_camera(camera_index, width, height, fps, &reader, &source,
                     &camera_name);
    if (FAILED(hr)) {
        print_hresult("opening MJPEG camera", hr);
        MFShutdown();
        CoUninitialize();
        return 1;
    }

    HANDLE mapping = CreateFileMappingW(
        INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
        static_cast<DWORD>(sizeof(qb_media_frame)), L"Local\\QBridgeCamera");
    if (!mapping) {
        std::fprintf(stderr, "CreateFileMapping failed: %lu\n", GetLastError());
        release(reader);
        source->Shutdown();
        release(source);
        MFShutdown();
        CoUninitialize();
        return 1;
    }
    auto *shared = static_cast<qb_media_frame *>(
        MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0,
                      sizeof(qb_media_frame)));
    if (!shared) {
        std::fprintf(stderr, "MapViewOfFile failed: %lu\n", GetLastError());
        CloseHandle(mapping);
        release(reader);
        source->Shutdown();
        release(source);
        MFShutdown();
        CoUninitialize();
        return 1;
    }
    std::memset(shared, 0, sizeof(*shared));
    shared->magic = QB_MEDIA_MAGIC;
    shared->version = QB_MEDIA_VERSION;
    shared->header_size = offsetof(qb_media_frame, data);

    std::fwprintf(stdout, L"capturing \"%ls\" as %ux%u MJPEG at %u fps\n",
                  camera_name.c_str(), width, height, fps);
    std::printf("publishing frames through %s; press Ctrl+C to stop\n",
                QB_MEDIA_MAPPING_NAME);
    std::fflush(stdout);
    SetConsoleCtrlHandler(console_handler, TRUE);

    uint64_t frame_number = 0;
    bool snapshot_written = false;
    auto last_report = std::chrono::steady_clock::now();
    while (!InterlockedCompareExchange(&stop_requested, 0, 0)) {
        DWORD stream_index = 0;
        DWORD flags = 0;
        LONGLONG timestamp = 0;
        IMFSample *sample = nullptr;
        hr = reader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0,
                                &stream_index, &flags, &timestamp, &sample);
        if (FAILED(hr)) {
            print_hresult("ReadSample", hr);
            break;
        }
        if (flags & MF_SOURCE_READERF_ERROR) {
            std::fprintf(stderr, "camera source reported an error\n");
            release(sample);
            break;
        }
        if (sample && publish_sample(shared, sample, width, height,
                                     frame_number + 1, snapshot_path,
                                     &snapshot_written)) {
            ++frame_number;
            if (maximum_frames && frame_number >= maximum_frames) {
                release(sample);
                break;
            }
        }
        release(sample);

        const auto now = std::chrono::steady_clock::now();
        if (now - last_report >= std::chrono::seconds(1)) {
            std::printf("frames=%llu latest=%u bytes\n",
                        static_cast<unsigned long long>(frame_number),
                        shared->frame_size);
            std::fflush(stdout);
            last_report = now;
        }
    }

    UnmapViewOfFile(shared);
    CloseHandle(mapping);
    release(reader);
    source->Shutdown();
    release(source);
    MFShutdown();
    CoUninitialize();
    return FAILED(hr) ? 1 : 0;
}
