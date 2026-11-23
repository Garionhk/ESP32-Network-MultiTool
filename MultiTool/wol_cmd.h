#pragma once

#include <Arduino.h>

#include "cmdutil.h"

// Wake-on-LAN commands, ported from ESP32 WOL-NTFY 2.0.1:
//
//   wake <name> | wake all      send the magic packet
//   list                        saved computers
//   wol add <name> <mac>        remember a computer  (save)
//   wol rm <name>               forget one           (remove, del, delete, forget)
//   wol list / wol wake <name>  the same, with the feature word
//   wol <name>                  WOL-NTFY's old alias for wake
//
// Plain "add" / "remove" are not taken here: the site monitor has them too,
// so the router answers those with a hint.

namespace wol_cmd {

// True if the verb belongs to Wake-on-LAN (whether or not it is enabled); the
// reply has then been published.
bool handle(commands::Ctx& c);

const char* helpLines();

// "computers 3/16". No trailing newline.
String statusSection();

}  // namespace wol_cmd
