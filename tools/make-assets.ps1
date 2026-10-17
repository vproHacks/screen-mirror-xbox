# Generates the app's tile and splash PNGs (a simple "screen + triangle" AirPlay-style glyph).
param([string]$OutDir = "$PSScriptRoot\..\src\app\Assets")

Add-Type -AssemblyName System.Drawing
New-Item -ItemType Directory -Force $OutDir | Out-Null

function New-Image([string]$name, [int]$w, [int]$h, [double]$glyphScale, [bool]$transparent) {
    $bmp = New-Object System.Drawing.Bitmap $w, $h
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    if ($transparent) { $g.Clear([System.Drawing.Color]::Transparent) }
    else { $g.Clear([System.Drawing.Color]::FromArgb(255, 16, 18, 24)) }

    $s = [Math]::Min($w, $h) * $glyphScale
    $cx = $w / 2.0; $cy = $h / 2.0
    $pen = New-Object System.Drawing.Pen ([System.Drawing.Color]::FromArgb(255, 235, 238, 245)), ([Math]::Max(1.5, $s * 0.07))
    $pen.LineJoin = [System.Drawing.Drawing2D.LineJoin]::Round
    # screen outline (open at the bottom middle)
    $left = $cx - $s * 0.5; $right = $cx + $s * 0.5; $top = $cy - $s * 0.42; $bottom = $cy + $s * 0.18
    $pts = @(
        (New-Object System.Drawing.PointF ($cx - $s * 0.22), $bottom),
        (New-Object System.Drawing.PointF $left, $bottom),
        (New-Object System.Drawing.PointF $left, $top),
        (New-Object System.Drawing.PointF $right, $top),
        (New-Object System.Drawing.PointF $right, $bottom),
        (New-Object System.Drawing.PointF ($cx + $s * 0.22), $bottom))
    $g.DrawLines($pen, $pts)
    # triangle
    $brush = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(255, 90, 170, 255))
    $tri = @(
        (New-Object System.Drawing.PointF $cx, ($cy - $s * 0.02)),
        (New-Object System.Drawing.PointF ($cx + $s * 0.24), ($cy + $s * 0.42)),
        (New-Object System.Drawing.PointF ($cx - $s * 0.24), ($cy + $s * 0.42)))
    $g.FillPolygon($brush, $tri)
    $g.Dispose()
    $bmp.Save((Join-Path $OutDir $name), [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
}

New-Image "Square44x44Logo.png" 44 44 0.8 $true
New-Image "Square44x44Logo.targetsize-44_altform-unplated.png" 44 44 0.8 $true
New-Image "Square150x150Logo.png" 150 150 0.5 $false
New-Image "Wide310x150Logo.png" 310 150 0.5 $false
New-Image "StoreLogo.png" 50 50 0.75 $false
New-Image "SplashScreen.png" 620 300 0.45 $false
Get-ChildItem $OutDir | Select-Object Name, Length
