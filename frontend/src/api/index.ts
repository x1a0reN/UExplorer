// API client for UExplorer Core DLL

const DEFAULT_PORT = 27015;
const DEFAULT_TOKEN = 'uexplorer-dev';

export interface ApiResponse<T> {
  success: boolean;
  data: T | null;
  error: string | null;
  timestamp: number;
}

export interface StatusData {
  game_name: string;
  game_version: string;
  gobjects_address: string;
  object_count: number;
  pid: number;
  architecture: string;
}

export interface WorldData {
  name: string;
  address: string;
  actor_count: number;
}

export interface ObjectItem {
  index: number;
  name: string;
  class: string;
  address: string;
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

export interface ClassItem {
  name: string;
  parent?: string;
  package?: string;
  size?: number;
  properties?: number;
  functions?: number;
}

export interface ClassesResponse {
  items: ClassItem[];
  total: number;
  offset: number;
  limit: number;
}

export interface PackageItem {
  index: number;
  name: string;
  address: string;
}

export interface PackagesResponse {
  items: PackageItem[];
  total: number;
  offset: number;
  limit: number;
}

export interface DumpJob {
  id: string;
  type: string;
  status: 'pending' | 'running' | 'completed' | 'failed';
  progress: number;
  message?: string;
  started_at?: number;
  completed_at?: number;
}

class UExplorerApi {
  private baseUrl: string;
  private token: string;

  constructor(port: number = DEFAULT_PORT, token: string = DEFAULT_TOKEN) {
    this.baseUrl = `http://127.0.0.1:${port}/api/v1`;
    this.token = token;
  }

  setPort(port: number) {
    this.baseUrl = `http://127.0.0.1:${port}/api/v1`;
  }

  setToken(token: string) {
    this.token = token;
  }

  private async request<T>(endpoint: string, options: RequestInit = {}): Promise<ApiResponse<T>> {
    const url = `${this.baseUrl}${endpoint}`;
    const headers: HeadersInit = {
      'Content-Type': 'application/json',
      'X-UExplorer-Token': this.token,
      ...options.headers,
    };

    try {
      const response = await fetch(url, {
        ...options,
        headers,
      });

      const data = await response.json();
      return data as ApiResponse<T>;
    } catch (error) {
      return {
        success: false,
        data: null,
        error: error instanceof Error ? error.message : 'Network error',
        timestamp: Date.now(),
      };
    }
  }

  // Status endpoints
  async getStatus(): Promise<ApiResponse<StatusData>> {
    return this.request<StatusData>('/status');
  }

  async healthCheck(): Promise<boolean> {
    const response = await this.request<{ status: string }>('/status/health');
    return response.success && response.data?.status === 'ok';
  }

  // World endpoints
  async getWorld(): Promise<ApiResponse<WorldData>> {
    return this.request<WorldData>('/world');
  }

  // Objects endpoints
  async getObjects(offset: number = 0, limit: number = 50): Promise<ApiResponse<ObjectsResponse>> {
    return this.request<ObjectsResponse>(`/objects?offset=${offset}&limit=${limit}`);
  }

  async searchObjects(
    query: string,
    options: { class?: string; package?: string; offset?: number; limit?: number } = {}
  ): Promise<ApiResponse<SearchResponse>> {
    const params = new URLSearchParams({ q: query });
    if (options.class) params.append('class', options.class);
    if (options.package) params.append('package', options.package);
    if (options.offset) params.append('offset', options.offset.toString());
    if (options.limit) params.append('limit', options.limit.toString());
    return this.request<SearchResponse>(`/objects/search?${params}`);
  }

  async getObjectByIndex(index: number): Promise<ApiResponse<ObjectItem>> {
    return this.request<ObjectItem>(`/objects/${index}`);
  }

  async getObjectByAddress(address: string): Promise<ApiResponse<ObjectItem>> {
    return this.request<ObjectItem>(`/objects/by-address/${address}`);
  }

  async getObjectByPath(path: string): Promise<ApiResponse<ObjectItem>> {
    return this.request<ObjectItem>(`/objects/by-path/${encodeURIComponent(path)}`);
  }

  // Classes endpoints
  async getClasses(offset: number = 0, limit: number = 50): Promise<ApiResponse<ClassesResponse>> {
    return this.request<ClassesResponse>(`/classes?offset=${offset}&limit=${limit}`);
  }

  async getClassByName(name: string): Promise<ApiResponse<ClassItem>> {
    return this.request<ClassItem>(`/classes/${encodeURIComponent(name)}`);
  }

  // Packages endpoints
  async getPackages(offset: number = 0, limit: number = 50): Promise<ApiResponse<PackagesResponse>> {
    return this.request<PackagesResponse>(`/packages?offset=${offset}&limit=${limit}`);
  }

  async getPackageContents(packageName: string, offset: number = 0, limit: number = 50): Promise<ApiResponse<ObjectsResponse>> {
    return this.request<ObjectsResponse>(
      `/packages/${encodeURIComponent(packageName)}/contents?offset=${offset}&limit=${limit}`
    );
  }

  // Dump endpoints
  async startDump(type: 'sdk' | 'usmap' | 'dumpspace' | 'ida-script'): Promise<ApiResponse<{ job_id: string }>> {
    return this.request<{ job_id: string }>(`/dump/${type}`, { method: 'POST' });
  }

  async getDumpJobs(): Promise<ApiResponse<DumpJob[]>> {
    return this.request<DumpJob[]>('/dump/jobs');
  }

  async getDumpJob(id: string): Promise<ApiResponse<DumpJob>> {
    return this.request<DumpJob>(`/dump/jobs/${id}`);
  }

  // Memory endpoints
  async readMemory(address: string, size: number): Promise<ApiResponse<{ bytes: string }>> {
    return this.request<{ bytes: string }>('/memory/read', {
      method: 'POST',
      body: JSON.stringify({ address, size }),
    });
  }

  async writeMemory(address: string, bytes: number[]): Promise<ApiResponse<{ written: number }>> {
    return this.request<{ written: number }>('/memory/write', {
      method: 'POST',
      body: JSON.stringify({ address, bytes }),
    });
  }

  // Call endpoints
  async callFunction(
    objectIndex: number,
    functionName: string,
    params: Record<string, unknown> = {}
  ): Promise<ApiResponse<{ result: unknown }>> {
    return this.request<{ result: unknown }>('/call/function', {
      method: 'POST',
      body: JSON.stringify({ object_index: objectIndex, function_name: functionName, params }),
    });
  }

  // Watch endpoints
  async addWatch(objectIndex: number, property: string): Promise<ApiResponse<{ id: number }>> {
    return this.request<{ id: number }>('/watch/add', {
      method: 'POST',
      body: JSON.stringify({ object_index: objectIndex, property }),
    });
  }

  async listWatches(): Promise<ApiResponse<{ watches: unknown[] }>> {
    return this.request<{ watches: unknown[] }>('/watch/list');
  }

  async removeWatch(id: number): Promise<ApiResponse<{ removed: boolean }>> {
    return this.request<{ removed: boolean }>(`/watch/${id}`, { method: 'DELETE' });
  }

  // Hook endpoints
  async addHook(functionPath: string): Promise<ApiResponse<{ id: number }>> {
    return this.request<{ id: number }>('/hooks/add', {
      method: 'POST',
      body: JSON.stringify({ function_path: functionPath }),
    });
  }

  async listHooks(): Promise<ApiResponse<{ hooks: unknown[] }>> {
    return this.request<{ hooks: unknown[] }>('/hooks/list');
  }

  async removeHook(id: number): Promise<ApiResponse<{ removed: boolean }>> {
    return this.request<{ removed: boolean }>(`/hooks/${id}`, { method: 'DELETE' });
  }

  async getHookLog(id: number): Promise<ApiResponse<{ entries: unknown[] }>> {
    return this.request<{ entries: unknown[] }>(`/hooks/${id}/log`);
  }

  // Process management (via Tauri commands)
  async scanUEProcesses(): Promise<{ pid: number; name: string; path: string }[]> {
    const { invoke } = await import('@tauri-apps/api/core');
    try {
      const processes = await invoke<{ pid: number; name: string; path: string }[]>('scan_ue_processes');
      return processes;
    } catch (error) {
      console.error('Failed to scan processes:', error);
      return [];
    }
  }

  async injectDLL(pid: number, dllPath: string): Promise<{ success: boolean; message: string }> {
    const { invoke } = await import('@tauri-apps/api/core');
    try {
      const result = await invoke<{ success: boolean; message: string }>('inject_dll', {
        pid,
        dllPath,
      });
      return result;
    } catch (error) {
      console.error('Failed to inject DLL:', error);
      return { success: false, message: String(error) };
    }
  }
}

export const api = new UExplorerApi();
export default api;
