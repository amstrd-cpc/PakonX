# Window

The XP-style frame every PakonX surface lives in: a glossy `titlebar`, a `surface` body and an optional status bar.

**Consumer provides:** `title`, the body content (fill the returned `body` element), optional `status` segments, `controls: false` for panels docked inside the app window, and a `mount` element.

- One window per task: Scan, Light table, Export, Log. Dialogs are windows too.
- The title names the scanner or the task — `Pakon F-135+ — Scan`, never "PakonX" alone.
- Unfocused windows pass `inactive: true`; the bar drops to `titlebar-inactive`.
- The status bar is for live, glanceable state (warm/cold, roll, frame count), in `caption` style. Max four segments.
- Don't nest windows or put a title bar inside a body. Group sections inside a body with hairline `line` group boxes.
