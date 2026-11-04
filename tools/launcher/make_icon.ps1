# Draw tools/launcher/icon.ico: "DRA2" in yellow-to-orange with a red rim and dark outline, on deep blue with light
# streaks bursting from the middle. Original art in the cover's spirit: no logo or wordmark from the game. Sizes 256,
# 48, 32 and 16, stored as PNG (Windows Vista and later); at 16 only the "2" on blue, so it stays readable.
# Usage: powershell -File tools\launcher\make_icon.ps1
#        powershell -File tools\launcher\make_icon.ps1 -Image <picture>
#   -Image: make tools/launcher/cover.ico from a picture of your own instead (any format Windows can open: png, jpg,
#   webp...; it's cropped to a square). cover.ico stays out of the repository; when it's there, build.ps1 and the
#   launcher window use it instead of icon.ico.
param([string]$Image)
Add-Type -AssemblyName System.Drawing, PresentationCore
$out = Join-Path $PSScriptRoot $(if ($Image) { 'cover.ico' } else { 'icon.ico' })

if ($Image) {
    # Windows Imaging Component opens more formats (webp, heic...) than System.Drawing; hand System.Drawing a PNG
    $decoder = [Windows.Media.Imaging.BitmapDecoder]::Create([Uri](Resolve-Path $Image).Path, 'None', 'OnLoad')
    $png = New-Object Windows.Media.Imaging.PngBitmapEncoder
    $png.Frames.Add([Windows.Media.Imaging.BitmapFrame]::Create($decoder.Frames[0]))
    $buffer = New-Object IO.MemoryStream
    $png.Save($buffer)
    $script:picture = [Drawing.Image]::FromStream($buffer)
}

function Draw-Picture([int]$size) {
    $bmp = New-Object Drawing.Bitmap $size, $size
    $g = [Drawing.Graphics]::FromImage($bmp)
    $g.InterpolationMode = 'HighQualityBicubic'; $g.PixelOffsetMode = 'HighQuality'; $g.SmoothingMode = 'AntiAlias'
    $side = [math]::Min($picture.Width, $picture.Height)  # the middle square of the picture
    $from = New-Object Drawing.Rectangle ([int](($picture.Width - $side) / 2)), ([int](($picture.Height - $side) / 2)), $side, $side
    $g.DrawImage($picture, (New-Object Drawing.Rectangle 0, 0, $size, $size), $from, 'Pixel')
    $g.Dispose()
    $stream = New-Object IO.MemoryStream
    $bmp.Save($stream, [Drawing.Imaging.ImageFormat]::Png); $bmp.Dispose()
    return , $stream.ToArray()
}

function Color([int]$a, [int]$r, [int]$g, [int]$b) { [Drawing.Color]::FromArgb($a, $r, $g, $b) }
function Point([double]$x, [double]$y) { New-Object Drawing.PointF ([single]$x), ([single]$y) }

function Draw([int]$size) {
    if ($Image) { return , (Draw-Picture $size) }
    $bmp = New-Object Drawing.Bitmap $size, $size
    $g = [Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = 'AntiAlias'; $g.PixelOffsetMode = 'HighQuality'
    $s = $size / 256.0
    # the tile: a rounded square
    $tile = New-Object Drawing.Drawing2D.GraphicsPath
    $r = 60 * $s; $e = [math]::Max(0.5, 6 * $s); $w = $size - 2 * $e
    $tile.AddArc($e, $e, $r, $r, 180, 90); $tile.AddArc($e + $w - $r, $e, $r, $r, 270, 90)
    $tile.AddArc($e + $w - $r, $e + $w - $r, $r, $r, 0, 90); $tile.AddArc($e, $e + $w - $r, $r, $r, 90, 90)
    $tile.CloseFigure()
    $g.SetClip($tile)
    # deep blue, brightening toward the middle
    $bg = New-Object Drawing.Drawing2D.PathGradientBrush $tile
    $bg.CenterColor = Color 255 40 92 210
    $bg.SurroundColors = @(Color 255 6 14 70)
    $g.FillPath($bg, $tile)
    # light streaks bursting out from the middle
    $c = 128 * $s
    if ($size -gt 16) {
        $rng = New-Object Random 7  # the same streaks every time
        $count = if ($size -ge 64) { 150 } else { 40 }
        for ($k = 0; $k -lt $count; $k++) {
            $a = $rng.NextDouble() * 2 * [math]::PI
            $from = (20 + $rng.NextDouble() * 50) * $s; $to = (120 + $rng.NextDouble() * 80) * $s
            $alpha = [int](40 + $rng.NextDouble() * 110)
            $streak = New-Object Drawing.Pen (Color $alpha 170 215 255), ([single]([math]::Max(0.8, (0.6 + $rng.NextDouble() * 2.2) * $s)))
            $g.DrawLine($streak, (Point ($c + $from * [math]::Cos($a)) ($c + $from * [math]::Sin($a))),
                (Point ($c + $to * [math]::Cos($a)) ($c + $to * [math]::Sin($a))))
        }
        # a white glow at the centre
        $core = New-Object Drawing.Drawing2D.GraphicsPath
        $core.AddEllipse([single]($c - 70 * $s), [single]($c - 70 * $s), [single](140 * $s), [single](140 * $s))
        $glow = New-Object Drawing.Drawing2D.PathGradientBrush $core
        $glow.CenterColor = Color 210 235 245 255
        $glow.SurroundColors = @(Color 0 120 180 255)
        $g.FillPath($glow, $core)
    }
    $g.ResetClip()
    # the text, leaning forward, yellow to orange, with a red rim inside a dark outline: "DRA" and a bigger "2", sitting on one baseline and centred; at 16 px only the "2" (four letters don't fit)
    $family = New-Object Drawing.FontFamily 'Arial Black'
    $italic = [int][Drawing.FontStyle]::Italic
    $origin = New-Object Drawing.PointF 0, 0
    $two = New-Object Drawing.Drawing2D.GraphicsPath
    $text = New-Object Drawing.Drawing2D.GraphicsPath
    if ($size -le 16) {
        $two.AddString('2', $family, $italic, [single](250 * $s), $origin, [Drawing.StringFormat]::GenericTypographic)
        $text.AddPath($two, $false)
    } else {
        $dra = New-Object Drawing.Drawing2D.GraphicsPath
        $dra.AddString('DRA', $family, $italic, [single](92 * $s), $origin, [Drawing.StringFormat]::GenericTypographic)
        $two.AddString('2', $family, $italic, [single](190 * $s), $origin, [Drawing.StringFormat]::GenericTypographic)
        $db = $dra.GetBounds(); $tb = $two.GetBounds()
        $place = New-Object Drawing.Drawing2D.Matrix  # "2" just after "DRA", bottoms level
        $place.Translate([single]($db.Right - $tb.Left - 4 * $s), [single]($db.Bottom - $tb.Bottom))
        $two.Transform($place)
        $text.AddPath($dra, $false); $text.AddPath($two, $false)
    }
    $tb = $text.GetBounds()
    $fit = [math]::Min(1.0, (212 * $s) / $tb.Width)  # leave room for the outline inside the tile
    $shrink = New-Object Drawing.Drawing2D.Matrix; $shrink.Scale([single]$fit, [single]$fit)
    $text.Transform($shrink); $tb = $text.GetBounds()
    $centre = New-Object Drawing.Drawing2D.Matrix
    $centre.Translate([single](($size - $tb.Width) / 2 - $tb.Left), [single](($size - $tb.Height) / 2 - $tb.Top + 2 * $s))
    $text.Transform($centre)
    if ($size -gt 16) {  # a drop shadow
        $shadow = $text.Clone()
        $shift = New-Object Drawing.Drawing2D.Matrix; $shift.Translate([single](7 * $s), [single](8 * $s))
        $shadow.Transform($shift)
        $g.FillPath((New-Object Drawing.SolidBrush (Color 150 0 0 20)), $shadow)
    }
    $dark = New-Object Drawing.Pen (Color 255 20 10 40), $(if ($size -le 16) { 1.6 } else { [math]::Max(1.4, 20 * $s) })
    $dark.LineJoin = 'Round'
    $g.DrawPath($dark, $text)
    if ($size -gt 16) {
        $rim = New-Object Drawing.Pen (Color 255 214 30 22), ([math]::Max(1, 9 * $s))
        $rim.LineJoin = 'Round'
        $g.DrawPath($rim, $text)
    }
    $bounds = $text.GetBounds()
    $fill = New-Object Drawing.Drawing2D.LinearGradientBrush (Point 0 $bounds.Top), (Point 0 $bounds.Bottom), (Color 255 255 240 80), (Color 255 240 120 10)
    $blend = New-Object Drawing.Drawing2D.ColorBlend 3
    $blend.Colors = @((Color 255 255 244 96), (Color 255 255 206 34), (Color 255 242 122 12))
    $blend.Positions = [single[]](0, 0.45, 1)
    $fill.InterpolationColors = $blend
    $g.FillPath($fill, $text)
    if ($size -ge 48) {  # a bright highlight along the top edge
        $g.SetClip($text)
        $g.FillRectangle((New-Object Drawing.SolidBrush (Color 90 255 255 230)), 0, $bounds.Top, $size, $bounds.Height * 0.22)
        $g.ResetClip()
    }
    # the tile's rim
    $g.DrawPath((New-Object Drawing.Pen (Color 255 120 180 255), ([math]::Max(1, 4 * $s))), $tile)
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
foreach ($entry in $images) { $writer.Write($entry) }  # not $image: that's -Image, a string
[IO.File]::WriteAllBytes($out, $file.ToArray())
"wrote $out ($($file.Length) bytes)"
