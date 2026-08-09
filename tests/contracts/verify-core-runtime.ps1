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

$runtime = Read-ProjectFile 'Dumper\Runtime\CoreRuntime.h'
$coreSession = Read-ProjectFile 'Dumper\Runtime\CoreSession.cpp'
$context = Read-ProjectFile 'Dumper\Runtime\EngineContext.h'
$capture = Read-ProjectFile 'Dumper\Runtime\EngineContextCapture.cpp'
$capabilities = Read-ProjectFile 'Dumper\Runtime\CoreCapabilities.h'
$shutdown = Read-ProjectFile 'Dumper\Runtime\ShutdownCoordinator.h'
$handleHeader = Read-ProjectFile 'Dumper\Runtime\ObjectHandle.h'
$handleImplementation = Read-ProjectFile 'Dumper\Runtime\ObjectHandle.cpp'
$identityLayout = Read-ProjectFile 'Dumper\Runtime\FUObjectItemLayout.cpp'
$identityContext = Read-ProjectFile 'Dumper\Runtime\ObjectIdentityContext.h'
$identitySource = Read-ProjectFile 'Dumper\Runtime\ObjectArrayIdentitySource.cpp'
$engineFacade = Read-ProjectFile 'Dumper\Runtime\EngineFacade.h'
$engineSnapshotHeader = Read-ProjectFile 'Dumper\Runtime\EngineSnapshot.h'
$engineSnapshot = Read-ProjectFile 'Dumper\Runtime\EngineSnapshot.cpp'
$snapshotCaptureHeader = Read-ProjectFile 'Dumper\Runtime\EngineSnapshotCapture.h'
$snapshotCapture = Read-ProjectFile 'Dumper\Runtime\EngineSnapshotCapture.cpp'
$objectArray = Read-ProjectFile 'Dumper\Engine\Private\Unreal\ObjectArray.cpp'
$callbackBarrier = Read-ProjectFile 'Dumper\Runtime\CallbackBarrier.h'
$safeMemoryHeader = Read-ProjectFile 'Dumper\Runtime\SafeMemory.h'
$safeMemory = Read-ProjectFile 'Dumper\Runtime\SafeMemory.cpp'
$vtableHook = Read-ProjectFile 'Dumper\Runtime\VTableHook.cpp'
$gameThreadHeader = Read-ProjectFile 'Dumper\Runtime\GameThreadExecutor.h'
$gameThreadImplementation = Read-ProjectFile 'Dumper\Runtime\GameThreadExecutor.cpp'
$gameThread = $gameThreadHeader + $gameThreadImplementation
$commandHeader = Read-ProjectFile 'Dumper\Services\CoreCommandService.h'
$commandImplementation = Read-ProjectFile 'Dumper\Services\CoreCommandService.cpp'
$commandService = $commandHeader + $commandImplementation
$memoryApi = Read-ProjectFile 'Dumper\API\MemoryApi.cpp'
$objectsApi = Read-ProjectFile 'Dumper\API\ObjectsApi.cpp'
$hookApi = Read-ProjectFile 'Dumper\API\HookApi.cpp'
$callApi = Read-ProjectFile 'Dumper\API\CallApi.cpp'
$statusApi = Read-ProjectFile 'Dumper\API\StatusApi.cpp'
$main = Read-ProjectFile 'Dumper\Main.cpp'
$harness = Read-ProjectFile 'tests\core-harness\main.cpp'

foreach ($token in @('Created', 'Initializing', 'Ready', 'Failed', 'Stopping', 'Stopped',
        'RequestLease', 'CORE_STOPPING', 'WaitForRequests', 'ReadinessSatisfied',
        'SessionId', 'BeginInitialize(std::string sessionId)',
        'std::shared_ptr<const EngineContext>')) {
    Assert-Contains $runtime $token 'CoreRuntime state/ownership contract regressed.'
}
foreach ($token in @('BCryptGenRandom', 'BCRYPT_USE_SYSTEM_PREFERRED_RNG', 'core-')) {
    Assert-Contains $coreSession $token 'Secure Core session generation regressed.'
}

foreach ($token in @('Generation()', 'OffsetReport', 'Required engine offsets are not validated',
        'std::shared_ptr<const EngineContext>')) {
    Assert-Contains $context $token 'Immutable EngineContext contract regressed.'
}

foreach ($token in @('gobjects', 'process_event.index', 'positive_member_offset',
        'count_within_capacity', 'validated_ini_override', 'fuobjectitem.serial_number')) {
    Assert-Contains $capture $token 'Offset validation report regressed.'
}

foreach ($token in @('transport.named_pipe', 'PIPE_LISTENER_NOT_READY', 'objects.identity_source',
		'functions.handles', 'FUNCTION_HANDLE_VALIDATION_NOT_READY',
        'GAME_THREAD_PUMP_NOT_OBSERVED', 'GAME_THREAD_PUMP_STALLED', 'RequiredReadyCapabilities')) {
    Assert-Contains $capabilities $token 'Capability dependency/readiness contract regressed.'
}

Assert-Contains $shutdown 'SafeToUnload' 'ShutdownCoordinator must report unload safety.'
Assert-Contains $gameThread 'PumpThreadStable' 'Game-thread pump identity diagnostics are missing.'
Assert-Contains $gameThread 'LastPumpTickMonotonicUs' 'Game-thread liveness diagnostics are missing.'
foreach ($token in @('GameThreadTaskTiming', 'TryGetTiming', 'PumpThreadWaitDenied',
        'releasedWork = std::move(task->Work)')) {
    Assert-Contains $gameThread $token 'Game-thread command timing/terminal ownership regressed.'
}

foreach ($token in @('status.inspect', 'status.engine', 'status.health',
        'objects.handle.issue', 'functions.handle.issue', 'SESSION_MISMATCH',
        'TryAcquireRequest', 'std::move(*lease)', 'm_GameThread.Enqueue',
        'onGameThreadQueued(ticket)', 'SerializeObjectHandle', 'SerializeFunctionHandle')) {
    Assert-Contains $commandService $token 'Transport-neutral Core command boundary regressed.'
}
Assert-NotContains $commandService 'HttpResponse' 'Core domain commands must not construct HTTP responses.'
Assert-NotContains $commandService 'HttpServer' 'Core domain commands must not depend on the legacy HTTP server.'

foreach ($token in @('CheckedAddressRange', 'ReadMemory', 'WriteMemory', 'CompareExchangePointer',
        'AllowExecutableWrite', 'FlushInstructionCache')) {
    Assert-Contains $safeMemoryHeader $token 'SafeMemory public contract regressed.'
}

foreach ($token in @('SessionId', 'ContextGeneration', 'SerialNumber', 'Address',
        'ClassFingerprint', 'FunctionHandle', 'IHandleIdentitySource',
		'ContextGeneration() const noexcept', 'IsCurrentExecutionThreadValid() const noexcept',
		'ValidateObject', 'ValidateFunction')) {
    Assert-Contains $handleHeader $token 'Stable object/function handle contract regressed.'
}
foreach ($token in @('HANDLE_SESSION_MISMATCH', 'HANDLE_CONTEXT_GENERATION_MISMATCH',
        'HANDLE_SERIAL_MISMATCH', 'FUNCTION_HANDLE_OWNER_MISMATCH',
		'HANDLE_EXECUTION_THREAD_INVALID', 'm_Source.ContextGeneration() == m_ContextGeneration',
        'IsCanonicalFunctionIdentityPath', 'CompareIdentity(handle.Function',
        'CompareIdentity(handle.Owner')) {
    Assert-Contains $handleImplementation $token 'Execution-point handle validation regressed.'
}
foreach ($token in @('epic_fuobjectitem_64_v1', 'exact_epic_object_offset',
        'supported_epic_item_size', 'positive_serial_witness',
        'FUOBJECTITEM_POSITIVE_SERIAL_NOT_OBSERVED')) {
    Assert-Contains $identityLayout $token 'FUObjectItem serial profile validation regressed.'
}
foreach ($token in @('CaptureObjectIdentityContext', 'CanIssueObjectHandles',
		'CanIssueFunctionHandles', 'FUObjectItemSerial', 'FNameComparisonIndex',
		'FunctionExec')) {
	Assert-Contains $identityContext $token 'Immutable object-identity offset context regressed.'
}
foreach ($token in @('TryReadIdentityCandidate', 'objectFirst != objectSecond', 'internalIndex != Index',
		'FUObjectItemSerialNumberOffset', 'count > capacity', 'Index >= count',
		'internalIndexOffset > (std::numeric_limits<uintptr_t>::max)() - objectAddress')) {
    Assert-Contains $objectArray $token 'Production FUObjectItem identity reads regressed.'
}
foreach ($token in @('IsCurrentExecutionThreadValid', 'executor.IsCurrentPumpThread()',
        'TryReadObjectCore', 'TryReadCanonicalFNameToken', 'TryBuildCanonicalFunctionPath',
		'm_Offsets.ObjectClass', 'm_Offsets.FunctionExec',
		'finalPath != fullPath', 'SignatureFingerprint')) {
    Assert-Contains $identitySource $token 'Production object/function identity source regressed.'
}
Assert-NotContains $identitySource 'Off::' 'Production identity validation must use its immutable context, not mutable offset globals.'
foreach ($token in @('ObjectHandleService', 'EngineSnapshotStore', 'EngineSnapshotCapture',
		'ConfigureSnapshotCapture', 'IssueObjectHandle', 'ValidateFunctionHandle',
		'std::shared_ptr<const EngineContext>')) {
	Assert-Contains $engineFacade $token 'EngineFacade ownership boundary regressed.'
}
foreach ($token in @('EngineSnapshotObject', 'SessionId', 'ContextGeneration', 'Generation',
		'SourceObjectCount', 'SkippedSlots', 'std::atomic<std::shared_ptr<const EngineSnapshot>>')) {
	Assert-Contains $engineSnapshotHeader $token 'Immutable EngineSnapshot contract regressed.'
}
foreach ($token in @('SNAPSHOT_GENERATION_NOT_MONOTONIC', 'SNAPSHOT_RECORDS_NOT_ORDERED',
		'IsValidHandleEnvelope', 'IsValidMetadata', 'm_Current.store', 'm_Current.load')) {
	Assert-Contains $engineSnapshot $token 'Atomic EngineSnapshot publication regressed.'
}
foreach ($token in @('IEngineSnapshotSource', 'kDefaultPumpBudget', 'kMaxPumpBudget',
		'Capturing', 'Validating', 'Publishing', 'StopAndDrain', 'CallbackBarrier')) {
	Assert-Contains $snapshotCaptureHeader $token 'Incremental snapshot capture contract regressed.'
}
foreach ($token in @('m_PumpOwned.test_and_set', 'SnapshotSlotReadResult::Empty',
		'ValidateSlot(index, expected)', 'PublishedObjects.reserve',
		'SnapshotCaptureError::SourceValidationFailed', 'm_PumpBarrier.WaitForDrain')) {
	Assert-Contains $snapshotCapture $token 'Budgeted snapshot capture implementation regressed.'
}
Assert-NotContains $snapshotCapture 'ObjectArray::' 'Generic snapshot scheduling must not bypass its source boundary.'
Assert-NotContains $snapshotCapture 'Off::' 'Generic snapshot scheduling must not read mutable engine offsets.'
foreach ($token in @('CALL_HANDLE_REQUIRED', 'SESSION_SERIAL_OBJECT_AND_FUNCTION_HANDLES_REQUIRED',
        'server.Post("/api/v1/call/function"',
        'server.Post("/api/v1/call/static"',
        'server.Post("/api/v1/call/batch"')) {
    Assert-Contains $callApi $token 'Legacy index-only call path became reachable.'
}
$callHandleGateCount = [regex]::Matches($callApi, 'return CallHandleRequired\(\);').Count
if ($callHandleGateCount -ne 3) {
    throw "Every legacy call route must return through the stable-handle gate; found $callHandleGateCount gates."
}
foreach ($token in @('VirtualQuery', 'CopyWithSeh', 'CompareExchangePointerWithSeh',
        'RestoreProtections', 'ExecutableWriteDenied', 'InstructionCacheFlushRequired')) {
    Assert-Contains $safeMemory $token 'SafeMemory implementation contract regressed.'
}
foreach ($token in @('std::from_chars', 'Too many offsets (max 64)', 'POINTER_CHAIN_OVERFLOW',
        'std::vector<std::int64_t>', 'Runtime::WriteMemory')) {
    Assert-Contains $memoryApi $token 'Memory API validation contract regressed.'
}
Assert-NotContains $memoryApi 'std::stoull' 'Memory API must fully parse addresses without exception-based partial conversion.'
Assert-Contains $objectsApi 'OBJECT_PROPERTY_WRITE_DISABLED' 'Unsafe raw UObject property writes became reachable.'
Assert-Contains $hookApi 'VTableHookToken::Install' 'Hook patching bypasses the RAII owner.'
Assert-Contains $vtableHook 'CompareExchangePointer' 'VTable Hook patching bypasses atomic SafeMemory.'
Assert-Contains $callbackBarrier 'WaitForDrain' 'Hook callback quiescence barrier is missing.'

foreach ($apiFile in Get-ChildItem -LiteralPath (Join-Path $root 'Dumper\API') -Filter '*.cpp') {
    $apiSource = Get-Content -LiteralPath $apiFile.FullName -Raw -Encoding UTF8
    Assert-NotContains $apiSource 'VirtualProtect' "API module $($apiFile.Name) bypasses SafeMemory."
}

foreach ($token in @('CaptureEngineContext', 'RefreshRuntimeCapabilities', 'ShutdownCoordinator',
        'BeginStopping', 'MarkStopped')) {
    Assert-Contains $main $token 'Main does not use the runtime ownership path.'
}

foreach ($token in @('liveness', 'readiness', 'offset_reports', 'capabilities', 'context_generation',
        'last_tick_monotonic_us', 'queue_depth', 'object_snapshot')) {
    Assert-Contains $commandService $token 'Core status command does not expose truthful runtime state.'
}
Assert-NotContains $statusApi 'Off::' 'Status handlers must read the immutable EngineContext, not raw offset globals.'
Assert-NotContains $statusApi 'Settings::' 'Status handlers must read the immutable EngineContext, not mutable settings globals.'
Assert-NotContains $statusApi 'ObjectArray::' 'Status handlers must not query the live object array from HTTP workers.'
foreach ($token in @('service->Execute', 'status.inspect', 'status.engine', 'status.health')) {
    Assert-Contains $statusApi $token 'Legacy status adapter bypassed the Core command service.'
}
Assert-NotContains $statusApi 'CoreRuntimeSnapshot' 'Status HTTP adapter must not duplicate runtime business logic.'

foreach ($token in @('TestEngineContextAndCapabilities', 'TestCoreRuntimeStateAndShutdown',
        'TestCoreSessionIdentity',
		'TestEngineFacadeAndImmutableSnapshots', 'Snapshot reader observed a torn generation',
		'Incomplete snapshot metadata was published as usable data',
		'TestIncrementalSnapshotCapture', 'Snapshot pump exceeded its per-frame work budget',
		'A slot mutation between capture and validation was published',
		'Concurrent snapshot pumps accessed one mutable working generation',
		'Snapshot source exception escaped the guarded pump boundary',
		'Snapshot producer crossed an identity-source context generation',
		'Snapshot shutdown ignored an in-flight pump',
        'TestCoreDomainCommandsAndHandleExecution', 'Handle command accepted transport-supplied identity fields',
		'Missing function metadata did not disable only function handles',
		'Function handle command ignored its dedicated capability',
        'Domain command ticket did not cancel queued work',
        'TestStableObjectAndFunctionHandles', 'Object handle crossed a session boundary',
        'Recycled object slot retained a valid handle', 'Function handle ignored owner recycling',
        'Non-canonical display path became a function execution identity',
        'TestFUObjectItemIdentityLayout', 'Custom FUObjectItem object offset was guessed',
        'Zero-only serial candidate was accepted',
        'TestHookOwnershipAndCallbackDrain', 'Failed VTable restore discarded hook ownership',
        'TestGenericGameThreadWorkAndCancellation', 'explicitly cancelled',
        'Post-stop callback was allowed to run owned work',
        'TestSafeMemory', 'ExecutableWriteDenied', 'InstructionCacheFlushRequired',
        'CoreRuntime became Ready without its pipe listener', 'Required capability loss left readiness true',
        'Shutdown coordinator ran twice')) {
    Assert-Contains $harness $token 'CoreRuntime harness coverage regressed.'
}

$payloadSchema = Get-Content -LiteralPath (Join-Path $root 'protocol\v1\schema\payload.schema.json') -Raw -Encoding UTF8 | ConvertFrom-Json
$objectHandleFixture = Get-Content -LiteralPath (Join-Path $root 'protocol\v1\fixtures\object-handle.json') -Raw -Encoding UTF8 | ConvertFrom-Json
$functionHandleFixture = Get-Content -LiteralPath (Join-Path $root 'protocol\v1\fixtures\function-handle.json') -Raw -Encoding UTF8 | ConvertFrom-Json
$objectHandleRequestFixture = Get-Content -LiteralPath (Join-Path $root 'protocol\v1\fixtures\object-handle-request.json') -Raw -Encoding UTF8 | ConvertFrom-Json
$objectHandleResponseFixture = Get-Content -LiteralPath (Join-Path $root 'protocol\v1\fixtures\object-handle-response.json') -Raw -Encoding UTF8 | ConvertFrom-Json
if ($null -eq $payloadSchema.'$defs'.objectHandle -or $null -eq $payloadSchema.'$defs'.functionHandle -or
        $null -eq $payloadSchema.'$defs'.handleIssueData -or $null -eq $payloadSchema.'$defs'.emptyCommandData) {
    throw 'IPC payload schema does not define stable object/function handles.'
}
if ($objectHandleFixture.serial -le 0 -or $objectHandleFixture.context_generation -le 0) {
    throw 'Object handle fixture lacks a positive serial or context generation.'
}
if ($functionHandleFixture.function.session_id -ne $functionHandleFixture.owner.session_id -or
        $functionHandleFixture.function.context_generation -ne $functionHandleFixture.owner.context_generation) {
    throw 'Function handle fixture crosses a session or context generation.'
}
if ($functionHandleFixture.full_path -notmatch '^Function fname:[0-9a-f]+:[0-9]+(?:\.fname:[0-9a-f]+:[0-9]+)+$') {
    throw 'Function handle fixture does not use the canonical raw-FName identity path.'
}
if ($objectHandleRequestFixture.operation -ne 'objects.handle.issue' -or
        $objectHandleRequestFixture.data.PSObject.Properties.Name.Count -ne 1 -or
        $objectHandleRequestFixture.data.index -ne $objectHandleResponseFixture.data.index -or
        $objectHandleRequestFixture.session_id -ne $objectHandleResponseFixture.session_id) {
    throw 'Object handle command fixtures do not preserve strict discovery input/session identity.'
}

Write-Host 'Core runtime contract verified: immutable context, stable handles, capability readiness, SafeMemory, request drain, and coordinated shutdown.'
