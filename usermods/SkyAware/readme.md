# SkyAware (METAR → Flight Category → LED Color)

A WLED usermod that fetches METAR data for a single airport at a user-defined interval and sets LEDs to a color by **flight category**:

- **VFR** → Green
- **MVFR** → Blue
- **IFR** → Red
- **LIFR** → Magenta
- **Unknown/Error** → Warm White

PoC behavior: overrides **Segment 0** with a static color on each successful fetch (or warm white on error). Simple by design—easy to refine later (e.g., tint only, target a specific segment, MQTT publish, etc.).

## Configuration (Config → Usermods)

- **Airport ID**  
  ICAO, e.g. `KPDX`. Default: `KPDX`.

- **Update Frequency (min)**  
  Poll interval in minutes (minimum 1). Default: `5`.

## Notes

- Uses `HTTPS` with `WiFiClientSecure::setInsecure()` for a quick PoC (no cert pinning).
- Uses AviationWeather.gov endpoint:  
  `https://aviationweather.gov/api/data/metar?format=json&ids=<AIRPORT>`
- Non-reentrant loop; won’t spam requests.
- Requires ESP32 build of WLED.

## Install

1. Place this folder under `usermods/skyaware/`.
2. Build & flash WLED for ESP32 (WLED’s PlatformIO already includes `usermods` via `lib_extra_dirs`).
3. Open WLED → **Config → Usermods** and set **Airport ID** and **Update Frequency (min)**.
