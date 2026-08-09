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
$context = Read-ProjectFile 'Dumper\Runtime\EngineContext.h'
$capture = Read-ProjectFile 'Dumper\Runtime\EngineContextCapture.cpp'
$capabilities = Read-ProjectFile 'Dumper\Runtime\CoreCapabilities.h'
$shutdown = Read-ProjectFile 'Dumper\Runtime\ShutdownCoordinator.h'
$handleHeader = Read-ProjectFile 'Dumper\Runtime\ObjectHandle.h'
$handleImplementation = Read-ProjectFile 'Dumper\Runtime\ObjectHandle.cpp'
$callbackBarrier = Read-ProjectFile 'Dumper\Runtime\CallbackBarrier.h'
$safeMemoryHeader = Read-ProjectFile 'Dumper\Runtime\SafeMemory.h'
$safeMemory = Read-ProjectFile 'Dumper\Runtime\SafeMemory.cpp'
$vtableHook = Read-ProjectFile 'Dumper\Runtime\VTableHook.cpp'
$gameThread = Read-ProjectFile 'Dumper\API\GameThreadQueue.h'
$memoryApi = Read-ProjectFile 'Dumper\API\MemoryApi.cpp'
$objectsApi = Read-ProjectFile 'Dumper\API\ObjectsApi.cpp'
$hookApi = Read-ProjectFile 'Dumper\API\HookApi.cpp'
$callApi = Read-ProjectFile 'Dumper\API\CallApi.cpp'
$statusApi = Read-ProjectFile 'Dumper\API\StatusApi.cpp'
$main = Read-ProjectFile 'Dumper\Main.cpp'
$harness = Read-ProjectFile 'tests\core-harness\main.cpp'

foreach ($token in @('Created', 'Initializing', 'Ready', 'Failed', 'Stopping', 'Stopped',
        'RequestLease', 'CORE_STOPPING', 'WaitForRequests', 'ReadinessSatisfied',
        'std::shared_ptr<const EngineContext>')) {
    Assert-Contains $runtime $token 'CoreRuntime state/ownership contract regressed.'
}

foreach ($token in @('Generation()', 'OffsetReport', 'Required engine offsets are not validated',
        'std::shared_ptr<const EngineContext>')) {
    Assert-Contains $context $token 'Immutable EngineContext contract regressed.'
}

foreach ($token in @('gobjects', 'process_event.index', 'positive_member_offset',
        'count_within_capacity', 'validated_ini_override')) {
    Assert-Contains $capture $token 'Offset validation report regressed.'
}

foreach ($token in @('transport.named_pipe', 'PIPE_LISTENER_NOT_READY',
        'GAME_THREAD_PUMP_NOT_OBSERVED', 'GAME_THREAD_PUMP_STALLED', 'RequiredReadyCapabilities')) {
    Assert-Contains $capabilities $token 'Capability dependency/readiness contract regressed.'
}

Assert-Contains $shutdown 'SafeToUnload' 'ShutdownCoordinator must report unload safety.'
Assert-Contains $gameThread 'PumpThreadStable' 'Game-thread pump identity diagnostics are missing.'
Assert-Contains $gameThread 'LastPumpTickMonotonicUs' 'Game-thread liveness diagnostics are missing.'

foreach ($token in @('CheckedAddressRange', 'ReadMemory', 'WriteMemory', 'CompareExchangePointer',
        'AllowExecutableWrite', 'FlushInstructionCache')) {
    Assert-Contains $safeMemoryHeader $token 'SafeMemory public contract regressed.'
}

foreach ($token in @('SessionId', 'ContextGeneration', 'SerialNumber', 'Address',
        'ClassFingerprint', 'FunctionHandle', 'IHandleIdentitySource', 'ValidateObject', 'ValidateFunction')) {
    Assert-Contains $handleHeader $token 'Stable object/function handle contract regressed.'
}
foreach ($token in @('HANDLE_SESSION_MISMATCH', 'HANDLE_CONTEXT_GENERATION_MISMATCH',
        'HANDLE_SERIAL_MISMATCH', 'FUNCTION_HANDLE_OWNER_MISMATCH',
        'CompareIdentity(handle.Function', 'CompareIdentity(handle.Owner')) {
    Assert-Contains $handleImplementation $token 'Execution-point handle validation regressed.'
}
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
        'last_tick_monotonic_us', 'queue_depth')) {
    Assert-Contains $statusApi $token 'Status API does not expose truthful runtime state.'
}
Assert-NotContains $statusApi 'Off::' 'Status handlers must read the immutable EngineContext, not raw offset globals.'
Assert-NotContains $statusApi 'Settings::' 'Status handlers must read the immutable EngineContext, not mutable settings globals.'
Assert-NotContains $statusApi 'ObjectArray::' 'Status handlers must not query the live object array from HTTP workers.'

foreach ($token in @('TestEngineContextAndCapabilities', 'TestCoreRuntimeStateAndShutdown',
        'TestStableObjectAndFunctionHandles', 'Object handle crossed a session boundary',
        'Recycled object slot retained a valid handle', 'Function handle ignored owner recycling',
        'TestHookOwnershipAndCallbackDrain', 'Failed VTable restore discarded hook ownership',
        'Post-stop callback was allowed to run owned work',
        'TestSafeMemory', 'ExecutableWriteDenied', 'InstructionCacheFlushRequired',
        'CoreRuntime became Ready without its pipe listener', 'Required capability loss left readiness true',
        'Shutdown coordinator ran twice')) {
    Assert-Contains $harness $token 'CoreRuntime harness coverage regressed.'
}

$payloadSchema = Get-Content -LiteralPath (Join-Path $root 'protocol\v1\schema\payload.schema.json') -Raw -Encoding UTF8 | ConvertFrom-Json
$objectHandleFixture = Get-Content -LiteralPath (Join-Path $root 'protocol\v1\fixtures\object-handle.json') -Raw -Encoding UTF8 | ConvertFrom-Json
$functionHandleFixture = Get-Content -LiteralPath (Join-Path $root 'protocol\v1\fixtures\function-handle.json') -Raw -Encoding UTF8 | ConvertFrom-Json
if ($null -eq $payloadSchema.'$defs'.objectHandle -or $null -eq $payloadSchema.'$defs'.functionHandle) {
    throw 'IPC payload schema does not define stable object/function handles.'
}
if ($objectHandleFixture.serial -le 0 -or $objectHandleFixture.context_generation -le 0) {
    throw 'Object handle fixture lacks a positive serial or context generation.'
}
if ($functionHandleFixture.function.session_id -ne $functionHandleFixture.owner.session_id -or
        $functionHandleFixture.function.context_generation -ne $functionHandleFixture.owner.context_generation) {
    throw 'Function handle fixture crosses a session or context generation.'
}

Write-Host 'Core runtime contract verified: immutable context, stable handles, capability readiness, SafeMemory, request drain, and coordinated shutdown.'
