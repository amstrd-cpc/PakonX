# Button

A 24px bevelled push button with the XP 3px corner; pressing sinks it into `shadow-sunken`.

**Consumer provides:** `label`, optional `variant`, `disabled`, `onClick`.

- `go` (green, the Start-button echo) is the single primary action in a window: Scan, Start, Save roll. Never two `go` buttons side by side.
- `danger` colours only the label; use it for Discard roll, Erase, Cancel scan in progress, and confirm in a dialog.
- Labels are verbs in sentence case: "Scan roll", "Export TIFFs". No trailing ellipsis unless a dialog follows.
- Order in a footer: secondary left, `go` far right. Minimum width 75px, like XP.
- Focus shows a 2px solid `accent` ring, 2px offset — ≥3:1 on every surface.
