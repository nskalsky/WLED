# SkyWizard Usermod for WLED

**SkyWizard** is a lightweight [WLED](https://github.com/Aircoookie/WLED) usermod that adds a **first-run captive wizard** for your device.  
It’s designed for projects like **SkyAware** that need one extra configuration step before normal WLED use.

---

## Features

- Shows a one-time **wizard page** at `/wizard`.
- Lets the user enter an **optional “Home Airport”** code (ICAO/IATA, e.g., `KPDX`).
- Redirects all traffic to the wizard until:
  1. The airport has been saved **and**
  2. The device is connected to Wi-Fi.
- After setup, the device runs as normal with the full WLED UI.
- Saves configuration to `cfg.json` under:

  ```json
  "um": {
    "SkyWizard": {
      "homeAirport": "KPDX",
      "wizardSaved": true
    }
  }
