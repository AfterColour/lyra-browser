// Lyra — местоположение от службы Windows (Windows.Devices.Geolocation), в фоновом потоке.
#pragma once

struct GeoFix { bool ok = false; double latitude = 0, longitude = 0, accuracy = 0; double timestamp = 0; };

// Blocks for up to a few seconds (call it on a worker thread).
GeoFix GeoLocateNow(bool highAccuracy);
