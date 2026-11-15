// Lyra - small copies of big pictures for previews (see Previews.h).
#include "ui/Previews.h"
#include "app/Log.h"
#include "core/Constants.h"
#include "ui/MainWindow.h"
#include "ui/settings/SettingsWindow.h"
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

namespace {

constexpr int kWidth = 640;              // px; the largest tile is ~300 px at 150 %

std::mutex s_mutex;
std::set<std::wstring> s_pending;        // previews being made
std::set<std::wstring> s_failed;         // pictures GDI+ could not read (this session)
std::vector<std::pair<std::wstring, std::wstring>> s_queue;   // (picture, preview)
bool s_working = false;

ULONGLONG WriteTime(const std::wstring& path, bool* exists = nullptr) {
    WIN32_FILE_ATTRIBUTE_DATA a = {};
    const bool ok = GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a) != 0;
    if (exists) *exists = ok;
    return ok ? ((ULONGLONG)a.ftLastWriteTime.dwHighDateTime << 32) | a.ftLastWriteTime.dwLowDateTime : 0;
}

bool JpegEncoder(CLSID& clsid) {
    UINT count = 0, size = 0;
    if (GetImageEncodersSize(&count, &size) != Ok || !size) return false;
    std::vector<BYTE> buffer(size);
    auto* codecs = (ImageCodecInfo*)buffer.data();
    if (GetImageEncoders(count, size, codecs) != Ok) return false;
    for (UINT i = 0; i < count; ++i)
        if (wcscmp(codecs[i].MimeType, L"image/jpeg") == 0) { clsid = codecs[i].Clsid; return true; }
    return false;
}

bool Make(const std::wstring& picture, const std::wstring& preview) {
    Bitmap src(picture.c_str());
    if (src.GetLastStatus() != Ok || !src.GetWidth() || !src.GetHeight()) return false;
    const UINT w = std::min<UINT>(kWidth, src.GetWidth());
    const UINT h = std::max<UINT>(1, (UINT)((unsigned long long)src.GetHeight() * w / src.GetWidth()));
    Bitmap out(w, h, PixelFormat24bppRGB);
    {
        Graphics g(&out);
        g.Clear(Color(255, 255, 255, 255));
        g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
        g.SetPixelOffsetMode(PixelOffsetModeHighQuality);
        ImageAttributes edges;
        edges.SetWrapMode(WrapModeTileFlipXY);           // no grey seam along the borders
        g.DrawImage(&src, Rect(0, 0, (INT)w, (INT)h), 0, 0, (INT)src.GetWidth(), (INT)src.GetHeight(), UnitPixel, &edges);
    }
    CLSID jpeg;
    if (!JpegEncoder(jpeg)) return false;
    EncoderParameters params = {};
    ULONG quality = 82;
    params.Count = 1;
    params.Parameter[0].Guid = EncoderQuality;
    params.Parameter[0].Type = EncoderParameterValueTypeLong;
    params.Parameter[0].NumberOfValues = 1;
    params.Parameter[0].Value = &quality;
    const std::wstring tmp = preview + L".tmp";
    if (out.Save(tmp.c_str(), &jpeg, &params) != Ok) { DeleteFileW(tmp.c_str()); return false; }
    return MoveFileExW(tmp.c_str(), preview.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
}

void Work() {
    for (;;) {
        std::pair<std::wstring, std::wstring> job;
        {
            std::lock_guard<std::mutex> lock(s_mutex);
            if (s_queue.empty()) { s_working = false; break; }
            job = s_queue.back();
            s_queue.pop_back();
        }
        const ULONGLONG started = GetTickCount64();
        const bool ok = Make(job.first, job.second);
        Log("preview: %ls %s (%llu ms)", job.second.c_str(), ok ? "made" : "FAILED", GetTickCount64() - started);
        std::lock_guard<std::mutex> lock(s_mutex);
        s_pending.erase(job.second);
        if (!ok) s_failed.insert(job.first);
    }
    if (g_hWnd) PostMessageW(g_hWnd, WM_APP_PREVIEWS, 0, 0);
}

}  // namespace

std::wstring PreviewFor(const std::wstring& picture, const std::wstring& preview) {
    bool hasPicture = false, hasPreview = false;
    const ULONGLONG pictureTime = WriteTime(picture, &hasPicture);
    const ULONGLONG previewTime = WriteTime(preview, &hasPreview);
    if (!hasPicture) return L"";
    if (hasPreview && previewTime >= pictureTime) return preview;
    std::lock_guard<std::mutex> lock(s_mutex);
    if (s_failed.count(picture)) return picture;
    if (s_pending.insert(preview).second) s_queue.push_back({ picture, preview });
    if (!s_working) {
        s_working = true;
        std::thread(Work).detach();
    }
    return L"";
}

void PreviewsReady() { SettingsRefresh(); }
