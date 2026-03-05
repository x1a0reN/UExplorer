import { useState } from 'react';
import { Layers, Box, Globe } from 'lucide-react';
import { t } from '../i18n';
import type { Page } from '../types';
import type { BrowseMode, ModeNavContext } from './objects/shared';
import TypeBrowser from './objects/TypeBrowser';
import InstanceBrowser from './objects/InstanceBrowser';
import WorldBrowser from './objects/WorldBrowser';

interface ObjectsProps {
  onNavigate?: (page: Page) => void;
}

const MODES: { id: BrowseMode; icon: typeof Layers; label: string; desc: string }[] = [
  { id: 'types', icon: Layers, label: 'Types', desc: 'Class / Struct / Enum' },
  { id: 'instances', icon: Box, label: 'Instances', desc: 'Live Objects' },
  { id: 'world', icon: Globe, label: 'World', desc: 'Actors & Levels' },
];

export default function Objects({ onNavigate }: ObjectsProps) {
  const [mode, setMode] = useState<BrowseMode>('types');
  const [navContext, setNavContext] = useState<ModeNavContext | undefined>();

  const handleSwitchMode = (newMode: BrowseMode, context?: ModeNavContext) => {
    setMode(newMode);
    setNavContext(context);
  };

  return (
    <div className="flex flex-col h-full bg-[#0A0A0C]">
      {/* ── Mode Selector Bar ── */}
      <div className="h-14 border-b border-white/5 bg-white/[0.02] backdrop-blur-3xl flex items-center px-6 gap-2 z-30 flex-none">
        {MODES.map((m) => {
          const Icon = m.icon;
          const active = mode === m.id;
          return (
            <button
              key={m.id}
              onClick={() => handleSwitchMode(m.id)}
              className={`relative h-10 px-4 flex items-center gap-2.5 rounded-lg text-[13px] font-semibold tracking-tight transition-all ${active
                  ? 'bg-white/10 text-white shadow-sm border border-white/10'
                  : 'text-white/40 hover:text-white/70 hover:bg-white/5 border border-transparent'
                }`}
            >
              <Icon className="w-4 h-4" />
              <span>{t(m.label)}</span>
              <span className={`text-[10px] ${active ? 'text-white/50' : 'text-white/25'}`}>{m.desc}</span>
            </button>
          );
        })}
      </div>

      {/* ── Mode Content ── */}
      <div className="flex-1 min-h-0">
        {mode === 'types' && <TypeBrowser onNavigate={onNavigate} onSwitchMode={handleSwitchMode} />}
        {mode === 'instances' && <InstanceBrowser onNavigate={onNavigate} onSwitchMode={handleSwitchMode} navContext={navContext} />}
        {mode === 'world' && <WorldBrowser onNavigate={onNavigate} onSwitchMode={handleSwitchMode} />}
      </div>
    </div>
  );
}
