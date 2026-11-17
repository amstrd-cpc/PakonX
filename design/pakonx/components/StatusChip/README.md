# StatusChip

A pill that reports one piece of live state — scanner, lamp, frame or roll — in a word plus a dot.

**Consumer provides:** `label` (the word that carries the meaning) and `tone`.

- Tones: `ok` (Warm, Booted, Saved), `warn` (Lamp warming, Dust flagged), `danger` (USB lost, MD5 mismatch), `neutral` (Cold ROM, Idle), `film` (the roll badge only).
- The label must stand on its own; colour is never the only signal.
- One or two words, sentence case. Use in status bars, panel headers and frame properties — not inside buttons.
