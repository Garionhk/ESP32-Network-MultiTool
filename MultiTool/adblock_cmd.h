#pragma once

#include <Arduino.h>

#include "cmdutil.h"

// The ad blocker's ntfy commands: stats, top, check, block, unblock, on, off,
// reload, clearcustom. Moved unchanged out of commands.cpp.

namespace adblock_cmd {

// True if the verb belongs to the ad blocker (whether or not it is enabled);
// the reply has then been published.
bool handle(commands::Ctx& c);

// Lines for the combined help message.
const char* helpLines();

// The ad blocker's part of the status report: upstream, blocklist, blocking,
// query counts. No trailing newline.
String statusSection();

}  // namespace adblock_cmd
