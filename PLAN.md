# ESP32-S3 Network MultiTool — merge plan

One ESP32-S3 N16R8 (16 MB flash, 8 MB OPI PSRAM) running all three existing projects:

| Source project | Version | What it brings |
|---|---|---|
| `ESP32 S3 N16R8 - AdBlocker` | 1.0.3 | DNS sinkhole + DoT, blocklists in PSRAM, query log, dashboard, local DNS, LED, scheduler |
| `ESP32 WOL-NTFY` | 2.0.1 | Wake-on-LAN over ntfy, up to 16 saved computers |
| `esp8266 site check` (ESP32S3-N16R8 build) | 2.0.0 | Up to 30 HTTP/HTTPS/ping monitors, down/recovery alerts, daily digest |

**Progress (2026-11-23):** phases 1–4 and 6 are done and released as 1.0.0 (457 host
checks). On hardware so far: an in-place upgrade of an AdBlocker board and a clean install,
with boot, blocking, DoT, ntfy and the web pages checked. Phase 5, the 48-hour soak test with
all three features busy, has **not** been run.

---

## 1. What the survey found

**The three projects already share most of their shape.** AdBlocker and WOL-NTFY have the same
`ntfy.cpp` (only comments and User-Agent differ), the same NVS `Settings` pattern, the same
setup portal / AP fallback / BOOT-button recovery, the same passphrase rules and the same host
test harness (`test/shim`). SiteCheck follows the same ideas but in an older style: `.ino`
tabs, settings in `/config.json` on LittleFS, and ntfy by **polling** every 60 s instead of a
stream.

**AdBlocker is the right base.** It is the newest and largest, already owns the hard parts
(PSRAM layout, FreeRTOS tasks, scheduler, LED, multi-page web UI, custom partition table), and
already notes how tight internal RAM is.

**Flash and PSRAM are not the problem.** App sizes today: AdBlocker 1.32 MB, SiteCheck
1.17 MB, WOL 1.11 MB — most of each is the same ESP32 core, WiFi and mbedTLS. The merged app
should land around 1.4–1.5 MB in AdBlocker's 4 MB app slot. WOL and SiteCheck need only a few
KB of RAM for their host lists.

**Internal RAM for TLS is the real constraint.** From the AdBlocker build's `sdkconfig`:

```
CONFIG_MBEDTLS_INTERNAL_MEM_ALLOC=y   # TLS buffers forced into internal RAM, never PSRAM
CONFIG_LWIP_MAX_SOCKETS=16            # socket ceiling, compiled into the core
```

AdBlocker runs with about **109 KB internal heap free**, and each TLS session needs roughly
35–45 KB. It already juggles this: DoT steps aside during the blocklist download. The merge
adds more TLS: SiteCheck's HTTPS probes (up to 30 per pass) and more ntfy publishes (alerts,
digest). Both are settings fixed in the prebuilt Arduino core, so `arduino-cli` cannot change
them; the firmware has to schedule around them (section 3).

**Licensing.** AdBlocker is AGPL-3.0 (it ports s60sc/ESP32_AdBlocker); the other two are
Apache-2.0. Apache-2.0 code can go into a GPLv3-family work, so the combined firmware must be
**AGPL-3.0**, keeping Apache notices on files that came from WOL-NTFY and SiteCheck.

---

## 2. Target architecture

```
                     ┌──────────────── main loop (core 1) ────────────────┐
                     │ webconfig::tick   ntfy::poll (stream)   scheduler  │
                     │ drain events from all modules → ntfy outbox        │
                     └──────────▲──────────────▲───────────────▲──────────┘
                                │ events       │ commands      │ events
  ┌─────────────────────────────┴───┐   ┌──────┴──────┐  ┌─────┴────────────────┐
  │ adblock                         │   │ wol         │  │ sitecheck            │
  │  DNS UDP/TCP tasks (existing)   │   │ sync, fast  │  │ NEW task: one host   │
  │  blocklist loader task          │   │ NVS store   │  │ at a time, ping/HTTP │
  │  DoT proxy task                 │   │             │  │ hosts.json on FS     │
  └─────────────────────────────────┘   └─────────────┘  └──────────────────────┘
                                │                               │
                        ┌───────┴───────────────────────────────┴───────┐
                        │ tlsgate: who may open a TLS session, and when │
                        └───────────────────────────────────────────────┘
```

### Modules

- **core** — `MultiTool.ino`, `config.h`, `settings`, `webconfig`, `ntfy`, `commands`
  (router), `scheduler`, `led`, `certs.h`, `util.h`, new `tlsgate`.
- **adblock** — the existing files unchanged in behaviour: `blocklist`, `blparse`, `sinkhole`,
  `dnscache`, `dnsmsg`, `dot`, `querylog`, `localdns`, `localrec`.
- **wol** — `wol.cpp` and `store.cpp` → `wolstore.cpp`. Wakes stay synchronous in the command
  handler (about 200 ms per host).
- **sitecheck** — `checks` → `sitecheck.cpp`, `storage` → `sitestore.cpp`, `report` →
  `sitereport.cpp`. Its own ntfy poller, `/config.json` settings and setup portal are
  **dropped**; it uses the shared ones.

Layout: flat sketch folder with module prefixes (`ab_*`, `wol_*`, `site_*`), which keeps
`arduino-cli` happy and the existing `#include "x.h"` lines short. (`src/<module>/` also works
with arduino-cli if you prefer folders.)

### Why SiteCheck gets its own task

Its loop does blocking work: an 8 s HTTP timeout, HEAD-then-GET retry, ICMP ping. In
AdBlocker's main loop that would freeze the web UI and the ntfy stream for up to ~16 s per
host. A low-priority task (8 KB stack, core 0 or 1, below the DNS tasks) does one host at a
time and posts results to an event queue, the same way `blocklist::takeEvent()` works today.
The main loop turns those events into ntfy messages.

---

## 3. TLS and socket budget (the key new piece)

New `tlsgate` module that every TLS user goes through:

| Session | Kind | Rule |
|---|---|---|
| ntfy subscribe stream | permanent | always allowed; highest priority |
| DoT upstream | permanent, optional | steps aside whenever a transient job needs room (generalises today's blocklist-download rule); plain DNS covers the gap |
| ntfy publish | transient | through an **outbox** in PSRAM, drained by the main loop when the slot is free, so a 16 s site check never stalls replies |
| SiteCheck HTTPS probe | transient | one at a time; skipped/deferred while the blocklist is downloading |
| Blocklist download | transient, long (~20 s) | holds the slot; site passes wait for it |

Mechanics: one FreeRTOS mutex = "the transient TLS slot", plus a check of
`heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)` before each connect. If the block is too
small, DoT is asked to disconnect, then the connect retries.

**Sockets (16 max) — counted in phase 2.** Only the BSD socket API counts against
`LWIP_MAX_SOCKETS`. DNS over UDP (sinkhole receive and upstream relay), SNTP, the lwIP
resolver and mDNS all use raw lwIP PCBs and cost nothing.

| User | Sockets at worst |
|---|---|
| DNS TCP listeners (v4 + v6) | 2 |
| DNS TCP clients (`DNS_TCP_CLIENTS`) | 3 |
| `tcpExchange` (truncated reply retried over TCP) | 1 |
| `sinkhole::resolves` (typo check on `block`) | 1 |
| DoT link | 1 |
| ntfy subscribe stream | 1 |
| Short-lived TLS: ntfy publish / blocklist download / site HTTPS probe (one at a time via `tlsgate`) | 1 |
| Site plain-HTTP probe or ping (phase 4; one at a time in the site task) | 1 |
| Web server listener + one client | 2 |
| WOL magic-packet UDP (phase 3, transient) | 1 |
| **Total** | **14 of 16** |

The captive-portal DNS server (1) only runs in AP mode, when the DoT link, ntfy stream and
site checks are down. So `DNS_TCP_CLIENTS` stays at 3 for now. The phase 5 soak test checks
for `socket()` failures in the log.

**Built in phase 2:** `tlsgate` (FreeRTOS mutex; DoT and the ntfy stream hold it only for the
handshake; publishes hold it per POST; the blocklist download holds it with DoT forced off) and
the ntfy outbox (16 × 3 KB slots in PSRAM, allocated before the blocklist arena; `publish()`
queues, the main loop sends one per pass, 3 tries 5 s apart; `flush()` before every restart).

**Alternative, not recommended for now:** build with ESP-IDF + Arduino as a component and set
`CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC` so TLS lives in PSRAM. That removes the limit but changes
the whole build system and makes TLS slower. Worth revisiting only if the gate proves
insufficient in the soak test.

---

## 4. Settings (one NVS `Settings` struct)

Start from AdBlocker's struct and **keep its NVS namespace**, so an existing AdBlocker board
upgrades in place with its settings and blocklist snapshot intact. Add:

| New field | From | Default |
|---|---|---|
| `adblockOn`, `wolOn`, `siteOn` | new | all on; a module that is off starts no task and binds no port |
| `hostname` (mDNS) | SiteCheck | `multitool` |
| `siteIntervalMin` | SiteCheck | 30 |
| `digestHour`, `digestMinute` | SiteCheck | 08:00 |
| `alertTopic` | new, optional | blank = same topic as commands |

Dropped as duplicates: SiteCheck's `ntfyBase`, `cmdSecret`, `adminPass`, `tz`, reboot time,
poll interval (the stream replaces polling). One passphrase, one admin login, one daily reboot
(04:00), one timezone.

Data stays where it lives now: WOL computers as a JSON blob in NVS (own namespace), monitored
sites in `/sites.json` on the same LittleFS partition as the blocklist snapshot.

---

## 5. ntfy commands (one passphrase, one router)

Three verbs collide: `status`, `check` (AdBlocker: is a domain blocked; SiteCheck: run checks)
and `add`/`remove` (WOL computers vs SiteCheck hosts). Proposal: keep the unambiguous verbs
exactly as they are today, and put the colliding ones behind a module word.

| Command | Does |
|---|---|
| `help`, `status` | combined: one section per enabled module |
| `stats`, `top`, `check <domain>`, `block`, `unblock`/`allow`, `on`, `off`, `reload`, `clearcustom` | AdBlocker, unchanged |
| `wake <name>` / `wake all`, `list` | WOL, unchanged |
| `wol add <name> <mac>`, `wol rm <name>` | WOL add/remove |
| `site add <name> <target>`, `site rm <name>`, `site list`, `site check` | SiteCheck; `site add` also accepts the old batch form `A\|t1;B\|t2` |
| `reboot`, `factoryreset` | shared |

Same safety rules as all three today: no passphrase → silent; state-changing commands require a
passphrase to be set; replies never echo it; own messages ignored via `NTFY_SELF_TAG`
(`multitool`).

---

## 6. Web UI

AdBlocker's pages stay (Status, Dashboard, Query log, Local DNS, Settings), plus:

- **Wake** — saved computers with a Wake button each, add/remove form.
- **Sites** — monitored hosts with state and last change, "check now", add/remove.
- **Status** — one combined report; **Settings** — grouped by module, with the module on/off
  switches.

Setup portal SSID becomes `MultiTool-Setup-XXXX`. mDNS `http://multitool.local` from SiteCheck
is kept.

---

## 7. Schedule (local time)

| Time | Job | Note |
|---|---|---|
| 03:00 | blocklist refresh | holds the TLS slot ~20 s; site pass waits |
| 04:00 | daily reboot | blocklist restored from snapshot in seconds |
| 08:00 | site digest | via outbox |
| every 30 min | site pass | first pass 30 s after boot |

---

## 8. Phases

1. **Scaffold** — copy AdBlocker into this folder, rename (app name, AP SSID, self-tag,
   User-Agent), set up `git`, build, flash, and confirm it behaves exactly like AdBlocker 1.0.3.
   Run the host tests.
2. **Core refactor** — command router with per-module handlers, extended `Settings`,
   `tlsgate`, ntfy outbox, socket count. No new features yet; re-test AdBlocker.
3. **WOL** — port `wol`/`store`, commands, Wake page, port `test_store`/`test_commands` cases.
4. **SiteCheck** — task, `sites.json`, probe code (HTTP/HTTPS via `HTTPClient`, ping via
   ESPping), alerts, digest, commands, Sites page, tests for target parsing and batch `add`.
5. **Integration and soak** — 48 h run with ~25 sites, DoT on, Hagezi Pro loaded; log minimum
   internal heap and largest free block; run `tools/dnsload.py` during a site pass and during the
   03:00 download; confirm no lost ntfy replies.
6. **Docs and release** — README, `flash.md`, AGPL `LICENSE` + Apache notices, `dist/` bins
   (plain and `-usbcdc`), version 1.0.0.

**Done means:** all three feature sets work on one board, DNS load numbers within ~10% of
AdBlocker 1.0.3, internal heap never under ~30 KB during the soak, no watchdog resets.

## 9. Build

```
arduino-cli compile --fqbn "esp32:esp32:esp32s3:FlashSize=16M,PSRAM=opi,PartitionScheme=custom" --export-binaries MultiTool
```

Libraries: ArduinoJson, ESPping. Partition table: AdBlocker's (4 MB app, ~11.9 MB LittleFS,
no OTA) — unchanged so an AdBlocker board keeps its snapshot.

## 10. Risks

| Risk | Mitigation |
|---|---|
| TLS out of memory during overlap | `tlsgate`, DoT step-aside, soak test; IDF build as fallback |
| 16-socket ceiling | count in phase 2; trim DNS TCP clients |
| Slow site probe starving DNS | site task below DNS task priority; DNS path never waits on it |
| `.lan` site names won't resolve (device uses public upstreams) | probe resolves through the local DNS records / router first |
| A WOL or SiteCheck board's saved hosts don't carry over | re-add with `wol add` / batch `site add` (one message each) |

## 11. Decisions to confirm before coding

1. **Command style** — module word only on the colliding verbs (above), or on every command
   (`dns block`, `wol wake`, `site add`)?
2. **Alerts topic** — one topic for everything, or a separate (optional) topic for site-down
   alerts and the digest?
3. **Upgrade path** — keep AdBlocker's NVS namespace and partition table so an AdBlocker board
   upgrades in place (recommended), or start clean?
4. **OTA** — leave it out (as all three are today), or split flash into two app slots
   (~3.5 MB each) and shrink LittleFS to ~8 MB to allow web updates?
