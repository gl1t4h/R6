[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateNotNullOrEmpty()]
    [string] $CuePath,

    [Parameter(Mandatory = $false)]
    [string] $ManifestPath
)

$ErrorActionPreference = 'Stop'

$cue = Get-Item -LiteralPath $CuePath
$cueDirectory = $cue.DirectoryName
$cueText = Get-Content -LiteralPath $cue.FullName -Raw

$fileMatches = [regex]::Matches(
    $cueText,
    '(?im)^\s*FILE\s+"([^"]+)"\s+(?:BINARY|MOTOROLA|AIFF|WAVE|MP3)\s*$'
)

if ($fileMatches.Count -eq 0) {
    throw "No supported FILE entries were found in the CUE file."
}

$tracks = @()
foreach ($match in $fileMatches) {
    $relativeName = $match.Groups[1].Value
    $trackPath = Join-Path $cueDirectory $relativeName
    if (-not (Test-Path -LiteralPath $trackPath -PathType Leaf)) {
        throw "Referenced track is missing: $relativeName"
    }

    $track = Get-Item -LiteralPath $trackPath
    $tracks += [ordered]@{
        name = $relativeName
        bytes = $track.Length
        sha256 = (Get-FileHash -LiteralPath $track.FullName -Algorithm SHA256).Hash
    }
}

$result = [ordered]@{
    tool = 'R6 verify-disc'
    schema = 1
    cue_file_name = $cue.Name
    cue_sha256 = (Get-FileHash -LiteralPath $cue.FullName -Algorithm SHA256).Hash
    tracks = $tracks
    note = 'Metadata only. Disc contents are not copied into the repository.'
}

$json = $result | ConvertTo-Json -Depth 5

if ($ManifestPath) {
    $manifestDirectory = Split-Path -Parent $ManifestPath
    if ($manifestDirectory -and -not (Test-Path -LiteralPath $manifestDirectory)) {
        New-Item -ItemType Directory -Path $manifestDirectory -Force | Out-Null
    }
    Set-Content -LiteralPath $ManifestPath -Value $json -Encoding UTF8
    Write-Host "Wrote local manifest: $ManifestPath"
} else {
    $json
}

Write-Host "Verified $($fileMatches.Count) track file(s) without copying disc data."
