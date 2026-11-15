// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "src/core/adblock/sites.h"

#include <algorithm>

namespace torfox::adblock {

namespace {

constexpr std::size_t kMaxSites = 1000;

std::string_view Trim(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) {
    s.remove_prefix(1);
  }
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) {
    s.remove_suffix(1);
  }
  return s;
}

// |s| as a lowercase host name without a trailing dot, or "" if it is not one.
std::string HostName(std::string_view s) {
  s = Trim(s);
  if (!s.empty() && s.back() == '.') {
    s.remove_suffix(1);
  }
  if (s.empty() || s.size() > 253) {
    return {};
  }
  std::string out;
  out.reserve(s.size());
  std::size_t label = 0;
  for (char c : s) {
    if (c == '.') {
      if (label == 0 || out.back() == '-') {
        return {};
      }
      label = 0;
      out.push_back(c);
      continue;
    }
    if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c - 'A' + 'a');
    }
    const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || (c == '-' && label > 0);
    if (!ok || ++label > 63) {
      return {};
    }
    out.push_back(c);
  }
  if (label == 0 || out.back() == '-') {
    return {};
  }
  return out;
}

bool SiteCovers(std::string_view site, std::string_view host) {
  return host.size() >= site.size() && host.ends_with(site) &&
         (host.size() == site.size() || host[host.size() - site.size() - 1] == '.');
}

std::vector<std::string_view> Lines(std::string_view text) {
  std::vector<std::string_view> out;
  while (!text.empty()) {
    const std::size_t nl = text.find('\n');
    out.push_back(text.substr(0, nl));
    if (nl == std::string_view::npos) {
      break;
    }
    text.remove_prefix(nl + 1);
  }
  return out;
}

}  // namespace

bool SitesTextIsValid(std::string_view text) {
  std::size_t count = 0;
  for (std::string_view line : Lines(text)) {
    if (Trim(line).empty()) {
      continue;
    }
    if (HostName(line).empty() || ++count > kMaxSites) {
      return false;
    }
  }
  return true;
}

std::vector<std::string> ParseSites(std::string_view text) {
  std::vector<std::string> out;
  for (std::string_view line : Lines(text)) {
    std::string host = HostName(line);
    if (!host.empty() && std::find(out.begin(), out.end(), host) == out.end() &&
        out.size() < kMaxSites) {
      out.push_back(std::move(host));
    }
  }
  return out;
}

std::string SitesText(const std::vector<std::string>& sites) {
  std::string out;
  for (const std::string& s : sites) {
    out += s;
    out += '\n';
  }
  return out;
}

std::string_view CoveringSite(const std::vector<std::string>& sites, std::string_view raw_host) {
  const std::string host = HostName(raw_host);
  if (host.empty()) {
    return {};
  }
  for (const std::string& s : sites) {
    if (SiteCovers(s, host)) {
      return s;
    }
  }
  return {};
}

void SiteList::Set(std::vector<std::string> sites) {
  std::lock_guard<std::mutex> hold(lock_);
  sites_ = std::move(sites);
}

std::vector<std::string> SiteList::Get() const {
  std::lock_guard<std::mutex> hold(lock_);
  return sites_;
}

bool SiteList::Covers(std::string_view host) const {
  std::lock_guard<std::mutex> hold(lock_);
  return !CoveringSite(sites_, host).empty();
}

bool SiteList::Add(std::string_view raw_host) {
  const std::string host = HostName(raw_host);
  std::lock_guard<std::mutex> hold(lock_);
  if (host.empty() || !CoveringSite(sites_, host).empty() || sites_.size() >= kMaxSites) {
    return false;
  }
  sites_.push_back(host);
  return true;
}

bool SiteList::Remove(std::string_view raw_host) {
  const std::string host = HostName(raw_host);
  std::lock_guard<std::mutex> hold(lock_);
  const auto before = sites_.size();
  if (!host.empty()) {
    sites_.erase(std::remove_if(sites_.begin(), sites_.end(),
                                [&](const std::string& s) { return SiteCovers(s, host); }),
                 sites_.end());
  }
  return sites_.size() != before;
}

}  // namespace torfox::adblock
