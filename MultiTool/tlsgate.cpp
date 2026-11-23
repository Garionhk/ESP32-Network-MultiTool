#include <Arduino.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "config.h"
#include "dot.h"
#include "tlsgate.h"

namespace tlsgate {
namespace {

SemaphoreHandle_t mutex = nullptr;
volatile bool     yieldDot = false;
const char*       who_ = "";

bool roomForTls() {
  return freeInternal() >= TLS_MIN_FREE && largestInternal() >= TLS_MIN_BLOCK;
}

}  // namespace

uint32_t freeInternal() {
  return (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

uint32_t largestInternal() {
  return (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

void begin() {
  if (!mutex) mutex = xSemaphoreCreateMutex();
}

bool acquire(const char* who, uint32_t waitMs, Need need) {
  if (!mutex) begin();
  if (xSemaphoreTake(mutex, pdMS_TO_TICKS(waitMs)) != pdTRUE) return false;
  who_ = who;

  bool askDot = need == Need::DotOff || (need == Need::Room && !roomForTls());
  if (askDot && dot::connected()) {
    yieldDot = true;
    Serial.printf("[tls] %s needs room (internal %u free, largest %u): DoT steps aside\n",
                  who, (unsigned)freeInternal(), (unsigned)largestInternal());
    uint32_t start = millis();
    while (dot::connected() && millis() - start < TLS_YIELD_WAIT_MS) delay(20);
  } else if (askDot) {
    yieldDot = true;   // keep it from reconnecting underneath us
  }

  if (need != Need::Handshake && !roomForTls()) {
    Serial.printf("[tls] %s: only %u free / %u largest internal -- trying anyway\n", who,
                  (unsigned)freeInternal(), (unsigned)largestInternal());
  }
  return true;
}

void release() {
  if (!mutex) return;
  yieldDot = false;
  who_ = "";
  xSemaphoreGive(mutex);
}

bool dotMustYield() { return yieldDot; }

bool busy() { return who_[0] != 0; }

const char* holder() { return who_; }

}  // namespace tlsgate
