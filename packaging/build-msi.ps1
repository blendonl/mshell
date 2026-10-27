param(
    [Parameter(Mandatory)] [string]$Version,
    [Parameter(Mandatory)] [string]$Stage,
    [Parameter(Mandatory)] [string]$Out,
    [string]$Companions,
    [switch]$LatestCompanions,
    [string]$WixVersion = '5.0.2',
    [string]$TimestampUrl = 'http://timestamp.digicert.com'
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

$Stage = (Resolve-Path $Stage).Path
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
if (-not $Companions) { $Companions = Join-Path $here 'companions.json' }
$pins = Get-Content $Companions -Raw | ConvertFrom-Json

function Get-Companion {
    param([string]$Name)

    $pin = $pins.$Name
    if (-not $LatestCompanions -and -not $pin) { throw "$Companions has no pin for $Name" }
    $endpoint = if ($LatestCompanions) { 'latest' } else { "tags/v$($pin.version.TrimStart('v'))" }
    $headers = @{ 'User-Agent' = 'mshell-build' }
    if ($env:GH_TOKEN) { $headers.Authorization = "Bearer $env:GH_TOKEN" }

    $release = Invoke-RestMethod -Headers $headers "https://api.github.com/repos/blendonl/$Name/releases/$endpoint"
    $asset = $release.assets | Where-Object name -like "$Name-*-win64.zip" | Select-Object -First 1
    if (-not $asset) { throw "$Name $($release.tag_name) has no $Name-*-win64.zip" }

    $work = Join-Path ([IO.Path]::GetTempPath()) "mshell-msi-$Name-$([guid]::NewGuid())"
    New-Item -ItemType Directory $work | Out-Null
    try {
        $zip = Join-Path $work $asset.name
        Invoke-WebRequest -Headers $headers $asset.browser_download_url -OutFile $zip -UseBasicParsing

        $got = (Get-FileHash $zip -Algorithm SHA256).Hash
        if (-not $LatestCompanions) {
            if ($got -ne $pin.sha256) { throw "$($asset.name): sha256 $got does not match the pinned $($pin.sha256)" }
        } elseif ($asset.digest -like 'sha256:*') {
            $want = $asset.digest.Substring(7)
            if ($got -ne $want) { throw "$($asset.name): sha256 $got does not match the published $want" }
        }

        Expand-Archive $zip (Join-Path $work 'unpacked')
        $payload = Get-ChildItem (Join-Path $work 'unpacked') -Directory | Select-Object -First 1
        if (-not (Test-Path (Join-Path $payload.FullName "$Name.exe"))) {
            throw "$($asset.name) has no $Name.exe at its root"
        }

        $dest = Join-Path $Stage $Name
        Remove-Item $dest -Recurse -Force -ErrorAction SilentlyContinue
        New-Item -ItemType Directory $dest | Out-Null
        Copy-Item (Join-Path $payload.FullName '*') $dest -Recurse -Force
    } finally {
        Remove-Item $work -Recurse -Force -ErrorAction SilentlyContinue
    }

    Write-Host "bundled $Name $($release.tag_name)"
    "$Name $($release.tag_name.TrimStart('v'))"
}

function ConvertTo-Rtf {
    param([string]$Text)
    $escaped = $Text.Replace('\', '\\').Replace('{', '\{').Replace('}', '\}')
    $body = ($escaped -split "`r?`n") -join "\par`r`n"
    "{\rtf1\ansi\deff0{\fonttbl{\f0 Segoe UI;}}\fs18`r`n$body\par`r`n}"
}

function Find-SignTool {
    $kits = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\bin'
    $tool = Get-ChildItem $kits -Recurse -Filter signtool.exe -ErrorAction SilentlyContinue |
        Where-Object { $_.Directory.Name -eq 'x64' } |
        Sort-Object { $_.Directory.Parent.Name } -Descending |
        Select-Object -First 1
    if (-not $tool) { throw 'signtool.exe was not found in the Windows SDK' }
    $tool.FullName
}

function Get-SigningCert {
    if (-not $env:MSHELL_SIGN_PFX_BASE64) {
        Write-Warning 'MSHELL_SIGN_PFX_BASE64 is not set, so the binaries and the installer stay unsigned'
        return $null
    }
    $pfx = Join-Path ([IO.Path]::GetTempPath()) "mshell-sign-$([guid]::NewGuid()).pfx"
    [IO.File]::WriteAllBytes($pfx, [Convert]::FromBase64String($env:MSHELL_SIGN_PFX_BASE64))
    @{ Path = $pfx; Password = $env:MSHELL_SIGN_PFX_PASSWORD; Tool = Find-SignTool }
}

function Invoke-Sign {
    param($Cert, [string[]]$Files, [string]$Description)
    if (-not $Cert -or -not $Files) { return }
    $passArgs = if ($Cert.Password) { @('/p', $Cert.Password) } else { @() }
    & $Cert.Tool sign /f $Cert.Path @passArgs /fd sha256 /td sha256 /tr $TimestampUrl /d $Description @Files
    if ($LASTEXITCODE) { throw "signtool failed ($LASTEXITCODE)" }
}

$bundled = @(
    Get-Companion 'mrun'
    Get-Companion 'mnotify'
    Get-Companion 'mcapture'
)
$bundled | Set-Content (Join-Path $Stage 'docs\BUNDLED.txt')

ConvertTo-Rtf (Get-Content (Join-Path $Stage 'docs\LICENSE') -Raw) |
    Set-Content (Join-Path $Stage 'license.rtf') -Encoding ascii

if (-not (Get-Command wix -ErrorAction SilentlyContinue)) {
    dotnet tool install --global wix --version $WixVersion
    $env:PATH = "$env:PATH;$env:USERPROFILE\.dotnet\tools"
}
wix extension add -g "WixToolset.UI.wixext/$WixVersion" "WixToolset.Util.wixext/$WixVersion"
if ($LASTEXITCODE) { throw "wix extension add failed ($LASTEXITCODE)" }

$cert = Get-SigningCert
try {
    $unsigned = Get-ChildItem $Stage -Recurse -Include *.exe, *.dll |
        Where-Object { (Get-AuthenticodeSignature $_.FullName).Status -ne 'Valid' } |
        ForEach-Object FullName
    Invoke-Sign $cert $unsigned 'mshell'

    New-Item -ItemType Directory -Force (Split-Path -Parent $Out) | Out-Null
    wix build (Join-Path $here 'mshell.wxs') `
        -arch x64 `
        -d "Version=$Version" `
        -d "Stage=$Stage" `
        -ext WixToolset.UI.wixext `
        -ext WixToolset.Util.wixext `
        -o $Out
    if ($LASTEXITCODE) { throw "wix build failed ($LASTEXITCODE)" }

    Invoke-Sign $cert @((Resolve-Path $Out).Path) "mshell $Version"
} finally {
    if ($cert) { Remove-Item $cert.Path -Force -ErrorAction SilentlyContinue }
}

Write-Host "built $Out"
