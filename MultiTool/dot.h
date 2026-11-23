#pragma once

#include <Arduino.h>

// DNS-over-TLS upstream. A proxy task owns one long-lived TLS connection to
// Settings::dotHost (RFC 7858). The sinkhole hands it queries through a
// queue and gets replies back through sinkhole::fromDot(), so it never blocks
// on TLS itself.

namespace dot {

// Starts the proxy task if DoT is enabled. Safe to call more than once.
void begin();

// True while the TLS connection is up and not paused; the sinkhole only
// routes queries here when it is.
bool ready();

// Queues one query for the TLS stream. False when DoT can't take it right
// now (down, paused, queue full), so the caller moves to the next server at
// once instead of waiting for a timeout.
bool submit(const uint8_t* data, int len);

// True while the TLS connection is up. tlsgate waits on this when it has
// asked the link to step aside (see tlsgate::dotMustYield()).
bool connected();

// "connected to one.one.one.one", "connecting", "off" ...
String status();

}  // namespace dot
