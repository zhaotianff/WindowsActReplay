# 生成多尺寸 32bpp ICO（深色圆环 + 红色“记录”圆点），仅使用 .NET，无第三方依赖。
# 用法：powershell -File tools\make_icon.ps1
[CmdletBinding()]
param([string]$OutFile)

if ([string]::IsNullOrEmpty($OutFile)) {
    $OutFile = Join-Path (Split-Path $PSScriptRoot -Parent) 'HistoryRecord\HistoryRecord.ico'
}

function New-IconImage([int]$size) {
    $xor = New-Object byte[] ($size * $size * 4)
    $cx = ($size - 1) / 2.0
    $ring = [Math]::Max(1.0, $size * 0.06)
    $rOut = $size * 0.46
    $rIn  = $size * 0.26
    for ($row = 0; $row -lt $size; $row++) {
        $y = $size - 1 - $row  # XOR 位图自下而上存储
        for ($x = 0; $x -lt $size; $x++) {
            $dx = $x - $cx; $dy = $y - $cx
            $d = [Math]::Sqrt($dx * $dx + $dy * $dy)
            if ($d -gt $rOut) { continue }   # alpha=0，透明
            $o = ($row * $size + $x) * 4
            if ($d -ge $rOut - $ring) {
                $b = 40;  $g = 40;  $r = 45    # 深色外圈
            } elseif ($d -le $rIn) {
                $b = 70;  $g = 70;  $r = 225   # 红色圆点（BGR 序）
            } else {
                $b = 245; $g = 245; $r = 245   # 白色间隔环
            }
            $xor[$o] = [byte]$b; $xor[$o + 1] = [byte]$g
            $xor[$o + 2] = [byte]$r; $xor[$o + 3] = 255
        }
    }
    $maskStride = [int][Math]::Ceiling($size / 32.0) * 4
    $and = New-Object byte[] ($maskStride * $size)  # 全 0：透明度由 XOR 的 alpha 通道决定

    $ms = New-Object System.IO.MemoryStream
    $bw = New-Object System.IO.BinaryWriter($ms)
    $bw.Write([uint32]40)                          # BITMAPINFOHEADER.biSize
    $bw.Write([int32]$size)                        # biWidth
    $bw.Write([int32]($size * 2))                  # biHeight = 2*h（含 AND 掩码）
    $bw.Write([uint16]1)                           # biPlanes
    $bw.Write([uint16]32)                          # biBitCount
    $bw.Write([uint32]0)                           # biCompression = BI_RGB
    $bw.Write([uint32]($xor.Length + $and.Length)) # biSizeImage
    $bw.Write([int32]0); $bw.Write([int32]0); $bw.Write([uint32]0); $bw.Write([uint32]0)
    $bw.Write($xor)
    $bw.Write($and)
    $bw.Flush()
    return ,$ms.ToArray()
}

$sizes = 16, 32, 48
$images = @()
foreach ($s in $sizes) { $images += ,(New-IconImage $s) }

$ms = New-Object System.IO.MemoryStream
$bw = New-Object System.IO.BinaryWriter($ms)
$bw.Write([uint16]0)              # ICONDIR.reserved
$bw.Write([uint16]1)              # ICONDIR.type = 1 (icon)
$bw.Write([uint16]$sizes.Count)   # ICONDIR.count
$offset = 6 + 16 * $sizes.Count
for ($i = 0; $i -lt $sizes.Count; $i++) {
    $bw.Write([byte]$sizes[$i])   # width
    $bw.Write([byte]$sizes[$i])   # height
    $bw.Write([byte]0)            # color count
    $bw.Write([byte]0)            # reserved
    $bw.Write([uint16]1)          # planes
    $bw.Write([uint16]32)         # bit count
    $bw.Write([uint32]$images[$i].Length)
    $bw.Write([uint32]$offset)
    $offset += $images[$i].Length
}
foreach ($img in $images) { $bw.Write($img) }
$bw.Flush()
[System.IO.File]::WriteAllBytes($OutFile, $ms.ToArray())
Write-Host "已生成 $OutFile ($($ms.Length) 字节)"
