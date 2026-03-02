import { useState } from 'react';
import type { Page } from './types';
import Dashboard from './pages/Dashboard';

// Placeholder pages
function PlaceholderPage({ title }: { title: string }) {
  return (
    <div className="flex-1 flex items-center justify-center">
      <div className="text-center">
        <span className="material-symbols-outlined text-[64px] text-white/20">construction</span>
        <h2 className="text-xl font-medium text-white mt-4">{title}</h2>
        <p className="text-white/40 text-sm mt-2">This page is under construction</p>
      </div>
    </div>
  );
}

function App() {
  const [currentPage, setCurrentPage] = useState<Page>('dashboard');

  const sidebarNavItems: { id: Page; label: string; icon: string }[] = [
    { id: 'objects', label: 'Objects', icon: 'data_object' },
    { id: 'functions', label: 'Functions', icon: 'functions' },
    { id: 'memory', label: 'Memory', icon: 'memory' },
  ];

  const sidebarExportItems = [
    { label: 'Cheat Engine (.lua)', icon: 'description' },
    { label: 'IDA Python (.py)', icon: 'code' },
    { label: 'JSON Dump', icon: 'javascript' },
  ];

  const renderPage = () => {
    switch (currentPage) {
      case 'dashboard':
        return <Dashboard onNavigate={(page) => setCurrentPage(page)} />;
      case 'objects':
        return <PlaceholderPage title="Object Browser" />;
      case 'functions':
        return <PlaceholderPage title="Function Browser" />;
      case 'memory':
        return <PlaceholderPage title="Memory Tools" />;
      case 'sdkdump':
        return <PlaceholderPage title="SDK Dump" />;
      case 'settings':
        return <PlaceholderPage title="Settings" />;
      default:
        return <Dashboard onNavigate={(page) => setCurrentPage(page)} />;
    }
  };

  return (
    <div className="h-screen flex flex-col bg-[#1e1e1e] text-white overflow-hidden">
      {/* macOS-style Title Bar / Header */}
      <header className="h-14 flex-none bg-[#1e1e1e]/95 border-b border-white/10 flex items-center justify-between px-6 z-50">
        <div className="flex items-center gap-6">
          {/* Window Controls Simulation */}
          <div className="flex gap-2 mr-2 group">
            <div className="w-3 h-3 rounded-full bg-[#ff5f57] border border-[#e0443e]"></div>
            <div className="w-3 h-3 rounded-full bg-[#febc2e] border border-[#d89e24]"></div>
            <div className="w-3 h-3 rounded-full bg-[#28c840] border border-[#1aab29]"></div>
          </div>
          <div className="h-6 w-px bg-white/10 mx-2"></div>
          <div className="flex items-center gap-3">
            <div className="w-8 h-8 rounded-lg bg-gradient-to-br from-primary to-blue-600 flex items-center justify-center text-white shadow-lg shadow-blue-500/20">
              <span className="material-symbols-outlined text-[20px]">terminal</span>
            </div>
            <h1 className="font-semibold text-sm tracking-wide text-white/90">
              UExplore <span className="text-white/40 font-normal">Dumpspace</span>
            </h1>
          </div>
          {/* Navigation Pills */}
          <nav className="flex bg-black/20 p-1 rounded-lg ml-8 backdrop-blur-md">
            <button
              onClick={() => setCurrentPage('dashboard')}
              className={`px-4 py-1 text-xs font-medium transition-colors rounded-md ${
                currentPage === 'dashboard'
                  ? 'bg-white/10 text-white shadow-sm'
                  : 'text-white/60 hover:text-white'
              }`}
            >
              Explorer
            </button>
            <button
              onClick={() => setCurrentPage('sdkdump')}
              className={`px-4 py-1 text-xs font-medium transition-colors rounded-md ${
                currentPage === 'sdkdump'
                  ? 'bg-white/10 text-white shadow-sm'
                  : 'text-white/60 hover:text-white'
              }`}
            >
              Dumpspace
            </button>
            <button className="px-4 py-1 text-xs font-medium text-white/60 hover:text-white transition-colors rounded-md">
              Class Viewer
            </button>
          </nav>
        </div>
        <div className="flex items-center gap-4">
          {/* Search Bar */}
          <div className="relative group">
            <span className="material-symbols-outlined absolute left-2.5 top-1.5 text-white/40 text-[18px]">search</span>
            <input
              className="bg-black/20 border border-transparent focus:border-primary/50 text-white text-xs rounded-md pl-9 pr-3 py-1.5 w-64 outline-none transition-all placeholder:text-white/20 hover:bg-black/30 focus:bg-black/40"
              placeholder="Search global symbols..."
              type="text"
            />
            <div className="absolute right-2 top-1.5 flex gap-1">
              <kbd className="hidden group-focus-within:inline-flex h-4 items-center gap-1 rounded border border-white/10 bg-white/5 px-1.5 font-mono text-[10px] font-medium text-white/50">⌘K</kbd>
            </div>
          </div>
          <div className="h-6 w-px bg-white/10"></div>
          <button className="flex items-center gap-2 hover:bg-white/5 py-1 px-2 rounded-md transition-colors">
            <div className="w-6 h-6 rounded-full bg-gradient-to-tr from-purple-500 to-indigo-500 shadow-inner"></div>
            <span className="text-xs font-medium text-white/80">DevUser</span>
          </button>
        </div>
      </header>

      {/* Main Layout */}
      <div className="flex flex-1 overflow-hidden">
        {/* Sidebar Navigation (macOS styled) */}
        <aside className="w-64 bg-[#1e1e1e]/90 flex flex-col border-r border-white/5 backdrop-blur-xl">
          <div className="p-4">
            <div className="text-[11px] font-bold text-white/40 uppercase tracking-wider mb-2 px-3">Metadata</div>
            <nav className="space-y-0.5">
              {sidebarNavItems.map((item) => (
                <button
                  key={item.id}
                  onClick={() => setCurrentPage(item.id)}
                  className={`w-full flex items-center gap-3 px-3 py-2 rounded-md transition-all group ${
                    currentPage === item.id
                      ? 'bg-primary text-white shadow-sm'
                      : 'text-white/60 hover:text-white hover:bg-white/5'
                  }`}
                >
                  <span className="material-symbols-outlined text-[18px]">{item.icon}</span>
                  <span className="text-sm font-medium">{item.label}</span>
                </button>
              ))}
            </nav>
            <div className="mt-8 text-[11px] font-bold text-white/40 uppercase tracking-wider mb-2 px-3">Export Center</div>
            <nav className="space-y-0.5">
              {sidebarExportItems.map((item, index) => (
                <button
                  key={index}
                  onClick={() => setCurrentPage('sdkdump')}
                  className="w-full flex items-center gap-3 px-3 py-2 rounded-md text-white/60 hover:text-white hover:bg-white/5 transition-all group"
                >
                  <span className="material-symbols-outlined text-[18px]">{item.icon}</span>
                  <span className="text-sm font-medium">{item.label}</span>
                </button>
              ))}
            </nav>
          </div>
          <div className="mt-auto p-4 border-t border-white/5">
            <div className="bg-gradient-to-br from-gray-800 to-gray-900 rounded-lg p-4 border border-white/5 shadow-inner relative overflow-hidden group cursor-pointer">
              <div className="absolute inset-0 bg-primary/10 opacity-0 group-hover:opacity-100 transition-opacity"></div>
              <div className="flex items-center justify-between mb-2 relative z-10">
                <span className="text-xs font-semibold text-white/80">Export Queue</span>
                <span className="text-[10px] bg-primary/20 text-primary px-1.5 rounded">Idle</span>
              </div>
              <div className="w-full bg-white/10 rounded-full h-1.5 mb-1 relative z-10">
                <div className="bg-primary h-1.5 rounded-full w-[0%]"></div>
              </div>
              <div className="flex justify-between text-[10px] text-white/40 relative z-10">
                <span>0 items</span>
                <span>Ready</span>
              </div>
            </div>
          </div>
        </aside>

        {/* Main Content Area */}
        <main className="flex-1 flex flex-col min-w-0 bg-[#1e1e1e] overflow-hidden">
          {renderPage()}
        </main>
      </div>
    </div>
  );
}

export default App;
