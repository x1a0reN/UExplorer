$ErrorActionPreference = 'Stop'

$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$rustPath = Join-Path $root 'frontend\src-tauri\src\lib.rs'
$apiPath = Join-Path $root 'frontend\src\api\client.ts'
$selectorPath = Join-Path $root 'frontend\src\components\ProcessSelector.tsx'
$fixtureSourcePath = Join-Path $root 'tests\injection-fixture\InjectionCoreFixture.cpp'
$fixtureTestPath = Join-Path $root 'frontend\src-tauri\tests\injection_process_fixture.rs'
$workflowPath = Join-Path $root '.github\workflows\ci.yml'

$rust = Get-Content -LiteralPath $rustPath -Raw -Encoding UTF8
$api = Get-Content -LiteralPath $apiPath -Raw -Encoding UTF8
$selector = Get-Content -LiteralPath $selectorPath -Raw -Encoding UTF8
$fixtureSource = Get-Content -LiteralPath $fixtureSourcePath -Raw -Encoding UTF8
$fixtureTest = Get-Content -LiteralPath $fixtureTestPath -Raw -Encoding UTF8
$workflow = Get-Content -LiteralPath $workflowPath -Raw -Encoding UTF8

$forbiddenRust = @(
    'PROCESS_ALL_ACCESS',
    'CREATE_SUSPENDED',
    'ResumeThread',
    'load_runtime_endpoint',
    'runtime.ini'
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
    'DllLoadState::Indeterminate',
    'DLL_MODULE_NOT_FOUND',
    'PROCESS_IDENTITY_MISMATCH',
    'TARGET_ARCH_MISMATCH',
    'async fn inject_and_connect',
    'State<''_, Arc<SessionManager>>',
    'State<''_, Arc<TargetOperationCoordinator>>',
    'MAX_CONCURRENT_TARGET_OPERATIONS: usize = 16',
    'TARGET_OPERATION_IN_PROGRESS',
    'spawn_blocking',
    'validated_target_identity',
    '"post_load_identity"',
    '"post_connect_identity"',
    'manager.connect(',
    'PipeConnectionState',
    'CoreReadinessState',
    'let sessions = Arc::new(SessionManager::new())',
    '.manage(sessions)',
    '.manage(Arc::new(TargetOperationCoordinator::default()))',
    'injection_result_serializes_independent_stage_states',
    'injection_admission_is_pid_scoped_and_released_by_raii',
    'session_connection_failures_have_stable_stage_classification'
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
foreach ($token in @('inject_and_connect', 'expectedStartTime100ns', 'expectedProcessPath')) {
    if (-not $injectMethod.Value.Contains($token)) {
        throw "Injection frontend contract is missing '$token'"
    }
}
foreach ($token in @('tryAdoptRuntimeEndpoint', 'load_runtime_endpoint', 'endpointRecovering')) {
    if ($api.Contains($token)) {
        throw "Frontend reintroduced implicit endpoint fallback '$token'"
    }
}

if ($selector.Contains('isUEProcessCandidate')) {
    throw 'React must not maintain a second hidden UE process filter'
}
foreach ($token in @(
        "result.status === 'ready'", "result.pipe === 'connected'",
        "result.core === 'ready'", "result.session?.target_pid === selectedProcess.pid",
        "result.session.phase === 'ready'", 'onCoreReady')) {
    if (-not $selector.Contains($token)) {
        throw "The UI readiness gate is missing '$token'"
    }
}
foreach ($token in @('onInjectSuccess', 'onDllLoaded')) {
    if ($selector.Contains($token)) {
        throw "The UI must not report readiness through legacy callback '$token'"
    }
}

foreach ($token in @(
        'RunFixtureServer', 'FrameKind::Hello', 'FrameKind::Welcome',
        'FrameKind::Shutdown', 'UEXPLORER_FIXTURE_DELAY_MS',
        'UEXPLORER_FIXTURE_REJECT_LOAD')) {
    if (-not $fixtureSource.Contains($token)) {
        throw "Live injection Core fixture is missing '$token'"
    }
}
foreach ($token in @(
        'inject_and_connect_target', 'CORE_READY', 'CORE_ALREADY_READY',
        'PROCESS_IDENTITY_MISMATCH', 'DLL_PE_INVALID', 'TARGET_ARCH_MISMATCH',
        'LOAD_LIBRARY_RETURNED_NULL', 'REMOTE_THREAD_TIMEOUT',
        'DllLoadState::Indeterminate')) {
    if (-not $fixtureTest.Contains($token)) {
        throw "Live injection matrix is missing '$token'"
    }
}
foreach ($token in @(
        'Build live injection fixtures',
        '--test injection_process_fixture')) {
    if (-not $workflow.Contains($token)) {
        throw "CI does not enforce the live injection fixture token '$token'"
    }
}

Write-Host 'Injection safety contract verified: identity, architecture, permissions, wait/exit, cleanup, managed Pipe/Core readiness, UI gating, and live process fixtures are explicit.'
