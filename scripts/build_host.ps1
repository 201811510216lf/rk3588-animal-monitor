# build_host.ps1 — host 构建（Windows + VS2022 自带 CMake/Ninja + MSVC）
# 用法：powershell -ExecutionPolicy Bypass -File scripts\build_host.ps1 [-Clean] [-NoTests]
# host 构建不依赖 Rockchip SDK（ANIMAL_MONITOR_ENABLE_TARGET_DEPS=OFF）。

param(
    [switch]$Clean,
    [switch]$NoTests
)

$ErrorActionPreference = 'Stop'

$VsRoot = "D:\Program Files\Microsoft Visual Studio\2022\Professional"
$Cmake  = Join-Path $VsRoot "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
$Ninja  = Join-Path $VsRoot "Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"
$Vcvars = Join-Path $VsRoot "VC\Auxiliary\Build\vcvars64.bat"

foreach ($tool in @($Cmake, $Ninja, $Vcvars)) {
    if (-not (Test-Path $tool)) {
        Write-Error "缺少工具: $tool"
        exit 1
    }
}

$ProjectRoot = Split-Path -Parent $PSScriptRoot
$BuildDir    = Join-Path $ProjectRoot "build"

if ($Clean -and (Test-Path $BuildDir)) {
    Remove-Item -Recurse -Force $BuildDir
}

# 在 VS 开发环境中执行（确保 cl 可见）
$Command = "call `"$Vcvars`" >nul && `"$Cmake`" -S `"$ProjectRoot`" -B `"$BuildDir`" -G Ninja -DCMAKE_BUILD_TYPE=Release -DANIMAL_MONITOR_ENABLE_TARGET_DEPS=OFF && `"$Cmake`" --build `"$BuildDir`""
cmd /c $Command
if ($LASTEXITCODE -ne 0) {
    Write-Error "构建失败，退出码 $LASTEXITCODE"
    exit $LASTEXITCODE
}

if (-not $NoTests) {
    & $Cmake --build $BuildDir --target test
    if ($LASTEXITCODE -ne 0) {
        Write-Error "测试失败，退出码 $LASTEXITCODE"
        exit $LASTEXITCODE
    }
}

Write-Host ""
Write-Host "构建与测试完成。产物:"
Write-Host "  应用: build\bin\animal_monitor_app.exe"
Write-Host "  测试: ctest --test-dir `"$BuildDir`" --output-on-failure"
