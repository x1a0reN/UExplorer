$ErrorActionPreference = 'Stop'

$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path

function Read-ProjectFile([string]$relativePath) {
    return Get-Content -LiteralPath (Join-Path $root $relativePath) -Raw -Encoding UTF8
}

function Assert-Contains([string]$text, [string]$token, [string]$message) {
    if (-not $text.Contains($token)) {
        throw "$message Missing token: $token"
    }
}

function Assert-NotContains([string]$text, [string]$token, [string]$message) {
    if ($text.Contains($token)) {
        throw "$message Forbidden token: $token"
    }
}

$discoveryHeader = Read-ProjectFile 'Dumper\Engine\Public\OffsetFinder\OffsetDiscovery.h'
$discovery = Read-ProjectFile 'Dumper\Engine\Private\OffsetFinder\OffsetDiscovery.cpp'
$offsets = Read-ProjectFile 'Dumper\Engine\Private\OffsetFinder\Offsets.cpp'
$finder = Read-ProjectFile 'Dumper\Engine\Private\OffsetFinder\OffsetFinder.cpp'
$context = Read-ProjectFile 'Dumper\Runtime\EngineContext.h'
$capture = Read-ProjectFile 'Dumper\Runtime\EngineContextCapture.cpp'
$commands = Read-ProjectFile 'Dumper\Services\CoreCommandService.cpp'
$harness = Read-ProjectFile 'tests\core-harness\main.cpp'
$project = Read-ProjectFile 'Dumper\UExplorerCore.vcxproj'
$harnessProject = Read-ProjectFile 'tests\core-harness\CoreHarness.vcxproj'

foreach ($token in @(
        'GlobalPointerCandidateObservation',
        'GlobalPointerCandidateEvidence',
        'AmbiguousCandidates',
        'CandidateLimitExceeded',
        'ResolveGlobalPointerCandidates')) {
    Assert-Contains ($discoveryHeader + $discovery) $token 'Global-pointer discovery contract regressed.'
}
foreach ($token in @(
        'candidate_slot_in_writable_non_executable_section',
		'candidate_slot_pointer_aligned',
        'expected_target_from_object_array',
        'expected_target_type_validated',
        'stable_target_value',
        'exactly_one_unique_candidate',
        'GLOBAL_POINTER_CANDIDATES_AMBIGUOUS')) {
    Assert-Contains ($discoveryHeader + $discovery) $token 'Global-pointer evidence or fail-closed diagnostics regressed.'
}
foreach ($token in @(
        'DiscoverGlobalPointer',
        'InspectLoadedPeImage',
        'Runtime::ReadValue',
		'TryReadFirstObjectVTable',
		'TryNarrowModuleOffset',
		'ProcessEvent VTable index is outside the validated range',
        'GetDiscoveryReport',
        'discovery failed closed')) {
    Assert-Contains $offsets $token 'Live GWorld/GEngine discovery bypassed validation.'
}
Assert-NotContains $offsets 'void** Vft = *(void***)ObjectArray::GetByIndex(0).GetAddress()' 'Raw ProcessEvent VTable discovery was reintroduced.'
foreach ($token in @(
        'Found {} candidates, using first',
        'Filter GActiveLogWorld')) {
    Assert-NotContains $offsets $token 'Ambiguous GWorld/GEngine selection was reintroduced.'
}
foreach ($token in @(
        'return Offset != OffsetNotFound ? Offset : 0x10',
        'assuming default layout')) {
    Assert-NotContains $finder $token 'Guessed offset fallback was reintroduced.'
}
foreach ($token in @('Candidates', 'Confidence')) {
    Assert-Contains $context $token 'Immutable offset evidence fields regressed.'
    Assert-Contains $capture $token 'Offset evidence capture regressed.'
    Assert-Contains $commands $token 'Offset evidence status serialization regressed.'
}
foreach ($token in @(
        'TestGlobalPointerDiscovery',
        'Multiple validated global-pointer slots did not fail closed',
        'Executable-section pointer candidate was accepted')) {
    Assert-Contains $harness $token 'Global-pointer discovery fixtures regressed.'
}
Assert-Contains $project 'OffsetDiscovery.cpp' 'Core project omitted offset discovery implementation.'
Assert-Contains $harnessProject 'OffsetDiscovery.cpp' 'Harness omitted offset discovery implementation.'

Write-Host 'Offset discovery contract verified: checked ProcessEvent/global candidates, explicit ambiguity, and audited default substitutions removed.'
