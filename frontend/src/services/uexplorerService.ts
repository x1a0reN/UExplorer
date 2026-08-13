import type {
  ApiClientSettings,
  ApiResponse,
  BlueprintBytecodeData,
  BlueprintDecompileData,
  ClassCDOResponse,
  ClassDetail,
  ClassFunction,
  ClassHierarchy,
  ClassInstancesResponse,
  ClassItem,
  ClassProperty,
  DumpJobListResponse,
  DumpJobSnapshotResponse,
  DumpScope,
  DumpStartRequest,
  DumpType,
  EngineStatusData,
  EnumDetail,
  EnumValuePageResponse,
  EnumItem,
  EventBridgeDiagnostics,
  FunctionCallResultData,
  FunctionCallArgument,
  FunctionCallBatchCancelResponse,
  FunctionCallBatchGetResponse,
  FunctionCallBatchListRequest,
  FunctionCallBatchListResponse,
  FunctionCallBatchLookupRequest,
  FunctionCallBatchSubmitRequest,
  FunctionCallBatchSubmitResponse,
  FunctionDetail,
  HookListResponse,
  HookLogResponse,
  HookMutationResponse,
  HookCapturePolicy,
  HostProcessInfo,
  InjectionCommandResult,
  MemoryReadData,
  MemoryTypedData,
  ObjectCountData,
  ObjectDetail,
  ObjectOuterChainData,
  ObjectProperty,
  ObjectPropertyValueData,
  ObjectsResponse,
  PackageContentsResponse,
  PackageItem,
  PaginatedResponse,
  PointerChainData,
  ReconnectStatusData,
  SearchResponse,
  SessionEventSubscribeOptions,
  SessionEventSubscription,
  SnapshotObjectKind,
  SnapshotQueryCursor,
  StableObjectHandle,
  StatusData,
  StructDetail,
  StructItem,
  TypeMemberPageResponse,
  TypeMemberScope,
  TypeQueryCursor,
  WatchHistoryData,
  WatchDrainData,
  WatchListResponse,
  WorldActorComponentsResponse,
  WorldActorDetail,
  WorldActorResponse,
  WorldActorTransformResponse,
  WorldActorTransformUpdate,
  WorldActorTransformUpdateResponse,
  WorldData,
  WorldLevelsResponse,
  WorldQueryCursor,
  WorldShortcuts,
} from '../contracts';
import { settingsService } from './settingsService';
import { tauriTransport } from '../transport/tauriTransport';

const MAX_PROPERTY_ROWS = 8_192;

class UExplorerApi {
  getSettings(): ApiClientSettings {
    return settingsService.get();
  }

  updateSettings(next: Partial<ApiClientSettings>): void {
    settingsService.update(next);
  }

  private async command<T>(
    operation: string,
    data: Record<string, unknown> = {},
    timeoutMs = 5_000,
    targetPid: number | null = null,
  ): Promise<ApiResponse<T>> {
    return tauriTransport.requestDomain<T>(operation, data, { timeoutMs, targetPid });
  }

  private failureFrom<T>(response: ApiResponse<unknown>): ApiResponse<T> {
    return {
      ...response,
      success: false,
      data: null,
    };
  }

  private localFailure<T>(code: string, message: string, details?: unknown): ApiResponse<T> {
    return {
      success: false,
      data: null,
      error: `${code}: ${message}`,
      error_code: code,
      details,
      timing: null,
      timestamp: Date.now(),
    };
  }

  async getStatus(): Promise<ApiResponse<StatusData>> {
    return this.command('status.inspect');
  }

  async getEngineStatus(): Promise<ApiResponse<EngineStatusData>> {
    return this.command('status.engine');
  }

  async reconnectEngine(): Promise<ApiResponse<ReconnectStatusData>> {
    return this.command('status.reconnect');
  }

  async healthCheck(): Promise<boolean> {
    const response = await this.command<{ alive: boolean }>('status.health');
    return response.success && response.data?.alive === true;
  }

  async refreshObjectSnapshot(): Promise<ApiResponse<{
    session_id: string;
    generation: number;
    context_generation: number;
    record_count: number;
  }>> {
    return this.command('objects.snapshot.refresh');
  }

  async getObjectCounts(): Promise<ApiResponse<ObjectCountData>> {
    return this.command('objects.count');
  }

  async getObjects(
    cursor: SnapshotQueryCursor | null = null,
    limit = 50,
    search = '',
  ): Promise<ApiResponse<ObjectsResponse>> {
    return this.command('objects.list', {
      cursor,
      limit,
      search: search.trim() || null,
    });
  }

  async searchObjects(
    query: string,
    options: {
      kind?: SnapshotObjectKind;
      classPath?: string;
      packagePath?: string;
      cursor?: SnapshotQueryCursor | null;
      limit?: number;
    } = {},
  ): Promise<ApiResponse<SearchResponse>> {
    return this.command('objects.search', {
      search: query.trim() || null,
      kind: options.kind ?? null,
      class_path: options.classPath?.trim() || null,
      package_path: options.packagePath?.trim() || null,
      cursor: options.cursor ?? null,
      limit: options.limit ?? 50,
    });
  }

  async getObjectByIndex(index: number): Promise<ApiResponse<ObjectDetail>> {
    return this.command('objects.get_by_index', { index });
  }

  async getObjectByAddress(address: string): Promise<ApiResponse<ObjectDetail>> {
    return this.command('objects.get_by_address', { address });
  }

  async getObjectByPath(path: string): Promise<ApiResponse<ObjectDetail>> {
    return this.command('objects.get_by_path', { path });
  }

  async getObjectProperties(index: number): Promise<ApiResponse<ObjectProperty[]>> {
    const detailResponse = await this.getObjectByIndex(index);
    if (!detailResponse.success || !detailResponse.data) {
      return this.failureFrom(detailResponse);
    }
    const detail = detailResponse.data;
    const object: StableObjectHandle | undefined = detail.handle;
    if (!object) {
      return this.localFailure(
        'OBJECT_HANDLE_UNAVAILABLE',
        'The current snapshot record does not contain a stable object handle',
        { index },
      );
    }

    const properties: ObjectProperty[] = [];
    let cursor: TypeQueryCursor | null = null;
    do {
      const fieldsResponse = await this.getClassFields(
        detail.class,
        cursor,
        128,
        'include_inherited',
      );
      if (!fieldsResponse.success || !fieldsResponse.data) {
        return this.failureFrom(fieldsResponse);
      }
      const page = fieldsResponse.data;
      for (const field of page.items) {
        for (let arrayIndex = 0; arrayIndex < field.array_dim; arrayIndex += 1) {
          if (properties.length >= MAX_PROPERTY_ROWS) {
            return this.localFailure(
              'PROPERTY_LIST_LIMIT_EXCEEDED',
              `The instance exposes more than ${MAX_PROPERTY_ROWS} fixed-array property rows`,
              { index, class_path: detail.class },
            );
          }
          properties.push({
            name: field.array_dim > 1 ? `${field.name}[${arrayIndex}]` : field.name,
            property_name: field.name,
            type: field.type_name,
            kind: field.kind,
            offset: field.offset + arrayIndex * field.size,
            size: field.size,
            array_index: arrayIndex,
            array_dim: field.array_dim,
            object,
            object_snapshot_generation: page.object_snapshot_generation,
            type_snapshot_generation: page.type_snapshot_generation,
            declaring_type_path: field.declaring_type.full_path,
            descriptor_available: field.descriptor_available,
            value: null,
            value_state: field.state === 'supported' ? 'not_loaded' : field.state,
          });
        }
      }
      if (page.has_more && !page.next_cursor) {
        return this.localFailure(
          'PROPERTY_METADATA_INVALID',
          'The type field page reports more data without a continuation cursor',
          { index, class_path: detail.class },
        );
      }
      cursor = page.next_cursor;
    } while (cursor);

    return {
      success: true,
      data: properties,
      error: null,
      error_code: null,
      timing: null,
      timestamp: Date.now(),
    };
  }

  async getObjectOuterChain(index: number): Promise<ApiResponse<ObjectOuterChainData>> {
    return this.command('objects.outer_chain', { index });
  }

  async getObjectPropertyValue(
    property: ObjectProperty,
  ): Promise<ApiResponse<ObjectPropertyValueData>> {
    return this.readExactObjectProperty(
      property.object,
      property.type_snapshot_generation,
      property.declaring_type_path,
      property.property_name,
      property.array_index,
    );
  }

  async readExactObjectProperty(
    object: StableObjectHandle,
    typeSnapshotGeneration: number,
    declaringTypePath: string,
    propertyName: string,
    arrayIndex = 0,
  ): Promise<ApiResponse<ObjectPropertyValueData>> {
    return this.command('objects.property.read', {
      object,
      type_snapshot_generation: typeSnapshotGeneration,
      declaring_type_path: declaringTypePath,
      property_name: propertyName,
      array_index: arrayIndex,
    });
  }

  async setObjectProperty(
    index: number,
    property: string,
    value: unknown,
  ): Promise<ApiResponse<{ property: string; written: boolean; new_value: unknown }>> {
    return this.command('objects.property.write', { index, property, value });
  }

  async getPackages(
    cursor: SnapshotQueryCursor | null = null,
    limit = 50,
    search = '',
  ): Promise<ApiResponse<PaginatedResponse<PackageItem>>> {
    return this.command('types.packages.list', {
      cursor,
      limit,
      search: search.trim() || null,
    });
  }

  async getPackageContents(
    packagePath: string,
    cursor: SnapshotQueryCursor | null = null,
    limit = 50,
  ): Promise<ApiResponse<PackageContentsResponse>> {
    return this.command('types.packages.contents', { package_path: packagePath, cursor, limit });
  }

  async getClasses(
    cursor: SnapshotQueryCursor | null = null,
    limit = 50,
    search = '',
  ): Promise<ApiResponse<PaginatedResponse<ClassItem>>> {
    return this.command('types.classes.list', {
      cursor,
      limit,
      search: search.trim() || null,
    });
  }

  async getClassByPath(path: string): Promise<ApiResponse<ClassDetail>> {
    return this.command('types.classes.get', { path });
  }

  async getClassFields(
    path: string,
    cursor: TypeQueryCursor | null = null,
    limit = 128,
    scope: TypeMemberScope = 'include_inherited',
  ): Promise<ApiResponse<TypeMemberPageResponse<ClassProperty>>> {
    return this.command('types.classes.fields', { path, scope, cursor, limit });
  }

  async getClassFunctions(
    path: string,
    cursor: TypeQueryCursor | null = null,
    limit = 128,
    scope: TypeMemberScope = 'include_inherited',
  ): Promise<ApiResponse<TypeMemberPageResponse<ClassFunction>>> {
    return this.command('types.classes.functions', { path, scope, cursor, limit });
  }

  async getFunctionByPath(path: string): Promise<ApiResponse<FunctionDetail>> {
    return this.command('types.functions.get', { path });
  }

  async getClassHierarchy(
    path: string,
    cursor: TypeQueryCursor | null = null,
    limit = 128,
  ): Promise<ApiResponse<ClassHierarchy>> {
    return this.command('types.classes.hierarchy', { path, cursor, limit });
  }

  async getClassInstances(
    classPath: string,
    cursor: SnapshotQueryCursor | null = null,
    limit = 50,
    search = '',
  ): Promise<ApiResponse<ClassInstancesResponse>> {
    return this.command('types.classes.instances', {
      class_path: classPath,
      search: search.trim() || null,
      cursor,
      limit,
    });
  }

  async getClassCDO(path: string): Promise<ApiResponse<ClassCDOResponse>> {
    return this.command('types.classes.cdo', { path });
  }

  async getStructs(
    cursor: SnapshotQueryCursor | null = null,
    limit = 50,
    search = '',
  ): Promise<ApiResponse<PaginatedResponse<StructItem>>> {
    return this.command('types.structs.list', {
      cursor,
      limit,
      search: search.trim() || null,
    });
  }

  async getStructByPath(path: string): Promise<ApiResponse<StructDetail>> {
    return this.command('types.structs.get', { path });
  }

  async getStructFields(
    path: string,
    cursor: TypeQueryCursor | null = null,
    limit = 128,
    scope: TypeMemberScope = 'include_inherited',
  ): Promise<ApiResponse<TypeMemberPageResponse<ClassProperty>>> {
    return this.command('types.structs.fields', { path, scope, cursor, limit });
  }

  async getEnums(
    cursor: SnapshotQueryCursor | null = null,
    limit = 50,
    search = '',
  ): Promise<ApiResponse<PaginatedResponse<EnumItem>>> {
    return this.command('types.enums.list', {
      cursor,
      limit,
      search: search.trim() || null,
    });
  }

  async getEnumByPath(path: string): Promise<ApiResponse<EnumDetail>> {
    return this.command('types.enums.get', { path });
  }

  async getEnumValues(
    path: string,
    cursor: TypeQueryCursor | null = null,
    limit = 128,
  ): Promise<ApiResponse<EnumValuePageResponse>> {
    return this.command('types.enums.values', { path, cursor, limit });
  }

  async getWorld(): Promise<ApiResponse<WorldData>> {
    return this.command('world.inspect');
  }

  async getWorldLevels(
    cursor: WorldQueryCursor | null = null,
    limit = 128,
  ): Promise<ApiResponse<WorldLevelsResponse>> {
    return this.command('world.levels', { cursor, limit });
  }

  async getWorldActors(
    cursor: WorldQueryCursor | null = null,
    limit = 50,
    search = '',
    classSearch = '',
    levelPath = '',
  ): Promise<ApiResponse<WorldActorResponse>> {
    return this.command('world.actors.list', {
      cursor,
      limit,
      search: search.trim() || null,
      class_search: classSearch.trim() || null,
      level_path: levelPath || null,
    });
  }

  async getWorldShortcuts(): Promise<ApiResponse<WorldShortcuts>> {
    return this.command('world.shortcuts');
  }

  async getWorldActorDetail(
    actor: StableObjectHandle,
    worldSnapshotGeneration: number,
  ): Promise<ApiResponse<WorldActorDetail>> {
    return this.command('world.actor.get', {
      actor,
      world_snapshot_generation: worldSnapshotGeneration,
    });
  }

  async getWorldActorComponents(
    actor: StableObjectHandle,
    worldSnapshotGeneration: number,
    cursor: WorldQueryCursor | null = null,
    limit = 128,
  ): Promise<ApiResponse<WorldActorComponentsResponse>> {
    return this.command('world.actor.components', {
      actor,
      world_snapshot_generation: worldSnapshotGeneration,
      cursor,
      limit,
    });
  }

  async getWorldActorTransform(
    actor: StableObjectHandle,
    worldSnapshotGeneration: number,
  ): Promise<ApiResponse<WorldActorTransformResponse>> {
    return this.command('world.actor.transform.get', {
      actor,
      world_snapshot_generation: worldSnapshotGeneration,
    });
  }

  async updateWorldActorTransform(
    scope: WorldActorTransformResponse,
    update: WorldActorTransformUpdate,
  ): Promise<ApiResponse<WorldActorTransformUpdateResponse>> {
    return this.command('world.actor.transform.update', {
      session_id: scope.actor.handle.session_id,
      context_generation: scope.context_generation,
      object_snapshot_generation: scope.object_snapshot_generation,
      type_snapshot_generation: scope.type_snapshot_generation,
      world_snapshot_generation: scope.generation,
      actor: scope.actor.handle,
      update,
    });
  }

  async readMemory(address: string, size: number): Promise<ApiResponse<MemoryReadData>> {
    return this.command('memory.raw.read', { address, size });
  }

  async readTypedMemory(address: string, type: string): Promise<ApiResponse<MemoryTypedData>> {
    return this.command('memory.typed.read', { address, type });
  }

  async writeMemory(
    address: string,
    bytes: number[],
  ): Promise<ApiResponse<{ address: string; bytes_written: number }>> {
    return this.command('memory.raw.write', { address, bytes });
  }

  async writeTypedMemory(
    address: string,
    type: string,
    value: string,
  ): Promise<ApiResponse<{ address: string; type: string; written: boolean }>> {
    return this.command('memory.typed.write', { address, type, value });
  }

  async resolvePointerChain(
    base: string,
    offsets: string[],
  ): Promise<ApiResponse<PointerChainData>> {
    return this.command('memory.pointer_chain.resolve', { base, offsets });
  }

  async invokeFunction(
    target: StableObjectHandle,
    fn: FunctionDetail,
    argumentsByName: Record<string, FunctionCallArgument>,
  ): Promise<ApiResponse<FunctionCallResultData>> {
    return this.command('call.invoke', {
      target,
      function: fn.handle,
      type_snapshot_generation: fn.type_snapshot_generation,
      function_path: fn.full_path,
      arguments: argumentsByName,
    });
  }

  async submitFunctionCallBatch(
    request: FunctionCallBatchSubmitRequest,
  ): Promise<ApiResponse<FunctionCallBatchSubmitResponse>> {
    return this.command('call.batch', { ...request });
  }

  async getFunctionCallBatch(
    request: FunctionCallBatchLookupRequest,
  ): Promise<ApiResponse<FunctionCallBatchGetResponse>> {
    return this.command('call.batch.get', { ...request });
  }

  async cancelFunctionCallBatch(
    request: FunctionCallBatchLookupRequest,
  ): Promise<ApiResponse<FunctionCallBatchCancelResponse>> {
    return this.command('call.batch.cancel', { ...request });
  }

  async listFunctionCallBatches(
    request: FunctionCallBatchListRequest,
  ): Promise<ApiResponse<FunctionCallBatchListResponse>> {
    return this.command('call.batch.list', { ...request });
  }

  async addHook(
    fn: FunctionDetail,
    capture: HookCapturePolicy = { mode: 'fixed_metadata' },
    enabled = false,
  ): Promise<ApiResponse<HookMutationResponse>> {
    return this.command('hook.add', {
      object_snapshot_generation: fn.object_snapshot_generation,
      type_snapshot_generation: fn.type_snapshot_generation,
      function: fn.handle,
      function_path: fn.full_path,
      capture,
      enabled,
    });
  }

  async listHooks(): Promise<ApiResponse<HookListResponse>> {
    return this.command('hook.list');
  }

  async setHookEnabled(
    id: number,
    enabled: boolean,
  ): Promise<ApiResponse<HookMutationResponse>> {
    return this.command('hook.enable', { id, enabled });
  }

  async removeHook(id: number): Promise<ApiResponse<{
    id: number;
    removed: true;
    enabled_snapshot_generation: number;
  }>> {
    return this.command('hook.remove', { id });
  }

  async getHookLog(id: number, limit = 128): Promise<ApiResponse<HookLogResponse>> {
    return this.command('hook.log', { id, limit });
  }

  async decompileBlueprint(
    fn: FunctionDetail,
    profileId: string,
  ): Promise<ApiResponse<BlueprintDecompileData>> {
    return this.command('blueprint.decompile', {
      function: fn.handle,
      function_path: fn.full_path,
      context_generation: fn.context_generation,
      object_snapshot_generation: fn.object_snapshot_generation,
      type_snapshot_generation: fn.type_snapshot_generation,
      profile_id: profileId,
    });
  }

  async getBlueprintBytecode(fn: FunctionDetail): Promise<ApiResponse<BlueprintBytecodeData>> {
    return this.command('blueprint.bytecode', {
      function: fn.handle,
      function_path: fn.full_path,
      context_generation: fn.context_generation,
      object_snapshot_generation: fn.object_snapshot_generation,
      type_snapshot_generation: fn.type_snapshot_generation,
    });
  }

  async addWatch(
    property: ObjectProperty,
    intervalMs = 250,
    enabled = true,
  ): Promise<ApiResponse<{
    id: number;
    state: 'enabled' | 'disabled';
    spec: unknown;
  }>> {
    return this.command('watch.add', {
      object: property.object,
      object_snapshot_generation: property.object_snapshot_generation,
      type_snapshot_generation: property.type_snapshot_generation,
      declaring_type_path: property.declaring_type_path,
      property_name: property.property_name,
      array_index: property.array_index,
      interval_ms: intervalMs,
      enabled,
    });
  }

  async listWatches(): Promise<ApiResponse<WatchListResponse>> {
    return this.command('watch.list');
  }

  async setWatchEnabled(id: number, enabled: boolean): Promise<ApiResponse<{ id: number; enabled: boolean }>> {
    return this.command('watch.enable', { id, enabled });
  }

  async removeWatch(id: number): Promise<ApiResponse<{ id: number; removed: true }>> {
    return this.command('watch.remove', { id });
  }

  async getWatchHistory(id: number, limit = 32): Promise<ApiResponse<WatchHistoryData>> {
    return this.command('watch.snapshot', { id, history_limit: limit });
  }

  async drainWatchEvents(limit = 32): Promise<ApiResponse<WatchDrainData>> {
    return this.command('watch.events.drain', { limit });
  }

  async startScopedDump(request: DumpStartRequest): Promise<ApiResponse<{
    job_id: string;
    format: DumpType;
    output_path_identity: string;
    admission: 'accepted';
  }>> {
    const operations: Record<DumpType, string> = {
      sdk: 'dump.sdk.start',
      usmap: 'dump.usmap.start',
      dumpspace: 'dump.dumpspace.start',
      'ida-script': 'dump.ida.start',
    };
    return this.command(operations[request.format], { ...request });
  }

  async listScopedDumpJobs(
    scope: DumpScope,
    maxJobs = 64,
  ): Promise<ApiResponse<DumpJobListResponse>> {
    return this.command('dump.jobs.list', { ...scope, max_jobs: maxJobs });
  }

  async getScopedDumpJob(
    scope: DumpScope,
    jobId: string,
    afterEventSequence = 0,
    maxEvents = 128,
  ): Promise<ApiResponse<DumpJobSnapshotResponse>> {
    return this.command('dump.jobs.get', {
      ...scope,
      job_id: jobId,
      after_event_sequence: afterEventSequence,
      max_events: maxEvents,
    });
  }

  async cancelScopedDumpJob(
    scope: DumpScope,
    jobId: string,
  ): Promise<ApiResponse<{
    job_id: string;
    disposition: 'cancelled_before_start' | 'cancellation_requested';
  }>> {
    return this.command('dump.jobs.cancel', { ...scope, job_id: jobId });
  }

  async scanUEProcesses(): Promise<HostProcessInfo[]> {
    return tauriTransport.scanUEProcesses();
  }

  async injectDLL(process: HostProcessInfo, dllPath: string): Promise<InjectionCommandResult> {
    return tauriTransport.injectDLL(process, dllPath);
  }

  async subscribeSessionEvents(
    pid: number,
    options: SessionEventSubscribeOptions,
  ): Promise<SessionEventSubscription> {
    return tauriTransport.subscribeSessionEvents(pid, options);
  }

  async getEventBridgeDiagnostics(): Promise<EventBridgeDiagnostics[]> {
    return tauriTransport.getEventBridgeDiagnostics();
  }

  async disconnectSession(pid: number, reason: string): Promise<boolean> {
    return tauriTransport.disconnectSession(pid, reason);
  }
}

export const api = new UExplorerApi();
export default api;
