param(
    [Parameter(Mandatory = $true)][string]$Executable,
    [Parameter(Mandatory = $true)][ValidateSet("MBR", "GPT")][string]$Style
)
$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest
$Executable = (Resolve-Path -LiteralPath $Executable).Path
$nonce = [guid]::NewGuid().ToString("N")
$vhd = Join-Path $env:RUNNER_TEMP "usbguard-ci-$Style-$nonce.vhdx"
$diskpartScript = Join-Path $env:RUNNER_TEMP "usbguard-ci-$nonce.txt"
$q = [char]34
try {
    @(
        "create vdisk file=$q$vhd$q maximum=256 type=expandable",
        "select vdisk file=$q$vhd$q",
        "attach vdisk"
    ) | Set-Content -LiteralPath $diskpartScript -Encoding ascii
    $output = & diskpart.exe /s $diskpartScript 2>&1 | Out-String
    Write-Host $output
    if ($LASTEXITCODE -ne 0 -or !(Test-Path -LiteralPath $vhd)) {
        throw "Unable to create the disposable VHD disk"
    }
    $mountedImage = Get-DiskImage -ImagePath $vhd -ErrorAction Stop
    if (!$mountedImage.Attached) { throw "VHD did not attach" }
    $disk = $mountedImage | Get-Disk -ErrorAction Stop
    if (@($disk).Count -ne 1) { throw "Expected exactly one VHD disk" }
    if ($disk.BusType.ToString() -notmatch 'Virtual|File') {
        throw "Refusing partition operation on unexpected bus type: $($disk.BusType)"
    }
    $systemLetter = $env:SystemDrive.Substring(0,1)
    $systemDisk = Get-Partition -DriveLetter $systemLetter | Get-Disk
    if ($disk.Number -eq $systemDisk.Number) { throw "Refusing to touch the Windows disk" }
    if ($disk.PartitionStyle -ne 'RAW') {
        throw "Refusing to initialize a VHD with preexisting partition table"
    }
    Initialize-Disk -Number $disk.Number -PartitionStyle $Style -ErrorAction Stop
    $partition = New-Partition -DiskNumber $disk.Number -UseMaximumSize -AssignDriveLetter -ErrorAction Stop
    if (!$partition.DriveLetter) { throw "VHD partition did not acquire a drive letter" }
    Format-Volume -Partition $partition -FileSystem NTFS -NewFileSystemLabel "USBGuardCI" -Confirm:$false -Force -ErrorAction Stop | Out-Null
    $drive = [string]$partition.DriveLetter
    Write-Host "Testing $Style virtual disk $($disk.Number), drive $drive with $Executable"
    & $Executable "$($disk.Number)" "$drive" "$Style"
    $exitCode = $LASTEXITCODE
    if ($exitCode -ne 0) {
        throw "Integration test returned $exitCode for $Style"
    }
}
finally {
    if (Test-Path -LiteralPath $vhd) {
        try { Dismount-DiskImage -ImagePath $vhd -ErrorAction Stop } catch {
            Write-Warning "VHD dismount failed: $_"
        }
        try { Remove-Item -LiteralPath $vhd -Force -ErrorAction Stop } catch {
            Write-Warning "VHD cleanup failed: $_"
        }
    }
    Remove-Item -LiteralPath $diskpartScript -Force -ErrorAction SilentlyContinue
}
