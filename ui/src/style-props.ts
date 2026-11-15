// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Inline style properties under TorFox's CSP (style-src 'self').
//
// lit's styleMap writes its first value as a style="..." attribute, which the
// CSP refuses (the element then has no style until its next update). This
// directive only ever uses the CSSOM (style.setProperty), which the CSP
// allows, so the first render is styled too.

import { noChange } from "lit";
import { Directive, directive, PartType } from "lit/directive.js";
import type { AttributePart, PartInfo } from "lit/directive.js";

class StylePropsDirective extends Directive {
  private previous = new Set<string>();

  constructor(partInfo: PartInfo) {
    super(partInfo);
    if (partInfo.type !== PartType.ATTRIBUTE || partInfo.name !== "style") {
      throw new Error("styleProps belongs in a style attribute");
    }
  }

  render(_props: Readonly<Record<string, string>>): typeof noChange {
    return noChange;
  }

  override update(part: AttributePart, [props]: [Readonly<Record<string, string>>]): typeof noChange {
    const style = (part.element as HTMLElement).style;
    for (const name of this.previous) {
      if (!(name in props)) {
        style.removeProperty(name);
      }
    }
    for (const [name, value] of Object.entries(props)) {
      style.setProperty(name, value);
    }
    this.previous = new Set(Object.keys(props));
    return noChange;
  }
}

// Usage: style=${styleProps({ width: "40%" })} (CSS property names).
export const styleProps = directive(StylePropsDirective);
