# ESP32-S3 Network MultiTool

**English** | [繁體中文](README.zh-HK.md)

Three home-network jobs on one ESP32-S3 N16R8 board, all reporting to and controlled from your
phone over [ntfy](https://ntfy.sh):

| Feature | What it does | From |
|---|---|---|
| **Ad blocker** | Pi-hole-style DNS blocking for every device on the network, with DNS-over-TLS upstream | ESP32 AdBlocker 1.0.3 |
| **Wake-on-LAN** | wakes your computers from anywhere: ntfy carries the command in, the board sends the magic packet on the LAN | ESP32 WOL-NTFY 2.0.1 |
| **Site monitor** | checks up to 30 web sites and devices by HTTP(S) or ping, alerts when one goes down or comes back, sends a daily digest | SiteCheck 2.0.0 |

Each feature can be switched off on the settings page; a feature that is off uses no memory and
ignores its commands.

> **Status: 1.0.0-rc1.** It compiles and passes 457 host-side checks, but the merged
> firmware has not yet had its 48-hour soak test on hardware (see [PLAN.md](PLAN.md),
> phase 5). Expect rough edges until then.

Example reply to `sekret status` (numbers are illustrative):

```
ESP32 Network MultiTool v1.0.0-rc1
ip 192.168.0.254 (static)
gw 192.168.0.1
wifi home (-57 dBm)
settings http://192.168.0.254
up 2h 5m, last reset: daily reboot
time 2026-11-23 06:05 PST
next reboot 04:00 PST
ntfy connected
heap 104 KB, psram 132 KB free

-- ad blocker --
upstream DNS-over-TLS, connected to one.one.one.one; fallback 1.1.1.1 / 1.0.0.1
blocklist 236,195 domains, downloaded 3h 5m ago (from flash)
blocking on
queries 4,812, blocked 1,377 (28%), cached 1,906
list refresh 03:00

-- wake-on-lan --
computers 3/16

-- site monitor --
12 sites: 11 up, 1 down; next check in 21 min; digest 08:00
  DOWN nas
```

## Hardware

- **ESP32-S3 N16R8**: 16 MB flash, 8 MB octal PSRAM. The PSRAM is required: the blocklist,
  DNS cache, query log, ntfy outbox and site list live there. Tested on
  ESP32-S3-DevKitC-1-style boards.
- **2.4 GHz WiFi.** The ESP32 has no 5 GHz radio.
- **A fixed IP address** (static, or a DHCP reservation in your router) if you use the ad
  blocker, because every device sends its DNS queries to it.
- **The same network segment** as the computers you want to wake. Magic packets are
  broadcasts and do not cross routers or VLANs.

## Quick start

### 1. Flash

| File | Use it when |
|---|---|
| `multitool-v1.0.0-rc1-esp32s3-n16r8.bin` | first install, through the board's **UART** port or a USB-serial adapter |
| `multitool-v1.0.0-rc1-esp32s3-n16r8-usbcdc.bin` | first install, through the board's **native USB** port |
| `…-app.bin` / `…-usbcdc-app.bin` | updating a board that already runs MultiTool **or ESP32 AdBlocker**. Keeps settings and the blocklist |

```bash
pip install esptool
```

```bash
esptool --port /dev/cu.usbserial-XXXX --baud 921600 write-flash 0x0 multitool-v1.0.0-rc1-esp32s3-n16r8.bin
```

[flash.md](flash.md) has the update command, port names, checksums and how to build it
yourself.

### 2. Set it up

1. Join the WiFi network **`MultiTool-Setup-XXXX`** from your phone. **Turn mobile data off
   first**, or the phone may leave a network with no internet before the page loads.
2. The setup page opens by itself, or go to **http://192.168.4.1**.
3. Fill in WiFi, an ntfy topic and two passwords, then tap **Save & restart**.

| Section | Fields |
|---|---|
| WiFi | network (from a live scan), password, device name (`multitool` → http://multitool.local) |
| Features | ad blocker, Wake-on-LAN, site monitor: on or off |
| Address | static IP, gateway, subnet; all blank for DHCP |
| Upstream DNS | plain servers, DNS-over-TLS and its server, local names to the router, IPv6 |
| Blocklist | up to 4 URLs, daily refresh hour, subdomain blocking, Firefox / iCloud switch-off |
| Daily reboot | on/off, time, timezone |
| Site monitor | minutes between checks (5–1440), daily digest time |
| Status LED | GPIO (0 = none), WS2812 or single colour, brightness |
| ntfy | server, port, TLS, topic, optional alerts topic, optional login or token |
| Passwords | a command passphrase for ntfy, and an admin password for the web pages (user `admin`) |

**Use a long, random topic name.** On public ntfy.sh the topic name is the only thing keeping
strangers from reading your messages. The passphrase stops them sending commands.

### 3. It reports in

The topic gets `MultiTool online` with the status report above. With the ad blocker on, the
first boot downloads the blocklist in the background (about 20 seconds for the default list)
and a second message reports the result.

### 4. Point your network at it (ad blocker)

In your router's DHCP settings, make the board's IP the **only** DNS server. A second one such
as `8.8.8.8` lets devices skip the blocker. Test first from a computer:

```bash
dig @192.168.0.254 doubleclick.net
```

`0.0.0.0` means it's blocking.

## ntfy commands

Every command starts with your passphrase: `sekret wake desktop`. A message without the right
passphrase gets no reply at all, and no reply ever repeats the passphrase. Commands that change
something are refused until a passphrase is set.

### Whole device

| Command | Aliases | What it does |
|---|---|---|
| `help` | `?`, `commands` | the commands for the features that are on |
| `status` | `stat` | the report above |
| `reboot` | `restart` | restart now |
| `factoryreset` | | wipe settings, reboot into setup. Keeps the blocklist, computers and sites |
| `factoryreset all` | | also forgets saved computers and monitored sites |

### Ad blocker

| Command | Aliases | What it does |
|---|---|---|
| `stats` | | counts and the last 10 blocked names |
| `top` | | most blocked names and busiest devices |
| `check <domain>` | | is it blocked, and by which entry |
| `block <domain>` | | add to your own list (it must resolve, which catches typos) |
| `unblock <domain>` | `allow` | let it through, even when a parent is blocked |
| `on` / `off` | | pause blocking until `on` or the next reboot |
| `reload` | `refresh` | download the blocklists now |
| `clearcustom` | | forget everything you blocked or allowed |

### Wake-on-LAN

| Command | Aliases | What it does |
|---|---|---|
| `wake <name>` | `wol <name>` | send the magic packet; `wake all` for every computer |
| `list` | `ls`, `wol list` | saved computers |
| `wol add <name> <mac>` | `wol save` | remember a computer (up to 16) |
| `wol rm <name>` | `remove`, `del`, `delete`, `forget` | forget one |

MACs can be `aa:bb:cc:dd:ee:ff`, `aa-bb-cc-dd-ee-ff` or `aabbccddeeff`. Names are up to 20
characters of `a-z 0-9 _ -`, case-insensitive, and can't be a command word (`list`, `all` …).
Nothing acknowledges a magic packet, so "sent" means sent, not woken.

### Site monitor

| Command | Aliases | What it does |
|---|---|---|
| `site list` | `ls`, `status` | every site and its state, plus when the next check runs |
| `site add <name> <target>` | `save` | monitor a site (up to 30) |
| `site add A\|target1;B\|target2` | | several at once; names may contain spaces |
| `site rm <name>` | `remove`, `del`, `delete` | stop monitoring; `site rm A;B` for several |
| `site check` | | check every site now |

A **target** is:

| You type | Checked as |
|---|---|
| `192.168.1.50` | ping, 2 echoes, any reply is up |
| `http://nas.lan:8080/health` or `https://…` | exactly that URL |
| `immich.example.com`, `nas` | `https://<name>` |

HTTP and HTTPS checks send `HEAD`, then `GET` if that fails, and count **2xx and 3xx as up**.
Redirects are not followed, and certificates are not checked (an expired certificate is still
"up"). Local names (`nas`, `printer.lan`, `*.home.arpa`) are looked up in your Local DNS records
first, then at your router.

SiteCheck's own syntax still works after the word `site`: `site ADD|Name|target;…` and
`site add | Name | target`. Plain `add` or `remove` without `wol` or `site` gets a reply asking
which list you meant.

### Site alerts

| Event | Title | Priority |
|---|---|---|
| a site goes down (or is down on its first check) | `DOWN: <name>` | 4 (high) |
| it comes back | `Recovered: <name>` | 3 |
| daily digest | `Daily status` or `Daily status - 2 down` | 2, or 3 if anything is down |

No change means no message. A site that is up on its first check after a boot or an add is not
announced. Set an **alerts topic** in Settings to send these (and only these) to a second topic,
so they can ring differently on your phone.

## Web pages

Browse to the board's IP or http://multitool.local and log in as `admin`. Pages for a feature
that is off are hidden.

| Page | What's on it |
|---|---|
| **Status** | the status report; ad-blocker check, block, unblock, reload, on/off; reboot |
| **Dashboard** | DNS totals, a 24-hour chart, top blocked, top allowed, top clients |
| **Query log** | the last 6,000 DNS queries, filterable, with block / allow on every row |
| **Local DNS** | your own `IP name` records (also used by the site monitor) |
| **Wake** | saved computers with Wake and Forget buttons, Wake all, add form |
| **Sites** | every site with its state, how long, last result; check now; add / remove |
| **Settings** | everything from the setup form |

## Status LED

The on-board WS2812 is on **GPIO 48** on ESP32-S3-DevKitC-1 v1.0 and **GPIO 38** on v1.1.

| Colour | Pattern | Meaning |
|---|---|---|
| Yellow | slow blink | setup portal, or WiFi down |
| Green | steady | running normally (with the ad blocker: blocking) |
| Blue | medium blink | downloading the blocklist |
| Purple | fast flashing | last blocklist download couldn't reach the internet |
| Teal | very slow blink | last download failed; still blocking with the previous list |
| Red | fast blink | no blocklist at all |

With the ad blocker off, only yellow and green are used.

## How it fits on one board

The three original firmwares were each comfortable on their own. Together, the limit is not
flash (1.4 MB of a 4 MB app slot) or PSRAM, but the S3's **internal RAM**, which is where every
TLS connection's buffers must live (the prebuilt Arduino core fixes that). With the ad blocker
running there are about 100 KB free and each TLS connection needs 35–45 KB. So:

- **One TLS handshake at a time.** A small gate (`tlsgate.cpp`) serialises every handshake:
  the ntfy stream, DNS-over-TLS, ntfy messages, the blocklist download and HTTPS site checks.
- **DNS-over-TLS steps aside** when a short connection needs room, and always during the ~20 s
  daily blocklist download. Lookups use plain DNS for those seconds.
- **Messages queue.** ntfy messages go into a 16-slot outbox in PSRAM and are sent one at a time
  from the main loop, with three tries. A slow site check never delays a command's reply by more
  than one send.
- **Site checks run in their own low-priority task**, one site at a time, below the DNS tasks.
  An HTTPS check during the 03:00 blocklist download waits for it to finish.
- **Sockets:** at most 14 of the core's fixed 16 are in use at once.

[PLAN.md](PLAN.md) has the full design and the socket count.

## Ad blocker notes

Everything in the ESP32 AdBlocker README still applies; the essentials:

- **Lists:** up to 4 merged lists (HOSTS, Adblock `||domain^` or plain), about 250,000
  domains in all. Combined lists past that are cut short, and the load message says
  `TRUNCATED`. Sizes as of November 2026 (they change daily):

  | List | Domains | Notes |
  |---|---|---|
  | **Hagezi Multi PRO** (default) `…/adblock/pro.txt` | ~236,000 | strong, few false positives; loads in full on the board |
  | Hagezi Multi PRO++ `…/adblock/pro.plus.txt` | ~249,000 | more aggressive, occasionally breaks a site; close to the limit |
  | Hagezi Multi NORMAL `…/adblock/multi.txt` | ~180,000 | gentler |
  | StevenBlack unified `https://raw.githubusercontent.com/StevenBlack/hosts/master/hosts` | ~76,000 | conservative |
  | Hagezi encrypted-DNS bypass `…/adblock/doh.txt` | ~3,600 | add as a second line next to any of the above |

  `…` is `https://cdn.jsdelivr.net/gh/hagezi/dns-blocklists@latest`. Recommended: PRO plus
  `doh.txt`.
- **Matching:** subdomains go with their parent (needed for Hagezi lists); the most specific
  entry wins, so `unblock cdn.example.com` works under a blocked `example.com`; CNAME-cloaked
  trackers are caught; a leading `www.` is ignored.
- **Local names** (`printer`, `*.lan`, `*.home`, `*.home.arpa`, `*.internal`, private reverse
  lookups) go to your router; `.local` is never forwarded.
- **What no DNS blocker can stop:** YouTube ads, sponsored posts in Facebook / Instagram /
  TikTok, and anything served from a site's own domain. Smart TVs, apps and games see the
  biggest difference.
- **Bypasses to close:** in Chrome, set "Use secure DNS" to "With your current service
  provider" or turn it off (Firefox's equivalent and iCloud Private Relay are switched off
  automatically), leave Android Private DNS on Automatic, and if your
  router allows, block outgoing ports 53 and 853 from everything except the board. The Hagezi
  `doh.txt` list (above) blocks public encrypted-DNS servers.
- **Capacity:** about 1–10 people or 50–80 devices. Measure with `tools/dnsload.py` (only on
  your own network).

## Daily schedule (local time)

| Time | Job |
|---|---|
| 03:00 | blocklist refresh (about 20 s; DoT and HTTPS site checks wait) |
| 04:00 | daily reboot (a few seconds of DNS outage; the blocklist is restored from flash in ~2 s) |
| 08:00 | site digest |
| every 30 min | site check pass, starting 30 s after the board comes online |

All configurable. Avoid 01:00–03:00 for the reboot: daylight-saving changes skip or repeat that
hour.

## Changing settings later

| Situation | What to do |
|---|---|
| Board is online | browse to its IP (or http://multitool.local), log in as `admin` |
| WiFi is down | it opens `MultiTool-Setup-XXXX` (http://192.168.4.1, admin login) and keeps retrying every 10 s |
| Moved house, or forgot the admin password | press **EN**, release, then **hold BOOT** for 2 s: the setup portal opens with no login |
| From anywhere | `<passphrase> factoryreset` over ntfy |

If it won't join WiFi, the serial log (115200 baud) lists every 2.4 GHz network in range and
says whether yours is among them.

## Coming from the separate firmwares

| From | What carries over |
|---|---|
| **ESP32 AdBlocker** on the same board | everything: flash the `-app.bin`. Settings, blocklist, custom list and local DNS records stay. The new settings take their defaults (all features on) |
| **ESP32 WOL-NTFY** | re-add computers with `wol add <name> <mac>`, one message each |
| **SiteCheck** | re-add sites in one message: `site add Name1\|target1;Name2\|target2;…` (the same list you once sent it as `ADD\|…`). Its `/hosts.json` format is also read as-is from `/sites.json`, if you have a copy |

The command passphrase, admin password and ntfy topic are set once, for all three features.

## Building

[arduino-cli](https://arduino.github.io/arduino-cli/) with the ESP32 core 3.1.1 or newer
(built with 3.3.10) and two libraries:

```bash
arduino-cli core install esp32:esp32
```

```bash
arduino-cli lib install ArduinoJson ESPping
```

```bash
arduino-cli compile --fqbn "esp32:esp32:esp32s3:FlashSize=16M,PSRAM=opi,PartitionScheme=custom" --export-binaries MultiTool
```

Add `,CDCOnBoot=cdc` for a board on its native USB port. `PartitionScheme=custom` uses
[`MultiTool/partitions.csv`](MultiTool/partitions.csv): a 4 MB app slot and 12 MB of LittleFS
(the same layout as ESP32 AdBlocker, so an AdBlocker board upgrades in place). There is no
over-the-air update.

### Tests

457 native checks need no board: DNS packet handling, blocklist parsing, settings and their
validation, reboot timing across DST changes, LED states, local DNS, the Wake-on-LAN store, the
site monitor's target rules / URL parsing / alert rules / digest timing / stored format, and
every ntfy command with each feature on and off, including the passphrase gate.

```bash
./test/run.sh
```

The runner needs ArduinoJson's sources; it looks in `~/Documents/Arduino/libraries/ArduinoJson/src`
or `$ARDUINOJSON_SRC`.

### Source map

| File | Role |
|---|---|
| `MultiTool.ino` | boot flow; setup / fallback / online states; PSRAM order |
| `commands.cpp`, `cmdutil.h` | ntfy command router: passphrase gate, help, status, reboot, factoryreset |
| `adblock_cmd.cpp`, `wol_cmd.cpp`, `site_cmd.cpp` | each feature's commands |
| `tlsgate.cpp` | one TLS handshake at a time; asks DoT to step aside |
| `ntfy.cpp` | ntfy stream and the outbox |
| `settings.cpp`, `config.h` | settings in NVS; tunables |
| `webconfig.cpp` | setup portal and every web page; mDNS |
| `scheduler.cpp`, `schedule.h` | daily reboot, daily blocklist refresh, reset reason |
| `led.cpp`, `ledstate.h` | status LED |
| `sinkhole.cpp`, `dnsmsg.h`, `dnscache.cpp`, `dot.cpp` | DNS server, wire format, cache, DNS-over-TLS |
| `blocklist.cpp`, `blparse.h`, `querylog.cpp`, `localdns.cpp`, `localrec.h` | blocklist, query log, local DNS |
| `wol.cpp`, `wolstore.cpp` | magic packets; saved computers |
| `sitelogic.h`, `sitestore.cpp`, `sitecheck.cpp` | site rules; site list; the check task, alerts and digest |
| `tools/dnsload.py` | DNS load tester (runs on your computer) |

## Credits and license

- The blocklist engine and original DNS responder come from
  [s60sc/ESP32_AdBlocker](https://github.com/s60sc/ESP32_AdBlocker) (s60sc and dateno1), under
  the AGPL-3.0. The combined firmware is therefore released under the
  **[AGPL-3.0](LICENSE)**. If you run a modified version as a network service, you must offer
  its source to its users.
- The Wake-on-LAN and site-monitor code comes from ESP32 WOL-NTFY and SiteCheck, both
  Apache-2.0. Their notices are in [NOTICE](NOTICE) and the licence text in
  [LICENSE-APACHE-2.0.txt](LICENSE-APACHE-2.0.txt).
- Default blocklist: [Hagezi DNS blocklists](https://github.com/hagezi/dns-blocklists). Ping:
  [ESPping](https://github.com/dvarrel/ESPping).
