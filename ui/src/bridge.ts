// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// The only channel from internal pages to TorFox. The browser process injects
// window.torfoxQuery into TorFox's own pages and nowhere else, and validates
// every message (page, command, argument types) before acting on it.

interface Query {
  request: string;
  persistent: boolean;
  onSuccess: (response: string) => void;
  onFailure: (code: number, message: string) => void;
}

interface BridgeWindow {
  torfoxQuery?: (query: Query) => number;
  torfoxQueryCancel?: (id: number) => void;
}

// Captured once at load time.
const query = (window as BridgeWindow).torfoxQuery;
const cancel = (window as BridgeWindow).torfoxQueryCancel;

export class BridgeError extends Error {
  readonly code: number;
  constructor(code: number, message: string) {
    super(message);
    this.code = code;
  }
}

export const bridgeAvailable = typeof query === "function";

function send(cmd: string, args: Record<string, unknown> | undefined, persistent: boolean,
              onSuccess: (r: string) => void, onFailure: (e: BridgeError) => void): number {
  if (!query) {
    onFailure(new BridgeError(-1, "TorFox bridge unavailable"));
    return -1;
  }
  const request = JSON.stringify(args === undefined ? { cmd } : { cmd, args });
  return query({
    request,
    persistent,
    onSuccess,
    onFailure: (code, message) => onFailure(new BridgeError(code, message)),
  });
}

/** One request, one response. */
export function call<T = unknown>(cmd: string, args?: Record<string, unknown>): Promise<T> {
  return new Promise<T>((resolve, reject) => {
    send(cmd, args, false, (r) => resolve(JSON.parse(r) as T), reject);
  });
}

/** A persistent query: |onMessage| runs for every update. Returns an unsubscribe function. */
export function subscribe<T>(cmd: string, onMessage: (message: T) => void,
                             onError?: (error: BridgeError) => void): () => void {
  const id = send(cmd, undefined, true, (r) => onMessage(JSON.parse(r) as T),
                  (e) => onError?.(e));
  return () => {
    if (id >= 0 && cancel) {
      cancel(id);
    }
  };
}
