$ErrorActionPreference = 'Stop'
function Get-Sha256([byte[]]$Bytes) {
    $sha = [Security.Cryptography.SHA256]::Create()
    try { [BitConverter]::ToString($sha.ComputeHash($Bytes)).Replace('-', '').ToLowerInvariant() }
    finally { $sha.Dispose() }
}
# One key for a build output: the content of every input file, plus a variant
# string for what is not a file (build flags, compiler version). The same
# commit can be checked out with CRLF on Windows and LF elsewhere, so text is
# hashed with LF endings. A file with a NUL byte is binary to git as well and
# is never converted, so it is hashed as it is.
function Get-PrebuiltKey([string]$SourceRoot, [string[]]$RelativePaths, [string]$Variant) {
    $bytewise = [Text.Encoding]::GetEncoding(28591)
    $paths = @($RelativePaths | Where-Object { $_ } | ForEach-Object { $_.Replace('\', '/') } | Select-Object -Unique)
    # Ordinal, so the order does not depend on the machine's language.
    [Array]::Sort($paths, [StringComparer]::Ordinal)
    $lines = foreach ($relative in $paths) {
        $text = $bytewise.GetString([IO.File]::ReadAllBytes((Join-Path $SourceRoot $relative)))
        if (!$text.Contains([char]0)) { $text = $text.Replace("`r`n", "`n") }
        "$relative $(Get-Sha256 $bytewise.GetBytes($text))"
    }
    Get-Sha256 ([Text.Encoding]::UTF8.GetBytes((@($lines) + "VARIANT $Variant") -join "`n"))
}
