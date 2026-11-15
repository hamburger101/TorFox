// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

import { css } from "lit";

/** Shared layout for full-page internal pages (bookmarks, history). */
export const pageStyles = css`
  :host {
    display: block;
    min-height: 100vh;
    background: var(--page);
    color: var(--fg);
  }
  header {
    position: sticky;
    top: 0;
    z-index: 1;
    display: flex;
    align-items: center;
    gap: 16px;
    padding: 18px 32px;
    background: var(--page);
    border-bottom: 1px solid var(--border);
  }
  h1 {
    margin: 0;
    font-family: var(--font-display);
    font-size: 22px;
    font-weight: 600;
    letter-spacing: -0.01em;
    flex: none;
  }
  .search {
    display: flex;
    align-items: center;
    gap: 8px;
    flex: 0 1 420px;
    height: 36px;
    margin-left: auto;
    padding: 0 14px;
    border-radius: 18px;
    background: var(--field);
    border: 1px solid transparent;
    color: var(--fg-muted);
  }
  .search:focus-within {
    background: var(--field-focus);
    border-color: var(--accent);
  }
  .search input {
    flex: 1;
    min-width: 0;
    height: 100%;
    border: 0;
    outline: none;
    background: transparent;
    color: var(--fg);
    font: inherit;
  }
  button {
    font: inherit;
    color: inherit;
  }
  .button {
    display: inline-flex;
    align-items: center;
    gap: 6px;
    height: 32px;
    padding: 0 12px;
    border: 1px solid var(--border-strong);
    border-radius: var(--radius-s);
    background: transparent;
    font-size: 13px;
    white-space: nowrap;
  }
  .button:hover:not(:disabled) {
    background: var(--field-hover);
  }
  .button:disabled {
    color: var(--fg-faint);
  }
  .button.primary {
    background: var(--accent-strong);
    border-color: var(--accent-strong);
    color: #fff;
  }
  .button.danger {
    border-color: var(--danger);
    color: var(--danger);
  }
  .button.danger.primary {
    background: var(--danger);
    color: var(--on-accent);
  }
  .icon-button {
    display: grid;
    place-items: center;
    flex: none;
    width: 28px;
    height: 28px;
    padding: 0;
    border: 0;
    border-radius: var(--radius-s);
    background: transparent;
    color: var(--fg-muted);
  }
  .icon-button:hover:not(:disabled) {
    background: var(--field-hover);
    color: var(--fg);
  }
  .icon-button:disabled {
    color: var(--fg-faint);
    opacity: 0.5;
  }
  .button:focus-visible,
  .icon-button:focus-visible,
  .row-main:focus-visible,
  input:focus-visible,
  select:focus-visible {
    outline: none;
    box-shadow: var(--focus-ring);
  }
  .banner {
    margin: 16px 32px 0;
    padding: 10px 14px;
    border-radius: var(--radius-m);
    background: var(--danger-soft);
    color: var(--danger);
    font-size: 13px;
  }
  .banner.info {
    background: var(--accent-soft);
    color: var(--fg);
  }
  .empty {
    padding: 48px 0;
    text-align: center;
    color: var(--fg-faint);
    font-size: 13px;
  }
  .row {
    display: flex;
    align-items: center;
    gap: 10px;
    min-height: 40px;
    padding: 4px 8px 4px 12px;
    border-radius: var(--radius-m);
  }
  .row:hover {
    background: var(--card);
  }
  .row-main {
    display: flex;
    align-items: center;
    gap: 10px;
    flex: 1;
    min-width: 0;
    padding: 6px 0;
    border: 0;
    border-radius: var(--radius-s);
    background: transparent;
    text-align: left;
    cursor: default;
  }
  .row-icon {
    flex: none;
    color: var(--fg-muted);
  }
  .row-title {
    overflow: hidden;
    white-space: nowrap;
    text-overflow: ellipsis;
  }
  .row-url {
    flex: 0 1 auto;
    min-width: 0;
    overflow: hidden;
    white-space: nowrap;
    text-overflow: ellipsis;
    color: var(--fg-faint);
    font-size: 12.5px;
  }
  .row-actions {
    display: flex;
    gap: 2px;
    opacity: 0;
  }
  .row:hover .row-actions,
  .row:focus-within .row-actions {
    opacity: 1;
  }
  input.text,
  select {
    height: 32px;
    padding: 0 10px;
    border: 1px solid var(--border-strong);
    border-radius: var(--radius-s);
    background: var(--field);
    color: var(--fg);
    font: inherit;
    font-size: 13px;
  }
`;

/** The host part of an http(s) URL, for compact display. */
export function hostOf(url: string): string {
  try {
    return new URL(url).host;
  } catch {
    return url;
  }
}
