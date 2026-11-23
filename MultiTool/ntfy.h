#pragma once

#include <Arduino.h>
#include <functional>

// ntfy transport: one long-lived TLS connection streaming inbound messages,
// plus short-lived POSTs for replies.
//
// Outbound messages go through an outbox in PSRAM: publish() and alert() only
// queue, from any task, and poll() sends one per pass when the TLS gate is
// free. A slow site probe or the blocklist download therefore never stalls a
// reply -- it just goes out a few seconds later.

namespace ntfy {

// Called for each inbound message that survives dedup and the self-message
// filter. The argument is the message body.
using MessageHandler = std::function<void(const String& message)>;

// Allocates the outbox in PSRAM. Call from setup() before blocklist::begin(),
// which takes whatever PSRAM is left.
void prealloc();

void begin(MessageHandler handler);

// Pumps the stream. Non-blocking; call every loop().
void poll();

// Queues a reply to the topic. Stamped with NTFY_SELF_TAG so poll() will
// ignore it when it comes back down the stream. False if the outbox is full
// or not allocated.
bool publish(const String& message, const String& title = "");

// Like publish(), but to Settings::alertTopic when one is set, with an ntfy
// priority (1-5, 3 = default) and extra tags (comma-separated emoji names).
bool alert(const String& message, const String& title, uint8_t priority = 3,
           const char* tags = "");

// Sends everything queued, waiting up to timeoutMs. For the moments before a
// restart. Main loop only. True if the outbox emptied.
bool flush(uint32_t timeoutMs);

// Messages waiting to go out.
int pending();

bool connected();

}  // namespace ntfy
