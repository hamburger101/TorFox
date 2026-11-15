# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.

"""Mutation check for TorFox's security tests.

Each mutation deliberately removes one protection from the source, rebuilds
the test build, and runs the checks that are supposed to catch it: unit tests
("unit:<gtest filter>"), end-to-end tests ("e2e:<unittest name>"), release-
build platform tests ("platform:<unittest name>") and installer tests
("installer:<test name>", which first build the per-user test setups of the
mutated sources with scripts/package_windows.py). A
mutation is only "killed" if EVERY listed check fails on its own; if any of
them still passes, that check does not really verify the protection, and the
mutation "survives" (this script then fails). Sources are always restored,
and the build is rebuilt from clean sources at the end.

TorFox's defences are layered (Chromium's own checks first, then TorFox's
navigation guard, resource guard, and a scheme handler that exists only in
the internal request context). Each layer has its own mutation; end-to-end
tests reach the inner layers with browser-initiated navigations and the
test-build-only --torfox-test-skip-guards switch.

Usage:  python tests/e2e/run_mutations.py [name ...]
Needs:  a configured out/test tree (TORFOX_TEST_HOOKS=ON); TORFOX_CMAKE may
        point at the CMake executable to use.
"""

from __future__ import annotations

import os
import pathlib
import re
import subprocess
import sys

REPO = pathlib.Path(__file__).resolve().parents[2]
CMAKE = os.environ.get("TORFOX_CMAKE", "cmake")
UNIT_EXE = REPO / "out" / "test" / "tests" / "unit" / "Release" / "torfox_unit_tests.exe"
PLATFORM_TESTS = REPO / "tests" / "platform"

GUARDS = "src/core/url/guards.cc"
POLICY = "src/core/cmdline/policy.cc"
QUIET = "e2e:test_quiet_startup.QuietStartupTest"
IDLE = QUIET + ".test_idle_browser_makes_no_network_connections"
TAB = "src/browser/shell/tab_client.cc"
APP = "src/browser/app/browser_app.cc"
PRIV = "e2e:test_privacy"
HTTPS = PRIV + ".HttpsFirstTest"
PERM = PRIV + ".PermissionTest"
EXT = PRIV + ".ExternalProtocolTest"
CLEAR = PRIV + ".ClearOnExitTest"
TOR = "e2e:test_tor"
SEARCH = "src/core/url/search.cc"
SESSION = "src/core/browsing/session.cc"
STARTUP = "e2e:test_startup.StartupTest"
CTX = "src/browser/app/context_manager.cc"
MINISIGN = "src/core/update/minisign.cc"
RELEASE = "src/core/update/release.cc"
UPDATER = "src/browser/app/update_service.cc"
UPD = "e2e:test_updates.UpdateTest"
ISS = "packaging/windows/TorFox.iss"
PACKAGER = "scripts/package_windows.py"
INST = "installer:"

# name -> ([(file, old, new), ...], [checks that must each fail])
MUTATIONS: dict[str, tuple[list[tuple[str, str, str]], list[str]]] = {
    # ----- Tor windows (Stage 5) -----
    # Layer 1: a Tor tab's own guard holds requests back until tor is connected.
    "tor-tab-guard-off": ([
        (TAB, "  return !(tor_service_ && tor_service_->connected());\n}", "  return false;\n}"),
    ], [TOR + ".TorWindowTest.test_tor_window_waits_for_tor_then_loads_through_tor_only",
        TOR + ".TorFailsClosedTest.test_when_tor_dies_nothing_loads_until_it_is_back"]),
    # Layer 2: the Tor context's gate, for requests no tab makes.
    "tor-context-gate-off": ([
        (CTX, "    if (why.empty() && !fault_injection::SkipTorGuard() && !(allowed_ && allowed_())) {",
         "    if (false) {"),
    ], [TOR + ".TorContextGateTest"]),
    "tor-context-gate-allows-local-addresses": ([
        (CTX, "    std::string why = guards::ResourceBlockReason(guards::Role::kTorTab, url);",
         "    std::string why;"),
    ], [TOR + ".TorContextGateTest"]),
    # Layer 3: the proxy never falls back to a direct connection.
    "tor-proxy-falls-back-to-direct": ([
        (CTX, '  dict->SetString("server", "socks5://127.0.0.1:" + std::to_string(port));',
         '  dict->SetString("server", "socks5://127.0.0.1:" + std::to_string(port) + ",direct://");'),
    ], [TOR + ".ProxyLayerTest", TOR + ".ProxyLayerAfterDeathTest"]),
    "tor-context-follows-system-proxy": ([
        (CTX, "  if (IsTor(context)) {\n    // Never: the Tor context's proxy", "  if (false) {\n    // Never: the Tor context's proxy"),
    ], [TOR + ".TorContextIsolationTest.test_system_proxy_setting_never_changes_the_tor_context"]),
    "tor-local-addresses-allowed": ([
        (GUARDS, '  if ((scheme == "http" || scheme == "https") && role == Role::kTorTab &&\n      IsLocalNetworkHost(HostOf(url))) {',
         "  if (false) {"),
        (GUARDS, "  if (IsHttpLike(scheme) && role == Role::kTorTab && IsLocalNetworkHost(HostOf(url))) {",
         "  if (false) {"),
    ], ["unit:TorGuards.TorTabsAllowOnionHostsAndRefuseLocalAddresses",
        TOR + ".TorWindowTest.test_tor_windows_refuse_this_computer_and_the_local_network"]),
    "tor-webrtc-not-restricted": ([
        (CTX, "  const char* webrtc = kind == Kind::kInternal || kind == Kind::kTor\n",
         "  const char* webrtc = kind == Kind::kInternal\n"),
    ], [TOR + ".TorContextIsolationTest.test_webrtc_in_tor_windows_reveals_no_address"]),
    "tor-camera-and-clipboard-asked": ([
        (TAB, '  if (tor_) {\n    return deny("tor window");\n  }\n  if (requested_permissions & ~(kCamera | kMicrophone)) {',
         "  if (requested_permissions & ~(kCamera | kMicrophone)) {"),
        (TAB, '  if (tor_) {\n    return deny("tor window");\n  }\n  if (requested_permissions != CEF_PERMISSION_TYPE_CLIPBOARD) {',
         "  if (requested_permissions != CEF_PERMISSION_TYPE_CLIPBOARD) {"),
    ], [TOR + ".TorSecureContextTest.test_permissions_and_other_apps_are_refused"]),
    "tor-other-apps-allowed": ([
        (TAB, "    if (tor_) {\n      // Another app would open the link without Tor.", "    if (false) {\n      // Another app would open the link without Tor."),
    ], [TOR + ".TorSecureContextTest.test_permissions_and_other_apps_are_refused"]),
    "tor-links-open-in-private-windows": ([
        (TAB, "    if (!tor_) {\n      model->AddItem(kOpenLinkInPrivateWindow", "    if (true) {\n      model->AddItem(kOpenLinkInPrivateWindow"),
    ], [TOR + ".TorWindowTest.test_link_menu_never_leaves_tor"]),
    "tor-searches-use-clearnet-duckduckgo": ([
        (SEARCH, "  if (tor) {\n    host = tor::kDuckDuckGoOnion;\n  }\n", ""),
    ], ["unit:Search.TorWindowsUseDuckDuckGosOnionService",
        TOR + ".TorWindowTest.test_searches_in_tor_windows_use_duckduckgos_onion_service"]),
    "onion-checksum-not-checked": ([
        ("src/core/tor/onion.cc", "  if (digest[0] != bytes[32] || digest[1] != bytes[33]) {", "  if (false) {"),
    ], ["unit:TorOnion.ChecksVersion3AddressesIncludingTheirChecksum",
        TOR + ".TorWindowTest.test_mistyped_onion_addresses_are_refused_and_never_searched"]),
    "tor-new-identity-keeps-the-context": ([
        (APP, "  std::string error;\n  if (!contexts_->DropTor(&error)) {\n"
              "    LOG(ERROR) << \"New identity: the old Tor context's proxy was not changed: \" << error;\n"
              "  }\n  tor_->NewIdentity(", "  tor_->NewIdentity("),
    ], [TOR + ".NewIdentityTest"]),
    "tor-not-in-a-kill-on-close-job": ([
        ("src/browser/platform/platform_win.cc", "  if (!::AssignProcessToJobObject(job, pi.hProcess)) {", "  if (false) {"),
    ], [TOR + ".TorProcessTest.test_the_job_alone_ends_tor_if_torfox_is_killed"]),
    "tor-custom-bridges-unchecked": ([
        ("src/core/tor/bridges.cc", "  return ParseBridgeLines(text).errors.empty();", "  return true;"),
    ], ["unit:SettingsSchema.ValidatesTypesAndConstraints", TOR + ".BridgeSettingsTest.test_invalid_custom_bridges_are_refused"]),
    "tor-per-destination-circuits-off": ([
        ("src/core/tor/torrc.cc", '  t += "SocksPort 127.0.0.1:auto ExtendedErrors IsolateDestAddr\\n";',
         '  t += "SocksPort 127.0.0.1:auto ExtendedErrors\\n";'),
    ], ["unit:Torrc.ListensOnLoopbackOnlyWithCookieAuthAndExitsWithTorFox",
        TOR + ".TorWindowTest.test_torrc_and_control_session"]),
    # Layer: TorFox's navigation guard (OnBeforeBrowse) for web tabs.
    "nav-guard-allows-privileged-schemes": ([
        (GUARDS,
         '  if (ReachesPrivilegedScheme(scheme, url)) {\n    return "privileged scheme";\n  }\n  if (scheme == "about") {',
         '  if (false) {\n    return "privileged scheme";\n  }\n  if (scheme == "about") {'),
    ], ["unit:GuardsTest.WebTabsNeverReachPrivilegedSchemes",
        "e2e:test_isolation.BrowserInitiatedNavigationTest.test_web_tab_refuses_privileged_urls"]),
    # Layer: TorFox's resource guard (OnBeforeResourceLoad) for web tabs.
    "resource-guard-allows-privileged-schemes": ([
        (GUARDS,
         '  if (ReachesPrivilegedScheme(scheme, url)) {\n    return "privileged scheme";\n  }\n  if (IsHttpLike',
         '  if (false) {\n    return "privileged scheme";\n  }\n  if (IsHttpLike'),
    ], ["unit:GuardsTest.WebTabsNeverReachPrivilegedSchemes",
        "e2e:test_isolation.GuardLayersTest.test_resource_guard_alone_refuses_internal_urls"]),
    # Layer: the torfox:// handler is registered in the internal context only.
    "scheme-handler-in-web-context": ([
        ("src/browser/app/context_manager.cc",
         "  if (kind == Kind::kInternal) {\n    if (!context->RegisterSchemeHandlerFactory(",
         "  if (true) {\n    if (!context->RegisterSchemeHandlerFactory("),
    ], ["e2e:test_isolation.GuardLayersTest.test_web_contexts_have_no_internal_scheme_handler"]),
    # Layer: the handler itself serves internal browsers only.
    "scheme-handler-serves-any-browser": ([
        (GUARDS, "  if (!known || !IsInternal(role)) {", "  if (false) {"),
    ], ["unit:GuardsTest.SchemeHandlerServesOnlyInternalBrowsers"]),
    # Internal browsers never leave TorFox pages.
    "internal-browsers-reach-web": ([
        (GUARDS,
         '    if (!parsed || !IsInternalPage(parsed->host)) {\n      return "internal browsers only show TorFox pages";\n    }',
         '    if (!parsed || !IsInternalPage(parsed->host)) {\n      if (SchemeOf(url) == "http") {\n        return std::string();\n      }\n      return "internal browsers only show TorFox pages";\n    }'),
    ], ["unit:GuardsTest.InternalBrowsersShowOnlyTheirOwnPages",
        "e2e:test_isolation.InternalPagesStayInternalTest.test_navigation_to_web_is_blocked",
        "e2e:test_isolation.BrowserInitiatedNavigationTest.test_internal_tab_refuses_web_urls"]),
    "shell-page-role-check": ([
        (GUARDS,
         '    if (!PageAllowedForRole(parsed->host, role)) {\n      return "page not allowed for this browser";',
         '    if (false) {\n      return "page not allowed for this browser";'),
    ], ["unit:GuardsTest.InternalBrowsersShowOnlyTheirOwnPages",
        "e2e:test_isolation.BrowserInitiatedNavigationTest.test_shell_refuses_other_pages"]),
    # Internal pages' network ban. End to end, their CSP (connect-src 'none',
    # img-src 'self' data:) refuses these loads first, so only the unit test
    # sees this layer on its own.
    "internal-pages-reach-network": ([
        (GUARDS, '    return "network access from internal page";', "    return std::string();"),
    ], ["unit:GuardsTest.InternalPagesLoadOnlyBundledResources"]),
    # The bridge exists only in internal pages.
    "bridge-in-web-pages": ([
        ("src/browser/app/renderer_app.cc",
         "if (!router_ || internal_browsers_.count(browser->GetIdentifier()) == 0 || !frame->IsMain()) {\n    return;\n  }\n  auto url = ParseInternalUrl(frame->GetURL().ToString());\n  if (!url || !IsInternalPage(url->host)) {\n    return;\n  }",
         "if (!router_) {\n    return;\n  }"),
    ], ["e2e:test_isolation.SchemeIsolationTest.test_no_bridge_in_web_pages",
        "e2e:test_isolation.SchemeIsolationTest.test_no_bridge_in_web_subframes"]),
    "bridge-page-allowlist": ([
        ("src/browser/bridge/bridge_handler.cc",
         "  if (msg.ok() && !guards::PageAllowedForRole(msg.page, GuardRole(entry->role))) {",
         "  if (false) {"),
        ("src/core/bridge/validator.cc",
         "  for (const CommandSpec& c : page->commands) {",
         "  for (const PageSpec& any_page : pages) for (const CommandSpec& c : any_page.commands) {"),
    ], ["e2e:test_bridge.BridgeValidationTest.test_commands_of_other_pages",
        "e2e:test_bridge.BridgeValidationTest.test_rejections_change_nothing"]),
    # ----- Stage 3: downloads (tests/e2e/test_downloads.py) -----
    "downloads-disable-ignored": ([
        ("src/core/downloads/policy.cc",
         "  if (disable_downloads) {\n    return Decision::kBlock;\n  }\n", ""),
    ], ["unit:DownloadPolicy.DisableOverridesAsk",
        "e2e:test_downloads.DownloadTest.test_disabled_blocks_every_vector"]),
    "downloads-allowed-by-can-download": ([
        ("src/browser/shell/tab_client.cc",
         "  if (CurrentDownloadDecision() == downloads::Decision::kBlock) {\n"
         "    LogBlock(\"download_blocked\", role_, target, \"downloads disabled\");",
         "  if (false) {\n"
         "    LogBlock(\"download_blocked\", role_, target, \"downloads disabled\");"),
    ], ["e2e:test_downloads.DownloadTest.test_disabled_blocks_every_vector"]),
    "downloads-no-recheck-before-download": ([
        ("src/browser/shell/tab_client.cc",
         "  if (role_ != BrowserRole::kWebTab || !window_ || !service ||\n"
         "      decision == downloads::Decision::kBlock) {",
         "  if (role_ != BrowserRole::kWebTab || !window_ || !service) {"),
    ], ["e2e:test_downloads.InnerLayerTest.test_before_download_refuses_every_vector"]),
    "downloads-ask-skipped": ([
        ("src/browser/shell/tab_client.cc",
         "  if (decision == downloads::Decision::kAllow) {\n"
         "    test_log::Event(\"download_started\", {{\"id\", id}, {\"name\", entry.name}, {\"asked\", false}});",
         "  if (true) {\n"
         "    test_log::Event(\"download_started\", {{\"id\", id}, {\"name\", entry.name}, {\"asked\", false}});"),
    ], ["e2e:test_downloads.DownloadTest.test_ask_and_cancel_saves_nothing", "e2e:test_downloads.DownloadTest.test_aa_a_new_profile_asks_first"]),
    "downloads-declined-answer-ignored": ([
        ("src/browser/shell/tab_client.cc",
         "    if (!ok || !e || e->cancel_requested || d->Decide() == downloads::Decision::kBlock) {",
         "    if (!e || e->cancel_requested || d->Decide() == downloads::Decision::kBlock) {"),
    ], ["e2e:test_downloads.DownloadTest.test_ask_and_cancel_saves_nothing"]),
    "downloads-unconfirmed-bytes-in-downloads": ([
        ("src/browser/app/browser_app.cc",
         "  contexts_->SetDownloadDirectory(downloads_->StagingDirectory());",
         "  contexts_->SetDownloadDirectory(downloads_->Directory());"),
    ], ["e2e:test_downloads.DownloadTest.test_closing_the_tab_cancels_an_open_prompt"]),
    "downloads-disabling-leaves-them-running": ([
        ("src/browser/app/download_service.cc",
         "        CancelAll(\"downloads disabled\");", "        Notify();"),
    ], ["e2e:test_downloads.SettingsLayerTest.test_disabling_cancels_running_downloads_and_prompts"]),
    "downloads-partial-file-has-final-name": ([
        ("src/browser/shell/tab_client.cc",
         "  const std::string target = platform::PathToUtf8(entry.partial);",
         "  const std::string target = platform::PathToUtf8(entry.path);"),
    ], ["e2e:test_downloads.DownloadTest.test_unfinished_files_never_have_their_final_name"]),
    "downloads-tab-close-not-recorded": ([
        ("src/browser/shell/browser_window.cc",
         "    cancelled_downloads = d->CancelForTab(id_, tab_id, \"tab closed\");",
         "    cancelled_downloads = 0;"),
    ], ["e2e:test_downloads.DownloadTest.test_unfinished_files_never_have_their_final_name"]),
    "downloads-private-visible-everywhere": ([
        ("src/browser/app/download_service.cc",
         "  if (viewer.private_window) {\n    return e.private_window && e.window_id == viewer.window_id;\n  }\n"
         "  return !e.private_window;",
         "  return true;"),
    ], ["e2e:test_downloads.DownloadTest.test_private_downloads_stay_in_their_window"]),
    "downloads-names-not-sanitized": ([
        ("src/browser/shell/tab_client.cc",
         "  const std::string name = downloads::SafeFileName(suggested_name.ToString());",
         "  const std::string name = suggested_name.ToString();"),
    ], ["e2e:test_downloads.DownloadTest.test_names_are_made_safe_and_unique"]),
    "downloads-names-not-unique": ([
        ("src/core/downloads/policy.cc",
         "    if (!exists(*path)) {", "    if (true) {"),
    ], ["unit:DownloadPath.*", "e2e:test_downloads.DownloadTest.test_names_are_made_safe_and_unique"]),
    "file-system-access-pickers-enabled": ([
        ("src/core/cmdline/policy.cc",
         "      {\"disable-blink-features\", \"FileSystemAccessLocal\"},\n", ""),
    ], ["e2e:test_downloads.DownloadTest.test_file_system_access_pickers_are_absent"]),
    "downloads-same-name-while-waiting": ([
        ("src/browser/shell/tab_client.cc",
         "               std::filesystem::exists(DownloadService::PartialPath(p), e) ||\n"
         "               service->PathInUse(p);",
         "               std::filesystem::exists(DownloadService::PartialPath(p), e);"),
    ], ["e2e:test_downloads.DownloadTest.test_downloads_waiting_at_the_same_time_get_different_names"]),
    "downloads-path-too-long": ([
        ("src/core/downloads/policy.cc",
         "    if (path.native().size() + reserve <= kMaxPathLength) {", "    if (true) {"),
    ], ["unit:DownloadPath.StaysWithinThePathLimit",
        "e2e:test_downloads.DownloadTest.test_names_are_made_safe_and_unique"]),
    "downloads-leftovers-kept": ([
        ("src/browser/app/download_service.cc",
         "    const bool removed = std::filesystem::remove(partial, ec);",
         "    const bool removed = false;"),
    ], ["e2e:test_downloads.DownloadTest.test_leftover_partial_files_are_removed"]),
    "downloads-restart-gets-new-name": ([
        ("src/browser/shell/tab_client.cc",
         "  if (DownloadService::Entry* known = service->FindItem(download_context_, download_item->GetId())) {",
         "  if (DownloadService::Entry* known = nullptr) {"),
    ], ["e2e:test_downloads.DownloadTest.test_restarted_downloads_keep_their_name"]),
    "chromium-download-records-kept": ([
        ("src/core/browsing/chromium_profile.cc",
         "      if (!ok || !Exec(db, (\"DELETE FROM \" + Quoted(table)).c_str())) {",
         "      if (!ok || table.empty()) {"),
    ], ["unit:ChromiumProfileTest.*", "e2e:test_downloads.DownloadRecordsTest.test_download_records_are_erased"]),
    "downloads-program-warning-missing": ([
        ("src/browser/shell/tab_client.cc",
         "  if (downloads::IsDangerousType(entry.name)) {", "  if (false) {"),
    ], ["e2e:test_downloads.DownloadTest.test_programs_are_flagged_in_the_prompt"]),
    "onion-allowed-in-web-tabs": ([
        (GUARDS, '  if ((scheme == "http" || scheme == "https") && role != Role::kTorTab &&\n      IsOnionHost(HostOf(url))) {',
         "  if (false) {"),
        (GUARDS, "  if (IsHttpLike(scheme) && role != Role::kTorTab && IsOnionHost(HostOf(url))) {", "  if (false) {"),
    ], ["unit:GuardsTest.WebTabsNeverReachOnionHosts",
        "e2e:test_stage1_defaults.Stage1DefaultsTest.test_onion_subresources_and_navigation_are_blocked"]),
    # The forced host-resolver rule: the only layer that stops onion
    # WebSockets (CEF does not show them to TorFox's request guards).
    "onion-resolver-rule-removed": ([
        (POLICY, '  std::string rules = kOnionRule;\n  if (!test_host_rules.empty()) {\n    rules += "," + test_host_rules;\n  }',
         '  std::string rules;\n  if (!test_host_rules.empty()) {\n    rules = test_host_rules;\n  }'),
    ], ["unit:CmdlinePolicy.AddsForcedSwitches",
        "e2e:test_quiet_startup.OnionResolverTest.test_onion_names_never_reach_the_resolver",
        "e2e:test_quiet_startup.OnionResolverTest.test_resolver_rule_alone_keeps_onion_names_local"]),
    # New identity: the old Tor context must send nothing more.
    "tor-retired-context-not-refused": ([
        (CTX, "  tor_retired_->store(true);\n", ""),
    ], [TOR + ".TorContextGateTest.test_after_new_identity_the_old_context_sends_nothing"]),
    "tor-retired-context-keeps-proxy": ([
        (CTX, '  const bool ok = SetPrefVerified(tor_, "tor (retired)", "proxy",\n'
              '                                  TorProxyValue(kTorPlaceholderPort), error);',
         "  const bool ok = true;"),
    ], [TOR + ".TorContextGateTest.test_after_new_identity_the_old_context_sends_nothing"]),
    # Site isolation turned off: the test must see frames of other sites
    # move into their parent's process.
    "site-isolation-off": ([
        (POLICY, '      {"site-per-process", ""},\n', '      {"disable-site-isolation-trials", ""},\n'),
    ], ["e2e:test_site_isolation.SiteIsolationTest.test_frames_of_other_sites_run_in_other_processes",
        "e2e:test_site_isolation.SiteIsolationTest.test_private_windows_isolate_sites_too"]),
    # Chromium's DNS probe (google.com) after a failed lookup.
    "dns-probe-left-on": ([
        (CTX, '  if (!SetPrefVerified(context, name, "alternate_error_pages.enabled", BoolValue(false), error)) {\n'
              '    return false;\n  }\n', ""),
    ], [QUIET[:-len(".QuietStartupTest")] + ".FailedLookupTest.test_a_failed_lookup_makes_no_other_lookup"]),
    # WebRTC looks names up outside the proxy: Tor windows have none.
    "tor-webrtc-kept": ([
        ("src/browser/app/renderer_app.cc", "    RemoveWebRtc(context);\n", ""),
    ], [TOR + ".TorContextIsolationTest.test_tor_windows_have_no_webrtc",
        TOR + ".TorDnsTest.test_webrtc_looks_up_no_name_for_tor_windows"]),
    # After tor stops, its old SOCKS port is not TorFox's any more.
    "tor-dead-port-kept": ([
        ("src/browser/app/tor_service.cc", "  if (status.state == State::kError) {\n    // This tor has stopped",
         "  if (false) {\n    // This tor has stopped"),
    ], [TOR + ".ProxyLayerAfterDeathTest.test_a_dead_tors_port_is_never_used_again"]),
    # A user switch must not remove CEF's own switch of the same name.
    "cmdline-erases-cef-switches": ([
        (POLICY, "    if (it != plan.switches.end() && it->second == value) {\n      plan.switches.erase(it);",
         "    if (it != plan.switches.end()) {\n      plan.switches.erase(it);"),
    ], ["unit:CmdlinePolicy.TheUsersSwitchNeverRemovesCefsOwn"]),
    "cmdline-release-logging-not-forced": ([
        (POLICY, '    plan.switches["log-severity"] = "disable";\n', ""),
    ], ["unit:CmdlinePolicy.TheUsersSwitchNeverRemovesCefsOwn"]),
    # "x.onion." does not match "*.onion" (docs/STAGE9_FINDINGS.md).
    "onion-trailing-dot-resolvable": ([
        (POLICY, 'constexpr char kOnionRule[] = "MAP *.onion ~NOTFOUND,MAP *.onion. ~NOTFOUND";',
         'constexpr char kOnionRule[] = "MAP *.onion ~NOTFOUND";'),
    ], ["e2e:test_quiet_startup.OnionResolverTest.test_onion_names_never_reach_the_resolver",
        "e2e:test_quiet_startup.OnionResolverTest.test_resolver_rule_alone_keeps_onion_names_local"]),
    # Service contexts (internal, global, system) never look for a proxy.
    "service-contexts-follow-system-proxy": ([
        ("src/browser/app/browser_app.cc",
         ' &&\n         SetPrefVerified(global, "global", "proxy", DirectProxy(), error) &&\n         SetPrefVerified(local_state, "local_state", "proxy", DirectProxy(), error);',
         ";"),
        ("src/browser/app/context_manager.cc", "  if (kind == Kind::kInternal &&\n", "  if (false &&\n"),
    ], [QUIET + ".test_service_contexts_never_autodiscover_proxies"]),
    # Each background-service switch, removed on its own: the idle browser
    # must then contact someone, or the switch is not needed.
    # Command-line hardening has two layers in release builds: CEF ignores
    # the command line (command_line_args_disabled) and TorFox's policy drops
    # every switch that is not allowlisted. Each holds alone, so the release
    # test can only fail when both are gone; the policy alone is unit tested.
    "cmdline-policy-keeps-user-switches": ([
        (POLICY, "    if (input.test_hooks && Contains(kTestHookSwitches, name)) {", "    if (true) {"),
    ], ["unit:CmdlinePolicy.ReleaseDropsDangerousUserSwitches"]),
    "cmdline-fully-open-in-release": ([
        (POLICY, "    if (input.test_hooks && Contains(kTestHookSwitches, name)) {", "    if (true) {"),
        ("src/browser/app/browser_app.cc", "  settings.command_line_args_disabled = !kTestHooks;",
         "  settings.command_line_args_disabled = false;"),
    ], ["platform:test_release_win.ReleaseBuildTest.test_command_line_cannot_weaken_torfox"]),
    "component-updater-not-redirected": ([
        (POLICY, '      {"component-updater", "url-source=http://127.0.0.1:9/,disable-pings"},\n', ""),
    ], [IDLE]),
    **{f"feature-{name}-enabled": ([(POLICY, f'    "{name}",', "    // removed")], [IDLE])
       for name in ("NetworkTimeServiceQuerying", "TranslateRankerQuery", "TranslateRankerEnforcement",
                    "AimServerRequestOnStartupEnabled")},
    # Stage 2. Chromium's own history service, switched back on everywhere.
    "chromium-history-enabled": ([
        ("src/browser/app/browser_app.cc",
         '         SetPrefVerified(global, "global", "history.saving_disabled", BoolValue(true), error) &&\n',
         ""),
        ("src/browser/app/context_manager.cc",
         '  if (!SetPrefVerified(context, name, "history.saving_disabled", BoolValue(true), error)) {',
         "  if (false) {"),
    ], ["e2e:test_places.PlacesTest.test_visits_are_recorded_by_torfox_not_chromium"]),
    "private-windows-record-history": ([
        ("src/browser/shell/browser_window.cc",
         "  return !private_ && tab.client && tab.client->role() == BrowserRole::kWebTab;",
         "  return tab.client && tab.client->role() == BrowserRole::kWebTab;"),
    ], ["e2e:test_private.PrivateWindowTest.test_private_window_is_marked_and_records_nothing",
        "e2e:test_private.PrivateWindowTest.test_nothing_from_a_private_window_reaches_the_disk"]),
    "private-windows-share-normal-context": ([
        ("src/browser/shell/browser_window.cc",
         "        CefWindow::CreateTopLevelWindow(new BrowserWindow(first_url, true, false, context));",
         "        CefWindow::CreateTopLevelWindow(new BrowserWindow(\n"
         "            first_url, true, false, GetServices().contexts->Get(ContextManager::Kind::kNormal)));"),
    ], ["e2e:test_private.PrivateWindowTest.test_storage_is_isolated_between_windows",
        "e2e:test_private.PrivateWindowTest.test_nothing_from_a_private_window_reaches_the_disk"]),
    "onion-selections-searchable": ([
        ("src/core/browsing/context_menu.cc",
         "  if (ContainsOnionText(query) || MentionsOnionHost(query)) {", "  if (false) {"),
    ], ["unit:ContextMenu.NeverSearchesOnionAddresses",
        "e2e:test_browsing.BrowsingTest.test_onion_selections_are_never_searched"]),
    # Stage 4: privacy & security.
    "https-first-skipped": ([
        (TAB, "  if (role_ == BrowserRole::kWebTab && frame->IsMain() &&\n      HttpsFirst(",
         "  if (false &&\n      HttpsFirst("),
    ], [HTTPS + ".test_http_is_tried_over_https_first",
        HTTPS + ".test_a_site_without_https_asks_before_using_http"]),
    "https-fallback-without-asking": ([
        (TAB, '  Ask("https", prompt, [self, host, http_url](bool ok) {',
         "  ([self, host, http_url](bool ok) {"),
        (TAB, "    self->browser_->GetMainFrame()->LoadURL(http_url);\n  });\n}",
         "    self->browser_->GetMainFrame()->LoadURL(http_url);\n  })(true);\n}"),
    ], [HTTPS + ".test_a_site_without_https_asks_before_using_http",
        HTTPS + ".test_declining_never_loads_http"]),
    "https-redirect-loop-not-detected": ([
        (TAB, "  if (!https_host_.empty() && host == https_host_) {\n    // The https site sent",
         "  if (false) {\n    // The https site sent"),
    ], [HTTPS + ".test_https_redirecting_back_to_http_asks"]),
    "https-exceptions-forgotten": ([
        (TAB, "  if (exceptions->Contains(host)) {", "  if (false) {"),
    ], [HTTPS + ".test_a_site_without_https_asks_before_using_http"]),
    "https-local-names-upgraded": ([
        ("src/core/privacy/https_first.cc", "  if (!IsUpgradableHost(host)) {", "  if (host.empty()) {"),
    ], ["unit:HttpsFirst.LeavesOtherUrlsAlone", HTTPS + ".test_local_names_are_not_upgraded"]),
    "trackers-not-blocked": ([
        (TAB, '      if (v.block) {\n        LogBlock("tracker_blocked"', '      if (false) {\n        LogBlock("tracker_blocked"'),
    ], [PRIV + ".TrackerTest.test_third_party_trackers_are_blocked"]),
    "trackers-first-party-blocked": ([
        ("src/core/privacy/trackers.cc",
         "  v.block = top_host.empty() || SiteOf(request_host) != SiteOf(top_host);", "  v.block = true;"),
    ], ["unit:Trackers.OnlyThirdPartyRequestsAreBlocked",
        PRIV + ".TrackerTest.test_tracker_sites_work_when_visited_directly"]),
    "third-party-cookies-allowed": ([
        ("src/browser/app/context_manager.cc",
         "      kind == Kind::kInternal || kind == Kind::kTor || block_third_party_cookies_ ? 1 : 0;",
         "      kind == Kind::kInternal || kind == Kind::kTor ? 1 : 0;"),
    ], [PRIV + ".CookieTest.test_third_party_cookies_are_blocked"]),
    "tor-third-party-cookies-follow-the-setting": ([
        ("src/browser/app/context_manager.cc",
         "      kind == Kind::kInternal || kind == Kind::kTor || block_third_party_cookies_ ? 1 : 0;",
         "      kind == Kind::kInternal || block_third_party_cookies_ ? 1 : 0;"),
    ], [TOR + ".TorSecureContextTest.test_third_party_cookies_are_always_blocked"]),
    "popups-without-click-allowed": ([
        (TAB, "  if (!user_gesture && (!privacy || privacy->block_popups())) {",
         "  if (!user_gesture && privacy == nullptr) {"),
    ], [PRIV + ".PopupTest.test_popups_without_a_click_are_blocked"]),
    "camera-asked-by-default": ([
        (TAB, '      (camera && privacy->PermissionFor("permissions.camera") != PrivacyService::Permission::kAsk) ||',
         "      false ||"),
    ], [PERM + ".test_camera_and_microphone_are_denied_by_default"]),
    "camera-granted-without-asking": ([
        (TAB, '  Ask("permission", prompt, [callback, requested_permissions, origin](bool ok) {',
         "  ([callback, requested_permissions, origin](bool ok) {"),
        (TAB, "      callback->Cancel();\n    }\n  });\n  return true;\n}",
         "      callback->Cancel();\n    }\n  })(true);\n  return true;\n}"),
    ], [PERM + ".test_camera_can_be_asked_for"]),
    "clipboard-asked-by-default": ([
        (TAB, '  if (privacy->PermissionFor("permissions.clipboard") != PrivacyService::Permission::kAsk) {',
         "  if (false) {"),
    ], [PERM + ".test_clipboard_reading_is_denied_or_asked"]),
    "permission-answers-remembered": ([
        (TAB, "  if (!browser || origin.empty()) {\n    return;\n  }\n  CefRefPtr<CefRequestContext> context",
         "  if (!browser || !origin.empty()) {\n    return;\n  }\n  CefRefPtr<CefRequestContext> context"),
    ], [PERM + ".test_clipboard_reading_is_denied_or_asked"]),
    "mail-links-open-without-asking": ([
        (TAB, '  const bool mail = scheme == "mailto";\n',
         '  if (!url.empty()) {\n    test_log::Event("external_opened", {{"scheme", scheme}});\n    return;\n  }\n'
         '  const bool mail = scheme == "mailto";\n'),
    ], [EXT + ".test_mail_links_ask_before_opening_another_app",
        EXT + ".test_declined_mail_links_open_nothing"]),
    "mail-links-ask-without-a-click": ([
        (TAB, '  if (!user_gesture) {\n    LogBlock("protocol_blocked"', '  if (!user_gesture && false) {\n    LogBlock("protocol_blocked"'),
    ], [EXT + ".test_mail_links_without_a_click_never_ask"]),
    "launchable-scheme-allows-quotes": ([
        ("src/core/privacy/external_protocols.cc", "    case '\"':\n", ""),
    ], ["unit:ExternalProtocols.RefusesUrlsACommandLineCouldSplit"]),
    "other-schemes-launchable": ([
        ("src/core/privacy/external_protocols.cc", '{"mailto", "tel"};', '{"mailto", "tel", "ms-settings"};'),
    ], ["unit:ExternalProtocols.OnlyMailAndPhoneLinksMayOpenAnApp",
        EXT + ".test_other_schemes_never_launch"]),
    "clear-on-exit-skipped": ([
        (APP, "  clear_data::Clear(normal_profile, clear_at_exit);\n  // Settings > Updates",
         "  // Settings > Updates"),
    ], [CLEAR + ".test_chosen_data_is_cleared_when_torfox_closes"]),
    "history-kept-at-exit": ([
        (APP, "  if (exit_clear_.history && places_ && places_->places()) {", "  if (false) {"),
    ], [CLEAR + ".test_chosen_data_is_cleared_when_torfox_closes"]),
    "clear-at-start-skipped": ([
        (APP, "  if (clear_at_start.any()) {", "  if (false) {"),
    ], [CLEAR + ".test_data_left_by_a_crash_is_cleared_at_the_next_start"]),
    "history-kept-at-start": ([
        (APP, '  if (settings_->store().GetBool("privacy.clearOnExit.history") && places_->places()) {',
         "  if (false) {"),
    ], [CLEAR + ".test_data_left_by_a_crash_is_cleared_at_the_next_start"]),
    "clear-data-follows-links": ([
        ("src/core/privacy/clear_data.cc",
         "    // remove_all removes a link itself and never follows it.\n    std::filesystem::remove_all(p, ec);",
         "    if (std::filesystem::is_directory(p, ec)) {\n"
         "      for (const auto& e : std::filesystem::directory_iterator(p, ec)) {\n"
         "        std::filesystem::remove_all(e.path(), ec);\n      }\n    }\n"
         "    std::filesystem::remove_all(p, ec);"),
    ], ["unit:ClearData.NeverFollowsJunctions"]),
    "site-records-kept": ([
        (APP, "  chromium_profile::ScrubSiteRecords(normal_profile, clear_at_exit.history);\n", ""),
    ], [CLEAR + ".test_control_without_clearing_everything_is_kept"]),
    "history-clear-keeps-zoom": ([
        ("src/browser/bridge/bridge_handler.cc", "    if (contexts && !contexts->ClearSiteRecords(&error)) {",
         "    if (contexts == nullptr && !error.empty()) {"),
    ], ["e2e:test_places.PlacesTest.test_clearing_history_forgets_zoomed_sites"]),
    "proxy-mode-not-written": ([
        (APP, "    const bool ok = chromium_profile::WriteProxyMode(normal_profile, mode);",
         "    const bool ok = mode != nullptr;"),
    ], [QUIET + ".test_idle_browser_does_not_autodiscover_proxies"]),
    # ----- Stage 6: settings -----
    "search-engine-ignored": ([
        (SEARCH, '  if (engine == "duckduckgo-html") {', "  if (false) {"),
    ], ["unit:Search.EachEngineHasItsPage",
        "e2e:test_browsing.SearchEngineTest.test_the_chosen_engine_gets_the_search"]),
    "session-keeps-private-windows": ([
        (APP, "          if (!w->is_private() && !w->closing()) {", "          if (!w->closing()) {"),
    ], [STARTUP + ".test_restore_after_a_crash_reopens_every_normal_window"]),
    "session-kept-while-history-cleared": ([
        ("src/browser/app/session_service.cc",
         ' == "restore" &&\n         !settings_->store().GetBool(kClearHistorySetting);', ' == "restore";'),
    ], [STARTUP + ".test_nothing_is_kept_while_history_is_cleared_on_exit"]),
    "session-keeps-onion-addresses": ([
        (SESSION, "  if (!IsStorableUrl(url) || guards::IsOnionHost(guards::HostOf(url))) {",
         "  if (!IsStorableUrl(url)) {"),
    ], ["unit:Session.KeepsWebPagesAndTorFoxPagesOnly",
        "unit:Session.UnrestorableTabsAndEmptyWindowsAreLeftOut"]),
    "session-file-trusted": ([
        (SESSION, "      if (std::optional<std::string> url = RestorableUrl(t.get_ref<const std::string&>())) {",
         "      if (std::optional<std::string> url = std::optional<std::string>(t.get<std::string>())) {"),
    ], ["unit:Session.DamagedOrForeignFilesRestoreNothingUnsafe",
        STARTUP + ".test_a_damaged_session_file_opens_a_new_tab"]),
    "first-tabs-created-while-the-window-shows": ([
        ("src/browser/shell/browser_window.cc",
         "  CefPostTask(TID_UI, base::BindOnce(&BrowserWindow::OpenFirstTabs, self));",
         "  OpenFirstTabs();"),
    ], [STARTUP + ".test_restore_reopens_the_tabs_after_a_normal_exit"]),

    # ----- Updates (Stage 7) -----
    # The signature over the manifest itself.
    "minisign-data-signature-ignored": ([
        (MINISIGN, " ||\n      crypto_sign_verify_detached(sig + 10, digest, sizeof digest, key.key.data()) != 0) {",
         ") {"),
    ], ["unit:Minisign.ChangedDataIsRefused",
        UPD + ".test_a_changed_manifest_or_signed_comment_is_ignored"]),
    # The signature over the trusted comment (version and time).
    "minisign-comment-signature-ignored": ([
        (MINISIGN, """  if (crypto_sign_verify_detached(global, signed_comment.data(), signed_comment.size(),
                                  key.key.data()) != 0) {
    return Fail("the trusted comment signature does not match");
  }
""", ""),
    ], ["unit:Minisign.ChangedTrustedCommentIsRefused"]),
    "manifest-comment-not-compared": ([
        (RELEASE, """  if (sig.trusted_comment != TrustedComment(*version, *timestamp)) {
    return Fail("the signed comment does not match the release manifest");
  }
""", ""),
    ], ["unit:Release.ManifestNeedsTheRightKeyAndComment", "unit:Release.ManifestContentRules",
        UPD + ".test_a_changed_manifest_or_signed_comment_is_ignored"]),
    "manifest-file-names-unchecked": ([
        (RELEASE, "    if (!IsValidAssetName(a.name, *version) || a.sha256.size() != 64",
         "    if (a.name.empty() || a.sha256.size() != 64"),
    ], ["unit:Release.ManifestContentRules"]),
    "update-same-version-offered": ([
        (RELEASE, "  if (CompareVersions(release.version, installed) <= 0) {",
         "  if (CompareVersions(release.version, installed) < 0) {"),
    ], ["unit:Release.OnlyNewerReleasesAreOffered",
        UPD + ".test_older_and_same_versions_are_never_offered"]),
    "update-old-release-replayable": ([
        (RELEASE, "  if (release.timestamp < newest_seen) {",
         "  if (release.timestamp < newest_seen - newest_seen) {"),
    ], ["unit:Release.OnlyNewerReleasesAreOffered",
        UPD + ".test_an_older_signed_release_cannot_be_replayed"]),
    "update-any-host-allowed": ([
        (RELEASE, """  return std::find(endpoints.allowed_origins.begin(), endpoints.allowed_origins.end(), origin) !=
         endpoints.allowed_origins.end();""", "  return !endpoints.allowed_origins.empty();"),
    ], ["unit:Release.OnlyGitHubsHostsAreAllowed",
        UPD + ".test_downloads_follow_redirects_only_to_allowed_hosts"]),
    # Both of the updater's own host checks (each redirect, each request).
    "updater-redirects-unchecked": ([
        (UPDATER, "      const bool allowed = update::IsAllowedUrl(endpoints_, location);",
         "      const bool allowed = !location.empty();"),
        (UPDATER, "    if (!update::IsAllowedUrl(endpoints_, url)) {", "    if (url.empty()) {"),
    ], [UPD + ".test_downloads_follow_redirects_only_to_allowed_hosts"]),
    "update-other-repository-files-used": ([
        (RELEASE, "    if (name.empty() || url != base + name || !IsAllowedUrl(endpoints, url)) {",
         "    if (name.empty() || !IsAllowedUrl(endpoints, url)) {"),
    ], ["unit:Release.LatestReleaseAnswer",
        UPD + ".test_release_files_of_another_repository_are_not_used"]),
    "update-download-hash-unchecked": ([
        (UPDATER, """          } else if (r.sha256 != asset_.sha256) {
            problem = "The downloaded file does not match the signed release, so TorFox deleted it.";
""", ""),
    ], [UPD + ".test_a_file_that_does_not_match_the_signed_release_is_deleted"]),
    "update-not-rechecked-before-install": ([
        (UPDATER, "  if (!d || d->size != asset_.size || d->sha256 != asset_.sha256) {", "  if (!d) {"),
    ], [UPD + ".test_a_file_changed_after_its_download_is_not_installed"]),
    # Stage 9 review findings.
    "update-started-through-links": ([
        ("src/browser/platform/platform_win.cc", "  info.lpFile = real.c_str();",
         "  info.lpFile = file.c_str();"),
    ], [UPD + ".test_the_installer_is_started_by_its_real_path"]),
    "update-replay-shown-as-up-to-date": ([
        (RELEASE, "    o.replayed = true;\n", ""),
    ], ["unit:Release.OnlyNewerReleasesAreOffered",
        UPD + ".test_an_older_signed_release_cannot_be_replayed"]),
    "update-record-save-failure-ignored": ([
        (UPDATER, "  if (!SaveState()) {", "  if (!SaveState() && false) {"),
    ], [UPD + ".test_no_update_is_offered_while_the_record_cannot_be_saved"]),
    "updates-off-ignored": ([
        (UPDATER, '  return settings_ && settings_->store().GetBool("updates.enabled");',
         "  return settings_ != nullptr;"),
    ], [UPD + ".test_with_updates_off_torfox_never_contacts_github"]),
    "update-auto-check-setting-ignored": ([
        (UPDATER, '  if (Enabled() && settings_->store().GetBool("updates.autoCheck") &&\n',
         "  if (Enabled() &&\n"),
    ], [UPD + ".test_automatic_checks_can_be_turned_off"]),
    "update-auto-check-not-daily": ([
        (UPDATER, "constexpr int64_t kCheckInterval = 24 * 60 * 60;", "constexpr int64_t kCheckInterval = 0;"),
    ], [UPD + ".test_automatic_checks_happen_once_a_day_and_tell_the_user"]),
    "update-notice-setting-ignored": ([
        (UPDATER, '  if (automatic && notify && settings_->store().GetBool("updates.notify")) {',
         "  if (automatic && notify) {"),
    ], [UPD + ".test_notices_can_be_turned_off"]),

    # ----- Windows installer (Stage 8) -----
    "installer-update-does-not-wait": ([
        (ISS, "    if not WaitForTorFox(Dir, UpdateWaitSeconds) then", "    if False then"),
    ], [INST + "test_update_waits_for_torfox_then_installs_and_restarts_it",
        INST + "test_update_gives_up_when_torfox_stays_open"]),
    "installer-ignores-running-torfox": ([
        (ISS, "  while TorFoxRunning(Dir) do\n  begin\n    if WizardSilent then",
         "  while False do\n  begin\n    if WizardSilent then"),
    ], [INST + "test_setup_and_uninstall_refuse_while_torfox_is_open"]),
    "uninstaller-ignores-running-torfox": ([
        (ISS, "  while TorFoxRunning(Dir) do\n  begin\n    if UninstallSilent then",
         "  while False do\n  begin\n    if UninstallSilent then"),
    ], [INST + "test_setup_and_uninstall_refuse_while_torfox_is_open"]),
    "update-not-restarted": ([
        (ISS, 'Filename: "{app}\\TorFox.exe"; Flags: nowait runasoriginaluser; Check: IsUpdate\n', ""),
    ], [INST + "test_update_waits_for_torfox_then_installs_and_restarts_it"]),
    "installer-keeps-stale-files": ([
        (ISS, 'Type: files; Name: "{app}\\dxil.dll"\n', ""),
    ], [INST + "test_update_waits_for_torfox_then_installs_and_restarts_it"]),
    "installer-ships-build-files": ([
        (PACKAGER, "    for name in REQUIRED_FILES + [f for f in OPTIONAL_FILES if (build / f).is_file()]:",
         "    for name in [p.name for p in build.iterdir() if p.is_file()]:"),
    ], [INST + "test_install_contents_shortcut_and_uninstall"]),
    "installer-keeps-nothing-running-check": ([
        (ISS, "  Result := CheckForMutexes(RunningMutex) or ProgramFolderInUse(Dir);", "  Result := False;"),
    ], [INST + "test_update_waits_for_torfox_then_installs_and_restarts_it",
        INST + "test_setup_and_uninstall_refuse_while_torfox_is_open"]),
    # Stage 9: address-bar suggestions. Tor windows list bookmarks only.
    "suggest-history-in-tor-windows": ([
        ("src/browser/shell/browser_window.cc",
         "db->Suggest(text, static_cast<std::size_t>(bridge::kMaxSuggestions), !tor_);",
         "db->Suggest(text, static_cast<std::size_t>(bridge::kMaxSuggestions), true);"),
    ], [TOR + ".TorSuggestTest.test_tor_windows_suggest_bookmarks_only"]),
    # The list's page is never tab content.
    "suggest-page-in-tabs": ([
        ("src/core/url/internal_url.cc",
         '  return IsInternalPage(host) && host != "shell" && host != "suggest";',
         '  return IsInternalPage(host) && host != "shell";'),
    ], ["unit:NavInput.InternalPages",
        "e2e:test_suggestions.SuggestionsTest.test_the_list_cannot_do_anything_else"]),
    # The list must not stay over another tab's page.
    "suggest-kept-on-tab-switch": ([
        ("src/browser/shell/browser_window.cc",
         "void BrowserWindow::Activate(int tab_id) {\n  SuggestHide();\n",
         "void BrowserWindow::Activate(int tab_id) {\n"),
    ], ["e2e:test_suggestions.SuggestionsTest.test_escape_and_switching_tabs_hide_the_list"]),
    # Closing a window closes its list too.
    "suggest-list-not-closed": ([
        ("src/browser/shell/browser_window.cc",
         "    StartClose(suggest_client_, suggest_view, nullptr, suggest_overlay);\n", ""),
    ], ["e2e:test_suggestions.SuggestionsTest.test_the_list_closes_with_its_window"]),
}


def build(release: bool = False) -> None:
    """Builds the test tree, and the release tree (out/dev) when |release|."""
    trees = [("test", ["TorFox", "torfox_unit_tests"])] + ([("dev", ["TorFox"])] if release else [])
    for tree, targets in trees:
        result = subprocess.run([CMAKE, "--build", str(REPO / "out" / tree), "--config", "Release",
                                 "--target", *targets, "--parallel"], capture_output=True, text=True)
        if result.returncode != 0:
            sys.stdout.write(result.stdout[-4000:])
            raise RuntimeError(f"build of out/{tree} failed")


def package_installers() -> None:
    """The per-user test setups, this version and the next (the update)."""
    text = (REPO / "CMakeLists.txt").read_text(encoding="utf-8")
    major, minor, patch = re.search(r"project\(TorFox VERSION ([0-9]+)\.([0-9]+)\.([0-9]+)", text).groups()
    for version in (f"{major}.{minor}.{patch}", f"{major}.{minor}.{int(patch) + 1}"):
        result = subprocess.run([sys.executable, "-I", str(REPO / PACKAGER), "--test-peruser",
                                 "--compression", "none", "--version", version],
                                capture_output=True, text=True)
        if result.returncode != 0:
            sys.stdout.write(result.stdout[-2000:] + result.stderr[-2000:])
            raise RuntimeError(f"the {version} test setup could not be built")


def check_passes(check: str) -> bool:
    kind, _, name = check.partition(":")
    if kind == "unit":
        cmd = [str(UNIT_EXE), f"--gtest_filter={name}", "--gtest_brief=1"]
    elif kind == "e2e":
        cmd = [sys.executable, "-m", "unittest", name]
    elif kind in ("platform", "installer"):
        if kind == "installer":
            name = f"test_installer_win.PerUserInstallerTest.{name}"
        cmd = [sys.executable, "-m", "unittest", name]
        result = subprocess.run(cmd, cwd=PLATFORM_TESTS, capture_output=True, text=True, timeout=900)
        if "skipped" in result.stderr.lower() and "OK (skipped" in result.stderr:
            raise RuntimeError(f"{check} was skipped; it cannot count as a detection either way")
        return result.returncode == 0
    else:
        raise ValueError(f"unknown check kind in {check!r}")
    result = subprocess.run(cmd, cwd=pathlib.Path(__file__).parent, capture_output=True, text=True,
                            timeout=900)
    return result.returncode == 0


def apply(name: str, edits: list[tuple[str, str, str]], originals: dict[pathlib.Path, bytes]) -> None:
    for rel, old, new in edits:
        path = REPO / rel
        originals.setdefault(path, path.read_bytes())
        # Byte-exact edits: no newline translation.
        current = path.read_bytes().decode("utf-8")
        old_n, new_n = old, new
        if "\r\n" in current:
            old_n, new_n = old.replace("\n", "\r\n"), new.replace("\n", "\r\n")
        if current.count(old_n) != 1:
            raise RuntimeError(f"{name}: mutation anchor not found exactly once in {rel}")
        path.write_bytes(current.replace(old_n, new_n).encode("utf-8"))


def main(names: list[str]) -> int:
    selected = names or list(MUTATIONS)
    survivors = []
    for name in selected:
        edits, checks = MUTATIONS[name]
        originals: dict[pathlib.Path, bytes] = {}
        try:
            apply(name, edits, originals)
            build(release=any(c.startswith(("platform:", INST)) for c in checks))
            if any(c.startswith(INST) for c in checks):
                package_installers()
            still_passing = [c for c in checks if check_passes(c)]
        finally:
            for path, data in originals.items():
                path.write_bytes(data)
        if still_passing:
            print(f"{name}: SURVIVED - still passing: {', '.join(still_passing)}", flush=True)
            survivors.append(name)
        else:
            print(f"{name}: killed by all {len(checks)} check(s)", flush=True)
    # Leave builds of the clean sources behind.
    build(release=any(c.startswith(("platform:", INST)) for n in selected for c in MUTATIONS[n][1]))
    if any(c.startswith(INST) for n in selected for c in MUTATIONS[n][1]):
        package_installers()
    if survivors:
        print("Surviving mutations:", ", ".join(survivors))
        return 1
    print("All mutations were detected.")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
