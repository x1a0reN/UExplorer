export type HexAddress = string & { readonly __hexAddress: unique symbol };

const U64_MAX = 0xFFFF_FFFF_FFFF_FFFFn;

export function parseAddress(value: string): HexAddress {
  const trimmed = value.trim();
  if (!/^(?:0[xX])?[0-9A-Fa-f]{1,16}$/.test(trimmed)) {
    throw new Error('Address must be one to sixteen hexadecimal digits');
  }
  const parsed = BigInt(trimmed.startsWith('0x') || trimmed.startsWith('0X') ? trimmed : `0x${trimmed}`);
  if (parsed < 0n || parsed > U64_MAX) throw new Error('Address is outside the unsigned 64-bit range');
  return `0x${parsed.toString(16).toUpperCase()}` as HexAddress;
}

export function tryParseAddress(value: string, fallback: HexAddress = '0x0' as HexAddress): HexAddress {
  try {
    return parseAddress(value);
  } catch {
    return fallback;
  }
}

export function addAddress(address: string, offset: bigint | number): HexAddress {
  const value = BigInt(parseAddress(address)) + BigInt(offset);
  if (value < 0n || value > U64_MAX) throw new Error('Address arithmetic overflowed the unsigned 64-bit range');
  return `0x${value.toString(16).toUpperCase()}` as HexAddress;
}

export function formatAddress(address: string | null | undefined): string {
  if (!address) return '-';
  try {
    return parseAddress(address);
  } catch {
    return address;
  }
}
