$path = 'c:\Users\Raps\Desktop\Everything Else\Arduino Projects\eink1\AdventureCattoEink\tiny-reader\src\FreeSerif9pt8b.h'
$text = Get-Content -Raw $path

# Expand underscore bitmap from 2 to 4 bytes
$oldBytes = '0xC1, 0xFF, 0x80, 0x84, 0x20'
$newBytes = '0xC1, 0x00, 0x00, 0xFF, 0x80, 0x84, 0x20'
if ($text -notlike "*$oldBytes*") { throw 'Bitmap context not found' }
$text = $text.Replace($oldBytes, $newBytes)

# Patch glyph table entries from 0x60 upward (+2 bitmap offset), and 0x5F height
$glyphStart = $text.IndexOf('const GFXglyph FreeSerif9ptGlyphs[] PROGMEM = {')
$glyphEnd = $text.IndexOf('};', $glyphStart)
$glyphBlock = $text.Substring($glyphStart, $glyphEnd - $glyphStart)

$linePattern = '\{\s*(\d+),\s*(\d+),\s*(\d+),\s*(\d+),\s*(-?\d+),\s*(-?\d+)\s*\}(\s*//[^\n]*)?'
$glyphIndex = 0
$newBlock = [regex]::Replace($glyphBlock, $linePattern, {
  param($m)
  $off = [int]$m.Groups[1].Value
  $w = [int]$m.Groups[2].Value
  $h = [int]$m.Groups[3].Value
  $xAdv = [int]$m.Groups[4].Value
  $xOff = [int]$m.Groups[5].Value
  $yOff = [int]$m.Groups[6].Value
  $comment = $m.Groups[7].Value

  if ($glyphIndex -eq (0x5F - 0x20)) { $h = 2 }
  elseif ($glyphIndex -gt (0x5F - 0x20)) { $off += 2 }

  $script:glyphIndex++
  return ('    {{ {0,5}, {1,3}, {2,3}, {3,3}, {4,4}, {5,4}}}{6}' -f $off, $w, $h, $xAdv, $xOff, $yOff, $comment)
})

$text = $text.Substring(0, $glyphStart) + $newBlock + $text.Substring($glyphEnd)
[System.IO.File]::WriteAllText($path, $text)
Write-Host 'Font patched successfully'
