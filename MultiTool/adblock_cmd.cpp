#include <Arduino.h>
#include <time.h>

#include "adblock_cmd.h"
#include "blocklist.h"
#include "config.h"
#include "ntfy.h"
#include "querylog.h"
#include "settings.h"
#include "sinkhole.h"
#include "util.h"

using commands::Ctx;

namespace adblock_cmd {
namespace {

String blocklistLine() {
  blocklist::Stats b = blocklist::stats();
  String out = "blocklist ";
  switch (b.state) {
    case blocklist::State::Ready:
      out += withCommas(b.domains) + " domains";
      if (b.loadedAt > 0 && time(nullptr) > (time_t)NTP_MIN_VALID_EPOCH) {
        out += ", downloaded " + fmtDuration((uint32_t)(time(nullptr) - b.loadedAt)) + " ago";
      }
      if (b.fromSnapshot) out += " (from flash)";
      break;
    case blocklist::State::Loading:
      out += "downloading " + String(b.progress) + "% -- not blocking until done";
      break;
    case blocklist::State::Failed:
      out += "FAILED to load -- not blocking";
      break;
    case blocklist::State::Empty:
      out += "not loaded yet";
      break;
  }
  return out;
}

String queriesLine() {
  sinkhole::Stats d = sinkhole::stats();
  String out = "queries " + withCommas(d.queries) + ", blocked " + withCommas(d.blocked);
  if (d.queries > 0) out += " (" + String((unsigned long)(d.blocked * 100ULL / d.queries)) + "%)";
  out += ", cached " + withCommas(d.cached);
  if (d.failed > 0) out += ", upstream failures " + withCommas(d.failed);
  if (d.dropped > 0) out += ", dropped (queue full) " + withCommas(d.dropped);
  return out;
}

String topLines(querylog::TopOf what, int n) {
  querylog::TopRow rows[5];
  int got = querylog::top(what, rows, n < 5 ? n : 5);
  String out;
  for (int i = 0; i < got; i++) out += "\n  " + String(rows[i].count) + "  " + rows[i].key;
  return got ? out : String("\n  (nothing yet)");
}

void replyStats() {
  String out = queriesLine() + "\n" + blocklistLine();
  String recent = sinkhole::recentBlocked();
  out += recent.isEmpty() ? "\n\nNothing blocked yet." : "\n\nRecently blocked:\n" + recent;
  ntfy::publish(out, "Stats");
}

void doCheck(const String& domain) {
  if (blocklist::stats().state != blocklist::State::Ready) {
    ntfy::publish("The blocklist is not loaded right now, so nothing is blocked.", "Check");
    return;
  }
  String via;
  bool hit = blocklist::explain(domain, via);
  String msg = domain + (hit ? " IS blocked" : " is NOT blocked");
  if (!via.isEmpty() && via != domain) {
    msg += hit ? " (via " + via + ")" : " (you allowed " + via + ")";
  }
  msg += ".";
  if (hit && !blocklist::enabled()) msg += " (Blocking is off at the moment.)";
  ntfy::publish(msg, "Check");
}

void doEdit(const Ctx& c, bool isBlock) {
  const char* title = isBlock ? "Block" : "Unblock";
  if (!commands::requireSecret(c, "changing the blocklist", title)) return;
  if (c.n < 2) {
    ntfy::publish(String("Usage: <passphrase> ") + (isBlock ? "block" : "unblock") + " <domain>",
                  title);
    return;
  }
  const String& d = c.tokens[1];
  blocklist::Edit r = isBlock ? blocklist::block(d) : blocklist::unblock(d);
  if (r == blocklist::Edit::Ok) {
    ntfy::publish(d + (isBlock ? " is now blocked." : " is now allowed."), title);
  } else {
    ntfy::publish(d + ": " + blocklist::editMessage(r) + ".", title);
  }
}

void doEnable(const Ctx& c, bool on) {
  if (!commands::requireSecret(c, "turning blocking on or off", "Blocking")) return;
  blocklist::setEnabled(on);
  ntfy::publish(on ? "Blocking is ON."
                   : "Blocking is OFF until you send \"on\" or the device reboots.",
                "Blocking");
}

void doReload(const Ctx& c) {
  if (!commands::requireSecret(c, "reloading the blocklist", "Reload")) return;
  if (blocklist::requestReload("ntfy request")) {
    ntfy::publish("Downloading the blocklist. Queries pass through unblocked until it "
                  "finishes; you will get a message when it is done.",
                  "Reload");
  } else {
    ntfy::publish("A download is already running.", "Reload");
  }
}

void doClearCustom(const Ctx& c) {
  if (!commands::requireSecret(c, "clearing the custom list", "Custom list")) return;
  if (blocklist::clearCustom()) {
    ntfy::publish("Custom blocks and unblocks deleted. Reloading the blocklist without them.",
                  "Custom list");
  } else {
    ntfy::publish("The blocklist is busy -- try again in a minute.", "Custom list");
  }
}

bool isMine(const String& v) {
  return v == "stats" || v == "top" || v == "check" || v == "block" || v == "unblock" ||
         v == "allow" || v == "whitelist" || v == "on" || v == "enable" || v == "off" ||
         v == "disable" || v == "reload" || v == "refresh" || v == "clearcustom";
}

}  // namespace

const char* helpLines() {
  return "stats              query counts and recently blocked names\n"
         "top                most blocked names and busiest devices\n"
         "check <domain>     is it blocked, and by which entry?\n"
         "block <domain>     add to your custom blocklist\n"
         "unblock <domain>   allow it through\n"
         "on / off           turn blocking on or off until reboot\n"
         "reload             download the blocklist again now\n"
         "clearcustom        forget every block / unblock you added\n";
}

String statusSection() {
  const Settings& s = g_settings;
  return "upstream " + sinkhole::upstreamStatus() + "\n" +
         blocklistLine() + "\n" +
         "blocking " + String(blocklist::enabled() ? "on" : "OFF") + "\n" +
         queriesLine() + "\n" +
         "list refresh " + (s.refreshHour < 10 ? "0" : "") + String(s.refreshHour) + ":00";
}

bool handle(Ctx& c) {
  const String& v = c.verb;
  if (!isMine(v)) return false;
  if (!g_settings.adblockOn) {
    commands::replyFeatureOff("The ad blocker", "Ad blocker");
    return true;
  }

  if (v == "stats") {
    replyStats();

  } else if (v == "top") {
    ntfy::publish("Most blocked:" + topLines(querylog::TopOf::BlockedNames, 5) +
                      "\n\nBusiest devices:" + topLines(querylog::TopOf::Clients, 5) +
                      "\n\nSince the last reboot.",
                  "Top");

  } else if (v == "check") {
    if (c.n < 2) {
      ntfy::publish("Usage: <passphrase> check <domain>", "Check");
      return true;
    }
    doCheck(c.tokens[1]);

  } else if (v == "block") {
    doEdit(c, true);

  } else if (v == "unblock" || v == "allow" || v == "whitelist") {
    doEdit(c, false);

  } else if (v == "on" || v == "enable") {
    doEnable(c, true);

  } else if (v == "off" || v == "disable") {
    doEnable(c, false);

  } else if (v == "reload" || v == "refresh") {
    doReload(c);

  } else if (v == "clearcustom") {
    doClearCustom(c);
  }
  return true;
}

}  // namespace adblock_cmd
