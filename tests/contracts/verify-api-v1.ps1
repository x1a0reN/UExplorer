$ErrorActionPreference = 'Stop'

$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$snapshotPath = Join-Path $PSScriptRoot 'api-v1-routes.tsv'
$apiPath = Join-Path $root 'Dumper\API'

$expectedRows = @(Import-Csv -LiteralPath $snapshotPath -Delimiter "`t")
$expected = @($expectedRows | ForEach-Object { "$($_.method.ToUpperInvariant()) $($_.path)" } | Sort-Object)
$actual = @(
    Get-ChildItem -LiteralPath $apiPath -Filter '*.cpp' | ForEach-Object {
        Get-Content -LiteralPath $_.FullName -Encoding UTF8 | ForEach-Object {
            if ($_ -match 'server\.(Get|Post|Patch|Delete)\("([^"]+)"') {
                "$($Matches[1].ToUpperInvariant()) $($Matches[2])"
            }
        }
    } | Sort-Object
)

if ($expected.Count -ne $actual.Count) {
    throw "API v1 route count changed: expected $($expected.Count), actual $($actual.Count)"
}

$difference = @(Compare-Object -ReferenceObject $expected -DifferenceObject $actual)
if ($difference.Count -gt 0) {
    $details = $difference | ForEach-Object { "$($_.SideIndicator) $($_.InputObject)" }
    throw "API v1 contract drift detected:`n$($details -join "`n")"
}

$missingDisposition = @($expectedRows | Where-Object { [string]::IsNullOrWhiteSpace($_.v2_disposition) })
if ($missingDisposition.Count -gt 0) {
    throw 'Every API v1 route must have an explicit v2 disposition'
}

Write-Host "API v1 contract verified: $($actual.Count) routes with explicit v2 dispositions."
