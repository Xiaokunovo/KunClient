# Multi-line-aware Button attribute scanner
$roots = @('src\KunClient.App\Pages','src\KunClient.App\Dialogs','src\KunClient.App\Controls','src\KunClient.App')
$files = Get-ChildItem $roots -Filter *.xaml -File -ErrorAction SilentlyContinue | Where-Object { $_.FullName -notmatch '\\obj\\|\\bin\\' } | Sort-Object FullName -Unique
$report = @()
foreach ($f in $files) {
    $raw = [IO.File]::ReadAllText($f.FullName)
    $ms = [regex]::Matches($raw, '<Button\b[^>]*?>', 'Singleline')
    foreach ($m in $ms) {
        $tag = $m.Value
        $line = ($raw.Substring(0, $m.Index) -split "`n").Count
        $style = if ($tag -match 'Style="[^"]*\{(Static|Dynamic)Resource ([^}]+)\}"') { $Matches[2] } else { '(none)' }
        $h  = if ($tag -match '\bHeight="([0-9.]+)"') { $Matches[1] } else { '' }
        $mh = if ($tag -match '\bMinHeight="([0-9.]+)"') { $Matches[1] } else { '' }
        $pad = if ($tag -match '\bPadding="([^"]+)"') { $Matches[1] } else { '' }
        $fs = if ($tag -match '\bFontSize="([0-9.]+)"') { $Matches[1] } else { '' }
        $w  = if ($tag -match '\bWidth="([0-9.]+)"') { $Matches[1] } else { '' }
        $flag = ''
        if ($style -eq '(none)' -or $h -ne '' -or ($pad -ne '' -and $pad -ne '14,9') -or ($fs -ne '' -and $fs -ne '12.5')) { $flag = '<< DEV' }
        $report += ('{0}:{1} style={2} H={3} MinH={4} Pad={5} FS={6} W={7} {8}' -f $f.Name, $line, $style, $h, $mh, $pad, $fs, $w, $flag)
    }
}
$report | Out-String -Width 220