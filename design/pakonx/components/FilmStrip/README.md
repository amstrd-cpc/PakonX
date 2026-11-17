# FilmStrip

The roll as a physical strip: an orange `film` base with sprocket rows, frames set in a sunken tray, arrow keys to step through.

**Consumer provides:** `frames` (`{ no, src?, flagged? }`, numbered as printed on the film edge), `selected`, `onSelect`, a `label` naming the roll, and optionally `multiple` + `onMark` for batch actions.

- Frames without a thumbnail yet show the neutral `loupe` grey with their number — that is the "not scanned" state.
- The selected frame gets a 2px `accent` outline; `flagged` adds a `warn` badge for dust or exposure problems.
- On the light table, keep the strip in one horizontal row that scrolls; never wrap frames onto a second line.
- For the roll view, `perRow` (6, like a sleeve) lays the roll out as a contact sheet: strips of that many frames, one under another, in one list. Up/Down move a strip. `--pk-frame-w` on the root sets the frame width (height follows at 3:2) for a size slider.
- `onOpen` fires on double-click or Enter: open that frame in the light table.
- Thumbnails are positives; the orange base is the only nod to the negative.
- With `multiple`, frames can be marked for a batch action (save these): Ctrl/Cmd-click or Space toggles one, Shift-click marks the run from the last frame clicked, a plain click clears the marks (and selects that frame). The consumer may call `mark()` for Select all (Ctrl+A) and to clear (Esc). Marked frames get a 3px `selection` border and a `selection` number tab; the selected frame keeps its `accent` outline. Say what is marked in words next to the action ("Save 3 pictures").
