$ErrorActionPreference = "Stop"

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$configPath = Join-Path $scriptDir "config.json"
$config = Get-Content -Raw -Path $configPath | ConvertFrom-Json

# ------------------------------------------------------------------
# Root CA certificates
# ------------------------------------------------------------------

$AwsMqttRootCaPem = @'
-----BEGIN CERTIFICATE-----
MIIDQTCCAimgAwIBAgITBmyfz5m/jAo54vB4ikPmljZbyjANBgkqhkiG9w0BAQsF
ADA5MQswCQYDVQQGEwJVUzEPMA0GA1UEChMGQW1hem9uMRkwFwYDVQQDExBBbWF6
b24gUm9vdCBDQSAxMB4XDTE1MDUyNjAwMDAwMFoXDTM4MDExNzAwMDAwMFowOTEL
MAkGA1UEBhMCVVMxDzANBgNVBAoTBkFtYXpvbjEZMBcGA1UEAxMQQW1hem9uIFJv
b3QgQ0EgMTCCASIwDQYJKoZIhvcNAQEBBQADggEPADCCAQoCggEBALJ4gHHKeNXj
ca9HgFB0fW7Y14h29Jlo91ghYPl0hAEvrAIthtOgQ3pOsqTQNroBvo3bSMgHFzZM
9O6II8c+6zf1tRn4SWiw3te5djgdYZ6k/oI2peVKVuRF4fn9tBb6dNqcmzU5L/qw
IFAGbHrQgLKm+a/sRxmPUDgH3KKHOVj4utWp+UhnMJbulHheb4mjUcAwhmahRWa6
VOujw5H5SNz/0egwLX0tdHA114gk957EWW67c4cX8jJGKLhD+rcdqsq08p8kDi1L
93FcXmn/6pUCyziKrlA4b9v7LWIbxcceVOF34GfID5yHI9Y/QCB/IIDEgEw+OyQm
jgSubJrIqg0CAwEAAaNCMEAwDwYDVR0TAQH/BAUwAwEB/zAOBgNVHQ8BAf8EBAMC
AYYwHQYDVR0OBBYEFIQYzIU07LwMlJQuCFmcx7IQTgoIMA0GCSqGSIb3DQEBCwUA
A4IBAQCY8jdaQZChGsV2USggNiMOruYou6r4lK5IpDB/G/wkjUu0yKGX9rbxenDI
U5PMCCjjmCXPI6T53iHTfIUJrU6adTrCC2qJeHZERxhlbI1Bjjt/msv0tadQ1wUs
N+gDS63pYaACbvXy8MWy7Vu33PqUXHeeE6V/Uq2V8viTO96LXFvKWlJbYK8U90vv
o/ufQJVtMVT8QtPHRh8jrdkPSHCa2XV4cdFyQzR1bldZwgJcJmApzyMZFo6IQ6XU
5MsI+yMRQ+hDKXJioaldXgjUkK642M4UwtBV8ob2xJNDd2ZhwLnoQdeXeGADbkpy
rqXRfboQnoZsG4q5WTP468SQvvG5
-----END CERTIFICATE-----
'@

$AzureMqttRootCaPem = @'
-----BEGIN CERTIFICATE-----
MIIDjjCCAnagAwIBAgIQAzrx5qcRqaC7KGSxHQn65TANBgkqhkiG9w0BAQsFADBh
MQswCQYDVQQGEwJVUzEVMBMGA1UEChMMRGlnaUNlcnQgSW5jMRkwFwYDVQQLExB3
d3cuZGlnaWNlcnQuY29tMSAwHgYDVQQDExdEaWdpQ2VydCBHbG9iYWwgUm9vdCBH
MjAeFw0xMzA4MDExMjAwMDBaFw0zODAxMTUxMjAwMDBaMGExCzAJBgNVBAYTAlVT
MRUwEwYDVQQKEwxEaWdpQ2VydCBJbmMxGTAXBgNVBAsTEHd3dy5kaWdpY2VydC5j
b20xIDAeBgNVBAMTF0RpZ2lDZXJ0IEdsb2JhbCBSb290IEcyMIIBIjANBgkqhkiG
9w0BAQEFAAOCAQ8AMIIBCgKCAQEAuzfNNNx7a8myaJCtSnX/RrohCgiN9RlUyfuI
2/Ou8jqJkTx65qsGGmvPrC3oXgkkRLpimn7Wo6h+4FR1IAWsULecYxpsMNzaHxmx
1x7e/dfgy5SDN67sH0NO3Xss0r0upS/kqbitOtSZpLYl6ZtrAGCSYP9PIUkY92eQ
q2EGnI/yuum06ZIya7XzV+hdG82MHauVBJVJ8zUtluNJbd134/tJS7SsVQepj5Wz
tCO7TG1F8PapspUwtP1MVYwnSlcUfIKdzXOS0xZKBgyMUNGPHgm+F6HmIcr9g+UQ
vIOlCsRnKPZzFBQ9RnbDhxSJITRNrw9FDKZJobq7nMWxM4MphQIDAQABo0IwQDAP
BgNVHRMBAf8EBTADAQH/MA4GA1UdDwEB/wQEAwIBhjAdBgNVHQ4EFgQUTiJUIBiV
5uNu5g/6+rkS7QYXjzkwDQYJKoZIhvcNAQELBQADggEBAGBnKJRvDkhj6zHd6mcY
1Yl9PMWLSn/pvtsrF9+wX3N3KjITOYFnQoQj8kVnNeyIv/iPsGEMNKSuIEyExtv4
NeF22d+mQrvHRAiGfzZ0JFrabA0UWTW98kndth/Jsw1HKj2ZL7tcu7XUIOGZX1NG
Fdtom/DzMNU+MeKNhJ7jitralj41E6Vf8PlwUHBHQRFXGU7Aj64GxJUTFy8bJZ91
8rGOmaFvE7FBcf6IKshPECBV1/MUReXgRPTqh5Uykw7+U0b6LJ3/iyK5S9kJRaTe
pLiaWN0bfVKfjllDiIGknibVb63dDcY3fe0Dkhvld1927jyNxF1WW6LZZm6zNTfl
MrY=
-----END CERTIFICATE-----
'@

$IotconnectDraCaPem = @'
-----BEGIN CERTIFICATE-----
MIIE0DCCA7igAwIBAgIBBzANBgkqhkiG9w0BAQsFADCBgzELMAkGA1UEBhMCVVMx
EDAOBgNVBAgTB0FyaXpvbmExEzARBgNVBAcTClNjb3R0c2RhbGUxGjAYBgNVBAoT
EUdvRGFkZHkuY29tLCBJbmMuMTEwLwYDVQQDEyhHbyBEYWRkeSBSb290IENlcnRp
ZmljYXRlIEF1dGhvcml0eSAtIEcyMB4XDTExMDUwMzA3MDAwMFoXDTMxMDUwMzA3
MDAwMFowgbQxCzAJBgNVBAYTAlVTMRAwDgYDVQQIEwdBcml6b25hMRMwEQYDVQQH
EwpTY290dHNkYWxlMRowGAYDVQQKExFHb0RhZGR5LmNvbSwgSW5jLjEtMCsGA1UE
CxMkaHR0cDovL2NlcnRzLmdvZGFkZHkuY29tL3JlcG9zaXRvcnkvMTMwMQYDVQQD
EypHbyBEYWRkeSBTZWN1cmUgQ2VydGlmaWNhdGUgQXV0aG9yaXR5IC0gRzIwggEi
MA0GCSqGSIb3DQEBAQUAA4IBDwAwggEKAoIBAQC54MsQ1K92vdSTYuswZLiBCGzD
BNliF44v/z5lz4/OYuY8UhzaFkVLVat4a2ODYpDOD2lsmcgaFItMzEUz6ojcnqOv
K/6AYZ15V8TPLvQ/MDxdR/yaFrzDN5ZBUY4RS1T4KL7QjL7wMDge87Am+GZHY23e
cSZHjzhHU9FGHbTj3ADqRay9vHHZqm8A29vNMDp5T19MR/gd71vCxJ1gO7GyQ5HY
pDNO6rPWJ0+tJYqlxvTV0KaudAVkV4i1RFXULSo6Pvi4vekyCgKUZMQWOlDxSq7n
eTOvDCAHf+jfBDnCaQJsY1L6d8EbyHSHyLmTGFBUNUtpTrw700kuH9zB0lL7AgMB
AAGjggEaMIIBFjAPBgNVHRMBAf8EBTADAQH/MA4GA1UdDwEB/wQEAwIBBjAdBgNV
HQ4EFgQUQMK9J47MNIMwojPX+2yz8LQsgM4wHwYDVR0jBBgwFoAUOpqFBxBnKLbv
9r0FQW4gwZTaD94wNAYIKwYBBQUHAQEEKDAmMCQGCCsGAQUFBzABhhhodHRwOi8v
b2NzcC5nb2RhZGR5LmNvbS8wNQYDVR0fBC4wLDAqoCigJoYkaHR0cDovL2NybC5n
b2RhZGR5LmNvbS9nZHJvb3QtZzIuY3JsMEYGA1UdIAQ/MD0wOwYEVR0gADAzMDEG
CCsGAQUFBwIBFiVodHRwczovL2NlcnRzLmdvZGFkZHkuY29tL3JlcG9zaXRvcnkv
MA0GCSqGSIb3DQEBCwUAA4IBAQAIfmyTEMg4uJapkEv/oV9PBO9sPpyIBslQj6Zz
91cxG7685C/b+LrTW+C05+Z5Yg4MotdqY3MxtfWoSKQ7CC2iXZDXtHwlTxFWMMS2
RJ17LJ3lXubvDGGqv+QqG+6EnriDfcFDzkSnE3ANkR/0yBOtg2DZ2HKocyQetawi
DsoXiWJYRBuriSUBAA/NxBti21G00w9RKpv0vHP8ds42pM3Z2Czqrpv1KrKQ0U11
GIo/ikGQI31bS/6kA1ibRrLDYGCD+H1QQc7CoZDDu+8CL9IVVO5EFdkKrqeKM+2x
LXY2JtwE65/3YR8V3Idv7kaWKK2hJn0KCacuBKONvPi8BDAB
-----END CERTIFICATE-----
'@

# ------------------------------------------------------------------
# Helper functions
# ------------------------------------------------------------------

function Read-ConfigValueOrPrompt {
    param(
        [string]$CurrentValue,
        [string]$Prompt,
        [string]$DefaultValue = ""
    )

    $effectiveDefault = $DefaultValue
    if (-not [string]::IsNullOrWhiteSpace($CurrentValue)) {
        $effectiveDefault = $CurrentValue
    }

    $suffix = ""
    if (-not [string]::IsNullOrWhiteSpace($effectiveDefault)) {
        $suffix = " [$effectiveDefault]"
    }

    $inputValue = Read-Host "$Prompt$suffix"
    if ([string]::IsNullOrWhiteSpace($inputValue)) {
        return $effectiveDefault
    }

    return $inputValue.Trim()
}

function Resolve-BoardSerialPort {
    param([Parameter(Mandatory = $true)]$Config)

    if (-not [string]::IsNullOrWhiteSpace($env:STM32_CLI_PORT)) {
        $envPort = $env:STM32_CLI_PORT.Trim()
        if ($envPort -notmatch '^(?i:COM)\d+$') {
            throw "STM32_CLI_PORT must look like COM12. Current value: '$envPort'."
        }
        return $envPort.ToUpperInvariant()
    }

    $detectedPorts = @(@(
        Get-CimInstance Win32_SerialPort |
            Where-Object { $_.Name -like "*STLink Virtual COM Port*" } |
            Select-Object -ExpandProperty DeviceID |
            ForEach-Object { ([string]$_).Trim().ToUpperInvariant() }
    ) | Where-Object { -not [string]::IsNullOrWhiteSpace($_) } | Select-Object -Unique)

    if ($detectedPorts.Count -eq 1) {
        return [string]$detectedPorts[0]
    }

    if ($detectedPorts.Count -gt 1) {
        Write-Host ""
        Write-Host "Multiple STLink Virtual COM ports detected:"
        foreach ($port in $detectedPorts) {
            Write-Host "  $port"
        }
        throw "More than one STLink Virtual COM Port is connected. Disconnect the extra board(s) and rerun the script."
    }

    throw "Could not auto-detect an STLink Virtual COM Port. Connect the board, make sure the ST-LINK VCP is enumerated, and rerun the script."
}

function Open-BoardSerialPort {
    param(
        [Parameter(Mandatory = $true)][string]$PortName,
        [Parameter(Mandatory = $true)][int]$BaudRate
    )

    $port = New-Object System.IO.Ports.SerialPort $PortName, $BaudRate, "None", 8, "One"
    $port.ReadTimeout = 500
    $port.WriteTimeout = 500
    $port.DtrEnable = $true
    $port.RtsEnable = $true
    try {
        $port.Open()
    }
    catch [System.UnauthorizedAccessException] {
        throw "Serial port '$PortName' is currently in use. Close your serial terminal or any other tool using $PortName, then rerun .\provision.ps1."
    }
    catch [System.IO.IOException] {
        throw "Serial port '$PortName' could not be opened. Close any terminal or tool using $PortName, confirm the board is connected, then rerun .\provision.ps1."
    }
    Start-Sleep -Milliseconds 300
    return $port
}

function Open-BoardSerialPortWithRetry {
    param(
        [Parameter(Mandatory = $true)][string]$PortName,
        [Parameter(Mandatory = $true)][int]$BaudRate
    )

    while ($true) {
        try {
            return Open-BoardSerialPort -PortName $PortName -BaudRate $BaudRate
        }
        catch {
            Write-Host ""
            Write-Host $_.Exception.Message
            $retry = Read-Host "Close the serial terminal using $PortName, then press Enter to retry or type q to quit"
            if ($retry -match '^(?i:q|quit)$') {
                throw "Provisioning canceled by user."
            }
        }
    }
}

function Read-UntilQuiet {
    param(
        [Parameter(Mandatory = $true)]$SerialPort,
        [int]$InitialDelayMs = 300,
        [int]$IdleMs = 700,
        [int]$MaxWaitMs = 6000
    )

    $response = ""
    $overall = [System.Diagnostics.Stopwatch]::StartNew()
    $idle = [System.Diagnostics.Stopwatch]::StartNew()

    Start-Sleep -Milliseconds $InitialDelayMs

    while ($overall.ElapsedMilliseconds -lt $MaxWaitMs) {
        if ($SerialPort.BytesToRead -gt 0) {
            $response += $SerialPort.ReadExisting()
            $idle.Restart()
        }
        elseif ($idle.ElapsedMilliseconds -ge $IdleMs) {
            break
        }

        Start-Sleep -Milliseconds 50
    }

    return $response
}

function Send-Command {
    param(
        [Parameter(Mandatory = $true)]$SerialPort,
        [Parameter(Mandatory = $true)][AllowEmptyString()][string]$Command,
        [int]$TimeoutMs = 1500
    )

    $SerialPort.WriteLine($Command)

    $response = ""
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    while ($sw.ElapsedMilliseconds -lt $TimeoutMs) {
        if ($SerialPort.BytesToRead -gt 0) {
            $response += $SerialPort.ReadExisting()
        }
        Start-Sleep -Milliseconds 50
    }

    return $response
}

function Wait-ForCliReady {
    param([Parameter(Mandatory = $true)]$SerialPort)

    for ($attempt = 1; $attempt -le 12; $attempt++) {
        $response = Send-Command -SerialPort $SerialPort -Command "" -TimeoutMs 1200
        if (-not [string]::IsNullOrWhiteSpace($response)) {
            return
        }

        $response = Send-Command -SerialPort $SerialPort -Command "conf get broker_type" -TimeoutMs 1800
        if (-not [string]::IsNullOrWhiteSpace($response)) {
            return
        }

        Start-Sleep -Milliseconds 1000
    }

    throw "Device CLI did not respond. Make sure the board is running the project app in Flash Boot."
}

function Assert-ResponseContains {
    param(
        [Parameter(Mandatory = $true)][string]$Response,
        [Parameter(Mandatory = $true)][string]$ExpectedText,
        [Parameter(Mandatory = $true)][string]$Operation
    )

    if ($Response -notmatch [regex]::Escape($ExpectedText)) {
        $trimmedResponse = $Response.Trim()
        if ([string]::IsNullOrWhiteSpace($trimmedResponse)) {
            $trimmedResponse = "<no response>"
        }
        throw "$Operation did not return the expected text '$ExpectedText'. Device response: $trimmedResponse"
    }
}

function Extract-PemBlock {
    param(
        [Parameter(Mandatory = $true)][string]$Text,
        [Parameter(Mandatory = $true)][string]$BeginMarker,
        [Parameter(Mandatory = $true)][string]$EndMarker
    )

    $pattern = [regex]::Escape($BeginMarker) + ".*?" + [regex]::Escape($EndMarker)
    $match = [regex]::Match($Text, $pattern, [System.Text.RegularExpressions.RegexOptions]::Singleline)

    if (-not $match.Success) {
        throw "Failed to extract PEM block between '$BeginMarker' and '$EndMarker'."
    }

    return $match.Value.Trim()
}

function Ensure-OutputDirectory {
    $path = Join-Path $scriptDir "generated_iotconnect_identity"
    if (-not (Test-Path -LiteralPath $path -PathType Container)) {
        New-Item -ItemType Directory -Path $path | Out-Null
    }
    return (Resolve-Path -LiteralPath $path).Path
}

function Get-ThingName {
    param([Parameter(Mandatory = $true)]$SerialPort)

    $response = Send-Command -SerialPort $SerialPort -Command "conf get thing_name" -TimeoutMs 3000
    $match = [regex]::Match($response, 'thing_name\s*=\s*"([^"]+)"')

    if (-not $match.Success) {
        throw "Failed to read thing_name from device. Response: $($response.Trim())"
    }

    return $match.Groups[1].Value
}

function Set-ConfigValue {
    param(
        [Parameter(Mandatory = $true)]$SerialPort,
        [Parameter(Mandatory = $true)][string]$Key,
        [string]$Value
    )

    if ([string]::IsNullOrWhiteSpace($Value)) {
        Send-Command -SerialPort $SerialPort -Command "conf set $Key " | Out-Null
    }
    else {
        Send-Command -SerialPort $SerialPort -Command "conf set $Key $Value" | Out-Null
    }
}

function Send-TextContent {
    param(
        [Parameter(Mandatory = $true)]$SerialPort,
        [Parameter(Mandatory = $true)][string]$Text
    )

    $SerialPort.WriteLine($Text)
    return Read-UntilQuiet -SerialPort $SerialPort -InitialDelayMs 400 -IdleMs 900 -MaxWaitMs 7000
}

function Parse-IotconnectDeviceConfigJson {
    param([Parameter(Mandatory = $true)][string]$JsonText)

    $deviceConfig = $JsonText | ConvertFrom-Json

    $platform = [string]$deviceConfig.pf
    switch ($platform.ToLowerInvariant()) {
        "aws" { $backend = "aws" }
        "az" { $backend = "azure" }
        "azure" { $backend = "azure" }
        default { throw "Unsupported pf value '$platform'. Expected 'aws' or 'az'." }
    }

    $cpid = [string]$deviceConfig.cpid
    $envName = [string]$deviceConfig.env
    $uid = [string]$deviceConfig.uid
    $did = [string]$deviceConfig.did
    $disc = [string]$deviceConfig.disc

    if ([string]::IsNullOrWhiteSpace($cpid) -or
        [string]::IsNullOrWhiteSpace($envName) -or
        ([string]::IsNullOrWhiteSpace($uid) -and [string]::IsNullOrWhiteSpace($did))) {
        throw "Device JSON is missing required values. Expected cpid, env, and uid or did."
    }

    if ([string]::IsNullOrWhiteSpace($uid)) {
        $uid = $did
    }

    return [PSCustomObject]@{
        Backend = $backend
        Cpid = $cpid
        Env = $envName
        Uid = $uid
        Did = $did
        DiscoveryUrl = $disc
    }
}

function Validate-DiscoveryUrl {
    param(
        [Parameter(Mandatory = $true)][string]$Backend,
        [string]$DiscoveryUrl
    )

    if ([string]::IsNullOrWhiteSpace($DiscoveryUrl)) {
        return
    }

    $uri = $null
    if (-not [System.Uri]::TryCreate($DiscoveryUrl, [System.UriKind]::Absolute, [ref]$uri)) {
        throw "The device JSON contains an invalid discovery URL: $DiscoveryUrl"
    }

    if ($uri.Scheme -ne "https") {
        throw "The device JSON discovery URL must use https: $DiscoveryUrl"
    }

    $discoveryHost = $uri.Host.ToLowerInvariant()
    if ($Backend -eq "aws") {
        if ($discoveryHost -notin @("awsdiscovery.iotconnect.io", "discovery.iotconnect.io")) {
            throw "Unexpected AWS discovery host '$discoveryHost' in device JSON."
        }
    }
    elseif ($Backend -eq "azure") {
        if ($discoveryHost -ne "discovery.iotconnect.io") {
            throw "Unexpected Azure discovery host '$discoveryHost' in device JSON."
        }
    }
}

function Read-PastedJson {
    Write-Host ""
    Write-Host "Paste the IOTCONNECT device JSON now."
    Write-Host "When you are finished, type ENDJSON on its own line and press Enter."
    Write-Host ""

    $lines = New-Object System.Collections.Generic.List[string]
    while ($true) {
        $line = Read-Host
        if ($line -eq "ENDJSON") {
            break
        }
        $lines.Add($line)
    }

    $jsonText = ($lines -join [Environment]::NewLine).Trim()
    if ([string]::IsNullOrWhiteSpace($jsonText)) {
        throw "No IOTCONNECT device JSON was pasted."
    }

    return $jsonText
}

function Validate-ThingName {
    param([Parameter(Mandatory = $true)][string]$ThingName)

    if ($ThingName -match '\s') {
        throw "thing_name must not contain whitespace."
    }

    if ($ThingName.Length -lt 1) {
        throw "thing_name must not be empty."
    }
}

function Show-UiInstructions {
    param(
        [Parameter(Mandatory = $true)][string]$ThingName,
        [Parameter(Mandatory = $true)][string]$CertificatePem,
        [Parameter(Mandatory = $true)][string]$SavedCertPath
    )

    Write-Host ""
    Write-Host "============================================================"
    Write-Host " IOTCONNECT UI STEPS"
    Write-Host "============================================================"
    Write-Host "1. Create a new device in IOTCONNECT."
    Write-Host "2. Unique Id:"
    Write-Host "   $ThingName"
    Write-Host "3. Device Name:"
    Write-Host "   $ThingName"
    Write-Host "4. Select the appropriate Entity."
    Write-Host "5. Select Template:"
    Write-Host "   STM32N6 W6X LED and Button Demo"
    Write-Host "6. In Device certificate, select ONLY:"
    Write-Host "   Use my certificate"
    Write-Host "   Do not select Auto-generated or any other certificate option."
    Write-Host "7. Paste the certificate below into the 'Certificate Text' box."
    Write-Host "   Copy the full PEM including:"
    Write-Host "   -----BEGIN CERTIFICATE-----"
    Write-Host "   -----END CERTIFICATE-----"
    Write-Host "8. Click 'Save & View'."
    Write-Host "9. On the device page, click the document + gear icon in the top right to download the device JSON."
    Write-Host ""
    Write-Host "A copy of the certificate was saved to:"
    Write-Host "  $SavedCertPath"
    Write-Host ""
    Write-Host "Certificate to paste into IOTCONNECT:"
    Write-Host $CertificatePem
}

# ------------------------------------------------------------------
# Collect user input up-front
# ------------------------------------------------------------------

$wifiSsid = Read-ConfigValueOrPrompt -CurrentValue ([string]$config.wifi_ssid) -Prompt "Wi-Fi SSID"
$wifiCredential = Read-ConfigValueOrPrompt -CurrentValue ([string]$config.wifi_credential) -Prompt "Wi-Fi credential/password"

$portName = Resolve-BoardSerialPort -Config $config
$appMode = "demo"

Write-Host ""
Write-Host "Using board serial port: $portName"

$outputDirectory = Ensure-OutputDirectory
$baudRate = 115200
$serialPort = $null

try {
    $serialPort = Open-BoardSerialPortWithRetry -PortName $portName -BaudRate $baudRate
    Wait-ForCliReady -SerialPort $serialPort

    # ------------------------------------------------------------------
    # Step 1: Thing name
    # ------------------------------------------------------------------
    $deviceThingName = Get-ThingName -SerialPort $serialPort
    Write-Host ""
    Write-Host "Detected device thing_name: $deviceThingName"

    $thingNameInput = Read-Host "Press Enter to keep this thing_name, or type a new one"
    if (-not [string]::IsNullOrWhiteSpace($thingNameInput)) {
        $newThingName = $thingNameInput.Trim()
        Validate-ThingName -ThingName $newThingName
        Set-ConfigValue -SerialPort $serialPort -Key "thing_name" -Value $newThingName
        Send-Command -SerialPort $serialPort -Command "conf commit" | Out-Null
        $deviceThingName = $newThingName
        Write-Host "Updated thing_name to: $deviceThingName"
    }

    # ------------------------------------------------------------------
    # Step 2: Generate device key pair and self-signed certificate
    # ------------------------------------------------------------------
    $generateKeyResponse = Send-Command -SerialPort $serialPort -Command "pki generate key tls_key_pub tls_key_priv ec prime256v1" -TimeoutMs 12000
    Assert-ResponseContains -Response $generateKeyResponse -ExpectedText "SUCCESS:" -Operation "Device key generation"

    $certResponse = Send-Command -SerialPort $serialPort -Command "pki generate cert tls_cert tls_key_priv" -TimeoutMs 15000
    $certPem = Extract-PemBlock -Text $certResponse -BeginMarker "-----BEGIN CERTIFICATE-----" -EndMarker "-----END CERTIFICATE-----"

    $certPath = Join-Path $outputDirectory "$deviceThingName.iotconnect.cert.pem"
    Set-Content -Path $certPath -Value ($certPem + "`r`n") -Encoding ascii

    # ------------------------------------------------------------------
    # Step 3: Show IoTConnect UI instructions
    # ------------------------------------------------------------------
    Show-UiInstructions -ThingName $deviceThingName -CertificatePem $certPem -SavedCertPath $certPath

    Write-Host ""
    Write-Host "After creating the device and downloading the IOTCONNECT device JSON, return here."
    [void](Read-Host "Press Enter when you are ready to paste the device JSON")

    # ------------------------------------------------------------------
    # Step 4: Parse device JSON
    # ------------------------------------------------------------------
    $pastedJson = Read-PastedJson
    $jsonSavePath = Join-Path $outputDirectory "$deviceThingName.iotcDeviceConfig.json"
    Set-Content -Path $jsonSavePath -Value ($pastedJson + "`r`n") -Encoding utf8

    $deviceConfigInfo = Parse-IotconnectDeviceConfigJson -JsonText $pastedJson
    Validate-DiscoveryUrl -Backend $deviceConfigInfo.Backend -DiscoveryUrl $deviceConfigInfo.DiscoveryUrl

    if ($deviceConfigInfo.Uid -ne $deviceThingName) {
        throw "IOTCONNECT device JSON uid '$($deviceConfigInfo.Uid)' does not match thing_name '$deviceThingName'."
    }

    Write-Host ""
    Write-Host "Device JSON accepted."
    Write-Host "  pf/backend : $($deviceConfigInfo.Backend)"
    Write-Host "  cpid       : $($deviceConfigInfo.Cpid)"
    Write-Host "  env        : $($deviceConfigInfo.Env)"
    Write-Host "  uid        : $($deviceConfigInfo.Uid)"
    if (-not [string]::IsNullOrWhiteSpace($deviceConfigInfo.Did)) {
        Write-Host "  did        : $($deviceConfigInfo.Did)"

        $expectedDidForms = @(
            $deviceThingName,
            "$($deviceConfigInfo.Cpid)-$deviceThingName"
        ) | Select-Object -Unique

        if ($deviceConfigInfo.Did -notin $expectedDidForms) {
            Write-Warning "Device JSON did '$($deviceConfigInfo.Did)' does not match the usual forms '$deviceThingName' or '$($deviceConfigInfo.Cpid)-$deviceThingName'. Continuing because UID is the authoritative board identity."
        }
    }
    if (-not [string]::IsNullOrWhiteSpace($deviceConfigInfo.DiscoveryUrl)) {
        Write-Host "  disc       : $($deviceConfigInfo.DiscoveryUrl)"
        Write-Host "Note: firmware derives discovery from pf/cpid/env; the discovery URL is validated for reference, not stored as a separate conf key."
    }
    Write-Host "  saved json : $jsonSavePath"

    # ------------------------------------------------------------------
    # Step 5: Import root CA certificates
    # ------------------------------------------------------------------
    if ($deviceConfigInfo.Backend -eq "azure") {
        $mqttRootCaText = $AzureMqttRootCaPem
    }
    else {
        $mqttRootCaText = $AwsMqttRootCaPem
    }

    Send-Command -SerialPort $serialPort -Command "pki import cert root_ca_cert" | Out-Null
    $rootCaResponse = Send-TextContent -SerialPort $serialPort -Text $mqttRootCaText
    Assert-ResponseContains -Response $rootCaResponse -ExpectedText "Success:" -Operation "MQTT root CA import"

    Send-Command -SerialPort $serialPort -Command "pki import cert iotconnect_dra_ca" | Out-Null
    $draCaResponse = Send-TextContent -SerialPort $serialPort -Text $IotconnectDraCaPem
    Assert-ResponseContains -Response $draCaResponse -ExpectedText "Success:" -Operation "IOTCONNECT DRA CA import"

    # Verify certificate and key are present on device
    $certVerifyResponse = Send-Command -SerialPort $serialPort -Command "pki export cert tls_cert" -TimeoutMs 5000
    Assert-ResponseContains -Response $certVerifyResponse -ExpectedText "-----BEGIN CERTIFICATE-----" -Operation "On-device certificate verification"

    $pubKeyResponse = Send-Command -SerialPort $serialPort -Command "pki export key tls_key_pub" -TimeoutMs 5000
    Assert-ResponseContains -Response $pubKeyResponse -ExpectedText "-----BEGIN PUBLIC KEY-----" -Operation "On-device public key verification"

    # ------------------------------------------------------------------
    # Step 6: Set IoTConnect configuration
    # ------------------------------------------------------------------
    Set-ConfigValue -SerialPort $serialPort -Key "broker_type" -Value "iotconnect"
    Set-ConfigValue -SerialPort $serialPort -Key "iotc_cloud" -Value $deviceConfigInfo.Backend
    Set-ConfigValue -SerialPort $serialPort -Key "iotc_cpid" -Value $deviceConfigInfo.Cpid
    Set-ConfigValue -SerialPort $serialPort -Key "iotc_env" -Value $deviceConfigInfo.Env
    Set-ConfigValue -SerialPort $serialPort -Key "iotc_app_mode" -Value $appMode
    Set-ConfigValue -SerialPort $serialPort -Key "iotc_identity_json" -Value ""
    Set-ConfigValue -SerialPort $serialPort -Key "wifi_ssid" -Value $wifiSsid
    Set-ConfigValue -SerialPort $serialPort -Key "wifi_credential" -Value $wifiCredential
    Send-Command -SerialPort $serialPort -Command "conf set iotc_cache_valid 0" | Out-Null

    # KVS WebRTC configuration is automatic — the firmware parses the
    # IoTConnect identity response "vs" block at runtime to get the
    # signaling channel ARN, credentials endpoint, and role alias.
    # No manual KVS conf set needed.

    # ------------------------------------------------------------------
    # Step 7: Commit and reset
    # ------------------------------------------------------------------
    Send-Command -SerialPort $serialPort -Command "conf commit" | Out-Null

    Write-Host ""
    Write-Host "Board configured for IOTCONNECT."
    Write-Host "KVS WebRTC will auto-configure from the IoTConnect device template"
    Write-Host "(requires a template with Video Streaming enabled, e.g. plitekvs)."
    Write-Host "Resetting device now..."
    Start-Sleep -Seconds 1
    Send-Command -SerialPort $serialPort -Command "reset" | Out-Null

    Write-Host ""
    Write-Host "Next:"
    Write-Host "1. Let the board reboot."
    Write-Host "2. Wait up to 60 seconds for the device to connect to IOTCONNECT."
    Write-Host "3. If the IoTConnect template has Video Streaming enabled, KVS WebRTC"
    Write-Host "   will auto-connect. View the stream in the KVS console or IoTConnect UI."
    Write-Host "4. Monitor $portName in your serial terminal at 115200 8N1."
    Write-Host ""
    Write-Host "Provisioning completed. Releasing the serial port now."
}
catch {
    Write-Host ""
    Write-Host $_.Exception.Message
    exit 1
}
finally {
    if ($null -ne $serialPort -and $serialPort.IsOpen) {
        $serialPort.Close()
        Write-Host "Serial port closed."
    }
}
