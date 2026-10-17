# The app's mark, shared by tools/make-assets.ps1 (package tiles) and tools/store/make-store-art.ps1
# (Store listing art): a TV with a phone shown on its screen, on a stand. Dot-source this file.

Add-Type -AssemblyName System.Drawing

function New-RoundedRect([double]$x, [double]$y, [double]$w, [double]$h, [double]$r) {
    $p = New-Object System.Drawing.Drawing2D.GraphicsPath
    $d = [float](2 * $r)
    $p.AddArc([float]$x, [float]$y, $d, $d, 180, 90)
    $p.AddArc([float]($x + $w - 2 * $r), [float]$y, $d, $d, 270, 90)
    $p.AddArc([float]($x + $w - 2 * $r), [float]($y + $h - 2 * $r), $d, $d, 0, 90)
    $p.AddArc([float]$x, [float]($y + $h - 2 * $r), $d, $d, 90, 90)
    $p.CloseFigure()
    return $p
}

# s: the glyph's width; (cx, cy): its center
function Add-AppGlyph($g, [double]$cx, [double]$cy, [double]$s, $ink, $accent) {
    $stroke = [Math]::Max(1.5, $s * 0.065)
    # TV
    $tvW = $s; $tvH = $s * 0.62
    $tvX = $cx - $tvW / 2; $tvY = $cy - $s * 0.40
    $pen = New-Object System.Drawing.Pen $ink, ([float]$stroke)
    $pen.LineJoin = [System.Drawing.Drawing2D.LineJoin]::Round
    $pen.StartCap = [System.Drawing.Drawing2D.LineCap]::Round
    $pen.EndCap = [System.Drawing.Drawing2D.LineCap]::Round
    $tv = New-RoundedRect $tvX $tvY $tvW $tvH ($s * 0.07)
    $g.DrawPath($pen, $tv)
    $tv.Dispose()
    # stand
    $standY = $tvY + $tvH + $s * 0.13
    $g.DrawLine($pen, [float]$cx, [float]($tvY + $tvH), [float]$cx, [float]$standY)
    $g.DrawLine($pen, [float]($cx - $s * 0.2), [float]$standY, [float]($cx + $s * 0.2), [float]$standY)
    $pen.Dispose()
    # the phone, shown on the TV's screen
    $phW = $s * 0.2; $phH = $tvH - $s * 0.16
    $phX = $cx - $phW / 2; $phY = $tvY + ($tvH - $phH) / 2
    $phone = New-RoundedRect $phX $phY $phW $phH ($s * 0.04)
    $brush = New-Object System.Drawing.SolidBrush $accent
    $g.FillPath($brush, $phone)
    $brush.Dispose()
    $phone.Dispose()
}
