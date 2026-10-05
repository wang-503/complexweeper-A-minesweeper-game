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
    # **必须用逗号包住**：只有一个候选时，`return ($out | Select-Object -Unique)`
    # 会被 PowerShell 解包成单个字符串；调用方 `[string[]]$Roots` 于是拿到字符串，
    # 参数绑定会把它**逐字符**展开成 ['C',':','\','A',...]，Test-Path 'C' 全失败，
    # 候选列表就空了 —— 表现为"明明装了 build-tools r36 却去下载"，
    # 进而让"按版本挑 build-tools/platform"整套逻辑静默失效。
    return , @($out | Select-Object -Unique)
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
    # **必须按版本挑，不能"找到哪个用哪个"。**
    #
    # 这个坑是在 GitHub Actions 上暴露的：runner 预装了 build-tools 34.0.0 与
    # platform-34，而原来的探测逻辑用 `Get-ChildItem -Recurse | Select-Object -First 1`，
    # 于是拿到了 34.0.0 —— $btver 这个变量定义了却从来没被使用。
    # 结果 build-tools 34 的 aapt2 产出的 APK，`dump badging` 读不到 minSdkVersion
    # （只读得到 targetSdkVersion），check_apk.js 报 "minSdk 不低于 24" 失败。
    # 本机之所以一直没事，纯粹是因为本机只装了 r36。
    #
    # 所以：候选按**版本降序**排，取第一个 ≥ 最低要求的；都不满足才下载固定版本。
    # 版本以 `aapt2 version` **自己报的**为准，不靠目录名 —— build-tools r36 的官方
    # zip 解出来叫 `android-16`、platform-35 的 zip 解出来叫 `android-35`，
    # 目录名既不一定是版本号、还会互相撞车。
    $btver = '36.0.0'
    $btMin = [version]'2.20'     # aapt2 报的是 2.NN，r36 = 2.20
    # 注意：**返回列表本身**（逗号包一层），不要写成 `return @(...)`。
    # PowerShell 会把 @() 里的元素展开成"多个返回值"，调用方拿到的 $c 就成了数组，
    # 于是 `$c.Ver -ge $min` 变成**数组比较**（结果也是数组、在 if 里恒真），
    # `$c.Dir` 也会变成数组、拼起来是 "路径A 路径B" —— 传给 aapt2 直接失败。
    # 这个坑是实测出来的：日志里出现过
    #   platform = android-35 35 (C:\...\platform\android-35 C:\...\plat\android-35)
    function Get-Aapt2Candidates([string[]]$Roots) {
        $seen = @{}
        $out = New-Object System.Collections.ArrayList
        foreach ($r in $Roots) {
            if (-not $r -or -not (Test-Path -LiteralPath $r)) { continue }
            foreach ($h in (Get-ChildItem -LiteralPath $r -Filter 'aapt2.exe' -Recurse -ErrorAction SilentlyContinue)) {
                $dir = Split-Path $h.FullName -Parent
                if ($seen.ContainsKey($dir)) { continue }
                $seen[$dir] = $true
                $ver = [version]'0.0'
                try {
                    # 版本以 aapt2 自己报的为准。走 `cmd /c` 取输出（与 build.ps1 的
                    # Invoke-Native 同一路子）：直接 `& $exe version` 时，aapt2 写到
                    # stderr 的版本行会被 PowerShell 包成 ErrorRecord，
                    # 在 try 块里会打断后面的解析，$ver 就一直是 0.0。
                    $txt = [string](& cmd.exe /c "`"$($h.FullName)`" version 2>&1")
                    $m = [regex]::Match($txt, '(\d+)\.(\d+)')
                    if ($m.Success) { $ver = [version]("$($m.Groups[1].Value).$($m.Groups[2].Value)") }
                    Write-Host ("[工具链]   探测 aapt2 " + $dir + " → " + $ver)
                } catch {
                    Write-Host ("[工具链]   探测 aapt2 " + $dir + " 失败：" + $_.Exception.Message)
                }
                [void]$out.Add([pscustomobject]@{ Dir = $dir; Ver = $ver; Aapt2 = $h.FullName })
            }
        }
        # 过滤掉 $null：`@($x)[0].Prop` 在 $x[0] 为 $null 时返回的是**空数组**
        # （在 if 里为真！），于是 $aapt2 会变成数组、Split-Path 返回多个父目录，
        # 命令行里就出现 "路径A 路径B" 这种非法参数
        # （实测报 "The filename, directory name, or volume label syntax is incorrect."）。
        return , @($out | Where-Object { $_ } | Sort-Object -Property Ver -Descending)
    }

    $aapt2 = $null
    $btChosen = $null
    # flatten：函数返回的是"数组"，`@(f(), g())` 会变成**嵌套数组**，
    # 于是 $c 是数组、$c.Ver -ge $btMin 变成数组比较。用加法拼平。
    $allBt = @()
    $allBt += Get-Aapt2Candidates @($root)
    $allBt += Get-Aapt2Candidates $sdkRoots
    foreach ($c in $allBt) {
        if ($c -and -not $btChosen -and $c.Ver -ge $btMin) { $btChosen = $c }
    }
    if ($btChosen) {
        Write-Host ("[工具链] build-tools = aapt " + $btChosen.Ver + "  (" + $btChosen.Dir + ")")
        $aapt2 = $btChosen.Aapt2
    }
    else {
        # 机器上只有更旧的 build-tools：**不要凑合**，下载工程要求的版本。
        # 静默降级正是上面那个 CI 故障的根源。
        $foundTxt = if ($allBt.Count) { ($allBt | ForEach-Object { $_.Ver.ToString() }) -join ', ' } else { '无' }
        if ($SkipDownload) { $missing += "build-tools aapt >= $btMin（现有：$foundTxt）" }
        else {
            Write-Host "[工具链] 机器上的 build-tools 是 [$foundTxt]，低于要求的 aapt $btMin，下载 r36（约 56 MB）"
            $zip = Join-Path $root 'pkg\build-tools_r36_windows.zip'
            Get-RemoteFile -Url 'https://dl.google.com/android/repository/build-tools_r36_windows.zip' -Dest $zip -MinBytes 20MB
            # 这个包解出来是一层 android-16/ 目录，内容就是 build-tools 本体
            Expand-Zip -Zip $zip -Dest (Join-Path $root 'build-tools')
            $again = @(Get-Aapt2Candidates @($root))
            if ($again.Count) { $btChosen = $again[0]; $aapt2 = $btChosen.Aapt2 }
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
    # 同样按 API 版本挑，理由与 build-tools 相同：别拿了机器上的 platform-34 就用。
    # android.jar 自己不带版本，但同目录下的 source.properties 里有 ApiLevel。
    $platMin = 35
    function Get-PlatformCandidates([string[]]$Roots) {
        $seen = @{}
        $out = New-Object System.Collections.ArrayList
        foreach ($r in $Roots) {
            if (-not $r -or -not (Test-Path -LiteralPath $r)) { continue }
            foreach ($h in (Get-ChildItem -LiteralPath $r -Filter 'android.jar' -Recurse -ErrorAction SilentlyContinue)) {
                $dir = Split-Path $h.FullName -Parent
                if ($seen.ContainsKey($dir)) { continue }
                $seen[$dir] = $true
                $api = 0
                $sp = Join-Path $dir 'source.properties'
                if (Test-Path -LiteralPath $sp) {
                    $m = [regex]::Match((Get-Content -LiteralPath $sp -Raw -ErrorAction SilentlyContinue),
                                        'AndroidVersion\.ApiLevel\s*=\s*(\d+)')
                    if ($m.Success) { $api = [int]$m.Groups[1].Value }
                }
                if ($api -eq 0) {
                    $m2 = [regex]::Match($dir, 'android-(\d+)$')   # 退路：目录名
                    if ($m2.Success) { $api = [int]$m2.Groups[1].Value }
                }
                [void]$out.Add([pscustomobject]@{ Dir = $dir; Api = $api; Jar = $h.FullName })
            }
        }
        # 同 Get-Aapt2Candidates：必须用逗号包住（否则元素被展开成多返回值），
        # 并且要过滤 $null（否则 "空数组在 if 里为真" 会把路径拼坏）
        return , @($out | Where-Object { $_ } | Sort-Object -Property Api -Descending)
    }

    $androidJar = $null
    $platChosen = $null
    $allPlat = @()
    $allPlat += Get-PlatformCandidates @($root)
    $allPlat += Get-PlatformCandidates $sdkRoots
    foreach ($c in $allPlat) {
        if ($c -and -not $platChosen -and $c.Api -ge $platMin) { $platChosen = $c }
    }
    if ($platChosen) {
        Write-Host ("[工具链] platform    = android-" + $platChosen.Api + "  (" + $platChosen.Dir + ")")
        $androidJar = $platChosen.Jar
    }
    else {
        $foundTxt = if ($allPlat.Count) { ($allPlat | ForEach-Object { 'android-' + $_.Api }) -join ', ' } else { '无' }
        if ($SkipDownload) { $missing += "platform-$platMin（android.jar，现有：$foundTxt）" }
        else {
            Write-Host "[工具链] 机器上的 platform 是 [$foundTxt]，低于要求的 android-$platMin，下载 platform-35（约 61 MB）"
            $zip = Join-Path $root 'pkg\platform-35_r02.zip'
            Get-RemoteFile -Url 'https://dl.google.com/android/repository/platform-35_r02.zip' -Dest $zip -MinBytes 20MB
            Expand-Zip -Zip $zip -Dest (Join-Path $root 'platform')
            $again = @(Get-PlatformCandidates @($root))
            if ($again.Count) { $androidJar = $again[0].Jar }
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
