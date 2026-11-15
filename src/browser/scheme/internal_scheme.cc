// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "src/browser/scheme/internal_scheme.h"

#include <string>

#include "include/cef_stream.h"
#include "include/wrapper/cef_stream_resource_handler.h"
#include "src/browser/app/browser_registry.h"
#include "src/browser/app/test_log.h"
#include "src/browser/scheme/resources.h"
#include "src/core/url/guards.h"

namespace torfox {

namespace {

// Serves bundled bytes; text resources are always UTF-8.
class ResourceHandler : public CefStreamResourceHandler {
 public:
  ResourceHandler(int status,
                  const std::string& status_text,
                  const std::string& mime,
                  const CefResponse::HeaderMap& headers,
                  CefRefPtr<CefStreamReader> stream)
      : CefStreamResourceHandler(status, status_text, mime, headers, stream),
        text_(mime.starts_with("text/") || mime == "image/svg+xml") {}

  void GetResponseHeaders(CefRefPtr<CefResponse> response,
                          int64_t& response_length,
                          CefString& redirect_url) override {
    CefStreamResourceHandler::GetResponseHeaders(response, response_length, redirect_url);
    if (text_) {
      response->SetCharset("utf-8");
    }
  }

 private:
  const bool text_;
};

CefResponse::HeaderMap SecurityHeaders() {
  CefResponse::HeaderMap h;
  h.insert({"Content-Security-Policy", std::string(kInternalCsp)});
  h.insert({"X-Frame-Options", "DENY"});
  h.insert({"X-Content-Type-Options", "nosniff"});
  h.insert({"Referrer-Policy", "no-referrer"});
  h.insert({"Cross-Origin-Opener-Policy", "same-origin"});
  h.insert({"Cross-Origin-Embedder-Policy", "require-corp"});
  h.insert({"Cross-Origin-Resource-Policy", "same-origin"});
  h.insert({"Cache-Control", "no-store"});
  h.insert({"Permissions-Policy",
            "camera=(), microphone=(), geolocation=(), display-capture=(), usb=(), serial=(), "
            "hid=(), bluetooth=(), payment=(), clipboard-read=(), clipboard-write=()"});
  return h;
}

CefRefPtr<CefResourceHandler> Error(int status, const char* text) {
  static constexpr char kBody[] = "";
  CefRefPtr<CefStreamReader> stream =
      CefStreamReader::CreateForData(const_cast<char*>(kBody), 0);
  return new ResourceHandler(status, text, "text/plain", SecurityHeaders(), stream);
}

class InternalSchemeFactory : public CefSchemeHandlerFactory {
 public:
  CefRefPtr<CefResourceHandler> Create(CefRefPtr<CefBrowser> browser,
                                       CefRefPtr<CefFrame> frame,
                                       const CefString& scheme_name,
                                       CefRefPtr<CefRequest> request) override {
    // Runs on the IO thread.
    const std::string url = request->GetURL().ToString();
    const int browser_id = browser ? browser->GetIdentifier() : 0;
    auto entry = browser ? Registry().Find(browser_id) : std::nullopt;

    auto log = [&](int status, const char* why) {
      test_log::Event("scheme_request",
                      {{"url", url.substr(0, 300)},
                       {"browser", browser_id},
                       {"role", entry ? std::string(RoleName(entry->role)) : std::string("none")},
                       {"status", status},
                       {"why", why}});
    };

    // Requests without a browser (workers, service workers) or from any
    // browser that is not internal get nothing; see guards::DecideInternalRequest.
    const guards::InternalRequest decision = guards::DecideInternalRequest(
        entry.has_value(), entry ? GuardRole(entry->role) : guards::Role::kWebTab,
        request->GetMethod().ToString(), url);
    if (decision.status != 200) {
      log(decision.status, decision.why.c_str());
      return Error(decision.status, decision.status == 405   ? "Method Not Allowed"
                                    : decision.status == 404 ? "Not Found"
                                                             : "Forbidden");
    }
    const resources::Resource* res = resources::Find(decision.resource_path);
    if (!res) {
      log(404, "no such resource");
      return Error(404, "Not Found");
    }
    log(200, "ok");
    CefRefPtr<CefStreamReader> stream = CefStreamReader::CreateForData(
        const_cast<unsigned char*>(res->data), res->size);
    return new ResourceHandler(200, "OK", std::string(res->mime), SecurityHeaders(),
                                        stream);
  }

 private:
  IMPLEMENT_REFCOUNTING(InternalSchemeFactory);
};

}  // namespace

CefRefPtr<CefSchemeHandlerFactory> CreateInternalSchemeFactory() {
  return new InternalSchemeFactory();
}

}  // namespace torfox
