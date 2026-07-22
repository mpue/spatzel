<#
.SYNOPSIS
    Windows build script for fitzel — configures once and builds Debug and/or Release.

.DESCRIPTION
    The project is generated with a Visual Studio (multi-config) generator, so a
    single build tree serves every configuration; only --config differs per build.

.EXAMPLE
    .\build.ps1                     # Debug + Release
    .\build.ps1 -Config Debug       # Debug only
    .\build.ps1 -Clean              # wipe the build tree first
    .\build.ps1 -NoVulkan           # OpenGL backend only
#>
[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release', 'Both')]
    [string]$Config = 'Both',

    [string]$BuildDir = 'build',

    # Empty means "whatever CMake picks by default" — normally the newest
    # installed Visual Studio, which is what this project expects.
    [string]$Generator = '',

    [ValidateSet('x64', 'Win32', 'ARM64')]
    [string]$Platform = 'x64',

    # Parallel compile jobs; 0 lets CMake pick.
    [int]$Jobs = 0,

    # Explicit path to cmake.exe; by default it is looked up (PATH, then the
    # usual install locations).
    [string]$CMake,

    # Explicit path to glslc.exe / glslangValidator.exe, if CMake cannot find one.
    [string]$GlslCompiler,

    [switch]$Clean,
    [switch]$NoVulkan,
    [switch]$NoOpenGL
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$root = $PSScriptRoot
if (-not [System.IO.Path]::IsPathRooted($BuildDir)) {
    $BuildDir = Join-Path $root $BuildDir
}

function Resolve-CMake {
    param([string]$Explicit)

    if ($Explicit) {
        if (-not (Test-Path $Explicit)) { throw "cmake not found at '$Explicit'." }
        return (Resolve-Path $Explicit).Path
    }

    $onPath = Get-Command cmake.exe -ErrorAction SilentlyContinue
    if ($onPath) { return $onPath.Source }

    # CMake is frequently installed without being put on PATH, and the copy that
    # ships inside Visual Studio never is.
    $candidates = @(
        "$env:ProgramFiles\CMake\bin\cmake.exe"
        "${env:ProgramFiles(x86)}\CMake\bin\cmake.exe"
    )
    $candidates += Get-ChildItem `
        -Path "$env:ProgramFiles\Microsoft Visual Studio", "${env:ProgramFiles(x86)}\Microsoft Visual Studio" `
        -Filter cmake.exe -Recurse -ErrorAction SilentlyContinue |
        Select-Object -ExpandProperty FullName

    foreach ($c in $candidates) {
        if ($c -and (Test-Path $c)) { return $c }
    }

    throw "cmake.exe was not found. Install CMake 3.24+, pass -CMake <path>, or use a Visual Studio Developer PowerShell."
}

$cmakeExe = Resolve-CMake -Explicit $CMake
Write-Host "Using $cmakeExe" -ForegroundColor DarkGray

function Invoke-Step {
    param([string]$Label, [string[]]$Arguments)

    Write-Host ""
    Write-Host "==> $Label" -ForegroundColor Cyan
    Write-Host "    cmake $($Arguments -join ' ')" -ForegroundColor DarkGray

    & $cmakeExe @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "$Label failed (exit code $LASTEXITCODE)."
    }
}

if ($Clean -and (Test-Path $BuildDir)) {
    Write-Host "==> Removing $BuildDir" -ForegroundColor Cyan
    Remove-Item -Recurse -Force $BuildDir
}

# FetchContent clones its dependencies with git. On volumes that do not record
# ownership (exFAT, some network/virtual disks) git refuses to touch the fresh
# clone with "dubious ownership" and the populate step fails. Relax it for this
# process only — GIT_CONFIG_* is scoped to the child processes we spawn and
# never touches the user's global config.
if (-not $env:GIT_CONFIG_COUNT) {
    $env:GIT_CONFIG_COUNT    = '1'
    $env:GIT_CONFIG_KEY_0    = 'safe.directory'
    $env:GIT_CONFIG_VALUE_0  = '*'
}

# ---------------------------------------------------------------------------
# Configure
#
# Re-running configure on an existing tree is cheap and picks up new sources,
# so it is unconditional. The generator can only be set on a fresh tree.
# ---------------------------------------------------------------------------
$configureArgs = @('-S', $root, '-B', $BuildDir)

$cacheFile = Join-Path $BuildDir 'CMakeCache.txt'
$freshTree = -not (Test-Path $cacheFile)

# A cache records the absolute path of the Visual Studio instance it was
# generated against. When VS is upgraded or replaced (Community -> Insiders,
# say) that path goes away and configure fails in a way no in-place re-run can
# fix — the tree has to be regenerated.
if (-not $freshTree) {
    $instance = Select-String -Path $cacheFile -Pattern '^CMAKE_GENERATOR_INSTANCE:INTERNAL=(.+)$' |
        Select-Object -First 1
    if ($instance) {
        $instancePath = $instance.Matches[0].Groups[1].Value
        if ($instancePath -and -not (Test-Path $instancePath)) {
            Write-Host ""
            Write-Host "Stale build tree: it was generated against a Visual Studio that no longer exists" -ForegroundColor Yellow
            Write-Host "  $instancePath" -ForegroundColor Yellow
            Write-Host "Re-run with -Clean to regenerate against the currently installed Visual Studio." -ForegroundColor Yellow
            Write-Host "(This discards the downloaded dependencies in $BuildDir\_deps.)" -ForegroundColor Yellow
            exit 1
        }
    }
}

if ($freshTree) {
    if ($Generator) { $configureArgs += @('-G', $Generator) }
    $configureArgs += @('-A', $Platform)
}

$configureArgs += "-DFITZEL_WITH_VULKAN:BOOL=$(if ($NoVulkan) { 'OFF' } else { 'ON' })"
$configureArgs += "-DFITZEL_WITH_OPENGL:BOOL=$(if ($NoOpenGL) { 'OFF' } else { 'ON' })"

# The shader step needs a GLSL compiler for both backends, so a missing Vulkan
# SDK stops the configure outright. Say so before CMake spends minutes cloning
# dependencies first.
if ($GlslCompiler) {
    if (-not (Test-Path $GlslCompiler)) { throw "GLSL compiler not found at '$GlslCompiler'." }
    $configureArgs += "-DFITZEL_GLSL_COMPILER:FILEPATH=$((Resolve-Path $GlslCompiler).Path)"
} elseif ($freshTree -and
          -not (Get-Command glslc.exe, glslangValidator.exe -ErrorAction SilentlyContinue) -and
          -not ($env:VULKAN_SDK -and (Test-Path $env:VULKAN_SDK))) {
    Write-Host ""
    Write-Host "Warning: no glslc/glslangValidator on PATH and VULKAN_SDK is unset." -ForegroundColor Yellow
    Write-Host "Configure will fail at the shader step. Install the Vulkan SDK, or pass -GlslCompiler <path>." -ForegroundColor Yellow
}

try {
    Invoke-Step -Label 'Configure' -Arguments $configureArgs
} catch {
    if (-not $freshTree) {
        # A cache that names a generator, toolchain or VS instance that no longer
        # exists cannot be repaired in place.
        Write-Host ""
        Write-Host "Configure failed on the existing tree in '$BuildDir'." -ForegroundColor Yellow
        Write-Host "If the cache is stale (Visual Studio updated or moved), re-run with -Clean." -ForegroundColor Yellow
    }
    throw
}

# ---------------------------------------------------------------------------
# Build
# ---------------------------------------------------------------------------
$configs = if ($Config -eq 'Both') { @('Debug', 'Release') } else { @($Config) }

foreach ($cfg in $configs) {
    $buildArgs = @('--build', $BuildDir, '--config', $cfg)
    if ($Jobs -gt 0) {
        $buildArgs += @('--parallel', "$Jobs")
    } else {
        $buildArgs += '--parallel'
    }

    Invoke-Step -Label "Build $cfg" -Arguments $buildArgs
}

Write-Host ""
Write-Host "Build finished." -ForegroundColor Green
foreach ($cfg in $configs) {
    $binDir = Join-Path $BuildDir "bin\$cfg"
    if (Test-Path $binDir) {
        Get-ChildItem $binDir -Filter *.exe | ForEach-Object {
            Write-Host ("  {0,-8} {1}" -f $cfg, $_.FullName)
        }
    }
}
