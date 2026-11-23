// Host-side tests for sitelogic.h: SiteCheck's target rules, URL splitting,
// the stored JSON, alert decisions and the report text.
#include <cstdio>
#include <cstring>

#include "Arduino.h"
#include "sitelogic.h"

SerialShim Serial;

using namespace sitelogic;

static int failures = 0, checks = 0;

static void ok(bool cond, const char* what) {
  checks++;
  if (!cond) { failures++; printf("  FAIL: %s\n", what); }
}

static Site mk(const char* name, const char* target, uint8_t type, uint8_t state) {
  Site s = {};
  strncpy(s.name, name, sizeof(s.name) - 1);
  strncpy(s.target, target, sizeof(s.target) - 1);
  s.type = type;
  s.state = state;
  return s;
}

int main() {
  uint8_t type = 99;
  String out;

  // --- parseTarget: SiteCheck's three rules ---
  ok(parseTarget("192.168.1.50", type, out) && type == TYPE_PING && out == "192.168.1.50",
     "IPv4 is pinged as typed");
  ok(parseTarget("http://nas.lan:8080/health", type, out) && type == TYPE_HTTP &&
     out == "http://nas.lan:8080/health", "http:// URL kept as typed");
  ok(parseTarget("https://example.com", type, out) && type == TYPE_HTTPS, "https:// URL kept");
  ok(parseTarget("immich.example.com", type, out) && type == TYPE_HTTPS &&
     out == "https://immich.example.com", "bare host becomes https://");
  ok(parseTarget("nas", type, out) && out == "https://nas", "single-label host allowed");
  ok(parseTarget("  example.com  ", type, out) && out == "https://example.com", "trimmed");
  ok(!parseTarget("", type, out), "empty rejected");
  ok(!parseTarget("ftp://x.example.com", type, out), "other schemes rejected");
  ok(!parseTarget("has space.com", type, out), "spaces rejected");
  ok(!parseTarget(String(std::string(100, 'a').c_str()), type, out), "too long rejected");
  ok(!parseTarget("256.1.1.1", type, out) || type != TYPE_PING, "256.x is not pinged");

  // --- isIPv4 ---
  ok(isIPv4("10.0.0.1") && !isIPv4("10.0.0") && !isIPv4("10.0.0.1.2") && !isIPv4("a.b.c.d"),
     "isIPv4 basics");

  // --- sanitize ---
  ok(sanitize("  My \"NAS\"|;  ") == "My NAS", "sanitize drops quotes and separators");

  // --- splitUrl ---
  bool tls; String host, path; uint16_t port;
  ok(splitUrl("https://example.com", tls, host, port, path) && tls && host == "example.com" &&
     port == 443 && path == "/", "https default port and path");
  ok(splitUrl("http://nas.lan:8080/api/health?x=1", tls, host, port, path) && !tls &&
     host == "nas.lan" && port == 8080 && path == "/api/health?x=1", "port, path and query");
  ok(splitUrl("http://host?q=1", tls, host, port, path) && path == "/?q=1", "query without path");
  ok(splitUrl("https://user:pw@h.example.com/", tls, host, port, path) && host == "h.example.com",
     "userinfo dropped");
  ok(!splitUrl("https://:8080/", tls, host, port, path), "no host rejected");
  ok(!splitUrl("http://h:99999/", tls, host, port, path), "bad port rejected");
  ok(!splitUrl("ftp://h/", tls, host, port, path), "other scheme rejected");

  // --- isLocalName ---
  ok(isLocalName("nas") && isLocalName("printer.lan") && isLocalName("pi.home.arpa") &&
     isLocalName("x.INTERNAL"), "local names");
  ok(!isLocalName("example.com") && !isLocalName("192.168.1.1") && !isLocalName("lan.example.com"),
     "public names and IPs are not local");

  // --- statusCode / codeIsUp ---
  ok(statusCode("HTTP/1.1 200 OK") == 200 && statusCode("HTTP/1.0 301 Moved") == 301,
     "status lines parse");
  ok(statusCode("") == -1 && statusCode("garbage") == -1 && statusCode("HTTP/1.1 2x0") == -1,
     "bad status lines");
  ok(codeIsUp(200) && codeIsUp(302) && !codeIsUp(404) && !codeIsUp(503) && !codeIsUp(0) &&
     !codeIsUp(-1), "2xx/3xx up, rest down");

  // --- alertFor: SiteCheck's rules ---
  ok(alertFor(STATE_UNKNOWN, STATE_UP) == Alert::None, "first result up: quiet");
  ok(alertFor(STATE_UNKNOWN, STATE_DOWN) == Alert::Down, "first result down: alert");
  ok(alertFor(STATE_UP, STATE_DOWN) == Alert::Down, "up -> down: alert");
  ok(alertFor(STATE_DOWN, STATE_UP) == Alert::Recovered, "down -> up: recovered");
  ok(alertFor(STATE_UP, STATE_UP) == Alert::None && alertFor(STATE_DOWN, STATE_DOWN) == Alert::None,
     "no change: quiet");
  ok(alertFor(STATE_UP, STATE_UNKNOWN) == Alert::None, "skipped check: quiet");

  // --- report ---
  Site list[3] = {mk("web", "https://example.com", TYPE_HTTPS, STATE_UP),
                  mk("nas", "192.168.1.50", TYPE_PING, STATE_DOWN),
                  mk("new", "https://n.example.com", TYPE_HTTPS, STATE_UNKNOWN)};
  ok(report(list, 3) == "web: up\nnas (192.168.1.50): down\nnew: unknown",
     "report: one line each, IP shown for ping");
  ok(report(list, 0) == "No sites monitored.", "report: empty");
  ok(countState(list, 3, STATE_DOWN) == 1 && countState(list, 3, STATE_UP) == 1, "countState");

  // --- digestDue ---
  struct tm t = {};
  t.tm_hour = 8; t.tm_min = 0; t.tm_yday = 100;
  ok(digestDue(t, 8, 0, 99), "digest due at the minute");
  ok(!digestDue(t, 8, 0, 100), "not twice the same day");
  t.tm_min = 45;
  ok(digestDue(t, 8, 0, 99), "still due later in that hour (missed minute)");
  t.tm_hour = 9;
  ok(!digestDue(t, 8, 0, 99), "not after the hour");
  t.tm_hour = 8; t.tm_min = 10;
  ok(!digestDue(t, 8, 30, 99), "not before the minute");

  // --- JSON round trip ---
  list[0].lastChange = 1732000000;
  String json = toJson(list, 3);
  Site back[SITE_MAX];
  int n = -1;
  ok(fromJson(json, back, n, SITE_MAX) && n == 3, "round trip count");
  ok(strcmp(back[1].name, "nas") == 0 && back[1].type == TYPE_PING && back[1].state == STATE_DOWN,
     "round trip ping entry");
  ok(back[0].lastChange == 1732000000 && back[0].state == STATE_UP, "round trip last_change");
  ok(back[2].state == STATE_UNKNOWN, "round trip unknown");

  // SiteCheck's own hosts.json format loads too.
  const char* legacy =
      "[{\"name\":\"focaltek\",\"target\":\"https://focaltek.com\",\"type\":\"https\","
      "\"state\":\"up\",\"last_change\":1732000000},"
      "{\"name\":\"nas\",\"target\":\"192.168.1.50\",\"type\":\"ping\",\"state\":\"down\","
      "\"last_change\":1732003600}]";
  ok(fromJson(legacy, back, n, SITE_MAX) && n == 2 && back[1].type == TYPE_PING,
     "SiteCheck hosts.json loads");
  ok(fromJson("[{\"name\":\"\",\"target\":\"x.com\"},{\"name\":\"ok\",\"target\":\"ftp://x\"},"
              "{\"name\":\"good\",\"target\":\"good.example.com\"}]", back, n, SITE_MAX) &&
     n == 1 && strcmp(back[0].name, "good") == 0, "bad entries skipped");
  ok(!fromJson("{not json", back, n, SITE_MAX), "garbage rejected");
  ok(fromJson(legacy, back, n, 1) && n == 1, "capped at max");

  printf("%d/%d checks passed\n", checks - failures, checks);
  return failures ? 1 : 0;
}
