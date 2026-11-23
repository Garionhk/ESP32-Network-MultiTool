#include <Arduino.h>
#include <WiFi.h>
#include <time.h>

#include "adblock_cmd.h"
#include "cmdutil.h"
#include "commands.h"
#include "config.h"
#include "ntfy.h"
#include "scheduler.h"
#include "settings.h"
#include "util.h"
#include "webconfig.h"
#include "wol_cmd.h"
#include "wolstore.h"

// The command router. It owns the passphrase gate and the commands that
// belong to the whole device (help, status, reboot, factoryreset); every
// other verb is offered to each feature's handler in turn.

namespace commands {
namespace {

int tokenize(const String& input, String out[MAX_TOKENS]) {
  int    n = 0;
  String current;

  for (unsigned int i = 0; i <= input.length() && n < MAX_TOKENS; i++) {
    char c = (i < input.length()) ? input[i] : ' ';
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
      if (current.length() > 0) {
        out[n++] = current;
        current  = "";
      }
    } else {
      current += c;
    }
  }
  return n;
}

// Compares without an early exit, so response time does not leak how many
// leading characters were right. Case-insensitive, so phone
// auto-capitalisation doesn't lock you out.
bool passphraseOk(const String& given, const String& expected) {
  String a = given, b = expected;
  a.toLowerCase();
  b.toLowerCase();

  uint32_t diff = (uint32_t)(a.length() ^ b.length());
  for (unsigned int i = 0; i < b.length(); i++) {
    char g = (i < a.length()) ? a[i] : '\0';
    diff |= (uint32_t)(g ^ b[i]);
  }
  return diff == 0;
}

void replyHelp() {
  String help =
      "MultiTool -- commands\n"
      "\n"
      "Start every command with your passphrase:\n"
      "<passphrase> <command>\n"
      "\n"
      "status             address, features, schedule, memory\n";
  if (g_settings.adblockOn) help += adblock_cmd::helpLines();
  if (g_settings.wolOn) help += wol_cmd::helpLines();
  help +=
      "reboot             restart now\n"
      "factoryreset       wipe settings, reboot into setup\n"
      "                   (\"factoryreset all\" also forgets saved computers)\n"
      "help               this message";
  if (g_settings.adblockOn) help += "\n\nA leading www. is ignored, so check example.com covers both.";
  if (g_settings.wolOn) {
    help += "\n\nMAC accepts aa:bb:cc:dd:ee:ff, aa-bb-cc-dd-ee-ff or aabbccddeeff. Computer "
            "names: up to " + String(WOL_NAME_LEN) + " of a-z 0-9 _ -.";
  }
  ntfy::publish(help, "Help");
}

void doReboot(const Ctx& c) {
  if (!requireSecret(c, "rebooting", "Reboot")) return;
  scheduler::restart("ntfy request");
}

void doFactoryReset(const Ctx& c) {
  if (!requireSecret(c, "using factoryreset", "Factory reset")) return;
  bool all = c.n >= 2 && c.tokens[1].equalsIgnoreCase("all");
  ntfy::publish(String("Clearing settings") + (all ? " and saved computers" : "") +
                    ". Rebooting into setup mode -- join the \"" + webconfig::apSsid() +
                    "\" network to reconfigure. The blocklist and custom list stay in flash.",
                "Factory reset");
  ntfy::flush(5000);    // the reply has to leave before the restart
  if (all) wolstore::clear();
  settings_clear();
  ESP.restart();
}

}  // namespace

bool requireSecret(const Ctx& c, const char* what, const char* title) {
  if (!c.secret.isEmpty()) return true;
  ntfy::publish(String("Set a command passphrase before ") + what + ".", title);
  return false;
}

void replyFeatureOff(const char* feature, const char* title) {
  ntfy::publish(String(feature) + " is turned off in the settings (" +
                    webconfig::settingsUrl() + ").",
                title);
}

String statusReport() {
  const Settings& s = g_settings;
  bool dhcp = settings_useDhcp(s);

  String out =
      String(APP_NAME " v" APP_VERSION) + "\n" +
      "ip " + WiFi.localIP().toString() + (dhcp ? " (DHCP)" : " (static)") + "\n";
  if (dhcp) {
    out += "mac " + WiFi.macAddress() + " -- reserve this IP for it in your router\n";
  }
  if (s.ipv6) {
    String v6 = WiFi.globalIPv6().toString();
    if (v6 != "::" && !v6.isEmpty()) out += "ipv6 " + v6 + "\n";
  }
  out += "gw " + WiFi.gatewayIP().toString() + "\n" +
         "wifi " + WiFi.SSID() + " (" + String(WiFi.RSSI()) + " dBm)\n" +
         "settings " + webconfig::settingsUrl() + "\n" +
         "up " + fmtDuration(millis() / 1000) + ", last reset: " +
         scheduler::lastResetReason() + "\n" +
         "time " + scheduler::localTimeStr() + "\n" +
         "next reboot " + scheduler::nextReboot() + "\n" +
         "ntfy " + String(ntfy::connected() ? "connected" : "reconnecting") + "\n" +
         "heap " + fmtBytes(ESP.getFreeHeap()) + ", psram " + fmtBytes(ESP.getFreePsram()) +
         " free";

  out += "\n\n-- ad blocker --\n";
  out += s.adblockOn ? adblock_cmd::statusSection() : String("off");
  out += "\n\n-- wake-on-lan --\n";
  out += s.wolOn ? wol_cmd::statusSection() : String("off");
  return out;
}

void handle(const String& message) {
  Ctx c;
  c.n = tokenize(message, c.tokens);
  if (c.n == 0) return;

  // --- passphrase gate --------------------------------------------------
  // The passphrase prefixes every command. Anything else is ignored without a
  // reply, so a stranger on the topic gets no hint the device is listening.
  c.secret = g_settings.cmdPass;
  c.secret.trim();
  if (!c.secret.isEmpty()) {
    if (!passphraseOk(c.tokens[0], c.secret)) return;
    for (int i = 1; i < c.n; i++) c.tokens[i - 1] = c.tokens[i];
    c.tokens[--c.n] = "";
    if (c.n == 0) return;
  }

  c.verb = c.tokens[0];
  c.verb.toLowerCase();
  const String& v = c.verb;

  if (v == "help" || v == "?" || v == "commands") {
    replyHelp();
  } else if (v == "status" || v == "stat") {
    ntfy::publish(statusReport(), "Status");
  } else if (v == "reboot" || v == "restart") {
    doReboot(c);
  } else if (v == "factoryreset") {
    doFactoryReset(c);
  } else if (adblock_cmd::handle(c) || wol_cmd::handle(c)) {
    // handled
  } else if (v == "add" || v == "save" || v == "rm" || v == "remove" || v == "del" ||
             v == "delete" || v == "forget") {
    // Both Wake-on-LAN and the site monitor keep lists, so say which.
    ntfy::publish("Say which list: \"wol " + v + " ...\" for a computer to wake, or "
                  "\"site " + v + " ...\" for a site to monitor.",
                  "Which list?");
  } else {
    ntfy::publish("Unknown command \"" + c.tokens[0] + "\". Send \"<passphrase> help\" "
                  "for the list.",
                  "Unknown command");
  }
}

}  // namespace commands
