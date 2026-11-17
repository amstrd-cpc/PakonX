// Copies the PakonX design system (design/pakonx) into the app's web root
// and compiles tokens.json into tokens.css. Runs before `tauri dev` and
// `tauri build`; the output folder web/ds/ is generated, never edited.
import { readFileSync, writeFileSync, mkdirSync, copyFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
const design = join(here, '..', '..', '..', 'design', 'pakonx');
const out = join(here, '..', 'web', 'ds');

const tokens = JSON.parse(readFileSync(join(design, 'tokens.json'), 'utf8'));
const themes = tokens.color.themes.map((t) => t.id);
const first = themes[0];

const alias = (v) => v.replace(/^\{(.+)\}$/, 'var(--$1)');
const valueFor = (token, theme) =>
  typeof token.value === 'string' ? token.value : token.value[theme] ?? token.value[first];

function themeBlock(theme) {
  const lines = [];
  for (const family of ['color', 'shadow']) {
    for (const t of tokens[family]?.tokens ?? []) {
      // Later themes re-declare only what differs from the first.
      if (theme !== first && valueFor(t, theme) === valueFor(t, first)) continue;
      lines.push(`  --${t.name}: ${alias(valueFor(t, theme))};`);
    }
  }
  return lines.join('\n');
}

let css = `/* PakonX — generated from tokens.json */\n`;
css += `:root, [data-theme="${first}"] {\n${themeBlock(first)}\n}\n`;
for (const theme of themes.slice(1)) css += `[data-theme="${theme}"] {\n${themeBlock(theme)}\n}\n`;

const scalars = [];
for (const family of ['spacing', 'radius']) {
  for (const t of tokens[family]?.tokens ?? []) scalars.push(`  --${t.name}: ${t.value};`);
}
for (const [key, stack] of Object.entries(tokens.type.families)) scalars.push(`  --font-${key}: ${stack};`);
css += `:root {\n${scalars.join('\n')}\n}\n`;

for (const group of tokens.type.groups) {
  for (const s of group.styles) {
    const decl = [
      `font-family: var(--font-${s.family ?? group.family})`,
      `font-size: ${s.fontSize}`,
      `line-height: ${s.lineHeight}`,
      `font-weight: ${s.fontWeight}`,
      s.letterSpacing && `letter-spacing: ${s.letterSpacing}`,
    ].filter(Boolean);
    css += `.${s.name} { ${decl.join('; ')}; }\n`;
  }
}

mkdirSync(out, { recursive: true });
writeFileSync(join(out, 'tokens.css'), css);
copyFileSync(join(design, 'components', 'bundle.css'), join(out, 'bundle.css'));
copyFileSync(join(design, 'components', 'bundle.js'), join(out, 'bundle.js'));
console.log(`design system synced → ${out}`);
