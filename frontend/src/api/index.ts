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
  gobjects_address: string;
  object_count: number;
  pid: number;
  architecture: string;
  script_offset_diagnostics?: ScriptOffsetDiagnosticsData;
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
  name: string;
  address: string;
  actor_count: number;
}

export interface WorldLevelItem extends ObjectItem {
  source?: string;
  actor_count?: number;
}

export interface WorldLevelsResponse {
  world: ObjectItem;
  levels: WorldLevelItem[];
  count: number;
}

export type WorldActorItem = ObjectItem;

export interface WorldActorResponse {
  items: WorldActorItem[];
  matched: number;
  offset: number;
  limit: number;
}

export interface Vec3Data {
  x: number;
  y: number;
  z: number;
}

export interface ActorTransformData {
  location?: Vec3Data;
  rotation?: Vec3Data;
  scale?: Vec3Data;
}

export interface WorldActorDetail {
  index: number;
  name: string;
  full_name: string;
  class: string;
  address: string;
  root_component: ObjectItem | null;
  transform: ActorTransformData;
  components: ObjectItem[];
  component_count: number;
}

export interface WorldActorComponentsResponse {
  actor_index: number;
  components: ObjectItem[];
  count: number;
}

export interface WorldActorTransformUpdateResponse {
  actor_index: number;
  updated: boolean;
  rolled_back?: boolean;
  transform: ActorTransformData;
}

export interface WorldShortcuts {
  game_mode: ObjectItem | null;
  game_state: ObjectItem | null;
  player_controller: ObjectItem | null;
  pawn: ObjectItem | null;
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

export interface WatchItem {
  id: number;
  object_index: number;
  property: string;
  value: unknown;
  changed: boolean;
  last_change: number;
  created: number;
}

export interface WatchListResponse {
  watches: WatchItem[];
  count: number;
}

export interface HookItem {
  id: number;
  function_path: string;
  enabled: boolean;
  hit_count: number;
  last_hit_time: number;
}

export interface HookListResponse {
  hooks: HookItem[];
  monitored_count: number;
  total_pe_calls: number;
  vtable_hook_installed: boolean;
  game_thread_enabled: boolean;
}

export interface HookLogEntry {
  timestamp: number;
  function_name: string;
  caller_name: string;
}

export interface HookLogResponse {
  entries: HookLogEntry[];
}

export interface FunctionCallResultData {
  function: string;
  called: boolean;
  object_index?: number;
  class?: string;
  is_static: boolean;
  result: Record<string, unknown>;
}

export interface BatchFunctionCallItem {
  object_index: number;
  called: boolean;
  result?: Record<string, unknown>;
  error?: string;
}

export interface BatchFunctionCallResultData {
  function: string;
  requested: number;
  success: number;
  failed: number;
  items: BatchFunctionCallItem[];
}

export interface BlueprintDecompileData {
  function: string;
  class: string;
  flags: string;
  script_size: number;
  pseudocode: string;
}

export interface BlueprintBytecodeData {
  function: string;
  size: number;
  hex: string;
}

export interface WatchHistoryEntry {
  timestamp: number;
  value: unknown;
}

export interface WatchHistoryData {
  id: number;
  object_index: number;
  property: string;
  history: WatchHistoryEntry[];
  total: number;
}

export type DumpType = 'sdk' | 'usmap' | 'dumpspace' | 'ida-script';

export interface DumpJob {
  id: string;
  format: DumpType;
  status: 'running' | 'completed' | 'failed';
  output_path?: string;
  start_time: number;
  end_time: number;
  duration_ms: number;
  error?: string;
}

export { api, default } from './client';
