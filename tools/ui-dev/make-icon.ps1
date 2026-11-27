# Renders the Somike app icon (gradient rounded square with a white microphone,
# same mark as the UI logo) at all Windows icon sizes and packs them into
# src/app/somike.ico (PNG-compressed entries).
#
#   powershell -ExecutionPolicy Bypass -File tools/ui-dev/make-icon.ps1
Add-Type -AssemblyName System.Drawing

$sizes = 16, 20, 24, 32, 40, 48, 64, 96, 128, 256
$output = Join-Path $PSScriptRoot '..\..\src\app\somike.ico'

function New-RoundedRect([float]$x, [float]$y, [float]$w, [float]$h, [float]$r) {
    $path = New-Object System.Drawing.Drawing2D.GraphicsPath
    $d = 2 * $r
    $path.AddArc($x, $y, $d, $d, 180, 90)
    $path.AddArc($x + $w - $d, $y, $d, $d, 270, 90)
    $path.AddArc($x + $w - $d, $y + $h - $d, $d, $d, 0, 90)
    $path.AddArc($x, $y + $h - $d, $d, $d, 90, 90)
    $path.CloseFigure()
    return $path
}

function New-IconPng([int]$size) {
    $bitmap = New-Object System.Drawing.Bitmap($size, $size, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bitmap)
    $g.SmoothingMode = 'AntiAlias'
    $g.PixelOffsetMode = 'HighQuality'
    $g.Clear([System.Drawing.Color]::Transparent)

    $s = $size / 32.0  # design grid: 32 x 32, like the SVG logo
    $background = New-RoundedRect 0 0 $size $size (9 * $s)
    $gradient = New-Object System.Drawing.Drawing2D.LinearGradientBrush(
        (New-Object System.Drawing.PointF(0, 0)), (New-Object System.Drawing.PointF($size, $size)),
        [System.Drawing.Color]::FromArgb(255, 0x8b, 0x5c, 0xf6), [System.Drawing.Color]::FromArgb(255, 0xec, 0x48, 0x99))
    $g.FillPath($gradient, $background)

    $white = [System.Drawing.Brushes]::White
    $capsule = New-RoundedRect (12.5 * $s) (7 * $s) (7 * $s) (12 * $s) (3.5 * $s)
    $g.FillPath($white, $capsule)
    $pen = New-Object System.Drawing.Pen([System.Drawing.Color]::White, [Math]::Max(1.0, 2 * $s))
    $pen.StartCap = 'Round'; $pen.EndCap = 'Round'
    $g.DrawArc($pen, (9.5 * $s), (9 * $s), (13 * $s), (13 * $s), 0, 180)
    $g.DrawLine($pen, (16 * $s), (22 * $s), (16 * $s), (25.5 * $s))

    $stream = New-Object System.IO.MemoryStream
    $bitmap.Save($stream, [System.Drawing.Imaging.ImageFormat]::Png)
    $g.Dispose(); $bitmap.Dispose()
    return , $stream.ToArray()
}

$images = foreach ($size in $sizes) { , (New-IconPng $size) }

$file = New-Object System.IO.MemoryStream
$writer = New-Object System.IO.BinaryWriter($file)
$writer.Write([UInt16]0); $writer.Write([UInt16]1); $writer.Write([UInt16]$sizes.Count)
$offset = 6 + 16 * $sizes.Count
for ($i = 0; $i -lt $sizes.Count; $i++) {
    $dim = if ($sizes[$i] -ge 256) { 0 } else { $sizes[$i] }
    $writer.Write([byte]$dim); $writer.Write([byte]$dim)
    $writer.Write([byte]0); $writer.Write([byte]0)
    $writer.Write([UInt16]1); $writer.Write([UInt16]32)
    $writer.Write([UInt32]$images[$i].Length); $writer.Write([UInt32]$offset)
    $offset += $images[$i].Length
}
foreach ($image in $images) { $writer.Write($image) }
$writer.Flush()
[System.IO.File]::WriteAllBytes((Resolve-Path (Split-Path $output)).Path + '\somike.ico', $file.ToArray())
Write-Output "Wrote $output ($($file.Length) bytes)"
