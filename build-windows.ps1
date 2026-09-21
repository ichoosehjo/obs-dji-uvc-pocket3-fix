# build-windows.ps1 — builds obs-dji-uvc against a local OBS checkout + obs-deps.
param(
    [string]$ObsDeps = "C:\obs-deps",
    [string]$ObsStudio = "",
    [string]$Config = "RelWithDebInfo"
)

$ErrorActionPreference = "Stop"
$root = $PSScriptRoot

# Manual obs-deps is optional: obs-studio's configure auto-downloads the real
# dependency bundle (FFmpeg, simde, ...) into <obs-studio>/.deps.
if ((Test-Path $ObsDeps) -and -not (Test-Path (Join-Path $ObsDeps "include"))) {
    $inner = Get-ChildItem -Path $ObsDeps -Directory | Where-Object { Test-Path (Join-Path $_.FullName "include") } | Select-Object -First 1
    if ($inner) { $ObsDeps = $inner.FullName }
}

if ($ObsStudio -eq "") {
    $ObsStudio = Join-Path $root "obs-studio"
}
if (-not (Test-Path $ObsStudio)) {
    # Pin to a release tag; shallow submodules break on pinned commits, so
    # the submodule fetch is full-depth (small repos, fast).
    git clone --depth 1 --branch 32.1.2 https://github.com/obsproject/obs-studio.git $ObsStudio
    if ($LASTEXITCODE -ne 0) { throw "obs-studio clone failed" }
    git -C $ObsStudio submodule update --init --recursive
    if ($LASTEXITCODE -ne 0) { throw "obs-studio submodule init failed" }
}

$obsBuild = Join-Path $ObsStudio "build"
if (-not (Test-Path (Join-Path $obsBuild "libobs"))) {
    cmake -S $ObsStudio -B $obsBuild -G "Visual Studio 17 2022" -A x64 `
        -DCMAKE_PREFIX_PATH="$ObsDeps" `
        -DOBS_VERSION_OVERRIDE="32.1.2" `
        -DENABLE_FRONTEND=OFF -DENABLE_UI=OFF -DENABLE_SCRIPTING=OFF `
        -DENABLE_BROWSER=OFF `
        -DCMAKE_INSTALL_PREFIX="$obsBuild\install"
    if ($LASTEXITCODE -ne 0) { throw "obs-studio configure failed" }
}
cmake --build $obsBuild --config $Config --target libobs
if ($LASTEXITCODE -ne 0) { throw "libobs build failed" }

# Try installing the cmake package; fall back to direct paths if unavailable.
cmake --install $obsBuild --config $Config --prefix "$obsBuild\install" 2>$null | Out-Null

$libobsConfig = Get-ChildItem -Path "$obsBuild\install" -Recurse -Filter "libobsConfig.cmake" -ErrorAction SilentlyContinue | Select-Object -First 1

# The real FFmpeg/simde live in obs-studio's auto-downloaded deps bundle.
$depsBundle = Get-ChildItem -Path (Join-Path $ObsStudio ".deps") -Directory -Filter "obs-deps-*" -ErrorAction SilentlyContinue |
    Where-Object { $_.Name -notmatch "qt6" } | Select-Object -First 1
if (-not $depsBundle) { throw "obs-deps prebuilt bundle not found under $ObsStudio\.deps" }
$prefixList = @($depsBundle.FullName)
if (Test-Path (Join-Path $ObsDeps "include")) { $prefixList += $ObsDeps }

$build = Join-Path $root "build"
$cmakeArgs = @(
    "-S", $root, "-B", $build,
    "-G", "Visual Studio 17 2022", "-A", "x64",
    "-DCMAKE_BUILD_TYPE=$Config"
)

if ($libobsConfig) {
    $prefixList += "$obsBuild\install"
    $cmakeArgs += "-DCMAKE_PREFIX_PATH=$($prefixList -join ';')"
} else {
    Write-Host "libobsConfig.cmake not found — using direct paths into the build tree"
    $obsLib = Get-ChildItem -Path $obsBuild -Recurse -Filter "obs.lib" | Select-Object -First 1
    if (-not $obsLib) { throw "obs.lib not found under $obsBuild" }
    $pthreadsLib = Get-ChildItem -Path $obsBuild -Recurse -Filter "w32-pthreads.lib" -ErrorAction SilentlyContinue | Select-Object -First 1
    $cmakeArgs += "-DCMAKE_PREFIX_PATH=$($prefixList -join ';')"
    $cmakeArgs += "-DLIBOBS_INCLUDE_DIR=$ObsStudio\libobs"
    $cmakeArgs += "-DLIBOBS_CONFIG_INCLUDE_DIR=$obsBuild\config"
    $cmakeArgs += "-DLIBOBS_LIB=$($obsLib.FullName)"
    if ($pthreadsLib) {
        $cmakeArgs += "-DLIBOBS_PTHREADS_LIB=$($pthreadsLib.FullName)"
        $cmakeArgs += "-DLIBOBS_PTHREADS_INCLUDE_DIR=$ObsStudio\deps\w32-pthreads"
    }
}

cmake @cmakeArgs
if ($LASTEXITCODE -ne 0) { throw "plugin configure failed" }
cmake --build $build --config $Config
if ($LASTEXITCODE -ne 0) { throw "plugin build failed" }

Write-Host ""
Write-Host "Output: $build\$Config\obs-dji-uvc.dll"
