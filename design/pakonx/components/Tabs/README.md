# Tabs

XP property-sheet tabs. In PakonX they sit on the glossy app bar and switch the main window between its tasks: **Scan**, **Roll**, **Light table**.

**Consumer provides:** `tabs` (`{ id, label }`), the `selected` id, `onSelect(id)`, and a `label` for the tab list. The consumer shows and hides the panels; `select(id)` moves the tab without calling `onSelect`.

- Labels are short nouns in sentence case. Three to five tabs; never scroll them.
- The selected tab takes `surface` and joins the panel under it. Unselected tabs are `tab-face` on the title bar, with `on-titlebar` text.
- Hover and selection add the thin `tab-hot` top edge, the one warm touch of XP Luna tabs. Use `tab-hot` nowhere else.
- Arrow Left/Right move between tabs; only the selected tab is in the Tab order.
