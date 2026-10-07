# FIXME: SIM7080 registration fix

The change that made cellular registration start working was applying the modem radio mode before operator registration:

```text
AT+CNMP=38
AT+CMNB=1
```

In code this came from making `MODEM_CONFIG.preferred_radio_mode = "CAT-M"` actually do something. `apply_preferred_radio_mode()` now maps `CAT-M` / `LTE-M` to `CMNB=1`, sets LTE-only mode with `CNMP=38`, and runs those commands before `AT+COPS=0` and the `+CEREG?` registration loop.

Before that, the firmware left the SIM7080 in whatever radio-access mode it already had stored internally. With the failing SIM, the modem repeatedly showed states like:

```text
+CPSI: NO SERVICE,Online
+CEREG: 2,6
+CEREG: 2,2
```

That means the SIM was ready, but the modem was not getting normal packet registration. For this board/SIM/network combination, forcing LTE CAT-M made the modem scan/register on the usable radio mode instead of drifting through an unsuitable or stale modem setting.

Do not confuse this with the later ICCID/APN parser fix. That second fix made `AT+CCID` parse plain numeric ICCIDs, so `894620...` SIMs select `ThingsData/Tele2 2G-4G` and `m2m.tele2.com` instead of fallback `internet.m2m`. The first connection breakthrough was the radio-mode fix; the APN fix made SIM profile selection correct and repeatable.
