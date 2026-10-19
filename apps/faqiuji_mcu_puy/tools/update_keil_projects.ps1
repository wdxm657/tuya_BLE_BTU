param([switch]$Check)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot

function Read-Project([string]$path) {
    $xml = New-Object System.Xml.XmlDocument
    $xml.PreserveWhitespace = $true
    $xml.Load($path)
    return ,$xml
}

function Save-Project($xml, [string]$path) {
    foreach ($file in $xml.SelectNodes('//File')) {
        if ($file.ChildNodes.Count -eq 3) {
            foreach ($child in @($file.ChildNodes)) {
                [void]$file.InsertBefore($xml.CreateWhitespace("`r`n              "), $child)
            }
            [void]$file.AppendChild($xml.CreateWhitespace("`r`n            "))
        }
        if ($file.PreviousSibling -eq $null -or
            $file.PreviousSibling.NodeType -ne [System.Xml.XmlNodeType]::Whitespace) {
            [void]$file.ParentNode.InsertBefore($xml.CreateWhitespace("`r`n            "), $file)
        }
    }
    foreach ($files in $xml.SelectNodes('//Files')) {
        if ($files.LastChild.NodeType -ne [System.Xml.XmlNodeType]::Whitespace) {
            [void]$files.AppendChild($xml.CreateWhitespace("`r`n          "))
        }
    }
    $settings = New-Object System.Xml.XmlWriterSettings
    $settings.Encoding = New-Object System.Text.UTF8Encoding($false)
    $writer = [System.Xml.XmlWriter]::Create($path, $settings)
    try { $xml.Save($writer) } finally { $writer.Dispose() }
}

function Set-Region($target, [string]$name, [string]$base, [string]$size) {
    $mem = $target.TargetOption.TargetArmAds.ArmAdsMisc.OnChipMemories
    foreach ($key in @('IROM', 'OCR_RVCT4')) {
        $mem.$key.StartAddress = $base
        $mem.$key.Size = $size
    }
    $target.TargetName = $name
    $common = $target.TargetOption.TargetCommonOption
    $common.Cpu = [regex]::Replace([string]$common.Cpu, 'IROM\([^)]*\)',
                                    "IROM($base,$size)", 1)
    $common.OutputName = $name
    $common.OutputDirectory = ".\Output\$name\"
    $common.ListingPath = ".\Output\$name\"
}

function Add-Source($xml, $group, [string]$name, [string]$path) {
    if ($xml.SelectSingleNode("//File[FileName='$name']")) { return }
    $file = $xml.CreateElement('File')
    foreach ($pair in @(@('FileName', $name), @('FileType', '1'), @('FilePath', $path))) {
        $node = $xml.CreateElement($pair[0])
        $node.InnerText = $pair[1]
        [void]$file.AppendChild($node)
    }
    [void]$group.Files.AppendChild($file)
}

function Update-Rte($xml, [string]$name) {
    foreach ($node in $xml.SelectNodes('//targetInfo')) {
        $node.SetAttribute('name', $name)
    }
}

if (!$Check) {
    $appPath = Join-Path $root 'faqiuji_mcu\MDK-ARM\Project.uvprojx'
    $app = Read-Project $appPath
    $target = $app.Project.Targets.Target
    Set-Region $target 'FAQIUJI_APP_A' '0x08002000' '0x0000C000'
    $target.TargetOption.TargetCommonOption.FlashDriverDll =
        $target.TargetOption.TargetCommonOption.FlashDriverDll.Replace(
            'PY063xx_128', 'PY32F040xx_128')
    $groups = $target.Groups.Group
    $user = $groups | Where-Object { $_.GroupName -eq 'Application/User' }
    $hal = $groups | Where-Object { $_.GroupName -eq 'PY32F040_HAL_Driver' }
    Add-Source $app $user 'faqiuji_ota.c' '..\Src\faqiuji_ota.c'
    Add-Source $app $user 'faqiuji_image.c' '..\..\common\faqiuji_image.c'
    Add-Source $app $hal 'py32f040_hal_flash.c' '..\..\resource\PY32F040_Firmware_V1.1.3\Drivers\PY32F040_HAL_Driver\Src\py32f040_hal_flash.c'
    $app.SelectSingleNode("//File[FileName='readme.txt']/FilePath").InnerText = '..\..\FLASH_LAYOUT.md'
    $target.TargetOption.TargetArmAds.Cads.VariousControls.Define = 'USE_HAL_DRIVER,PY32F040xB'
    Update-Rte $app 'FAQIUJI_APP_A'
    Save-Project $app $appPath

    # Same source tree; only the slot, output path and compile-time base differ.
    Set-Region $target 'FAQIUJI_APP_B' '0x0800E000' '0x0000C000'
    $target.TargetOption.TargetArmAds.Cads.VariousControls.Define = 'USE_HAL_DRIVER,PY32F040xB,FAQIUJI_APP_SLOT_B'
    Update-Rte $app 'FAQIUJI_APP_B'
    Save-Project $app (Join-Path $root 'faqiuji_mcu\MDK-ARM\Project_APP_B.uvprojx')

    $bootPath = Join-Path $root 'faqiuji_bootloader\MDK-ARM\Project.uvprojx'
    $boot = Read-Project $bootPath
    $target = $boot.Project.Targets.Target
    Set-Region $target 'FAQIUJI_IAP' '0x08000000' '0x00002000'
    foreach ($file in @($boot.SelectNodes('//File'))) {
        if ($file.FileName -in @('app_flash.c', 'app_i2c.c', 'app_wdg.c',
                                 'py32f040_hal_i2c.c', 'py32f040_hal_pwr.c',
                                 'py32f040_hal_dma.c', 'py32f040_hal_uart.c',
                                 'py32f040xx_Start_Kit.c')) {
            [void]$file.ParentNode.RemoveChild($file)
        } else {
            $file.FilePath = $file.FilePath.Replace('..\..\..\Drivers\',
                '..\..\resource\PY32F040_Firmware_V1.1.3\Drivers\')
        }
    }
    foreach ($group in @($target.Groups.Group)) {
        if ($group.GroupName -like 'Drivers/BSP/*') {
            [void]$group.ParentNode.RemoveChild($group)
        }
    }
    $user = $target.Groups.Group | Where-Object { $_.GroupName -eq 'Application/User' }
    Add-Source $boot $user 'faqiuji_image.c' '..\..\common\faqiuji_image.c'
    $controls = $target.TargetOption.TargetArmAds.Cads.VariousControls
    $controls.Define = 'USE_HAL_DRIVER,PY32F040xB'
    $controls.IncludePath = '..\Inc;..\..\resource\PY32F040_Firmware_V1.1.3\Drivers\CMSIS\Include;..\..\resource\PY32F040_Firmware_V1.1.3\Drivers\CMSIS\Device\PY32F040\Include;..\..\resource\PY32F040_Firmware_V1.1.3\Drivers\PY32F040_HAL_Driver\Inc'
    $boot.SelectSingleNode("//File[FileName='readme.txt']/FilePath").InnerText = '..\..\FLASH_LAYOUT.md'
    Update-Rte $boot 'FAQIUJI_IAP'
    Save-Project $boot $bootPath
}

$projects = @(
    @('faqiuji_bootloader\MDK-ARM\Project.uvprojx', 0x08000000, 0x2000),
    @('faqiuji_mcu\MDK-ARM\Project.uvprojx', 0x08002000, 0xC000),
    @('faqiuji_mcu\MDK-ARM\Project_APP_B.uvprojx', 0x0800E000, 0xC000)
)
foreach ($spec in $projects) {
    $path = Join-Path $root $spec[0]
    $xml = Read-Project $path
    $target = $xml.Project.Targets.Target
    foreach ($key in @('IROM', 'OCR_RVCT4')) {
        $region = $target.TargetOption.TargetArmAds.ArmAdsMisc.OnChipMemories.$key
        if ([Convert]::ToUInt32($region.StartAddress, 16) -ne $spec[1] -or
            [Convert]::ToUInt32($region.Size, 16) -ne $spec[2]) {
            throw "Wrong Flash region: $path / $key"
        }
    }
    foreach ($file in $xml.SelectNodes('//File')) {
        $source = Join-Path (Split-Path $path) $file.FilePath
        if (!(Test-Path -LiteralPath $source)) { throw "Missing source: $source" }
    }
    $defines = $target.TargetOption.TargetArmAds.Cads.VariousControls.Define
    if ($target.TargetName -eq 'FAQIUJI_APP_B' -and
        $defines -notmatch '(^|,)FAQIUJI_APP_SLOT_B(,|$)') {
        throw 'APP B is missing FAQIUJI_APP_SLOT_B.'
    }
    if ($target.TargetName -eq 'FAQIUJI_APP_A' -and
        $defines -match 'FAQIUJI_APP_SLOT_B') {
        throw 'APP A must not define FAQIUJI_APP_SLOT_B.'
    }
    if ($target.TargetName -like 'FAQIUJI_APP_*' -and
        $target.TargetOption.TargetCommonOption.FlashDriverDll -notmatch 'PY32F040xx_128\.FLM') {
        throw "Wrong PY32F040 Flash driver: $path"
    }
    foreach ($include in $target.TargetOption.TargetArmAds.Cads.VariousControls.IncludePath.Split(';')) {
        if ($include -and !(Test-Path -LiteralPath (Join-Path (Split-Path $path) $include))) {
            throw "Missing include path: $include"
        }
    }
    Write-Output "$($target.TargetName): Flash region, source and include paths OK"
}
