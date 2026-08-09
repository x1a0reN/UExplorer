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

$serviceHeader = Read-ProjectFile 'Dumper\Services\TypeCommandService.h'
$service = Read-ProjectFile 'Dumper\Services\TypeCommandService.cpp'
$snapshotHeader = Read-ProjectFile 'Dumper\Runtime\TypeSnapshot.h'
$capabilities = Read-ProjectFile 'Dumper\Runtime\CoreCapabilities.h'
$coreCommand = Read-ProjectFile 'Dumper\Services\CoreCommandService.cpp'
$domainHost = Read-ProjectFile 'frontend\src-tauri\src\services\domain_service.rs'
$frontendClient = Read-ProjectFile 'frontend\src\api\client.ts'
$frontendTypes = Read-ProjectFile 'frontend\src\api\index.ts'
$functionsPage = Read-ProjectFile 'frontend\src\pages\Functions.tsx'
$protocol = Read-ProjectFile 'protocol\rust\src\lib.rs'
$schema = Read-ProjectFile 'protocol\v1\schema\payload.schema.json' | ConvertFrom-Json
$requestFixture = Read-ProjectFile 'protocol\v1\fixtures\type-class-fields-request.json' | ConvertFrom-Json
$responseFixture = Read-ProjectFile 'protocol\v1\fixtures\type-class-fields-response.json' | ConvertFrom-Json
$coreProject = Read-ProjectFile 'Dumper\UExplorerCore.vcxproj'
$harnessProject = Read-ProjectFile 'tests\core-harness\CoreHarness.vcxproj'
$harness = Read-ProjectFile 'tests\core-harness\main.cpp'

foreach ($token in @(
        'kMaxPageRecords = 128', 'kMaxFunctionParameters = 128',
        'kMaxResponseBytes = 4 * 1024 * 1024', 'TYPE_SNAPSHOT_CONTEXT_MISMATCH',
        'TYPE_SNAPSHOT_GENERATION_MISMATCH', 'TYPE_QUERY_CURSOR_MISMATCH',
        'TYPE_RESPONSE_LIMIT_EXCEEDED', 'std::to_string(entry.Value)',
        'std::format("0x{:016X}", property.Flags)', 'FindDirectChildIndices',
        'FindFunctionByFullPath',
        'append(snapshot.SessionId())',
        'total == 0 || cursor->AfterOrdinal >= total - 1',
        'serializedBytes > TypeCommandService::kMaxResponseBytes')) {
    Assert-Contains ($serviceHeader + $service + $snapshotHeader) $token 'Immutable type command bounds or identity semantics regressed.'
}

foreach ($token in @('ObjectArray::', 'NameArray::', 'Off::', 'Settings::', 'SafeMemory::', 'GameThreadExecutor')) {
    Assert-NotContains $service $token 'Type metadata serialization crossed into live UE memory or the game-thread execution boundary.'
}

Assert-Contains $capabilities '"types.inspect",' 'Type capability is no longer declared.'
Assert-Contains $capabilities '{"engine.type_snapshot"}' 'Type capability lost its immutable snapshot dependency.'
Assert-NotContains $capabilities 'TYPE_COMMAND_NOT_IMPLEMENTED' 'Type capability reverted to a hard-coded unavailable placeholder.'
Assert-Contains $coreCommand 'TypeCommandService::Handles(request.Operation)' 'Core command dispatch no longer routes the type registry.'
Assert-Contains $coreCommand 'constexpr const char* capabilityName = "types.inspect"' 'Core type commands bypassed capability admission.'

$operations = @(
    'types.classes.get', 'types.classes.fields', 'types.classes.functions',
    'types.classes.hierarchy', 'types.classes.cdo', 'types.functions.get',
    'types.structs.get', 'types.structs.fields', 'types.enums.get', 'types.enums.values'
)
foreach ($operation in $operations) {
    Assert-Contains $service $operation "Core type registry lost $operation."
    Assert-Contains $domainHost "DomainRoute::Core(`"$operation`")" "Host does not transparently route $operation."
}

foreach ($token in @(
        'pub struct TypeQueryCursor', 'pub struct TypeMemberPageData',
        'pub struct TypeDetail', 'pub struct TypeFunctionDetail',
        'pub struct TypeMemberPage<T>', 'pub struct TypeEnumValuePage',
		'pub enum TypeDefaultObjectState', 'pub enum ClassDefaultObjectState',
		'deserialize_i64_decimal',
        'type_query_golden_payloads_are_strict_and_generation_bound')) {
    Assert-Contains $protocol $token 'Shared Rust type protocol lost strict typed metadata.'
}

foreach ($definition in @(
        'typeQueryCursor', 'typePathData', 'typeMemberPageData', 'typePathPageData',
        'typeDetailResult', 'typePropertyPageResult', 'typeFunctionPageResult',
        'typeFunctionDetailResult', 'typeHierarchyPageResult',
        'classDefaultObjectResult', 'typeEnumValuePageResult')) {
    if (-not $schema.'$defs'.$definition) {
        throw "IPC schema lost type definition: $definition"
    }
}
if ($schema.'$defs'.typeMemberPageData.properties.limit.maximum -ne 128 -or
        $schema.'$defs'.typeQueryCursor.properties.after_ordinal.maximum -ne 7999999 -or
        $schema.'$defs'.typePropertyPageResult.properties.items.maxItems -ne 128 -or
        $schema.'$defs'.typeFunctionPageResult.properties.items.maxItems -ne 128 -or
        $schema.'$defs'.typeEnumValuePageResult.properties.items.maxItems -ne 128) {
    throw 'Type protocol paging limits drifted between request and response schemas.'
}
if ($requestFixture.operation -ne 'types.classes.fields' -or
        $requestFixture.data.path -ne '/Script/Fixture.Derived' -or
        $requestFixture.data.PSObject.Properties.Name -contains 'name' -or
        $responseFixture.data.items.Count -ne 1 -or
        $responseFixture.data.next_cursor.generation -ne $responseFixture.data.type_snapshot_generation -or
        $responseFixture.data.next_cursor.after_ordinal -ne 0) {
    throw 'Type command golden fixtures no longer preserve exact path and generation-bound cursor semantics.'
}

foreach ($token in @(
        "this.command('types.classes.get', { path })",
        "this.command('types.functions.get', { path })",
        "this.command('types.structs.fields', { path, scope, cursor, limit })",
        "this.command('types.enums.values', { path, cursor, limit })",
        'export interface TypeQueryCursor', 'value: string;',
        "implementation: 'unavailable' | 'native' | 'bytecode' | 'native_and_bytecode'")) {
    Assert-Contains ($frontendClient + $frontendTypes) $token 'Frontend type API no longer matches the strict Host/Core contract.'
}
foreach ($token in @(
        'getClassByName', 'getStructByName', 'getEnumByName', 'has_script ?? false',
        '.params.map', "n.includes('native')", 'api.getBlueprintBytecode(selected.index)',
        'setTargetIndex(String(classDetail.data.index))')) {
    Assert-NotContains ($frontendClient + $functionsPage) $token 'Frontend reintroduced short-name identity or fabricated function metadata.'
}
Assert-Contains $functionsPage 'Invalid canonical function flags' 'Frontend silently defaults malformed function flags to an execution mode.'
Assert-Contains $functionsPage 'Backend-indexed implementation filtering is not available yet' 'Frontend exposed a function implementation filter without backend metadata.'

foreach ($project in @($coreProject, $harnessProject)) {
    Assert-Contains $project 'TypeCommandService.cpp' 'A build target omitted the production type command service.'
}
foreach ($token in @(
        'Type paging cursor crossed a query or immutable generation boundary',
        'Type paging accepted a fabricated cursor that cannot have a continuation',
        'Enum query truncated int64 values',
        'Function pages guessed implementation state',
        'Class hierarchy did not use the immutable direct-child index')) {
    Assert-Contains $harness $token 'Core type command behavioral coverage regressed.'
}

Write-Host 'Type command contract verified: immutable full-path lookup, bounded pages, query-bound cursors, truthful unavailable states, int64-safe values, Host routing, and frontend consumption.'
