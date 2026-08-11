param(
    [string]$DllPath = ''
)

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

$main = Read-ProjectFile 'Dumper\Main.cpp'
$coreProject = Read-ProjectFile 'Dumper\UExplorerCore.vcxproj'
$capabilities = Read-ProjectFile 'Dumper\Runtime\CoreCapabilities.h'
$tauriHost = Read-ProjectFile 'frontend\src-tauri\src\lib.rs'
$domain = Read-ProjectFile 'frontend\src-tauri\src\services\domain_service.rs'
$client = Read-ProjectFile 'frontend\src\api\client.ts'
$payloadSchema = Read-ProjectFile 'protocol\v1\schema\payload.schema.json'

foreach ($token in @(
        'g_PipeServer->Start()', 'g_PostRenderHook->Install()',
        'shutdown.AddStage("named_pipe"', 'shutdown.AddStage("post_render_hook"')) {
    Assert-Contains $main $token 'Core lifecycle lost its Pipe/PostRender ownership boundary.'
}
foreach ($token in @(
        'HttpServer', 'RegisterAllRoutes', 'SetServer(', 'SetToken(',
        'runtime.ini', 'connection.ini', '27015', 'X-UExplorer-Token')) {
    Assert-NotContains $main $token 'Release Core entrypoint contains a legacy network/configuration path.'
}

foreach ($token in @(
        'Server\HttpServer.cpp', 'API\Router.cpp', 'API\StatusApi.cpp',
        'API\EventsApi.cpp', 'API\HookApi.cpp', 'ws2_32.lib')) {
    Assert-NotContains $coreProject $token 'Release Core project still compiles or links the legacy transport.'
}
foreach ($token in @(
        'IPC\NamedPipeRpcServer.cpp', 'Runtime\PostRenderHook.cpp',
        'Runtime\WorldSnapshotCapture.cpp', 'Services\CoreCommandService.cpp',
        'Services\WorldCommandService.cpp', 'Services\WorldTransformCommandService.cpp',
        'advapi32.lib')) {
    Assert-Contains $coreProject $token 'Release Core project omitted a required Pipe/domain component.'
}
foreach ($path in @('Dumper\Server\HttpServer.cpp', 'Dumper\API\Router.cpp')) {
    if (-not (Test-Path -LiteralPath (Join-Path $root $path) -PathType Leaf)) {
        throw "Legacy source archive was removed instead of being retained: $path"
    }
}

foreach ($token in @(
        '"memory.raw_read",', '"memory.raw_write",', '"engine.world_snapshot",',
        '"world.inspect",', '"world.details",', 'MEMORY_READ_COMMAND_NOT_IMPLEMENTED',
        'WorldInspectServiceEnabled', 'WORLD_COMMAND_NOT_READY')) {
    Assert-Contains $capabilities $token 'Core capability publication lost a required domain boundary.'
}
foreach ($token in @(
        'pub struct DomainService', 'pub fn execute(&self, request: DomainRequest)',
        'OPERATION_NOT_SUPPORTED', 'CAPABILITY_UNAVAILABLE',
        'DomainRoute::ObjectsList', 'DomainRoute::TypeList',
        'DomainRoute::Unavailable("memory.raw_read")',
        'DomainRoute::Core("call.invoke")',
        'DomainRoute::Core("world.inspect")',
        'DomainRoute::Core("world.levels")',
        'DomainRoute::Core("world.actors.list")',
        'DomainRoute::Core("world.shortcuts")',
        'DomainRoute::Core("world.actor.get")',
        'DomainRoute::Core("world.actor.components")',
        'DomainRoute::Core("world.actor.transform.get")',
        'DomainRoute::Unavailable("watch.properties")',
        'DomainRoute::Unavailable("hook.monitor")',
        'DomainRoute::Unavailable("dump.cpp")')) {
    Assert-Contains $domain $token 'Rust Host DomainService lost an explicit domain or error boundary.'
}
foreach ($token in @(
        'Arc::new(DomainService::new', 'async fn domain_request(',
        'spawn_blocking(move || service.execute(request))', 'domain_request,')) {
    Assert-Contains $tauriHost $token 'Tauri did not expose the owned DomainService command boundary.'
}
foreach ($token in @('runtime.ini', 'connection.ini', 'save_connection_settings')) {
    Assert-NotContains $tauriHost $token 'Tauri Host reintroduced legacy endpoint state.'
}
foreach ($text in @($client, $payloadSchema)) {
    Assert-NotContains $text 'use_game_thread' 'Release call contract reintroduced caller-controlled UE thread selection.'
}

foreach ($token in @(
        "'domain_request'", "'status.inspect'", "'objects.list'",
        "'types.classes.list'", "'memory.raw.read'", "'call.invoke'",
        "'world.inspect'", "'watch.list'", "'hook.list'", "'dump.sdk.start'",
        'new Channel<HostSessionEvent>()')) {
    Assert-Contains $client $token 'React API lost a required Tauri domain/event command.'
}

$frontendSources = Get-ChildItem -LiteralPath (Join-Path $root 'frontend\src') -Recurse -File |
    Where-Object { $_.Extension -in @('.ts', '.tsx') } |
    ForEach-Object { Get-Content -LiteralPath $_.FullName -Raw -Encoding UTF8 }
$frontend = $frontendSources -join "`n"
foreach ($pattern in @(
        '\bfetch\s*\(', '\bEventSource\b', 'new\s+WebSocket\s*\(',
        'connectWebSocket', 'subscribeEventStream', 'https?://127\.0\.0\.1',
        'wss?://', 'X-UExplorer-Token', 'persistConnectionSettings',
        'settings\.port\b', 'settings\.token\b')) {
    if ([regex]::IsMatch($frontend, $pattern, [System.Text.RegularExpressions.RegexOptions]::IgnoreCase)) {
        throw "React source contains a legacy transport path matching: $pattern"
    }
}

if ($DllPath) {
    $resolvedDll = (Resolve-Path -LiteralPath $DllPath).Path
    $dumpbin = Get-Command dumpbin.exe -ErrorAction SilentlyContinue
    if (-not $dumpbin) {
        $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
        if (-not (Test-Path -LiteralPath $vswhere -PathType Leaf)) {
            throw 'dumpbin.exe and vswhere.exe are unavailable; binary transport verification cannot run.'
        }
        $installation = (& $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath | Select-Object -First 1)
        if (-not $installation) {
            throw 'No Visual C++ toolchain installation was found for dumpbin verification.'
        }
        $dumpbinPath = Get-ChildItem -LiteralPath (Join-Path $installation 'VC\Tools\MSVC') -Recurse -Filter dumpbin.exe -File |
            Where-Object { $_.FullName -match '\\bin\\Hostx64\\x64\\dumpbin\.exe$' } |
            Sort-Object FullName -Descending |
            Select-Object -First 1 -ExpandProperty FullName
        if (-not $dumpbinPath) {
            throw 'The Visual C++ x64 dumpbin.exe was not found.'
        }
    }
    else {
        $dumpbinPath = $dumpbin.Source
    }

    $imports = (& $dumpbinPath /nologo /imports $resolvedDll 2>&1) -join "`n"
    if ($LASTEXITCODE -ne 0) {
        throw "dumpbin failed for $resolvedDll`n$imports"
    }
    foreach ($module in @('WS2_32.dll', 'WSOCK32.dll', 'WINHTTP.dll', 'WININET.dll')) {
        if ($imports.IndexOf($module, [StringComparison]::OrdinalIgnoreCase) -ge 0) {
            throw "Release Core DLL imports forbidden network module: $module"
        }
    }

    $bytes = [System.IO.File]::ReadAllBytes($resolvedDll)
    $ascii = [System.Text.Encoding]::ASCII.GetString($bytes)
    $unicode = [System.Text.Encoding]::Unicode.GetString($bytes)
    foreach ($token in @('HttpServer', '/api/v1', 'X-UExplorer-Token', 'runtime.ini', 'connection.ini', '127.0.0.1')) {
        if ($ascii.IndexOf($token, [StringComparison]::OrdinalIgnoreCase) -ge 0 -or
                $unicode.IndexOf($token, [StringComparison]::OrdinalIgnoreCase) -ge 0) {
            throw "Release Core DLL contains a forbidden legacy transport marker: $token"
        }
    }
}

$scope = if ($DllPath) { 'static and release-binary' } else { 'static' }
Write-Host "Transport cutover contract verified ($scope): React -> Tauri DomainService -> PID Pipe -> Core, without an in-version legacy transport path."
