export function parseInputValue(raw: string): unknown {
    const trimmed = raw.trim();
    if (trimmed === '') return '';
    if (trimmed === 'true') return true;
    if (trimmed === 'false') return false;
    if (trimmed === 'null') return null;
    if (!Number.isNaN(Number(trimmed))) return Number(trimmed);
    try {
        return JSON.parse(trimmed);
    } catch {
        return trimmed;
    }
}

export function toEditable(value: unknown): string {
    if (typeof value === 'string') return value;
    if (typeof value === 'number' || typeof value === 'boolean') return String(value);
    if (value === null || value === undefined) return '';
    try {
        return JSON.stringify(value);
    } catch {
        return String(value);
    }
}
