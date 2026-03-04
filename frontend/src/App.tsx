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
        return <Objects onNavigate={setCurrentPage} />;
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
    <div className="h-screen flex bg-black text-white font-sans overflow-hidden selection:bg-primary/30 selection:text-white">

      {/* Sidebar - macOS Style Frosted Glass */}
      <aside className="w-64 flex flex-col apple-glass border-r border-white/5 z-20 relative shadow-2xl">
        {/* macOS Window Controls */}
        <div className="h-14 flex items-center px-4 gap-2 mb-2 window-drag-region">
          <div className="flex gap-2 group">
            <div className="w-3 h-3 rounded-full bg-white/20 group-hover:bg-[#FF5F56] transition-colors border border-black/10"></div>
            <div className="w-3 h-3 rounded-full bg-white/20 group-hover:bg-[#FFBD2E] transition-colors border border-black/10"></div>
            <div className="w-3 h-3 rounded-full bg-white/20 group-hover:bg-[#27C93F] transition-colors border border-black/10"></div>
          </div>
        </div>

        {/* Sidebar Content */}
        <div className="flex-1 overflow-y-auto px-3 pb-4 space-y-6">
          <div className="px-2">
            <h1 className="text-[11px] font-bold text-white/30 uppercase tracking-widest mb-3">UExplorer</h1>
          </div>

          <div className="space-y-1">
            {sidebarNavItems.map((item) => (
              <button
                key={item.key}
                onClick={() => {
                  setCurrentPage(item.id);
                  if (item.id === 'functions' && item.mode) {
                    setFunctionsViewMode(item.mode);
                  }
                }}
                className={`w-full flex items-center gap-3 px-3 py-2 rounded-xl transition-all duration-200 cursor-pointer ${
                  currentPage === item.id && (item.id !== 'functions' || functionsViewMode === item.mode)
                    ? 'bg-primary text-white shadow-md shadow-primary/20'
                    : 'text-white/60 hover:bg-white/10 hover:text-white'
                  }`}
              >
                <item.icon className={`w-[18px] h-[18px] stroke-[2] ${
                  currentPage === item.id && (item.id !== 'functions' || functionsViewMode === item.mode)
                    ? 'text-white'
                    : 'text-white/50'
                }`} />
                <span className="text-[13px] font-medium tracking-tight">{item.label}</span>
              </button>
            ))}
          </div>

          <div className="space-y-1">
            <div className="px-2 mb-2 pt-2">
              <span className="text-[11px] font-bold text-white/30 uppercase tracking-widest">System</span>
            </div>
            {exportItems.map((item) => (
              <button
                key={item.key}
                onClick={() => setCurrentPage(item.id)}
                className={`w-full flex items-center gap-3 px-3 py-2 rounded-xl transition-all duration-200 cursor-pointer ${currentPage === item.id
                    ? 'bg-primary text-white shadow-md shadow-primary/20'
                    : 'text-white/60 hover:bg-white/10 hover:text-white'
                  }`}
              >
                <item.icon className={`w-[18px] h-[18px] stroke-[2] ${currentPage === item.id ? 'text-white' : 'text-white/50'}`} />
                <span className="text-[13px] font-medium tracking-tight">{item.label}</span>
              </button>
            ))}
          </div>
        </div>

        {/* User / Dev Profile Area at bottom */}
        <div className="p-4 border-t border-white/5">
          <button className="w-full flex items-center gap-3 px-2 py-2 rounded-[14px] hover:bg-white/5 transition-colors cursor-pointer group">
            <div className="w-9 h-9 rounded-full bg-gradient-to-tr from-blue-600 to-indigo-500 shadow-inner flex items-center justify-center border border-white/10">
              <span className="text-[13px] font-semibold text-white">U</span>
            </div>
            <div className="flex flex-col items-start flex-1 min-w-0">
              <span className="text-[13px] font-medium text-white/90 truncate tracking-tight">DevUser</span>
              <span className="text-[11px] text-white/40 truncate">Local Agent</span>
            </div>
          </button>
        </div>
      </aside>

      {/* Main Content Area */}
      <main className="flex-1 flex flex-col min-w-0 bg-[#000000] relative z-10">
        {/* Top Header */}
        <header className="h-14 flex-none apple-glass border-b border-white/5 flex items-center justify-between px-8 sticky top-0 z-30">
          <div className="flex items-center gap-4 text-white/60">
            {/* View Title */}
            <span className="text-[14px] font-semibold tracking-tight text-white/90">
              {currentPage === 'functions'
                ? (functionsViewMode === 'hookManager' ? t('Hook Manager') : t('Functions'))
                : ([...sidebarNavItems, ...exportItems].find(i => i.id === currentPage)?.label || t('Overview'))}
            </span>
          </div>

          {/* Search Bar matching macOS Spotlight vibe */}
          <div className="relative group flex items-center">
            <div className="absolute left-3 flex items-center justify-center pointer-events-none">
              <Search className="w-3.5 h-3.5 text-white/40 group-focus-within:text-white/70 transition-colors" />
            </div>
            <input
              className="bg-white/5 border border-white/10 focus:border-white/20 focus:bg-white/10 text-white text-[13px] rounded-lg pl-9 pr-12 py-1.5 w-[280px] outline-none transition-all placeholder:text-white/30 font-medium"
              placeholder={t('Search...')}
              type="text"
            />
            <div className="absolute right-2 flex gap-1 items-center pointer-events-none">
              <Command className="w-3 h-3 text-white/40" />
              <span className="text-[10px] font-medium text-white/40 font-mono">K</span>
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
