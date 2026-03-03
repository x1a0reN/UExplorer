const DEFAULT_PORT = 27015;
const DEFAULT_TOKEN = 'uexplorer-dev';
const SETTINGS_KEY = 'uexplorer.settings';

export interface ApiResponse<T> {
  success: boolean;
  data: T | null;
  error: string | null;
  timestamp?: number;
}

export interface ApiClientSettings {
  port: number;
  token: string;
  dllPath: string;
  autoReconnect: boolean;
  injectionMethod: 'CreateRemoteThread' | 'DLL Proxy (xinput1_3)';
  defaultDumpFormat: DumpType;
  outputDir: string;
}

export interface StatusData {
  game_name: string;
  game_version: string;
  gobjects_address: string;
  object_count: number;
  pid: number;
  architecture: string;
}

export interface ObjectItem {
  index: number;
  name: string;
  class: string;
  address: string;
}

export interface ObjectDetail extends ObjectItem {
  full_name: string;
  outer_chain: string[];
}

export interface ObjectProperty {
  name: string;
  type: string;
  offset: number;
  size: number;
  value: unknown;
}

export interface ObjectsResponse {
  items: ObjectItem[];
  total: number;
  offset: number;
  limit: number;
}

export interface SearchResponse {
  items: ObjectItem[];
  matched: number;
  offset: number;
  limit: number;
}

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
  size: number;
  super: string;
  address: string;
}

export interface ClassProperty {
  name: string;
  type: string;
  offset: number;
  size: number;
  array_dim: number;
  flags: string;
}

export interface ClassFunction {
  name: string;
  full_name: string;
  flags: string;
  param_size: number;
  has_script: boolean;
  address: string;
  params: ClassProperty[];
}

export interface ClassDetail {
  name: string;
  full_name: string;
  cpp_name: string;
  size: number;
  alignment: number;
  index: number;
  address: string;
  super: string;
  fields: ClassProperty[];
  functions: ClassFunction[];
}

export interface PaginatedResponse<T> {
  items: T[];
  total: number;
  offset: number;
  limit: number;
}

export interface StructItem {
  index: number;
  name: string;
  size: number;
  super: string;
}

export interface StructDetail {
  name: string;
  full_name: string;
  size: number;
  alignment: number;
  super: string;
  fields: ClassProperty[];
}

export interface EnumItem {
  index: number;
  name: string;
  full_name: string;
}

export interface EnumDetail {
  name: string;
  full_name: string;
  underlying_type: string;
  values: Array<{ name: string; value: number }>;
}

export interface PackageItem {
  index: number;
  name: string;
  address: string;
}

export interface PackageContentsResponse {
  package: string;
  items: ObjectItem[];
  count: number;
}

export interface ClassHierarchy {
  name: string;
  parents: string[];
  children: string[];
}

export interface ClassInstancesResponse {
  class: string;
  items: Array<{ index: number; name: string; address: string }>;
  matched: number;
  offset: number;
  limit: number;
}

export interface ClassCDOResponse {
  class: string;
  cdo_address: string;
  properties: ObjectProperty[];
}

export interface WorldData {
  name: string;
  address: string;
  actor_count: number;
}

export interface WorldActorItem extends ObjectItem {}

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

export interface RuntimeEndpoint {
  pid: number;
  port: number;
  token: string;
  running: boolean;
}

const DEFAULT_SETTINGS: ApiClientSettings = {
  port: DEFAULT_PORT,
  token: DEFAULT_TOKEN,
  dllPath: 'D:\\Projects\\UExplorer\\Dumper\\x64\\Release\\UExplorerCore.dll',
  autoReconnect: true,
  injectionMethod: 'CreateRemoteThread',
  defaultDumpFormat: 'sdk',
  outputDir: '',
};

function readSettings(): ApiClientSettings {
  try {
    const raw = localStorage.getItem(SETTINGS_KEY);
    if (!raw) return DEFAULT_SETTINGS;
    const parsed = JSON.parse(raw) as Partial<ApiClientSettings>;
    return { ...DEFAULT_SETTINGS, ...parsed };
  } catch {
    return DEFAULT_SETTINGS;
  }
}

function toQuery(params: Record<string, string | number | undefined>): string {
  const qs = new URLSearchParams();
  Object.entries(params).forEach(([k, v]) => {
    if (v === undefined || v === '') return;
    qs.append(k, String(v));
  });
  return qs.toString();
}

type SSECallback = (event: string, payload: unknown) => void;
type SSECloseReason = 'eof' | 'error' | 'abort';

interface SSESubscribeOptions {
  onOpen?: () => void;
  onError?: (error: unknown) => void;
  onClose?: (reason: SSECloseReason) => void;
}

class UExplorerApi {
  private baseUrl: string;
  private token: string;
  private settings: ApiClientSettings;
  private endpointRecovering = false;

  constructor() {
    this.settings = readSettings();
    this.baseUrl = `http://127.0.0.1:${this.settings.port}/api/v1`;
    this.token = this.settings.token;
    void this.persistConnectionSettings();
    void this.tryAdoptRuntimeEndpoint();
  }

  getSettings() {
    return this.settings;
  }

  updateSettings(next: Partial<ApiClientSettings>) {
    this.settings = { ...this.settings, ...next };
    this.baseUrl = `http://127.0.0.1:${this.settings.port}/api/v1`;
    this.token = this.settings.token;
    localStorage.setItem(SETTINGS_KEY, JSON.stringify(this.settings));
    void this.persistConnectionSettings();
  }

  async persistConnectionSettings(): Promise<boolean> {
    const { invoke } = await import('@tauri-apps/api/core');
    try {
      return await invoke<boolean>('save_connection_settings', {
        port: this.settings.port,
        token: this.settings.token,
      });
    } catch (error) {
      console.error('Failed to persist connection settings:', error);
      return false;
    }
  }

  private applyEndpoint(port: number, token: string) {
    if (!Number.isFinite(port) || port <= 0 || port > 65535) return;
    if (!token) return;

    this.settings = { ...this.settings, port, token };
    this.baseUrl = `http://127.0.0.1:${port}/api/v1`;
    this.token = token;
    localStorage.setItem(SETTINGS_KEY, JSON.stringify(this.settings));
  }

  async tryAdoptRuntimeEndpoint(expectedPid?: number): Promise<boolean> {
    if (this.endpointRecovering) return false;
    this.endpointRecovering = true;
    try {
      const { invoke } = await import('@tauri-apps/api/core');
      const runtime = await invoke<RuntimeEndpoint | null>('load_runtime_endpoint');
      if (!runtime || !runtime.running) return false;
      if (expectedPid !== undefined && runtime.pid > 0 && runtime.pid !== expectedPid) return false;
      this.applyEndpoint(runtime.port, runtime.token);
      return true;
    } catch (error) {
      console.error('Failed to load runtime endpoint:', error);
      return false;
    } finally {
      this.endpointRecovering = false;
    }
  }

  private shouldAttemptEndpointRecovery(error: string | null) {
    if (!error) return false;
    const msg = error.toLowerCase();
    return (
      msg.includes('network error') ||
      msg.includes('failed to fetch') ||
      msg.includes('http 401') ||
      msg.includes('unauthorized')
    );
  }

  private async request<T>(endpoint: string, options: RequestInit = {}): Promise<ApiResponse<T>> {
    const execute = async (): Promise<ApiResponse<T>> => {
      const url = `${this.baseUrl}${endpoint}`;
      const headers: HeadersInit = {
        'X-UExplorer-Token': this.token,
        ...options.headers,
      };

      if (options.body && !(headers as Record<string, string>)['Content-Type']) {
        (headers as Record<string, string>)['Content-Type'] = 'application/json';
      }

      try {
        const response = await fetch(url, {
          ...options,
          headers,
        });
        const text = await response.text();
        if (!text) {
          return {
            success: response.ok,
            data: null,
            error: response.ok ? null : `HTTP ${response.status}`,
          };
        }

        const parsed = JSON.parse(text) as ApiResponse<T>;
        if (!response.ok && parsed.success !== false) {
          return {
            success: false,
            data: null,
            error: parsed.error || `HTTP ${response.status}`,
            timestamp: parsed.timestamp,
          };
        }
        return parsed;
      } catch (error) {
        return {
          success: false,
          data: null,
          error: error instanceof Error ? error.message : 'Network error',
          timestamp: Date.now(),
        };
      }
    };

    let result = await execute();
    if (!result.success && this.shouldAttemptEndpointRecovery(result.error)) {
      const recovered = await this.tryAdoptRuntimeEndpoint();
      if (recovered) {
        result = await execute();
      }
    }
    return result;
  }

  // Status
  async getStatus(): Promise<ApiResponse<StatusData>> {
    return this.request('/status');
  }

  async healthCheck(): Promise<boolean> {
    const response = await this.request<{ alive: boolean }>('/status/health');
    return response.success && !!response.data?.alive;
  }

  // Objects
  async getObjectCounts(): Promise<ApiResponse<ObjectCountData>> {
    return this.request('/objects/count');
  }

  async getObjects(offset = 0, limit = 50, q = ''): Promise<ApiResponse<ObjectsResponse>> {
    const qs = toQuery({ offset, limit, q });
    return this.request(`/objects?${qs}`);
  }

  async searchObjects(
    query: string,
    options: { class?: string; package?: string; offset?: number; limit?: number } = {}
  ): Promise<ApiResponse<SearchResponse>> {
    const qs = toQuery({
      q: query,
      class: options.class,
      package: options.package,
      offset: options.offset ?? 0,
      limit: options.limit ?? 50,
    });
    return this.request(`/objects/search?${qs}`);
  }

  async getObjectByIndex(index: number): Promise<ApiResponse<ObjectDetail>> {
    return this.request(`/objects/${index}`);
  }

  async getObjectByAddress(address: string): Promise<ApiResponse<ObjectDetail>> {
    return this.request(`/objects/by-address/${encodeURIComponent(address)}`);
  }

  async getObjectByPath(path: string): Promise<ApiResponse<ObjectDetail>> {
    return this.request(`/objects/by-path/${encodeURIComponent(path)}`);
  }

  async getObjectProperties(index: number): Promise<ApiResponse<ObjectProperty[]>> {
    return this.request(`/objects/${index}/properties`);
  }

  async setObjectProperty(index: number, property: string, value: unknown): Promise<ApiResponse<{ property: string; written: boolean; new_value: unknown }>> {
    return this.request(`/objects/${index}/property/${encodeURIComponent(property)}`, {
      method: 'POST',
      body: JSON.stringify({ value }),
    });
  }

  async getPackages(offset = 0, limit = 50, q = ''): Promise<ApiResponse<PaginatedResponse<PackageItem>>> {
    const qs = toQuery({ offset, limit, q });
    return this.request(`/packages?${qs}`);
  }

  async getPackageContents(packageName: string): Promise<ApiResponse<PackageContentsResponse>> {
    return this.request(`/packages/${encodeURIComponent(packageName)}/contents`);
  }

  // Classes / Structs / Enums
  async getClasses(offset = 0, limit = 50, q = ''): Promise<ApiResponse<PaginatedResponse<ClassItem>>> {
    const qs = toQuery({ offset, limit, q });
    return this.request(`/classes?${qs}`);
  }

  async getClassByName(name: string): Promise<ApiResponse<ClassDetail>> {
    return this.request(`/classes/${encodeURIComponent(name)}`);
  }

  async getClassFields(name: string): Promise<ApiResponse<ClassProperty[]>> {
    return this.request(`/classes/${encodeURIComponent(name)}/fields`);
  }

  async getClassFunctions(name: string): Promise<ApiResponse<ClassFunction[]>> {
    return this.request(`/classes/${encodeURIComponent(name)}/functions`);
  }

  async getClassHierarchy(name: string): Promise<ApiResponse<ClassHierarchy>> {
    return this.request(`/classes/${encodeURIComponent(name)}/hierarchy`);
  }

  async getClassInstances(name: string, offset = 0, limit = 50): Promise<ApiResponse<ClassInstancesResponse>> {
    const qs = toQuery({ offset, limit });
    return this.request(`/classes/${encodeURIComponent(name)}/instances?${qs}`);
  }

  async getClassCDO(name: string): Promise<ApiResponse<ClassCDOResponse>> {
    return this.request(`/classes/${encodeURIComponent(name)}/cdo`);
  }

  async getStructs(offset = 0, limit = 50, q = ''): Promise<ApiResponse<PaginatedResponse<StructItem>>> {
    const qs = toQuery({ offset, limit, q });
    return this.request(`/structs?${qs}`);
  }

  async getStructByName(name: string): Promise<ApiResponse<StructDetail>> {
    return this.request(`/structs/${encodeURIComponent(name)}`);
  }

  async getEnums(offset = 0, limit = 50, q = ''): Promise<ApiResponse<PaginatedResponse<EnumItem>>> {
    const qs = toQuery({ offset, limit, q });
    return this.request(`/enums?${qs}`);
  }

  async getEnumByName(name: string): Promise<ApiResponse<EnumDetail>> {
    return this.request(`/enums/${encodeURIComponent(name)}`);
  }

  // World
  async getWorld(): Promise<ApiResponse<WorldData>> {
    return this.request('/world');
  }

  async getWorldActors(
    offset = 0,
    limit = 50,
    q = '',
    classFilter = ''
  ): Promise<ApiResponse<WorldActorResponse>> {
    const qs = toQuery({ offset, limit, q, class: classFilter });
    return this.request(`/world/actors?${qs}`);
  }

  async getWorldShortcuts(): Promise<ApiResponse<WorldShortcuts>> {
    return this.request('/world/shortcuts');
  }

  async getWorldActorDetail(index: number): Promise<ApiResponse<WorldActorDetail>> {
    return this.request(`/world/actors/${index}`);
  }

  async updateWorldActorTransform(
    index: number,
    transform: {
      location?: Vec3Data | [number, number, number];
      rotation?: Vec3Data | [number, number, number];
      scale?: Vec3Data | [number, number, number];
    }
  ): Promise<ApiResponse<WorldActorTransformUpdateResponse>> {
    return this.request(`/world/actors/${index}/transform`, {
      method: 'POST',
      body: JSON.stringify(transform),
    });
  }

  // Memory
  async readMemory(address: string, size: number): Promise<ApiResponse<MemoryReadData>> {
    return this.request('/memory/read', {
      method: 'POST',
      body: JSON.stringify({ address, size }),
    });
  }

  async readTypedMemory(address: string, type: string): Promise<ApiResponse<MemoryTypedData>> {
    return this.request('/memory/read-typed', {
      method: 'POST',
      body: JSON.stringify({ address, type }),
    });
  }

  async writeMemory(address: string, bytes: number[]): Promise<ApiResponse<{ address: string; bytes_written: number }>> {
    return this.request('/memory/write', {
      method: 'POST',
      body: JSON.stringify({ address, bytes }),
    });
  }

  async writeTypedMemory(address: string, type: string, value: unknown): Promise<ApiResponse<{ address: string; type: string; written: boolean }>> {
    return this.request('/memory/write-typed', {
      method: 'POST',
      body: JSON.stringify({ address, type, value }),
    });
  }

  async resolvePointerChain(base: string, offsets: number[]): Promise<ApiResponse<PointerChainData>> {
    return this.request('/memory/pointer-chain', {
      method: 'POST',
      body: JSON.stringify({ base, offsets }),
    });
  }

  // Function call / hook / blueprint
  async callFunction(
    objectIndex: number,
    functionName: string,
    params: Record<string, unknown> = {},
    useGameThread = true
  ): Promise<ApiResponse<FunctionCallResultData>> {
    return this.request('/call/function', {
      method: 'POST',
      body: JSON.stringify({
        object_index: objectIndex,
        function_name: functionName,
        params,
        use_game_thread: useGameThread,
      }),
    });
  }

  async callStaticFunction(
    functionName: string,
    options: {
      className?: string;
      classIndex?: number;
      objectIndex?: number;
      params?: Record<string, unknown>;
      useGameThread?: boolean;
    }
  ): Promise<ApiResponse<FunctionCallResultData>> {
    const body: Record<string, unknown> = {
      function_name: functionName,
      params: options.params ?? {},
      use_game_thread: options.useGameThread ?? true,
    };
    if (options.className) body.class_name = options.className;
    if (options.classIndex !== undefined) body.class_index = options.classIndex;
    if (options.objectIndex !== undefined) body.object_index = options.objectIndex;

    return this.request('/call/static', {
      method: 'POST',
      body: JSON.stringify(body),
    });
  }

  async callFunctionBatch(
    objectIndices: number[],
    functionName: string,
    params: Record<string, unknown> = {},
    useGameThread = true
  ): Promise<ApiResponse<BatchFunctionCallResultData>> {
    return this.request('/call/batch', {
      method: 'POST',
      body: JSON.stringify({
        object_indices: objectIndices,
        function_name: functionName,
        params,
        use_game_thread: useGameThread,
      }),
    });
  }

  async addHook(functionPath: string): Promise<ApiResponse<{ id: number; function_path: string; enabled: boolean }>> {
    return this.request('/hooks/add', {
      method: 'POST',
      body: JSON.stringify({ function_path: functionPath }),
    });
  }

  async listHooks(): Promise<ApiResponse<HookListResponse>> {
    return this.request('/hooks/list');
  }

  async setHookEnabled(id: number, enabled: boolean): Promise<ApiResponse<{ id: number; enabled: boolean }>> {
    return this.request(`/hooks/${id}?id=${id}`, {
      method: 'PATCH',
      body: JSON.stringify({ enabled }),
    });
  }

  async removeHook(id: number): Promise<ApiResponse<{ removed: boolean }>> {
    return this.request(`/hooks/${id}?id=${id}`, { method: 'DELETE' });
  }

  async getHookLog(id: number): Promise<ApiResponse<HookLogResponse>> {
    return this.request(`/hooks/${id}/log?id=${id}`);
  }

  async decompileBlueprint(index: number): Promise<ApiResponse<BlueprintDecompileData>> {
    return this.request(`/blueprint/decompile?index=${index}`);
  }

  async getBlueprintBytecode(index: number): Promise<ApiResponse<BlueprintBytecodeData>> {
    return this.request(`/blueprint/bytecode?index=${index}`);
  }

  // Watch
  async addWatch(objectIndex: number, property: string): Promise<ApiResponse<{ id: number; object_index: number; property: string; current_value: unknown }>> {
    return this.request('/watch/add', {
      method: 'POST',
      body: JSON.stringify({ object_index: objectIndex, property }),
    });
  }

  async listWatches(): Promise<ApiResponse<WatchListResponse>> {
    return this.request('/watch/list');
  }

  async removeWatch(id: number): Promise<ApiResponse<{ removed: number }>> {
    return this.request(`/watch/${id}`, { method: 'DELETE' });
  }

  // Dump
  async startDump(type: DumpType, options: Record<string, unknown> = {}): Promise<ApiResponse<{ job_id: string; message: string }>> {
    return this.request(`/dump/${type}`, {
      method: 'POST',
      body: JSON.stringify(options),
    });
  }

  async getDumpJobs(): Promise<ApiResponse<DumpJob[]>> {
    return this.request('/dump/jobs');
  }

  async getDumpJob(id: string): Promise<ApiResponse<DumpJob>> {
    return this.request(`/dump/jobs/${id}`);
  }

  // SSE by fetch stream, because native EventSource cannot attach auth header.
  subscribeEventStream(
    path: '/events/stream' | '/events/hooks' | '/events/watches',
    onMessage: SSECallback,
    optionsOrOnError?: SSESubscribeOptions | ((error: unknown) => void)
  ) {
    const options: SSESubscribeOptions =
      typeof optionsOrOnError === 'function'
        ? { onError: optionsOrOnError }
        : optionsOrOnError ?? {};
    const controller = new AbortController();
    let closed = false;

    const emitClose = (reason: SSECloseReason) => {
      if (closed) return;
      closed = true;
      options.onClose?.(reason);
    };

    const run = async () => {
      try {
        const res = await fetch(`${this.baseUrl}${path}`, {
          method: 'GET',
          headers: {
            'X-UExplorer-Token': this.token,
            Accept: 'text/event-stream',
          },
          signal: controller.signal,
        });

        if (!res.ok || !res.body) {
          throw new Error(`SSE failed: HTTP ${res.status}`);
        }

        options.onOpen?.();
        const reader = res.body.getReader();
        const decoder = new TextDecoder();
        let buffer = '';
        let event = 'message';
        let data = '';

        while (true) {
          const { done, value } = await reader.read();
          if (done) break;
          buffer += decoder.decode(value, { stream: true });

          const lines = buffer.split('\n');
          buffer = lines.pop() ?? '';

          for (const rawLine of lines) {
            const line = rawLine.trimEnd();
            if (line.startsWith(':')) {
              continue;
            }
            if (!line) {
              if (data) {
                let parsed: unknown = data;
                try {
                  parsed = JSON.parse(data);
                } catch {
                  // keep raw text
                }
                onMessage(event, parsed);
              }
              event = 'message';
              data = '';
              continue;
            }
            if (line.startsWith('event:')) {
              event = line.slice(6).trim();
            } else if (line.startsWith('data:')) {
              data = data ? `${data}\n${line.slice(5).trim()}` : line.slice(5).trim();
            }
          }
        }

        if (!controller.signal.aborted) {
          emitClose('eof');
        }
      } catch (error) {
        if (!controller.signal.aborted) {
          options.onError?.(error);
          emitClose('error');
        }
      }
    };

    run();
    return () => {
      if (!controller.signal.aborted) {
        controller.abort();
      }
      emitClose('abort');
    };
  }

  // Process management (Tauri invoke)
  async scanUEProcesses(): Promise<{ pid: number; name: string; path: string }[]> {
    const { invoke } = await import('@tauri-apps/api/core');
    try {
      return await invoke<{ pid: number; name: string; path: string }[]>('scan_ue_processes');
    } catch (error) {
      console.error('Failed to scan UE processes:', error);
      return [];
    }
  }

  async injectDLL(pid: number, dllPath: string): Promise<{ success: boolean; message: string }> {
    const { invoke } = await import('@tauri-apps/api/core');
    try {
      await this.persistConnectionSettings();
      const result = await invoke<{ success: boolean; message: string }>('inject_dll', { pid, dllPath });
      if (result.success) {
        // Wait for DLL runtime state publication and adopt actual endpoint.
        for (let i = 0; i < 50; i++) {
          const adopted = await this.tryAdoptRuntimeEndpoint(pid);
          if (adopted) break;
          await new Promise((resolve) => setTimeout(resolve, 100));
        }
      }
      return result;
    } catch (error) {
      console.error('Failed to inject DLL:', error);
      return { success: false, message: String(error) };
    }
  }
}

export const api = new UExplorerApi();
export default api;
