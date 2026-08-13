import { beforeEach, describe, expect, it, vi } from 'vitest';

const { invokeMock } = vi.hoisted(() => ({ invokeMock: vi.fn() }));

vi.mock('@tauri-apps/api/core', () => ({
  invoke: invokeMock,
  Channel: class<T> {
    onmessage: (message: T) => void = () => undefined;
  },
}));

import api from './client';
import { isHookPushEventData } from './index';

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

  it('uses generation-bound snapshot cursors instead of offset pagination', async () => {
    const cursor = {
      generation: 7,
      after_index: 42,
      query_fingerprint: 'fixture-query',
    };
    invokeMock.mockResolvedValue({ success: true, data: { items: [], has_more: false } });

    await api.searchObjects('Actor', {
      kind: 'class',
      classPath: '/Script/CoreUObject.Class',
      packagePath: '/Script/Engine',
      cursor,
      limit: 64,
    });

    expect(invokeMock).toHaveBeenCalledWith('domain_request', {
      request: {
        targetPid: null,
        operation: 'objects.search',
        timeoutMs: 5_000,
        data: {
          search: 'Actor',
          kind: 'class',
          class_path: '/Script/CoreUObject.Class',
          package_path: '/Script/Engine',
          cursor,
          limit: 64,
        },
      },
    });
  });

  it('uses an immutable world cursor and explicit actor filters', async () => {
    const cursor = {
      generation: 3,
      after_ordinal: 127,
      query_fingerprint: 'A1B2C3D4E5F60708',
    };
    invokeMock.mockResolvedValue({ success: true, data: { items: [], has_more: false } });

    await api.getWorldActors(
      cursor,
      64,
      'Player',
      'Character',
      '/Game/Maps/Fixture.Fixture.PersistentLevel',
    );

    expect(invokeMock).toHaveBeenCalledWith('domain_request', {
      request: {
        targetPid: null,
        operation: 'world.actors.list',
        timeoutMs: 5_000,
        data: {
          cursor,
          limit: 64,
          search: 'Player',
          class_search: 'Character',
          level_path: '/Game/Maps/Fixture.Fixture.PersistentLevel',
        },
      },
    });
  });

  it('uses an exact actor handle and world generation for immutable details', async () => {
    const actor = {
      session_id: 'world-fixture',
      context_generation: 81,
      index: 3,
      serial: 103,
      address: '0x3000',
      class_fingerprint: '000000000000A003',
    };
    invokeMock.mockResolvedValue({ success: true, data: { components: [], has_more: false } });

    await api.getWorldActorDetail(actor, 3);
    await api.getWorldActorComponents(actor, 3, null, 64);
    await api.getWorldActorTransform(actor, 3);

    expect(invokeMock).toHaveBeenNthCalledWith(1, 'domain_request', {
      request: {
        targetPid: null,
        operation: 'world.actor.get',
        timeoutMs: 5_000,
        data: { actor, world_snapshot_generation: 3 },
      },
    });
    expect(invokeMock).toHaveBeenNthCalledWith(2, 'domain_request', {
      request: {
        targetPid: null,
        operation: 'world.actor.components',
        timeoutMs: 5_000,
        data: {
          actor,
          world_snapshot_generation: 3,
          cursor: null,
          limit: 64,
        },
      },
    });
    expect(invokeMock).toHaveBeenNthCalledWith(3, 'domain_request', {
      request: {
        targetPid: null,
        operation: 'world.actor.transform.get',
        timeoutMs: 5_000,
        data: { actor, world_snapshot_generation: 3 },
      },
    });
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

describe('Hook push event guard', () => {
  const scalarEvent = {
    hook_name: 'Function /Script/Fixture.Target',
    hook_id: '7',
    id: 7,
    function_path: 'Function /Script/Fixture.Target',
    source_sequence: 11,
    configuration_generation: 3,
    source: '0x7FF61234ABCD',
    correlation: 9,
    coalesced_before: 0,
    drained_at_monotonic_us: 123456,
    capture: { mode: 'scalar_parameters' },
    parameters: {
      encoding: 'uexplorer.hook-parameters.v1',
      phase: 'enter',
      status: 'ok',
      plan_fingerprint: '0123456789ABCDEF',
      error_code: null,
      values: [{
        name: 'Count',
        type_name: 'int32',
        direction: 'input',
        kind: 'int32',
        value: '-7',
      }],
    },
    payload: { encoding: 'hex', size: 2, data: '00FF' },
    payload_omitted: false,
    retained_log_dropped_before: 0,
    collector_overflow_dropped_before: 0,
    collector_oversize_dropped_before: 0,
    collector_contention_dropped_before: 0,
    push_dropped_before: 0,
    publisher_dropped_before: 0,
  };

  it('accepts a complete scalar capture and rejects crossed or malformed payload state', () => {
    expect(isHookPushEventData(scalarEvent)).toBe(true);
    expect(isHookPushEventData({ ...scalarEvent, parameters: null })).toBe(false);
    expect(isHookPushEventData({
      ...scalarEvent,
      parameters: {
        ...scalarEvent.parameters,
        plan_fingerprint: '0123456789abcdef',
      },
    })).toBe(false);
    expect(isHookPushEventData({
      ...scalarEvent,
      payload: { encoding: 'hex', size: 1, data: '00FF' },
    })).toBe(false);
    expect(isHookPushEventData({
      ...scalarEvent,
      capture: { mode: 'fixed_metadata' },
    })).toBe(false);
  });

  it('accepts an explicit scalar capture failure without fabricating values', () => {
    expect(isHookPushEventData({
      ...scalarEvent,
      parameters: {
        ...scalarEvent.parameters,
        status: 'read_failed',
        error_code: 'HOOK_PARAMETER_READ_FAILED',
        values: [],
      },
    })).toBe(true);
    expect(isHookPushEventData({
      ...scalarEvent,
      parameters: {
        ...scalarEvent.parameters,
        status: 'read_failed',
        error_code: 'HOOK_PARAMETER_READ_FAILED',
      },
    })).toBe(false);
    expect(isHookPushEventData({
      ...scalarEvent,
      parameters: {
        ...scalarEvent.parameters,
        status: 'read_failed',
        error_code: 'HOOK_PARAMETER_PLAN_MISMATCH',
        values: [],
      },
    })).toBe(false);
    expect(isHookPushEventData({
      ...scalarEvent,
      parameters: {
        ...scalarEvent.parameters,
        unexpected: true,
      },
    })).toBe(false);
  });
});
