<#
.SYNOPSIS
  Registers the packaged UWP test app from a loose layout, runs it inside its
  AppContainer, prints its report and unregisters it again.

.DESCRIPTION
  Requires Windows Developer Mode (loose-layout registration of an unsigned
  package). The layout is staged on an NTFS volume because Windows refuses to
  deploy packages from exFAT/FAT volumes (HRESULT 0x80073CFD).

  Runs for the same package name are serialized across processes, so Debug
  and Release can be tested at the same time.

  Exit code: the test app's exit code (0 = pass), or 1 if the harness fails.
  Compatible with Windows PowerShell 5.1.
#>
[CmdletBinding()]
param(
    # Per-configuration package output directory (AppxPackageDir).
    [Parameter(Mandatory = $true)] [string] $AppPackagesDir,
    [Parameter(Mandatory = $true)] [string] $Configuration,
    [Parameter(Mandatory = $true)] [string] $PackageName,
    [Parameter(Mandatory = $true)] [string] $Alias,
    [Parameter(Mandatory = $true)] [string] $MakeAppx,
    [string] $ReportName = 'chromaroute-uwp-tests.txt',
    [string] $StagingRoot = (Join-Path $env:LOCALAPPDATA 'LibChromaRoute\uwp-staging'),
    # How long to wait for another run of the same package. Keep it below the
    # CTest TIMEOUT of uwp_runtime_tests (tests/platform/uwp/CMakeLists.txt).
    [int] $LockTimeoutSeconds = 300,
    [switch] $KeepRegistered
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Unregister-TestPackage {
    Get-AppxPackage -Name $PackageName | Remove-AppxPackage
}

# Every run registers the same package identity and alias, so runs from
# different build trees (Debug and Release at once, say) would unregister each
# other's package. A named mutex serializes the whole register/run/unregister
# cycle across processes; only its holder may unregister. Registration is per
# user in every logon session, so the mutex is global and keyed by the user.
$userSid = [System.Security.Principal.WindowsIdentity]::GetCurrent().User.Value
$lock = New-Object System.Threading.Mutex($false, "Global\LibChromaRoute.UwpTestPackage.$userSid.$PackageName")
$locked = $false
try {
    try {
        $locked = $lock.WaitOne([TimeSpan]::FromSeconds($LockTimeoutSeconds))
    }
    catch [System.Threading.AbandonedMutexException] {
        $locked = $true # the previous holder exited without releasing it; it is ours now
    }
    if (-not $locked) {
        throw "Timed out after $LockTimeoutSeconds s waiting for another UWP test run of $PackageName to finish."
    }

    $package = $null
    if (Test-Path -LiteralPath $AppPackagesDir) {
        $package = Get-ChildItem -LiteralPath $AppPackagesDir -Recurse -File |
            Where-Object { $_.Extension -eq '.msix' -or $_.Extension -eq '.appx' } |
            Sort-Object LastWriteTime -Descending |
            Select-Object -First 1
    }
    if (-not $package) {
        throw "No '$Configuration' package found under $AppPackagesDir; build the chromaroute_uwp_tests target first."
    }

    New-Item -ItemType Directory -Force -Path $StagingRoot | Out-Null
    $volumeRoot = [System.IO.Path]::GetPathRoot((Resolve-Path -LiteralPath $StagingRoot).Path)
    $fileSystem = (New-Object System.IO.DriveInfo($volumeRoot)).DriveFormat
    if ($fileSystem -ne 'NTFS') {
        throw "Staging root $StagingRoot is on $fileSystem; UWP packages can only be registered from NTFS. Pass -StagingRoot."
    }

    Unregister-TestPackage
    $layout = Join-Path $StagingRoot "$PackageName-$Configuration"
    if (Test-Path -LiteralPath $layout) {
        Remove-Item -LiteralPath $layout -Recurse -Force
    }
    & $MakeAppx unpack /p $package.FullName /d $layout /o | Out-Null
    if ($LASTEXITCODE -ne 0) {
        throw "makeappx unpack failed with exit code $LASTEXITCODE"
    }
    # Package-only metadata; a registered loose layout must not contain it.
    foreach ($name in @('AppxSignature.p7x', 'AppxBlockMap.xml', '[Content_Types].xml')) {
        $path = Join-Path $layout $name
        if (Test-Path -LiteralPath $path) {
            Remove-Item -LiteralPath $path -Force
        }
    }

    Add-AppxPackage -Register (Join-Path $layout 'AppxManifest.xml') -ForceUpdateFromAnyVersion
    $registered = Get-AppxPackage -Name $PackageName
    if (-not $registered) {
        throw "Package $PackageName did not register."
    }
    Write-Host "Registered $($registered.PackageFullName) from $layout"

    $report = Join-Path $env:LOCALAPPDATA "Packages\$($registered.PackageFamilyName)\AC\Temp\$ReportName"
    if (Test-Path -LiteralPath $report) {
        Remove-Item -LiteralPath $report -Force
    }

    $aliasPath = Join-Path $env:LOCALAPPDATA "Microsoft\WindowsApps\$Alias"
    & $aliasPath --report $ReportName
    $exitCode = $LASTEXITCODE

    if (-not (Test-Path -LiteralPath $report)) {
        throw "The test app exited with code $exitCode without writing $report"
    }
    Get-Content -LiteralPath $report | Write-Host
    Write-Host "UWP test app exit code: $exitCode"
}
catch {
    Write-Host "UWP test harness error: $($_.Exception.Message)"
    $exitCode = 1
}
finally {
    if ($locked) {
        if (-not $KeepRegistered) {
            try { Unregister-TestPackage } catch { Write-Host "Unregister failed: $($_.Exception.Message)" }
        }
        $lock.ReleaseMutex()
    }
    $lock.Dispose()
}

exit $exitCode
