$path = 'c:\Users\Raps\Desktop\Everything Else\Arduino Projects\eink1\AdventureCattoEink\tiny-reader\src\FreeSerif9pt8b.h'
$text = Get-Content -Raw $path
$bm = [regex]::Match($text, 'const uint8_t FreeSerif9ptBitmaps\[\] PROGMEM = \{(.*?)\};', 'Singleline').Groups[1].Value
$vals = New-Object System.Collections.Generic.List[int]
foreach ($part in $bm.Split(',')) {
  $p = $part.Trim()
  if ($p -match '^0x[0-9A-Fa-f]+$') { [void]$vals.Add([Convert]::ToInt32($p, 16)) }
}
Write-Host "783-788:" (($vals[780..787] | ForEach-Object { '0x{0:X2}' -f $_ }) -join ', ')
