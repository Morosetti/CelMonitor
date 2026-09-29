<#
  Generates CelMonitor.ico (16/24/32/48/64/256 px, PNG-compressed entries) — a monitor with a phone beside it.
  Run once; the .ico is versioned. Requires Windows PowerShell (System.Drawing).
#>
Add-Type -AssemblyName System.Drawing
$sizes = 16, 24, 32, 48, 64, 256
$images = foreach ($s in $sizes) {
    $bmp = New-Object System.Drawing.Bitmap $s, $s
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = 'AntiAlias'
    $g.Clear([System.Drawing.Color]::Transparent)
    $u = $s / 32.0
    $blue = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(255, 33, 150, 243))
    $dark = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(255, 20, 28, 40))
    $screen = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(255, 144, 202, 249))
    # monitor
    $g.FillRectangle($blue, 1 * $u, 4 * $u, 22 * $u, 16 * $u)
    $g.FillRectangle($screen, 3 * $u, 6 * $u, 18 * $u, 12 * $u)
    $g.FillRectangle($blue, 10 * $u, 20 * $u, 4 * $u, 4 * $u)
    $g.FillRectangle($blue, 6 * $u, 24 * $u, 12 * $u, 2 * $u)
    # phone (in front, right)
    $g.FillRectangle($dark, 19 * $u, 9 * $u, 12 * $u, 21 * $u)
    $g.FillRectangle($blue, 20 * $u, 10 * $u, 10 * $u, 17 * $u)
    $g.FillRectangle($screen, 21 * $u, 11 * $u, 8 * $u, 15 * $u)
    $g.Dispose()
    $ms = New-Object System.IO.MemoryStream
    $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
    , $ms.ToArray()
}
$out = New-Object System.IO.MemoryStream
$w = New-Object System.IO.BinaryWriter $out
$w.Write([uint16]0); $w.Write([uint16]1); $w.Write([uint16]$sizes.Count)   # ICONDIR
$offset = 6 + 16 * $sizes.Count
for ($i = 0; $i -lt $sizes.Count; $i++) {                                   # ICONDIRENTRY
    $dim = if ($sizes[$i] -ge 256) { 0 } else { $sizes[$i] }
    $w.Write([byte]$dim); $w.Write([byte]$dim); $w.Write([byte]0); $w.Write([byte]0)
    $w.Write([uint16]1); $w.Write([uint16]32)
    $w.Write([uint32]$images[$i].Length); $w.Write([uint32]$offset)
    $offset += $images[$i].Length
}
foreach ($img in $images) { $w.Write($img) }
[IO.File]::WriteAllBytes((Join-Path $PSScriptRoot "CelMonitor.ico"), $out.ToArray())
Write-Host "CelMonitor.ico gerado"
