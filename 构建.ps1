param(
    [string]$QtRoot = 'C:\Qt\6.5.3\mingw_64',
    [string]$CompilerRoot = 'C:\Qt\Tools\mingw1120_64',
    [string]$CMake = 'C:\Qt\Tools\CMake_64\bin\cmake.exe',
    [string]$Ninja = 'C:\Qt\Tools\Ninja\ninja.exe',
    [switch]$SkipPackage
)
$ErrorActionPreference = 'Stop'
$projectRoot = $PSScriptRoot
$buildRoot = Join-Path $projectRoot 'build'
$env:PATH = "$CompilerRoot\bin;$QtRoot\bin;$env:PATH"
& $CMake -S $projectRoot -B $buildRoot -G Ninja -DCMAKE_BUILD_TYPE=Release "-DCMAKE_PREFIX_PATH=$QtRoot" "-DCMAKE_MAKE_PROGRAM=$Ninja" "-DCMAKE_CXX_COMPILER=$CompilerRoot/bin/g++.exe"
if ($LASTEXITCODE -ne 0) { throw 'CMake 配置失败' }
& $CMake --build $buildRoot -j 4
if ($LASTEXITCODE -ne 0) { throw '编译失败' }
$oldQtPlatform = $env:QT_QPA_PLATFORM
try {
    $env:QT_QPA_PLATFORM = 'offscreen'
    & (Join-Path (Split-Path $CMake) 'ctest.exe') --test-dir $buildRoot --output-on-failure
    if ($LASTEXITCODE -ne 0) { throw '离线测试失败' }
} finally { $env:QT_QPA_PLATFORM = $oldQtPlatform }
if ($SkipPackage) { return }
$qtSource = Join-Path $projectRoot 'third_party\qt-source\qtbase-everywhere-src-6.5.3.tar.xz'
if (!(Test-Path -LiteralPath $qtSource)) { $qtSource = Join-Path $projectRoot '..\Qt\qtbase-everywhere-src-6.5.3.tar.xz' }
if (!(Test-Path -LiteralPath $qtSource)) { throw '缺少配套 Qt 源码，暂不生成可分发包。请读取 third_party/qt-source 中的来源说明。' }
$expectedQtHash = 'DF2F4A230BE4EA04F9798F2C19AB1413A3B8EC6A80BEF359F50284235307B546'
if ((Get-FileHash -LiteralPath $qtSource -Algorithm SHA256).Hash -ne $expectedQtHash) { throw 'Qt 源码 SHA-256 不符' }
$driverRoot = Join-Path $projectRoot 'third_party\drivers'
if (!(Test-Path -LiteralPath (Join-Path $driverRoot 'ni-visa_26.8_online.exe'))) { $driverRoot = Join-Path $projectRoot '..\..\drivers' }
$driverInstaller = Join-Path $driverRoot 'ni-visa_26.8_online.exe'
if (!(Test-Path -LiteralPath $driverInstaller)) { throw '缺少NI官方在线安装器，暂不生成含驱动入口的分发包。' }
if ((Get-FileHash -LiteralPath $driverInstaller).Hash -ne '00E3A6FE2D9609972983F44812FD33B468935480F82D6A98F27D4DD590B58E0F') { throw 'NI安装器SHA-256不符' }
if ((Get-AuthenticodeSignature -LiteralPath $driverInstaller).Status -ne 'Valid') { throw 'NI安装器签名无效' }
$version = '0.2.0'
$packageName = "ScopeSnap-$version-Windows11-x64"
$distRoot = Join-Path $projectRoot 'dist'
$packageRoot = Join-Path $distRoot $packageName
if (Test-Path -LiteralPath $packageRoot) { $packageRoot = Join-Path $distRoot ($packageName + '-' + (Get-Date -Format 'yyyyMMdd-HHmmss')) }
New-Item -ItemType Directory -Path $packageRoot,(Join-Path $packageRoot 'platforms'),(Join-Path $packageRoot '源码\ScopeSnap'),(Join-Path $packageRoot '源码\Qt') -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $buildRoot 'ScopeCapture.exe') -Destination (Join-Path $packageRoot 'ScopeSnap.exe')
# 固定模块清单：Qt Widgets/Fusion + Qt Network，PNG解码内置QtGui。
# 本机 windeployqt 曾误判 MinGW 插件为 Debug，显式复制当前 Release 套件。
foreach ($name in @('Qt6Core.dll','Qt6Gui.dll','Qt6Widgets.dll','Qt6Network.dll')) {
    Copy-Item -LiteralPath (Join-Path "$QtRoot\bin" $name) -Destination $packageRoot
}
foreach ($name in @('libgcc_s_seh-1.dll','libstdc++-6.dll','libwinpthread-1.dll')) {
    Copy-Item -LiteralPath (Join-Path "$CompilerRoot\bin" $name) -Destination $packageRoot
}
Copy-Item -LiteralPath "$QtRoot\plugins\platforms\qwindows.dll" -Destination (Join-Path $packageRoot 'platforms')
Set-Content -LiteralPath (Join-Path $packageRoot 'qt.conf') -Encoding ASCII -Value "[Paths]`r`nPlugins=."
Copy-Item -LiteralPath (Join-Path $projectRoot 'resources\licenses') -Destination (Join-Path $packageRoot 'licenses') -Recurse
New-Item -ItemType Directory -Path (Join-Path $packageRoot 'drivers') -Force | Out-Null
Get-ChildItem -LiteralPath $driverRoot -File | Where-Object { $_.Name -eq 'ni-visa_26.8_online.exe' -or $_.Extension -in @('.txt','.md','.pdf','.rtf') } | ForEach-Object {
    Copy-Item -LiteralPath $_.FullName -Destination (Join-Path $packageRoot 'drivers')
}
Copy-Item -LiteralPath (Join-Path $projectRoot '便携包说明.md') -Destination (Join-Path $packageRoot 'README.md')
foreach ($name in @('LICENSE','使用说明.md')) {
    Copy-Item -LiteralPath (Join-Path $projectRoot $name) -Destination $packageRoot
}
foreach ($name in @('src','tests','resources','CMakeLists.txt','构建.ps1','README.md','LICENSE','使用说明.md','便携包说明.md','.gitignore','.gitattributes')) {
    Copy-Item -LiteralPath (Join-Path $projectRoot $name) -Destination (Join-Path $packageRoot '源码\ScopeSnap') -Recurse
}
foreach ($group in @('drivers','qt-source')) {
    $sourceMetadata = Join-Path $projectRoot "third_party\$group"
    $targetMetadata = Join-Path $packageRoot "源码\ScopeSnap\third_party\$group"
    New-Item -ItemType Directory -Path $targetMetadata -Force | Out-Null
    Get-ChildItem -LiteralPath $sourceMetadata -File | Where-Object { $_.Extension -in @('.md','.txt','.pdf','.rtf','.sha256','.json') } | ForEach-Object {
        Copy-Item -LiteralPath $_.FullName -Destination $targetMetadata
    }
}
Copy-Item -LiteralPath $qtSource -Destination (Join-Path $packageRoot '源码\Qt\qtbase-everywhere-src-6.5.3.tar.xz')
Get-ChildItem -LiteralPath (Split-Path $qtSource) -File | Where-Object { $_.Extension -in @('.md','.txt','.sha256','.json') } | ForEach-Object {
    Copy-Item -LiteralPath $_.FullName -Destination (Join-Path $packageRoot '源码\Qt')
}
$zipPath = "$packageRoot.zip"
Compress-Archive -LiteralPath $packageRoot -DestinationPath $zipPath -CompressionLevel Optimal
Get-FileHash -LiteralPath (Join-Path $packageRoot 'ScopeSnap.exe'),$zipPath -Algorithm SHA256 | Format-Table -AutoSize
Write-Output "分发目录：$packageRoot"
Write-Output "完整压缩包：$zipPath"
