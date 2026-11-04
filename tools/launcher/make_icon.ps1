# Draw tools/launcher/icon.ico: an orange hexagon with a "2" on a dark-blue tile, inside a ring of green "binary"
# dashes, in the launcher's colours. Sizes 256, 48, 32 and 16, stored as PNG (Windows Vista and later).
# Usage: powershell -File tools\launcher\make_icon.ps1
Add-Type -AssemblyName System.Drawing
$out = Join-Path $PSScriptRoot 'icon.ico'

function Draw([int]$size) {
    $bmp = New-Object Drawing.Bitmap $size, $size
    $g = [Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = 'AntiAlias'; $g.TextRenderingHint = 'AntiAliasGridFit'
    $s = $size / 256.0
    # the tile: a rounded square, dark blue in the middle
    $tile = New-Object Drawing.Drawing2D.GraphicsPath
    $r = 56 * $s; $e = 8 * $s; $w = $size - 2 * $e
    $tile.AddArc($e, $e, $r, $r, 180, 90); $tile.AddArc($e + $w - $r, $e, $r, $r, 270, 90)
    $tile.AddArc($e + $w - $r, $e + $w - $r, $r, $r, 0, 90); $tile.AddArc($e, $e + $w - $r, $r, $r, 90, 90)
    $tile.CloseFigure()
    $fill = New-Object Drawing.Drawing2D.PathGradientBrush $tile
    $fill.CenterColor = [Drawing.Color]::FromArgb(255, 16, 40, 96)
    $fill.SurroundColors = @([Drawing.Color]::FromArgb(255, 4, 8, 22))
    $g.FillPath($fill, $tile)
    $g.DrawPath((New-Object Drawing.Pen ([Drawing.Color]::FromArgb(255, 56, 200, 255)), ([math]::Max(1, 4 * $s))), $tile)
    # the ring of dashes
    if ($size -ge 32) {
        $ring = New-Object Drawing.Pen ([Drawing.Color]::FromArgb(200, 43, 224, 122)), (14 * $s)
        $ring.DashPattern = [single[]](0.5, 0.9, 1.6, 0.6, 0.5, 1.8)
        $g.DrawEllipse($ring, 40 * $s, 40 * $s, 176 * $s, 176 * $s)
    }
    # the hexagon
    $c = 128 * $s; $h = 74 * $s
    $points = foreach ($k in 0..5) {
        $a = [math]::PI / 3 * $k
        New-Object Drawing.PointF ([single]($c + $h * [math]::Cos($a))), ([single]($c + $h * [math]::Sin($a)))
    }
    $orange = New-Object Drawing.Drawing2D.LinearGradientBrush (New-Object Drawing.PointF 0, ($c - $h)),
        (New-Object Drawing.PointF 0, ($c + $h)), ([Drawing.Color]::FromArgb(255, 255, 228, 92)), ([Drawing.Color]::FromArgb(255, 232, 51, 27))
    $g.FillPolygon($orange, [Drawing.PointF[]]$points)
    $g.DrawPolygon((New-Object Drawing.Pen ([Drawing.Color]::FromArgb(255, 255, 242, 160)), ([math]::Max(1, 5 * $s))), [Drawing.PointF[]]$points)
    # the "2"
    $font = New-Object Drawing.Font 'Segoe UI Black', ([single](96 * $s)), ([Drawing.FontStyle]::Italic), ([Drawing.GraphicsUnit]::Pixel)
    $format = New-Object Drawing.StringFormat; $format.Alignment = 'Center'; $format.LineAlignment = 'Center'
    $box = New-Object Drawing.RectangleF ([single](0)), ([single](6 * $s)), ([single]$size), ([single]$size)
    $g.DrawString('2', $font, (New-Object Drawing.SolidBrush ([Drawing.Color]::FromArgb(255, 42, 10, 0))), $box, $format)
    $g.Dispose()
    $stream = New-Object IO.MemoryStream
    $bmp.Save($stream, [Drawing.Imaging.ImageFormat]::Png); $bmp.Dispose()
    return , $stream.ToArray()
}

$sizes = 256, 48, 32, 16
$images = foreach ($size in $sizes) { , (Draw $size) }
$file = New-Object IO.MemoryStream
$writer = New-Object IO.BinaryWriter $file
$writer.Write([uint16]0); $writer.Write([uint16]1); $writer.Write([uint16]$sizes.Count)  # ICONDIR
$offset = 6 + 16 * $sizes.Count
for ($i = 0; $i -lt $sizes.Count; $i++) {                                                 # ICONDIRENTRY
    $dim = if ($sizes[$i] -ge 256) { 0 } else { $sizes[$i] }
    $writer.Write([byte]$dim); $writer.Write([byte]$dim); $writer.Write([byte]0); $writer.Write([byte]0)
    $writer.Write([uint16]1); $writer.Write([uint16]32)
    $writer.Write([uint32]$images[$i].Length); $writer.Write([uint32]$offset)
    $offset += $images[$i].Length
}
foreach ($image in $images) { $writer.Write($image) }
[IO.File]::WriteAllBytes($out, $file.ToArray())
"wrote $out ($($file.Length) bytes)"
