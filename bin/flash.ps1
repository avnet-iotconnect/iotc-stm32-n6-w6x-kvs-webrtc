$ErrorActionPreference = "Stop"

$APP_NAME = "stm32n6570_dk_w6x_iot_reference"
# $BUIL_CONFIG="SW_Crypto"
$BUIL_CONFIG="HW_Crypto"

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot = Split-Path -Parent $scriptDir

$FSBL_BIN_FILE = Join-Path $repoRoot "FSBL\Release\${APP_NAME}_FSBL.bin"
$APP_BIN_FILE = Join-Path $repoRoot "Appli\${BUIL_CONFIG}\${APP_NAME}_Appli.bin"

Write-Output "Using FSBL binary: $FSBL_BIN_FILE"
Write-Output "Using Appli binary: $APP_BIN_FILE"

# Delay for 2 seconds to allow user to review the selected binaries before proceeding with flashing
Start-Sleep -Seconds 2

# NOR external flash start address: 0x70000000
$FLASH_FSBL_ADDRESS = "0x70000000"
$FLASH_APP_ADDRESS = "0x70100000"

# Remove previously signed binaries in the bin/ working directory
Remove-Item -Path (Join-Path $scriptDir "*.bin") -Force -ErrorAction SilentlyContinue

# Detect OS and set tool paths accordingly
$isLinuxHost = $false
$isMacOSHost = $false
$isWindowsHost = $false

if (Get-Variable -Name IsLinux -ErrorAction SilentlyContinue) { $isLinuxHost = [bool]$IsLinux }
if (Get-Variable -Name IsMacOS -ErrorAction SilentlyContinue) { $isMacOSHost = [bool]$IsMacOS }
if (Get-Variable -Name IsWindows -ErrorAction SilentlyContinue) { $isWindowsHost = [bool]$IsWindows }

# Windows PowerShell 5.1 fallback (where $IsWindows/$IsLinux/$IsMacOS do not exist)
if (-not $isWindowsHost -and -not $isLinuxHost -and -not $isMacOSHost -and $env:OS -eq "Windows_NT") {
    $isWindowsHost = $true
}

if ($isLinuxHost) {
    $PROGRAMMER = "$HOME/STMicroelectronics/STM32Cube/STM32CubeProgrammer20/bin/STM32_Programmer_CLI"
    $SIGNING_TOOL = "$HOME/STMicroelectronics/STM32Cube/STM32CubeProgrammer20/bin/STM32_SigningTool_CLI"
    $EXTERNAL_LOADER = "$HOME/STMicroelectronics/STM32Cube/STM32CubeProgrammer20/bin/ExternalLoader/MX66UW1G45G_STM32N6570-DK.stldr"
} elseif ($isMacOSHost) {
    $PROGRAMMER = "/Applications/STMicroelectronics/STM32CubeProgrammer.app/Contents/MacOS/bin/STM32_Programmer_CLI"
    $SIGNING_TOOL = "/Applications/STMicroelectronics/STM32CubeProgrammer.app/Contents/MacOS/bin/STM32_SigningTool_CLI"
    $EXTERNAL_LOADER = "/Applications/STMicroelectronics/STM32CubeProgrammer.app/Contents/MacOS/bin/ExternalLoader/MX66UW1G45G_STM32N6570-DK.stldr"
} elseif ($isWindowsHost) {
    $PROGRAMMER = "C:\Program Files\STMicroelectronics\STM32Cube\STM32CubeProgrammer20\bin\STM32_Programmer_CLI.exe"
    $SIGNING_TOOL = "C:\Program Files\STMicroelectronics\STM32Cube\STM32CubeProgrammer20\bin\STM32_SigningTool_CLI.exe"
    $EXTERNAL_LOADER = "C:\Program Files\STMicroelectronics\STM32Cube\STM32CubeProgrammer20\bin\ExternalLoader\MX66UW1G45G_STM32N6570-DK.stldr"
} else {
    Write-Error "Unsupported OS."
    exit 1
}

foreach ($requiredPath in @($FSBL_BIN_FILE, $APP_BIN_FILE, $PROGRAMMER, $SIGNING_TOOL, $EXTERNAL_LOADER)) {
    if (-not (Test-Path -LiteralPath $requiredPath -PathType Leaf)) {
        Write-Error "Required file not found: $requiredPath"
        exit 1
    }
}

$SIGNED_FSBL_BIN = Join-Path $scriptDir "FSBL-trusted.bin"
$SIGNED_APP_BIN = Join-Path $scriptDir "Appli-trusted.bin"

# Required FSBL signing for BOOTROM copy and jump (adds padding and header)
& "$SIGNING_TOOL" -bin "$FSBL_BIN_FILE" -nk -of 0x80000000 -t fsbl -o "$SIGNED_FSBL_BIN" -hv 2.3
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

# Required Appli signing for FSBL copy and jump (adds padding and header)
& "$SIGNING_TOOL" -bin "$APP_BIN_FILE" -nk -of 0x80000000 -t fsbl -o "$SIGNED_APP_BIN" -hv 2.3
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

# Flash signed binaries to NOR external flash
& "$PROGRAMMER" -c port=SWD mode=HOTPLUG ap=1 -w "$SIGNED_FSBL_BIN" $FLASH_FSBL_ADDRESS -el "$EXTERNAL_LOADER"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

& "$PROGRAMMER" -c port=SWD mode=HOTPLUG ap=1 -w "$SIGNED_APP_BIN" $FLASH_APP_ADDRESS -el "$EXTERNAL_LOADER"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
