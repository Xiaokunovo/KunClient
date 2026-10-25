// Button style normalizer: adds explicit ThemedFlatButton to styleless buttons,
// strips FontSize(11|12|13) and Height<32 overrides from <Button ...> tags.
// Intentionally SKIPS MainWindow.xaml (nav/chrome design) and LoginDialog.xaml (hero button design).
const fs = require('fs'), path = require('path');
const APP = path.resolve(__dirname, '..', 'src', 'KunClient.App');
const SKIP = new Set(['MainWindow.xaml', 'LoginDialog.xaml']);
const STYLELESS_TARGETS = new Set(['AboutDialog.xaml', 'DonateDialog.xaml', 'SettingsDialog.xaml', 'UpdateDialog.xaml', 'LogPage.xaml']);

function walk(d, out = []) {
  for (const e of fs.readdirSync(d, { withFileTypes: true })) {
    if (['obj', 'bin'].includes(e.name)) continue;
    const p = path.join(d, e.name);
    if (e.isDirectory()) walk(p, out);
    else if (p.endsWith('.xaml')) out.push(p);
  }
  return out;
}

const files = walk(APP).filter(f => !SKIP.has(path.basename(f)));
const changed = [];
for (const f of files) {
  const base = path.basename(f);
  let s = fs.readFileSync(f, 'utf8');
  const o = s;
  s = s.replace(/<Button\b[^>]*?(?:\/>|>)/g, tag => {
    let t = tag;
    if (STYLELESS_TARGETS.has(base) && !/Style=/.test(t)) {
      t = t.replace('<Button', '<Button Style="{StaticResource ThemedFlatButton}"');
    }
    // normalize font size overrides -> inherit 12.5 from style
    t = t.replace(/\s*FontSize="(?:11|12|13)"/g, '');
    // strip fixed heights below 32 (small HC-default-sized buttons)
    t = t.replace(/(\s)Height="(\d+(?:\.\d+)?)"(?=[\s/>])/, (m, sp, v) => parseFloat(v) < 32 ? '' : m);
    return t;
  });
  if (base === 'FeaturePage.xaml') {
    s = s.replace(/(<Setter Property="FontSize" Value=")12(")/g, '$1' + '12.5' + '$2');
  }
  if (s !== o) { fs.writeFileSync(f, s, 'utf8'); changed.push(base); }
}
console.log('CHANGED FILES:', changed.length, changed.join(', '));