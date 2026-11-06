// Lyra — местоположение от службы Windows (Windows.Devices.Geolocation), в фоновом потоке.
#include "common/Platform.h"
#include "browser/GeoLocation.h"
#include "app/Log.h"

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Devices.Geolocation.h>
#pragma comment(lib, "windowsapp")

GeoFix GeoLocateNow(bool highAccuracy) {
    GeoFix fix;
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
    } catch (...) { }
    try {
        using namespace winrt::Windows::Devices::Geolocation;
        const auto access = Geolocator::RequestAccessAsync().get();
        if (access != GeolocationAccessStatus::Allowed) {
            Log("geolocation: Windows location access is %d", (int)access);
            return fix;
        }
        Geolocator locator;
        locator.DesiredAccuracy(highAccuracy ? PositionAccuracy::High : PositionAccuracy::Default);
        const auto position = locator.GetGeopositionAsync().get();
        const auto coordinate = position.Coordinate();
        const auto point = coordinate.Point().Position();
        fix.latitude = point.Latitude;
        fix.longitude = point.Longitude;
        fix.accuracy = coordinate.Accuracy();
        FILETIME ft;
        GetSystemTimeAsFileTime(&ft);
        ULARGE_INTEGER t;
        t.LowPart = ft.dwLowDateTime;
        t.HighPart = ft.dwHighDateTime;
        fix.timestamp = (double)(t.QuadPart - 116444736000000000ULL) / 1e7;   // seconds since 1970
        fix.ok = true;
    } catch (const winrt::hresult_error& e) {
        Log("geolocation: failed 0x%08x", (unsigned)e.code());
    } catch (...) {
        Log("geolocation: failed");
    }
    return fix;
}
