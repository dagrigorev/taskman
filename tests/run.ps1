<# Build and run all ten CTest suites with the selected application compiler. #>
[CmdletBinding()]
param(
    [ValidateSet('auto', 'msvc', 'mingw')]
    [string] $Toolchain = 'auto',
    [ValidateSet('Release', 'Debug')]
    [string] $Configuration = 'Release',
    [switch] $Strict
)
$ErrorActionPreference = 'Stop'
$driver = Join-Path (Split-Path $PSScriptRoot -Parent) 'build.ps1'
& $driver -Target test -Toolchain $Toolchain -Configuration $Configuration -Strict:$Strict -Verbose:($VerbosePreference -eq 'Continue')
exit $LASTEXITCODE
