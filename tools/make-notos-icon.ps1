# Draws the Notos icon (Notos: the south wind) into D:\Notos\fritchii\dist\notos.ico and notos.png:
# three white wind strokes curling on a blue rounded square, the launcher's default accent.
param([string]$Out = "D:\Notos\fritchii\dist")
Add-Type -AssemblyName System.Drawing

function Draw([int]$size) {
    $bmp = New-Object System.Drawing.Bitmap $size, $size, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = 'AntiAlias'; $g.PixelOffsetMode = 'HighQuality'
    $g.ScaleTransform($size / 256.0, $size / 256.0)

    # Rounded square with a top-to-bottom blue gradient.
    $r = 56; $rect = New-Object System.Drawing.RectangleF 8, 8, 240, 240
    $bg = New-Object System.Drawing.Drawing2D.GraphicsPath
    $bg.AddArc($rect.X, $rect.Y, $r, $r, 180, 90)
    $bg.AddArc($rect.Right - $r, $rect.Y, $r, $r, 270, 90)
    $bg.AddArc($rect.Right - $r, $rect.Bottom - $r, $r, $r, 0, 90)
    $bg.AddArc($rect.X, $rect.Bottom - $r, $r, $r, 90, 90)
    $bg.CloseFigure()
    $fill = New-Object System.Drawing.Drawing2D.LinearGradientBrush $rect, ([System.Drawing.Color]::FromArgb(255, 0x4F, 0xB0, 0xFF)), ([System.Drawing.Color]::FromArgb(255, 0x14, 0x5C, 0xC8)), 90.0
    $g.FillPath($fill, $bg)

    # Three wind strokes, each ending in a curl.
    $pen = New-Object System.Drawing.Pen ([System.Drawing.Color]::White), 18
    $pen.StartCap = 'Round'; $pen.EndCap = 'Round'; $pen.LineJoin = 'Round'
    $strokes = @(
        @{ y = 86;  x0 = 48; x1 = 168; c = 30 },
        @{ y = 132; x0 = 48; x1 = 196; c = 26 },
        @{ y = 178; x0 = 70; x1 = 150; c = 22 })
    foreach ($s in $strokes) {
        $p = New-Object System.Drawing.Drawing2D.GraphicsPath
        $p.AddLine([single]$s.x0, [single]$s.y, [single]$s.x1, [single]$s.y)
        # Curl up and back over the line's end.
        $p.AddArc([single]($s.x1 - $s.c), [single]($s.y - 2 * $s.c), [single](2 * $s.c), [single](2 * $s.c), 90, -270)
        $g.DrawPath($pen, $p)
    }
    $g.Dispose()
    return $bmp
}

$sizes = 16, 24, 32, 48, 64, 128, 256
$pngs = foreach ($s in $sizes) {
    $b = Draw $s; $ms = New-Object IO.MemoryStream
    $b.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
    if ($s -eq 256) { $b.Save((Join-Path $Out "notos.png"), [System.Drawing.Imaging.ImageFormat]::Png) }
    $b.Dispose(); , $ms.ToArray()
}

# ICO with PNG entries.
$ico = New-Object IO.MemoryStream; $w = New-Object IO.BinaryWriter $ico
$w.Write([uint16]0); $w.Write([uint16]1); $w.Write([uint16]$sizes.Count)
$offset = 6 + 16 * $sizes.Count
for ($i = 0; $i -lt $sizes.Count; $i++) {
    $d = if ($sizes[$i] -ge 256) { 0 } else { $sizes[$i] }
    $w.Write([byte]$d); $w.Write([byte]$d); $w.Write([byte]0); $w.Write([byte]0)
    $w.Write([uint16]1); $w.Write([uint16]32); $w.Write([uint32]$pngs[$i].Length); $w.Write([uint32]$offset)
    $offset += $pngs[$i].Length
}
foreach ($p in $pngs) { $w.Write($p) }
$w.Flush()
[IO.File]::WriteAllBytes((Join-Path $Out "notos.ico"), $ico.ToArray())
"notos.ico: $($ico.Length) bytes"
