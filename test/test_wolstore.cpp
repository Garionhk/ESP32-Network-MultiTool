// Host-side tests for the real wolstore.cpp (compiled in, not copied).
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "Arduino.h"
#include "Preferences.h"
#include "config.h"
#include "wolstore.h"

SerialShim Serial;
std::map<std::string, std::string> Preferences::backing;

static int failures = 0;
static int checks = 0;

static void ok(bool cond, const char* what) {
  checks++;
  if (!cond) { failures++; printf("  FAIL: %s\n", what); }
}

static String mac2str(const uint8_t m[6]) { return wolstore::formatMac(m); }

int main() {
  uint8_t mac[6];

  printf("-- parseMac accepts the formats people type\n");
  ok(wolstore::parseMac("aa:bb:cc:dd:ee:ff", mac) && mac2str(mac) == String("aa:bb:cc:dd:ee:ff"), "colon form");
  ok(wolstore::parseMac("AA-BB-CC-DD-EE-FF", mac) && mac2str(mac) == String("aa:bb:cc:dd:ee:ff"), "dash form, uppercase");
  ok(wolstore::parseMac("aabbccddeeff", mac) && mac2str(mac) == String("aa:bb:cc:dd:ee:ff"), "bare hex");
  ok(wolstore::parseMac("00:11:22:33:44:55", mac) && mac[0] == 0x00 && mac[5] == 0x55, "byte values");

  printf("-- parseMac rejects malformed input\n");
  ok(!wolstore::parseMac("", mac), "empty");
  ok(!wolstore::parseMac("aa:bb:cc:dd:ee", mac), "too short");
  ok(!wolstore::parseMac("aa:bb:cc:dd:ee:ff:00", mac), "too long");
  ok(!wolstore::parseMac("gg:bb:cc:dd:ee:ff", mac), "non-hex digit");
  ok(!wolstore::parseMac("aa:bb:cc:dd:ee:fz", mac), "trailing non-hex");

  printf("-- normalizeName\n");
  ok(wolstore::normalizeName("Desktop") == String("desktop"), "lowercased");
  ok(wolstore::normalizeName("  nas  ") == String("nas"), "trimmed");
  ok(wolstore::normalizeName("my-pc_2") == String("my-pc_2"), "dash and underscore allowed");
  ok(wolstore::normalizeName("").isEmpty(), "empty rejected");
  ok(wolstore::normalizeName("has space").isEmpty(), "space rejected");
  ok(wolstore::normalizeName("bad!").isEmpty(), "punctuation rejected");
  ok(wolstore::normalizeName("123456789012345678901").isEmpty(), "21 chars rejected");
  ok(wolstore::normalizeName("12345678901234567890") == String("12345678901234567890"), "20 chars ok");

  printf("-- add / find / remove\n");
  wolstore::begin();
  ok(wolstore::count() == 0, "starts empty");

  wolstore::parseMac("aa:bb:cc:dd:ee:ff", mac);
  ok(wolstore::add("Desktop", mac) == wolstore::AddResult::Ok, "add ok");
  ok(wolstore::count() == 1, "count is 1");
  ok(wolstore::add("desktop", mac) == wolstore::AddResult::Duplicate, "duplicate rejected case-insensitively");
  ok(wolstore::add("bad name!", mac) == wolstore::AddResult::BadName, "bad name rejected");

  uint8_t got[6];
  ok(wolstore::find("DESKTOP", got) && mac2str(got) == String("aa:bb:cc:dd:ee:ff"), "find is case-insensitive");
  ok(!wolstore::find("nope", got), "missing host not found");

  printf("-- capacity\n");
  for (int i = 1; i < WOL_MAX_HOSTS; i++) {
    char n[16]; snprintf(n, sizeof n, "host%d", i);
    ok(wolstore::add(n, mac) == wolstore::AddResult::Ok, "fill to capacity");
  }
  ok(wolstore::count() == WOL_MAX_HOSTS, "at WOL_MAX_HOSTS");
  ok(wolstore::add("overflow", mac) == wolstore::AddResult::Full, "rejects past WOL_MAX_HOSTS");

  printf("-- persistence across a simulated reboot\n");
  wolstore::begin();   // re-reads the backing store
  ok(wolstore::count() == WOL_MAX_HOSTS, "list survived reboot");
  ok(wolstore::find("desktop", got) && mac2str(got) == String("aa:bb:cc:dd:ee:ff"), "entry intact after reboot");

  printf("-- remove\n");
  ok(wolstore::remove("desktop"), "remove existing");
  ok(!wolstore::find("desktop", got), "gone after remove");
  ok(wolstore::count() == WOL_MAX_HOSTS - 1, "count decremented");
  ok(!wolstore::remove("desktop"), "removing twice fails");
  ok(wolstore::find("host1", got), "neighbour survived the shift");
  ok(wolstore::find("host15", got), "last entry survived the shift");

  wolstore::begin();
  ok(wolstore::count() == WOL_MAX_HOSTS - 1, "removal persisted");

  printf("-- corrupt blob does not wedge the device\n");
  // The shim namespaces keys, so poke the same slot wolstore.cpp writes.
  Preferences::backing[std::string(WOL_NVS_NAMESPACE) + "/" + WOL_NVS_KEY] =
      "{not json at all";
  wolstore::begin();
  ok(wolstore::count() == 0, "recovers to empty list");
  wolstore::parseMac("11:22:33:44:55:66", mac);
  ok(wolstore::add("after", mac) == wolstore::AddResult::Ok, "still usable after corruption");

  printf("-- command words cannot be names\n");
  ok(wolstore::add("list", mac) == wolstore::AddResult::BadName, "\"list\" rejected");
  ok(wolstore::add("ALL", mac) == wolstore::AddResult::BadName, "\"all\" rejected, any case");
  ok(wolstore::add("wake", mac) == wolstore::AddResult::BadName, "\"wake\" rejected");
  ok(wolstore::add("listing", mac) == wolstore::AddResult::Ok, "a name merely starting with one is fine");

  printf("-- WOL_MAX_HOSTS worst case\n");
  {
    wolstore::clear();
    for (int i = 0; i < WOL_MAX_HOSTS; i++) {
      char n[WOL_NAME_LEN + 1];
      snprintf(n, sizeof n, "pc%018d", i);   // exactly WOL_NAME_LEN characters
      uint8_t m[6] = {0xaa, 0xbb, 0xcc, 0xdd, 0xee, (uint8_t)i};
      ok(wolstore::add(n, m) == wolstore::AddResult::Ok, "fill with longest names");
    }
    size_t blob = Preferences::backing[std::string(WOL_NVS_NAMESPACE) + "/" + WOL_NVS_KEY].size();
    printf("  NVS blob at %d computers: %zu bytes (NVS string limit 4000)\n", WOL_MAX_HOSTS, blob);
    ok(blob < 3500, "full list stays well inside the 4000-byte NVS string limit");
    // The same text wol_cmd's "list" reply builds.
    String reply = "Saved computers (" + String(WOL_MAX_HOSTS) + "/" + String(WOL_MAX_HOSTS) + ")\n";
    wolstore::forEach([&reply](const String& name, const uint8_t* mac) {
      reply += "\n" + name + "  " + wolstore::formatMac(mac);
    });
    printf("  list reply: %u bytes (limit %d)\n", reply.length(), NTFY_OUTBOX_BODY);
    ok(reply.length() < NTFY_OUTBOX_BODY, "full list reply fits one ntfy message");
  }

  printf("-- clear\n");
  wolstore::clear();
  ok(wolstore::count() == 0, "clear empties the list");
  wolstore::begin();
  ok(wolstore::count() == 0, "clear persisted");

  printf("%d/%d checks passed\n", checks - failures, checks);
  return failures ? 1 : 0;
}
