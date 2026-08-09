import { describe, expect, it } from 'vitest';
import { parseInputValue, toEditable } from './valueUtils';

describe('parseInputValue', () => {
  it('preserves empty and string values', () => {
    expect(parseInputValue('')).toBe('');
    expect(parseInputValue('  text  ')).toBe('text');
  });

  it('parses supported primitive values', () => {
    expect(parseInputValue('true')).toBe(true);
    expect(parseInputValue('false')).toBe(false);
    expect(parseInputValue('null')).toBeNull();
    expect(parseInputValue('42.5')).toBe(42.5);
  });

  it('parses JSON objects without evaluating input', () => {
    expect(parseInputValue('{"enabled":true}')).toEqual({ enabled: true });
  });
});

describe('toEditable', () => {
  it('serializes values into stable input text', () => {
    expect(toEditable(undefined)).toBe('');
    expect(toEditable(null)).toBe('');
    expect(toEditable(false)).toBe('false');
    expect(toEditable({ value: 7 })).toBe('{"value":7}');
  });
});
