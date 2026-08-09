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

$pattern = Read-ProjectFile 'Dumper\Platform\Public\BytePattern.h'
$peImage = Read-ProjectFile 'Dumper\Platform\Private\PeImage.cpp'
$platformHeader = Read-ProjectFile 'Dumper\Platform\Private\PlatformWindows.h'
$platformPublic = Read-ProjectFile 'Dumper\Platform\Public\Platform.h'
$platform = Read-ProjectFile 'Dumper\Platform\Private\PlatformWindows.cpp'
$safeMemoryHeader = Read-ProjectFile 'Dumper\Runtime\SafeMemory.h'
$safeMemory = Read-ProjectFile 'Dumper\Runtime\SafeMemory.cpp'
$versionProbe = Read-ProjectFile 'Dumper\Runtime\EngineVersionProbe.cpp'
$main = Read-ProjectFile 'Dumper\Main.cpp'
$supportMatrix = Read-ProjectFile 'docs\SUPPORT_MATRIX.md'
$coreProject = Read-ProjectFile 'Dumper\UExplorerCore.vcxproj'
$harnessProject = Read-ProjectFile 'tests\core-harness\CoreHarness.vcxproj'
$harness = Read-ProjectFile 'tests\core-harness\main.cpp'

foreach ($token in @('std::from_chars', 'parsedPattern', 'remainingSkips', 'offset <= lastStart',
        'value < -1 || value > 0xFF')) {
    Assert-Contains $pattern $token 'Byte-pattern parser/scanner contract regressed.'
}

foreach ($token in @('IMAGE_FILE_MACHINE_AMD64', 'IMAGE_NT_OPTIONAL_HDR64_MAGIC',
        'SizeOfHeaders > optionalHeader.SizeOfImage', 'sectionSize > inspected.Size - section.VirtualAddress',
        'TryAdd(base, optionalHeader.SizeOfImage', 'PeImageError::AllocationFailed')) {
    Assert-Contains $peImage $token 'Bounded x64 PE inspection contract regressed.'
}

foreach ($token in @('VisitLoadedModules',
        'linkAddress - offsetof(LDR_DATA_TABLE_ENTRY, InMemoryOrderLinks)',
        'entry.InMemoryOrderLinks.Blink', 'offsetof(LDR_DATA_TABLE_ENTRY, DllBase) == 0x30',
        'GetModuleHandleA', 'InspectPeImage', 'FindPatternInReadableRange',
        'FindBytePatternOffset', 'TryMeasureStringWithSeh', 'CompareMemoryValueWithSeh',
        'ReadMemory', 'ValidateReadableMemory', 'GetProcAddress')) {
    Assert-Contains $platform $token 'Windows platform safety boundary regressed.'
}
foreach ($token in @('GetModuleLdrTableEntry', 'int CurrentSkips = 0',
        'reinterpret_cast<const LDR_DATA_TABLE_ENTRY*>(P)',
        'WinSectionInfo.SectionHeader->Misc.VirtualSize;`r`n`tconst uintptr_t')) {
    Assert-NotContains $platform $token 'Known unsafe Windows platform implementation returned.'
}

foreach ($text in @($platformHeader, $platformPublic)) {
    Assert-Contains $text '#error "UExplorer Core supports Windows x64 only."' `
        'The unsupported x86 build must fail explicitly.'
}
Assert-NotContains $platformPublic 'PLATFORM_WINDOWS32' 'Unsupported x86 capability marker returned.'
Assert-NotContains $platform '__readfsdword' 'Unsupported x86 TEB traversal returned.'
Assert-Contains $supportMatrix 'x86/Win32 is intentionally unsupported' `
    'Support matrix no longer states the x64-only boundary.'
foreach ($token in @('IsBadReadPtr(const uintptr_t Address, std::size_t Size)',
        'IsBadReadPtr(const void* Address, std::size_t Size)')) {
    Assert-Contains $platformHeader $token 'Range-aware readable-memory interface regressed.'
}
foreach ($token in @('ValidateReadableMemory', 'AllocationFailed')) {
    Assert-Contains $safeMemoryHeader $token 'SafeMemory readable-range contract regressed.'
}
foreach ($token in @('ValidateReadableMemory', 'MemoryError::AllocationFailed')) {
    Assert-Contains $safeMemory $token 'SafeMemory readable-range implementation regressed.'
}

foreach ($token in @('image.Sections', 'section.IsReadable()', 'kReadChunkBytes',
        'kWindowOverlapBytes', 'ReadMemory', 'EngineVersionProbeError::MemoryReadFailed',
        'firstMemoryFailure', 'inspected.NativeError')) {
    Assert-Contains $versionProbe $token 'Bounded engine-version probing regressed.'
}
Assert-Contains $main 'ProbeLoadedEngineVersion' 'Startup no longer uses the bounded engine-version probe.'
Assert-Contains $main 'probe.MemoryFailure' 'Startup version diagnostics lost their typed memory failure.'
Assert-NotContains $main 'TryProbeEngineVersionFromImage' 'Whole-image startup scanner returned.'
Assert-NotContains $main 'OptionalHeader.SizeOfImage' 'Startup must not scan a raw SizeOfImage range.'

foreach ($token in @('Platform\Private\PeImage.cpp', 'Runtime\EngineVersionProbe.cpp')) {
    Assert-Contains $coreProject $token 'Core project omitted a platform safety implementation.'
    Assert-Contains $harnessProject "..\..\Dumper\$token" 'Core harness omitted a platform safety implementation.'
}
foreach ($token in @('TestBytePatternScanner', 'final candidate offset',
        'TestPeImageInspectionAndEngineVersionProbe', 'Out-of-image PE section was accepted',
        'Unreadable PE section did not return a typed probe error',
        'SafeMemory missed a no-access page inside the requested range')) {
    Assert-Contains $harness $token 'Platform safety harness coverage regressed.'
}

Write-Host 'Platform safety contract verified: x64 PE/LDR bounds, range-safe memory, pattern scanning, and readable-section version probing.'
