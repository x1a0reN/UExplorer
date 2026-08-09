$ErrorActionPreference = 'Stop'

$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$rustPath = Join-Path $root 'frontend\src-tauri\src\lib.rs'
$apiPath = Join-Path $root 'frontend\src\api\index.ts'
$selectorPath = Join-Path $root 'frontend\src\components\ProcessSelector.tsx'

$rust = Get-Content -LiteralPath $rustPath -Raw -Encoding UTF8
$api = Get-Content -LiteralPath $apiPath -Raw -Encoding UTF8
$selector = Get-Content -LiteralPath $selectorPath -Raw -Encoding UTF8

$forbiddenRust = @(
    'PROCESS_ALL_ACCESS',
    'CREATE_SUSPENDED',
    'ResumeThread'
)
foreach ($token in $forbiddenRust) {
    if ($rust.Contains($token)) {
        throw "Unsafe injection regression: forbidden token '$token' is present"
    }
}

$requiredRust = @(
    'GetExitCodeThread',
    'expected_start_time_100ns',
    'PROCESS_CREATE_THREAD',
    'PROCESS_QUERY_INFORMATION',
    'module_path_is_loaded',
    'resolve_remote_load_library',
    'defer_injection_cleanup',
    'WAIT_TIMEOUT',
    'DLL_MODULE_NOT_FOUND',
    'PROCESS_IDENTITY_MISMATCH',
    'TARGET_ARCH_MISMATCH'
)
foreach ($token in $requiredRust) {
    if (-not $rust.Contains($token)) {
        throw "Injection safety contract is missing '$token'"
    }
}

$injectMethod = [regex]::Match(
    $api,
    '(?s)async injectDLL\(process: HostProcessInfo.*?^  \}',
    [System.Text.RegularExpressions.RegexOptions]::Multiline
)
if (-not $injectMethod.Success) {
    throw 'Could not locate the frontend injectDLL method'
}
foreach ($token in @('persistConnectionSettings', 'tryAdoptRuntimeEndpoint', 'setTimeout')) {
    if ($injectMethod.Value.Contains($token)) {
        throw "Injection must not imply Core readiness through '$token'"
    }
}

if ($selector.Contains('isUEProcessCandidate')) {
    throw 'React must not maintain a second hidden UE process filter'
}
if ($selector.Contains('onInjectSuccess')) {
    throw 'The UI must distinguish DLL loaded from Core ready'
}

Write-Host 'Injection safety contract verified: identity, architecture, permissions, wait/exit, cleanup, and UI readiness are explicit.'
