// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#ifndef TORFOX_BROWSER_APP_CONTENT_FILTER_H_
#define TORFOX_BROWSER_APP_CONTENT_FILTER_H_

#include <memory>
#include <string>

#include "include/cef_request.h"
#include "include/cef_resource_request_handler.h"
#include "src/core/adblock/filters.h"
#include "src/core/adblock/sites.h"

namespace torfox::content_filter {

// Settings > Privacy & Security > Block trackers and Block ads, for one
// request a page or one of its workers makes (never the page itself). Used
// by the tabs (TabClient) and, for requests no tab makes, by the web
// contexts (ContextRequestHandler).
struct Request {
  std::string url;
  cef_resource_type_t type = RT_SUB_RESOURCE;
  // The request's site for cookies (Chromium's first party); may be empty.
  std::string first_party;
  // The document that made the request, and the page's top-level document
  // (as adblock::Request). Empty when not known.
  std::string document;
  std::string top;
};

struct Decision {
  bool block = false;
  bool ad = false;  // EasyList decided; else the tracker list.
  std::string why;  // The listed tracker domain, or the ad rule.
};

// |ads_allowed|: the sites the user allowed ads on, or null for none.
// Any thread.
Decision Check(const Request& request, const adblock::SiteList* ads_allowed);

// Whether ads are blocked on a page whose top-level document is |top_url|:
// Block ads is on, and the site is not in |ads_allowed| (may be null).
bool BlocksAdsOn(const std::string& top_url, const adblock::SiteList* ads_allowed);

// The top-level document's address for a request: |candidate| (a full
// address, such as the main frame's or a worker's origin) if it is on the
// site the request names as its first party (which Chromium gives as a site
// only, such as "https://example.com/"), else that site. Empty if neither is
// a web address.
std::string TopUrl(const std::string& candidate, const std::string& first_party);

// What an EasyList rule calls a request of |type| to |url|.
adblock::Type AdTypeOf(cef_resource_type_t type, const std::string& url);

// For a web context: checks the requests that no tab makes (service and
// shared workers), which CEF passes to the context instead of a tab. One
// per request, to keep its initiator. |gate| (Tor contexts) runs first;
// |ads_allowed| as for Check().
class ContextRequestHandler : public CefResourceRequestHandler {
 public:
  ContextRequestHandler(std::string initiator,
                        CefRefPtr<CefResourceRequestHandler> gate,
                        std::shared_ptr<const adblock::SiteList> ads_allowed);

  ReturnValue OnBeforeResourceLoad(CefRefPtr<CefBrowser> browser,
                                   CefRefPtr<CefFrame> frame,
                                   CefRefPtr<CefRequest> request,
                                   CefRefPtr<CefCallback> callback) override;

 private:
  const std::string initiator_;
  const CefRefPtr<CefResourceRequestHandler> gate_;
  const std::shared_ptr<const adblock::SiteList> ads_allowed_;
  IMPLEMENT_REFCOUNTING(ContextRequestHandler);
};

}  // namespace torfox::content_filter

#endif  // TORFOX_BROWSER_APP_CONTENT_FILTER_H_
