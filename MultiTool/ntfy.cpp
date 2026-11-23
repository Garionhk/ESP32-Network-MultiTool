#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <base64.h>

#include "certs.h"
#include "config.h"
#include "ntfy.h"
#include "settings.h"
#include "tlsgate.h"

#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace ntfy {
namespace {

// TLS is now a runtime setting, so the client type cannot be a typedef.
// NetworkClientSecure derives from NetworkClient and HTTPClient::begin() takes
// the base reference, so one pointer covers both cases.
NetworkClient* stream = nullptr;
MessageHandler onMessage;

String   cachedAuth;
bool     authBuilt = false;

String scheme() { return g_settings.ntfyTls ? "https://" : "http://"; }

// Allocates the right client for the configured mode, with the CA bundle and
// read timeout applied. Caller owns the result.
NetworkClient* makeClient() {
  if (g_settings.ntfyTls) {
    NetworkClientSecure* s = new NetworkClientSecure();
#if defined(NTFY_TLS_INSECURE)
    s->setInsecure();
#else
    s->setCACert(NTFY_ROOT_CAS);
#endif
    s->setTimeout(NTFY_SOCKET_TIMEOUT_MS);
    return s;
  }
  NetworkClient* c = new NetworkClient();
  c->setTimeout(NTFY_SOCKET_TIMEOUT_MS);
  return c;
}

// "Bearer tk_..." or "Basic base64(user:password)", or empty for an open
// server. Settings are fixed until the next reboot, so building once is safe.
const String& authHeader() {
  if (!authBuilt) {
    if (!g_settings.ntfyToken.isEmpty()) {
      cachedAuth = "Bearer " + g_settings.ntfyToken;
    } else if (!g_settings.ntfyUser.isEmpty()) {
      // This core's libb64 does not wrap lines, so the result is header-safe.
      cachedAuth = "Basic " +
                   base64::encode(g_settings.ntfyUser + ":" + g_settings.ntfyPass);
    }
    authBuilt = true;
  }
  return cachedAuth;
}

void drainOutbox();   // below; poll() sends from the outbox

enum class State { Disconnected, Streaming };
State state = State::Disconnected;

String   lineBuf;
uint32_t lastDataMs      = 0;
uint32_t nextAttemptMs   = 0;
uint32_t backoffMs       = NTFY_RECONNECT_MIN_MS;

// Last message ID seen, used as ?since= on reconnect so the gap while we were
// away gets replayed. Deliberately empty on cold boot -- see connect().
String lastId;

// Ring of recently handled IDs. A reconnect can legitimately redeliver a
// message we already acted on; without this, a block or a reboot could run twice.
String   seenIds[NTFY_DEDUP_SLOTS];
uint8_t  seenNext = 0;

bool alreadySeen(const String& id) {
  if (id.isEmpty()) return false;
  for (uint8_t i = 0; i < NTFY_DEDUP_SLOTS; i++) {
    if (seenIds[i] == id) return true;
  }
  seenIds[seenNext] = id;
  seenNext = (seenNext + 1) % NTFY_DEDUP_SLOTS;
  return false;
}

void disconnect(const char* why) {
  if (state != State::Disconnected || (stream && stream->connected())) {
    Serial.printf("[ntfy] disconnecting: %s\n", why);
  }
  if (stream) stream->stop();
  state = State::Disconnected;
  lineBuf = "";

  nextAttemptMs = millis() + backoffMs;
  backoffMs = min(backoffMs * 2, (uint32_t)NTFY_RECONNECT_MAX_MS);
}

// The caller holds the TLS gate (when TLS is on): the handshake is when
// mbedTLS needs the most memory, so only one runs at a time.
bool connect() {
  Serial.printf("[ntfy] connecting to %s:%u (%s, heap %u)\n",
                g_settings.ntfyHost.c_str(), (unsigned)g_settings.ntfyPort,
                g_settings.ntfyTls ? "tls" : "plain",
                (unsigned)ESP.getFreeHeap());

  bool opened = stream->connect(g_settings.ntfyHost.c_str(), g_settings.ntfyPort);
  if (g_settings.ntfyTls) tlsgate::release();   // handshake done either way
  if (!opened) {
    Serial.println("[ntfy] TCP/TLS connect failed");
    return false;
  }

  // On a cold boot lastId is empty and no ?since= is sent, so only genuinely
  // new messages arrive. Replaying the whole topic history at boot would
  // re-execute every command ever sent.
  String path = "/" + g_settings.ntfyTopic + "/json";
  if (!lastId.isEmpty()) path += "?since=" + lastId;

  String req = "GET " + path + " HTTP/1.1\r\n"
               "Host: " + g_settings.ntfyHost + "\r\n"
               "User-Agent: esp32-multitool\r\n"
               "Accept: application/x-ndjson\r\n";
  if (!authHeader().isEmpty()) {
    req += "Authorization: " + authHeader() + "\r\n";
  }
  req += "Connection: keep-alive\r\n\r\n";

  stream->print(req);

  // Status line, then headers up to the blank line.
  String status = stream->readStringUntil('\n');
  status.trim();
  if (status.indexOf("200") < 0) {
    Serial.printf("[ntfy] unexpected response: %s\n", status.c_str());
    if (status.indexOf("401") >= 0 || status.indexOf("403") >= 0) {
      Serial.println("[ntfy] the server rejected the login -- check the ntfy "
                     "username, password or token in the settings page, and "
                     "this user's access to the topic");
    }
    stream->stop();
    return false;
  }

  while (stream->connected()) {
    String header = stream->readStringUntil('\n');
    header.trim();
    if (header.isEmpty()) break;   // end of headers
  }

  Serial.println("[ntfy] subscribed");
  state       = State::Streaming;
  lastDataMs  = millis();
  backoffMs   = NTFY_RECONNECT_MIN_MS;
  lineBuf     = "";
  return true;
}

// One newline-delimited JSON object from the stream.
void handleLine(const String& line) {
  if (line.length() < 2) return;   // blank keepalive padding

  // Keep only the fields we use, so an unexpectedly chatty message cannot
  // blow up the heap.
  JsonDocument filter;
  filter["id"]      = true;
  filter["event"]   = true;
  filter["message"] = true;
  filter["tags"]    = true;

  JsonDocument doc;
  DeserializationError err =
      deserializeJson(doc, line, DeserializationOption::Filter(filter));
  if (err) {
    Serial.printf("[ntfy] could not parse line (%s)\n", err.c_str());
    return;
  }

  const char* event = doc["event"] | "";

  if (strcmp(event, "message") != 0) {
    // "open", "keepalive", "poll_request" -- nothing to do, but they prove the
    // connection is alive, which poll()'s watchdog already recorded.
    return;
  }

  // Only real messages advance the cursor. Keepalive events carry IDs too, and
  // feeding one back as ?since= would ask the server for a message it has
  // never heard of.
  String id = doc["id"] | "";
  if (!id.isEmpty()) lastId = id;

  // Commands and replies share one topic, so skip anything this device sent.
  for (JsonVariant tag : doc["tags"].as<JsonArray>()) {
    const char* t = tag.as<const char*>();
    if (t != nullptr && strcmp(t, NTFY_SELF_TAG) == 0) return;
  }

  if (alreadySeen(id)) {
    Serial.printf("[ntfy] ignoring duplicate %s\n", id.c_str());
    return;
  }

  String message = doc["message"] | "";
  if (message.isEmpty()) return;

  Serial.printf("[ntfy] << %s\n", message.c_str());
  if (onMessage) onMessage(message);
}

}  // namespace

void begin(MessageHandler handler) {
  onMessage = handler;
  lineBuf.reserve(256);
  nextAttemptMs = 0;

  // Allocated once. Settings cannot change without a reboot, so the client
  // type is fixed for the lifetime of the process.
  if (!stream) stream = makeClient();
}

bool connected() {
  return state == State::Streaming && stream && stream->connected();
}

void poll() {
  if (!stream) return;
  drainOutbox();

  if (state == State::Disconnected) {
    if ((int32_t)(millis() - nextAttemptMs) < 0) return;
    if (WiFi.status() != WL_CONNECTED) return;
    // Gate busy (a download, probe or publish is handshaking): try again on
    // the next pass, without growing the backoff.
    if (g_settings.ntfyTls && !tlsgate::acquire("ntfy stream", 0, tlsgate::Need::Handshake)) {
      return;
    }
    if (!connect()) {
      nextAttemptMs = millis() + backoffMs;
      backoffMs = min(backoffMs * 2, (uint32_t)NTFY_RECONNECT_MAX_MS);
    }
    return;
  }

  if (!stream->connected()) {
    disconnect("socket closed by peer");
    return;
  }

  while (stream->available()) {
    char c = (char)stream->read();
    lastDataMs = millis();

    if (c == '\n') {
      String line = lineBuf;
      lineBuf = "";
      line.trim();
      handleLine(line);
    } else if (c != '\r') {
      // Drop absurdly long lines rather than growing the heap without bound.
      if (lineBuf.length() < NTFY_MAX_LINE) {
        lineBuf += c;
      } else if (lineBuf.length() == NTFY_MAX_LINE) {
        Serial.println("[ntfy] line too long, discarding");
        lineBuf += ' ';   // marks it oversized so the rest is skipped
      }
    }
  }

  // ntfy keepalives arrive roughly every 45 s. Silence past the timeout means
  // a socket that looks open but will never deliver again.
  if (millis() - lastDataMs > NTFY_STREAM_TIMEOUT_MS) {
    disconnect("no data past keepalive timeout");
  }
}

namespace {

// ---- outbox ---------------------------------------------------------------

struct Msg {
  uint8_t  alert;          // 1 = Settings::alertTopic when set
  uint8_t  priority;       // 0 = no X-Priority header
  uint8_t  tries;
  uint32_t notBeforeMs;    // retry gap
  uint16_t len;
  char     title[NTFY_OUTBOX_TITLE];
  char     tags[32];
  char     body[NTFY_OUTBOX_BODY];
};

Msg*              ring  = nullptr;   // PSRAM
SemaphoreHandle_t ringMu = nullptr;
int               head  = 0;         // oldest
int               count = 0;

void copyTrunc(char* dst, size_t cap, const char* src, size_t len) {
  if (len >= cap) {
    memcpy(dst, src, cap - 4);
    memcpy(dst + cap - 4, "...", 4);
  } else {
    memcpy(dst, src, len);
    dst[len] = 0;
  }
}

bool enqueue(const String& message, const String& title, bool alert, uint8_t priority,
             const char* tags) {
  if (!ring || !ringMu) {
    Serial.printf("[ntfy] outbox not ready, dropped: %s\n", message.c_str());
    return false;
  }
  xSemaphoreTake(ringMu, portMAX_DELAY);
  if (count >= NTFY_OUTBOX_SLOTS) {
    xSemaphoreGive(ringMu);
    Serial.printf("[ntfy] outbox full, dropped: %s\n", title.c_str());
    return false;
  }
  Msg& m = ring[(head + count) % NTFY_OUTBOX_SLOTS];
  m.alert = alert ? 1 : 0;
  m.priority = priority;
  m.tries = 0;
  m.notBeforeMs = 0;
  copyTrunc(m.body, sizeof(m.body), message.c_str(), message.length());
  m.len = (uint16_t)strlen(m.body);
  copyTrunc(m.title, sizeof(m.title), title.c_str(), title.length());
  copyTrunc(m.tags, sizeof(m.tags), tags, strlen(tags));
  count++;
  xSemaphoreGive(ringMu);
  return true;
}

// The oldest message, or null. Producers only ever write past the tail, so
// the head slot is stable while it is being sent, without the lock.
Msg* front() {
  if (!ring) return nullptr;
  xSemaphoreTake(ringMu, portMAX_DELAY);
  Msg* m = count ? &ring[head] : nullptr;
  xSemaphoreGive(ringMu);
  return m;
}

void popFront() {
  xSemaphoreTake(ringMu, portMAX_DELAY);
  if (count) {
    head = (head + 1) % NTFY_OUTBOX_SLOTS;
    count--;
  }
  xSemaphoreGive(ringMu);
}

// One POST. The caller holds the TLS gate when TLS is on.
bool postMsg(const Msg& m) {
  // A short-lived client per publish, so the TLS buffers are freed straight
  // after rather than sitting alongside the subscribe connection.
  NetworkClient* client = makeClient();
  if (!client) return false;

  const String& topic = (m.alert && !g_settings.alertTopic.isEmpty()) ? g_settings.alertTopic
                                                                       : g_settings.ntfyTopic;
  int code;
  {
    // HTTPClient must be gone before the client is deleted. It keeps the
    // connection for reuse by default, so end() leaves its pointer to the
    // client set, and its destructor calls client->stop(). Destroyed after
    // `delete client`, that is a use-after-free -- harmless until the freed
    // block is reused, which a big blocklist load followed by the DoT
    // reconnect makes near-certain.
    HTTPClient http;
    String url = scheme() + g_settings.ntfyHost + ":" + String(g_settings.ntfyPort) + "/" + topic;

    if (!http.begin(*client, url)) {
      Serial.println("[ntfy] publish: begin failed");
      code = -1;
    } else {
      String tags = NTFY_SELF_TAG;   // lets poll() ignore our own echo
      if (m.tags[0]) tags += String(",") + m.tags;
      http.addHeader("Content-Type", "text/plain; charset=utf-8");
      http.addHeader("X-Tags", tags);
      if (m.title[0]) http.addHeader("X-Title", m.title);
      if (m.priority) http.addHeader("X-Priority", String(m.priority));
      if (!authHeader().isEmpty()) http.addHeader("Authorization", authHeader());

      code = http.POST((uint8_t*)m.body, m.len);
      http.end();
    }
  }
  delete client;

  if (code != HTTP_CODE_OK) {
    Serial.printf("[ntfy] publish failed (%d, internal %u free)\n", code,
                  (unsigned)tlsgate::freeInternal());
    return false;
  }
  Serial.printf("[ntfy] >> %s\n", m.body);
  return true;
}

// Sends the oldest message if it is due and the gate is free. waitMs is how
// long to wait for the gate. True if a message left (or was given up on).
bool sendOne(uint32_t waitMs) {
  Msg* m = front();
  if (!m || WiFi.status() != WL_CONNECTED) return false;
  if ((int32_t)(millis() - m->notBeforeMs) < 0) return false;

  bool tls = g_settings.ntfyTls;
  if (tls && !tlsgate::acquire("ntfy publish", waitMs, tlsgate::Need::Room)) return false;
  bool ok = postMsg(*m);
  if (tls) tlsgate::release();

  if (ok) {
    popFront();
    return true;
  }
  if (++m->tries >= NTFY_SEND_RETRIES) {
    Serial.printf("[ntfy] giving up on \"%s\" after %u tries\n", m->title, (unsigned)m->tries);
    popFront();
    return true;
  }
  m->notBeforeMs = millis() + NTFY_RETRY_GAP_MS;
  return false;
}

void drainOutbox() { sendOne(0); }

}  // namespace

void prealloc() {
  if (ring) return;
  ringMu = xSemaphoreCreateMutex();
  ring = (Msg*)heap_caps_calloc(NTFY_OUTBOX_SLOTS, sizeof(Msg), MALLOC_CAP_SPIRAM);
  if (!ring) ring = (Msg*)calloc(NTFY_OUTBOX_SLOTS, sizeof(Msg));   // no PSRAM: last resort
  Serial.printf("[ntfy] outbox %u x %u B\n", (unsigned)NTFY_OUTBOX_SLOTS, (unsigned)sizeof(Msg));
}

bool publish(const String& message, const String& title) {
  return enqueue(message, title, false, 0, "");
}

bool alert(const String& message, const String& title, uint8_t priority, const char* tags) {
  return enqueue(message, title, true, priority, tags ? tags : "");
}

int pending() {
  if (!ringMu) return 0;
  xSemaphoreTake(ringMu, portMAX_DELAY);
  int n = count;
  xSemaphoreGive(ringMu);
  return n;
}

bool flush(uint32_t timeoutMs) {
  uint32_t start = millis();
  while (pending() > 0 && millis() - start < timeoutMs) {
    if (!sendOne(500)) delay(50);
  }
  return pending() == 0;
}

}  // namespace ntfy
