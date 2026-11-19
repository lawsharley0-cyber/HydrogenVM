# Groove Zero 1.0.0 — Linux bench, 13 November 2026

**Blocked.** Taito's Groove Coaster Zero reaches past interface
enumeration after a general HydrogenVM fix, then stops at its mandatory
`http://gczero.com/start.php` handshake. The title menu is never
reached. Not recorded in `must_launch`.

## IPA

- Source: [Various Testflight Teraleak ipas](https://archive.org/details/various-testflightleak-ipas),
  `groove zero d33c1aaa876c114e919b25066665f118.ipa`
- SHA-256: `0d5bc908afe2a432dabe098f72483083421a06416dd3ca5e73b19c3abf1fd228`
  (SHA-1 matches archive.org: `d61528b9dda1ec4d9252098d884c14bfd9a6ac1e`;
  MD5 `fa989f5a8cb1f12c46e559e095a56c0e`; 26,424,760 bytes)
- Unpacked at `~/bench/apps/Groove_Zero_1.0.0` (not in the repo)
- Bundle `jp.co.taito.groovecoasterzero`, display name "Groove Zero",
  executable `Groove Zero`, CFBundleVersion 1.0.0, MinimumOSVersion 4.2,
  armv7 32-bit, not FairPlay-encrypted (`cryptid=0`), iPhone and iPad,
  `opengles-2`, iOS 5.1 SDK (Xcode 4E2002). Screen used here: 320×480 @2x.
- Custom engine (`.pak` skins/tunes, GLES 2 shaders, OpenAL + AudioToolbox
  `.m4a`, StoreKit / GameKit). Ad-hoc / TestFlight-style distribution.

## First stop: empty `SIOCGIFCONF` (fixed)

Fresh home, `./build/hvm run … --keep-going --run-for 30 --gpu --graceful`.
The process died at 0.18 s wall, 1 black frame, in

```
rfServerManager::GetMacAddress
  → ogStrcpy → strlen(NULL)
```

`NetworkInformation refresh` opens an `AF_INET`/`SOCK_DGRAM` socket and
issues Darwin `ioctl(SIOCGIFCONF)` (`_IOWR('i', 36, struct ifconf)` =
`0xc0086924` on 32-bit). HydrogenVM's `ioctl` only handled `FIONBIO` and
`FIONREAD`; every other request returned 0 without touching the
`ifconf`. The guest left `ifc_len` at its 4000-byte capacity, walked
uninitialized stack as ifreq records, found no `AF_INET`/`AF_LINK`
families, and logged `allInterfaces = { }`.
`primaryMACAddress` then returned nil, and `GetMacAddress` copied it
with `strlen`. A real iPhone always has at least `lo0` and `en0`.

The same class also calls `ether_ntoa` on each `AF_LINK` address.
That import was unimplemented. Darwin Libc (`net/ether_addr.c`) formats
with unpadded `%x:%x:%x:%x:%x:%x`, which this binary `sscanf`s.

### The fix (general, iOS-accurate)

- `ioctl(SIOCGIFCONF)` on a guest socket now reports `lo0` (AF_LINK
  loopback + AF_INET `127.0.0.1`) and `en0` (AF_LINK MAC
  `02:00:00:00:00:00`, the locally-administered address
  `sysctl(NET_RT_IFLIST)` already used, + AF_INET `10.0.0.2`). Packed
  as Darwin ifconf (`IFNAMSIZ + sa_len`). `lo0`'s `sockaddr_dl` is
  padded to 16 so walkers that use `max(sizeof(ifreq), IFNAMSIZ+sa_len)`
  — including this game — stay aligned. A NULL `ifc_buf` returns the
  size needed, as Darwin's `ifconf` does.
- `ether_ntoa` is implemented with Darwin's unpadded format, in a
  per-thread 18-byte buffer (same shape as `inet_ntoa`).

Fail-first tests in `tests/runner/TestRunner.cpp`: `ether_ntoa` of
`{0x02,0,0,0x0a,0x0b,0xcc}` was unregistered (red:
`symbol not registered`); `SIOCGIFCONF` returned 0 with `ifc_len`
still 1024 and no `lo0`/`en0` (red). After the fix, both are green.
`hvm_tests fulltest`: 824 passed, 0 failed (was 822 + these two).

### After the fix

```
PrimaryIPAddress[10.0.0.2]
PrimaryMacAddress[02:00:00:00:00:00]
ConvertURL="http://gczero.com/start.php?tid=020000000000&ver=1.0.0&lang=en&(C)TAITOCORP2012"
[ParseXMLFileAtURL] url="http://gczero.com/start.php?<hash>"
```

## Why it still stops: required Taito start.php

`WakeupConnection` → `ComWakeup` fetches that URL and
`TuneAppMain::AnalyzeStartXML`. The bundled `setting.cfg` is explicit:

```
[server]
serverUrl = "http://gczero.com/"
startPhp = "start.php"
```

plus `shop_list.php`, `sync.php`, `song_info.php`, `save.php`,
`load.php`, event/ranking/purchase endpoints. This is a free-to-play
client that cannot reach a title menu without the 2012 Taito backend.

HydrogenVM runs apps offline by design (`NSData` /
`NSXMLParser initWithContentsOfURL:` treat the http URL as a file;
the report lists `NSData /start.php: No such file or directory`).
`NSXMLParser parse` then returns YES on the empty body
(`parserError` is nil), and `AnalyzeStartXML` dereferences a null
`ogXmlTreeManager` (`bad memory access at 0x00000004`) at 0.12 s
wall, still one black frame, no audio. Opening outbound HTTP to a
vanished 2012 host, or inventing a `start.php` XML document, would
not be making the emulator match iOS.

## Reproduction

```sh
APP=~/bench/apps/Groove_Zero_1.0.0/Payload/Groove\ Zero.app
H=$(mktemp -d)
./build/hvm run "$APP" --home "$H" --keep-going --run-for 30 --gpu --graceful \
  --report r.json --screenshot s.png
# After this branch: PrimaryMacAddress is filled; crash is in AnalyzeStartXML.
```

## Still needed

A live `gczero.com` (or a captured, source-matched start.php transcript
used only under a documented server-replay policy) would be required
to continue playability work. This IPA is not an offline build. Set-3
continues at the owner's next title.

## Phone-only / not checked

No iPhone run. On a real device this IPA would get past `GetMacAddress`
and then depend on the same `http://gczero.com/start.php` endpoint.
Whether that host still serves the 2012 XML was not used as evidence.
Mesa vs device GPU was not reached. The first frame is black because
the game dies in `Initialize` before presenting a title.
