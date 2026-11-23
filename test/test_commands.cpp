// Host-side tests for the real commands.cpp. The transport, the blocklist,
// the DNS server and the scheduler are stubbed so the replies and the calls
// made can be inspected; settings.cpp is the genuine article.
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "Arduino.h"
#include "Preferences.h"
#include "WiFi.h"
#include "blocklist.h"
#include "commands.h"
#include "config.h"
#include "querylog.h"
#include "scheduler.h"
#include "settings.h"
#include "sinkhole.h"
#include "webconfig.h"
#include "wolstore.h"
#include "sitecheck.h"
#include "sitestore.h"

#define COMMAND_PASSPHRASE "test-passphrase"

SerialShim Serial;
FakeWiFi   WiFi;
FakeESP    ESP;
unsigned long fakeMillis = 1000;
bool g_restarted = false;
std::map<std::string, std::string> Preferences::backing;

// ---- stubs ----------------------------------------------------------------

static std::vector<std::string> sent;
static std::string restartReason;
static std::set<std::string> blocked = {"ads.example.com"};
static bool blockingOn = true;
static int  reloads = 0;
static bool reloadBusy = false;
static blocklist::State listState = blocklist::State::Ready;

namespace ntfy {
bool publish(const String& message, const String& title) {
  sent.push_back(std::string(title.s) + "|" + message.s);
  return true;
}
bool connected() { return true; }
static int flushes = 0;
bool flush(uint32_t) { flushes++; return true; }
}  // namespace ntfy

static int wakeCalls = 0;
namespace wol {
bool wake(const uint8_t mac[6]) { (void)mac; wakeCalls++; return true; }
}  // namespace wol

// An in-memory site store with the real rules (sitelogic), so site_cmd.cpp
// can be exercised without LittleFS; the check task is stubbed.
namespace sitestore {
static Site list[SITE_MAX];
static int n = 0;
int count() { return n; }
int snapshot(Site* out, int max) { int c = n < max ? n : max; memcpy(out, list, sizeof(Site) * c); return c; }
AddResult add(const String& rawName, const String& rawTarget, Site* site) {
  String name = sitelogic::sanitize(rawName);
  if (name.isEmpty()) return AddResult::BadName;
  String target; uint8_t type;
  if (!sitelogic::parseTarget(sitelogic::sanitize(rawTarget), type, target)) return AddResult::BadTarget;
  for (int i = 0; i < n; i++) if (String(list[i].name).equalsIgnoreCase(name)) return AddResult::Duplicate;
  if (n >= SITE_MAX) return AddResult::Full;
  Site& s = list[n++];
  memset(&s, 0, sizeof(s));
  strncpy(s.name, name.c_str(), sizeof(s.name) - 1);
  strncpy(s.target, target.c_str(), sizeof(s.target) - 1);
  s.type = type;
  if (site) *site = s;
  return AddResult::Ok;
}
bool remove(const String& name, String* removed) {
  for (int i = 0; i < n; i++) {
    if (String(list[i].name).equalsIgnoreCase(sitelogic::sanitize(name))) {
      if (removed) *removed = list[i].name;
      for (int j = i; j + 1 < n; j++) list[j] = list[j + 1];
      n--;
      return true;
    }
  }
  return false;
}
void clear() { n = 0; }
Snapshot::Snapshot() { sites = new Site[SITE_MAX]; n = snapshot(sites, SITE_MAX); }
Snapshot::~Snapshot() { delete[] sites; }
}  // namespace sitestore

static int checkNows = 0;
static bool passBusy = false;
namespace sitecheck {
bool checkNow() { if (passBusy) return false; checkNows++; return true; }
String summary() { return String("summary-line"); }
}  // namespace sitecheck

namespace webconfig {
String settingsUrl() { return String("http://192.168.1.50"); }
String apSsid()      { return String("MultiTool-Setup-TEST"); }
}  // namespace webconfig

namespace scheduler {
void   restart(const char* why) { restartReason = why; }
String lastResetReason() { return String("daily reboot"); }
String nextReboot()      { return String("04:00 EST"); }
String localTimeStr()    { return String("2026-10-25 14:00 EDT"); }
}  // namespace scheduler

namespace sinkhole {
Stats  stats() { return Stats{200, 50, 148, 2}; }
String recentBlocked() { return String("ads.example.com\ntrack.example.net"); }
String upstreamStatus() { return String("DNS-over-TLS, connected to one.one.one.one"); }
}  // namespace sinkhole

namespace blocklist {
Stats stats() {
  Stats s = {};
  s.state = listState;
  s.domains = 162431;
  s.enabled = blockingOn;
  return s;
}
bool enabled() { return blockingOn; }
void setEnabled(bool on) { blockingOn = on; }
bool contains(const String& d) { return blocked.count(d.s) > 0; }
bool explain(const String& d, String& via) {
  // Mimics subdomain matching: x.ads.example.com is blocked via its parent.
  for (std::string n = d.s;;) {
    if (blocked.count(n)) { via = String(n); return true; }
    size_t dot = n.find('.');
    if (dot == std::string::npos) break;
    n = n.substr(dot + 1);
  }
  via = String("");
  return false;
}
Edit block(const String& d) {
  if (blocked.count(d.s)) return Edit::Duplicate;
  blocked.insert(d.s);
  return Edit::Ok;
}
Edit unblock(const String& d) {
  if (!blocked.erase(d.s)) return Edit::NotFound;
  return Edit::Ok;
}
const char* editMessage(Edit e) { return e == Edit::Duplicate ? "already blocked" : "not in the blocklist"; }
bool requestReload(const char*) { if (reloadBusy) return false; reloads++; return true; }
bool clearCustom() { return requestReload("clear"); }
}  // namespace blocklist

namespace querylog {
int top(TopOf what, TopRow* out, int max) {
  if (max < 1) return 0;
  strcpy(out[0].key, what == TopOf::Clients ? "192.168.1.20" : "ads.example.com");
  out[0].count = 42;
  return 1;
}
}  // namespace querylog

// ---- harness --------------------------------------------------------------

static int failures = 0, checks = 0;

static void ok(bool cond, const char* what) {
  checks++;
  if (!cond) { failures++; printf("  FAIL: %s\n", what); }
}

static std::string lastReply() { return sent.empty() ? "" : sent.back(); }

static bool replyHas(const char* needle) {
  return lastReply().find(needle) != std::string::npos;
}

static void send(const char* msg) {
  sent.clear();
  wakeCalls = 0;
  restartReason.clear();
  g_restarted = false;
  commands::handle(String(msg));
}

#define P COMMAND_PASSPHRASE " "

static bool anyReplyLeaks() {
  for (auto& m : sent) if (m.find(COMMAND_PASSPHRASE) != std::string::npos) return true;
  return false;
}

int main() {
  wolstore::begin();
  g_settings.cmdPass   = COMMAND_PASSPHRASE;
  g_settings.ntfyTopic = "t";
  g_settings.wifiSsid  = "w";

  // --- the gate ---
  send("status");
  ok(sent.empty(), "no passphrase: silent");
  send("wrong-pass status");
  ok(sent.empty(), "wrong passphrase: silent");
  send("test-passphrasex status");
  ok(sent.empty(), "passphrase with extra chars: silent");
  send(COMMAND_PASSPHRASE);
  ok(sent.empty(), "passphrase alone: silent");
  send("TEST-Passphrase help");
  ok(replyHas("Help|"), "passphrase is case-insensitive");
  ok(!anyReplyLeaks(), "help does not echo the passphrase");

  // --- read-only commands ---
  send(P "status");
  ok(replyHas("Status|"), "status replies");
  ok(replyHas("192.168.1.50 (DHCP)"), "status shows IP and DHCP mode");
  ok(replyHas("AA:BB:CC:DD:EE:FF"), "DHCP status shows the MAC to reserve");
  ok(replyHas("162,431 domains"), "status shows list size with separators");
  ok(replyHas("next reboot 04:00 EST"), "status shows next reboot");
  ok(replyHas("upstream DNS-over-TLS"), "status shows the upstream path");
  ok(replyHas("last reset: daily reboot"), "status shows last reset reason");
  ok(replyHas("blocked 50 (25%)"), "status shows block rate");
  ok(!anyReplyLeaks(), "status does not echo the passphrase");

  g_settings.staticIp = "192.168.1.50";
  g_settings.staticGw = "192.168.1.1";
  g_settings.staticMask = "255.255.255.0";
  send(P "status");
  ok(replyHas("(static)") && !replyHas("reserve"), "static mode: no reservation hint");
  g_settings.staticIp = g_settings.staticGw = g_settings.staticMask = "";

  send(P "STATS");
  ok(replyHas("Stats|") && replyHas("track.example.net"), "stats lists recent blocks");

  send(P "check ads.example.com");
  ok(replyHas("IS blocked"), "check: blocked");
  send(P "check x.ads.example.com");
  ok(replyHas("IS blocked (via ads.example.com)"), "check names the parent that blocks it");
  send(P "top");
  ok(replyHas("Top|") && replyHas("42  ads.example.com") && replyHas("192.168.1.20"),
     "top lists names and clients");
  send(P "check good.example.com");
  ok(replyHas("NOT blocked"), "check: allowed");
  send(P "check");
  ok(replyHas("Usage"), "check without a domain shows usage");

  listState = blocklist::State::Loading;
  send(P "check ads.example.com");
  ok(replyHas("not loaded"), "check while loading says nothing is blocked");
  listState = blocklist::State::Ready;

  // --- editing ---
  send(P "block new.example.com");
  ok(replyHas("now blocked") && blocked.count("new.example.com"), "block adds");
  send(P "block new.example.com");
  ok(replyHas("already blocked"), "block twice reports duplicate");
  send(P "unblock new.example.com");
  ok(replyHas("now allowed") && !blocked.count("new.example.com"), "unblock removes");
  send(P "allow ads.example.com");
  ok(replyHas("now allowed"), "allow is an alias for unblock");
  send(P "unblock nothere.example.com");
  ok(replyHas("not in the blocklist"), "unblock of unknown name");
  send(P "block");
  ok(replyHas("Usage"), "block without a domain shows usage");

  send(P "off");
  ok(!blockingOn && replyHas("OFF"), "off disables blocking");
  send(P "on");
  ok(blockingOn && replyHas("ON"), "on enables blocking");

  send(P "reload");
  ok(reloads == 1 && replyHas("Downloading"), "reload queues a download");
  reloadBusy = true;
  send(P "reload");
  ok(reloads == 1 && replyHas("already running"), "reload while busy is refused");
  reloadBusy = false;

  send(P "clearcustom");
  ok(reloads == 2 && replyHas("deleted"), "clearcustom reloads");

  send(P "reboot");
  ok(restartReason == "ntfy request", "reboot restarts with a reason");

  send(P "frobnicate");
  ok(replyHas("Unknown command \"frobnicate\""), "unknown command");
  ok(!anyReplyLeaks(), "unknown-command reply does not echo the passphrase");

  // --- with no passphrase configured, state changes are refused ---
  g_settings.cmdPass = "";
  send("status");
  ok(replyHas("Status|"), "no passphrase set: read-only commands answer");
  send("block x.example.com");
  ok(replyHas("Set a command passphrase") && !blocked.count("x.example.com"),
     "no passphrase set: block refused");
  send("off");
  ok(blockingOn && replyHas("Set a command passphrase"), "no passphrase set: off refused");
  send("reboot");
  ok(restartReason.empty(), "no passphrase set: reboot refused");
  send("factoryreset");
  ok(!g_restarted, "no passphrase set: factoryreset refused");

  // --- wake-on-lan ---
  g_settings.cmdPass = COMMAND_PASSPHRASE;
  send(P "list");
  ok(replyHas("No computers saved") && replyHas("wol add"), "list when empty points at wol add");
  send(P "wol add desktop aa:bb:cc:dd:ee:ff");
  ok(replyHas("Saved desktop") && replyHas("aa:bb:cc:dd:ee:ff"), "wol add saves");
  ok(!anyReplyLeaks(), "wol add reply does not echo the passphrase");
  send(P "WOL SAVE Other 11-22-33-44-55-66");
  ok(replyHas("Saved other") && wolstore::count() == 2, "wol save alias, case-insensitive");
  send(P "wol add desktop 99:88:77:66:55:44");
  ok(replyHas("already a computer"), "wol add duplicate refused");
  send(P "wol add pc not-a-mac");
  ok(replyHas("not a valid MAC"), "wol add bad MAC refused");
  send(P "wol add list aa:bb:cc:dd:ee:01");
  ok(replyHas("command word") && wolstore::count() == 2, "wol add refuses a command word as name");
  send(P "wol add only-name");
  ok(replyHas("Usage"), "wol add without MAC shows usage");
  send(P "list");
  ok(replyHas("desktop") && replyHas("11:22:33:44:55:66"), "list shows both");
  send(P "ls");
  ok(replyHas(("Saved computers (2/" + std::to_string(WOL_MAX_HOSTS) + ")").c_str()), "ls is an alias for list");
  send(P "wol list");
  ok(replyHas("Saved computers"), "wol list works too");

  send(P "wake desktop");
  ok(wakeCalls == 1 && replyHas("Magic packet sent to desktop"), "wake by name");
  send(P "wake DESKTOP");
  ok(wakeCalls == 1, "wake is case-insensitive");
  send(P "wol desktop");
  ok(wakeCalls == 1 && replyHas("Magic packet"), "wol <name> still wakes (WOL-NTFY alias)");
  send(P "wol wake other");
  ok(wakeCalls == 1, "wol wake <name>");
  send(P "wake all");
  ok(wakeCalls == 2 && replyHas("Waking all 2"), "wake all hits every computer");
  send(P "wake ghost");
  ok(wakeCalls == 0 && replyHas("No computer named"), "wake unknown");
  send(P "wake");
  ok(wakeCalls == 0 && replyHas("Usage"), "wake without a name shows usage");
  send(P "wol");
  ok(replyHas("wol add <name> <mac>"), "bare wol lists its commands");

  send(P "status");
  ok(replyHas(("-- wake-on-lan --\ncomputers 2/" + std::to_string(WOL_MAX_HOSTS)).c_str()), "status has the WOL section");
  send(P "help");
  ok(replyHas("wake <name>") && replyHas("wol add <name> <mac>"), "help lists WOL commands");

  send(P "add pc aa:bb:cc:dd:ee:02");
  ok(replyHas("Say which list") && replyHas("wol add") && replyHas("site add") &&
     wolstore::count() == 2, "plain add asks which list");
  send(P "remove other");
  ok(replyHas("wol remove") && wolstore::count() == 2, "plain remove asks which list");

  send(P "wol rm other");
  ok(replyHas("Forgot other") && wolstore::count() == 1, "wol rm forgets");
  send(P "wol forget ghost");
  ok(replyHas("No computer named"), "wol forget unknown");

  g_settings.wolOn = false;
  send(P "wake desktop");
  ok(wakeCalls == 0 && replyHas("turned off"), "WOL off: wake refused");
  send(P "list");
  ok(replyHas("turned off"), "WOL off: list says so");
  send(P "status");
  ok(replyHas("-- wake-on-lan --\noff"), "WOL off: status says off");
  send(P "help");
  ok(!replyHas("wol add"), "WOL off: help omits it");
  g_settings.wolOn = true;

  g_settings.cmdPass = "";
  send("wake desktop");
  ok(wakeCalls == 1, "no passphrase set: wake still works");
  send("wol add newpc aa:bb:cc:dd:ee:09");
  ok(replyHas("Set a command passphrase") && wolstore::count() == 1,
     "no passphrase set: wol add refused");
  send("wol rm desktop");
  ok(replyHas("Set a command passphrase") && wolstore::count() == 1,
     "no passphrase set: wol rm refused");
  g_settings.cmdPass = COMMAND_PASSPHRASE;

  // --- site monitor ---
  g_settings.cmdPass = COMMAND_PASSPHRASE;
  send(P "site");
  ok(replyHas("site add <name> <target>"), "bare site lists its commands");
  send(P "site list");
  ok(replyHas("No sites monitored") && replyHas("site add"), "site list when empty");
  send(P "site add web example.com");
  ok(replyHas("Added: web (https://example.com)") && sitestore::count() == 1, "site add, bare host");
  ok(!anyReplyLeaks(), "site add does not echo the passphrase");
  send(P "SITE ADD nas 192.168.1.50");
  ok(replyHas("nas (192.168.1.50)") && sitestore::list[1].type == sitelogic::TYPE_PING,
     "site add IPv4 is a ping target, case-insensitive verb");
  send(P "site add Web https://other.example.com");
  ok(replyHas("Already present: Web") && sitestore::count() == 2, "site add duplicate, any case");
  send(P "site add bad ftp://x");
  ok(replyHas("Could not parse: bad|ftp://x"), "site add bad target");
  send(P "site add onlyname");
  ok(replyHas("Usage"), "site add without target shows usage");
  send(P "site add two words here");
  ok(replyHas("batch form"), "site add with a spaced name points at the batch form");

  send(P "site add File Server|10.0.0.5; Immich | immich.example.com ;broken");
  ok(replyHas("Added: File Server (10.0.0.5), Immich (https://immich.example.com)") &&
     replyHas("Could not parse: broken") && sitestore::count() == 4, "batch add with spaces");
  send(P "site ADD|Docs|docs.example.com");
  ok(replyHas("Added: Docs") && sitestore::count() == 5, "SiteCheck's ADD|Name|target form");
  send(P "site add |Wiki|wiki.example.com");
  ok(replyHas("Added: Wiki"), "SiteCheck's ADD | Name | target form");

  send(P "site list");
  ok(replyHas("web: unknown") && replyHas("nas (192.168.1.50): unknown") &&
     replyHas("summary-line"), "site list shows each site and the summary");
  send(P "status");
  ok(replyHas("-- site monitor --\nsummary-line"), "status has the site section");
  sitestore::list[1].state = sitelogic::STATE_DOWN;
  send(P "status");
  ok(replyHas("DOWN nas"), "status names sites that are down");
  send(P "help");
  ok(replyHas("site add A|t1;B|t2"), "help lists site commands");

  send(P "site check");
  ok(checkNows == 1 && replyHas("Checking 6"), "site check starts a pass");
  passBusy = true;
  send(P "site check");
  ok(checkNows == 1 && replyHas("already running"), "site check while running");
  passBusy = false;

  send(P "site rm file server; ghost");
  ok(replyHas("Removed: File Server") && replyHas("Unknown name: ghost") &&
     sitestore::count() == 5, "site rm by name with spaces, several, case-insensitive");
  send(P "site delete Docs");
  ok(replyHas("Removed: Docs"), "site delete alias");
  send(P "site rm");
  ok(replyHas("Usage"), "site rm without a name");
  send(P "site frob");
  ok(replyHas("Unknown site command"), "unknown site subcommand");

  g_settings.siteOn = false;
  send(P "site list");
  ok(replyHas("turned off"), "site monitor off: commands refused");
  send(P "status");
  ok(replyHas("-- site monitor --\noff"), "site monitor off: status says off");
  send(P "help");
  ok(!replyHas("site add"), "site monitor off: help omits it");
  g_settings.siteOn = true;

  g_settings.cmdPass = "";
  send("site list");
  ok(replyHas("web"), "no passphrase set: site list still answers");
  send("site add x x.example.com");
  ok(replyHas("Set a command passphrase") && sitestore::count() == 4, "no passphrase: site add refused");
  send("site rm web");
  ok(replyHas("Set a command passphrase") && sitestore::count() == 4, "no passphrase: site rm refused");
  g_settings.cmdPass = COMMAND_PASSPHRASE;

  // --- feature switched off ---
  g_settings.cmdPass = COMMAND_PASSPHRASE;
  g_settings.adblockOn = false;
  send(P "block y.example.com");
  ok(replyHas("turned off") && !blocked.count("y.example.com"), "adblock off: block refused");
  send(P "stats");
  ok(replyHas("turned off"), "adblock off: stats says so");
  send(P "status");
  ok(replyHas("-- ad blocker --\noff") && !replyHas("162,431"), "adblock off: status section says off");
  send(P "help");
  ok(!replyHas("unblock <domain>") && replyHas("reboot"), "adblock off: help omits its commands");
  send(P "frobnicate");
  ok(replyHas("Unknown command"), "adblock off: unknown verbs still unknown");
  g_settings.adblockOn = true;
  send(P "help");
  ok(replyHas("unblock <domain>"), "adblock on: help lists its commands");

  // --- factoryreset ---
  g_settings.cmdPass = COMMAND_PASSPHRASE;
  settings_save();
  send(P "factoryreset");
  ok(g_restarted, "factoryreset restarts");
  ok(ntfy::flushes > 0, "factoryreset flushes the outbox before restarting");
  ok(replyHas("MultiTool-Setup-TEST"), "factoryreset names the setup network");
  ok(!anyReplyLeaks(), "factoryreset does not echo the passphrase");
  ok(wolstore::count() == 1, "factoryreset keeps saved computers");
  ok(sitestore::count() == 4, "factoryreset keeps sites");
  g_settings = Settings();
  settings_begin();
  ok(!g_settings.provisioned, "factoryreset cleared the stored settings");

  g_settings.cmdPass = COMMAND_PASSPHRASE;
  send(P "factoryreset all");
  ok(g_restarted && wolstore::count() == 0, "factoryreset all forgets saved computers");
  ok(sitestore::count() == 0, "factoryreset all forgets sites");
  ok(replyHas("saved computers and sites"), "factoryreset all says so");

  printf("%d/%d checks passed\n", checks - failures, checks);
  return failures ? 1 : 0;
}
