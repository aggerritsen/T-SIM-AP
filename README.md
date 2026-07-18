# T-SIM-AP

ESP-IDF firmware for a LilyGO T-SIM7080G-S3 style board that exposes a WiFi access point and routes clients over the SIM7080 cellular modem using PPP, IPv4 forwarding, NAPT, and a small DNS proxy.

## Current Behavior

- WiFi AP SSID: `AP-<last 3 bytes of AP MAC>`, for example `AP-A5BE6D`
- WiFi password: `tsim7080`
- AP address: `192.168.4.1`
- DHCP client range: ESP-IDF default SoftAP DHCP range
- DNS for clients: `192.168.4.1`, proxied to upstream DNS over PPP
- Cellular path: SIM7080 PPP dial using `ATD*99***1#`
- NAT: enabled on the AP interface after PPP receives an IP
- Current modem data baud: `115200`

This is intended for low-bandwidth IoT clients such as a WiFi thermostat. Heavy browser pages and speedtest sites may be very slow on CAT-M1.

## Requirements

- ESP-IDF 5.5.1 installed under `C:\Espressif\frameworks\esp-idf-v5.5.1`
- ESP-IDF tools under the normal user tools path, for example `C:\Users\<user>\.espressif`
- ESP32-S3 target
- Board connected on a serial port, currently tested on `COM7`

## Build And Flash

Open PowerShell in this repo:

```powershell
. .\idf-env.ps1
idf.py set-target esp32s3
idf.py build
idf.py -p COM7 flash monitor
```

Quit the monitor with `Ctrl+]`.

The helper scripts default to `COM7` and accept another port if needed:

```powershell
.\build.ps1
.\flash.ps1 -Port COM9
```

## Configuration

Main firmware variables live in [src/config.h](src/config.h).

AP settings:

```cpp
static constexpr ApConfig AP_CONFIG = {
    "AP",
    "tsim7080",
    6,
    4,
};
```

SIM/APN profiles currently include:

- Onomondo: `onomondo`
- KPNThings: `internet.m2m`
- ThingsData/Tele2 2G-4G: `m2m.tele2.com`
- ThingsData/Tele2 5G: `iot.tele2.com`

Tested SIM compatibility:

- KPN Things works with APN `internet.m2m`.
- Lebara Netherlands registers and establishes PPP, but browser DNS traffic did not work reliably in testing.

The modem config currently uses:

```cpp
data_baud = 115200
fallback_apn = "internet.m2m"
```

The firmware also probes common baud rates during boot to recover a modem left at a previous test speed.

## Expected Serial Milestones

A healthy boot should show lines like:

```text
AP: ssid=AP-A5BE6D password=tsim7080 ip=192.168.4.1 dns=192.168.4.1
DNS: proxy listening on 192.168.4.1:53
MODEM: responding at 115200
MODEM: IMSI=... supplier=... apn=internet.m2m
MODEM: CEREG raw ... +CEREG: 2,1,...
PPP: modem CONNECT, starting netif
PPP: got IP ...
ROUTER: NAT enabled on AP 192.168.4.1
ROUTER: AP=... PPP=... NAT=on
```

Client connection:

```text
DHCP server assigned IP to a client, IP is: 192.168.4.2
```

## Troubleshooting

- `MODEM: AT timeout` repeatedly: the modem may be on another baud. Recovery probes `115200`, configured baud, `460800`, and `921600`.
- `+CME ERROR` after `AT+CNACT=0,0`: acceptable when no old data context is active.
- `AT+COPS=0` reports `+CME ERROR`: acceptable if registration still follows shortly after, as shown by `+CEREG: ...,1,...` or `+CEREG: ...,5,...`.
- `AT+CIMI` reports `+CME ERROR`: the firmware falls back to the configured APN and can still connect if registration and PPP succeed.
- `DNS: upstream timeout count=... errno=11`: one or more DNS queries timed out. Phone browsers can generate bursts of lookups over a slow CAT-M1 link, so these are rate-limited informational summaries instead of one warning per query. IoT clients usually make far fewer queries.
- Phone says connected but no internet: wait until serial shows `PPP connected` and `NAT enabled`, then reconnect the phone to refresh DHCP/DNS.

## Repo Notes

This is a native ESP-IDF project. PlatformIO files and generated build output are intentionally not tracked.
