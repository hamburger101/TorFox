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
    note: "Searches from the address bar and the new tab page go to DuckDuckGo.",
  },
  privacy: {
    title: "Privacy & Security",
    icon: icons.shield,
    note:
      "These protections are still being built. Each control stays disabled until its feature " +
      "is finished and tested. In this build, websites cannot use your camera, microphone, " +
      "location, notifications or other permissions: every request is denied.",
  },
  downloads: {
    title: "Downloads",
    icon: icons.download,
    note: "Download protection is still being built. Until it is finished, this build blocks every download.",
  },
  tor: {
    title: "Tor Network",
    icon: icons.onion,
    note:
      "Tor is not available in this build. Onion addresses are refused and are never sent " +
      "to a search engine or resolved through ordinary DNS.",
  },
  appearance: { title: "Appearance", icon: icons.appearance },
  updates: {
    title: "Updates",
    icon: icons.update,
    note:
      "Automatic updates are not built yet. This build never checks for or installs updates " +
      "by itself.",
  },
  about: { title: "About TorFox", icon: icons.info },
};

export interface SettingLabel {
  title: string;
  description?: string;
  values?: Record<string, string>;
  group?: string;
}

const PERMISSION_VALUES = { deny: "Block", ask: "Ask" };

export const SETTINGS: Record<string, SettingLabel> = {
  "general.startupPage": {
    title: "On startup",
    values: { newtab: "Open a new tab", restore: "Restore previous tabs" },
  },
  "search.engine": {
    title: "Search engine",
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
    description: "Block requests to known tracking domains.",
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
  "privacy.clearOnExit.history": { title: "History", group: "Clear when TorFox closes" },
  "privacy.clearOnExit.cookies": { title: "Cookies and site data", group: "Clear when TorFox closes" },
  "privacy.clearOnExit.cache": { title: "Cached files", group: "Clear when TorFox closes" },
  "privacy.clearOnExit.downloads": { title: "Download list", group: "Clear when TorFox closes" },
  "permissions.camera": { title: "Camera", values: PERMISSION_VALUES, group: "Site permissions" },
  "permissions.microphone": { title: "Microphone", values: PERMISSION_VALUES, group: "Site permissions" },
  "permissions.location": { title: "Location", values: PERMISSION_VALUES, group: "Site permissions" },
  "permissions.notifications": {
    title: "Notifications",
    values: PERMISSION_VALUES,
    group: "Site permissions",
  },
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
    values: { "on-demand": "When I open a Tor tab", "at-launch": "When TorFox starts" },
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
    description: "One bridge line per line.",
    group: "Bridges",
  },
  "appearance.theme": {
    title: "Theme",
    values: { dark: "Dark", light: "Light", system: "Match system" },
  },
  "updates.enabled": { title: "Updates", description: "Allow TorFox to update itself." },
  "updates.autoCheck": {
    title: "Check for updates automatically",
    description: "Look for new releases on GitHub once a day.",
  },
  "updates.notify": { title: "Tell me when an update is available" },
};

export const ERRORS: Record<string, string> = {
  not_available: "This setting is not available in this build yet.",
  invalid_value: "That value is not allowed.",
  unknown_key: "TorFox does not know this setting.",
  save_failed: "TorFox could not save your settings to disk. The change was not applied.",
  rejected: "TorFox refused this request.",
};
