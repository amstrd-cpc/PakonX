# Notes for Claude

- Start with docs/DEVELOPMENT.md and docs/STATUS.md. Every protocol claim carries an
  evidence marker; never invent protocol values.
- Hardware safety charter (docs/DEVELOPMENT.md) is binding: no live scan, lamp,
  motor or EEPROM-write path is ever triggered without the operator's
  explicit approval.

## UI work (PakonX)

- The desktop app is `apps/pakonx/` (Tauri 2, plain HTML/CSS/JS front end).
- All UI follows the PakonX design system: read `design/pakonx/README.md`
  first, then the component guidelines in `design/pakonx/components/*/README.md`.
  Use the tokens (`var(--name)`) and the `window.PakonX` components; don't
  hard-code colours, sizes or fonts.
- Edit the design system in `design/pakonx/`; `apps/pakonx/web/ds/` is generated.
- The UI reaches hardware only through `apps/pakonx/src-tauri/src/cli.rs`.
  Extending its allow-list is a hardware-safety change: say so and ask first.
