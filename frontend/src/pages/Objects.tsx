import { useState } from 'react';
import { Diamond, Settings, Terminal } from 'lucide-react';
import { t } from '../i18n';
import HierarchyPane from './objects/HierarchyPane';
import InstancePane from './objects/InstancePane';
import InspectorPane from './objects/InspectorPane';

export default function Objects() {
  const [selectedClass, setSelectedClass] = useState<string | null>(null);
  const [selectedIndex, setSelectedIndex] = useState<number | null>(null);

  const handleSelectClass = (cls: string) => {
    setSelectedClass(cls);
    setSelectedIndex(null); // Reset instance selection when switching class
  };

  const handleSelectInstance = (idx: number) => {
    setSelectedIndex(idx);
  };

  return (
    <div className="h-full flex flex-col bg-background-base text-text-mid font-ui overflow-hidden antialiased selection:bg-primary selection:text-white">
      {/* ── Top Toolbar: Glass Effect ── */}
      <header className="h-[42px] apple-glass-panel border-b border-border-subtle flex items-center justify-between px-3 z-50 shrink-0">
        {/* Left: Logo & Process */}
        <div className="flex items-center gap-4">
          <div className="flex items-center gap-2 text-text-high">
            <Diamond className="w-5 h-5 text-primary fill-primary/20" />
            <span className="font-display font-bold text-sm tracking-wide">{t('UExplorer')}</span>
          </div>
          <div className="h-4 w-[1px] bg-border-subtle"></div>
          {/* Process Selector (Static for now) */}
          <button className="flex items-center gap-2 px-2 py-1 rounded hover:bg-white/5 transition-colors group">
            <Terminal className="w-4 h-4 text-accent-green" />
            <span className="text-xs font-mono text-text-high">{t('UExplorer Process')}</span>
          </button>
        </div>

        {/* Center: Search / Commands */}
        <div className="flex-1 max-w-lg mx-4">
          <div className="relative group flex items-center">
            <input
              className="block w-full bg-[#121212] border border-border-subtle text-text-high text-xs rounded pl-3 pr-3 py-1.5 focus:outline-none focus:border-primary focus:ring-1 focus:ring-primary placeholder-text-low font-mono transition-all"
              placeholder={t('Find Command or Item...')}
              type="text"
            />
          </div>
        </div>

        {/* Right: Actions */}
        <div className="flex items-center gap-1">
          <button className="p-1.5 rounded hover:bg-white/5 text-text-mid hover:text-text-high transition-colors" title={t('Settings')}>
            <Settings className="w-4.5 h-4.5" />
          </button>
        </div>
      </header>

      {/* ── Main Workspace: Split Panes ── */}
      <main className="flex-1 flex overflow-hidden">
        {/* Pane 1: Class Hierarchy Tree */}
        <HierarchyPane onSelectClass={handleSelectClass} />

        {/* Resizer */}
        <div className="w-[1px] bg-border-subtle cursor-col-resize hover:bg-primary resizer-handle shrink-0 z-10"></div>

        {/* Pane 2: Instance List */}
        <InstancePane selectedClass={selectedClass} onSelectInstance={handleSelectInstance} />

        {/* Resizer */}
        <div className="w-[1px] bg-border-subtle cursor-col-resize hover:bg-primary resizer-handle shrink-0 z-10"></div>

        {/* Pane 3: Property Inspector */}
        <InspectorPane selectedClass={selectedClass} selectedIndex={selectedIndex} />
      </main>

      {/* ── Global Status Bar (Bottom) ── */}
      <footer className="h-6 bg-primary text-white flex items-center px-3 justify-between shrink-0 text-2xs font-mono">
        <div className="flex items-center gap-4">
          <span className="flex items-center gap-1">
            <span className="w-1.5 h-1.5 bg-white rounded-full animate-pulse"></span>
            {t('Connected')}
          </span>
        </div>
        <div className="opacity-80">{t('UExplorer')}</div>
      </footer>
    </div>
  );
}
