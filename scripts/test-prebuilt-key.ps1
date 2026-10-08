$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Prebuilt.ps1')
$fixture = Join-Path ([IO.Path]::GetTempPath()) ('sf4-prebuilt-key-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path (Join-Path $fixture 'src') | Out-Null
function Set-Fixture([string]$Text, [byte[]]$Icon) {
    [IO.File]::WriteAllText((Join-Path $fixture 'src/main.rs'), $Text)
    [IO.File]::WriteAllBytes((Join-Path $fixture 'icon.ico'), $Icon)
}
function Get-Key([string]$Variant = 'release') { Get-PrebuiltKey $fixture @('src/main.rs', 'icon.ico') $Variant }
[byte[]]$icon = 0, 1, 13, 10, 2
Set-Fixture "fn main() {`n}`n" $icon
$key = Get-Key
if ($key -notmatch '^[0-9a-f]{64}$') { throw "Key is not a registry tag: $key" }
if ((Get-PrebuiltKey $fixture @('icon.ico', 'src\main.rs', 'icon.ico') 'release') -ne $key) { throw 'Key depends on the order or spelling of the input list' }
Set-Fixture "fn main() {`r`n}`r`n" $icon
if ((Get-Key) -ne $key) { throw 'CRLF checkout of the same text changed the key' }
Set-Fixture "fn main() { }`n" $icon
if ((Get-Key) -eq $key) { throw 'Changed source kept the key' }
Set-Fixture "fn main() {`n}`n" ([byte[]](0, 1, 10, 2))
if ((Get-Key) -eq $key) { throw 'Changed binary input kept the key' }
Set-Fixture "fn main() {`n}`n" $icon
if ((Get-Key 'debug') -eq $key) { throw 'Changed variant kept the key' }
if ((Get-Key) -ne $key) { throw 'Key is not stable' }
Remove-Item -LiteralPath $fixture -Recurse -Force
Write-Output 'PASS prebuilt key: line endings, order, source, binary and variant'
