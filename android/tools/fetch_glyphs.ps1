# 用构建机的系统字体把界面文案里的字符位图化成 1 位色字形。
#
# 这是 tools/gen_font.js 三段式流水线的第二段：它只负责"渲染"，不做排版与打包。
#
# 刻意**不使用 JSON**：这个脚本要在 Windows PowerShell 5.1 下跑，而实测
# `ConvertFrom-Json` 在 `param()` 脚本里对多行文本会返回字符串而不是对象
# （同一段代码在 -Command 下又正常），排查成本很高。所以改成最朴素的
# "命令行参数进、逐行文本出"，两边都只做字符串切分，没有解析器参与。
#
# 入参:
#   -Sizes      'sm:12,md:16,lg:22'   字号档（名字:像素高）
#   -Codepoints '32,33,65,...'        要渲染的码点（十进制）
#   -Out        build/glyphs_raw.txt  输出路径
# 出参（UTF-8，每行以空格分隔，除 hexrows 外全部为 ASCII）:
#   SIZE <name> <px> <ascent> <lineH>
#   G <cp> <advance> <bx> <by> <inkW> <inkH> <hexrow>,<hexrow>,...
#      hexrow: 一行墨水，每 4 位一个十六进制字符，MSB 在左；行数 = inkH
param(
    [Parameter(Mandatory = $true)][string]$Sizes,
    [Parameter(Mandatory = $true)][string]$Codepoints,
    [Parameter(Mandatory = $true)][string]$Out,
    # 换字体（默认按顺序挑第一个可用的）
    [string]$FontName
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

# 挑一个能画中文的字体。排在前面的更接近原版的硬边像素观感。
$candidates = if ($FontName) { @($FontName) } else {
    @('Microsoft YaHei', 'SimHei', 'SimSun', 'Microsoft JhengHei', 'MingLiU')
}
$chosen = $null
foreach ($name in $candidates) {
    try {
        $probe = New-Object System.Drawing.Font($name, 16, [System.Drawing.FontStyle]::Regular, [System.Drawing.GraphicsUnit]::Pixel)
        if ($probe.FontFamily.Name -eq $name) { $chosen = $name }
        $probe.Dispose()
        if ($chosen) { break }
    }
    catch { }
}
if (-not $chosen) { throw "找不到可用字体（试过：$($candidates -join ', ')）" }
Write-Host "  字体 = $chosen"

$canvasW = 160
$canvasH = 160

function Render-OneGlyph {
    param([System.Drawing.Font]$font, [int]$cp)
    $bmp = New-Object System.Drawing.Bitmap($canvasW, $canvasH, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.Clear([System.Drawing.Color]::Transparent)
    $g.TextRenderingHint = [System.Drawing.Text.TextRenderingHint]::SingleBitPerPixelGridFit
    $fmt = [System.Drawing.StringFormat]::GenericTypographic
    $fmt.FormatFlags = $fmt.FormatFlags -bor [System.Drawing.StringFormatFlags]::MeasureTrailingSpaces
    $s = [string][char]$cp
    # 单字前进宽度：GenericTypographic 量出来的是排版宽度，不含额外行距
    $advance = [int][Math]::Round($g.MeasureString($s, $font, [System.Drawing.PointF]::new(0, 0), $fmt).Width)
    $g.DrawString($s, $font, [System.Drawing.Brushes]::White, 0, 0, $fmt)
    $g.Dispose()

    # 扫出墨水包围盒（阈值 128：只认实心的那部分）
    $minX = $canvasW; $minY = $canvasH; $maxX = -1; $maxY = -1
    for ($y = 0; $y -lt $canvasH; $y++) {
        for ($x = 0; $x -lt $canvasW; $x++) {
            if ($bmp.GetPixel($x, $y).A -ge 128) {
                if ($x -lt $minX) { $minX = $x }
                if ($y -lt $minY) { $minY = $y }
                if ($x -gt $maxX) { $maxX = $x }
                if ($y -gt $maxY) { $maxY = $y }
            }
        }
    }
    if ($maxX -lt 0) {
        $bmp.Dispose()
        return "$cp $advance 0 0 0 0 -"
    }
    $w = $maxX - $minX + 1
    $h = $maxY - $minY + 1
    $rows = New-Object System.Collections.Generic.List[string]
    for ($y = $minY; $y -le $maxY; $y++) {
        $hex = New-Object System.Text.StringBuilder
        for ($i = 0; $i -lt $w; $i += 4) {
            $nib = 0
            for ($b = 0; $b -lt 4; $b++) {
                $nib = $nib -shl 1
                $x = $i + $b
                if ($x -lt $w -and $bmp.GetPixel($minX + $x, $y).A -ge 128) { $nib = $nib -bor 1 }
            }
            [void]$hex.Append($nib.ToString('x'))
        }
        $rows.Add($hex.ToString())
    }
    $bmp.Dispose()
    return "$cp $advance $minX $minY $w $h $($rows -join ',')"
}

$cps = @()
foreach ($p in $Codepoints.Split(',')) {
    if ($p.Trim().Length -gt 0) { $cps += [int]$p.Trim() }
}

$outLines = New-Object System.Collections.Generic.List[string]
foreach ($spec in $Sizes.Split(',')) {
    $bits = $spec.Trim().Split(':')
    $name = $bits[0]
    $px = [int]$bits[1]
    $font = New-Object System.Drawing.Font($chosen, $px, [System.Drawing.FontStyle]::Regular, [System.Drawing.GraphicsUnit]::Pixel)
    $fam = $font.FontFamily
    $ascent = [int][Math]::Round($fam.GetCellAscent([System.Drawing.FontStyle]::Regular) * $px / $fam.GetEmHeight([System.Drawing.FontStyle]::Regular))
    # 用 .Height 而不是 GetHeight()：PowerShell 对 GetHeight 的重载解析会报
    # "Multiple ambiguous overloads"，而 .Height 本来就是像素高度
    $lineH = [int][Math]::Ceiling($font.Height)
    Write-Host "  字号 $name = ${px}px  ascent=$ascent  lineH=$lineH（$($cps.Count) 个字符）"
    $outLines.Add("SIZE $name $px $ascent $lineH")
    foreach ($cp in $cps) {
        $outLines.Add("G " + (Render-OneGlyph -font $font -cp $cp))
    }
    $font.Dispose()
}

if (-not [System.IO.Path]::IsPathRooted($Out)) { $Out = Join-Path (Get-Location).Path $Out }
[System.IO.File]::WriteAllLines($Out, $outLines, (New-Object System.Text.UTF8Encoding($false)))
Write-Host "  写出 $Out（$($outLines.Count) 行）"
