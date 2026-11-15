// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "src/browser/app/content_filter.h"

#include <utility>

#include "src/browser/app/privacy_service.h"
#include "src/browser/app/services.h"
#include "src/browser/app/test_log.h"
#include "src/core/privacy/trackers.h"
#include "src/core/url/guards.h"

namespace torfox::content_filter {

namespace {

bool IsWebUrl(const std::string& url) {
  const std::string scheme = guards::SchemeOf(url);
  return scheme == "http" || scheme == "https";
}

}  // namespace

adblock::Type AdTypeOf(cef_resource_type_t type, const std::string& url) {
  const std::string scheme = guards::SchemeOf(url);
  if (scheme == "ws" || scheme == "wss") {
    return adblock::kWebSocket;
  }
  switch (type) {
    case RT_SUB_FRAME:
    case RT_NAVIGATION_PRELOAD_SUB_FRAME:
      return adblock::kSubdocument;
    case RT_STYLESHEET:
      return adblock::kStylesheet;
    case RT_SCRIPT:
    case RT_WORKER:
    case RT_SHARED_WORKER:
    case RT_SERVICE_WORKER:
      return adblock::kScript;
    case RT_IMAGE:
    case RT_FAVICON:
      return adblock::kImage;
    case RT_FONT_RESOURCE:
      return adblock::kFont;
    case RT_OBJECT:
    case RT_PLUGIN_RESOURCE:
      return adblock::kObject;
    case RT_MEDIA:
      return adblock::kMedia;
    case RT_XHR:
      return adblock::kXmlHttpRequest;
    case RT_PING:
      return adblock::kPing;
    default:
      return adblock::kOther;
  }
}

std::string TopUrl(const std::string& candidate, const std::string& first_party) {
  if (!IsWebUrl(first_party)) {
    return IsWebUrl(candidate) ? candidate : std::string();
  }
  if (IsWebUrl(candidate) && trackers::SiteOf(guards::HostOf(candidate)) ==
                                 trackers::SiteOf(guards::HostOf(first_party))) {
    return candidate;
  }
  return first_party;
}

bool BlocksAdsOn(const std::string& top_url, const adblock::SiteList* ads_allowed) {
  PrivacyService* privacy = GetServices().privacy;
  if (!privacy || !privacy->block_ads()) {
    return false;
  }
  return !(ads_allowed && ads_allowed->Covers(guards::HostOf(top_url)));
}

Decision Check(const Request& request, const adblock::SiteList* ads_allowed) {
  PrivacyService* privacy = GetServices().privacy;
  // Tracker blocking: requests a page makes to a listed domain of another
  // site. Pages of the listed site itself still work.
  if (privacy && privacy->block_trackers()) {
    const trackers::Verdict v =
        trackers::Check(guards::HostOf(request.url), guards::HostOf(request.first_party));
    if (v.block) {
      return {true, false, std::string(v.listed)};
    }
  }
  // Ad blocking: what EasyList's rules say about this request and the
  // documents it is made for.
  const std::string scheme = guards::SchemeOf(request.url);
  if (scheme != "http" && scheme != "https" && scheme != "ws" && scheme != "wss") {
    return {};
  }
  if (!BlocksAdsOn(request.top, ads_allowed)) {
    return {};
  }
  adblock::Request ad;
  ad.url = request.url;
  ad.frame_url = request.document;
  ad.top_url = request.top;
  ad.type = AdTypeOf(request.type, request.url);
  const adblock::Verdict v = adblock::BuiltIn().Check(ad);
  if (v.block) {
    return {true, true, std::string(v.rule)};
  }
  return {};
}

ContextRequestHandler::ContextRequestHandler(std::string initiator,
                                             CefRefPtr<CefResourceRequestHandler> gate,
                                             std::shared_ptr<const adblock::SiteList> ads_allowed)
    : initiator_(std::move(initiator)), gate_(std::move(gate)), ads_allowed_(std::move(ads_allowed)) {}

CefResourceRequestHandler::ReturnValue ContextRequestHandler::OnBeforeResourceLoad(
    CefRefPtr<CefBrowser> browser,
    CefRefPtr<CefFrame> frame,
    CefRefPtr<CefRequest> request,
    CefRefPtr<CefCallback> callback) {
  if (gate_ && gate_->OnBeforeResourceLoad(browser, frame, request, callback) == RV_CANCEL) {
    return RV_CANCEL;
  }
  // A worker's requests: its own origin is the document, and the page too
  // unless the request names another site as its first party.
  Request r;
  r.url = request->GetURL().ToString();
  r.type = request->GetResourceType();
  r.first_party = request->GetFirstPartyForCookies().ToString();
  r.document = IsWebUrl(initiator_) ? initiator_ : r.first_party;
  r.top = TopUrl(r.document, r.first_party);
  if (r.first_party.empty()) {
    r.first_party = r.document;
  }
  const Decision d = Check(r, ads_allowed_.get());
  if (d.block) {
    test_log::Event(d.ad ? "ad_blocked" : "tracker_blocked",
                    {{"role", "context"},
                     {"url", r.url.substr(0, 300)},
                     {"why", d.why},
                     {"initiator", initiator_.substr(0, 300)},
                     {"firstParty", request->GetFirstPartyForCookies().ToString().substr(0, 300)}});
    return RV_CANCEL;
  }
  return RV_CONTINUE;
}

}  // namespace torfox::content_filter
