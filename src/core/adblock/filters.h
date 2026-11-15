// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#ifndef TORFOX_CORE_ADBLOCK_FILTERS_H_
#define TORFOX_CORE_ADBLOCK_FILTERS_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace torfox::adblock {

// TorFox's ad blocker: a filter engine for the Adblock Plus rule syntax that
// EasyList is written in (https://help.adblockplus.org/hc/en-us/articles/360062733293).
//
// Understood:
//  - request rules: "||host^", "|", "*", "^", "@@" exceptions; the options
//    script, image, stylesheet, object, xmlhttprequest, subdocument, ping,
//    websocket, font, media, other and popup (and their "~" forms, plus the
//    usual short names), third-party / first-party, domain=, match-case,
//    important, and on exceptions document, elemhide, generichide and
//    genericblock;
//  - element hiding: "##selector", "domains##selector" and "#@#" exceptions,
//    with "~domain" exclusions and "name.*" domains.
// Anything else is ignored, rule by rule: a rule TorFox does not fully
// understand is never applied (so it is never applied more broadly than its
// authors meant). Rules that would block a top-level page ($document on a
// blocking rule) are ignored too: TorFox never blocks a page the user opens.
//
// Element-hiding selectors are checked before use: one that could close the
// style rule it is put in, open a comment or leave a string unterminated is
// ignored, so a selector can only ever hide elements.

// What a request is for, as the rules' type options name it.
enum Type : std::uint32_t {
  kScript = 1u << 0,
  kImage = 1u << 1,
  kStylesheet = 1u << 2,
  kObject = 1u << 3,
  kXmlHttpRequest = 1u << 4,
  kSubdocument = 1u << 5,
  kPing = 1u << 6,
  kWebSocket = 1u << 7,
  kFont = 1u << 8,
  kMedia = 1u << 9,
  kOther = 1u << 10,
  // A new window a page opens (checked before it opens).
  kPopup = 1u << 11,
};

struct Request {
  // The address requested (http, https, ws or wss), or for kPopup the
  // address the new window would open.
  std::string_view url;
  // The document the request is made for: |frame_url| is that frame's
  // document (for a frame being loaded, the document that contains it; for
  // a popup, the opener), |top_url| the top-level document. Either may be
  // empty when it is not known; |frame_url| then defaults to |top_url|.
  std::string_view frame_url;
  std::string_view top_url;
  Type type = kOther;
};

struct Verdict {
  bool block = false;
  // The rule that decided: the blocking rule, or the exception that let the
  // request through. Empty when no rule matched.
  std::string_view rule;
};

// What exception rules say about a whole document.
struct PageOptions {
  bool disabled = false;             // @@...$document: nothing is blocked or hidden.
  bool no_hiding = false;            // @@...$elemhide: nothing is hidden.
  bool no_generic_hiding = false;    // @@...$generichide: only site-specific hiding.
  bool no_generic_blocking = false;  // @@...$genericblock: only site-specific blocking.
};

class Engine {
 public:
  // Parses |rules| (one rule per element; comments and blank lines are
  // ignored). The engine keeps views into the strings, which must outlive it.
  explicit Engine(std::span<const std::string_view> rules);
  ~Engine();
  Engine(const Engine&) = delete;
  Engine& operator=(const Engine&) = delete;

  // Whether |request| is to be blocked. Thread safe.
  Verdict Check(const Request& request) const;

  // The exception rules for the document at |page_url|, inside the document
  // at |parent_url| (empty for a top-level document). Thread safe.
  PageOptions ForPage(std::string_view page_url, std::string_view parent_url = {}) const;

  // The element-hiding style sheet for a document whose host is |host|: one
  // "selector{display:none!important}" rule per line, site-specific rules
  // first. |generic| adds the rules meant for every site. Thread safe.
  std::string HidingCss(std::string_view host, bool generic) const;

  std::size_t request_rules() const;
  std::size_t hiding_rules() const;
  // Rules that were not used: not understood, or not applicable in TorFox.
  std::size_t ignored_rules() const;
  // Bytes the engine holds (indexes and the generic style sheet), roughly.
  std::size_t memory_bytes() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// Whether |selector| may be put in a style sheet (see above).
bool SafeSelector(std::string_view selector);

// The engine for the EasyList rules compiled into TorFox
// (third_party/easylist, scripts/gen_adblock.py). It is made on first use;
// other threads asking meanwhile wait for it. It lives until TorFox exits.
const Engine& BuiltIn();
std::string_view BuiltInCommit();
std::size_t BuiltInRuleCount();

}  // namespace torfox::adblock

#endif  // TORFOX_CORE_ADBLOCK_FILTERS_H_
