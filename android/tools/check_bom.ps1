# 检查仓库里所有 .ps1 是否带 UTF-8 BOM。
# 为什么需要：PowerShell 5.1 读无 BOM 的 UTF-8 文件会按 ANSI 解析，
# 中文全乱码、脚本直接报 Missing type name after '[' 之类的语法错误。
# 编辑工具保存时常常把 BOM 去掉，CI（.github/workflows/build.yml）也会查这一条。
$root = Split-Path $PSScriptRoot -Parent
$bad = @()
Get-ChildItem $root -Recurse -Filter *.ps1 |
    Where-Object { $_.FullName -notmatch '\\build\\|\\.android-tools\\' } |
    ForEach-Object {
        $b = [System.IO.File]::ReadAllBytes($_.FullName)
        $ok = $b.Length -ge 3 -and $b[0] -eq 0xEF -and $b[1] -eq 0xBB -and $b[2] -eq 0xBF
        "{0}  {1}" -f $(if ($ok) { 'OK    ' } else { '缺 BOM' }), $_.FullName.Substring($root.Length + 1)
        if (-not $ok) { $bad += $_.FullName }
    }
if ($bad.Count) { Write-Error ("这些脚本缺 UTF-8 BOM：" + ($bad -join ' / ')); exit 1 }