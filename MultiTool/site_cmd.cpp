// Site-monitor commands. The batch protocol and its replies come from
// SiteCheck 2.0.0 (Apache-2.0).

#include <Arduino.h>

#include "config.h"
#include "ntfy.h"
#include "settings.h"
#include "site_cmd.h"
#include "sitecheck.h"
#include "sitestore.h"

using commands::Ctx;
using sitelogic::Site;

namespace site_cmd {
namespace {

void appendList(String& dst, const String& item) {
  if (!dst.isEmpty()) dst += ", ";
  dst += item;
}

String countLine() {
  return String(sitestore::count()) + "/" + String(SITE_MAX) + " sites";
}

// Splits on `sep`, trimming each piece and skipping empty ones.
template <typename F>
void forEachPiece(const String& text, char sep, F fn) {
  String piece;
  for (unsigned int i = 0; i <= text.length(); i++) {
    char c = i < text.length() ? text[i] : sep;
    if (c == sep) {
      piece.trim();
      if (!piece.isEmpty()) fn(piece);
      piece = "";
    } else {
      piece += c;
    }
  }
}

// One add, folded into the batch reply lists.
void addOne(const String& name, const String& target, String& added, String& exists,
            String& full, String& bad) {
  Site s;
  sitestore::AddResult r = sitestore::add(name, target, &s);
  switch (r) {
    case sitestore::AddResult::Ok:
    case sitestore::AddResult::IoError:
      appendList(added, String(s.name) + " (" + s.target + ")" +
                            (r == sitestore::AddResult::IoError ? " [not saved to flash]" : ""));
      break;
    case sitestore::AddResult::Duplicate: appendList(exists, name); break;
    case sitestore::AddResult::Full:      appendList(full, name); break;
    default:                              appendList(bad, name + "|" + target); break;
  }
}

void doAdd(const Ctx& c, const String& args) {
  if (!commands::requireSecret(c, "adding or removing sites", "Site add")) return;
  if (args.isEmpty()) {
    ntfy::publish("Usage: <passphrase> site add <name> <target>\n"
                  "or: <passphrase> site add Name1|target1;Name2|target2",
                  "Site add");
    return;
  }

  String added, exists, full, bad;
  if (args.indexOf("|") >= 0) {
    // SiteCheck's batch form. "ADD | Name | target" with spaces works too.
    String a = args;
    if (a.startsWith("|")) a = a.substring(1);
    forEachPiece(a, ';', [&](const String& entry) {
      int pipe = entry.indexOf("|");
      if (pipe < 0) {
        appendList(bad, entry);
        return;
      }
      String name = entry.substring(0, pipe);
      String target = entry.substring(pipe + 1);
      name.trim();
      target.trim();
      addOne(name, target, added, exists, full, bad);
    });
  } else {
    String name = commands::afterWords(args, 0);
    int sp = name.indexOf(" ");
    if (sp < 0) {
      ntfy::publish("Usage: <passphrase> site add <name> <target>", "Site add");
      return;
    }
    String target = name.substring(sp + 1);
    name = name.substring(0, sp);
    target.trim();
    if (target.indexOf(" ") >= 0) {
      ntfy::publish("A name with spaces needs the batch form: site add My Name|target",
                    "Site add");
      return;
    }
    addOne(name, target, added, exists, full, bad);
  }

  String reply;
  if (!added.isEmpty())  reply += "Added: " + added + "\n";
  if (!exists.isEmpty()) reply += "Already present: " + exists + "\n";
  if (!full.isEmpty())   reply += "Skipped, " + String(SITE_MAX) + "-site limit reached: " + full + "\n";
  if (!bad.isEmpty())    reply += "Could not parse: " + bad + "\n";
  if (reply.isEmpty())   reply = "Nothing to add.\n";
  reply += countLine();
  if (!added.isEmpty()) reply += "\nThey are checked on the next pass; send \"site check\" to check now.";
  ntfy::publish(reply, "Site add");
}

void doRemove(const Ctx& c, const String& args) {
  if (!commands::requireSecret(c, "adding or removing sites", "Site remove")) return;
  if (args.isEmpty()) {
    ntfy::publish("Usage: <passphrase> site rm <name>  (several: name1;name2)", "Site remove");
    return;
  }
  String removed, unknown;
  forEachPiece(args, ';', [&](const String& name) {
    String stored;
    if (sitestore::remove(name, &stored)) appendList(removed, stored);
    else appendList(unknown, name);
  });
  String reply;
  if (!removed.isEmpty()) reply += "Removed: " + removed + "\n";
  if (!unknown.isEmpty()) reply += "Unknown name: " + unknown + "\n";
  reply += countLine();
  ntfy::publish(reply, "Site remove");
}

void replyList() {
  int n;
  String out;
  {
    sitestore::Snapshot snap;
    n = snap.n;
    out = sitelogic::report(snap.sites, n);
  }
  if (n) out += "\n\n" + sitecheck::summary();
  else out += "\nAdd one with: <passphrase> site add <name> <target>";
  ntfy::publish(out, "Sites");
}

void doCheck() {
  if (sitestore::count() == 0) {
    ntfy::publish("No sites to check.", "Site check");
  } else if (sitecheck::checkNow()) {
    ntfy::publish("Checking " + String(sitestore::count()) +
                      " site(s) now. Only changes are reported; send \"site list\" afterwards "
                      "for the full picture.",
                  "Site check");
  } else {
    ntfy::publish("A check is already running.", "Site check");
  }
}

}  // namespace

const char* helpLines() {
  return "site list          monitored sites and their state\n"
         "site add <name> <target>  monitor a site (IP = ping, URL or host = HTTP)\n"
         "site add A|t1;B|t2  several at once\n"
         "site rm <name>     stop monitoring (name1;name2 for several)\n"
         "site check         check every site now\n";
}

String statusSection() {
  String out = sitecheck::summary();
  sitestore::Snapshot snap;
  for (int i = 0; i < snap.n; i++) {
    if (snap.sites[i].state == sitelogic::STATE_DOWN) out += "\n  DOWN " + String(snap.sites[i].name);
  }
  return out;
}

bool handle(Ctx& c) {
  if (c.verb != "site" && c.verb != "sites") return false;
  if (!g_settings.siteOn) {
    commands::replyFeatureOff("The site monitor", "Site monitor");
    return true;
  }
  if (c.n < 2) {
    ntfy::publish(String("Site monitor commands:\n") + helpLines(), "Site monitor");
    return true;
  }

  String sub = c.tokens[1];
  sub.toLowerCase();
  String args = commands::afterWords(c.rest, 1);

  if (sub == "add" || sub == "save") {
    doAdd(c, args);
  } else if (sub == "rm" || sub == "remove" || sub == "del" || sub == "delete") {
    doRemove(c, args);
  } else if (sub == "list" || sub == "ls" || sub == "status") {
    replyList();
  } else if (sub == "check") {
    doCheck();
  } else if (sub.startsWith("add|")) {
    // "site ADD|Name|target" -- SiteCheck's form with no space after the verb.
    doAdd(c, c.rest.substring(4));
  } else {
    ntfy::publish("Unknown site command \"" + c.tokens[1] + "\".\n" + helpLines(),
                  "Site monitor");
  }
  return true;
}

}  // namespace site_cmd
