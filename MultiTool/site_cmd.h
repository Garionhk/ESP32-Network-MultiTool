#pragma once

#include <Arduino.h>

#include "cmdutil.h"

// Site-monitor commands. SiteCheck's protocol, behind the word "site":
//
//   site add <name> <target>          one site
//   site add Name1|target1;Name2|t2   SiteCheck's batch form (names may hold spaces)
//   site rm <name>[;name2...]         (remove, del, delete)
//   site list                         every site and its state (ls, status)
//   site check                        run a pass now
//
// A target is an IPv4 address (pinged), an http:// or https:// URL, or a
// host name, which becomes https://<name>.

namespace site_cmd {

// True if the verb belongs to the site monitor (whether or not it is
// enabled); the reply has then been published.
bool handle(commands::Ctx& c);

const char* helpLines();

// "3 sites: 2 up, 1 down; ..." plus one line per site that is down.
String statusSection();

}  // namespace site_cmd
