import { useCallback, useEffect, useRef, useState } from 'react';
import { Diamond, Settings, Terminal } from 'lucide-react';
import { t } from '../i18n';
import api from '../api';
import HierarchyPane from './objects/HierarchyPane';
import InstancePane from './objects/InstancePane';
import InspectorPane from './objects/InspectorPane';

export default function Objects() {
  const [selectedClass, setSelectedClass] = useState<string | null>(null);
  const [selectedIndex, setSelectedIndex] = useState<number | null>(null);
  const [connected, setConnected] = useState(false);

  const [pane1Width, setPane1Width] = useState(280);
  const [pane2Width, setPane2Width] = useState(360);
  const draggingRef = useRef<'r1' | 'r2' | null>(null);
  const startXRef = useRef(0);
  const startWidthRef = useRef(0);

  useEffect(() => {
    let alive = true;
    const check = async () => {
      const ok = await api.healthCheck();
      if (alive) setConnected(ok);
    };
    void check();
    const timer = setInterval(() => void check(), 5000);
    return () => { alive = false; clearInterval(timer); };
  }, []);

  const handleSelectClass = (cls: string) => {
    setSelectedClass(cls);
    setSelectedIndex(null);
  };

  const handleSelectInstance = (idx: number) => {
    setSelectedIndex(idx);
  };

  const onResizerDown = useCallback((which: 'r1' | 'r2') => (e: React.MouseEvent) => {
    e.preventDefault();
    draggingRef.current = which;
    startXRef.current = e.clientX;
    startWidthRef.current = which === 'r1' ? pane1Width : pane2Width;

    const onMove = (ev: MouseEvent) => {
      const delta = ev.clientX - startXRef.current;
      const newW = Math.max(180, Math.min(600, startWidthRef.current + delta));
      if (draggingRef.current === 'r1') setPane1Width(newW);
      else setPane2Width(newW);
    };
    const onUp = () => {
      draggingRef.current = null;
      document.removeEventListener('mousemove', onMove);
      document.removeEventListener('mouseup', onUp);
    };
    document.addEventListener('mousemove', onMove);
    document.addEventListener('mouseup', onUp);
  }, [pane1Width, pane2Width]);

  return (
    <div className="h-full flex flex-col bg-background-base text-text-mid font-ui overflow-hidden antialiased selection:bg-primary selection:text-white">
      <header className="h-[42px] apple-glass-panel border-b border-border-subtle flex items-center justify-between px-3 z-50 shrink-0">
        <div className="flex items-center gap-4">
          <div className="flex items-center gap-2 text-text-high">
            <Diamond className="w-5 h-5 text-primary fill-primary/20" />
            <span className="font-display font-bold text-sm tracking-wide">{t('UExplorer')}</span>
          </div>
          <div className="h-4 w-[1px] bg-border-subtle"></div>
          <button className="flex items-center gap-2 px-2 py-1 rounded hover:bg-white/5 transition-colors group">
            <Terminal className="w-4 h-4 text-accent-green" />
            <span className="text-xs font-mono text-text-high">{t('UExplorer Process')}</span>
          </button>
        </div>

        <div className="flex-1 max-w-lg mx-4">
          <div className="relative group flex items-center">
            <input
              className="block w-full bg-[#121212] border border-border-subtle text-text-high text-xs rounded pl-3 pr-3 py-1.5 focus:outline-none focus:border-primary focus:ring-1 focus:ring-primary placeholder-text-low font-mono transition-all"
              placeholder={t('Find Command or Item...')}
              type="text"
            />
          </div>
        </div>

        <div className="flex items-center gap-1">
          <button className="p-1.5 rounded hover:bg-white/5 text-text-mid hover:text-text-high transition-colors" title={t('Settings')}>
            <Settings className="w-4.5 h-4.5" />
          </button>
        </div>
      </header>

      <main className="flex-1 flex overflow-hidden">
        <div style={{ width: pane1Width, minWidth: 180, maxWidth: 600, flexShrink: 0 }}>
          <HierarchyPane onSelectClass={handleSelectClass} />
        </div>

        <div
          onMouseDown={onResizerDown('r1')}
          className="w-[4px] bg-border-subtle cursor-col-resize hover:bg-primary active:bg-primary shrink-0 z-10 transition-colors"
        />

        <div style={{ width: pane2Width, minWidth: 180, maxWidth: 600, flexShrink: 0 }}>
          <InstancePane selectedClass={selectedClass} onSelectInstance={handleSelectInstance} />
        </div>

        <div
          onMouseDown={onResizerDown('r2')}
          className="w-[4px] bg-border-subtle cursor-col-resize hover:bg-primary active:bg-primary shrink-0 z-10 transition-colors"
        />

        <InspectorPane selectedClass={selectedClass} selectedIndex={selectedIndex} />
      </main>

      <footer className="h-6 bg-primary text-white flex items-center px-3 justify-between shrink-0 text-2xs font-mono">
        <div className="flex items-center gap-4">
          <span className="flex items-center gap-1">
            <span className={`w-1.5 h-1.5 rounded-full ${connected ? 'bg-white animate-pulse' : 'bg-white/40'}`}></span>
            {connected ? t('Connected') : t('Disconnected')}
          </span>
        </div>
        <div className="opacity-80">{t('UExplorer')}</div>
      </footer>
    </div>
  );
}
