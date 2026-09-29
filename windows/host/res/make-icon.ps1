<#
  Generates the CelMonitor logo: a monitor with a phone beside it and one window spanning both screens
  ("the phone extends the desktop"). Outputs (versioned, run again only to change the logo):
    CelMonitor.ico            16..256 px (PNG entries) - exe, window, taskbar, tray while streaming, installer
    CelMonitorIdle.ico        same with grey screens - tray/window while no phone is streaming
    ..\..\..\docs\logo.png    256 px, for the README
    -Preview <file.png>       optional sheet with every size on light and dark backgrounds (for review)
  Requires Windows PowerShell (System.Drawing). The Android launcher icon (android/.../ic_launcher_foreground.xml)
  uses the same geometry on its 108-unit grid.
#>
param([string]$Preview)
Add-Type -AssemblyName System.Drawing

function RoundRect([float]$x, [float]$y, [float]$w, [float]$h, [float]$r) {
    $p = New-Object System.Drawing.Drawing2D.GraphicsPath
    $d = [Math]::Min(2 * $r, [Math]::Min($w, $h))
    if ($d -le 0.01) { $p.AddRectangle((New-Object System.Drawing.RectangleF $x, $y, $w, $h)); return , $p }
    $p.AddArc($x, $y, $d, $d, 180, 90)
    $p.AddArc($x + $w - $d, $y, $d, $d, 270, 90)
    $p.AddArc($x + $w - $d, $y + $h - $d, $d, $d, 0, 90)
    $p.AddArc($x, $y + $h - $d, $d, $d, 90, 90)
    $p.CloseFigure()
    , $p
}

function Color([string]$hex, [int]$a = 255) {
    [System.Drawing.Color]::FromArgb($a, [Convert]::ToInt32($hex.Substring(1, 2), 16),
        [Convert]::ToInt32($hex.Substring(3, 2), 16), [Convert]::ToInt32($hex.Substring(5, 2), 16))
}

# Design grid: 32 x 32 units.
# -Idle: grey screens, used by the tray/window while no phone is streaming.
function Draw-Logo([int]$s, [switch]$Idle) {
    $bmp = New-Object System.Drawing.Bitmap $s, $s
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = 'AntiAlias'
    $g.PixelOffsetMode = 'HighQuality'
    $g.Clear([System.Drawing.Color]::Transparent)
    $g.ScaleTransform($s / 32.0, $s / 32.0)
    $small = $s -le 20

    $frame = New-Object System.Drawing.SolidBrush (Color '#3D4B63')
    $screenRect = New-Object System.Drawing.RectangleF 0, 4, 32, 24
    if ($Idle) { $c1 = '#A3AEBF'; $c2 = '#77839A'; $barC = '#D9DEE6' } else { $c1 = '#4DA3FF'; $c2 = '#1D5FE0'; $barC = '#FFB020' }
    $screen = New-Object System.Drawing.Drawing2D.LinearGradientBrush $screenRect, (Color $c1), (Color $c2), 60.0
    $win = New-Object System.Drawing.SolidBrush (Color '#FFFFFF')
    $bar = New-Object System.Drawing.SolidBrush (Color $barC)

    # Monitor + stand
    $g.FillPath($frame, (RoundRect 0.5 4 24 17.5 2.2))
    $monScreen = RoundRect 2 5.5 21 14.5 1
    $g.FillPath($screen, $monScreen)
    $g.FillRectangle($frame, 10.5, 21, 4, 3.5)
    $g.FillPath($frame, (RoundRect 6.5 24 12 2.5 1.2))

    # Phone in front (portrait), separated from the monitor by a transparent gap so it reads at 16 px.
    $gap = if ($small) { 2 } else { 1.3 }
    $px = 19.5; $py = 10.5; $pw = 12; $ph = 20
    $g.CompositingMode = 'SourceCopy'
    $clear = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::Transparent)
    $g.FillPath($clear, (RoundRect ($px - $gap) ($py - $gap) ($pw + 2 * $gap) ($ph + 2 * $gap) (2.6 + $gap)))
    $g.CompositingMode = 'SourceOver'
    $g.FillPath($frame, (RoundRect $px $py $pw $ph 2.6))
    $phScreen = RoundRect ($px + 1.4) ($py + 1.6) ($pw - 2.8) ($ph - 3.6) 1
    $g.FillPath($screen, $phScreen)

    # One window spanning both screens: clipped to the monitor screen (minus the phone) and to the phone screen.
    # Same height on both screens, so its title bar visibly continues from one screen into the other.
    $wx = 8; $wy = 13.5; $ww = 19.5; $wh = 8
    $barH = if ($small) { 2.5 } else { 2.2 }
    $clip = New-Object System.Drawing.Region $monScreen
    $clip.Exclude((RoundRect ($px - $gap) ($py - $gap) ($pw + 2 * $gap) ($ph + 2 * $gap) (2.6 + $gap)))
    $clip.Union($phScreen)
    $g.Clip = $clip
    $g.FillPath($win, (RoundRect $wx $wy $ww $wh 0.8))
    $g.FillRectangle($bar, $wx, $wy, $ww, $barH)
    $g.ResetClip()

    $g.Dispose()
    $bmp
}

function PngBytes($bmp) {
    $ms = New-Object System.IO.MemoryStream
    $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
    , $ms.ToArray()
}

function Write-Ico([hashtable]$bitmaps, [int[]]$sizes, [string]$path) {
    # .ico with PNG-compressed entries (Vista+)
    $out = New-Object System.IO.MemoryStream
    $w = New-Object System.IO.BinaryWriter $out
    $w.Write([uint16]0); $w.Write([uint16]1); $w.Write([uint16]$sizes.Count)
    $images = foreach ($s in $sizes) { , (PngBytes $bitmaps[$s]) }
    $offset = 6 + 16 * $sizes.Count
    for ($i = 0; $i -lt $sizes.Count; $i++) {
        $dim = if ($sizes[$i] -ge 256) { 0 } else { $sizes[$i] }
        $w.Write([byte]$dim); $w.Write([byte]$dim); $w.Write([byte]0); $w.Write([byte]0)
        $w.Write([uint16]1); $w.Write([uint16]32)
        $w.Write([uint32]$images[$i].Length); $w.Write([uint32]$offset)
        $offset += $images[$i].Length
    }
    foreach ($img in $images) { $w.Write($img) }
    [IO.File]::WriteAllBytes($path, $out.ToArray())
}

$sizes = 16, 20, 24, 32, 40, 48, 64, 96, 256
$bitmaps = @{}; $idle = @{}
foreach ($s in $sizes) { $bitmaps[$s] = Draw-Logo $s; $idle[$s] = Draw-Logo $s -Idle }
Write-Ico $bitmaps $sizes (Join-Path $PSScriptRoot "CelMonitor.ico")
Write-Ico $idle $sizes (Join-Path $PSScriptRoot "CelMonitorIdle.ico")
$bitmaps[256].Save((Join-Path $PSScriptRoot "..\..\..\docs\logo.png"), [System.Drawing.Imaging.ImageFormat]::Png)
Write-Host "CelMonitor.ico, CelMonitorIdle.ico e docs\logo.png gerados"

# Android: the same geometry as VectorDrawables. The transparent gap around the phone becomes a shape filled
# with the background colour (the launcher icon's background, or the app's black status screen).
function RR([double]$x, [double]$y, [double]$w, [double]$h, [double]$r) {
    $f = { param($v) ([Math]::Round($v, 3)).ToString([Globalization.CultureInfo]::InvariantCulture) }
    if ($r -le 0) { return "M$(& $f $x),$(& $f $y)h$(& $f $w)v$(& $f $h)h$(& $f (-$w))z" }
    $a = "a$(& $f $r),$(& $f $r) 0 0 1"
    "M$(& $f ($x + $r)),$(& $f $y)h$(& $f ($w - 2 * $r))$a $(& $f $r),$(& $f $r)v$(& $f ($h - 2 * $r))$a $(& $f (-$r)),$(& $f $r)" +
    "h$(& $f (-($w - 2 * $r)))$a $(& $f (-$r)),$(& $f (-$r))v$(& $f (-($h - 2 * $r)))$a $(& $f $r),$(& $f (-$r))z"
}
function Android-Vector([double]$size, [double]$viewport, [double]$scale, [double]$tx, [double]$ty, [string]$gapColor) {
    $mon = RR 2 5.5 21 14.5 1
    $phs = RR 20.9 12.1 9.2 16.4 1
    $grad = { param($p) @"
        <path android:pathData="$p">
            <aapt:attr name="android:fillColor">
                <gradient android:type="linear" android:startX="4" android:startY="4" android:endX="20" android:endY="30"
                    android:startColor="#FF4DA3FF" android:endColor="#FF1D5FE0" />
            </aapt:attr>
        </path>
"@ }
    $window = @"
            <path android:fillColor="#FFFFFFFF" android:pathData="$(RR 8 13.5 19.5 8 0.8)" />
            <path android:fillColor="#FFFFB020" android:pathData="$(RR 8 13.5 19.5 2.2 0)" />
"@
    @"
<?xml version="1.0" encoding="utf-8"?>
<!-- Generated by windows/host/res/make-icon.ps1 (same logo as the Windows app). -->
<vector xmlns:android="http://schemas.android.com/apk/res/android"
    xmlns:aapt="http://schemas.android.com/aapt"
    android:width="$($size)dp" android:height="$($size)dp"
    android:viewportWidth="$viewport" android:viewportHeight="$viewport">
    <group android:scaleX="$scale" android:scaleY="$scale" android:translateX="$tx" android:translateY="$ty">
        <path android:fillColor="#FF3D4B63" android:pathData="$(RR 0.5 4 24 17.5 2.2)" />
$(& $grad $mon)
        <path android:fillColor="#FF3D4B63" android:pathData="$(RR 10.5 21 4 3.5 0)$(RR 6.5 24 12 2.5 1.2)" />
        <group>
            <clip-path android:pathData="$mon" />
$window
        </group>
        <path android:fillColor="$gapColor" android:pathData="$(RR 18.2 9.2 14.6 22.6 3.9)" />
        <path android:fillColor="#FF3D4B63" android:pathData="$(RR 19.5 10.5 12 20 2.6)" />
$(& $grad $phs)
        <group>
            <clip-path android:pathData="$phs" />
$window
        </group>
    </group>
</vector>
"@
}
$drawable = Join-Path $PSScriptRoot "..\..\..\android\app\src\main\res\drawable"
$utf8 = New-Object System.Text.UTF8Encoding $false
# Launcher foreground: 108-unit canvas, logo scaled 1.6x and centred inside the 66-unit safe circle.
[IO.File]::WriteAllText((Join-Path $drawable "ic_launcher_foreground.xml"), (Android-Vector 108 108 1.6 28.4 26.4 "#FFF1F5FB"), $utf8)
# Status screen logo on the app's black background.
[IO.File]::WriteAllText((Join-Path $drawable "logo.xml"), (Android-Vector 96 32 1 0 -1.5 "#FF000000"), $utf8)
Write-Host "Android: ic_launcher_foreground.xml e logo.xml gerados"

if ($Preview) {
    # Each size at 1:1 and magnified 4x (nearest neighbour), on a light and a dark taskbar colour.
    $sheet = New-Object System.Drawing.Bitmap 1500, 700
    $g = [System.Drawing.Graphics]::FromImage($sheet)
    $g.InterpolationMode = 'NearestNeighbor'
    $g.PixelOffsetMode = 'Half'
    foreach ($row in @(@{y = 0; c = '#F3F3F3' }, @{y = 350; c = '#202020' })) {
        $g.FillRectangle((New-Object System.Drawing.SolidBrush (Color $row.c)), 0, $row.y, 1500, 350)
        $x = 20
        foreach ($s in 16, 24, 32, 48) {
            $g.DrawImage($bitmaps[$s], $x, $row.y + 20, $s, $s)
            $g.DrawImage($idle[$s], $x + $s + 8, $row.y + 20, $s, $s)
            $g.DrawImage($bitmaps[$s], $x, $row.y + 90, $s * 4, $s * 4)
            $x += $s * 4 + 30
        }
        $g.DrawImage($bitmaps[256], $x + 20, $row.y + 40, 256, 256)
    }
    $g.Dispose()
    $sheet.Save($Preview, [System.Drawing.Imaging.ImageFormat]::Png)
    Write-Host "preview: $Preview"
}
