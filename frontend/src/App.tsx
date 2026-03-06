import { useState } from 'react';
import { t } from './i18n';
import {
  Box,
  TerminalSquare,
  MemoryStick,
  Download,
  Settings,
  Search,
  Command,
  LayoutGrid,
  Power,
} from 'lucide-react';
import type { Page } from './types';
import Dashboard from './pages/Dashboard';
import Objects from './pages/Objects';
import Functions from './pages/Functions';
import Memory from './pages/Memory';
import SDKDump from './pages/SDKDump';
import SettingsView from './pages/Settings';

function App() {
  const [currentPage, setCurrentPage] = useState<Page>('dashboard');
  const [functionsViewMode, setFunctionsViewMode] = useState<'function' | 'hookManager'>('function');

  const sidebarNavItems = [
    { key: 'dashboard', id: 'dashboard' as Page, label: t('Overview'), icon: LayoutGrid },
    { key: 'objects', id: 'objects' as Page, label: t('Object Browser'), icon: Box },
    { key: 'functions', id: 'functions' as Page, label: t('Functions'), icon: TerminalSquare, mode: 'function' as const },
    { key: 'hooks', id: 'functions' as Page, label: t('Hook Manager'), icon: Power, mode: 'hookManager' as const },
    { key: 'memory', id: 'memory' as Page, label: t('Memory Tool'), icon: MemoryStick },
  ];

  const exportItems = [
    { key: 'sdkdump', id: 'sdkdump' as Page, label: t('Export Center'), icon: Download },
    { key: 'settings', id: 'settings' as Page, label: t('Settings'), icon: Settings },
  ];

  const renderPage = () => {
    switch (currentPage) {
      case 'dashboard':
        return <Dashboard onNavigate={(page) => {
          if (page === 'functions') {
            setFunctionsViewMode('function');
          }
          setCurrentPage(page);
        }} />;
      case 'objects':
        return <Objects />;
      case 'functions':
        return <Functions viewMode={functionsViewMode} onViewModeChange={setFunctionsViewMode} />;
      case 'memory':
        return <Memory />;
      case 'sdkdump':
        return <SDKDump />;
      case 'settings':
        return <SettingsView />;
      default:
        return <Dashboard onNavigate={(page) => {
          if (page === 'functions') {
            setFunctionsViewMode('function');
          }
          setCurrentPage(page);
        }} />;
    }
  };

  return (
    <div className="h-screen flex bg-background-base text-text-mid font-ui overflow-hidden selection:bg-primary/30 selection:text-white antialiased">

      {/* Sidebar - macOS Style Frosted Glass */}
      <aside className="w-56 flex flex-col bg-surface-dark border-r border-border-subtle z-20 relative">
        {/* macOS Window Controls */}
        <div className="h-10 flex items-center px-4 gap-2 window-drag-region">
          <div className="flex gap-2 group">
            <div className="w-3 h-3 rounded-full bg-text-low/40 group-hover:bg-accent-red transition-colors border border-border-subtle"></div>
            <div className="w-3 h-3 rounded-full bg-text-low/40 group-hover:bg-accent-yellow transition-colors border border-border-subtle"></div>
            <div className="w-3 h-3 rounded-full bg-text-low/40 group-hover:bg-accent-green transition-colors border border-border-subtle"></div>
          </div>
        </div>

        {/* Sidebar Content */}
        <div className="flex-1 overflow-y-auto px-2 pb-4 space-y-4">
          <div className="px-2 pt-2">
            <h1 className="text-[10px] font-bold text-text-low uppercase tracking-widest mb-2 font-display">{t('UExplorer')}</h1>
          </div>

          <div className="space-y-0.5">
            {sidebarNavItems.map((item) => {
              const isActive = currentPage === item.id && (item.id !== 'functions' || functionsViewMode === item.mode);
              return (
                <button
                  key={item.key}
                  onClick={() => {
                    setCurrentPage(item.id);
                    if (item.id === 'functions' && item.mode) {
                      setFunctionsViewMode(item.mode);
                    }
                  }}
                  className={`w-full flex items-center gap-2.5 px-2.5 py-1.5 rounded-lg transition-all duration-150 cursor-pointer font-display text-[12px] font-medium tracking-tight ${isActive
                    ? 'bg-primary text-white'
                    : 'text-text-mid hover:bg-surface-stripe hover:text-text-high'
                    }`}
                >
                  <item.icon className={`w-4 h-4 stroke-[2] ${isActive ? 'text-white' : 'text-text-low'}`} />
                  {item.label}
                </button>
              );
            })}
          </div>

          <div className="space-y-0.5">
            <div className="px-2.5 mb-1 pt-3">
              <span className="text-[10px] font-bold text-text-low uppercase tracking-widest font-display">{t('System')}</span>
            </div>
            {exportItems.map((item) => {
              const isActive = currentPage === item.id;
              return (
                <button
                  key={item.key}
                  onClick={() => setCurrentPage(item.id)}
                  className={`w-full flex items-center gap-2.5 px-2.5 py-1.5 rounded-lg transition-all duration-150 cursor-pointer font-display text-[12px] font-medium tracking-tight ${isActive
                    ? 'bg-primary text-white'
                    : 'text-text-mid hover:bg-surface-stripe hover:text-text-high'
                    }`}
                >
                  <item.icon className={`w-4 h-4 stroke-[2] ${isActive ? 'text-white' : 'text-text-low'}`} />
                  {item.label}
                </button>
              );
            })}
          </div>
        </div>

        {/* User / Dev Profile Area at bottom */}
        <div className="p-3 border-t border-border-subtle">
          <button className="w-full flex items-center gap-2.5 px-2 py-1.5 rounded-lg hover:bg-surface-stripe transition-colors cursor-pointer group">
            <div className="w-7 h-7 rounded-full bg-gradient-to-tr from-primary to-indigo-500 flex items-center justify-center border border-border-subtle">
              <span className="text-[11px] font-bold text-white font-display">U</span>
            </div>
            <div className="flex flex-col items-start flex-1 min-w-0">
              <span className="text-[12px] font-medium text-text-high truncate tracking-tight font-display">{t('DevUser')}</span>
              <span className="text-[10px] text-text-low truncate font-mono">{t('Local Agent')}</span>
            </div>
          </button>
        </div>
      </aside>

      {/* Main Content Area */}
      <main className="flex-1 flex flex-col min-w-0 bg-background-base relative z-10">
        {/* Top Header */}
        <header className="h-10 flex-none bg-surface-dark border-b border-border-subtle flex items-center justify-between px-6 sticky top-0 z-30">
          <div className="flex items-center gap-3">
            <span className="text-[13px] font-semibold tracking-tight text-text-high font-display">
              {currentPage === 'functions'
                ? (functionsViewMode === 'hookManager' ? t('Hook Manager') : t('Functions'))
                : ([...sidebarNavItems, ...exportItems].find(i => i.id === currentPage)?.label || t('Overview'))}
            </span>
          </div>

          {/* Search Bar */}
          <div className="relative group flex items-center">
            <div className="absolute left-2.5 flex items-center justify-center pointer-events-none">
              <Search className="w-3.5 h-3.5 text-text-low group-focus-within:text-text-mid transition-colors" />
            </div>
            <input
              className="bg-background-base border border-border-subtle focus:border-primary text-text-high text-xs rounded px-8 py-1 w-[240px] outline-none transition-all placeholder:text-text-low font-mono"
              placeholder={t('Search...')}
              type="text"
            />
            <div className="absolute right-2 flex gap-0.5 items-center pointer-events-none">
              <Command className="w-3 h-3 text-text-low" />
              <span className="text-[10px] font-medium text-text-low font-mono">K</span>
            </div>
          </div>
        </header>

        {/* Scrollable Page Content */}
        {renderPage()}
      </main>
    </div>
  );
}

export default App;
