<#
    build.ps1 - CMake front end for Classic Task Manager.

    Works from any shell. It does NOT need a "Native Tools Command Prompt":
    if MSVC is installed, the script locates it with vswhere and imports the
    compiler environment itself. If MSVC is absent it falls back to MinGW-w64.

        .\build.ps1                 release build -> build\taskman.exe
        .\build.ps1 rebuild         clean, then build
        .\build.ps1 debug           unoptimised build with symbols
        .\build.ps1 test            build and run all ten CTest suites
        .\build.ps1 run             build, then launch the executable
        .\build.ps1 clean           remove build\

        .\build.ps1 -Toolchain mingw    force gcc/windres
        .\build.ps1 -Toolchain msvc     force cl/rc (fails if not installed)
        .\build.ps1 -Strict             warnings become errors
        .\build.ps1 -Verbose            echo every compiler command line

    From cmd.exe (or any plain terminal) use the build.cmd shim instead:

        build            build test            build clean
#>
[CmdletBinding()]
param(
    [ValidateSet('build', 'rebuild', 'debug', 'test', 'run', 'clean')]
    [string] $Target = 'build',

    [ValidateSet('auto', 'msvc', 'mingw')]
    [string] $Toolchain = 'auto',

    [switch] $Strict,

    [ValidateSet('Release', 'Debug')]
    [string] $Configuration = 'Release'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$Root    = $PSScriptRoot
$BuildIn = Join-Path $Root 'build'
$Exe     = Join-Path $BuildIn 'taskman.exe'

# ---------------------------------------------------------------- helpers --

function Write-Step ([string] $Text) {
    Write-Host "==> $Text" -ForegroundColor Cyan
}

function Write-Good ([string] $Text) {
    Write-Host "    $Text" -ForegroundColor Green
}

function Invoke-Tool {
    param(
        [Parameter(Mandatory)] [string]   $Exe,
        [Parameter(Mandatory)] [string[]] $Arguments,
        [Parameter(Mandatory)] [string]   $What
    )
    Write-Verbose "$Exe $($Arguments -join ' ')"
    & $Exe @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "$What failed (exit $LASTEXITCODE)."
    }
}

# ------------------------------------------------------- MSVC environment --

# Import the x64 MSVC environment into this process so cl/rc/link resolve even
# when the script was started from an ordinary shell.
function Import-MsvcEnvironment {
    if (Get-Command cl.exe -ErrorAction SilentlyContinue) {
        Write-Verbose 'cl.exe already on PATH; keeping the current environment.'
        return $true
    }

    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path $vswhere)) { return $false }

    $install = & $vswhere -latest -products * `
                          -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
                          -property installationPath 2>$null | Select-Object -First 1
    if (-not $install) { return $false }

    $vcvars = Join-Path $install 'VC\Auxiliary\Build\vcvars64.bat'
    if (-not (Test-Path $vcvars)) { return $false }

    Write-Verbose "Importing MSVC environment from $vcvars"
    # `set` after the batch file gives us the resulting environment verbatim.
    $captured = cmd.exe /s /c "`"$vcvars`" >nul 2>&1 && set"
    if ($LASTEXITCODE -ne 0) { return $false }

    foreach ($line in $captured) {
        $split = $line.IndexOf('=')
        if ($split -gt 0) {
            $name = $line.Substring(0, $split)
            Set-Item -Path "Env:$name" -Value $line.Substring($split + 1)
        }
    }
    return [bool](Get-Command cl.exe -ErrorAction SilentlyContinue)
}

function Resolve-Toolchain {
    if ($Toolchain -eq 'msvc') {
        if (-not (Import-MsvcEnvironment)) {
            throw 'MSVC was requested but no x64 toolset was found. Install the "Desktop development with C++" workload, or use -Toolchain mingw.'
        }
        return 'msvc'
    }
    if ($Toolchain -eq 'mingw') {
        if (-not (Get-Command gcc.exe -ErrorAction SilentlyContinue)) {
            throw 'MinGW was requested but gcc.exe is not on PATH.'
        }
        return 'mingw'
    }
    if (Import-MsvcEnvironment) { return 'msvc' }
    if (Get-Command gcc.exe -ErrorAction SilentlyContinue) { return 'mingw' }
    throw 'No usable toolchain. Install Visual Studio with the C++ workload, or MinGW-w64 (gcc + windres) on PATH.'
}

# CMake owns all sources, flags, resources, libraries, and test definitions.
function Invoke-Build {
    foreach ($tool in @('cmake.exe', 'ctest.exe', 'ninja.exe')) {
        if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) {
            throw "$tool not found. Install CMake 3.25+ and Ninja, and add them to PATH."
        }
    }
    $chain = Resolve-Toolchain
    $config = if ($Target -eq 'debug') { 'Debug' } else { $Configuration }
    $preset = "$chain-$($config.ToLowerInvariant())"
    $strictValue = if ($Strict) { 'ON' } else { 'OFF' }
    Write-Step "configure $preset"
    Invoke-Tool cmake.exe @('--preset', $preset, "-DTASKMAN_STRICT=$strictValue", '-DBUILD_TESTING=ON') 'Configure'
    $arguments = @('--build', '--preset', $preset, '--parallel')
    if ($Target -ne 'test') { $arguments += @('--target', 'taskman') }
    if ($VerbosePreference -eq 'Continue') { $arguments += '--verbose' }
    Invoke-Tool cmake.exe $arguments 'Build'

    if ($Target -eq 'test') {
        Write-Step 'CTest'
        Invoke-Tool ctest.exe @('--preset', $preset) 'Tests'
    }
    # Keep the original entry point's executable path for existing shortcuts.
    $builtExe = Join-Path $BuildIn "$preset/bin/$config/taskman.exe"
    if (-not (Test-Path -LiteralPath $builtExe)) { throw "Missing output: $builtExe" }
    Copy-Item -LiteralPath $builtExe -Destination $Exe -Force
    Write-Good $Exe
    if ($Target -eq 'run') { Start-Process -FilePath $Exe }
}

function Invoke-Clean {
    Write-Step 'clean'
    $rootPath = [IO.Path]::GetFullPath($Root).TrimEnd('\', '/')
    foreach ($relative in @('build', 'tests/.build')) {
        $path = [IO.Path]::GetFullPath((Join-Path $Root $relative))
        if (-not $path.StartsWith($rootPath + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
            throw "Refusing to clean outside the project: $path"
        }
        if (Test-Path -LiteralPath $path) {
            # Do not recursively clean through a directory link or junction.
            $cursor = Get-Item -LiteralPath $path -Force
            while ($cursor.FullName -ne $rootPath) {
                if ($cursor.Attributes -band [IO.FileAttributes]::ReparsePoint) {
                    throw "Refusing to clean through a directory link: $($cursor.FullName)"
                }
                $cursor = $cursor.Parent
            }
            Remove-Item -LiteralPath $path -Recurse -Force
        }
    }
    Write-Good 'build/ and tests/.build/ removed'
}

Push-Location $Root
try {
    if ($Target -in @('clean', 'rebuild')) { Invoke-Clean }
    if ($Target -ne 'clean') { Invoke-Build }
}
catch {
    Write-Host "BUILD FAILED: $($_.Exception.Message)" -ForegroundColor Red
    exit 1
}
finally { Pop-Location }
exit 0
