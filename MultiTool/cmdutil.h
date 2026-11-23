#pragma once

#include <Arduino.h>

// What the command router hands each feature's handler: the message split
// into words with the passphrase already removed, and the lowercased verb.

namespace commands {

const int MAX_TOKENS = 8;

struct Ctx {
  String tokens[MAX_TOKENS];   // tokens[0] is the verb as typed
  int    n = 0;
  String verb;                 // tokens[0], lowercased
  String secret;               // the configured passphrase, trimmed; may be empty
  String rest;                 // the raw text after the verb, trimmed -- for
                               // arguments that may hold spaces ("A|t1; B|t2")
};

// `text` with its first `words` whitespace-separated words removed, trimmed.
String afterWords(const String& text, int words);

// State-changing commands ALWAYS need a passphrase to be set, even though the
// router lets everything through when none is -- ntfy.sh topics are public,
// so an ungated topic stays read-only. Publishes the refusal itself.
bool requireSecret(const Ctx& c, const char* what, const char* title);

// Publishes "<Feature> is turned off in the settings." for a disabled module.
void replyFeatureOff(const char* feature, const char* title);

}  // namespace commands
