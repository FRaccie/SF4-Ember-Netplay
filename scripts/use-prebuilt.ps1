# Wraps one slow build step of a test or CI build. When an earlier build
# already made the output from the same inputs, it is fetched from the
# project's Harbor registry and the step is skipped. Otherwise the step runs
# and its output is uploaded for the next build. The build command is one
# value with "|" between its arguments.
#
# Release builds never come through here (scripts/build-current.ps1): a
# fetched file was made on another machine, and the build receipt may only
# vouch for what this build compiled.
param([Parameter(Mandatory)][string]$Repository, [Parameter(Mandatory)][string]$Output,
      [Parameter(Mandatory)][string]$SourceRoot, [Parameter(Mandatory)][string]$InputList,
      [Parameter(Mandatory)][string]$Variant, [Parameter(Mandatory)][string]$BuildCommand)
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
. (Join-Path $PSScriptRoot 'Prebuilt.ps1')
# The compiler supplies the linker and the static runtime inside the output.
if (!$env:VCToolsVersion) { throw 'Run inside the Visual Studio compiler environment: its version is part of the key.' }
$key = Get-PrebuiltKey $SourceRoot (Get-Content -LiteralPath $InputList) "$Variant msvc=$env:VCToolsVersion sdk=$env:WindowsSDKVersion"
# Every address is built here with https, so neither the file nor the
# credentials can travel in the clear, whatever scheme the registry names.
$registry, $path = $Repository.Split('/', 2)
$api = "https://$registry/v2/$path"
$manifestType = 'application/vnd.oci.image.manifest.v1+json'

# The registry is a shortcut, never a requirement: a missing key or any
# trouble reaching it ends in a normal build.
try {
    # The project is public, so Harbor gives anyone a token that can read it.
    $token = (Invoke-RestMethod -UseBasicParsing -Uri "https://$registry/service/token?service=harbor-registry&scope=repository:${path}:pull").token
    $read = @{Authorization = "Bearer $token"}
    $fetched = "$Output.fetched"
    New-Item -ItemType Directory -Force -Path (Split-Path $Output -Parent) | Out-Null
    Invoke-WebRequest -UseBasicParsing -Uri "$api/manifests/$key" -Headers ($read + @{Accept = $manifestType}) -OutFile $fetched
    $layers = @((Get-Content -LiteralPath $fetched -Raw | ConvertFrom-Json).layers)
    if ($layers.Count -ne 1 -or $layers[0].digest -notmatch '^sha256:[0-9a-f]{64}$') { throw 'unexpected manifest' }
    # The file is used only when its hash is the one the manifest names.
    Invoke-WebRequest -UseBasicParsing -Uri "$api/blobs/$($layers[0].digest)" -Headers $read -OutFile $fetched
    if ("sha256:$((Get-FileHash -LiteralPath $fetched -Algorithm SHA256).Hash)" -ne $layers[0].digest) { throw "content does not match $($layers[0].digest)" }
    Move-Item -LiteralPath $fetched -Destination $Output -Force
    Write-Output "Prebuilt: fetched ${Repository}:$key"
    exit 0
}
catch { Write-Output "Prebuilt: ${Repository}:$key not fetched ($($_.Exception.Message)); building it." }

$program, $arguments = $BuildCommand.Split('|')
& $program $arguments
if ($LASTEXITCODE) { exit $LASTEXITCODE }

if (!$env:HARBOR_ROBOT_USER -or !$env:HARBOR_ROBOT_TOKEN) {
    Write-Output 'Prebuilt: not uploaded (HARBOR_ROBOT_USER and HARBOR_ROBOT_TOKEN are not set).'
    exit 0
}
$write = @{Authorization = 'Basic ' + [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes("${env:HARBOR_ROBOT_USER}:${env:HARBOR_ROBOT_TOKEN}"))}
function Send-Blob([byte[]]$Bytes) {
    $digest = "sha256:$(Get-Sha256 $Bytes)"
    $location = "$((Invoke-WebRequest -UseBasicParsing -Method Post -Uri "$api/blobs/uploads/" -Headers $write).Headers.Location)"
    if (!$location.StartsWith("https://$registry/")) { throw "upload address is outside the registry: $location" }
    Invoke-WebRequest -UseBasicParsing -Method Put -Uri "$location&digest=$digest" -Headers $write -ContentType 'application/octet-stream' -Body $Bytes | Out-Null
    @{digest = $digest; size = $Bytes.Length}
}
try {
    $config = (Send-Blob ([Text.Encoding]::ASCII.GetBytes('{}'))) + @{mediaType = 'application/vnd.oci.empty.v1+json'}
    $layer = (Send-Blob ([IO.File]::ReadAllBytes($Output))) + @{mediaType = 'application/octet-stream'
        annotations = @{'org.opencontainers.image.title' = (Split-Path $Output -Leaf)}}
    $manifest = @{schemaVersion = 2; mediaType = $manifestType; artifactType = 'application/vnd.sf4ember.prebuilt'
        config = $config; layers = @($layer)} | ConvertTo-Json -Depth 4
    Invoke-WebRequest -UseBasicParsing -Method Put -Uri "$api/manifests/$key" -Headers $write -ContentType $manifestType -Body $manifest | Out-Null
    Write-Output "Prebuilt: uploaded ${Repository}:$key"
}
catch { Write-Warning "Prebuilt: upload of ${Repository}:$key failed ($($_.Exception.Message)); the next build will try again." }
exit 0
