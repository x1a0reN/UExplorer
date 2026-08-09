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

$protocol = Read-ProjectFile 'protocol\rust\src\lib.rs'
$snapshotHost = Read-ProjectFile 'frontend\src-tauri\src\session\snapshot_cache.rs'
$hostCargo = Read-ProjectFile 'frontend\src-tauri\Cargo.toml'
$schema = Read-ProjectFile 'protocol\v1\schema\payload.schema.json' | ConvertFrom-Json
$fixture = Read-ProjectFile 'protocol\v1\fixtures\object-snapshot-page-response.json' | ConvertFrom-Json

foreach ($token in @(
        'pub const MAX_GENERATION', 'pub const MAX_SNAPSHOT_SOURCE_OBJECTS',
        'pub const MAX_SNAPSHOT_PAGE_RECORDS', 'pub struct ObjectHandle',
        'pub struct SnapshotCursor', 'pub struct SnapshotPage',
        'serde(deny_unknown_fields)', 'snapshot_page_golden_payload_has_strict_typed_identity')) {
    Assert-Contains $protocol $token 'Canonical Rust snapshot protocol contract regressed.'
}
Assert-Contains $hostCargo 'uexplorer-protocol = { path = "../../protocol/rust" }' 'Host duplicated or lost the canonical Rust protocol dependency.'

foreach ($token in @(
        'pub struct SnapshotAssembler', 'RequestCursorMismatch',
        'snapshot-wide metadata changed between pages', 'IncompleteGeneration',
        'assembler is terminal after a rejected page',
        'RwLock<Option<Arc<SnapshotIndex>>>', 'StaleGeneration',
        'by_kind', 'by_full_path', 'by_class_path', 'by_package_path',
        'by_address', 'search_tokens', 'query_fingerprint',
        'MAX_SEARCH_TOKENS_PER_RECORD', 'DuplicateAddress',
        'parse_canonical_hex', 'is_valid_text',
        'complete_generation_is_published_atomically_and_indexed',
        'mixed_or_incomplete_generation_never_replaces_current',
        'indexed_queries_have_exact_totals_and_query_bound_cursors')) {
    Assert-Contains $snapshotHost $token 'Host SnapshotCache validation/index/publication contract regressed.'
}
foreach ($token in @('ObjectArray::', 'Off::', 'HttpServer', 'TcpStream', 'fetch(')) {
    Assert-NotContains $snapshotHost $token 'Host SnapshotCache crossed its immutable transport/domain boundary.'
}
Assert-NotContains $snapshotHost 'records.reserve(page.record_count' 'Host eagerly allocated from an untrusted announced record count.'

if ($schema.'$defs'.snapshotPageData.properties.limit.maximum -ne 128 -or
        $schema.'$defs'.snapshotPageResult.properties.items.maxItems -ne 128 -or
        $fixture.data.items.Count -gt 128 -or
        $fixture.data.next_cursor.generation -ne $fixture.data.generation -or
        $fixture.data.next_cursor.after_index -ne $fixture.data.items[-1].handle.index) {
    throw 'Host/Core snapshot limits or golden continuation cursor drifted.'
}

Write-Host 'Host snapshot contract verified: strict typed pages, terminal assembly, atomic generation publication, bounded indexes, and query-bound cursors.'
