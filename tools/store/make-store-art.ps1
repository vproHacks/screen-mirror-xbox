# Generates the Microsoft Store listing art (Partner Center > Store listings) from the app's glyph
# (tools/glyph.ps1, as in the app tiles) into docs/store/art:
#   poster-1440x2160.png               9:16 Poster art (main logo on Windows and Xbox), with the title
#   box-2160x2160.png                  1:1 Box art, with the title
#   super-hero-3840x2160.png           16:9 Super hero art (must not include the title)
#   branded-key-art-584x800.png        Xbox Branded key art (title in the top 3/4)
#   titled-hero-1920x1080.png          Xbox Titled hero art (title in the top 3/4)
#   featured-square-1080x1080.png      Xbox Featured promotional square art (no title)
#   tile-300.png, tile-150.png, tile-71.png   Store display images for Windows (optional)
# Usage (from Git Bash): powershell -NoProfile -ExecutionPolicy Bypass -File tools/store/make-store-art.ps1
param([string]$OutDir = (Join-Path $PSScriptRoot "..\..\docs\store\art"))
$ErrorActionPreference = "Stop"
. "$PSScriptRoot\..\glyph.ps1"
New-Item -ItemType Directory -Force $OutDir | Out-Null

$Title = "Screen Mirror Server"
$Tagline = "Your phone on the big screen"
$Ink = [System.Drawing.Color]::FromArgb(255, 235, 238, 245)
$Muted = [System.Drawing.Color]::FromArgb(255, 150, 160, 180)
$Blue = [System.Drawing.Color]::FromArgb(255, 90, 170, 255)

function New-Canvas([int]$w, [int]$h) {
    $bmp = New-Object System.Drawing.Bitmap $w, $h
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $g.TextRenderingHint = [System.Drawing.Text.TextRenderingHint]::AntiAliasGridFit
    $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    # the app's background (#101218), a little lighter towards the top
    $rect = New-Object System.Drawing.Rectangle 0, 0, $w, $h
    $bg = New-Object System.Drawing.Drawing2D.LinearGradientBrush $rect, ([System.Drawing.Color]::FromArgb(255, 26, 30, 42)),
        ([System.Drawing.Color]::FromArgb(255, 12, 14, 20)), 90.0
    $g.FillRectangle($bg, $rect)
    $bg.Dispose()
    return @($bmp, $g)
}

# soft blue light behind the glyph
function Add-Glow($g, [double]$cx, [double]$cy, [double]$r) {
    $path = New-Object System.Drawing.Drawing2D.GraphicsPath
    $path.AddEllipse([float]($cx - $r), [float]($cy - $r), [float](2 * $r), [float](2 * $r))
    $glow = New-Object System.Drawing.Drawing2D.PathGradientBrush $path
    $glow.CenterColor = [System.Drawing.Color]::FromArgb(110, 70, 140, 255)
    $glow.SurroundColors = @([System.Drawing.Color]::FromArgb(0, 70, 140, 255))
    $g.FillPath($glow, $path)
    $glow.Dispose()
    $path.Dispose()
}

# the app mark (tools/glyph.ps1); s = its width
function Add-Glyph($g, [double]$cx, [double]$cy, [double]$s) { Add-AppGlyph $g $cx $cy $s $Ink $Blue }

# centered text in a band; returns nothing
function Add-Text($g, [string]$text, [double]$cx, [double]$cy, [double]$size, $color, [string]$family, [double]$width) {
    $font = New-Object System.Drawing.Font $family, ([float]$size), ([System.Drawing.FontStyle]::Regular), ([System.Drawing.GraphicsUnit]::Pixel)
    $fmt = New-Object System.Drawing.StringFormat
    $fmt.Alignment = [System.Drawing.StringAlignment]::Center
    $fmt.LineAlignment = [System.Drawing.StringAlignment]::Center
    $brush = New-Object System.Drawing.SolidBrush $color
    $rect = New-Object System.Drawing.RectangleF ([float]($cx - $width / 2)), ([float]($cy - $size * 1.5)), ([float]$width), ([float]($size * 3))
    $g.DrawString($text, $font, $brush, $rect, $fmt)
    $brush.Dispose(); $fmt.Dispose(); $font.Dispose()
}

function Save($pair, [string]$name) {
    $bmp = $pair[0]; $g = $pair[1]
    $g.Dispose()
    $bmp.Save((Join-Path $OutDir $name), [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
    Write-Output $name
}

# 9:16 poster: glyph in the upper half, title and tagline below it
$c = New-Canvas 1440 2160; $g = $c[1]
Add-Glow $g 720 820 620
Add-Glyph $g 720 820 620
Add-Text $g $Title 720 1400 124 $Ink "Segoe UI Semibold" 1300
Add-Text $g $Tagline 720 1560 64 $Muted "Segoe UI" 1300
Save $c "poster-1440x2160.png"

# 1:1 box art: glyph and title
$c = New-Canvas 2160 2160; $g = $c[1]
Add-Glow $g 1080 900 760
Add-Glyph $g 1080 900 760
Add-Text $g $Title 1080 1590 170 $Ink "Segoe UI Semibold" 2000
Save $c "box-2160x2160.png"

# 16:9 super hero: no title; the glyph to the right leaves room for the Store's overlay on the left
$c = New-Canvas 3840 2160; $g = $c[1]
Add-Glow $g 2600 1080 1300
Add-Glyph $g 2600 1080 1100
Save $c "super-hero-3840x2160.png"

# Xbox branded key art (584 x 800): title in the top 3/4
$c = New-Canvas 584 800; $g = $c[1]
Add-Glow $g 292 250 230
Add-Glyph $g 292 250 230
Add-Text $g "Screen Mirror" 292 470 62 $Ink "Segoe UI Semibold" 560
Add-Text $g "Server" 292 540 62 $Ink "Segoe UI Semibold" 560
Save $c "branded-key-art-584x800.png"

# Xbox titled hero (1920 x 1080): glyph left, title right, all in the top 3/4
$c = New-Canvas 1920 1080; $g = $c[1]
Add-Glow $g 430 400 440
Add-Glyph $g 430 400 380
Add-Text $g $Title 1220 360 100 $Ink "Segoe UI Semibold" 1200
Add-Text $g $Tagline 1220 480 46 $Muted "Segoe UI" 1200
Save $c "titled-hero-1920x1080.png"

# Xbox featured promotional square (1080 x 1080): no title
$c = New-Canvas 1080 1080; $g = $c[1]
Add-Glow $g 540 540 480
Add-Glyph $g 540 540 520
Save $c "featured-square-1080x1080.png"

# Windows Store display images (optional; the package logos are used otherwise)
foreach ($size in 300, 150, 71) {
    $c = New-Canvas $size $size; $g = $c[1]
    Add-Glyph $g ($size / 2) ($size / 2) ($size * 0.62)
    Save $c "tile-$size.png"
}
