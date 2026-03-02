// Application types

export interface GameInfo {
  gameName: string;
  gameVersion: string;
  ueVersion: string;
  gobjectsAddress: string;
  objectCount: number;
}

export interface ConnectionState {
  connected: boolean;
  port: number;
  lastError?: string;
}

export interface Stats {
  classes: number;
  structs: number;
  enums: number;
  functions: number;
  packages: number;
  actors: number;
}

export type Page = 'dashboard' | 'objects' | 'functions' | 'memory' | 'sdkdump' | 'settings';

export interface NavItem {
  id: Page;
  label: string;
  icon: string;
}
