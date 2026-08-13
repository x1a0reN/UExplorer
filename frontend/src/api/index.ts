export interface ApiResponse<T> {
  success: boolean;
  data: T | null;
  error: string | null;
  error_code?: string | null;
  details?: unknown;
  timing?: { queued_us: number; execute_us: number } | null;
  timestamp?: number;
}

export interface ApiClientSettings {
  dllPath: string;
  defaultDumpFormat: DumpType;
  outputDir: string;
}

export interface HostProcessInfo {
  pid: number;
  name: string;
  path: string;
  start_time_100ns: string;
  architecture: string;
  candidate_reasons: string[];
}

export interface InjectionCommandResult {
  success: boolean;
  status: 'ready' | 'failed';
  stage: string;
  code: string;
  message: string;
  dll: 'not_attempted' | 'loaded' | 'already_loaded' | 'indeterminate' | 'failed';
  pipe: 'not_attempted' | 'connected' | 'failed' | 'rejected';
  core: 'not_checked' | 'ready' | 'failed';
  session?: {
    target_pid: number;
    target_start_time_100ns: string;
    target_process_path: string;
    phase: 'ready' | 'closing' | 'closed' | 'failed';
    core_session_id: string;
    capabilities: Record<string, boolean>;
  } | null;
}

export interface HostEventPayload {
  seq: number;
  kind: string;
  timestamp_us: number;
  session_id: string;
  dropped_before: number;
  data: unknown;
}

export interface HostSessionEvent {
  event: HostEventPayload;
  host_dropped_before: number;
}

export interface WatchPushEventData {
  watch_id: string;
  id: number;
  source_sequence: number;
  captured_at_monotonic_us: number;
  value: WatchValue | null;
  value_omitted: boolean;
  value_omission_code?: string;
  reason_code: string | null;
  reason: string | null;
  pull_dropped_before: number;
  pull_coalesced_before: number;
  push_dropped_before: number;
  push_coalesced_before: number;
  publisher_dropped_before: number;
}

export interface HookPushEventData {
  hook_name: string;
  hook_id: string;
  id: number;
  function_path: string;
  source_sequence: number;
  configuration_generation: number;
  source: string;
  correlation: number;
  coalesced_before: number;
  drained_at_monotonic_us: number;
  capture: { mode: 'fixed_metadata' | 'preencoded_payload' };
  payload: { encoding: 'hex'; size: number; data: string } | null;
  payload_omitted: boolean;
  payload_omission_code?: string;
  retained_log_dropped_before: number;
  collector_overflow_dropped_before: number;
  collector_oversize_dropped_before: number;
  collector_contention_dropped_before: number;
  push_dropped_before: number;
  publisher_dropped_before: number;
}

export function isWatchPushEventData(value: unknown): value is WatchPushEventData {
  if (!value || typeof value !== 'object') return false;
  const data = value as Record<string, unknown>;
  return typeof data.watch_id === 'string'
    && typeof data.id === 'number' && Number.isSafeInteger(data.id) && data.id > 0
    && typeof data.source_sequence === 'number'
    && Number.isSafeInteger(data.source_sequence) && data.source_sequence > 0
    && typeof data.captured_at_monotonic_us === 'number'
    && Number.isSafeInteger(data.captured_at_monotonic_us)
    && typeof data.push_dropped_before === 'number'
    && Number.isSafeInteger(data.push_dropped_before) && data.push_dropped_before >= 0
    && (data.value === null || (typeof data.value === 'object' && data.value !== null))
    && typeof data.value_omitted === 'boolean';
}

export function isHookPushEventData(value: unknown): value is HookPushEventData {
  if (!value || typeof value !== 'object') return false;
  const data = value as Record<string, unknown>;
  return typeof data.hook_name === 'string'
    && typeof data.hook_id === 'string'
    && typeof data.id === 'number' && Number.isSafeInteger(data.id) && data.id > 0
    && typeof data.source_sequence === 'number'
    && Number.isSafeInteger(data.source_sequence) && data.source_sequence > 0
    && typeof data.configuration_generation === 'number'
    && Number.isSafeInteger(data.configuration_generation)
    && typeof data.function_path === 'string'
    && typeof data.source === 'string'
    && typeof data.correlation === 'number' && Number.isSafeInteger(data.correlation)
    && typeof data.payload_omitted === 'boolean';
}

export interface SessionEventFilter {
  kinds?: string[];
  watch_ids?: string[];
  hook_names?: string[];
}

export interface EventBridgeFailure {
  code: string;
  message: string;
}

export interface EventBridgeDiagnostics {
  bridge_id: number;
  target_pid: number;
  source_subscription_id: number;
  delivered_events: number;
  stopping: boolean;
  finished: boolean;
  failure: EventBridgeFailure | null;
}

export interface SessionEventSubscribeOptions {
  filter?: SessionEventFilter;
  replayAfterSeq?: number | null;
  capacity?: number;
  onEvent: (event: HostSessionEvent) => void;
}

export interface SessionEventSubscription {
  diagnostics: EventBridgeDiagnostics;
  unsubscribe: () => Promise<boolean>;
}

export interface StatusData {
  game_name: string;
  game_version: string;
  blueprint_profile_id?: string | null;
  gobjects_address: string;
  object_count: number;
  pid: number;
  architecture: string;
  script_offset_diagnostics?: ScriptOffsetDiagnosticsData;
  runtime?: {
    state: string;
    session_id: string | null;
    liveness: boolean;
    readiness: boolean;
    active_requests: number;
    context_generation: number | null;
  };
  capabilities?: Record<string, {
    available: boolean;
    reason_code: string | null;
    reason: string | null;
    dependencies: string[];
  }>;
  object_snapshot?: {
    published: boolean;
    generation: number | null;
    context_generation?: number;
    object_count?: number;
  };
  type_snapshot?: {
    published: boolean;
    generation: number | null;
    context_generation?: number;
    object_snapshot_generation?: number;
    type_count?: number;
  };
}

export interface ScriptOffsetDiagnosticsData {
  selected_offset: number;
  selected_score: number;
  score_gap_top2: number;
  bp_end_hits: number;
  weighted_bp_end_hits: number;
  generic_script_hits: number;
  verify_probed: number;
  verify_header_valid: number;
  verify_end_hits: number;
  verify_first_opcode_valid: number;
  verify_size_sane: number;
  verify_end_rate: number;
  verify_opcode_rate: number;
  confidence: string;
  anomaly_tags: string;
}

export interface EngineStatusData {
  game_name: string;
  game_version: string;
  blueprint_profile_id?: string | null;
  architecture: string;
  pid: number;
  object_count: number;
  offsets: Record<string, number>;
  addresses: Record<string, string | null>;
  internals: Record<string, boolean>;
  script_offset_diagnostics: ScriptOffsetDiagnosticsData;
}

export interface ReconnectStatusData {
  reconnected: boolean;
  object_count: number;
  game_name: string;
  game_version: string;
  gobjects_address: string;
}

export interface ObjectItem {
  index: number;
  name: string;
  class: string;
  address: string;
}

export interface ObjectDetail extends ObjectItem {
  full_name: string;
  outer_chain?: string[];
  package_path?: string;
  kind?: string;
  handle?: StableObjectHandle;
  flags?: string;
  flags_raw?: number;
}

export interface ObjectProperty {
  name: string;
  property_name: string;
  type: string;
  kind: string;
  offset: number;
  size: number;
  array_index: number;
  array_dim: number;
  object: StableObjectHandle;
  object_snapshot_generation: number;
  type_snapshot_generation: number;
  declaring_type_path: string;
  descriptor_available: boolean;
  value: unknown;
  value_state: string;
}

export interface OuterChainItem {
  name: string;
  full_name: string;
  index: number;
  address: string;
}

export interface ObjectOuterChainData {
  index: number;
  name: string;
  outer_chain: OuterChainItem[];
}

export interface ObjectPropertyValueData {
  object: StableObjectHandle;
  type_snapshot_generation: number;
  object_snapshot_generation: number;
  declaring_type_path: string;
  property_name: string;
  array_index: number;
  offset: number;
  size: number;
  value: unknown;
}

export interface SnapshotQueryCursor {
  generation: number;
  after_index: number;
  query_fingerprint: string;
}

export type SnapshotObjectKind = 'object' | 'package' | 'class' | 'struct' | 'enum' | 'function';

export interface SnapshotPageResponse<T> {
  items: T[];
  total: number;
  matched: number;
  limit: number;
  has_more: boolean;
  next_cursor: SnapshotQueryCursor | null;
  snapshot_generation: number;
  context_generation: number;
  source_object_count: number;
  snapshot_record_count: number;
}

export type ObjectsResponse = SnapshotPageResponse<ObjectItem>;

export type SearchResponse = SnapshotPageResponse<ObjectItem>;

export interface ObjectCountData {
  total: number;
  classes: number;
  structs: number;
  enums: number;
  functions: number;
  packages: number;
}

export interface ClassItem {
  index: number;
  name: string;
  full_name: string;
  size?: number;
  super?: string;
  address: string;
}

export interface StableObjectHandle {
  session_id: string;
  context_generation: number;
  index: number;
  serial: number;
  address: string;
  class_fingerprint: string;
}

export type ReflectedTypeKind = 'class' | 'struct' | 'enum';
export type ReflectedMemberState = 'supported' | 'unsupported' | 'unavailable';
export type TypeMemberScope = 'direct' | 'include_inherited';

export interface TypeQueryCursor {
  generation: number;
  after_ordinal: number;
  query_fingerprint: string;
}

export interface TypeReference {
  handle: StableObjectHandle;
  kind: ReflectedTypeKind;
  name: string;
  full_path: string;
}

export interface ClassProperty {
  name: string;
  type_name: string;
  kind: string;
  offset: number;
  size: number;
  array_dim: number;
  flags: string;
  state: ReflectedMemberState;
  reason_code: string | null;
  reason: string | null;
  descriptor_available: boolean;
  declaring_type: TypeReference;
  inheritance_depth: number;
}

export interface ClassFunctionParameter {
  name: string;
  type_name: string;
  kind: string;
  offset: number;
  size: number;
  array_dim: number;
  flags: string;
  state: ReflectedMemberState;
  reason_code: string | null;
  reason: string | null;
  descriptor_available: boolean;
  direction: 'input' | 'output' | 'inout' | 'return';
}

export interface StableFunctionHandle {
  function: StableObjectHandle;
  owner: StableObjectHandle;
  full_path: string;
  signature_fingerprint: string;
}

export interface ClassFunction {
  handle: StableFunctionHandle;
  name: string;
  full_path: string;
  flags: string;
  parameter_size: number;
  parameter_count: number;
  native_address: string | null;
  implementation: 'unavailable' | 'native' | 'bytecode' | 'native_and_bytecode';
  reason_code: string | null;
  reason: string | null;
  parameters: ClassFunctionParameter[];
  declaring_type: TypeReference;
  inheritance_depth: number;
}

export interface FunctionDetail extends ClassFunction {
  type_snapshot_generation: number;
  object_snapshot_generation: number;
  context_generation: number;
}

export interface TypeDefaultObjectMetadata {
  state: 'not_applicable' | 'present' | 'not_constructed' | 'unavailable';
  handle: StableObjectHandle | null;
  reason_code: string | null;
  reason: string | null;
}

export interface TypeEnumMetadata {
  state: ReflectedMemberState;
  underlying_kind: string | null;
  reason_code: string | null;
  reason: string | null;
  value_count: number;
}

export interface TypeDetail {
  type_snapshot_generation: number;
  object_snapshot_generation: number;
  context_generation: number;
  handle: StableObjectHandle;
  kind: ReflectedTypeKind;
  name: string;
  full_path: string;
  package_path: string;
  properties_size: number;
  min_alignment: number;
  super: TypeReference | null;
  direct_property_count: number;
  direct_function_count: number;
  default_object: TypeDefaultObjectMetadata | null;
  enum: TypeEnumMetadata | null;
}

export type ClassDetail = TypeDetail;

export interface TypeMemberPageResponse<T> {
  type_snapshot_generation: number;
  object_snapshot_generation: number;
  context_generation: number;
  path: string;
  kind: ReflectedTypeKind;
  scope: TypeMemberScope;
  items: T[];
  total: number;
  matched: number;
  limit: number;
  has_more: boolean;
  next_cursor: TypeQueryCursor | null;
}

export type PaginatedResponse<T> = SnapshotPageResponse<T>;

export interface StructItem {
  index: number;
  name: string;
  full_name: string;
  size?: number;
  super?: string;
  address: string;
}

export type StructDetail = TypeDetail;

export interface EnumItem {
  index: number;
  name: string;
  full_name: string;
  address: string;
}

export interface EnumDetail {
  type_snapshot_generation: number;
  object_snapshot_generation: number;
  context_generation: number;
  handle: StableObjectHandle;
  kind: 'enum';
  name: string;
  full_path: string;
  package_path: string;
  properties_size: 0;
  min_alignment: 0;
  super: null;
  direct_property_count: 0;
  direct_function_count: 0;
  default_object: null;
  enum: TypeEnumMetadata;
}

export interface EnumValue {
  name: string;
  value: string;
}

export interface EnumValuePageResponse {
  type_snapshot_generation: number;
  object_snapshot_generation: number;
  context_generation: number;
  path: string;
  state: ReflectedMemberState;
  underlying_kind: string | null;
  reason_code: string | null;
  reason: string | null;
  items: EnumValue[];
  total: number;
  matched: number;
  limit: number;
  has_more: boolean;
  next_cursor: TypeQueryCursor | null;
}

export interface PackageItem {
  index: number;
  name: string;
  full_name: string;
  address: string;
}

export interface PackageContentsResponse extends SnapshotPageResponse<ObjectItem> {
  package: string;
  count: number;
}

export interface ClassHierarchy {
  type_snapshot_generation: number;
  object_snapshot_generation: number;
  context_generation: number;
  path: string;
  parents: Array<TypeReference & { inheritance_depth: number }>;
  children: TypeReference[];
  total: number;
  matched: number;
  limit: number;
  has_more: boolean;
  next_cursor: TypeQueryCursor | null;
}

export interface ClassInstancesResponse
  extends SnapshotPageResponse<{ index: number; name: string; address: string; outer_name?: string }> {
  class: string;
}

export interface ClassCDOResponse {
  type_snapshot_generation: number;
  object_snapshot_generation: number;
  context_generation: number;
  class_path: string;
  state: Exclude<TypeDefaultObjectMetadata['state'], 'not_applicable'>;
  handle: StableObjectHandle | null;
  reason_code: string | null;
  reason: string | null;
}

export interface WorldData {
  generation: number;
  context_generation: number;
  object_snapshot_generation: number;
  type_snapshot_generation: number;
  captured_at_monotonic_us: number;
  capture_duration_us: number;
  world: WorldSnapshotObject;
  level_count: number;
  actor_count: number;
  components: WorldCollectionAvailability;
}

export interface WorldQueryCursor {
  generation: number;
  after_ordinal: number;
  query_fingerprint: string;
}

export interface WorldSnapshotObject extends ObjectItem {
  handle: StableObjectHandle;
  full_path: string;
  class_path: string;
}

export interface WorldLevelItem extends WorldSnapshotObject {
  source: string;
  actor_count: number;
}

export interface WorldLevelsResponse {
  generation: number;
  context_generation: number;
  object_snapshot_generation: number;
  type_snapshot_generation: number;
  world: WorldSnapshotObject;
  levels: WorldLevelItem[];
  count: number;
  limit: number;
  has_more: boolean;
  next_cursor: WorldQueryCursor | null;
}

export interface WorldActorItem extends WorldSnapshotObject {
  level: WorldSnapshotObject;
}

export interface WorldActorResponse {
  generation: number;
  context_generation: number;
  object_snapshot_generation: number;
  type_snapshot_generation: number;
  items: WorldActorItem[];
  matched: number;
  limit: number;
  has_more: boolean;
  next_cursor: WorldQueryCursor | null;
}

export interface Vec3Data {
  x: number;
  y: number;
  z: number;
}

export interface RotatorData {
  pitch: number;
  yaw: number;
  roll: number;
}

export interface ActorTransformData {
  location?: Vec3Data;
  rotation?: Vec3Data;
  scale?: Vec3Data;
}

export type WorldReferenceState = 'present' | 'not_present' | 'unavailable';

export interface WorldReference {
  state: WorldReferenceState;
  object: WorldSnapshotObject | null;
  reason_code: string | null;
  reason: string | null;
}

export interface WorldCollectionAvailability {
  state: 'available' | 'unavailable';
  count: number | null;
  reason_code: string | null;
  reason: string | null;
}

export interface WorldTransformAvailability {
  state: 'unavailable';
  reason_code: string;
  reason: string;
}

export interface WorldActorDetail {
  generation: number;
  context_generation: number;
  object_snapshot_generation: number;
  type_snapshot_generation: number;
  actor: WorldSnapshotObject;
  level: WorldSnapshotObject;
  root_component: WorldReference;
  components: WorldCollectionAvailability;
  transform: WorldTransformAvailability;
}

export interface WorldActorComponentsResponse {
  generation: number;
  context_generation: number;
  object_snapshot_generation: number;
  type_snapshot_generation: number;
  actor: WorldSnapshotObject;
  components: WorldSnapshotObject[];
  count: number;
  limit: number;
  has_more: boolean;
  next_cursor: WorldQueryCursor | null;
}

export type WorldTransformSpace = 'relative' | 'world';

export interface WorldActorStoredTransform {
  source: 'scene_component_stored_relative';
  computed_world: false;
  precision: 'float32' | 'float64';
  location: Vec3Data;
  rotation: RotatorData;
  scale: Vec3Data;
  absolute: {
    location: boolean;
    rotation: boolean;
    scale: boolean;
  };
  space: {
    location: WorldTransformSpace;
    rotation: WorldTransformSpace;
    scale: WorldTransformSpace;
  };
}

export type WorldActorComputedTransform =
  | {
      state: 'available';
      source: 'actor_reflected_getters';
      computed_world: true;
      precision: 'float32' | 'float64';
      location: Vec3Data;
      rotation: RotatorData;
      scale: Vec3Data;
    }
  | {
      state: 'unavailable';
      reason_code: string;
      reason: string;
    };

export interface WorldActorTransformResponse {
  generation: number;
  context_generation: number;
  object_snapshot_generation: number;
  type_snapshot_generation: number;
  actor: WorldSnapshotObject;
  root_component: WorldSnapshotObject;
  transform: WorldActorStoredTransform;
  computed_transform: WorldActorComputedTransform;
}

export type WorldTransformInputNumber = string;

export type WorldActorTransformUpdate =
  | {
      field: 'scale';
      space: WorldTransformSpace;
      value: { x: WorldTransformInputNumber; y: WorldTransformInputNumber; z: WorldTransformInputNumber };
    }
  | {
      field: 'rotation';
      space: 'world';
      value: { pitch: WorldTransformInputNumber; yaw: WorldTransformInputNumber; roll: WorldTransformInputNumber };
      teleport_physics: boolean;
    }
  | {
      field: 'location';
      space: WorldTransformSpace;
      value: { x: WorldTransformInputNumber; y: WorldTransformInputNumber; z: WorldTransformInputNumber };
      sweep: boolean;
      teleport: boolean;
    }
  | {
      field: 'rotation';
      space: 'relative';
      value: { pitch: WorldTransformInputNumber; yaw: WorldTransformInputNumber; roll: WorldTransformInputNumber };
      sweep: boolean;
      teleport: boolean;
    };

export type WorldActorTransformAppliedUpdate =
  | {
      field: 'scale';
      space: WorldTransformSpace;
      value: Vec3Data;
    }
  | {
      field: 'rotation';
      space: 'world';
      value: RotatorData;
      teleport_physics: boolean;
    };

export interface WorldActorTransformUpdateResponse {
  session_id: string;
  context_generation: number;
  object_snapshot_generation: number;
  type_snapshot_generation: number;
  world_snapshot_generation: number;
  actor: WorldSnapshotObject;
  root_component: WorldSnapshotObject;
  target: StableObjectHandle;
  setter: {
    function_path: string;
    handle: StableFunctionHandle;
  };
  update: WorldActorTransformAppliedUpdate;
  execution: {
    invoked: true;
    atomicity: 'single_field_process_event';
    post_identity_validated: true;
    setter_result: boolean | null;
    mutation_state:
      | 'setter_reported_applied'
      | 'setter_reported_not_applied'
      | 'setter_returned_without_result';
  };
}

export interface WorldShortcuts {
  generation: number;
  context_generation: number;
  object_snapshot_generation: number;
  type_snapshot_generation: number;
  world: WorldSnapshotObject;
  game_mode: WorldReference;
  game_state: WorldReference;
  player_controller: WorldReference;
  pawn: WorldReference;
}

export interface MemoryReadData {
  address: string;
  size: number;
  hex: string;
  interpret: Record<string, unknown>;
}

export interface MemoryTypedData {
  address: string;
  type: string;
  value: unknown;
}

export interface PointerChainData {
  final_address?: string;
  steps: Array<Record<string, unknown>>;
  value?: Record<string, unknown>;
  error?: string;
}

export interface WatchSpec {
  object: StableObjectHandle;
  context_generation: number;
  object_snapshot_generation: number;
  type_snapshot_generation: number;
  declaring_type_path: string;
  property_name: string;
  array_index: number;
  interval_ms: number;
}

export interface WatchItem {
  id: number;
  state: 'enabled' | 'disabled' | 'terminal';
  spec: WatchSpec;
  created_at_monotonic_us: number;
  next_due_monotonic_us: number;
  last_sampled_at_monotonic_us: number;
  last_change_sequence: number;
  sample_count: number;
  failure_count: number;
  history_count: number;
  history_bytes: number;
  history_drop_count: number;
  terminal_reason_code: string | null;
  terminal_reason: string | null;
}

export interface WatchListResponse {
  enabled_snapshot_generation: number;
  subscriptions: WatchItem[];
  scheduler: Record<string, unknown>;
}

export type HookCapturePolicy =
  | { mode: 'fixed_metadata' }
  | { mode: 'preencoded_payload'; max_payload_bytes: number };

export interface HookSubscriptionSpec {
  session_id: string;
  context_generation: number;
  object_snapshot_generation: number;
  type_snapshot_generation: number;
  function: StableFunctionHandle;
  function_path: string;
  capture: HookCapturePolicy;
}

export interface HookItem {
  id: number;
  state: 'enabled' | 'disabled' | 'terminal';
  function_path: string;
  enabled: boolean;
  hit_count: number;
  spec: HookSubscriptionSpec;
  created_at_monotonic_us: number;
  last_event_sequence: number;
  last_correlation: number;
  log_count: number;
  log_bytes: number;
  log_drop_count: number;
  terminal_reason_code: string | null;
  terminal_reason: string | null;
}

export interface HookListResponse {
  hooks: HookItem[];
  monitored_count: number;
  enabled_snapshot_generation: number;
  collector: {
    drained_count: number;
    more_available: boolean;
    published_total: number;
    drained_total: number;
    dropped_overflow_total: number;
    dropped_oversize_total: number;
    dropped_contention_total: number;
    coalesced_overflow_total: number;
    unmatched_event_total: number;
    policy_rejected_event_total: number;
    drain_failure_total: number;
    pending_push_event_count: number;
    pending_push_event_bytes: number;
    push_dropped_total: number;
  };
}

export interface HookMutationResponse {
  subscription: HookItem;
  enabled_snapshot_generation: number;
}

export interface HookLogEntry {
  sequence: number;
  configuration_generation: number;
  kind: 'post_render' | 'process_event_enter' | 'process_event_exit' | 'diagnostic';
  source: string;
  subject: number;
  correlation: number;
  coalesced_before: number;
  drained_at_monotonic_us: number;
  function_path: string;
  payload: { encoding: 'hex'; size: number; data: string };
  push_payload_omitted?: boolean;
  push_payload_omission_code?: string;
}

export interface HookLogResponse {
  subscription: HookItem;
  entries: HookLogEntry[];
  returned: number;
  total: number;
  truncated: boolean;
  collector_more_available: boolean;
}

export type FunctionCallArgument =
  | { kind: 'bool'; value: boolean }
  | { kind: 'int8' | 'int16' | 'int32' | 'int64'; value: string }
  | { kind: 'uint8' | 'uint16' | 'uint32' | 'uint64'; value: string }
  | { kind: 'float' | 'double'; value: string }
  | { kind: 'object'; value: StableObjectHandle | null }
  | {
      kind: 'enum';
      type_name: string;
      value: { name: string } | { raw: string };
    }
  | {
      kind: 'struct';
      type_name: '/Script/CoreUObject.Vector';
      value: { X: string; Y: string; Z: string };
    }
  | {
      kind: 'struct';
      type_name: '/Script/CoreUObject.Rotator';
      value: { Pitch: string; Yaw: string; Roll: string };
    };

export interface FunctionCallResultData {
  target: StableObjectHandle;
  function: StableFunctionHandle;
  function_path: string;
  type_snapshot_generation: number;
  object_snapshot_generation: number;
  invoked: true;
  outputs: Array<{
    name: string;
    direction: 'output' | 'inout' | 'return';
    value: unknown;
  }>;
}

export interface FunctionCallBatchScope {
  session_id: string;
  context_generation: number;
  object_snapshot_generation: number;
  type_snapshot_generation: number;
}

export type FunctionCallBatchPolicy = 'continue_on_error' | 'stop_on_first_failure';

export interface FunctionCallBatchItemRequest {
  target: StableObjectHandle;
  function: StableFunctionHandle;
  function_path: string;
  arguments: Record<string, FunctionCallArgument>;
}

export interface FunctionCallBatchSubmitRequest extends FunctionCallBatchScope {
  policy: FunctionCallBatchPolicy;
  deadline_ms: number;
  items: FunctionCallBatchItemRequest[];
}

export interface FunctionCallBatchLookupRequest extends FunctionCallBatchScope {
  batch_id: string;
}

export interface FunctionCallBatchListRequest extends FunctionCallBatchScope {
  max_batches: number;
}

export type FunctionCallBatchState =
  | 'queued'
  | 'running'
  | 'succeeded'
  | 'failed'
  | 'cancelled'
  | 'deadline_exceeded';

export type FunctionCallBatchItemState =
  | 'pending'
  | 'running'
  | 'succeeded'
  | 'failed'
  | 'cancelled'
  | 'deadline_exceeded'
  | 'skipped_fail_fast';

export interface FunctionCallBatchMetadata {
  batch_id: string;
  scope: FunctionCallBatchScope;
  policy: FunctionCallBatchPolicy;
  state: FunctionCallBatchState;
  submitted_at_monotonic_us: number;
  deadline_at_monotonic_us: number;
  started_at_monotonic_us: number;
  finished_at_monotonic_us: number;
  cancellation_requested: boolean;
  deadline_exceeded: boolean;
  shutdown_cancellation_requested: boolean;
  retained_result_bytes: number;
  item_count: number;
  item_counts: {
    succeeded: number;
    failed: number;
    cancelled_or_deadline: number;
    pending_or_running: number;
  };
}

export interface FunctionCallBatchItemResult {
  index: number;
  state: FunctionCallBatchItemState;
  target: StableObjectHandle;
  function: StableFunctionHandle;
  function_path: string;
  request_bytes: number;
  request_fingerprint: string;
  started_at_monotonic_us: number;
  finished_at_monotonic_us: number;
  response: FunctionCallResultData | null;
  error: { code: string; message: string } | null;
}

export interface FunctionCallBatchRecord extends FunctionCallBatchMetadata {
  items: FunctionCallBatchItemResult[];
}

export interface FunctionCallBatchSubmitResponse {
  batch_id: string;
  admission: 'accepted';
  item_count: number;
  policy: FunctionCallBatchPolicy;
  scope: FunctionCallBatchScope;
}

export interface FunctionCallBatchGetResponse {
  batch: FunctionCallBatchRecord;
}

export interface FunctionCallBatchCancelResponse {
  batch_id: string;
  disposition: 'cancelled_before_start' | 'cancellation_requested';
}

export interface FunctionCallBatchListResponse {
  scope: FunctionCallBatchScope;
  batches: FunctionCallBatchMetadata[];
  more_batches_available: boolean;
}

export interface BlueprintCaptureData {
  function: StableFunctionHandle;
  function_path: string;
  context_generation: number;
  object_snapshot_generation: number;
  type_snapshot_generation: number;
  byte_length: number;
  capture_source: string;
  captured_at_monotonic_us: string;
  script_field_offset: number;
  script_data_address: string;
  script_num: number;
  script_max: number;
  header_witness_fingerprint: string;
  capture_fingerprint: string;
}

export interface BlueprintInstruction {
  offset: number;
  size: number;
  depth: number;
  raw_opcode: string;
  semantic: 'unknown' | 'expr_token' | 'primitive_cast';
  token: string | null;
  token_value: string | null;
  text: string;
}

export interface BlueprintDecompileData extends BlueprintCaptureData {
  profile: { id: string; source: string; fingerprint: string };
  disassembly: {
    status: 'complete' | 'incomplete' | 'error';
    profile_id: string;
    input_size: number;
    bytes_consumed: number;
    coverage: number;
    unknown_count: number;
    saw_end_of_script: boolean;
    first_error: { offset: number; code: string; message: string } | null;
    instructions: BlueprintInstruction[];
    pseudocode: string;
  };
}

export interface BlueprintBytecodeData extends BlueprintCaptureData {
  encoding: 'hex_upper';
  bytecode: string;
}

export interface WatchHistoryEntry {
  sequence: number;
  captured_at_monotonic_us: number;
  value: WatchValue | null;
}

export interface WatchHistoryData {
  subscription: WatchItem;
  last_value: WatchValue | null;
  history: WatchHistoryEntry[];
  history_returned: number;
  history_total: number;
  history_truncated: boolean;
}

export interface WatchValue {
  encoding: 'uexplorer.property-value.v1.base64';
  type_name: string;
  canonical_value: string;
  display_value: string;
}

export type WatchEventKind =
  | 'value_changed'
  | 'sample_unavailable'
  | 'sample_failed'
  | 'terminal_stale';

export interface WatchEvent {
  sequence: number;
  id: number;
  kind: WatchEventKind;
  captured_at_monotonic_us: number;
  value: WatchValue | null;
  reason_code: string | null;
  reason: string | null;
  drop_count: number;
  coalesce_count: number;
}

export interface WatchDrainData {
  events: WatchEvent[];
  count: number;
  dropped_total: number;
  coalesced_total: number;
  more_available: boolean;
}

export type DumpType = 'sdk' | 'usmap' | 'dumpspace' | 'ida-script';

export interface DumpScope {
  session_id: string;
  context_generation: number;
  object_snapshot_generation: number;
  type_snapshot_generation: number;
}

export interface DumpStartRequest extends DumpScope {
  format: DumpType;
  deadline_ms: number;
  options: Record<string, never>;
}

export interface DumpJobRecord {
  job_id: string;
  format: DumpType;
  state: 'queued' | 'running' | 'succeeded' | 'failed' | 'cancelled';
  scope: DumpScope;
  output_path_identity: string;
  submitted_at_monotonic_us: number;
  deadline_at_monotonic_us: number;
  started_at_monotonic_us: number;
  finished_at_monotonic_us: number;
  cancellation_requested: boolean;
  deadline_exceeded: boolean;
  error: { code: string; message: string } | null;
  retained_event_count: number;
  retained_event_bytes: number;
  dropped_event_count: number;
  last_event_sequence: number;
}

export type DumpJobEvent = {
  sequence: number;
  recorded_at_monotonic_us: number;
} & (
  | {
      kind: 'progress';
      payload: { phase: string; completed: number; total: number; message: string };
    }
  | {
      kind: 'diagnostic';
      payload: { severity: 'info' | 'warning' | 'error'; code: string; message: string };
    }
);

export interface DumpJobListResponse {
  scope: DumpScope;
  jobs: DumpJobRecord[];
  more_jobs_available: boolean;
}

export interface DumpJobSnapshotResponse {
  job: DumpJobRecord;
  events: DumpJobEvent[];
  more_events_available: boolean;
  event_gap_detected: boolean;
}

export { api, default } from './client';
