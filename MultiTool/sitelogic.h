// The site monitor's pure logic: target parsing, URL splitting, the stored
// JSON, alert decisions and the report text. No I/O, so the host tests can
// cover all of it. Rules and formats come from SiteCheck 2.0.0 (Apache-2.0).
#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <string.h>
#include <string>
#include <time.h>

#include "config.h"

namespace sitelogic {

enum Type : uint8_t { TYPE_HTTPS = 0, TYPE_HTTP = 1, TYPE_PING = 2 };
enum State : uint8_t { STATE_UNKNOWN = 0, STATE_UP = 1, STATE_DOWN = 2 };

struct Site {
  char     name[SITE_NAME_LEN];       // as typed; matched case-insensitively
  char     target[SITE_TARGET_LEN];   // full URL, or an IPv4 address for ping
  uint8_t  type;                      // Type
  uint8_t  state;                     // State
  uint32_t lastChange;                // epoch of the last transition, 0 = never
  int16_t  lastCode;                  // HTTP status, 1 = ping ok, <= 0 failure
};

inline const char* stateName(uint8_t s) {
  return s == STATE_UP ? "up" : s == STATE_DOWN ? "down" : "unknown";
}

inline const char* typeName(uint8_t t) {
  return t == TYPE_HTTP ? "http" : t == TYPE_PING ? "ping" : "https";
}

inline bool isIPv4(const String& s) {
  int octets = 0, value = 0, digits = 0;
  for (unsigned int i = 0; i <= s.length(); i++) {
    char c = (i < s.length()) ? s[i] : '.';
    if (c >= '0' && c <= '9') {
      if (++digits > 3) return false;
      value = value * 10 + (c - '0');
      if (value > 255) return false;
    } else if (c == '.') {
      if (digits == 0) return false;
      octets++;
      value = 0;
      digits = 0;
    } else {
      return false;
    }
  }
  return octets == 4;
}

// Strips characters that would break the JSON file or an ntfy header, and
// the separators of the batch form. ASCII only.
inline String sanitize(const String& in) {
  String out;
  for (unsigned int i = 0; i < in.length(); i++) {
    char c = in[i];
    if (c == '"' || c == '\\' || c == '|' || c == ';') continue;
    if ((uint8_t)c < 0x20 || (uint8_t)c > 0x7E) continue;
    out += c;
  }
  out.trim();
  return out;
}

// SiteCheck's rules: an IPv4 address is pinged; http:// and https:// URLs are
// used as typed; anything else with a dot (or a single-label name such as
// "nas") becomes https://<input>.
inline bool parseTarget(const String& input, uint8_t& type, String& out) {
  String t = input;
  t.trim();
  if (t.isEmpty() || t.indexOf(" ") >= 0) return false;

  if (isIPv4(t)) {
    type = TYPE_PING;
  } else if (t.startsWith("http://")) {
    type = TYPE_HTTP;
  } else if (t.startsWith("https://")) {
    type = TYPE_HTTPS;
  } else {
    if (t.indexOf("://") >= 0) return false;   // ftp://, ws:// ...
    type = TYPE_HTTPS;
    t = String("https://") + t;
  }
  if (t.length() >= SITE_TARGET_LEN) return false;
  out = t;
  return true;
}

// "https://host:8443/path?q" -> tls, host, port, "/path?q". False if there is
// no host or the port is not a number.
inline bool splitUrl(const String& url, bool& tls, String& host, uint16_t& port, String& path) {
  int start;
  if (url.startsWith("https://"))     { tls = true;  start = 8; port = 443; }
  else if (url.startsWith("http://")) { tls = false; start = 7; port = 80; }
  else return false;

  std::string s = url.c_str();
  size_t slash = s.find('/', start);
  size_t q = s.find('?', start);
  if (q != std::string::npos && (slash == std::string::npos || q < slash)) slash = q;
  std::string authority = s.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
  path = slash == std::string::npos ? String("/") : String(s.substr(slash).c_str());
  if (path.startsWith("?")) path = String("/") + path;

  size_t at = authority.rfind('@');   // user:pass@ is not supported, drop it
  if (at != std::string::npos) authority = authority.substr(at + 1);
  size_t colon = authority.rfind(':');
  if (colon != std::string::npos) {
    std::string p = authority.substr(colon + 1);
    if (p.empty() || p.size() > 5) return false;
    long v = 0;
    for (char c : p) {
      if (c < '0' || c > '9') return false;
      v = v * 10 + (c - '0');
    }
    if (v < 1 || v > 65535) return false;
    port = (uint16_t)v;
    authority = authority.substr(0, colon);
  }
  if (authority.empty()) return false;
  host = String(authority.c_str());
  return true;
}

// Names the public resolvers will never know: single labels ("nas") and the
// home-network suffixes. These are looked up in the local DNS records and at
// the router instead. .local is mDNS and is left to lwIP.
inline bool isLocalName(const String& host) {
  if (isIPv4(host)) return false;
  if (host.indexOf(".") < 0) return true;
  String h = host;
  h.toLowerCase();
  const char* suffixes[] = {".lan", ".home", ".home.arpa", ".internal", ".localdomain"};
  for (const char* sfx : suffixes) {
    size_t n = strlen(sfx);
    if (h.length() > n && strcmp(h.c_str() + h.length() - n, sfx) == 0) return true;
  }
  return false;
}

// "HTTP/1.1 301 Moved" -> 301; anything else -> -1.
inline int statusCode(const String& line) {
  if (!line.startsWith("HTTP/")) return -1;
  int sp = line.indexOf(" ");
  if (sp < 0 || line.length() < (unsigned)sp + 4) return -1;
  int code = 0;
  for (int i = sp + 1; i < sp + 4; i++) {
    char c = line[i];
    if (c < '0' || c > '9') return -1;
    code = code * 10 + (c - '0');
  }
  return code;
}

// 2xx and 3xx count as up; redirects are reported, not followed.
inline bool codeIsUp(int code) { return code >= 200 && code < 400; }

// What a check result means for notifications.
enum class Alert : uint8_t { None, Down, Recovered };

// SiteCheck's rules: the first result after a boot or an add reports only bad
// news; after that every transition is reported; no change stays quiet.
inline Alert alertFor(uint8_t previous, uint8_t now) {
  if (now == STATE_UNKNOWN || now == previous) return Alert::None;
  if (now == STATE_DOWN) return Alert::Down;
  return previous == STATE_UNKNOWN ? Alert::None : Alert::Recovered;
}

// One line per site: "name: up", with the address for ping targets since the
// name alone rarely identifies an IP. Used by the digest, "site list" and the
// status reply.
inline String report(const Site* sites, int n) {
  if (n == 0) return "No sites monitored.";
  String out;
  for (int i = 0; i < n; i++) {
    const Site& s = sites[i];
    if (i) out += "\n";
    out += s.name;
    if (s.type == TYPE_PING) out += String(" (") + s.target + ")";
    out += String(": ") + stateName(s.state);
  }
  return out;
}

inline int countState(const Site* sites, int n, uint8_t state) {
  int c = 0;
  for (int i = 0; i < n; i++) if (sites[i].state == state) c++;
  return c;
}

// True once per day, from the digest minute until the end of that hour.
inline bool digestDue(const struct tm& local, uint8_t hour, uint8_t minute, int lastYday) {
  return local.tm_hour == hour && local.tm_min >= minute && local.tm_yday != lastYday;
}

// ---- the stored list (/sites.json, SiteCheck's hosts.json format) ----------

inline String toJson(const Site* sites, int n) {
  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();
  for (int i = 0; i < n; i++) {
    JsonObject o = arr.add<JsonObject>();
    o["name"] = sites[i].name;
    o["target"] = sites[i].target;
    o["type"] = typeName(sites[i].type);
    o["state"] = stateName(sites[i].state);
    o["last_change"] = sites[i].lastChange;
  }
  String out;
  serializeJson(doc, out);
  return out;
}

// Fills `sites` from the JSON text. Entries with no name or an unusable target
// are skipped; past `max` are dropped. False only if the text is not JSON.
inline bool fromJson(const String& text, Site* sites, int& n, int max) {
  n = 0;
  JsonDocument doc;
  if (deserializeJson(doc, text.c_str())) return false;
  for (JsonObject o : doc.as<JsonArray>()) {
    if (n >= max) break;
    String name = sanitize(String(o["name"] | ""));
    String target;
    uint8_t type;
    if (name.isEmpty() || !parseTarget(String(o["target"] | ""), type, target)) continue;

    Site& s = sites[n];
    memset(&s, 0, sizeof(s));
    strncpy(s.name, name.c_str(), sizeof(s.name) - 1);
    strncpy(s.target, target.c_str(), sizeof(s.target) - 1);
    s.type = type;
    const char* st = o["state"] | "unknown";
    s.state = !strcmp(st, "up") ? STATE_UP : !strcmp(st, "down") ? STATE_DOWN : STATE_UNKNOWN;
    s.lastChange = o["last_change"] | 0UL;
    n++;
  }
  return true;
}

}  // namespace sitelogic
