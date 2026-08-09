$ErrorActionPreference = 'Stop'

$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$planPath = Join-Path $root 'REFACTOR_PLAN.md'
$statusPath = Join-Path $root 'docs\issue-status.json'

$plan = Get-Content -LiteralPath $planPath -Raw -Encoding UTF8
$matches = [regex]::Matches($plan, '(?m)^\| ([A-Z]+-[0-9]+) \|')
$ids = @($matches | ForEach-Object { $_.Groups[1].Value })
$uniqueIds = @($ids | Sort-Object -Unique)

if ($ids.Count -eq 0) {
    throw 'No issue IDs were found in REFACTOR_PLAN.md'
}
if ($ids.Count -ne $uniqueIds.Count) {
    $duplicates = $ids | Group-Object | Where-Object Count -gt 1 | Select-Object -ExpandProperty Name
    throw "Duplicate issue IDs: $($duplicates -join ', ')"
}

$status = Get-Content -LiteralPath $statusPath -Raw -Encoding UTF8 | ConvertFrom-Json
$allowedStatuses = @($status.allowed_statuses)
$knownIds = [System.Collections.Generic.HashSet[string]]::new([string[]]$uniqueIds)

foreach ($id in $uniqueIds) {
    $prefix = $id.Split('-')[0]
    if ($null -eq $status.owners_by_prefix.$prefix) {
        throw "Issue $id has no owner mapping for prefix $prefix"
    }
}

foreach ($entry in $status.overrides.PSObject.Properties) {
    if (-not $knownIds.Contains($entry.Name)) {
        throw "Unknown issue override: $($entry.Name)"
    }
    if ($allowedStatuses -notcontains $entry.Value.status) {
        throw "Invalid status '$($entry.Value.status)' for $($entry.Name)"
    }
    if ($null -eq $entry.Value.verification) {
        throw "Issue override $($entry.Name) has no verification field"
    }
}

Write-Host "Issue register verified: $($uniqueIds.Count) unique issues, all with resolved owners and statuses."
