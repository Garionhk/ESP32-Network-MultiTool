#pragma once

#include <Arduino.h>

#include "sitelogic.h"

// The monitored sites: up to SITE_MAX, in PSRAM (130 B each), persisted to
// /sites.json on LittleFS. The command handler, the web page and the check
// task all use it, so every call takes a lock and copies out.
//
// Flash wear stays low the way SiteCheck kept it low: the file is rewritten on
// add / remove and on a state transition, never on a check that changed
// nothing.

namespace sitestore {

using sitelogic::Site;

enum class AddResult : uint8_t {
  Ok,
  Duplicate,   // name already taken (case-insensitive)
  Full,        // SITE_MAX reached
  BadName,     // empty after sanitizing
  BadTarget,   // not an IPv4 address, URL or host name
  IoError,     // added in RAM, but the file could not be written
};

// Mounts LittleFS (a no-op when the blocklist already did) and loads the list.
bool begin();

int count();

// Copies the list out. Returns how many were copied (at most `max`).
int snapshot(Site* out, int max);

// A copy of the whole list in PSRAM, freed when it goes out of scope. Internal
// RAM is what TLS runs on, so ~4 KB lists are never kept there.
struct Snapshot {
  Site* sites = nullptr;
  int   n = 0;
  Snapshot();
  ~Snapshot();
  Snapshot(const Snapshot&) = delete;
  Snapshot& operator=(const Snapshot&) = delete;
};

// Copies one site out by index. False past the end.
bool at(int index, Site& out);

// `site` gets the stored entry (with the normalized target) on Ok.
AddResult add(const String& name, const String& target, Site* site = nullptr);

// By name, case-insensitive. `removed` gets the stored spelling.
bool remove(const String& name, String* removed = nullptr);

void clear();

// Records a check result for the site at `index` -- if it is still the same
// site (`name`), since a remove can shift the list during a pass. Returns the
// previous state, or 0xFF if the site is gone. Persists on a transition.
uint8_t record(int index, const char* name, uint8_t state, int16_t code);

const char* addMessage(AddResult r);

}  // namespace sitestore
