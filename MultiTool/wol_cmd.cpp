// Wake-on-LAN commands. Ported from ESP32 WOL-NTFY 2.0.1 (Apache-2.0).

#include <Arduino.h>

#include "config.h"
#include "ntfy.h"
#include "settings.h"
#include "wol.h"
#include "wol_cmd.h"
#include "wolstore.h"

using commands::Ctx;

namespace wol_cmd {
namespace {

bool isAdd(const String& v) { return v == "add" || v == "save"; }
bool isRemove(const String& v) {
  return v == "rm" || v == "remove" || v == "del" || v == "delete" || v == "forget";
}
bool isList(const String& v) { return v == "list" || v == "ls"; }

void replyList() {
  if (wolstore::count() == 0) {
    ntfy::publish("No computers saved yet. Add one with:\n"
                  "<passphrase> wol add <name> <mac>",
                  "Saved computers");
    return;
  }
  String out = "Saved computers (" + String(wolstore::count()) + "/" +
               String(WOL_MAX_HOSTS) + ")\n";
  wolstore::forEach([&out](const String& name, const uint8_t* mac) {
    out += "\n" + name + "  " + wolstore::formatMac(mac);
  });
  ntfy::publish(out, "Saved computers");
}

void doWake(const String& target) {
  if (target.equalsIgnoreCase("all")) {
    if (wolstore::count() == 0) {
      ntfy::publish("Nothing to wake -- no computers saved.", "Wake");
      return;
    }
    String out = "Waking all " + String(wolstore::count()) + ":";
    wolstore::forEach([&out](const String& name, const uint8_t* mac) {
      bool ok = wol::wake(mac);
      out += "\n" + String(ok ? "sent " : "failed ") + name;
    });
    ntfy::publish(out, "Wake");
    return;
  }

  uint8_t mac[6];
  if (!wolstore::find(target, mac)) {
    ntfy::publish("No computer named \"" + target + "\". Send \"list\" to see saved ones.",
                  "Wake");
    return;
  }

  bool ok = wol::wake(mac);
  ntfy::publish(ok ? "Magic packet sent to " + target + " (" + wolstore::formatMac(mac) + ")"
                   : "Could not send the magic packet to " + target + ".",
                "Wake");
}

// args[0] = name, args[1] = mac
void doAdd(const Ctx& c, const String* args, int nargs) {
  if (!commands::requireSecret(c, "adding or removing computers", "Add")) return;
  if (nargs < 2) {
    ntfy::publish("Usage: <passphrase> wol add <name> <mac>", "Add");
    return;
  }


  uint8_t mac[6];
  if (!wolstore::parseMac(args[1], mac)) {
    ntfy::publish("\"" + args[1] + "\" is not a valid MAC address.", "Add");
    return;
  }

  switch (wolstore::add(args[0], mac)) {
    case wolstore::AddResult::Ok:
      ntfy::publish("Saved " + wolstore::normalizeName(args[0]) + " -> " +
                        wolstore::formatMac(mac),
                    "Add");
      break;
    case wolstore::AddResult::Duplicate:
      ntfy::publish("There is already a computer named \"" + args[0] + "\". Remove it first.",
                    "Add");
      break;
    case wolstore::AddResult::Full:
      ntfy::publish("Storage is full (" + String(WOL_MAX_HOSTS) + " computers).", "Add");
      break;
    case wolstore::AddResult::BadName:
      ntfy::publish("Bad name. Use up to " + String(WOL_NAME_LEN) +
                        " characters of a-z, 0-9, _ or -, and not a command word "
                        "(add, list, wake, all ...).",
                    "Add");
      break;
  }
}

void doRemove(const Ctx& c, const String* args, int nargs) {
  if (!commands::requireSecret(c, "adding or removing computers", "Remove")) return;
  if (nargs < 1) {
    ntfy::publish("Usage: <passphrase> wol rm <name>", "Remove");
    return;
  }
  if (wolstore::remove(args[0])) {
    ntfy::publish("Forgot " + wolstore::normalizeName(args[0]) + ".", "Remove");
  } else {
    ntfy::publish("No computer named \"" + args[0] + "\".", "Remove");
  }
}

void wakeUsage() { ntfy::publish("Usage: <passphrase> wake <name>  (or wake all)", "Wake"); }

}  // namespace

const char* helpLines() {
  return "wake <name>        send the magic packet (\"wake all\" for every one)\n"
         "list               saved computers\n"
         "wol add <name> <mac>  remember a computer\n"
         "wol rm <name>      forget a computer\n";
}

String statusSection() {
  return "computers " + String(wolstore::count()) + "/" + String(WOL_MAX_HOSTS);
}

bool handle(Ctx& c) {
  const String& v = c.verb;
  if (!(v == "wake" || v == "wol" || isList(v))) return false;
  if (!g_settings.wolOn) {
    commands::replyFeatureOff("Wake-on-LAN", "Wake-on-LAN");
    return true;
  }

  if (isList(v)) {
    replyList();
    return true;
  }

  if (v == "wake") {
    if (c.n < 2) wakeUsage();
    else doWake(c.tokens[1]);
    return true;
  }

  // "wol ..." -- a subcommand, or WOL-NTFY's "wol <name>" alias for wake.
  if (c.n < 2) {
    ntfy::publish(String("Wake-on-LAN commands:\n") + helpLines(), "Wake-on-LAN");
    return true;
  }
  String sub = c.tokens[1];
  sub.toLowerCase();
  const String* args = &c.tokens[2];
  int nargs = c.n - 2;

  if (isAdd(sub)) {
    doAdd(c, args, nargs);
  } else if (isRemove(sub)) {
    doRemove(c, args, nargs);
  } else if (isList(sub)) {
    replyList();
  } else if (sub == "wake") {
    if (nargs < 1) wakeUsage();
    else doWake(args[0]);
  } else {
    doWake(c.tokens[1]);
  }
  return true;
}

}  // namespace wol_cmd
