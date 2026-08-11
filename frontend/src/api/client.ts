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
  DumpJob,
  DumpType,
  EngineStatusData,
  EnumDetail,
  EnumValuePageResponse,
  EnumItem,
  EventBridgeDiagnostics,
  FunctionCallResultData,
  FunctionCallArgument,
  FunctionDetail,
  HookListResponse,
  HookLogResponse,
  HostProcessInfo,
  HostSessionEvent,
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
  Vec3Data,
  WatchHistoryData,
  WatchListResponse,
  WorldActorComponentsResponse,
  WorldActorDetail,
  WorldActorResponse,
  WorldActorTransformResponse,
  WorldActorTransformUpdateResponse,
  WorldData,
  WorldLevelsResponse,
  WorldQueryCursor,
  WorldShortcuts,
} from './index';

const SETTINGS_KEY = 'uexplorer.settings';
const MAX_PROPERTY_ROWS = 8_192;

const DEFAULT_SETTINGS: ApiClientSettings = {
  dllPath: 'D:\\Projects\\UExplorer\\Dumper\\x64\\Release\\UExplorerCore.dll',
  defaultDumpFormat: 'sdk',
  outputDir: 'D:\\UExplorer_Output',
};

function isDumpType(value: unknown): value is DumpType {
  return value === 'sdk' || value === 'usmap' || value === 'dumpspace' || value === 'ida-script';
}

function loadSettings(): ApiClientSettings {
  if (typeof localStorage === 'undefined') return { ...DEFAULT_SETTINGS };
  try {
    const value = JSON.parse(localStorage.getItem(SETTINGS_KEY) ?? '{}') as Record<string, unknown>;
    return {
      dllPath: typeof value.dllPath === 'string' ? value.dllPath : DEFAULT_SETTINGS.dllPath,
      defaultDumpFormat: isDumpType(value.defaultDumpFormat)
        ? value.defaultDumpFormat
        : DEFAULT_SETTINGS.defaultDumpFormat,
      outputDir: typeof value.outputDir === 'string' ? value.outputDir : DEFAULT_SETTINGS.outputDir,
    };
  } catch {
    return { ...DEFAULT_SETTINGS };
  }
}

class UExplorerApi {
  private settings: ApiClientSettings = loadSettings();

  getSettings(): ApiClientSettings {
    return { ...this.settings };
  }

  updateSettings(next: Partial<ApiClientSettings>): void {
    this.settings = { ...this.settings, ...next };
    if (typeof localStorage !== 'undefined') {
      localStorage.setItem(SETTINGS_KEY, JSON.stringify(this.settings));
    }
  }

  private async command<T>(
    operation: string,
    data: Record<string, unknown> = {},
    timeoutMs = 5_000,
    targetPid: number | null = null,
  ): Promise<ApiResponse<T>> {
    try {
      const { invoke } = await import('@tauri-apps/api/core');
      return await invoke<ApiResponse<T>>('domain_request', {
        request: { targetPid, operation, timeoutMs, data },
      });
    } catch (error) {
      const message = error instanceof Error ? error.message : String(error);
      return {
        success: false,
        data: null,
        error: `HOST_INVOKE_FAILED: ${message}`,
        error_code: 'HOST_INVOKE_FAILED',
        timestamp: Date.now(),
      };
    }
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
    index: number,
    transform: {
      location?: Vec3Data | [number, number, number];
      rotation?: Vec3Data | [number, number, number];
      scale?: Vec3Data | [number, number, number];
    },
  ): Promise<ApiResponse<WorldActorTransformUpdateResponse>> {
    return this.command('world.actor.transform.update', { index, ...transform });
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
    value: unknown,
  ): Promise<ApiResponse<{ address: string; type: string; written: boolean }>> {
    return this.command('memory.typed.write', { address, type, value });
  }

  async resolvePointerChain(
    base: string,
    offsets: number[],
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

  async addHook(
    functionPath: string,
  ): Promise<ApiResponse<{ id: number; function_path: string; enabled: boolean }>> {
    return this.command('hook.add', { function_path: functionPath });
  }

  async listHooks(): Promise<ApiResponse<HookListResponse>> {
    return this.command('hook.list');
  }

  async setHookEnabled(
    id: number,
    enabled: boolean,
  ): Promise<ApiResponse<{ id: number; enabled: boolean }>> {
    return this.command('hook.enable', { id, enabled });
  }

  async removeHook(id: number): Promise<ApiResponse<{ removed: boolean }>> {
    return this.command('hook.remove', { id });
  }

  async getHookLog(id: number): Promise<ApiResponse<HookLogResponse>> {
    return this.command('hook.log', { id });
  }

  async decompileBlueprint(index: number): Promise<ApiResponse<BlueprintDecompileData>> {
    return this.command('blueprint.decompile', { index });
  }

  async getBlueprintBytecode(index: number): Promise<ApiResponse<BlueprintBytecodeData>> {
    return this.command('blueprint.bytecode', { index });
  }

  async decompileBlueprintByPath(
    functionPath: string,
  ): Promise<ApiResponse<BlueprintDecompileData>> {
    return this.command('blueprint.decompile', { function_path: functionPath });
  }

  async getBlueprintBytecodeByPath(
    functionPath: string,
  ): Promise<ApiResponse<BlueprintBytecodeData>> {
    return this.command('blueprint.bytecode', { function_path: functionPath });
  }

  async addWatch(
    objectIndex: number,
    property: string,
  ): Promise<ApiResponse<{
    id: number;
    object_index: number;
    property: string;
    current_value: unknown;
  }>> {
    return this.command('watch.add', { object_index: objectIndex, property });
  }

  async listWatches(): Promise<ApiResponse<WatchListResponse>> {
    return this.command('watch.list');
  }

  async removeWatch(id: number): Promise<ApiResponse<{ removed: number }>> {
    return this.command('watch.remove', { id });
  }

  async getWatchHistory(id: number, limit = 200): Promise<ApiResponse<WatchHistoryData>> {
    return this.command('watch.history', { id, limit });
  }

  async startDump(type: DumpType): Promise<ApiResponse<{ job_id: string; message: string }>> {
    const operations: Record<DumpType, string> = {
      sdk: 'dump.sdk.start',
      usmap: 'dump.usmap.start',
      dumpspace: 'dump.dumpspace.start',
      'ida-script': 'dump.ida.start',
    };
    return this.command(operations[type], { type });
  }

  async getDumpJobs(): Promise<ApiResponse<DumpJob[]>> {
    return this.command('dump.jobs.list');
  }

  async getDumpJob(id: string): Promise<ApiResponse<DumpJob>> {
    return this.command('dump.jobs.get', { id });
  }

  async scanUEProcesses(): Promise<HostProcessInfo[]> {
    const { invoke } = await import('@tauri-apps/api/core');
    return invoke<HostProcessInfo[]>('scan_ue_processes');
  }

  async injectDLL(process: HostProcessInfo, dllPath: string): Promise<InjectionCommandResult> {
    const { invoke } = await import('@tauri-apps/api/core');
    return invoke<InjectionCommandResult>('inject_and_connect', {
      pid: process.pid,
      dllPath,
      expectedStartTime100ns: process.start_time_100ns,
      expectedProcessPath: process.path,
    });
  }

  async subscribeSessionEvents(
    pid: number,
    options: SessionEventSubscribeOptions,
  ): Promise<SessionEventSubscription> {
    const { Channel, invoke } = await import('@tauri-apps/api/core');
    const channel = new Channel<HostSessionEvent>();
    channel.onmessage = options.onEvent;
    const diagnostics = await invoke<EventBridgeDiagnostics>('subscribe_session_events', {
      pid,
      filter: options.filter ?? {},
      replayAfterSeq: options.replayAfterSeq ?? null,
      capacity: options.capacity ?? 256,
      onEvent: channel,
    });
    let subscribed = true;

    return {
      diagnostics,
      unsubscribe: async () => {
        if (!subscribed) return true;
        const removed = await invoke<boolean>('unsubscribe_session_events', {
          bridgeId: diagnostics.bridge_id,
        });
        if (removed) {
          subscribed = false;
          channel.onmessage = () => undefined;
        }
        return removed;
      },
    };
  }

  async getEventBridgeDiagnostics(): Promise<EventBridgeDiagnostics[]> {
    const { invoke } = await import('@tauri-apps/api/core');
    return invoke<EventBridgeDiagnostics[]>('event_bridge_diagnostics');
  }

  async disconnectSession(pid: number, reason: string): Promise<boolean> {
    const { invoke } = await import('@tauri-apps/api/core');
    return invoke<boolean>('disconnect_session', { pid, reason });
  }
}

export const api = new UExplorerApi();
export default api;
