import { useState } from 'react';
import { Box, Database, Globe2 } from 'lucide-react';
import { t } from '../i18n';
import type { Page } from '../types';
import InstanceBrowser from './objects/InstanceBrowser';
import TypeBrowser from './objects/TypeBrowser';
import WorldBrowser from './objects/WorldBrowser';
import type { BrowseMode, ModeNavContext } from './objects/shared';

interface ObjectsProps {
  onNavigate?: (page: Page) => void;
}

const MODES = [
  { id: 'types' as const, label: 'Types', icon: Database },
  { id: 'instances' as const, label: 'Instances', icon: Box },
  { id: 'world' as const, label: 'World', icon: Globe2 },
];

export default function Objects({ onNavigate }: ObjectsProps) {
  const [mode, setMode] = useState<BrowseMode>('types');
  const [navContext, setNavContext] = useState<ModeNavContext | undefined>();

  const switchMode = (next: BrowseMode, context?: ModeNavContext) => {
    setNavContext(context);
    setMode(next);
  };

  return (
    <div className="h-full flex flex-col bg-background-base text-text-mid overflow-hidden">
      <header className="h-11 flex-none border-b border-border-subtle bg-surface-dark flex items-center px-4 gap-1">
        {MODES.map((item) => (
          <button
            key={item.id}
            onClick={() => switchMode(item.id)}
            className={`h-8 px-3 rounded-lg flex items-center gap-2 text-xs font-semibold transition-colors ${mode === item.id ? 'bg-primary text-white' : 'text-text-low hover:bg-surface-stripe hover:text-text-high'}`}
          >
            <item.icon className="w-3.5 h-3.5" />
            {t(item.label)}
          </button>
        ))}
      </header>
      <main className="flex-1 min-h-0 overflow-hidden">
        {mode === 'types' && <TypeBrowser navContext={navContext} onSwitchMode={switchMode} />}
        {mode === 'instances' && (
          <InstanceBrowser
            navContext={navContext}
            onSwitchMode={switchMode}
            onNavigate={onNavigate}
          />
        )}
        {mode === 'world' && <WorldBrowser onSwitchMode={switchMode} />}
      </main>
    </div>
  );
}
