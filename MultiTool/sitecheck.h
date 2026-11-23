#pragma once

#include <Arduino.h>

// The site monitor's worker: a low-priority FreeRTOS task that checks one site
// at a time, sends down / recovered alerts and the daily digest. Strictly one
// socket at a time, as SiteCheck did; HTTPS probes go through the TLS gate,
// so they wait out the blocklist download instead of competing with it.
//
// It never blocks the DNS tasks or the main loop: alerts go through the ntfy
// outbox, which any task may fill.

namespace sitecheck {

// Starts the task (once). The first pass runs SITE_FIRST_PASS_MS later.
void begin();

// Asks for a pass now. False if one is already running.
bool checkNow();

// "3 sites: 2 up, 1 down; next check in 12 min" -- for status and the web page.
String summary();

bool passRunning();

}  // namespace sitecheck
