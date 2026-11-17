PakonX is the interface for a from-scratch scanning stack for the Kodak/Pakon F-135 / F-135+ / F-235 / F-335 family. It borrows the confidence of early-2000s Windows XP — glossy title bars, bevelled buttons, segmented progress, a green "go" — and sets it on a modern 4px grid with neutral surfaces, so the frames, not the chrome, are what you judge.

## Principles

- **Nostalgia in the chrome, neutrality in the work.** XP gloss and bevels live only on window frames, buttons, progress and chips. Anything around a scanned frame — the loupe, the strip tray, the light table — is flat, neutral `desktop`, `surface-sunken` or `loupe` grey.
- **Say how sure we are.** The scanner stack never invents. The UI matches it: state is shown as words (`Warm`, `Cold ROM`, `USB lost`), numbers are exact and in `data` mono, and nothing animates to suggest progress we can't measure.
- **One green button.** Every window has at most one `go` action. Everything else is a plain bevelled button.
- **Film is orange.** The `film` colour — C-41 base orange — is the brand. Use it for the strip, the lamp sweep and the roll badge, nowhere else.

## Voice

- Plain, technical, calm. Talk like the lab tech, not the marketing page: "Insert the strip emulsion side down." "Lamp warming — about 30 seconds."
- Sentence case everywhere, including title bars and buttons. Buttons are verbs: "Scan roll", "Export TIFFs", "Discard roll".
- Address the user as *you*; the app doesn't say *I* or *we*.
- Name hardware the way it identifies itself: `F-135+`, `0F05:F135`, `Pakon7`. Hex and IDs go in `data` style.
- Errors say what happened and what to do: "USB lost. Reconnect the scanner; the roll is kept." No exclamation marks, no emoji.

## Colour

- Two themes. **Darkroom** (first, the default) is for scanning and grading — near-black `desktop`, graphite `surface`. **Luna** is the daylight theme with the classic XP `surface` beige and a brighter `titlebar` blue.
- `ink` on `desktop`, `surface`, `surface-raised` and `surface-sunken`; `ink-muted` for captions on `surface` and `surface-raised`.
- `titlebar` + `titlebar-gloss` only for window title bars and the app header; text there is `on-titlebar`.
- `selection` fills selected rows and menu items with `on-selection` text. `accent` is for links, the focus ring, slider thumbs and the loupe crosshair.
- `go` is the primary action fill (label in `on-go`). `film` is the brand orange (label in `on-film`), never used as text.
- `ok`, `warn`, `danger` are text/icon colours for state and always come with a word.
- `loupe` is the 18%-ish grey matte around a frame under inspection. Never put UI or text on it.

## Type

- `chrome` family (Trebuchet MS → Segoe UI → Tahoma) for headings and title bars — the XP title-bar face. Styles: `display`, `heading`, `title`.
- `ui` family (Tahoma → Segoe UI → Verdana) for everything you read in the interface. Styles: `body` (default, 13px), `label`, `caption`.
- `mono` family (IBM Plex Mono, from Google Fonts → Consolas) as `data` for frame numbers, DX codes, USB IDs, hex dumps and every progress number.
- Chrome and UI faces are system fonts on Windows (the platform the scanner runs on); macOS and Linux fall back through the stack.

## Space, shape, depth

- 4px grid: `space-1` 4 · `space-2` 8 · `space-3` 12 · `space-4` 16 · `space-6` 24 · `space-8` 32. Window bodies pad `space-4`; group boxes `space-3`.
- Corners: `radius-sm` (3px) on every control, `radius-md` on group boxes and the strip tray, `radius-window` on a window's top corners only, `radius-pill` for chips.
- Depth is bevels, not blur. `shadow-raised` for buttons, `shadow-sunken` for fields, wells, the progress trough and pressed buttons. `shadow-window` is the only soft shadow, for a window over `desktop`.
- Borders: controls take a 1px `control-border`; dividers and group boxes a 1px `line`.

## States

- Hover: border turns `accent`. Pressed: swap to `shadow-sunken` and nudge the label 1px down.
- Focus: 2px solid `accent` outline, 2px offset, on every focusable thing — ≥3:1 on every surface in both themes.
- Disabled: `ink-muted` label, `line` border, no bevel.
- Motion: progress fills step linearly; nothing bounces, pulses or fades in. The only "animated" thing is real progress.

## Layout

- The main window has three tasks on XP Tabs in the app bar: **Scan**, **Roll** (the contact sheet: FilmStrip with `perRow` 6) and **Light table** (one frame on a `loupe` matte, the FilmStrip along the bottom).
- Roll and Light table share a Rolls task pane on the left and an inspector on the right. Roll holds what applies to the whole roll (film, dust removal, frame format, framing, saving); Light table holds what applies to one picture (its look, turn, save picture).
- The log is a drawer opened from the status bar, not a pane; the status bar shows its last line.
- The status bar at the foot of the main window always shows scanner state, roll and frame count.

## Iconography

- No icon set ships yet. Until one does, use words and the StatusChip dot; the window icon is a plain `film` square with a `surface-sunken` inset.
- When icons arrive: 16px and 32px, XP-style full-colour with a 1px dark outline in the chrome; 1.5px monoline in `ink` inside the workspace. Never emoji.

## Logo

- There is no PakonX logo in this system. Set the name "PakonX" in `display` type — one word, capital P and X. Do not use Kodak's marks or trade dress.
