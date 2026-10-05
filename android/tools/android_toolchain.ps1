# 安卓构建工具链的探测与获取。
#
# 这个脚本只做一件事：让 build.ps1 能拿到四个东西的绝对路径 ——
#   NDK(clang/llvm-ar/sysroot) · build-tools(aapt2/zipalign/apksigner.jar) · platform(android.jar) · JDK(java/keytool)
# 探测顺序是「先用机器上已经有的，缺的才下载」，下载目标默认是仓库内的
#   android/.android-tools/   （已 gitignore）
# 这样第二次构建完全离线可用，也不会污染系统的 SDK 目录。
#
# 存放路径想放别处就设环境变量 CSB_ANDROID_TOOLS；想完全禁止联网就加 -SkipDownload。
#
# 用法（一般不用直接调，build.ps1 会 dot-source 它）：
#   . tools/android_toolchain.ps1 -SkipDownload
#   $tc = Get-Toolchain
param(
    # 只探测不下下载；缺东西就直接报错并告诉你缺什么
    [switch]$SkipDownload,
    # 覆盖缓存目录
    [string]$ToolsDir
)

$ErrorActionPreference = 'Stop'

# 本文件是纯 ASCII，不需要 BOM 处理；下面的探测结果统一放这个 hashtable 里
function Get-ToolchainRoot {
    param([string]$Override)
    if ($Override) { return (Resolve-Path -LiteralPath $Override).Path }
    if ($env:CSB_ANDROID_TOOLS) { return $env:CSB_ANDROID_TOOLS }
    # 默认落在仓库里：android/.android-tools
    $here = Split-Path $PSScriptRoot -Parent          # android/
    return (Join-Path $here '.android-tools')
}

# 下载带一个 ".part" 临时名 + 尺寸校验，避免半截文件留在缓存里被当成有效工具链
function Get-RemoteFile {
    param([string]$Url, [string]$Dest, [long]$MinBytes = 1MB)
    if (Test-Path -LiteralPath $Dest) {
        if ((Get-Item -LiteralPath $Dest).Length -ge $MinBytes) { Write-Host "  已有 $(Split-Path $Dest -Leaf)"; return }
        Write-Host "  已有的 $(Split-Path $Dest -Leaf) 尺寸不对，重下"
        Remove-Item -LiteralPath $Dest -Force
    }
    $part = "$Dest.part"
    if (Test-Path -LiteralPath $part) { Remove-Item -LiteralPath $part -Force }
    New-Item -ItemType Directory -Force -Path (Split-Path $Dest -Parent) | Out-Null
    Write-Host "  下载 $Url"
    $old = $ProgressPreference
    $ProgressPreference = 'SilentlyContinue'   # 关掉进度条，Invoke-WebRequest 快一个量级
    try {
        Invoke-WebRequest -Uri $Url -OutFile $part -TimeoutSec 3600 -UseBasicParsing | Out-Null
    } finally {
        $ProgressPreference = $old
    }
    $size = (Get-Item -LiteralPath $part).Length
    if ($size -lt $MinBytes) { throw "下载的文件太小（$size 字节），像是失败了：$Url" }
    Move-Item -LiteralPath $part -Destination $Dest -Force
    Write-Host ("  完成 {0:N1} MB" -f ($size / 1MB))
}

# Expand-Archive 在很深的 NDK 路径上偶发长路径问题，统一走 .NET 的 ZipFile
function Expand-Zip {
    param([string]$Zip, [string]$Dest)
    $marker = Join-Path $Dest '.extracted'
    if (Test-Path -LiteralPath $marker) { return }
    Write-Host "  解压 $(Split-Path $Zip -Leaf) → $Dest"
    New-Item -ItemType Directory -Force -Path $Dest | Out-Null
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    [System.IO.Compression.ZipFile]::ExtractToDirectory($Zip, $Dest)
    Set-Content -LiteralPath $marker -Value 'ok' -Encoding ASCII
}

# 收集机器上所有可能的 Android SDK 根目录
function Get-SdkCandidates {
    $out = @()
    foreach ($k in 'ANDROID_HOME', 'ANDROID_SDK_ROOT') {
        $v = [Environment]::GetEnvironmentVariable($k)
        if ($v -and (Test-Path -LiteralPath $v)) { $out += $v }
    }
    foreach ($p in @(
            (Join-Path $env:LOCALAPPDATA 'Android\Sdk'),
            'C:\Android\Sdk',
            (Join-Path ${env:ProgramFiles(x86)} 'Android\android-sdk'))) {
        if ($p -and (Test-Path -LiteralPath $p)) { $out += $p }
    }
    return ($out | Select-Object -Unique)
}

# 在若干候选目录里找一个文件/目录；返回绝对路径或 $null
function Find-In {
    param([string[]]$Roots, [string]$Leaf, [switch]$Directory)
    foreach ($r in $Roots) {
        if (-not $r -or -not (Test-Path -LiteralPath $r)) { continue }
        $hit = Get-ChildItem -LiteralPath $r -Filter $Leaf -Recurse -ErrorAction SilentlyContinue |
        Where-Object { if ($Directory) { $_.PSIsContainer } else { -not $_.PSIsContainer } } |
        Select-Object -First 1
        if ($hit) { return $hit.FullName }
    }
    return $null
}

# ---- JDK：优先 21（实测 25 也能签名，只是会打 native-access 警告） ----
function Resolve-Jdk {
    foreach ($v in 'JAVA_HOME') {
        $j = [Environment]::GetEnvironmentVariable($v)
        if ($j -and (Test-Path -LiteralPath (Join-Path $j 'bin\java.exe'))) { return $j }
    }
    # 常见的安装位置，21 优先
    $roots = @()
    foreach ($base in @("$env:ProgramFiles\Java", "$env:ProgramFiles\Eclipse Adoptium",
            "$env:ProgramFiles\Microsoft", "$env:LOCALAPPDATA\Programs\Eclipse Adoptium")) {
        if (Test-Path -LiteralPath $base) {
            $roots += Get-ChildItem -LiteralPath $base -Directory -ErrorAction SilentlyContinue |
            Where-Object { $_.Name -match 'jdk|jre' } | Select-Object -ExpandProperty FullName
        }
    }
    $pref = $roots | Where-Object { $_ -match '21' } | Select-Object -First 1
    if ($pref) { return $pref }
    $any = $roots | Select-Object -First 1
    if ($any) { return $any }
    # 最后退回 PATH 上的 java（keytool 可能不在，build 时会明确报错）
    return $null
}

function Get-Toolchain {
    param([switch]$SkipDownload, [string]$ToolsDir)

    $root = Get-ToolchainRoot -Override $ToolsDir
    $tc = @{ Root = $root }
    $missing = @()
    $sdkRoots = Get-SdkCandidates

    # ---------------- NDK ----------------
    $ndk = $null
    foreach ($k in 'ANDROID_NDK_HOME', 'ANDROID_NDK_ROOT') {
        $v = [Environment]::GetEnvironmentVariable($k)
        if ($v -and (Test-Path -LiteralPath $v)) { $ndk = $v; break }
    }
    if (-not $ndk) {
        $ndk = Find-In -Roots $root -Leaf 'android-ndk-*' -Directory
        # NDK 的 clang 必须真的在，光有个目录名不算
        if ($ndk -and -not (Test-Path -LiteralPath (Join-Path $ndk 'toolchains\llvm\prebuilt'))) { $ndk = $null }
    }
    if (-not $ndk) { $ndk = Find-In -Roots $sdkRoots -Leaf 'android-ndk-*' -Directory }
    if (-not $ndk) {
        if ($SkipDownload) { $missing += 'NDK（android-ndk-r27c）' }
        else {
            Write-Host '[工具链] NDK 没找到，下载 r27c（约 745 MB，只有第一次要下）'
            $zip = Join-Path $root 'pkg\android-ndk-r27c-windows.zip'
            Get-RemoteFile -Url 'https://dl.google.com/android/repository/android-ndk-r27c-windows.zip' -Dest $zip -MinBytes 400MB
            Expand-Zip -Zip $zip -Dest $root
            $ndk = Find-In -Roots $root -Leaf 'android-ndk-*' -Directory
        }
    }
    if ($ndk) {
        $tc.Ndk = $ndk
        $tc.Clang = Join-Path $ndk 'toolchains\llvm\prebuilt\windows-x86_64\bin\clang.exe'
        $tc.Ar = Join-Path $ndk 'toolchains\llvm\prebuilt\windows-x86_64\bin\llvm-ar.exe'
        $tc.Sysroot = Join-Path $ndk 'toolchains\llvm\prebuilt\windows-x86_64\sysroot'
        $tc.Glue = Join-Path $ndk 'sources\android\native_app_glue\android_native_app_glue.c'
        $tc.GlueInc = Join-Path $ndk 'sources\android\native_app_glue'
    }

    # ---------------- build-tools ----------------
    $btver = '36.0.0'
    $aapt2 = Find-In -Roots $root -Leaf 'aapt2.exe'
    if (-not $aapt2) { $aapt2 = Find-In -Roots $sdkRoots -Leaf 'aapt2.exe' }
    if (-not $aapt2) {
        if ($SkipDownload) { $missing += 'build-tools（aapt2/zipalign/apksigner）' }
        else {
            Write-Host '[工具链] build-tools 没找到，下载 r36（约 56 MB）'
            $zip = Join-Path $root 'pkg\build-tools_r36_windows.zip'
            Get-RemoteFile -Url 'https://dl.google.com/android/repository/build-tools_r36_windows.zip' -Dest $zip -MinBytes 20MB
            # 这个包解出来是一层 android-16/ 目录，内容就是 build-tools 本体
            Expand-Zip -Zip $zip -Dest (Join-Path $root 'build-tools')
            $aapt2 = Find-In -Roots $root -Leaf 'aapt2.exe'
        }
    }
    if ($aapt2) {
        $bt = Split-Path $aapt2 -Parent
        $tc.BuildTools = $bt
        $tc.Aapt2 = $aapt2
        $tc.Zipalign = Join-Path $bt 'zipalign.exe'
        $tc.ApkSignerJar = Join-Path $bt 'lib\apksigner.jar'
    }

    # ---------------- platform（为 android.jar） ----------------
    $androidJar = Find-In -Roots $root -Leaf 'android.jar'
    if (-not $androidJar) { $androidJar = Find-In -Roots $sdkRoots -Leaf 'android.jar' }
    if (-not $androidJar) {
        if ($SkipDownload) { $missing += 'platform-35（android.jar）' }
        else {
            Write-Host '[工具链] android.jar 没找到，下载 platform-35（约 61 MB）'
            $zip = Join-Path $root 'pkg\platform-35_r02.zip'
            Get-RemoteFile -Url 'https://dl.google.com/android/repository/platform-35_r02.zip' -Dest $zip -MinBytes 20MB
            Expand-Zip -Zip $zip -Dest (Join-Path $root 'platform')
            $androidJar = Find-In -Roots $root -Leaf 'android.jar'
        }
    }
    if ($androidJar) { $tc.AndroidJar = $androidJar }

    # ---------------- JDK ----------------
    $jdk = Resolve-Jdk
    if ($jdk) {
        $tc.Jdk = $jdk
        $tc.Java = Join-Path $jdk 'bin\java.exe'
        $tc.Keytool = Join-Path $jdk 'bin\keytool.exe'
    }
    else {
        $java = (Get-Command java.exe -ErrorAction SilentlyContinue).Source
        $keytool = (Get-Command keytool.exe -ErrorAction SilentlyContinue).Source
        if ($java) { $tc.Java = $java }
        if ($keytool) { $tc.Keytool = $keytool }
        if (-not ($java -and $keytool)) { $missing += 'JDK（java + keytool）' }
    }

    # ---------------- zig（宿主机自检/预览编译器） ----------------
    # 必须点名 0.14.1：缓存目录里可能同时躺着别的版本，泛泛找 zig.exe 会抽到别的版本，
    # 那会导致 Windows 版（按 0.14.1 写）编译行为漂移。
    $zig = (Get-Command zig.exe -ErrorAction SilentlyContinue).Source
    if (-not $zig -and (Test-Path -LiteralPath $root)) {
        $zig = Get-ChildItem -LiteralPath $root -Filter 'zig.exe' -Recurse -ErrorAction SilentlyContinue |
        Where-Object { $_.FullName -match 'zig-.*0\.14\.1' } | Select-Object -First 1 -ExpandProperty FullName
    }
    if (-not $zig) {
        $zig = Get-ChildItem -LiteralPath $env:LOCALAPPDATA -Filter 'zig.exe' -Recurse -ErrorAction SilentlyContinue |
        Where-Object { $_.FullName -match 'zig-.*0\.14\.1' } | Select-Object -First 1 -ExpandProperty FullName
    }
    if (-not $zig) {
        if ($SkipDownload) { $missing += 'zig 0.14.1（宿主机自检与预览）' }
        else {
            Write-Host '[工具链] zig 没找到，下载 0.14.1（约 82 MB）'
            $zip = Join-Path $root 'pkg\zig-x86_64-windows-0.14.1.zip'
            Get-RemoteFile -Url 'https://ziglang.org/download/0.14.1/zig-x86_64-windows-0.14.1.zip' -Dest $zip -MinBytes 40MB
            Expand-Zip -Zip $zip -Dest $root
            $zig = Find-In -Roots $root -Leaf 'zig.exe'
        }
    }
    if ($zig) {
        # 版本对不上就是迟早出事，这里当场说清楚
        $v = (& $zig version 2>&1 | Select-Object -First 1)
        if ("$v" -ne '0.14.1') { Write-Host "[工具链] 注意：zig 版本是 $v，本工程按 0.14.1 写" -ForegroundColor Yellow }
        $tc.Zig = $zig
    }
    if (-not $tc.Ndk) { $missing += 'NDK' }
    if (-not $tc.Aapt2) { $missing += 'build-tools（aapt2）' }
    if (-not $tc.AndroidJar) { $missing += 'android.jar' }

    if ($missing.Count) {
        throw ("工具链缺这些，且当前是 -SkipDownload：`n  - " + ($missing -join "`n  - ") +
            "`n去掉 -SkipDownload 让脚本自己下载，或把已有 SDK 的路径设进 ANDROID_HOME / ANDROID_NDK_HOME。")
    }
    return $tc
}
