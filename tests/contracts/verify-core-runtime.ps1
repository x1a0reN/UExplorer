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
$gameThread = Read-ProjectFile 'Dumper\API\GameThreadQueue.h'
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
        'CoreRuntime became Ready without its pipe listener', 'Required capability loss left readiness true',
        'Shutdown coordinator ran twice')) {
    Assert-Contains $harness $token 'CoreRuntime harness coverage regressed.'
}

Write-Host 'Core runtime contract verified: immutable context, dependency capabilities, truthful readiness, request drain, and coordinated shutdown.'
