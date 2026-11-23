// Monitored-site list. Storage format and rules from SiteCheck 2.0.0
// (Apache-2.0); the file moved from /hosts.json to /sites.json, since the ad
// blocker's local DNS already owns /hosts.txt on the same partition.

#include <Arduino.h>
#include <LittleFS.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <strings.h>

#include "config.h"
#include "sitestore.h"

namespace sitestore {
namespace {

Site*             sites = nullptr;   // SITE_MAX entries, PSRAM
int               n = 0;
SemaphoreHandle_t mu = nullptr;
bool              fsOk = false;

struct Lock {
  Lock()  { xSemaphoreTake(mu, portMAX_DELAY); }
  ~Lock() { xSemaphoreGive(mu); }
};

Site* psramSites() {
  Site* p = (Site*)heap_caps_calloc(SITE_MAX, sizeof(Site), MALLOC_CAP_SPIRAM);
  return p ? p : (Site*)calloc(SITE_MAX, sizeof(Site));   // no PSRAM: last resort
}

int find(const String& name) {
  for (int i = 0; i < n; i++) {
    if (strcasecmp(sites[i].name, name.c_str()) == 0) return i;
  }
  return -1;
}

// Written to a temporary file and renamed, so a power cut mid-write leaves the
// old list rather than half a new one. Caller holds the lock.
bool persist() {
  if (!fsOk) return false;
  String json = sitelogic::toJson(sites, n);
  File f = LittleFS.open(SITE_TMP_PATH, "w");
  if (!f) {
    Serial.println("[site] cannot write " SITE_TMP_PATH);
    return false;
  }
  size_t w = f.print(json);
  f.close();
  if (w != json.length()) {
    Serial.println("[site] short write -- flash full?");
    LittleFS.remove(SITE_TMP_PATH);
    return false;
  }
  LittleFS.remove(SITE_PATH);
  if (!LittleFS.rename(SITE_TMP_PATH, SITE_PATH)) {
    Serial.println("[site] rename failed");
    return false;
  }
  return true;
}

}  // namespace

bool begin() {
  if (!sites) sites = psramSites();
  if (!mu) mu = xSemaphoreCreateMutex();
  // Same partition the blocklist mounts; LittleFS.begin() on an already
  // mounted filesystem just says so and returns true.
  fsOk = LittleFS.begin(/*formatOnFail=*/true);
  if (!fsOk) {
    Serial.println("[site] LittleFS mount failed -- sites will not survive a reboot");
    return false;
  }

  Lock l;
  n = 0;
  if (!LittleFS.exists(SITE_PATH)) {
    Serial.println("[site] no " SITE_PATH " yet");
    return true;
  }
  File f = LittleFS.open(SITE_PATH, "r");
  String text = f ? f.readString() : String();
  if (f) f.close();
  if (!sitelogic::fromJson(text, sites, n, SITE_MAX)) {
    // A corrupt file would otherwise wedge every boot.
    Serial.println("[site] " SITE_PATH " is unreadable, starting empty");
    n = 0;
  }
  Serial.printf("[site] loaded %d site(s)\n", n);
  return true;
}

int count() {
  if (!mu) return 0;
  Lock l;
  return n;
}

Snapshot::Snapshot() {
  sites = psramSites();
  if (sites) n = snapshot(sites, SITE_MAX);
}

Snapshot::~Snapshot() { free(sites); }

int snapshot(Site* out, int max) {
  if (!mu) return 0;
  Lock l;
  int c = n < max ? n : max;
  memcpy(out, sites, sizeof(Site) * c);
  return c;
}

bool at(int index, Site& out) {
  if (!mu) return false;
  Lock l;
  if (index < 0 || index >= n) return false;
  out = sites[index];
  return true;
}

AddResult add(const String& rawName, const String& rawTarget, Site* site) {
  String name = sitelogic::sanitize(rawName);
  if (name.length() >= SITE_NAME_LEN) name = name.substring(0, SITE_NAME_LEN - 1);
  name.trim();
  if (name.isEmpty()) return AddResult::BadName;

  String target;
  uint8_t type;
  if (!sitelogic::parseTarget(sitelogic::sanitize(rawTarget), type, target)) {
    return AddResult::BadTarget;
  }

  if (!mu) return AddResult::IoError;
  Lock l;
  if (find(name) >= 0) return AddResult::Duplicate;
  if (n >= SITE_MAX) return AddResult::Full;

  Site& s = sites[n];
  memset(&s, 0, sizeof(s));
  strncpy(s.name, name.c_str(), sizeof(s.name) - 1);
  strncpy(s.target, target.c_str(), sizeof(s.target) - 1);
  s.type = type;
  s.state = sitelogic::STATE_UNKNOWN;   // the first check decides; no alert for "up"
  s.lastChange = time(nullptr) > (time_t)NTP_MIN_VALID_EPOCH ? (uint32_t)time(nullptr) : 0;
  n++;
  if (site) *site = s;
  return persist() ? AddResult::Ok : AddResult::IoError;
}

bool remove(const String& rawName, String* removed) {
  String name = sitelogic::sanitize(rawName);
  if (!mu) return false;
  Lock l;
  int i = find(name);
  if (i < 0) return false;
  if (removed) *removed = sites[i].name;
  for (int j = i; j + 1 < n; j++) sites[j] = sites[j + 1];
  n--;
  persist();
  return true;
}

void clear() {
  if (!mu) return;
  Lock l;
  n = 0;
  persist();
}

uint8_t record(int index, const char* name, uint8_t state, int16_t code) {
  if (!mu) return 0xFF;
  Lock l;
  if (index < 0 || index >= n || strcmp(sites[index].name, name) != 0) {
    int i = find(String(name));
    if (i < 0) return 0xFF;
    index = i;
  }
  Site& s = sites[index];
  uint8_t previous = s.state;
  s.lastCode = code;
  if (state != sitelogic::STATE_UNKNOWN && state != previous) {
    s.state = state;
    s.lastChange = time(nullptr) > (time_t)NTP_MIN_VALID_EPOCH ? (uint32_t)time(nullptr) : 0;
    persist();
  }
  return previous;
}

const char* addMessage(AddResult r) {
  switch (r) {
    case AddResult::Ok:        return "added";
    case AddResult::Duplicate: return "already monitored";
    case AddResult::Full:      return "the list is full";
    case AddResult::BadName:   return "needs a name";
    case AddResult::BadTarget: return "not an IPv4 address, URL or host name";
    case AddResult::IoError:   return "added, but could not be saved to flash";
  }
  return "?";
}

}  // namespace sitestore
