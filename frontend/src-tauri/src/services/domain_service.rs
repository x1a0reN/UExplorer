use serde::{Deserialize, Serialize};
use serde_json::{json, Map, Value};
use std::time::{SystemTime, UNIX_EPOCH};

#[cfg(windows)]
use crate::session::session_manager::{ManagedSession, SessionManager, SessionManagerError};
#[cfg(windows)]
use crate::session::snapshot_cache::{
    SnapshotCacheError, SnapshotIndex, SnapshotObjectKind, SnapshotQuery, SnapshotQueryCursor,
    SnapshotQueryPage, SnapshotRecord,
};
#[cfg(windows)]
use std::sync::Arc;
#[cfg(windows)]
use std::time::Duration;

const DEFAULT_TIMEOUT_MS: u32 = 5_000;
const MAX_TIMEOUT_MS: u32 = 120_000;
const DEFAULT_PAGE_LIMIT: u32 = 50;
const MAX_PAGE_LIMIT: u32 = 128;
const MAX_QUERY_BYTES: usize = 4_096;

#[derive(Clone, Debug, Deserialize, Eq, PartialEq)]
#[serde(default, deny_unknown_fields, rename_all = "camelCase")]
pub struct DomainRequest {
    pub target_pid: Option<u32>,
    pub operation: String,
    pub timeout_ms: u32,
    pub data: Value,
}

impl Default for DomainRequest {
    fn default() -> Self {
        Self {
            target_pid: None,
            operation: String::new(),
            timeout_ms: DEFAULT_TIMEOUT_MS,
            data: json!({}),
        }
    }
}

#[derive(Clone, Debug, Serialize)]
pub struct DomainResponse {
    pub success: bool,
    pub data: Value,
    pub error: Option<String>,
    pub error_code: Option<String>,
    pub details: Value,
    pub timing: Option<Value>,
    pub timestamp: u64,
}

impl DomainResponse {
    fn success(data: Value, timing: Option<Value>) -> Self {
        Self {
            success: true,
            data,
            error: None,
            error_code: None,
            details: Value::Null,
            timing,
            timestamp: unix_timestamp_ms(),
        }
    }

    fn failure(
        code: impl Into<String>,
        message: impl Into<String>,
        details: Value,
        timing: Option<Value>,
    ) -> Self {
        let code = code.into();
        let message = message.into();
        Self {
            success: false,
            data: Value::Null,
            error: Some(format!("{code}: {message}")),
            error_code: Some(code),
            details,
            timing,
            timestamp: unix_timestamp_ms(),
        }
    }
}

fn unix_timestamp_ms() -> u64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|value| value.as_millis().min(u128::from(u64::MAX)) as u64)
        .unwrap_or(0)
}

#[cfg(windows)]
pub struct DomainService {
    sessions: Arc<SessionManager>,
}

#[cfg(windows)]
impl DomainService {
    pub fn new(sessions: Arc<SessionManager>) -> Self {
        Self { sessions }
    }

    pub fn execute(&self, request: DomainRequest) -> DomainResponse {
        let Some(route) = resolve_operation(&request.operation) else {
            return DomainResponse::failure(
                "OPERATION_NOT_SUPPORTED",
                "No Host domain command is registered for the requested operation",
                json!({"operation": request.operation}),
                None,
            );
        };
        if request.timeout_ms == 0 || request.timeout_ms > MAX_TIMEOUT_MS {
            return DomainResponse::failure(
                "TIMEOUT_INVALID",
                "timeoutMs must be in range 1..120000",
                json!({"timeout_ms": request.timeout_ms}),
                None,
            );
        }

        let session = match self.resolve_session(request.target_pid) {
            Ok(session) => session,
            Err(error) => return manager_failure(error),
        };

        match route {
            DomainRoute::Core(core_operation) => {
                self.execute_core(&session, core_operation, request.timeout_ms, request.data)
            }
            DomainRoute::SnapshotRefresh => {
                match session.refresh_snapshot(Duration::from_millis(u64::from(request.timeout_ms)))
                {
                    Ok(snapshot) => DomainResponse::success(snapshot_metadata(&snapshot), None),
                    Err(error) => manager_failure(error),
                }
            }
            DomainRoute::ObjectsCount => {
                self.with_snapshot(&session, request.timeout_ms, |index| {
                    Ok(object_counts(index))
                })
            }
            DomainRoute::ObjectsList => self.with_snapshot(&session, request.timeout_ms, |index| {
                let args: ObjectQueryArgs = parse_data(&request.data)?;
                query_objects(index, &args)
            }),
            DomainRoute::ObjectsSearch => {
                self.with_snapshot(&session, request.timeout_ms, |index| {
                    let args: ObjectQueryArgs = parse_data(&request.data)?;
                    query_objects(index, &args)
                })
            }
            DomainRoute::ObjectByIndex => {
                self.with_snapshot(&session, request.timeout_ms, |index| {
                    let args: IndexArgs = parse_data(&request.data)?;
                    let record = index.object_by_index(args.index).ok_or_else(|| {
                        DomainFailure::new(
                            "OBJECT_NOT_FOUND",
                            "No snapshot object has the requested index",
                            json!({"index": args.index}),
                        )
                    })?;
                    Ok(object_detail(record))
                })
            }
            DomainRoute::ObjectByAddress => {
                self.with_snapshot(&session, request.timeout_ms, |index| {
                    let args: AddressArgs = parse_data(&request.data)?;
                    validate_text("address", &args.address)?;
                    let record = index.object_by_address(&args.address).ok_or_else(|| {
                        DomainFailure::new(
                            "OBJECT_NOT_FOUND",
                            "No snapshot object has the requested address",
                            json!({"address": args.address}),
                        )
                    })?;
                    Ok(object_detail(record))
                })
            }
            DomainRoute::ObjectByPath => {
                self.with_snapshot(&session, request.timeout_ms, |index| {
                    let args: PathArgs = parse_data(&request.data)?;
                    validate_text("path", &args.path)?;
                    object_by_full_path(index, &args.path)
                })
            }
            DomainRoute::TypeList(kind) => {
                self.with_snapshot(&session, request.timeout_ms, |index| {
                    let args: CollectionQueryArgs = parse_data(&request.data)?;
                    query_types(index, &args, kind)
                })
            }
            DomainRoute::PackageContents => {
                self.with_snapshot(&session, request.timeout_ms, |index| {
                    let args: PackageArgs = parse_data(&request.data)?;
                    validate_text("package_path", &args.package_path)?;
                    package_contents(index, &args)
                })
            }
            DomainRoute::ClassInstances => {
                self.with_snapshot(&session, request.timeout_ms, |index| {
                    let args: ClassInstancesArgs = parse_data(&request.data)?;
                    validate_text("class_path", &args.class_path)?;
                    class_instances(index, &args)
                })
            }
            DomainRoute::Unavailable(capability) => {
                capability_unavailable(&session, &request.operation, capability)
            }
        }
    }

    fn resolve_session(
        &self,
        target_pid: Option<u32>,
    ) -> Result<Arc<ManagedSession>, SessionManagerError> {
        match target_pid {
            Some(0) => Err(SessionManagerError::InvalidConfiguration(
                "target PID must be positive",
            )),
            Some(pid) => self.sessions.get(pid),
            None => self
                .sessions
                .active()?
                .ok_or(SessionManagerError::SessionNotFound(0)),
        }
    }

    fn execute_core(
        &self,
        session: &ManagedSession,
        operation: &str,
        timeout_ms: u32,
        data: Value,
    ) -> DomainResponse {
        match session.request(operation, timeout_ms, data) {
            Ok(response) => {
                let timing = Some(json!({
                    "queued_us": response.timing.queued_us,
                    "execute_us": response.timing.execute_us,
                }));
                if response.ok {
                    DomainResponse::success(response.data, timing)
                } else if let Some(error) = response.error {
                    DomainResponse::failure(error.code, error.message, error.details, timing)
                } else {
                    DomainResponse::failure(
                        "CORE_RESPONSE_INVALID",
                        "Core returned a failure response without an error payload",
                        json!({"operation": operation}),
                        timing,
                    )
                }
            }
            Err(error) => manager_failure(error),
        }
    }

    fn with_snapshot<F>(
        &self,
        session: &ManagedSession,
        timeout_ms: u32,
        operation: F,
    ) -> DomainResponse
    where
        F: FnOnce(&SnapshotIndex) -> Result<Value, DomainFailure>,
    {
        let snapshot = match session.current_snapshot() {
            Ok(Some(snapshot)) => snapshot,
            Ok(None) => {
                match session.refresh_snapshot(Duration::from_millis(u64::from(timeout_ms))) {
                    Ok(snapshot) => snapshot,
                    Err(error) => return manager_failure(error),
                }
            }
            Err(error) => return manager_failure(error),
        };
        match operation(&snapshot) {
            Ok(data) => DomainResponse::success(data, None),
            Err(error) => error.into_response(),
        }
    }
}

#[cfg(windows)]
fn manager_failure(error: SessionManagerError) -> DomainResponse {
    DomainResponse::failure(error.code(), error.to_string(), Value::Null, None)
}

#[cfg(windows)]
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
enum DomainRoute {
    Core(&'static str),
    SnapshotRefresh,
    ObjectsCount,
    ObjectsList,
    ObjectsSearch,
    ObjectByIndex,
    ObjectByAddress,
    ObjectByPath,
    TypeList(SnapshotObjectKind),
    PackageContents,
    ClassInstances,
    Unavailable(&'static str),
}

#[cfg(windows)]
fn resolve_operation(operation: &str) -> Option<DomainRoute> {
    let route = match operation {
        "status.inspect" => DomainRoute::Core("status.inspect"),
        "status.engine" => DomainRoute::Core("status.engine"),
        "status.health" => DomainRoute::Core("status.health"),
        "status.reconnect" => DomainRoute::Core("status.reconnect"),
        "objects.snapshot.refresh" => DomainRoute::SnapshotRefresh,
        "objects.count" => DomainRoute::ObjectsCount,
        "objects.list" => DomainRoute::ObjectsList,
        "objects.search" => DomainRoute::ObjectsSearch,
        "objects.get_by_index" => DomainRoute::ObjectByIndex,
        "objects.get_by_address" => DomainRoute::ObjectByAddress,
        "objects.get_by_path" => DomainRoute::ObjectByPath,
        "types.packages.list" => DomainRoute::TypeList(SnapshotObjectKind::Package),
        "types.packages.contents" => DomainRoute::PackageContents,
        "types.classes.list" => DomainRoute::TypeList(SnapshotObjectKind::Class),
        "types.structs.list" => DomainRoute::TypeList(SnapshotObjectKind::Struct),
        "types.enums.list" => DomainRoute::TypeList(SnapshotObjectKind::Enum),
        "types.classes.instances" => DomainRoute::ClassInstances,
        "objects.properties.list"
        | "objects.outer_chain"
        | "objects.property.read"
        | "objects.property.write" => DomainRoute::Unavailable("objects.properties"),
        "types.classes.get"
        | "types.classes.fields"
        | "types.classes.functions"
        | "types.classes.hierarchy"
        | "types.classes.cdo"
        | "types.structs.get"
        | "types.enums.get" => DomainRoute::Unavailable("types.inspect"),
        "world.inspect"
        | "world.levels"
        | "world.actors.list"
        | "world.shortcuts"
        | "world.actor.get"
        | "world.actor.components" => DomainRoute::Unavailable("world.inspect"),
        "world.actor.transform.update" => DomainRoute::Unavailable("world.mutate"),
        "memory.raw.read" => DomainRoute::Unavailable("memory.raw_read"),
        "memory.raw.write" => DomainRoute::Unavailable("memory.raw_write"),
        "memory.typed.read" | "memory.typed.write" => DomainRoute::Unavailable("memory.typed"),
        "memory.pointer_chain.resolve" => DomainRoute::Unavailable("memory.pointer_chain"),
        "call.invoke" | "call.static" | "call.batch" => DomainRoute::Unavailable("call.invoke"),
        "watch.add" | "watch.list" | "watch.remove" | "watch.history" => {
            DomainRoute::Unavailable("watch.properties")
        }
        "hook.add" | "hook.list" | "hook.enable" | "hook.remove" | "hook.log" => {
            DomainRoute::Unavailable("hook.monitor")
        }
        "blueprint.bytecode" | "blueprint.decompile" => {
            DomainRoute::Unavailable("blueprint.decompile")
        }
        "dump.sdk.start" => DomainRoute::Unavailable("dump.cpp"),
        "dump.usmap.start" => DomainRoute::Unavailable("dump.usmap"),
        "dump.dumpspace.start" => DomainRoute::Unavailable("dump.dumpspace"),
        "dump.ida.start" => DomainRoute::Unavailable("dump.ida"),
        "dump.jobs.list" | "dump.jobs.get" => DomainRoute::Unavailable("dump.jobs"),
        _ => return None,
    };
    Some(route)
}

#[cfg(windows)]
fn capability_unavailable(
    session: &ManagedSession,
    operation: &str,
    capability: &str,
) -> DomainResponse {
    let published = session.welcome().capabilities.get(capability).copied();
    if published == Some(true) {
        return DomainResponse::failure(
            "DOMAIN_COMMAND_NOT_REGISTERED",
            "Core advertised a capability without a registered Host domain command",
            json!({
                "operation": operation,
                "capability": capability,
                "published": true,
            }),
            None,
        );
    }
    DomainResponse::failure(
        "CAPABILITY_UNAVAILABLE",
        "The requested domain capability is unavailable for this Core session",
        json!({
            "operation": operation,
            "capability": capability,
            "published": published,
        }),
        None,
    )
}

#[cfg(windows)]
#[derive(Debug)]
struct DomainFailure {
    code: &'static str,
    message: String,
    details: Value,
}

#[cfg(windows)]
impl DomainFailure {
    fn new(code: &'static str, message: impl Into<String>, details: Value) -> Self {
        Self {
            code,
            message: message.into(),
            details,
        }
    }

    fn into_response(self) -> DomainResponse {
        DomainResponse::failure(self.code, self.message, self.details, None)
    }
}

#[cfg(windows)]
fn parse_data<T>(data: &Value) -> Result<T, DomainFailure>
where
    T: for<'de> Deserialize<'de>,
{
    serde_json::from_value(data.clone()).map_err(|error| {
        DomainFailure::new(
            "INVALID_ARGUMENT",
            format!("Domain command data is invalid: {error}"),
            Value::Null,
        )
    })
}

#[cfg(windows)]
#[derive(Debug, Deserialize)]
#[serde(default, deny_unknown_fields)]
struct ObjectQueryArgs {
    cursor: Option<SnapshotQueryCursor>,
    limit: u32,
    search: Option<String>,
    kind: Option<SnapshotObjectKind>,
    class_path: Option<String>,
    package_path: Option<String>,
}

#[cfg(windows)]
impl Default for ObjectQueryArgs {
    fn default() -> Self {
        Self {
            cursor: None,
            limit: DEFAULT_PAGE_LIMIT,
            search: None,
            kind: None,
            class_path: None,
            package_path: None,
        }
    }
}

#[cfg(windows)]
#[derive(Debug, Deserialize)]
#[serde(default, deny_unknown_fields)]
struct CollectionQueryArgs {
    cursor: Option<SnapshotQueryCursor>,
    limit: u32,
    search: Option<String>,
}

#[cfg(windows)]
impl Default for CollectionQueryArgs {
    fn default() -> Self {
        Self {
            cursor: None,
            limit: DEFAULT_PAGE_LIMIT,
            search: None,
        }
    }
}

#[cfg(windows)]
#[derive(Debug, Deserialize)]
#[serde(deny_unknown_fields)]
struct IndexArgs {
    index: i32,
}

#[cfg(windows)]
#[derive(Debug, Deserialize)]
#[serde(deny_unknown_fields)]
struct AddressArgs {
    address: String,
}

#[cfg(windows)]
#[derive(Debug, Deserialize)]
#[serde(deny_unknown_fields)]
struct PathArgs {
    path: String,
}

#[cfg(windows)]
#[derive(Debug, Deserialize)]
#[serde(default, deny_unknown_fields)]
struct PackageArgs {
    package_path: String,
    cursor: Option<SnapshotQueryCursor>,
    limit: u32,
}

#[cfg(windows)]
impl Default for PackageArgs {
    fn default() -> Self {
        Self {
            package_path: String::new(),
            cursor: None,
            limit: DEFAULT_PAGE_LIMIT,
        }
    }
}

#[cfg(windows)]
#[derive(Debug, Deserialize)]
#[serde(default, deny_unknown_fields)]
struct ClassInstancesArgs {
    class_path: String,
    search: Option<String>,
    cursor: Option<SnapshotQueryCursor>,
    limit: u32,
}

#[cfg(windows)]
impl Default for ClassInstancesArgs {
    fn default() -> Self {
        Self {
            class_path: String::new(),
            search: None,
            cursor: None,
            limit: DEFAULT_PAGE_LIMIT,
        }
    }
}

#[cfg(windows)]
fn validate_limit(limit: u32) -> Result<usize, DomainFailure> {
    if !(1..=MAX_PAGE_LIMIT).contains(&limit) {
        return Err(DomainFailure::new(
            "PAGINATION_INVALID",
            "limit must be in range 1..128",
            json!({"limit": limit}),
        ));
    }
    Ok(limit as usize)
}

#[cfg(windows)]
fn validate_text(field: &'static str, value: &str) -> Result<(), DomainFailure> {
    if value.is_empty() || value.len() > MAX_QUERY_BYTES || value.chars().any(char::is_control) {
        return Err(DomainFailure::new(
            "INVALID_ARGUMENT",
            format!("{field} must be non-empty, bounded UTF-8 without control characters"),
            json!({"field": field}),
        ));
    }
    Ok(())
}

#[cfg(windows)]
fn object_item(record: &SnapshotRecord) -> Value {
    json!({
        "index": record.handle.index,
        "name": record.name,
        "class": record.class_path,
        "address": record.handle.address,
    })
}

#[cfg(windows)]
fn object_detail(record: &SnapshotRecord) -> Value {
    json!({
        "index": record.handle.index,
        "name": record.name,
        "class": record.class_path,
        "address": record.handle.address,
        "full_name": record.full_path,
        "package_path": record.package_path,
        "handle": record.handle,
        "kind": record.kind,
    })
}

#[cfg(windows)]
fn snapshot_failure(error: SnapshotCacheError) -> DomainFailure {
    DomainFailure::new(error.code(), error.to_string(), Value::Null)
}

#[cfg(windows)]
fn object_by_full_path(index: &SnapshotIndex, path: &str) -> Result<Value, DomainFailure> {
    let page = index
        .query(
            &SnapshotQuery {
                full_path: Some(path.to_string()),
                ..SnapshotQuery::default()
            },
            None,
            2,
        )
        .map_err(snapshot_failure)?;
    match page.matched_count {
        0 => Err(DomainFailure::new(
            "OBJECT_NOT_FOUND",
            "No snapshot object has the requested full path",
            json!({"path": path}),
        )),
        1 => Ok(object_detail(&page.items[0])),
        count => Err(DomainFailure::new(
            "OBJECT_IDENTITY_AMBIGUOUS",
            "The snapshot contains more than one object with the requested full path",
            json!({"path": path, "matched": count}),
        )),
    }
}

#[cfg(windows)]
fn mapped_page<F>(page: SnapshotQueryPage, limit: u32, map: F) -> Map<String, Value>
where
    F: Fn(&SnapshotRecord) -> Value,
{
    let items = page.items.iter().map(map).collect::<Vec<_>>();
    Map::from_iter([
        ("items".to_string(), json!(items)),
        ("total".to_string(), json!(page.matched_count)),
        ("matched".to_string(), json!(page.matched_count)),
        ("limit".to_string(), json!(limit)),
        ("has_more".to_string(), json!(page.has_more)),
        ("next_cursor".to_string(), json!(page.next_cursor)),
        ("snapshot_generation".to_string(), json!(page.generation)),
        (
            "context_generation".to_string(),
            json!(page.context_generation),
        ),
        (
            "source_object_count".to_string(),
            json!(page.source_object_count),
        ),
        (
            "snapshot_record_count".to_string(),
            json!(page.snapshot_record_count),
        ),
    ])
}

#[cfg(windows)]
fn snapshot_metadata(index: &SnapshotIndex) -> Value {
    json!({
        "session_id": index.session_id(),
        "generation": index.generation(),
        "context_generation": index.context_generation(),
        "record_count": index.record_count(),
    })
}

#[cfg(windows)]
fn object_counts(index: &SnapshotIndex) -> Value {
    json!({
        "total": index.record_count(),
        "classes": index.count_by_kind(SnapshotObjectKind::Class),
        "structs": index.count_by_kind(SnapshotObjectKind::Struct),
        "enums": index.count_by_kind(SnapshotObjectKind::Enum),
        "functions": index.count_by_kind(SnapshotObjectKind::Function),
        "packages": index.count_by_kind(SnapshotObjectKind::Package),
    })
}

#[cfg(windows)]
fn query_objects(index: &SnapshotIndex, args: &ObjectQueryArgs) -> Result<Value, DomainFailure> {
    let limit = validate_limit(args.limit)?;
    let page = index
        .query(
            &SnapshotQuery {
                kind: args.kind,
                class_path: args.class_path.clone(),
                package_path: args.package_path.clone(),
                search: args.search.clone(),
                ..SnapshotQuery::default()
            },
            args.cursor.as_ref(),
            limit,
        )
        .map_err(snapshot_failure)?;
    Ok(Value::Object(mapped_page(page, args.limit, object_item)))
}

#[cfg(windows)]
fn query_types(
    index: &SnapshotIndex,
    args: &CollectionQueryArgs,
    kind: SnapshotObjectKind,
) -> Result<Value, DomainFailure> {
    let limit = validate_limit(args.limit)?;
    let page = index
        .query(
            &SnapshotQuery {
                kind: Some(kind),
                search: args.search.clone(),
                ..SnapshotQuery::default()
            },
            args.cursor.as_ref(),
            limit,
        )
        .map_err(snapshot_failure)?;
    Ok(Value::Object(mapped_page(page, args.limit, |record| {
        json!({
            "index": record.handle.index,
            "name": record.name,
            "full_name": record.full_path,
            "address": record.handle.address,
        })
    })))
}

#[cfg(windows)]
fn package_contents(index: &SnapshotIndex, args: &PackageArgs) -> Result<Value, DomainFailure> {
    let limit = validate_limit(args.limit)?;
    let page = index
        .query(
            &SnapshotQuery {
                package_path: Some(args.package_path.clone()),
                ..SnapshotQuery::default()
            },
            args.cursor.as_ref(),
            limit,
        )
        .map_err(snapshot_failure)?;
    let count = page.matched_count;
    let mut response = mapped_page(page, args.limit, object_item);
    response.insert("package".to_string(), json!(args.package_path));
    response.insert("count".to_string(), json!(count));
    Ok(Value::Object(response))
}

#[cfg(windows)]
fn class_instances(
    index: &SnapshotIndex,
    args: &ClassInstancesArgs,
) -> Result<Value, DomainFailure> {
    let limit = validate_limit(args.limit)?;
    let page = index
        .query(
            &SnapshotQuery {
                class_path: Some(args.class_path.clone()),
                search: args.search.clone(),
                ..SnapshotQuery::default()
            },
            args.cursor.as_ref(),
            limit,
        )
        .map_err(snapshot_failure)?;
    let mut response = mapped_page(page, args.limit, |record| {
        json!({
            "index": record.handle.index,
            "name": record.name,
            "address": record.handle.address,
        })
    });
    response.insert("class".to_string(), json!(args.class_path));
    Ok(Value::Object(response))
}

#[cfg(test)]
#[cfg(windows)]
mod tests {
    use super::*;
    use crate::session::snapshot_cache::SnapshotAssembler;
    use uexplorer_protocol::{ObjectHandle, SnapshotPage};

    const FIXTURE_SESSION: &str = "core-DOMAIN-FIXTURE";

    fn snapshot_record(
        index: i32,
        name: &str,
        full_path: &str,
        class_path: &str,
        package_path: &str,
    ) -> SnapshotRecord {
        SnapshotRecord {
            handle: ObjectHandle {
                session_id: FIXTURE_SESSION.to_string(),
                context_generation: 1,
                index,
                serial: index + 100,
                address: format!("0x{:016X}", 0x1000 + index as u64),
                class_fingerprint: format!("{:016X}", 0x2000 + index as u64),
            },
            name: name.to_string(),
            full_path: full_path.to_string(),
            class_path: class_path.to_string(),
            package_path: package_path.to_string(),
            kind: SnapshotObjectKind::Object,
        }
    }

    fn indexed_fixture(records: Vec<SnapshotRecord>) -> SnapshotIndex {
        let record_count = records.len() as u32;
        let source_object_count = 16;
        let mut assembler = SnapshotAssembler::new(FIXTURE_SESSION).unwrap();
        assembler
            .push_page(
                None,
                SnapshotPage {
                    generation: 7,
                    context_generation: 1,
                    captured_at_monotonic_us: 10,
                    capture_duration_us: 2,
                    source_object_count,
                    record_count,
                    skipped_slots: source_object_count - record_count,
                    items: records,
                    has_more: false,
                    next_cursor: None,
                },
            )
            .unwrap();
        assembler.finish().unwrap()
    }

    #[test]
    fn operation_registry_is_explicit_across_every_domain() {
        for operation in [
            "status.inspect",
            "objects.list",
            "types.classes.list",
            "memory.raw.read",
            "call.invoke",
            "world.inspect",
            "watch.list",
            "hook.list",
            "blueprint.decompile",
            "dump.sdk.start",
        ] {
            assert!(
                resolve_operation(operation).is_some(),
                "missing {operation}"
            );
        }
        assert!(resolve_operation("core.raw_passthrough").is_none());
    }

    #[test]
    fn unknown_operation_is_rejected_before_session_resolution() {
        let service = DomainService::new(Arc::new(SessionManager::new()));
        let response = service.execute(DomainRequest {
            operation: "core.raw_passthrough".to_string(),
            ..DomainRequest::default()
        });
        assert!(!response.success);
        assert_eq!(
            response.error_code.as_deref(),
            Some("OPERATION_NOT_SUPPORTED")
        );
    }

    #[test]
    fn known_operation_without_active_session_is_explicit() {
        let service = DomainService::new(Arc::new(SessionManager::new()));
        let response = service.execute(DomainRequest {
            operation: "status.inspect".to_string(),
            ..DomainRequest::default()
        });
        assert!(!response.success);
        assert_eq!(response.error_code.as_deref(), Some("SESSION_NOT_FOUND"));
    }

    #[test]
    fn pagination_and_timeout_are_hard_bounded() {
        assert_eq!(validate_limit(1).unwrap(), 1);
        assert_eq!(validate_limit(MAX_PAGE_LIMIT).unwrap(), 128);
        assert!(validate_limit(0).is_err());
        assert!(validate_limit(MAX_PAGE_LIMIT + 1).is_err());

        let service = DomainService::new(Arc::new(SessionManager::new()));
        let response = service.execute(DomainRequest {
            operation: "status.inspect".to_string(),
            timeout_ms: 0,
            ..DomainRequest::default()
        });
        assert_eq!(response.error_code.as_deref(), Some("TIMEOUT_INVALID"));
    }

    #[test]
    fn package_and_class_queries_use_exact_paths_and_query_bound_cursors() {
        let index = indexed_fixture(vec![
            snapshot_record(
                1,
                "FirstPlayer",
                "/Game/Maps/Main.FirstPlayer",
                "/Script/Game.Player",
                "/Game/Maps/Main",
            ),
            snapshot_record(
                2,
                "SecondPlayer",
                "/Game/Maps/Main.SecondPlayer",
                "/Script/Game.Player",
                "/Game/Maps/Main",
            ),
            snapshot_record(
                3,
                "PluginPlayer",
                "/Plugin/Maps/Main.PluginPlayer",
                "/Plugin/Other.Player",
                "/Plugin/Maps/Main",
            ),
        ]);

        let first = package_contents(
            &index,
            &PackageArgs {
                package_path: "/Game/Maps/Main".to_string(),
                cursor: None,
                limit: 1,
            },
        )
        .unwrap();
        assert_eq!(first["count"], 2);
        assert_eq!(first["items"][0]["name"], "FirstPlayer");
        assert_eq!(first["has_more"], true);
        let cursor: SnapshotQueryCursor =
            serde_json::from_value(first["next_cursor"].clone()).unwrap();

        let second = package_contents(
            &index,
            &PackageArgs {
                package_path: "/Game/Maps/Main".to_string(),
                cursor: Some(cursor.clone()),
                limit: 1,
            },
        )
        .unwrap();
        assert_eq!(second["items"][0]["name"], "SecondPlayer");
        assert_eq!(second["has_more"], false);

        let wrong_query = package_contents(
            &index,
            &PackageArgs {
                package_path: "/Plugin/Maps/Main".to_string(),
                cursor: Some(cursor),
                limit: 1,
            },
        )
        .unwrap_err();
        assert_eq!(wrong_query.code, "SNAPSHOT_QUERY_CURSOR_MISMATCH");

        let instances = class_instances(
            &index,
            &ClassInstancesArgs {
                class_path: "/Script/Game.Player".to_string(),
                search: None,
                cursor: None,
                limit: 128,
            },
        )
        .unwrap();
        assert_eq!(instances["matched"], 2);
        assert_eq!(instances["items"][0]["name"], "FirstPlayer");
        assert_eq!(instances["items"][1]["name"], "SecondPlayer");
    }

    #[test]
    fn object_identity_and_query_schema_reject_ambiguous_or_legacy_inputs() {
        let index = indexed_fixture(vec![
            snapshot_record(
                1,
                "DuplicateA",
                "/Game/Shared.Duplicate",
                "/Script/Game.TypeA",
                "/Game/Shared",
            ),
            snapshot_record(
                2,
                "DuplicateB",
                "/Game/Shared.Duplicate",
                "/Script/Game.TypeB",
                "/Game/Shared",
            ),
        ]);
        let ambiguous = object_by_full_path(&index, "/Game/Shared.Duplicate").unwrap_err();
        assert_eq!(ambiguous.code, "OBJECT_IDENTITY_AMBIGUOUS");

        let legacy = parse_data::<ObjectQueryArgs>(&json!({
            "offset": 0,
            "limit": 50,
            "q": "Duplicate"
        }))
        .unwrap_err();
        assert_eq!(legacy.code, "INVALID_ARGUMENT");
    }
}
