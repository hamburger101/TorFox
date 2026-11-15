// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Display text for the settings schema (src/core/settings/schema.cc).
// Notes describe what the current build actually does and must stay
// truthful: update them only together with the tested behaviour.

import type { TemplateResult } from "lit";

import { icons } from "../icons.js";

export interface SectionLabel {
  title: string;
  icon: TemplateResult;
  note?: string;
}

export const SECTIONS: Record<string, SectionLabel> = {
  general: { title: "General", icon: icons.general },
  search: {
    title: "Search Engine",
    icon: icons.search,
    note:
      "Searches from the address bar, the new tab page and the page menu go to the DuckDuckGo " +
      "page you choose here. In Tor windows they go to the same page on DuckDuckGo's onion " +
      "service. Onion addresses are never searched for.",
  },
  privacy: {
    title: "Privacy & Security",
    icon: icons.shield,
    note:
      "Camera, microphone and clipboard are blocked unless you choose Ask: TorFox then asks " +
      "every time and never remembers the answer. Location, notifications and every other " +
      "permission are always denied in this build. Links that open another app ask first, and " +
      "only email and phone links can do so.",
  },
  downloads: {
    title: "Downloads",
    icon: icons.download,
    note:
      "Files are saved to your Downloads folder. TorFox never opens a downloaded file for you. " +
      "Pages that try to save a file another way (for example a “Save as” picker) are refused " +
      "while downloads are disabled.",
  },
  tor: {
    title: "Tor Network",
    icon: icons.onion,
    note:
      "Tor windows send every connection through the Tor network, using the official Tor " +
      "client that comes with TorFox, and wait while Tor is not connected. Onion addresses " +
      "open only in Tor windows and are never searched for or looked up with ordinary DNS. " +
      "TorFox is not Tor Browser: it lacks Tor Browser's protections against fingerprinting, " +
      "and it keeps sites apart only by giving each site address its own Tor circuits.",
  },
  appearance: { title: "Appearance", icon: icons.appearance },
  updates: {
    title: "Updates",
    icon: icons.update,
    note:
      "TorFox looks for new releases on GitHub, in TorFox's official repository. It uses an " +
      "update only if the release's signature verifies with the key built into this copy of " +
      "TorFox, the release is newer than this version, and the downloaded file has exactly the " +
      "size and SHA-256 the signed release lists. On Windows the installer starts when TorFox " +
      "closes, installs the update and starts TorFox again. On macOS TorFox opens the checked disk image " +
      "and you copy TorFox to Applications yourself.",
  },
  about: { title: "About TorFox", icon: icons.info },
};

// Rows that are not settings: shown after a group's settings.
export interface FixedRow {
  title: string;
  value: string;
  description?: string;
}

export const FIXED_ROWS: Record<string, FixedRow[]> = {
  "Site permissions": [
    {
      title: "Location",
      value: "Always blocked",
      description: "TorFox does not share your location with sites.",
    },
    {
      title: "Notifications",
      value: "Always blocked",
      description: "TorFox does not show notifications from sites.",
    },
  ],
};

export interface SettingLabel {
  title: string;
  description?: string;
  values?: Record<string, string>;
  group?: string;
  // Shown instead of the generic message when the value is refused.
  invalid?: string;
}

const PERMISSION_VALUES = { deny: "Block", ask: "Ask" };

export const SETTINGS: Record<string, SettingLabel> = {
  "general.startupPage": {
    title: "On startup",
    description:
      "Restore previous tabs reopens the tabs of your normal windows, also after a crash. " +
      "Private and Tor windows are never reopened. While “Clear when TorFox closes: History” " +
      "is on, TorFox keeps no tabs and opens a new tab.",
    values: { newtab: "Open a new tab", restore: "Restore previous tabs" },
  },
  "search.engine": {
    title: "Search engine",
    description: "HTML works without JavaScript; Lite is a plain text page.",
    values: {
      duckduckgo: "DuckDuckGo",
      "duckduckgo-html": "DuckDuckGo (HTML)",
      "duckduckgo-lite": "DuckDuckGo Lite",
    },
  },
  "privacy.httpsFirst": {
    title: "HTTPS-First",
    description: "Try every site over a secure connection first and warn before using HTTP.",
    group: "Browsing protection",
  },
  "privacy.blockTrackers": {
    title: "Block trackers",
    description: "Block other sites' requests to known tracking domains (EasyPrivacy list).",
    group: "Browsing protection",
  },
  "privacy.blockThirdPartyCookies": {
    title: "Block third-party cookies",
    description: "Stop embedded sites from setting or reading cookies.",
    group: "Browsing protection",
  },
  "privacy.blockPopups": {
    title: "Block pop-ups",
    description: "Block windows that sites open without you clicking.",
    group: "Browsing protection",
  },
  "privacy.useSystemProxy": {
    title: "Use the system's proxy settings",
    description:
      "Off: TorFox connects directly and never looks for a proxy on your network. Turn this on " +
      "only if your network requires a proxy. With Windows' “Automatically detect settings” on, " +
      "TorFox then asks the local network for one (WPAD), which others on it can answer.",
    group: "Network",
  },
  "privacy.clearOnExit.history": {
    title: "History",
    description: "Your browsing history, and the site icons and shortcuts kept with it.",
    group: "Clear when TorFox closes",
  },
  "privacy.clearOnExit.cookies": {
    title: "Cookies and site data",
    description: "Cookies and everything sites store: local storage, databases, service workers.",
    group: "Clear when TorFox closes",
  },
  "privacy.clearOnExit.cache": {
    title: "Cached files",
    description: "Copies of pages and images, and the record of which sites TorFox connected to.",
    group: "Clear when TorFox closes",
  },
  "permissions.camera": { title: "Camera", values: PERMISSION_VALUES, group: "Site permissions" },
  "permissions.microphone": { title: "Microphone", values: PERMISSION_VALUES, group: "Site permissions" },
  "permissions.clipboard": { title: "Clipboard", values: PERMISSION_VALUES, group: "Site permissions" },
  "downloads.askBeforeDownload": {
    title: "Ask before downloading",
    description: "Confirm every download, showing its name, size and source.",
  },
  "downloads.disableDownloads": {
    title: "Disable all downloads",
    description: "Block every download. This overrides “Ask before downloading”.",
  },
  "tor.startMode": {
    title: "Connect to Tor",
    values: { "on-demand": "When I open a Tor window", "at-launch": "When TorFox starts" },
  },
  "tor.bridges.mode": {
    title: "Bridges",
    description: "Bridges help you connect where Tor is blocked.",
    values: { none: "None", builtin: "Built-in", custom: "Custom" },
    group: "Bridges",
  },
  "tor.bridges.builtinType": {
    title: "Built-in bridge type",
    values: { obfs4: "obfs4", snowflake: "Snowflake", meek: "meek" },
    group: "Bridges",
  },
  "tor.bridges.custom": {
    title: "Custom bridges",
    description:
      "Used when Bridges is Custom: one bridge line per line, as bridges.torproject.org gives " +
      "them (obfs4, WebTunnel, Snowflake, meek, or a plain address). Lines starting with # are ignored.",
    group: "Bridges",
    invalid:
      "One of these lines is not a bridge line TorFox can use. Check that each line was copied " +
      "completely; nothing was saved.",
  },
  "appearance.theme": {
    title: "Theme",
    values: { dark: "Dark", light: "Light", system: "Match system" },
  },
  "appearance.showBookmarksBar": {
    title: "Show bookmarks bar",
    description: "Show the Bookmarks bar folder below the address bar.",
  },
  "updates.enabled": {
    title: "Updates",
    description: "Allow TorFox to look for, download and install updates. When this is off, " +
      "TorFox never contacts GitHub.",
  },
  "updates.autoCheck": {
    title: "Check for updates automatically",
    description: "Look for a new release at most once a day, a little after TorFox starts. " +
      "Nothing is downloaded or installed without you.",
  },
  "updates.notify": {
    title: "Tell me when an update is available",
    description: "After an automatic check finds one, show a notice in a TorFox window.",
  },
};

export const ERRORS: Record<string, string> = {
  not_available: "This setting is not available in this build yet.",
  invalid_value: "That value is not allowed.",
  unknown_key: "TorFox does not know this setting.",
  save_failed: "TorFox could not save your settings to disk. The change was not applied.",
  rejected: "TorFox refused this request.",
};
