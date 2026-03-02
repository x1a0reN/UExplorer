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

  const navItems: { id: Page; label: string; icon: string }[] = [
    { id: 'dashboard', label: 'Dashboard', icon: 'dashboard' },
    { id: 'objects', label: 'Objects', icon: 'data_object' },
    { id: 'functions', label: 'Functions', icon: 'functions' },
    { id: 'memory', label: 'Memory', icon: 'memory' },
    { id: 'sdkdump', label: 'SDK Dump', icon: 'download' },
    { id: 'settings', label: 'Settings', icon: 'settings' },
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
    <div className="h-screen flex bg-background-dark text-white overflow-hidden">
      {/* Sidebar */}
      <aside className="w-64 bg-[#1e1e1e]/90 flex flex-col border-r border-white/5 backdrop-blur-xl">
        {/* Logo */}
        <div className="h-14 flex items-center px-4 border-b border-white/5">
          <div className="flex items-center gap-3">
            <div className="w-8 h-8 rounded-lg bg-gradient-to-br from-primary to-blue-600 flex items-center justify-center text-white shadow-lg shadow-blue-500/20">
              <span className="material-symbols-outlined text-[20px]">terminal</span>
            </div>
            <h1 className="font-semibold text-sm tracking-wide text-white/90">
              UExplore
            </h1>
          </div>
        </div>

        {/* Navigation */}
        <nav className="flex-1 p-4">
          <div className="space-y-1">
            {navItems.map((item) => (
              <button
                key={item.id}
                onClick={() => setCurrentPage(item.id)}
                className={`w-full flex items-center gap-3 px-3 py-2.5 rounded-lg transition-all ${
                  currentPage === item.id
                    ? 'bg-primary text-white shadow-sm'
                    : 'text-white/60 hover:text-white hover:bg-white/5'
                }`}
              >
                <span className="material-symbols-outlined text-[20px]">{item.icon}</span>
                <span className="text-sm font-medium">{item.label}</span>
              </button>
            ))}
          </div>
        </nav>

        {/* Footer */}
        <div className="p-4 border-t border-white/5">
          <div className="text-xs text-white/30 text-center">
            UExplorer v0.1.0
          </div>
        </div>
      </aside>

      {/* Main Content */}
      <main className="flex-1 flex flex-col min-w-0 bg-background-dark overflow-hidden">
        {renderPage()}
      </main>
    </div>
  );
}

export default App;
