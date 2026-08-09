use serde::{Deserialize, Serialize};
use serde_json::{json, Value};
use std::time::{SystemTime, UNIX_EPOCH};

#[cfg(windows)]
use crate::session::session_manager::{ManagedSession, SessionManager, SessionManagerError};
#[cfg(windows)]
use crate::session::snapshot_cache::{SnapshotIndex, SnapshotObjectKind, SnapshotRecord};
#[cfg(windows)]
use std::sync::Arc;
#[cfg(windows)]
use std::time::Duration;

const DEFAULT_TIMEOUT_MS: u32 = 5_000;
const MAX_TIMEOUT_MS: u32 = 120_000;
const DEFAULT_PAGE_LIMIT: u32 = 50;
const MAX_PAGE_LIMIT: u32 = 128;
const MAX_OFFSET: u32 = 8_000_000;
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
                let args: RecordQueryArgs = parse_data(&request.data)?;
                query_objects(index, &args, None)
            }),
            DomainRoute::ObjectsSearch => {
                self.with_snapshot(&session, request.timeout_ms, |index| {
                    let args: RecordQueryArgs = parse_data(&request.data)?;
                    query_objects(index, &args, None)
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
                    let record = index
                        .records()
                        .iter()
                        .find(|record| record.full_path.eq_ignore_ascii_case(&args.path))
                        .ok_or_else(|| {
                            DomainFailure::new(
                                "OBJECT_NOT_FOUND",
                                "No snapshot object has the requested full path",
                                json!({"path": args.path}),
                            )
                        })?;
                    Ok(object_detail(record))
                })
            }
            DomainRoute::TypeList(kind) => {
                self.with_snapshot(&session, request.timeout_ms, |index| {
                    let args: RecordQueryArgs = parse_data(&request.data)?;
                    query_types(index, &args, kind)
                })
            }
            DomainRoute::PackageContents => {
                self.with_snapshot(&session, request.timeout_ms, |index| {
                    let args: PackageArgs = parse_data(&request.data)?;
                    validate_text("package", &args.package)?;
                    package_contents(index, &args.package)
                })
            }
            DomainRoute::ClassInstances => {
                self.with_snapshot(&session, request.timeout_ms, |index| {
                    let args: ClassInstancesArgs = parse_data(&request.data)?;
                    validate_text("class", &args.class_name)?;
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
struct RecordQueryArgs {
    offset: u32,
    limit: u32,
    q: String,
    #[serde(rename = "class")]
    class_filter: String,
    #[serde(rename = "package")]
    package_filter: String,
}

#[cfg(windows)]
impl Default for RecordQueryArgs {
    fn default() -> Self {
        Self {
            offset: 0,
            limit: DEFAULT_PAGE_LIMIT,
            q: String::new(),
            class_filter: String::new(),
            package_filter: String::new(),
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
#[serde(deny_unknown_fields)]
struct PackageArgs {
    package: String,
}

#[cfg(windows)]
#[derive(Debug, Deserialize)]
#[serde(default, deny_unknown_fields)]
struct ClassInstancesArgs {
    class_name: String,
    offset: u32,
    limit: u32,
}

#[cfg(windows)]
impl Default for ClassInstancesArgs {
    fn default() -> Self {
        Self {
            class_name: String::new(),
            offset: 0,
            limit: DEFAULT_PAGE_LIMIT,
        }
    }
}

#[cfg(windows)]
fn validate_page(offset: u32, limit: u32) -> Result<(), DomainFailure> {
    if offset > MAX_OFFSET || !(1..=MAX_PAGE_LIMIT).contains(&limit) {
        return Err(DomainFailure::new(
            "PAGINATION_INVALID",
            "offset must be in range 0..8000000 and limit in range 1..128",
            json!({"offset": offset, "limit": limit}),
        ));
    }
    Ok(())
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
fn validate_optional_text(field: &'static str, value: &str) -> Result<(), DomainFailure> {
    if value.len() > MAX_QUERY_BYTES || value.chars().any(char::is_control) {
        return Err(DomainFailure::new(
            "INVALID_ARGUMENT",
            format!("{field} must be bounded UTF-8 without control characters"),
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
    let mut classes = 0u32;
    let mut structs = 0u32;
    let mut enums = 0u32;
    let mut functions = 0u32;
    let mut packages = 0u32;
    for record in index.records() {
        match record.kind {
            SnapshotObjectKind::Class => classes += 1,
            SnapshotObjectKind::Struct => structs += 1,
            SnapshotObjectKind::Enum => enums += 1,
            SnapshotObjectKind::Function => functions += 1,
            SnapshotObjectKind::Package => packages += 1,
            SnapshotObjectKind::Object => {}
        }
    }
    json!({
        "total": index.record_count(),
        "classes": classes,
        "structs": structs,
        "enums": enums,
        "functions": functions,
        "packages": packages,
    })
}

#[cfg(windows)]
fn query_objects(
    index: &SnapshotIndex,
    args: &RecordQueryArgs,
    kind: Option<SnapshotObjectKind>,
) -> Result<Value, DomainFailure> {
    validate_page(args.offset, args.limit)?;
    validate_optional_text("q", &args.q)?;
    validate_optional_text("class", &args.class_filter)?;
    validate_optional_text("package", &args.package_filter)?;
    let query = args.q.to_lowercase();
    let mut matched = 0u32;
    let mut items = Vec::with_capacity(args.limit as usize);
    for record in index.records() {
        if kind.is_some_and(|expected| record.kind != expected)
            || !record_matches(record, &query, &args.class_filter, &args.package_filter)
        {
            continue;
        }
        if matched >= args.offset && items.len() < args.limit as usize {
            items.push(object_item(record));
        }
        matched += 1;
    }
    Ok(json!({
        "items": items,
        "total": matched,
        "matched": matched,
        "offset": args.offset,
        "limit": args.limit,
        "snapshot_generation": index.generation(),
    }))
}

#[cfg(windows)]
fn record_matches(
    record: &SnapshotRecord,
    lowercase_query: &str,
    class_filter: &str,
    package_filter: &str,
) -> bool {
    let query_matches = lowercase_query.is_empty()
        || [
            &record.name,
            &record.full_path,
            &record.class_path,
            &record.package_path,
        ]
        .iter()
        .any(|value| value.to_lowercase().contains(lowercase_query));
    query_matches
        && (class_filter.is_empty() || path_name_matches(&record.class_path, class_filter))
        && (package_filter.is_empty()
            || record.package_path.eq_ignore_ascii_case(package_filter)
            || path_name_matches(&record.package_path, package_filter))
}

#[cfg(windows)]
fn path_name_matches(path: &str, expected: &str) -> bool {
    if path.eq_ignore_ascii_case(expected) {
        return true;
    }
    path.rsplit(['.', '/', ' '])
        .next()
        .is_some_and(|name| name.eq_ignore_ascii_case(expected))
}

#[cfg(windows)]
fn query_types(
    index: &SnapshotIndex,
    args: &RecordQueryArgs,
    kind: SnapshotObjectKind,
) -> Result<Value, DomainFailure> {
    validate_page(args.offset, args.limit)?;
    validate_optional_text("q", &args.q)?;
    validate_optional_text("class", &args.class_filter)?;
    validate_optional_text("package", &args.package_filter)?;
    let query = args.q.to_lowercase();
    let mut matched = 0u32;
    let mut items = Vec::with_capacity(args.limit as usize);
    for record in index.records() {
        if record.kind != kind
            || !record_matches(record, &query, &args.class_filter, &args.package_filter)
        {
            continue;
        }
        if matched >= args.offset && items.len() < args.limit as usize {
            items.push(json!({
                "index": record.handle.index,
                "name": record.name,
                "full_name": record.full_path,
                "address": record.handle.address,
            }));
        }
        matched += 1;
    }
    Ok(json!({
        "items": items,
        "total": matched,
        "offset": args.offset,
        "limit": args.limit,
        "snapshot_generation": index.generation(),
    }))
}

#[cfg(windows)]
fn package_contents(index: &SnapshotIndex, package: &str) -> Result<Value, DomainFailure> {
    let mut items = Vec::new();
    for record in index.records() {
        if record.package_path.eq_ignore_ascii_case(package)
            || path_name_matches(&record.package_path, package)
        {
            items.push(object_item(record));
            if items.len() > MAX_PAGE_LIMIT as usize {
                return Err(DomainFailure::new(
                    "RESULT_LIMIT_EXCEEDED",
                    "Package contents exceed the bounded response limit; use object search",
                    json!({"package": package, "limit": MAX_PAGE_LIMIT}),
                ));
            }
        }
    }
    let count = items.len();
    Ok(json!({"package": package, "items": items, "count": count}))
}

#[cfg(windows)]
fn class_instances(
    index: &SnapshotIndex,
    args: &ClassInstancesArgs,
) -> Result<Value, DomainFailure> {
    validate_page(args.offset, args.limit)?;
    let mut matched = 0u32;
    let mut items = Vec::with_capacity(args.limit as usize);
    for record in index.records() {
        if !path_name_matches(&record.class_path, &args.class_name) {
            continue;
        }
        if matched >= args.offset && items.len() < args.limit as usize {
            items.push(json!({
                "index": record.handle.index,
                "name": record.name,
                "address": record.handle.address,
            }));
        }
        matched += 1;
    }
    Ok(json!({
        "class": args.class_name,
        "items": items,
        "matched": matched,
        "offset": args.offset,
        "limit": args.limit,
    }))
}

#[cfg(test)]
#[cfg(windows)]
mod tests {
    use super::*;

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
        assert!(validate_page(0, 1).is_ok());
        assert!(validate_page(MAX_OFFSET, MAX_PAGE_LIMIT).is_ok());
        assert!(validate_page(MAX_OFFSET + 1, 1).is_err());
        assert!(validate_page(0, MAX_PAGE_LIMIT + 1).is_err());

        let service = DomainService::new(Arc::new(SessionManager::new()));
        let response = service.execute(DomainRequest {
            operation: "status.inspect".to_string(),
            timeout_ms: 0,
            ..DomainRequest::default()
        });
        assert_eq!(response.error_code.as_deref(), Some("TIMEOUT_INVALID"));
    }
}
