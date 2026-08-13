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

$gameThreadHeader = Read-ProjectFile 'Dumper\Runtime\GameThreadExecutor.h'
$gameThreadImplementation = Read-ProjectFile 'Dumper\Runtime\GameThreadExecutor.cpp'
$gameThreadAdapter = Read-ProjectFile 'Dumper\API\GameThreadQueue.h'
$gameThread = $gameThreadHeader + $gameThreadImplementation
$commandService = Read-ProjectFile 'Dumper\Services\CoreCommandService.cpp'
$capabilities = Read-ProjectFile 'Dumper\Runtime\CoreCapabilities.h'
$callApi = Read-ProjectFile 'Dumper\API\CallApi.cpp'
$hookApi = Read-ProjectFile 'Dumper\API\HookApi.cpp'
$processEventHook = Read-ProjectFile 'Dumper\Services\ProcessEventHookOwner.cpp'
$postRenderHook = Read-ProjectFile 'Dumper\Runtime\PostRenderHook.cpp'
$callbackBarrier = Read-ProjectFile 'Dumper\Runtime\CallbackBarrier.h'
$vtableHook = Read-ProjectFile 'Dumper\Runtime\VTableHook.cpp'
$dumpApi = Read-ProjectFile 'Dumper\API\DumpApi.cpp'
$httpServer = Read-ProjectFile 'Dumper\Server\HttpServer.cpp'
$main = Read-ProjectFile 'Dumper\Main.cpp'
$mapping = Read-ProjectFile 'Dumper\Generator\Private\Generators\MappingGenerator.cpp'
$usmapContainer = Read-ProjectFile 'Dumper\Generator\Public\Generators\UsmapContainer.h'
$settings = Read-ProjectFile 'Dumper\Settings.h'
$statusApi = Read-ProjectFile 'Dumper\API\StatusApi.cpp'
$worldApi = Read-ProjectFile 'Dumper\API\WorldApi.cpp'
$memoryPage = Read-ProjectFile 'frontend\src\pages\Memory.tsx'
$functionsPage = Read-ProjectFile 'frontend\src\pages\Functions.tsx'
$domainService = Read-ProjectFile 'frontend\src-tauri\src\services\domain_service.rs'
$dumpPage = Read-ProjectFile 'frontend\src\pages\SDKDump.tsx'

foreach ($token in @('std::deque<std::shared_ptr<GameThreadTaskControl>>', 'kCapacity = 128',
        'GameThreadTicket', 'GameThreadCancelResult', 'Deadline', 'TimedOutBeforeStart',
        'TimedOutWhileRunning', 'GameThreadSubmitResult::Cancelled', '__try',
        'PumpThreadWaitDenied', 'IsCurrentPumpThread()', 'DisableAndDrain', 'IGameThreadPump',
        'PostRenderPumpBackend')) {
    Assert-Contains $gameThread $token 'Game-thread ownership/state contract regressed.'
}
Assert-NotContains $gameThread 'g_Pending' 'The single borrowed task slot must not return.'
Assert-NotContains $gameThreadAdapter 'std::deque<' 'Legacy API adapter must not own a second executor queue.'

foreach ($token in @('use_game_thread=false is disabled', 'INVALID_PARAM_SIZE', 'Too many object_indices (max 64)')) {
    Assert-Contains $callApi $token 'Function-call safety contract regressed.'
}
Assert-NotContains $callApi 'params.resize(256)' 'Parameter buffer size must never be guessed.'

foreach ($token in @('CallbackBarrier', 'BeginStopping', 'WaitForDrain', 'DisableAndDrain',
        'VTableHookToken::Install', 'g_PEPatches', 'g_PostRenderPatch', 'DetachPostRenderPatch',
        'unload is unsafe', 'ProcessEvent monitoring is installed lazily',
        'kLegacyHookMonitoringEnabled = false')) {
    Assert-Contains $hookApi $token 'Hook lifecycle contract regressed.'
}
foreach ($token in @('PostRenderHook::Install', 'PostRenderHook::Stop',
        'VTableHookToken::Install', 'm_CallbackBarrier.BeginStopping()',
        'm_CallbackBarrier.WaitForDrain(timeout)', 'DisableAndDrain',
        's_Active.compare_exchange_strong', 'unload is unsafe')) {
    Assert-Contains $postRenderHook $token 'Production PostRender pump ownership or drain contract regressed.'
}
foreach ($token in @('OwnedWorkAllowed', 'm_InFlight.fetch_add', 'm_InFlight.fetch_sub',
        'quietPeriod', 'm_ActivitySequence')) {
    Assert-Contains $callbackBarrier $token 'Callback drain barrier regressed.'
}
foreach ($token in @('~VTableHookToken', 'CompareExchangePointer', 'MemoryError::ValueMismatch',
        'm_Active = false', 'VTABLE_HOOK_RESTORE_FAILED')) {
    Assert-Contains $vtableHook $token 'VTable Hook RAII ownership regressed.'
}
foreach ($token in @('g_PostRenderHookInstalled', 'g_PEHookInstalled', 'g_PatchedPESlots',
        'CallbackGuard', 'WaitForCallbacks')) {
    Assert-NotContains $hookApi $token 'Manual Hook ownership state must not return.'
}

Assert-NotContains $httpServer '.detach()' 'HTTP workers must remain joinable.'
Assert-NotContains $httpServer '27016, 27017, 27018' 'Exact-port bind must not silently fall back.'
foreach ($token in @('SO_EXCLUSIVEADDRUSE', 'SO_RCVTIMEO', 'SO_SNDTIMEO',
        'ClientThreads', 'worker.join()', 'WS_CONSOLE_DISABLED')) {
    Assert-Contains $httpServer $token 'Temporary HTTP safety contract regressed.'
}
$sendCalls = [regex]::Matches($httpServer, '(?m)\bsend\s*\(').Count
if ($sendCalls -ne 1) {
    throw "All socket writes must pass through SendAll; found $sendCalls direct send calls"
}

Assert-Contains $main 'if (!unloadSafe)' 'Unsafe shutdown must refuse DLL unload.'
foreach ($token in @('g_PostRenderHook->Install()', 'shutdown.AddStage("post_render_hook"',
		'g_PostRenderHook->Stop(std::chrono::milliseconds(5000))',
		'shutdown.AddStage("reflection_frame_client"',
		'DetachReflectionFrameClient(std::chrono::milliseconds(5000))')) {
    Assert-Contains $main $token 'Production PostRender lifecycle is not explicitly owned by Main.'
}
foreach ($token in @('HttpServer', 'RegisterAllRoutes', 'SetServer(', 'runtime.ini', 'connection.ini')) {
    Assert-NotContains $main $token 'Core entrypoint reintroduced a legacy transport/configuration path.'
}
Assert-NotContains $main 'Kismet.ProcessEvent' 'Startup worker must not invoke ProcessEvent directly.'

Assert-Contains $settings 'EUsmapCompressionMethod::None' 'USMAP compression header must match its payload.'
Assert-NotContains $mapping 'ZSTD_compress' 'Disabled Zstd must not leave a partial compression path.'
Assert-Contains $mapping 'Usmap::WriteUncompressed' 'MappingGenerator must use the tested USMAP container writer.'
foreach ($token in @('kCompressionNone = 0', 'WriteU32LittleEndian(header + 8, payloadSize)',
        'WriteU32LittleEndian(header + 12, payloadSize)')) {
    Assert-Contains $usmapContainer $token 'USMAP payload/header invariant is missing.'
}

Assert-Contains $statusApi 'status.reconnect' 'Status adapter must route reconnect through the command boundary.'
Assert-Contains $commandService 'RECONNECT_DISABLED' 'Unsafe global reconnect must remain disabled.'
Assert-NotContains $commandService 'ProcessEvent(' 'Domain command handlers must not call naked ProcessEvent.'
Assert-Contains $worldApi 'ACTOR_TRANSFORM_WRITE_DISABLED' 'Raw actor transform writes must remain disabled.'
Assert-NotContains $memoryPage "connectWebSocket('/ws/console'" 'The fake WebSocket console must not be reachable from the UI.'
Assert-NotContains $memoryPage "subscribeEventStream('/events/watches'" 'Polling-driven watches must not claim SSE real-time behavior.'
Assert-Contains $functionsPage 'subscribeSessionEvents' 'Hook events must use the owned Tauri Channel bridge.'
Assert-NotContains $functionsPage 'subscribeEventStream' 'Functions UI reintroduced the legacy event transport.'
Assert-Contains $domainService '"hook.add" => DomainRoute::Core("hook.add")' 'Hook commands must use the Core capability boundary.'
foreach ($token in @('ProcessEventHookOwner', 'g_ProcessEventHook->Reconcile',
        'shutdown.AddStage("process_event_hook"', 'g_ProcessEventHook->StopAndDrain')) {
    Assert-Contains $main $token 'Main lost production ProcessEvent hook ownership or ordered restoration.'
}
foreach ($token in @('VTableHookToken::Install', 'Callbacks.BeginStopping()',
        'Callbacks.WaitForDrain', 'const std::shared_ptr<State> state = s_Active.load',
        'TryReadObjectVTable', 'FindByFunctionAddress', 'enterPublished', 'TryPublish',
        'Engine->ValidateObjectHandle', 'Engine->Types().Current() != operation.Types')) {
    Assert-Contains $processEventHook $token 'Production ProcessEvent hook lost exact evidence, bounded callback, or drain behavior.'
}
Assert-Contains $capabilities 'probes.HookCommandServiceEnabled && probes.HookProducerInstalled' 'Unsafe Hook monitoring must remain producer-gated.'
Assert-Contains $capabilities 'HOOK_PRODUCER_NOT_READY' 'Incomplete Hook producer coverage must have a stable capability reason.'

foreach ($token in @('DUMP_EXECUTOR_BUSY', 'DUMP_OPTIONS_UNAVAILABLE', 'g_DumpThread',
        'g_DumpStoppedCV.wait_for')) {
    Assert-Contains $dumpApi $token 'Dump executor safety contract regressed.'
}
Assert-NotContains $dumpApi 'g_DumpThreads' 'Dump jobs must have one explicit executor owner.'
Assert-NotContains $dumpPage "'60%'" 'The UI must not report synthetic dump progress.'
Assert-NotContains $dumpPage 'include_packages' 'Unsupported dump options must not remain interactive.'

Write-Host 'Core safety contract verified: owned tasks, production PostRender/ProcessEvent drain, archived server safety, capability gates, and USMAP framing.'
