#pragma once

#include <Arduino.h>

// Who may open a TLS connection, and when.
//
// mbedTLS buffers live in internal RAM -- the prebuilt Arduino core sets
// CONFIG_MBEDTLS_INTERNAL_MEM_ALLOC and that cannot be changed from
// arduino-cli -- and the S3 has only ~100 KB of it free once the ad blocker
// is running. Each session needs ~35-45 KB, most of it during the handshake.
// So:
//
//   * every TLS handshake in the firmware goes through this gate, one at a
//     time;
//   * short-lived sessions (ntfy publish, blocklist download, site probes)
//     hold the gate for their whole life, so at most one is open at once;
//   * long-lived sessions (ntfy stream, DoT) hold it only for the handshake;
//   * when a short-lived session needs room, the DoT link is asked to step
//     aside until the gate is released. Lookups fall back to plain DNS for
//     those seconds -- the same trade the ad blocker already made for the
//     blocklist download.
//
// The gate is a FreeRTOS mutex: acquire() and release() must come from the
// same task.

namespace tlsgate {

enum class Need : uint8_t {
  Handshake,   // long-lived session: just one handshake at a time
  Room,        // short-lived session: DoT yields if internal RAM is short
  DotOff,      // DoT always yields (the blocklist download, which runs ~20 s)
};

void begin();

// Waits up to waitMs for the gate. With Need::Room / DotOff it then makes
// room, waiting up to TLS_YIELD_WAIT_MS for DoT to disconnect. Returns false
// if the gate could not be had; on true, call release() when the session is
// closed (or, for a long-lived one, when the handshake is done).
bool acquire(const char* who, uint32_t waitMs, Need need = Need::Room);
void release();

// True while some session has asked the DoT link to stand down.
bool dotMustYield();

// True while the gate is held -- DoT does not start a handshake then.
bool busy();

// Who holds it now, "" if nobody. For the status report and the DoT status.
const char* holder();

// Internal RAM free and the largest block of it, for logs and status.
uint32_t freeInternal();
uint32_t largestInternal();

}  // namespace tlsgate
