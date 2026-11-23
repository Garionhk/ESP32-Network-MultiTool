// Site checks. The schedule, alert rules and digest come from SiteCheck 2.0.0
// (Apache-2.0); the probe is new: it resolves the name itself, so local names
// (nas.lan, printer) work even though this device resolves through public
// servers, and it speaks just enough HTTP to read the status line.

#include <Arduino.h>
#include <ESPping.h>
#include <NetworkClientSecure.h>
#include <WiFi.h>
#include <esp_attr.h>
#include <lwip/sockets.h>
#include <time.h>

#include "config.h"
#include "dnsmsg.h"
#include "localdns.h"
#include "ntfy.h"
#include "settings.h"
#include "sitecheck.h"
#include "sitestore.h"
#include "tlsgate.h"

using sitelogic::Site;

namespace sitecheck {
namespace {

TaskHandle_t      task = nullptr;
volatile bool     requested = false;
volatile bool     running = false;
volatile uint32_t nextPassMs = 0;

// RTC memory survives the daily reboot, so a crash inside the digest hour
// cannot send the digest twice.
const uint32_t DIGEST_MAGIC = 0x51DE0D16;
RTC_NOINIT_ATTR uint32_t digestMagic;
RTC_NOINIT_ATTR int32_t  digestYday;

bool clockValid() { return time(nullptr) > (time_t)NTP_MIN_VALID_EPOCH; }
bool online() { return WiFi.status() == WL_CONNECTED; }

// ---- name resolution --------------------------------------------------------

// One A query to `server` over UDP. Same approach as sinkhole::resolves().
bool queryA(uint32_t server, const char* host, IPAddress& ip) {
  uint8_t q[300], r[DNS_MAX_PACKET];
  uint16_t qid = (uint16_t)esp_random();
  int qlen = dnsmsg::makeQuery(q, sizeof(q), qid, host, dnsmsg::T_A);
  if (!qlen) return false;

  int fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) return false;
  struct timeval tv = {1, 500 * 1000};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  struct sockaddr_in to = {};
  to.sin_family = AF_INET;
  to.sin_port = htons(DNS_PORT);
  to.sin_addr.s_addr = server;
  bool ok = false;
  uint8_t a[4];
  if (sendto(fd, q, qlen, 0, (struct sockaddr*)&to, sizeof(to)) == qlen) {
    int n = recv(fd, r, sizeof(r), 0);
    ok = n >= dnsmsg::HEADER && dnsmsg::id(r) == qid && dnsmsg::rcode(r) == dnsmsg::RC_OK &&
         dnsmsg::firstA(r, n, a);
  }
  close(fd);
  if (ok) ip = IPAddress(a[0], a[1], a[2], a[3]);
  return ok;
}

// Public names through lwIP (dns1 / dns2). Local names through your local DNS
// records first, when the ad blocker is on, then the router.
bool resolve(const String& host, IPAddress& ip) {
  if (ip.fromString(host)) return true;
  if (sitelogic::isLocalName(host)) {
    uint8_t a[4];
    if (g_settings.adblockOn && localdns::lookup(host.c_str(), a)) {
      ip = IPAddress(a[0], a[1], a[2], a[3]);
      return true;
    }
    return queryA((uint32_t)WiFi.gatewayIP(), host.c_str(), ip);
  }
  return WiFi.hostByName(host.c_str(), ip) == 1;
}

// ---- probes -----------------------------------------------------------------

// Reads one line, sleeping between polls. Stream::readStringUntil() spins on
// read() without ever yielding, and this task runs on core 0 above the idle
// task: a server that accepts the connection but answers slowly starved
// IDLE0 for the whole timeout, and the 5 s task watchdog rebooted the board.
String readLine(NetworkClient* c, uint32_t timeoutMs) {
  String line;
  uint32_t start = millis();
  while (millis() - start < timeoutMs) {
    int a = c->available();
    if (a > 0) {
      while (a-- > 0) {
        int ch = c->read();
        if (ch < 0) break;
        if (ch == '\n') return line;
        if (line.length() < 256) line += (char)ch;
      }
      continue;
    }
    if (!c->connected()) break;
    vTaskDelay(pdMS_TO_TICKS(10));
  }
  return line;
}

// One request on a fresh connection; returns the status code, or <= 0:
//   0 connect failed, -1 no / bad status line.
int request(bool tls, const IPAddress& ip, const String& host, uint16_t port,
            const String& path, const char* method) {
  NetworkClient* c;
  if (tls) {
    NetworkClientSecure* s = new NetworkClientSecure();
    s->setInsecure();   // up/down only: no certificate check, by design (as SiteCheck)
    s->setHandshakeTimeout(SITE_HTTP_TIMEOUT_MS / 1000);
    c = s;
  } else {
    c = new NetworkClient();
  }
  c->setTimeout(SITE_HTTP_TIMEOUT_MS);   // Stream timeout, milliseconds

  bool connected;
  if (tls) {
    // The host name goes in as SNI, so name-based virtual hosts answer.
    connected = ((NetworkClientSecure*)c)->connect(ip, port, host.c_str(), nullptr, nullptr,
                                                   nullptr) == 1;
  } else {
    connected = c->connect(ip, port, (int32_t)SITE_HTTP_TIMEOUT_MS) == 1;
  }

  int code = 0;
  if (connected) {
    bool defaultPort = port == (tls ? 443 : 80);
    String req = String(method) + " " + path + " HTTP/1.1\r\n" +
                 "Host: " + host + (defaultPort ? String("") : ":" + String(port)) + "\r\n" +
                 "User-Agent: " SITE_USER_AGENT "\r\n"
                 "Accept: */*\r\n"
                 "Connection: close\r\n\r\n";
    c->print(req);
    String line = readLine(c, SITE_HTTP_TIMEOUT_MS);
    line.trim();
    code = sitelogic::statusCode(line);
  }
  c->stop();
  delete c;
  return code;
}

// HEAD first; some servers reject or drop it, so anything but up gets one GET.
uint8_t checkWeb(const Site& s, int16_t& code) {
  bool tls;
  String host, path;
  uint16_t port;
  if (!sitelogic::splitUrl(s.target, tls, host, port, path)) {
    code = -3;
    return sitelogic::STATE_DOWN;
  }
  IPAddress ip;
  if (!resolve(host, ip)) {
    Serial.printf("[site] %s: cannot resolve %s\n", s.name, host.c_str());
    code = -2;
    return sitelogic::STATE_DOWN;
  }

  if (tls && !tlsgate::acquire("site check", SITE_GATE_WAIT_MS, tlsgate::Need::Room)) {
    Serial.printf("[site] %s: TLS gate busy, skipped this pass\n", s.name);
    return sitelogic::STATE_UNKNOWN;
  }
  int c = request(tls, ip, host, port, path, "HEAD");
  if (!sitelogic::codeIsUp(c)) {
    delay(200);
    c = request(tls, ip, host, port, path, "GET");
  }
  if (tls) tlsgate::release();

  Serial.printf("[site] %s %s -> %d\n", s.name, s.target, c);
  code = (int16_t)c;
  return sitelogic::codeIsUp(c) ? sitelogic::STATE_UP : sitelogic::STATE_DOWN;
}

uint8_t checkPing(const Site& s, int16_t& code) {
  IPAddress ip;
  if (!ip.fromString(s.target)) {
    code = -3;
    return sitelogic::STATE_DOWN;
  }
  bool ok = Ping.ping(ip, SITE_PING_COUNT);
  Serial.printf("[site] %s ping %s -> %s\n", s.name, s.target, ok ? "ok" : "no reply");
  code = ok ? 1 : 0;
  return ok ? sitelogic::STATE_UP : sitelogic::STATE_DOWN;
}

// ---- alerts and digest -------------------------------------------------------

void announce(const Site& s, uint8_t previous, uint8_t now) {
  sitelogic::Alert a = sitelogic::alertFor(previous, now);
  if (a == sitelogic::Alert::None) return;
  String body = String(s.name) + " (" + s.target + ") is " +
                (now == sitelogic::STATE_UP ? "UP" : "DOWN");
  if (a == sitelogic::Alert::Down) {
    ntfy::alert(body, String("DOWN: ") + s.name, 4, "rotating_light");
  } else {
    ntfy::alert(body, String("Recovered: ") + s.name, 3, "white_check_mark");
  }
}

void sendDigest() {
  sitestore::Snapshot snap;
  int down = sitelogic::countState(snap.sites, snap.n, sitelogic::STATE_DOWN);
  String title = "Daily status";
  if (down) title += " - " + String(down) + " down";
  ntfy::alert(sitelogic::report(snap.sites, snap.n), title, down ? 3 : 2,
              down ? "warning" : "white_check_mark");
  Serial.println("[site] digest sent");
}

void maybeDigest() {
  if (!clockValid()) return;
  time_t now = time(nullptr);
  struct tm local;
  localtime_r(&now, &local);
  int last = digestMagic == DIGEST_MAGIC ? digestYday : -1;
  if (!sitelogic::digestDue(local, g_settings.digestHour, g_settings.digestMinute, last)) return;
  digestYday = local.tm_yday;
  digestMagic = DIGEST_MAGIC;
  if (sitestore::count() > 0) sendDigest();
}

// ---- the pass -----------------------------------------------------------------

void runPass() {
  running = true;
  int total = sitestore::count();
  Serial.printf("[site] pass: %d site(s), internal heap %u\n", total,
                (unsigned)tlsgate::freeInternal());

  // The list can shrink or grow under us (commands, the web page), so the
  // index is re-checked against the store every time.
  for (int i = 0; i < SITE_MAX; i++) {
    Site s;
    if (!sitestore::at(i, s)) break;
    if (!online()) break;

    int16_t code = 0;
    uint8_t state = s.type == sitelogic::TYPE_PING ? checkPing(s, code) : checkWeb(s, code);

    // A result taken while the WiFi dropped says nothing about the site.
    if (!online()) break;
    uint8_t previous = sitestore::record(i, s.name, state, code);
    if (previous != 0xFF) announce(s, previous, state);

    vTaskDelay(pdMS_TO_TICKS(SITE_GAP_MS));
  }
  Serial.printf("[site] pass done, internal heap %u\n", (unsigned)tlsgate::freeInternal());
  running = false;
}

void taskMain(void*) {
  nextPassMs = millis() + SITE_FIRST_PASS_MS;
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(500));
    if (!online()) continue;
    maybeDigest();

    bool due = (int32_t)(millis() - nextPassMs) >= 0;
    if (!due && !requested) continue;
    requested = false;
    nextPassMs = millis() + (uint32_t)g_settings.siteIntervalMin * 60000UL;
    if (sitestore::count() > 0) runPass();
  }
}

}  // namespace

void begin() {
  if (task) return;
  xTaskCreatePinnedToCore(taskMain, "site", SITE_TASK_STACK, nullptr, SITE_TASK_PRIORITY,
                          &task, 0);
}

bool checkNow() {
  if (running || !task) return false;
  requested = true;
  return true;
}

bool passRunning() { return running; }

String summary() {
  sitestore::Snapshot snap;
  int n = snap.n;
  if (n == 0) return "no sites monitored";
  int up = sitelogic::countState(snap.sites, n, sitelogic::STATE_UP);
  int down = sitelogic::countState(snap.sites, n, sitelogic::STATE_DOWN);
  String out = String(n) + " site" + (n == 1 ? "" : "s") + ": " + String(up) + " up, " +
               String(down) + " down";
  if (n - up - down) out += ", " + String(n - up - down) + " not checked yet";
  if (running) {
    out += "; checking now";
  } else if (task) {
    int32_t left = (int32_t)(nextPassMs - millis());
    out += "; next check in " + String(left > 0 ? (left + 59999) / 60000 : 0) + " min";
  }
  char dg[8];
  snprintf(dg, sizeof(dg), "%02u:%02u", g_settings.digestHour, g_settings.digestMinute);
  return out + "; digest " + dg;
}

}  // namespace sitecheck
