# 复扫雷 安卓版 · 构建脚本
#
# 一条命令做完全部事情：
#   1) 生成图集（与 Windows 版共用 tools/gen_atlas.js）
#   2) 生成界面文案表与中文字形图集（字形覆盖率缺一个就让构建失败）
#   3) 编宿主机自检程序（用 zig cc），跑规则自检 + 界面/触屏自检 + 跨端规则对照
#   4) 用 NDK clang 编 arm64/x86_64 的 libcomplexsweeper.so
#   5) aapt2 打包 → 加 so → zipalign → apksigner 签名 → 产出 APK
#   6) 校验 APK 结构
#
# 用法：
#   powershell -ExecutionPolicy Bypass -File build.ps1
#   ... -SkipTests      只出 APK，不跑自检（赶时间用，交付前别省）
#   ... -SkipDownload   不联网；缺工具链就直接报错
#   ... -Abis arm64-v8a,x86_64   想带上模拟器用的 x86_64
#   ... -Scale 3        预览图放大倍率
param(
    [switch]$SkipTests,
    [switch]$SkipDownload,
    [string]$Abis = 'arm64-v8a,x86_64',
    [int]$Scale = 2,
    [string]$PackageName = '',
    # 强制重新位图化中文字形（需要 Windows 的 System.Drawing）。
    # 平时不用加：仓库里提交了 font_atlas.bin + font_meta.h + tools/font.stamp，
    # 三者配套就直接复用。只在改了文案/字号、或想换字体时才需要。
    [switch]$RegenFont,
    [switch]$Quiet
)
# 注意这里**故意用 Continue 而不是 Stop**：
# PowerShell 5.1 在 $ErrorActionPreference='Stop' 下，会把"原生程序往 stderr 写的任何
# 输出"当成终止性错误。而 keytool、apksigner、clang 都会正常地往 stderr 打进度或警告，
# 于是脚本会在明明成功的时候中断（实测 keytool 生成完密钥就直接抛异常退出）。
# 所以改成每一步自己查 $LASTEXITCODE，失败就调 Fail 明确报错。
$ErrorActionPreference = 'Continue'

# 原生工具统一走下面这个包装：cmd /c "整条命令 2>&1"，再把输出收进来。
# 为什么要这么绕（三条都是实测踩出来的）：
#   1) PowerShell 5.1 会把原生程序写到 stderr 的**任何**输出包成 ErrorRecord。
#      clang / keytool / apksigner 都会正常地往 stderr 打警告或进度，于是脚本会在
#      明明成功的时候被判失败、并以 1 退出。
#   2) PowerShell 5.1 还会把未加引号的 "-I<路径>" 从空格处拆开
#      （"-IC:\Code Programs\..." 变成两段），编译器于是找不到 include 目录。
#   3) cmd /c 自己解析引号时要求"整条命令外面再套一层引号"。
# 用这个函数发原生命令，三个坑一次消掉。
function Invoke-Native([string]$exe, [string[]]$argv, [switch]$QuietOutput) {
    $quoted = @()
    foreach ($a in $argv) {
        if ($a -match '[\s"]') { $quoted += '"' + ($a -replace '"', '\"') + '"' } else { $quoted += $a }
    }
    $line = 'cmd /c ""' + $exe + '" ' + ($quoted -join ' ') + ' 2>&1"'
    $out = cmd /c $line
    $code = $LASTEXITCODE
    if (-not $QuietOutput) {
        foreach ($l in @($out)) { if ("$l".Trim().Length -gt 0) { Write-Host "  | $l" } }
    }
    return $code
}

$root = $PSScriptRoot                     # android/
$srcDir = Join-Path $root 'src'
$bldDir = Join-Path $root 'build'
$assetsDir = Join-Path $root 'assets'

function Say([string]$msg) { if (-not $Quiet) { Write-Host $msg } }
function Fail([string]$msg) { throw $msg }

# 先决条件：所有 .ps1 必须带 UTF-8 BOM。
# PowerShell 5.1 读无 BOM 的 UTF-8 文件会按 ANSI 解析 → 中文注释全乱码 →
# 直接报 "Missing type name after '['" 之类的语法错误，而且报错位置完全指不到真因。
# 编辑工具保存时很容易把 BOM 去掉，所以每次构建先查一遍（这条已经在 CI 里也有）。
if ((Invoke-Native 'powershell' @('-NoProfile', '-ExecutionPolicy', 'Bypass',
        '-File', (Join-Path $root 'tools\check_bom.ps1'))) -ne 0) {
    Fail '有 .ps1 缺 UTF-8 BOM（见上面的列表）；补齐后重试'
}

# ABI → NDK 目标三元组 + API 级别
function Get-AbiTarget([string]$abi) {
    switch ($abi) {
        'arm64-v8a' { return 'aarch64-linux-android24' }
        'armeabi-v7a' { return 'armv7a-linux-androideabi24' }
        'x86_64' { return 'x86_64-linux-android24' }
        default { Fail "不认识的 ABI：$abi（支持 arm64-v8a / armeabi-v7a / x86_64）" }
    }
}

New-Item -ItemType Directory -Force -Path $bldDir, $assetsDir | Out-Null

# ------------------------------------------------------------------ 工具链
Say "[0/6] 准备工具链…"
. (Join-Path $root 'tools\android_toolchain.ps1')
$tc = Get-Toolchain -SkipDownload:$SkipDownload
Say "  NDK        = $($tc.Ndk)"
Say "  build-tools= $($tc.BuildTools)"
Say "  android.jar= $($tc.AndroidJar)"
Say "  zig        = $($tc.Zig)"
Say "  JDK        = $($tc.Jdk)"
if (-not $tc.Zig) { Fail "找不到 zig；宿主机自检必须要有它。去掉 -SkipDownload 让脚本自己下。" }

# 版本号只有一处来源：正式版/src/main.zig 里的 APP_VERSION。
# 生成出来的 ui_strings.h 里也有一份 CS_APP_VERSION_STR，构建时核对两者必须一致，
# 免得安卓版和 Windows 版报出不同的版本号。
$mainZig = Join-Path $root '..\正式版\src\main.zig'
# 一律用 [System.IO.File]::ReadAllText 读：Get-Content -Raw 在 PowerShell 5.1 下
# 会把很长的行折断（生成的 ui_strings.h 里就有几千字符的行），正则就匹配不到了。
$mainZigText = [System.IO.File]::ReadAllText($mainZig, [System.Text.Encoding]::UTF8)
$verMatch = [regex]::Match($mainZigText, 'const APP_VERSION = "([0-9]+\.[0-9]+\.[0-9]+)";')
if (-not $verMatch.Success) { Fail '读不到 正式版/src/main.zig 里的 APP_VERSION' }
$version = $verMatch.Groups[1].Value
Say "  版本号     = $version（来自 正式版/src/main.zig）"

# ------------------------------------------------------------------ 1 图集
Say "`n[1/6] 生成图集…"
if ((Invoke-Native 'node' @((Join-Path $root '..\正式版\tools\gen_atlas.js'), '--out-bin', (Join-Path $assetsDir 'atlas.bin'), '--out-zig', (Join-Path $bldDir 'atlas_zig_unused.zig'))) -ne 0) { Fail '生成图集失败' }




# ------------------------------------------------------------------ 2 字体与文案
Say "`n[2/6] 生成界面文案与中文字形…"
if ((Invoke-Native 'node' @((Join-Path $root 'tools\gen_ui_strings.js'))) -ne 0) { Fail '生成界面文案失败' }


# 版本号一致性：gen_ui_strings.js 从 tools/ui_strings.js 里取 APP_VERSION
$uiStringsH = [System.IO.File]::ReadAllText((Join-Path $srcDir 'ui_strings.h'), [System.Text.Encoding]::UTF8)
$uiVer = [regex]::Match($uiStringsH, '#define CS_APP_VERSION_STR "([^"]+)"').Groups[1].Value
if ($uiVer -ne $version) {
    Fail "版本号不一致：正式版/src/main.zig 是 $version，tools/ui_strings.js 是 $uiVer。请把 ui_strings.js 里的 APP_VERSION 改成 $version。"
}

# 字形图集：只要 ui_strings.js 或 gen_font.js 变了就必须重生成。
#
# **判断方式：自洽对比，不依赖任何额外的 stamp 文件。**
# gen_font.js 会把 `FONT_SRC_HASH = sha256(ui_strings.js) + sha256(gen_font.js)`
# 写进它生成的 src/font_meta.h；这里现场重算一遍、和那份产物里记的比。
# 一致且 font_atlas.bin 也在 → 直接复用；否则重新跑字形流水线。
#
# 为什么不用 stamp 文件（前后踩过两次）：
#   1. 放在 build/（已 gitignore）→ 新克隆里没有，必然重新生成，而字形流水线
#      依赖 Windows 的 System.Drawing，别人根本跑不了。
#   2. 放在 tools/ 入库 → 又变成"stamp 与脚本可能不同步"。我改完 gen_font.js
#      忘了更新它，构建就一直复用旧图集，真机表现为整屏乱码、排查很久。
# 把哈希写进产物自己，就不存在"两份东西要同时更新"的问题了。
#
# gen_font.js 也必须进哈希：它决定图集怎么排布，只按文案判断会漏掉排布改动。
#
# **哈希前必须把 CRLF 归一成 LF**，且算法要和 gen_font.js 里那两行完全一致。
# 为什么：同一个 tools/ui_strings.js 在我本机工作区是 CRLF、在 git blob 里是 LF，
# 直接对文件字节做 SHA256 会得出两个值 —— 于是"本机算的"和"别人 clone 出来算的"
# 永远不同，每个新克隆都会去重新跑字形流水线（而那依赖 Windows 的 System.Drawing）。
# 归一化只吃掉换行符，其余字节照常参与哈希。
function Get-FontSrcHash([string[]]$paths) {
    $sha = [System.Security.Cryptography.SHA256]::Create()
    $sb = [System.Text.StringBuilder]::new()
    foreach ($p in $paths) {
        $text = [System.IO.File]::ReadAllText($p, [System.Text.Encoding]::UTF8) -replace "`r`n", "`n"
        $bytes = [System.Text.Encoding]::UTF8.GetBytes($text)
        foreach ($b in $sha.ComputeHash($bytes)) { [void]$sb.Append($b.ToString('x2')) }
    }
    return $sb.ToString()
}
$fontAtlasBin = Join-Path $assetsDir 'font_atlas.bin'
$fontMetaH = Join-Path $srcDir 'font_meta.h'
$fontHashSrc = (Get-FontSrcHash @((Join-Path $root 'tools\ui_strings.js'),
                                  (Join-Path $root 'tools\gen_font.js'))).ToUpperInvariant()

$needFont = $true
if ((Test-Path -LiteralPath $fontMetaH) -and (Test-Path -LiteralPath $fontAtlasBin)) {
    $metaText = [System.IO.File]::ReadAllText($fontMetaH, [System.Text.Encoding]::UTF8)
    $m = [regex]::Match($metaText, '#define FONT_SRC_HASH "([0-9a-f]+)"')
    if ($m.Success -and ($m.Groups[1].Value.ToUpperInvariant() -eq $fontHashSrc)) { $needFont = $false }
}
if ($RegenFont) { $needFont = $true }

if ($needFont) {
    if ((Invoke-Native 'node' @((Join-Path $root 'tools\gen_font.js'))) -ne 0) { Fail '生成字形图集失败' }
}
else {
    Say "  字形图集与当前脚本一致（font_meta.h 里的 FONT_SRC_HASH 对得上），直接复用"
}

# 中文字形覆盖率门禁：ui_strings.js 里出现的每个字符都必须在图集里。
# 这一条必须在编译之前过，否则装到手机上会看到方块。
if ((Invoke-Native 'node' @((Join-Path $root 'tools\check_glyphs.js'))) -ne 0) { Fail '字形覆盖率检查失败（界面文案里有字符没被生成进图集）' }

# 图集**结构**门禁：每页数据长度必须恰好等于 stride × page_h。
# 运行时就是按这个式子算每页步长的，短一页就会从下一段内存取字形 ——
# 表现是**整个界面文字乱码**（多页时才会暴露，单页时永远看不出来）。
if ((Invoke-Native 'node' @((Join-Path $root 'tools\check_atlas.js'))) -ne 0) { Fail '字形图集结构检查失败（页长度与 page_w×page_h 不符）' }


# ------------------------------------------------------------------ 3 宿主机自检
Say "`n[3/6] 编译宿主机自检程序…"
# 先把两份二进制图集转成 C 头（main_host.c 会 #include 它们），**必须在编译之前**，
# 而且每次构建都重做一遍：图集变了这里必须跟着变。
# 显式检查"到底有没有生成出来"，别只信 node 的退出码。
foreach ($blob in @(
        @{ bin = (Join-Path $assetsDir 'atlas.bin'); inc = (Join-Path $bldDir 'atlas_blob.inc'); name = 'atlas' },
        @{ bin = (Join-Path $assetsDir 'font_atlas.bin'); inc = (Join-Path $bldDir 'font_blob.inc'); name = 'font' })) {
    if (-not (Test-Path -LiteralPath $blob.bin)) { Fail "缺少 $($blob.bin)，前面生成图集的步骤没成功" }
    if ((Invoke-Native 'node' @((Join-Path $root 'tools\embed_bin.js'), $blob.bin, $blob.inc, $blob.name)) -ne 0) { Fail "嵌入 $($blob.name) 失败" }
    if (-not (Test-Path -LiteralPath $blob.inc)) { Fail "嵌入 $($blob.name) 之后没生成 $($blob.inc)" }
}

$commonCFlags = @('-std=c11', '-Wall', '-Wextra', '-Wno-unused-parameter', '-Wno-unused-function')
# 关于路径里的空格（很重要，踩了很久）：
# PowerShell 5.1 把**未加引号**的 "-I$dir" 当成一个待拆分的值 —— 路径里有空格时
# 会被拆成 "-IC:\Code" 和 "Programs\..."，编译器于是找不到 include 目录。
# 把整个参数写成一个**带引号的字符串**（"-I$dir"）就不会被拆。
# 所以下面一律用 "$dir\x.c" / "-I$dir" 这种形态拼参数数组，再 @ 展开。
$hostInc = @("-I$srcDir", "-I$bldDir")
$hostObjs = @()
foreach ($f in @('strbuf', 'game', 'selftest', 'render', 'font', 'ui', 'main_host')) {
    $obj = Join-Path $bldDir "host_$f.obj"
    $ccArgs = @('cc') + $commonCFlags + $hostInc + @('-O2', '-c', "$srcDir\$f.c", '-o', "$obj")
if ((Invoke-Native $tc.Zig $ccArgs) -ne 0) { Fail "宿主机编译失败：$f.c" }
    if ($LASTEXITCODE -ne 0) { Fail "宿主机编译失败：$f.c" }
    $hostObjs += $obj
}
$hostExe = Join-Path $bldDir 'cs_android_host.exe'
if ((Invoke-Native $tc.Zig (@('cc') + $hostObjs + @('-O2', '-o', "$hostExe"))) -ne 0) { Fail '宿主机链接失败' }
if ($LASTEXITCODE -ne 0) { Fail '宿主机链接失败' }
Say "  $hostExe"

if (-not $SkipTests) {
    $st = Join-Path $bldDir 'selftest.txt'
    $ut = Join-Path $bldDir 'uitest.txt'
    $dump = Join-Path $bldDir 'rules_dump.txt'
    & $hostExe --selftest $st --uitest $ut --rules-dump $dump
    if ($LASTEXITCODE -ne 0) {
        Write-Host (Get-Content -LiteralPath $st -Encoding UTF8 -Raw)
        Write-Host (Get-Content -LiteralPath $ut -Encoding UTF8 -Raw)
        Fail '宿主机自检失败'
    }
    Get-Content -LiteralPath $st -Encoding UTF8 | Select-Object -Last 3
    Get-Content -LiteralPath $ut -Encoding UTF8 | Select-Object -Last 3

    # 跨端规则对照：安卓版这份 C 规则必须和 Windows 版那份 Zig 规则
    # 在同种子下给出同一个局面。
    $winExe = Join-Path $root '..\正式版\复扫雷.exe'
    if (Test-Path -LiteralPath $winExe) {
        Invoke-Native 'node' @((Join-Path $root 'tools\parity_check.js'), $winExe, $hostExe, $bldDir) | Out-Null
        if ($LASTEXITCODE -ne 0) { Fail '跨端规则对照失败：C 规则与 Zig 规则结果不一致' }
    }
    else {
        Write-Host "  跳过跨端对照（还没构建 Windows 版）。先跑一次 正式版/build.ps1 就能带上这一项。" -ForegroundColor Yellow
    }
}

# ------------------------------------------------------------------ 4 原生库
Say "`n[4/6] 编译安卓原生库（$Abis）…"
$abiList = $Abis.Split(',') | ForEach-Object { $_.Trim() } | Where-Object { $_ }
$jniLibs = Join-Path $bldDir 'jni'
Remove-Item -LiteralPath $jniLibs -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $jniLibs | Out-Null

# native_app_glue 每次构建都从 NDK 源码重编：不往仓库里放第三方源码，
# 也保证用的就是当前 NDK 自带的那一份。
foreach ($abi in $abiList) {
    $target = Get-AbiTarget $abi
    $abiOut = Join-Path $jniLibs $abi
    New-Item -ItemType Directory -Force -Path $abiOut | Out-Null
    Say "  $abi ($target)"

    $glueObj = Join-Path $bldDir "glue_$abi.o"
    # 同样：带引号的 "-I$dir" 才不会被空格拆开（见上面工具的注释）
    $glueArgs = @('-target', $target, '-O2', '-fPIC', "-I$($tc.GlueInc)", '-c', "$($tc.Glue)", '-o', "$glueObj")
if ((Invoke-Native $tc.Clang $glueArgs) -ne 0) { Fail "native_app_glue 编译失败（$abi）" }
    if ($LASTEXITCODE -ne 0) { Fail "native_app_glue 编译失败（$abi）" }

    $ndkInc = @("-I$srcDir", "-I$bldDir", "-I$($tc.GlueInc)")
    $objs = @()
    foreach ($f in @('game', 'selftest', 'render', 'font', 'ui', 'strbuf', 'platform_android')) {
        $obj = Join-Path $bldDir "ndk_$($abi)_$f.o"
        $ccArgs = @('-target', $target) + $commonCFlags + $ndkInc + @('-O2', '-fPIC', '-c', "$srcDir\$f.c", '-o', "$obj")
if ((Invoke-Native $tc.Clang $ccArgs) -ne 0) { Fail "安卓编译失败：$f.c（$abi）" }
        if ($LASTEXITCODE -ne 0) { Fail "安卓编译失败：$f.c（$abi）" }
        $objs += $obj
    }
    $so = Join-Path $abiOut 'libcomplexsweeper.so'
    $soLinkArgs = @('-target', $target, '-shared', '-o', "$so") + $objs + @($glueObj, '-landroid', '-llog', '-lEGL', '-lGLESv2', '-lm')
if ((Invoke-Native $tc.Clang $soLinkArgs) -ne 0) { Fail "链接 libcomplexsweeper.so 失败（$abi）" }
    if ($LASTEXITCODE -ne 0) { Fail "链接 libcomplexsweeper.so 失败（$abi）" }
    $size = (Get-Item -LiteralPath $so).Length
    Say ("    libcomplexsweeper.so  {0:N0} 字节" -f $size)
}

# ------------------------------------------------------------------ 5 打包
Say "`n[5/6] 打包 APK…"
# 图标从图集里切（与 Windows 版同一份素材，不另外维护一份图）
Invoke-Native 'node' @((Join-Path $root 'tools\gen_icon.js')) -QuietOutput | Out-Null
if ($LASTEXITCODE -ne 0) { Fail '生成图标失败' }

$resDir = Join-Path $root 'res'
# 版本号与 versionCode 都从 APP_VERSION 推出来，注入到一份临时 manifest 里，
# 免得 manifest 里的 versionName/versionCode 与程序里报的版本号对不上。
$verParts = [regex]::Match($version, '^(\d+)\.(\d+)\.(\d+)$')
if (-not $verParts.Success) { Fail "版本号格式不认识：$version（要 x.y.z）" }
$versionCode = [int]$verParts.Groups[1].Value * 10000 +
               [int]$verParts.Groups[2].Value * 100 +
               [int]$verParts.Groups[3].Value
$genManifest = Join-Path $bldDir 'AndroidManifest.xml'
$manifestText = [System.IO.File]::ReadAllText((Join-Path $root 'AndroidManifest.xml'), [System.Text.Encoding]::UTF8)
$manifestText = [regex]::Replace($manifestText, 'android:versionCode="\d+"', "android:versionCode=`"$versionCode`"")
$manifestText = [regex]::Replace($manifestText, 'android:versionName="[^"]*"', "android:versionName=`"$version`"")
# -PackageName：换一个包名构建。用途：模拟器里某个包名留下了坏掉的安装记录
# （装上去 pm path 查不到、卸载报 DELETE_FAILED_INTERNAL_ERROR），
# 换个包名就能继续验证，同时**不改**源码里的 AndroidManifest.xml。
if ($PackageName) {
    $manifestText = [regex]::Replace($manifestText, '(?<=package=")[^"]*', $PackageName)
}
[System.IO.File]::WriteAllText($genManifest, $manifestText, (New-Object System.Text.UTF8Encoding($false)))
$manifest = $genManifest
$resZip = Join-Path $bldDir 'res.zip'
Remove-Item -LiteralPath $resZip -Force -ErrorAction SilentlyContinue

if ((Invoke-Native $tc.Aapt2 @('compile', '--dir', $resDir, '-o', $resZip)) -ne 0) { Fail 'aapt2 compile 失败' }


$unsigned = Join-Path $bldDir 'unsigned.apk'
Remove-Item -LiteralPath $unsigned -Force -ErrorAction SilentlyContinue
# aapt2 link 的 -A：把 assets/ 整个打包进去（atlas.bin 与 font_atlas.bin 都在那里）。
# -0 .so：**必须**带上。不带的话 .so 会被 deflate 压缩，而 Android 是直接从 APK 里
#   mmap 这些库的，压缩过的库在老系统上装不上（check_apk.js 专门查这一条）。
$aaptLinkArgs = @('link', '-o', $unsigned, '-I', $tc.AndroidJar, '--auto-add-overlay',
    '--manifest', $manifest, '-R', $resZip, '-A', $assetsDir, '-0', '.so',
    '--min-sdk-version', '24', '--target-sdk-version', '35',
    '--version-code', "$versionCode", '--version-name', "$version")
if ((Invoke-Native $tc.Aapt2 $aaptLinkArgs) -ne 0) { Fail 'aapt2 link 失败' }







# 把原生库补进 APK，并且**以 stored（不压缩）方式**写：
# Android 直接从 APK 里 mmap 这些 .so，压缩过的库在老系统上装不上。
# 用 tools/pack_apk.js 自己拼 ZIP：Windows PowerShell 5.1 的
# System.IO.Compression 实测无法产出 stored 条目（NoCompression 被忽略，方法位仍是 8）。
$packed = Join-Path $bldDir 'packed.apk'
$soSpecs = @()
foreach ($abi in $abiList) {
    $so = Join-Path (Join-Path $jniLibs $abi) 'libcomplexsweeper.so'
    $soSpecs += "$abi=$so"
}
$packArgs = @((Join-Path $root 'tools\pack_apk.js'), $unsigned, $packed) + $soSpecs
if ((Invoke-Native 'node' $packArgs) -ne 0) { Fail '打包原生库失败' }

$aligned = Join-Path $bldDir 'aligned.apk'
Remove-Item -LiteralPath $aligned -Force -ErrorAction SilentlyContinue
if ((Invoke-Native $tc.Zipalign @('-f', '4', $packed, $aligned)) -ne 0) { Fail 'zipalign 失败' }

# 签名：用本地调试密钥（自用安装够；要发布请换成你自己的密钥）
$ks = Join-Path $root 'debug.keystore'
if (-not (Test-Path -LiteralPath $ks)) {
    Say "  生成调试密钥 $ks"
    $keytool = $tc.Keytool
    if (-not $keytool) { $keytool = 'keytool' }
    # 统一走 Invoke-Native：它会替我们处理引号与 stderr。
    # （-dname 的值里有空格，不能让 PowerShell 自己拆参数）
    $ktArgs = @('-genkeypair', '-keystore', $ks, '-alias', 'androiddebugkey',
        '-storepass', 'android', '-keypass', 'android', '-keyalg', 'RSA', '-keysize', '2048',
        '-validity', '10000', '-dname', 'CN=Android Debug,O=Android,C=US')
    if ((Invoke-Native $keytool $ktArgs -QuietOutput) -ne 0) { Fail '生成调试密钥失败' }
    if (-not (Test-Path -LiteralPath $ks)) { Fail "keytool 报成功但没生成 $ks" }
}
$apk = Join-Path $bldDir "复扫雷-android-$version.apk"
Remove-Item -LiteralPath $apk -Force -ErrorAction SilentlyContinue
$java = $tc.Java
if (-not $java) { $java = 'java' }
if ((Invoke-Native $java @('-jar', $tc.ApkSignerJar, 'sign', '--ks', $ks, '--ks-pass', 'pass:android', '--key-pass', 'pass:android', '--v1-signing-enabled', 'true', '--v2-signing-enabled', 'true', '--out', $apk, $aligned)) -ne 0) { Fail 'apksigner 签名失败' }




# ------------------------------------------------------------------ 6 校验
Say "`n[6/6] 校验 APK…"
Invoke-Native 'node' @((Join-Path $root 'tools\check_apk.js'), $apk, $tc.Aapt2, $tc.ApkSignerJar, $java) | Out-Null
if ($LASTEXITCODE -ne 0) { Fail 'APK 结构校验失败' }

# 预览图（人工核对观感用；自检里已经有渲染断言，这里是给人看的）
if (-not $SkipTests) {
    $shots = Join-Path $bldDir 'shots'
    New-Item -ItemType Directory -Force -Path $shots | Out-Null
    foreach ($d in @('mid', 'lose', 'win', 'custom', 'flags')) {
        & $hostExe --shot (Join-Path $shots "demo-$d.png") --demo $d --zoom 2 --scale $Scale | Out-Null
    }
    foreach ($o in @('menu', 'custom', 'best', 'help', 'about')) {
        & $hostExe --shot (Join-Path $shots "overlay-$o.png") --demo mid --zoom 2 --scale $Scale --overlay $o | Out-Null
    }
}

$size = (Get-Item -LiteralPath $apk).Length
$sha = (Get-FileHash -LiteralPath $apk -Algorithm SHA256).Hash
Write-Host ""
Write-Host ("完成：{0}" -f $apk)
Write-Host ("      {0:N0} 字节（{1:N0} KB）  ABI: {2}" -f $size, ($size / 1KB), ($abiList -join ', '))
Write-Host ("      SHA-256 {0}" -f $sha)
Write-Host ""
Write-Host "装到手机上："
Write-Host ("  adb install -r `"{0}`"" -f $apk)
Write-Host "装上后如果启动就闪退，看日志里我们自己的标签："
Write-Host "  adb logcat -s complexsweeper:V AndroidRuntime:E"
