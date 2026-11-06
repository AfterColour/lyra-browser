// Lyra — плашка Windows «Сейчас играет» и медиа-клавиши (System Media Transport Controls).
#include "browser/SystemMedia.h"
#include "app/Log.h"
#include "core/Constants.h"

#include <systemmediatransportcontrolsinterop.h>
#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Media.h>
#include <winrt/Windows.Storage.Streams.h>
#pragma comment(lib, "windowsapp")

using namespace winrt::Windows::Media;

namespace {
SystemMediaTransportControls s_smtc{ nullptr };
winrt::event_token s_token{};
HWND s_window = nullptr;
std::wstring s_artwork;
}

bool SystemMediaInit(HWND window) {
    s_window = window;
    try {
        auto interop = winrt::get_activation_factory<SystemMediaTransportControls, ISystemMediaTransportControlsInterop>();
        SystemMediaTransportControls smtc{ nullptr };
        winrt::check_hresult(interop->GetForWindow(window, winrt::guid_of<SystemMediaTransportControls>(), winrt::put_abi(smtc)));
        s_smtc = smtc;
        s_smtc.IsEnabled(false);
        s_smtc.IsPlayEnabled(true);
        s_smtc.IsPauseEnabled(true);
        s_smtc.IsStopEnabled(true);
        s_token = s_smtc.ButtonPressed([](const SystemMediaTransportControls&,
                                         const SystemMediaTransportControlsButtonPressedEventArgs& args) {
            // A worker thread: hand the button to the main window.
            PostMessageW(s_window, WM_APP_MEDIA, (WPARAM)args.Button(), 0);
        });
        return true;
    } catch (const winrt::hresult_error& e) {
        Log("media: SMTC unavailable 0x%08x", (unsigned)e.code());
        s_smtc = nullptr;
        return false;
    }
}

void SystemMediaShow(const SystemMediaInfo& info) {
    if (!s_smtc) return;
    try {
        s_smtc.IsNextEnabled(info.canNext);
        s_smtc.IsPreviousEnabled(info.canPrevious);
        auto updater = s_smtc.DisplayUpdater();
        updater.Type(MediaPlaybackType::Music);
        auto music = updater.MusicProperties();
        music.Title(winrt::hstring(info.title));
        music.Artist(winrt::hstring(info.artist));
        music.AlbumTitle(winrt::hstring(info.album));
        if (info.artworkUrl != s_artwork) {
            s_artwork = info.artworkUrl;
            if (s_artwork.compare(0, 4, L"http") == 0) {
                try {
                    updater.Thumbnail(winrt::Windows::Storage::Streams::RandomAccessStreamReference::CreateFromUri(
                        winrt::Windows::Foundation::Uri(winrt::hstring(s_artwork))));
                } catch (...) { updater.Thumbnail(nullptr); }
            } else {
                updater.Thumbnail(nullptr);
            }
        }
        updater.Update();
        s_smtc.PlaybackStatus(info.playing ? MediaPlaybackStatus::Playing : MediaPlaybackStatus::Paused);
        s_smtc.IsEnabled(true);
    } catch (const winrt::hresult_error& e) {
        Log("media: SMTC update failed 0x%08x", (unsigned)e.code());
    }
}

void SystemMediaHide() {
    if (!s_smtc) return;
    try {
        s_artwork.clear();
        s_smtc.PlaybackStatus(MediaPlaybackStatus::Closed);
        s_smtc.DisplayUpdater().ClearAll();
        s_smtc.IsEnabled(false);
    } catch (...) { }
}

void SystemMediaShutdown() {
    if (!s_smtc) return;
    SystemMediaHide();
    try { s_smtc.ButtonPressed(s_token); } catch (...) { }
    s_smtc = nullptr;
}
