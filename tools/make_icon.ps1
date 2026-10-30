# Generates studio/MixedMedia.ico from vector shapes (GDI+).
# Run from the repository root: powershell -ExecutionPolicy Bypass -File tools\make_icon.ps1
$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Drawing

$root = Split-Path -Parent $PSScriptRoot
$outDir = Join-Path $root "studio"
New-Item -ItemType Directory -Force $outDir | Out-Null
$outPath = Join-Path $outDir "MixedMedia.ico"

function New-RoundRectPath([float]$x, [float]$y, [float]$w, [float]$h, [float]$r) {
    $p = New-Object System.Drawing.Drawing2D.GraphicsPath
    $d = $r * 2
    $p.AddArc($x, $y, $d, $d, 180, 90)
    $p.AddArc($x + $w - $d, $y, $d, $d, 270, 90)
    $p.AddArc($x + $w - $d, $y + $h - $d, $d, $d, 0, 90)
    $p.AddArc($x, $y + $h - $d, $d, $d, 90, 90)
    $p.CloseFigure()
    return $p
}

function Add-PaperPiece($g, [float]$scale, [float]$x, [float]$y, [float]$w, [float]$h, [float]$angle, [System.Drawing.Color]$color) {
    $state = $g.Save()
    $cx = $x + $w / 2
    $cy = $y + $h / 2
    $g.TranslateTransform($cx, $cy)
    $g.RotateTransform($angle)
    $g.TranslateTransform(-$cx, -$cy)

    $pts = New-Object 'System.Drawing.PointF[]' 12
    $j = [Math]::Max(1.0, 1.8 * $scale)
    $pts[0]  = New-Object System.Drawing.PointF(($x + $j * 0.2), ($y + $j * 0.8))
    $pts[1]  = New-Object System.Drawing.PointF(($x + $w * 0.18), ($y - $j * 0.2))
    $pts[2]  = New-Object System.Drawing.PointF(($x + $w * 0.42), ($y + $j * 0.3))
    $pts[3]  = New-Object System.Drawing.PointF(($x + $w * 0.66), ($y - $j * 0.3))
    $pts[4]  = New-Object System.Drawing.PointF(($x + $w * 0.88), ($y + $j * 0.2))
    $pts[5]  = New-Object System.Drawing.PointF(($x + $w + $j * 0.1), ($y + $h * 0.22))
    $pts[6]  = New-Object System.Drawing.PointF(($x + $w - $j * 0.2), ($y + $h * 0.52))
    $pts[7]  = New-Object System.Drawing.PointF(($x + $w + $j * 0.2), ($y + $h * 0.78))
    $pts[8]  = New-Object System.Drawing.PointF(($x + $w * 0.76), ($y + $h + $j * 0.1))
    $pts[9]  = New-Object System.Drawing.PointF(($x + $w * 0.52), ($y + $h - $j * 0.25))
    $pts[10] = New-Object System.Drawing.PointF(($x + $w * 0.26), ($y + $h + $j * 0.2))
    $pts[11] = New-Object System.Drawing.PointF(($x - $j * 0.2), ($y + $h * 0.62))

    $brush = New-Object System.Drawing.SolidBrush($color)
    $g.FillPolygon($brush, $pts)
    $brush.Dispose()
    $g.Restore($state)
}

function New-IconPng([int]$size) {
    $bmp = New-Object System.Drawing.Bitmap($size, $size)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
    $g.Clear([System.Drawing.Color]::Transparent)

    $s = $size / 256.0
    $bgPath = New-RoundRectPath (10 * $s) (10 * $s) (236 * $s) (236 * $s) (48 * $s)
    $bgBrush = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(255, 31, 27, 22))
    $g.FillPath($bgBrush, $bgPath)
    $bgBrush.Dispose()
    $bgPath.Dispose()

    # Warm paper scraps
    Add-PaperPiece $g $s (46 * $s) (54 * $s) (102 * $s) (112 * $s) -9 ([System.Drawing.Color]::FromArgb(255, 247, 239, 220))
    Add-PaperPiece $g $s (118 * $s) (72 * $s) (94 * $s) (126 * $s) 8 ([System.Drawing.Color]::FromArgb(255, 228, 87, 46))
    Add-PaperPiece $g $s (78 * $s) (128 * $s) (118 * $s) (76 * $s) 3 ([System.Drawing.Color]::FromArgb(255, 45, 156, 148))

    # Halftone dots on the top paper
    $dotBrush = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(210, 31, 27, 22))
    $dotR = [Math]::Max(0.8, 2.6 * $s)
    for ($row = 0; $row -lt 3; $row++) {
        for ($col = 0; $col -lt 4; $col++) {
            $dx = (60 + $col * 17) * $s
            $dy = (70 + $row * 17) * $s
            $g.FillEllipse($dotBrush, $dx, $dy, $dotR * 2, $dotR * 2)
        }
    }
    $dotBrush.Dispose()

    # Accent notch / registration mark
    $pen = New-Object System.Drawing.Pen([System.Drawing.Color]::FromArgb(255, 255, 190, 80), [Math]::Max(1.0, 5 * $s))
    $g.DrawLine($pen, 184 * $s, 192 * $s, 208 * $s, 192 * $s)
    $g.DrawLine($pen, 196 * $s, 180 * $s, 196 * $s, 204 * $s)
    $pen.Dispose()

    $ms = New-Object System.IO.MemoryStream
    $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
    $bytes = $ms.ToArray()
    $ms.Dispose()
    $g.Dispose()
    $bmp.Dispose()
    return $bytes
}

$sizes = @(16, 24, 32, 48, 64, 128, 256)
$pngs = New-Object 'System.Collections.Generic.List[byte[]]'
foreach ($size in $sizes) {
    $pngs.Add([byte[]](New-IconPng $size))
}

$fs = [System.IO.File]::Create($outPath)
$bw = New-Object System.IO.BinaryWriter($fs)
$bw.Write([UInt16]0)              # reserved
$bw.Write([UInt16]1)              # type: icon
$bw.Write([UInt16]$sizes.Count)   # count
$offset = 6 + (16 * $sizes.Count)
for ($i = 0; $i -lt $sizes.Count; $i++) {
    $size = $sizes[$i]
    $data = $pngs[$i]
    $bw.Write([Byte]($(if ($size -ge 256) { 0 } else { $size })))
    $bw.Write([Byte]($(if ($size -ge 256) { 0 } else { $size })))
    $bw.Write([Byte]0)
    $bw.Write([Byte]0)
    $bw.Write([UInt16]1)
    $bw.Write([UInt16]32)
    $bw.Write([UInt32]$data.Length)
    $bw.Write([UInt32]$offset)
    $offset += $data.Length
}
foreach ($data in $pngs) { $bw.Write([byte[]]$data, 0, $data.Length) }
$bw.Flush()
$bw.Close()
$fs.Close()

# Also keep a large preview for the installer/readme.
$preview = New-IconPng 256
[System.IO.File]::WriteAllBytes((Join-Path $outDir "icon-preview.png"), $preview)

Write-Host "Wrote $outPath ($([Math]::Round((Get-Item $outPath).Length / 1024, 1)) KB)"
