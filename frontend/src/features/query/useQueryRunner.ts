import { useCallback, useEffect, useRef } from 'react';

interface CacheEntry<T> {
  value: T;
  expiresAt: number;
}

interface QueryOptions {
  cacheMs?: number;
}

const cache = new Map<string, CacheEntry<unknown>>();

function cancelled(): DOMException {
  return new DOMException('The query was superseded or cancelled', 'AbortError');
}

export function isAbortError(error: unknown): boolean {
  return error instanceof DOMException && error.name === 'AbortError';
}

export function invalidateQueries(prefix = ''): void {
  for (const key of cache.keys()) {
    if (!prefix || key.startsWith(prefix)) cache.delete(key);
  }
}

export function useQueryRunner() {
  const requests = useRef(new Map<string, { version: number; controller: AbortController }>());

  useEffect(() => () => {
    requests.current.forEach(({ controller }) => controller.abort());
    requests.current.clear();
  }, []);

  const cancel = useCallback((key?: string) => {
    if (key) {
      requests.current.get(key)?.controller.abort();
      requests.current.delete(key);
      return;
    }
    requests.current.forEach(({ controller }) => controller.abort());
    requests.current.clear();
  }, []);

  const run = useCallback(async <T,>(
    key: string,
    loader: (signal: AbortSignal) => Promise<T>,
    options: QueryOptions = {},
  ): Promise<T> => {
    const cached = cache.get(key) as CacheEntry<T> | undefined;
    if (cached && cached.expiresAt > Date.now()) return cached.value;

    const previous = requests.current.get(key);
    previous?.controller.abort();
    const controller = new AbortController();
    const version = (previous?.version ?? 0) + 1;
    requests.current.set(key, { version, controller });

    const value = await loader(controller.signal);
    const current = requests.current.get(key);
    if (controller.signal.aborted || current?.version !== version) throw cancelled();
    requests.current.delete(key);
    if ((options.cacheMs ?? 0) > 0) {
      cache.set(key, { value, expiresAt: Date.now() + (options.cacheMs ?? 0) });
    }
    return value;
  }, []);

  return { run, cancel };
}
