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

$serverHeader = Read-ProjectFile 'Dumper\IPC\NamedPipeRpcServer.h'
$server = Read-ProjectFile 'Dumper\IPC\NamedPipeRpcServer.cpp'
$protocol = Read-ProjectFile 'Dumper\IPC\Protocol.h'
$main = Read-ProjectFile 'Dumper\Main.cpp'
$coreProject = Read-ProjectFile 'Dumper\UExplorerCore.vcxproj'
$harnessProject = Read-ProjectFile 'tests\core-harness\CoreHarness.vcxproj'
$harness = Read-ProjectFile 'tests\core-harness\main.cpp'

foreach ($token in @(
        'kWorkerCount = 4', 'kPendingRequestLimit = 256', 'kIoChunkBytes = 64 * 1024',
        'bool OpenAdmissions()', 'bool Stop(std::chrono::milliseconds timeout)',
        'NamedPipeServerDiagnostics')) {
    Assert-Contains $serverHeader $token 'Named Pipe public lifecycle/bound contract regressed.'
}

foreach ($token in @(
        'CreateNamedPipeW', 'FILE_FLAG_OVERLAPPED', 'FILE_FLAG_FIRST_PIPE_INSTANCE',
        'PIPE_REJECT_REMOTE_CLIENTS', 'ConvertStringSecurityDescriptorToSecurityDescriptorW',
        'ImpersonateNamedPipeClient', 'RevertToSelf', 'EqualSid',
        'GetNamedPipeClientProcessId', 'ConnectNamedPipe', 'CancelIoEx',
        'CancelSynchronousIo', 'FlushFileBuffers', 'JoinOwnedThreads',
        'm_GameThread.Cancel', 'RPC_BACKPRESSURE', 'REQUEST_CANCELLED',
        'DEADLINE_EXPIRED', 'FrameKind::Welcome', 'FrameKind::Pong',
        'FrameKind::Shutdown', 'transport.named_pipe')) {
    Assert-Contains $server $token 'Named Pipe security, framing, cancellation, or shutdown contract regressed.'
}
Assert-NotContains $server '.detach(' 'Named Pipe transport reintroduced detached thread ownership.'
Assert-NotContains $server 'CreateThread(' 'Named Pipe transport bypassed joinable std::thread ownership.'

foreach ($token in @(
        'SetPayloadLimit', 'm_MaxPayloadSize', 'ProtocolError::DecoderFailed',
        'while (!remaining.empty())', 'header.PayloadLength > m_MaxPayloadSize')) {
    Assert-Contains $protocol $token 'C++ framing lost bounded negotiated streaming behavior.'
}
Assert-NotContains $protocol 'm_Buffer.insert(m_Buffer.end(), bytes.begin(), bytes.end())' 'Decoder copies an untrusted coalesced read before validating frames.'

foreach ($token in @(
        'g_PipeServer->Start()', 'probes.NamedPipeListening = g_PipeServer && g_PipeServer->IsListening()',
        'EnsurePipeAdmissions()', 'g_PipeServer->OpenAdmissions()',
        'shutdown.AddStage("named_pipe"', 'g_PipeServer->Stop(std::chrono::milliseconds(5000))')) {
    Assert-Contains $main $token 'Core entrypoint lost factual Pipe readiness or owned shutdown.'
}
$pipeStart = $main.IndexOf('g_PipeServer->Start()', [StringComparison]::Ordinal)
$hookStart = $main.IndexOf('UExplorer::API::InitHooks()', [StringComparison]::Ordinal)
if ($pipeStart -lt 0 -or $hookStart -lt 0 -or $pipeStart -ge $hookStart) {
    throw 'Core must bind the required Named Pipe before installing game-process hooks.'
}

foreach ($project in @($coreProject, $harnessProject)) {
    Assert-Contains $project 'NamedPipeRpcServer.cpp' 'A Windows build target omitted the real Pipe server.'
    Assert-Contains $project 'advapi32.lib' 'A Windows build target omitted Pipe security APIs.'
}

foreach ($token in @(
        'TestNamedPipeRpcServerLifecycle', 'GetNamedPipeServerProcessId',
        'FrameKind::Hello', 'FrameKind::Welcome', 'FrameKind::Request',
        'FrameKind::Ping', 'FrameKind::Pong', 'FrameKind::Cancel',
        'FrameKind::Shutdown', 'REQUEST_CANCELLED',
        'server.Stop(std::chrono::milliseconds(5000))')) {
    Assert-Contains $harness $token 'Real Windows Named Pipe lifecycle fixture regressed.'
}

Write-Host 'Named Pipe contract verified: current-user ACL, peer PIDs, strict v1 lifecycle, bounded RPC, cancellation, and joinable shutdown.'
