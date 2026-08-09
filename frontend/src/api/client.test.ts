import { beforeEach, describe, expect, it, vi } from 'vitest';

const { invokeMock } = vi.hoisted(() => ({ invokeMock: vi.fn() }));

vi.mock('@tauri-apps/api/core', () => ({
  invoke: invokeMock,
  Channel: class<T> {
    onmessage: (message: T) => void = () => undefined;
  },
}));

import api from './client';

describe('UExplorerApi Tauri domain boundary', () => {
  beforeEach(() => {
    invokeMock.mockReset();
  });

  it('routes status through the typed domain_request command', async () => {
    invokeMock.mockResolvedValue({ success: true, data: { pid: 4242 }, error: null });

    const response = await api.getStatus();

    expect(response.success).toBe(true);
    expect(invokeMock).toHaveBeenCalledWith('domain_request', {
      request: {
        targetPid: null,
        operation: 'status.inspect',
        timeoutMs: 5_000,
        data: {},
      },
    });
  });

  it('routes every memory request to a named Host operation without transport recovery', async () => {
    invokeMock.mockResolvedValue({
      success: false,
      data: null,
      error: 'CAPABILITY_UNAVAILABLE',
      error_code: 'CAPABILITY_UNAVAILABLE',
    });

    const response = await api.readMemory('0x1000', 16);

    expect(response.error_code).toBe('CAPABILITY_UNAVAILABLE');
    expect(invokeMock).toHaveBeenCalledWith('domain_request', {
      request: {
        targetPid: null,
        operation: 'memory.raw.read',
        timeoutMs: 5_000,
        data: { address: '0x1000', size: 16 },
      },
    });
    expect(invokeMock).toHaveBeenCalledTimes(1);
  });

  it('turns a failed native invocation into one explicit Host error', async () => {
    invokeMock.mockRejectedValue(new Error('channel closed'));

    const response = await api.getObjectCounts();

    expect(response).toMatchObject({
      success: false,
      data: null,
      error_code: 'HOST_INVOKE_FAILED',
      error: 'HOST_INVOKE_FAILED: channel closed',
    });
    expect(invokeMock).toHaveBeenCalledTimes(1);
  });
});
