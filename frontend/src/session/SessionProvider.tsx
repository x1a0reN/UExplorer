import {
  createContext,
  useCallback,
  useContext,
  useEffect,
  useMemo,
  useReducer,
  type ReactNode,
} from 'react';
import type {
  ApiResponse,
  EngineStatusData,
  HostSessionEvent,
  StatusData,
} from '../contracts';
import { isHookPushEventData, isWatchPushEventData } from '../contracts';
import api from '../services/uexplorerService';
import { isAbortError, useQueryRunner } from '../features/query/useQueryRunner';

export type ConnectionLayerState = 'checking' | 'ready' | 'unavailable';

export interface ConnectionLayer {
  id: 'host' | 'ipc' | 'core' | 'engine' | 'capability';
  label: string;
  state: ConnectionLayerState;
  detail: string;
}

interface SessionState {
  status: StatusData | null;
  engineStatus: EngineStatusData | null;
  checking: boolean;
  lastError: string | null;
  lastErrorCode: string | null;
  eventChannel: ConnectionLayerState;
  eventError: string | null;
  eventCount: number;
  droppedEvents: number;
  watchEvents: HostSessionEvent[];
  hookEvents: HostSessionEvent[];
}

type Action =
  | { type: 'checking' }
  | {
      type: 'status';
      status: ApiResponse<StatusData>;
      engine: ApiResponse<EngineStatusData>;
    }
  | { type: 'event_channel'; state: ConnectionLayerState; error?: string | null }
  | { type: 'event'; value: HostSessionEvent };

const initialState: SessionState = {
  status: null,
  engineStatus: null,
  checking: true,
  lastError: null,
  lastErrorCode: null,
  eventChannel: 'checking',
  eventError: null,
  eventCount: 0,
  droppedEvents: 0,
  watchEvents: [],
  hookEvents: [],
};

function reducer(state: SessionState, action: Action): SessionState {
  if (action.type === 'checking') return { ...state, checking: true };
  if (action.type === 'status') {
    const nextStatus = action.status.success ? action.status.data : null;
    const sessionChanged = state.status?.pid !== nextStatus?.pid;
    return {
      ...state,
      checking: false,
      status: nextStatus,
      engineStatus: action.engine.success ? action.engine.data : null,
      lastError: action.status.success ? null : action.status.error,
      lastErrorCode: action.status.success ? null : action.status.error_code ?? null,
      watchEvents: sessionChanged ? [] : state.watchEvents,
      hookEvents: sessionChanged ? [] : state.hookEvents,
    };
  }
  if (action.type === 'event_channel') {
    return { ...state, eventChannel: action.state, eventError: action.error ?? null };
  }

  const { event, host_dropped_before: hostDropped } = action.value;
  const droppedEvents = Math.max(state.droppedEvents, event.dropped_before + hostDropped);
  if (event.kind.startsWith('watch.') && isWatchPushEventData(event.data)) {
    return {
      ...state,
      eventCount: state.eventCount + 1,
      droppedEvents,
      watchEvents: [...state.watchEvents, action.value].slice(-512),
    };
  }
  if (event.kind.startsWith('hook.') && isHookPushEventData(event.data)) {
    return {
      ...state,
      eventCount: state.eventCount + 1,
      droppedEvents,
      hookEvents: [...state.hookEvents, action.value].slice(-512),
    };
  }
  return { ...state, eventCount: state.eventCount + 1, droppedEvents };
}

interface SessionContextValue extends SessionState {
  layers: ConnectionLayer[];
  refresh: () => Promise<void>;
}

const SessionContext = createContext<SessionContextValue | null>(null);

function connectionLayers(state: SessionState): ConnectionLayer[] {
  const hostUnavailable = state.lastErrorCode === 'HOST_INVOKE_FAILED';
  const runtime = state.status?.runtime;
  const capabilities = Object.values(state.status?.capabilities ?? {});
  const availableCapabilities = capabilities.filter((capability) => capability.available).length;
  return [
    {
      id: 'host',
      label: 'Host',
      state: state.checking && !state.status ? 'checking' : hostUnavailable ? 'unavailable' : 'ready',
      detail: hostUnavailable ? state.lastError || 'Tauri Host unavailable' : 'Desktop Host responding',
    },
    {
      id: 'ipc',
      label: 'IPC',
      state: state.status ? 'ready' : state.checking ? 'checking' : 'unavailable',
      detail: state.status
        ? `PID ${state.status.pid} / events ${state.eventChannel}`
        : state.lastError || 'No active PID-scoped session',
    },
    {
      id: 'core',
      label: 'Core',
      state: runtime?.liveness ? 'ready' : state.checking ? 'checking' : 'unavailable',
      detail: runtime ? `${runtime.state} / ${runtime.session_id || 'no session id'}` : 'Core status unavailable',
    },
    {
      id: 'engine',
      label: 'Engine',
      state: runtime?.readiness && state.engineStatus ? 'ready' : state.checking ? 'checking' : 'unavailable',
      detail: state.engineStatus
        ? `${state.engineStatus.game_name} ${state.engineStatus.game_version}`
        : 'Engine context is not ready',
    },
    {
      id: 'capability',
      label: 'Capability',
      state: availableCapabilities > 0 ? 'ready' : state.checking ? 'checking' : 'unavailable',
      detail: capabilities.length > 0
        ? `${availableCapabilities}/${capabilities.length} available`
        : 'No capability snapshot',
    },
  ];
}

export function SessionProvider({ children }: { children: ReactNode }) {
  const [state, dispatch] = useReducer(reducer, initialState);
  const { run } = useQueryRunner();

  const refresh = useCallback(async () => {
    dispatch({ type: 'checking' });
    try {
      const [status, engine] = await run('session:status', async () => Promise.all([
        api.getStatus(),
        api.getEngineStatus(),
      ]), { cacheMs: 500 });
      dispatch({ type: 'status', status, engine });
    } catch (error) {
      if (isAbortError(error)) return;
      dispatch({
        type: 'status',
        status: {
          success: false,
          data: null,
          error: error instanceof Error ? error.message : String(error),
          error_code: 'SESSION_REFRESH_FAILED',
        },
        engine: { success: false, data: null, error: null },
      });
    }
  }, [run]);

  useEffect(() => {
    void refresh();
    const timer = window.setInterval(() => void refresh(), 3_000);
    return () => window.clearInterval(timer);
  }, [refresh]);

  useEffect(() => {
    const pid = state.status?.pid;
    if (!pid) {
      dispatch({ type: 'event_channel', state: 'unavailable', error: 'No active session' });
      return;
    }

    let disposed = false;
    let retryTimer: number | null = null;
    let unsubscribe: (() => Promise<boolean>) | null = null;

    const connect = async () => {
      dispatch({ type: 'event_channel', state: 'checking' });
      try {
        const subscription = await api.subscribeSessionEvents(pid, {
          onEvent: (event) => dispatch({ type: 'event', value: event }),
        });
        if (disposed) {
          void subscription.unsubscribe();
          return;
        }
        unsubscribe = subscription.unsubscribe;
        dispatch({ type: 'event_channel', state: 'ready' });
      } catch (error) {
        if (disposed) return;
        dispatch({
          type: 'event_channel',
          state: 'unavailable',
          error: error instanceof Error ? error.message : String(error),
        });
        retryTimer = window.setTimeout(() => void connect(), 2_000);
      }
    };

    void connect();
    return () => {
      disposed = true;
      if (retryTimer !== null) window.clearTimeout(retryTimer);
      if (unsubscribe) void unsubscribe();
    };
  }, [state.status?.pid]);

  const value = useMemo<SessionContextValue>(() => ({
    ...state,
    layers: connectionLayers(state),
    refresh,
  }), [refresh, state]);

  return <SessionContext.Provider value={value}>{children}</SessionContext.Provider>;
}

export function useSession(): SessionContextValue {
  const value = useContext(SessionContext);
  if (!value) throw new Error('useSession must be used inside SessionProvider');
  return value;
}
