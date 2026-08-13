import type {
  ApiResponse,
  EventBridgeDiagnostics,
  HostProcessInfo,
  HostSessionEvent,
  InjectionCommandResult,
  SessionEventSubscribeOptions,
  SessionEventSubscription,
} from '../contracts';

export interface DomainRequestOptions {
  timeoutMs?: number;
  targetPid?: number | null;
  signal?: AbortSignal;
}

function abortError(): DOMException {
  return new DOMException('The request was cancelled', 'AbortError');
}

async function abortable<T>(promise: Promise<T>, signal?: AbortSignal): Promise<T> {
  if (!signal) return promise;
  if (signal.aborted) throw abortError();

  return new Promise<T>((resolve, reject) => {
    const onAbort = () => reject(abortError());
    signal.addEventListener('abort', onAbort, { once: true });
    promise.then(
      (value) => {
        signal.removeEventListener('abort', onAbort);
        resolve(value);
      },
      (error) => {
        signal.removeEventListener('abort', onAbort);
        reject(error);
      },
    );
  });
}

class TauriTransport {
  async requestDomain<T>(
    operation: string,
    data: Record<string, unknown> = {},
    options: DomainRequestOptions = {},
  ): Promise<ApiResponse<T>> {
    try {
      const { invoke } = await import('@tauri-apps/api/core');
      return await abortable(invoke<ApiResponse<T>>('domain_request', {
        request: {
          targetPid: options.targetPid ?? null,
          operation,
          timeoutMs: options.timeoutMs ?? 5_000,
          data,
        },
      }), options.signal);
    } catch (error) {
      if (error instanceof DOMException && error.name === 'AbortError') throw error;
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

  async scanUEProcesses(signal?: AbortSignal): Promise<HostProcessInfo[]> {
    const { invoke } = await import('@tauri-apps/api/core');
    return abortable(invoke<HostProcessInfo[]>('scan_ue_processes'), signal);
  }

  async injectDLL(
    process: HostProcessInfo,
    dllPath: string,
    signal?: AbortSignal,
  ): Promise<InjectionCommandResult> {
    const { invoke } = await import('@tauri-apps/api/core');
    return abortable(invoke<InjectionCommandResult>('inject_and_connect', {
      pid: process.pid,
      dllPath,
      expectedStartTime100ns: process.start_time_100ns,
      expectedProcessPath: process.path,
    }), signal);
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

export const tauriTransport = new TauriTransport();
