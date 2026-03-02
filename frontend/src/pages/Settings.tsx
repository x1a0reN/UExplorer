import { useState } from 'react';
import { Settings, Globe, Shield, HardDrive, Monitor } from 'lucide-react';

export default function SettingsView() {
    const [activeTab, setActiveTab] = useState('Connection');

    const tabs = [
        { id: 'Connection', icon: Globe },
        { id: 'Injection', icon: Shield },
        { id: 'Dump', icon: HardDrive },
        { id: 'Display', icon: Monitor },
    ];

    return (
        <div className="flex-1 flex overflow-hidden bg-[#0A0A0C]">
            {/* Settings Navigation (macOS style left pane) */}
            <div className="w-[280px] flex-none border-r border-white/5 bg-white/[0.02] flex flex-col z-10">
                <div className="h-14 flex items-center px-6 border-b border-transparent">
                    {/* Empty space balancing the header */}
                </div>
                <div className="p-4 space-y-1">
                    {tabs.map(tab => (
                        <button
                            key={tab.id}
                            onClick={() => setActiveTab(tab.id)}
                            className={`w-full flex items-center gap-3 px-3 py-2 rounded-xl transition-all duration-200 cursor-pointer ${activeTab === tab.id
                                ? 'bg-primary text-white shadow-md shadow-primary/20'
                                : 'text-white/60 hover:bg-white/10 hover:text-white'
                                }`}
                        >
                            <div className={`w-7 h-7 rounded-[8px] flex items-center justify-center ${activeTab === tab.id ? 'bg-white/20' : 'bg-white/5'}`}>
                                <tab.icon className={`w-3.5 h-3.5 stroke-[2] ${activeTab === tab.id ? 'text-white' : 'text-white/50'}`} />
                            </div>
                            <span className="text-[13px] font-medium tracking-tight">{tab.id}</span>
                        </button>
                    ))}
                </div>
            </div>

            {/* Settings Content Area */}
            <div className="flex-1 overflow-y-auto w-full max-w-3xl px-12 py-8">
                <div className="mb-8">
                    <h1 className="text-[28px] font-semibold text-white tracking-tight">{activeTab}</h1>
                </div>

                <div className="space-y-6">
                    {activeTab === 'Connection' && (
                        <>
                            <div className="apple-glass-panel rounded-[16px] overflow-hidden">
                                <div className="p-4 flex items-center justify-between border-b border-white/5 bg-white/[0.01]">
                                    <div>
                                        <div className="text-[14px] font-medium text-white/90">HTTP Port</div>
                                        <div className="text-[12px] text-white/40">Port used to communicate with the injected DLL.</div>
                                    </div>
                                    <input type="text" defaultValue="27015" className="bg-black/40 border border-white/10 text-white font-mono text-[13px] rounded-lg px-3 py-1.5 w-24 text-center outline-none focus:border-primary/50" />
                                </div>

                                <div className="p-4 flex items-center justify-between bg-white/[0.01]">
                                    <div>
                                        <div className="text-[14px] font-medium text-white/90">Access Token</div>
                                        <div className="text-[12px] text-white/40">Shared secret for API requests.</div>
                                    </div>
                                    <input type="text" defaultValue="uexplorer-dev" className="bg-black/40 border border-white/10 text-white font-mono text-[13px] rounded-lg px-3 py-1.5 w-40 outline-none focus:border-primary/50" />
                                </div>
                            </div>

                            <div className="apple-glass-panel rounded-[16px] overflow-hidden">
                                <div className="p-4 flex items-center justify-between bg-white/[0.01]">
                                    <div>
                                        <div className="text-[14px] font-medium text-white/90">Auto Reconnect</div>
                                        <div className="text-[12px] text-white/40">Constantly poll for the game process if disconnected.</div>
                                    </div>
                                    <label className="relative inline-flex items-center cursor-pointer">
                                        <input type="checkbox" defaultChecked className="sr-only peer" />
                                        <div className="w-11 h-6 bg-white/10 rounded-full peer peer-checked:bg-[#28C840] transition-colors after:content-[''] after:absolute after:top-[2px] after:left-[2px] after:bg-white after:rounded-full after:h-5 after:w-5 after:transition-all peer-checked:after:translate-x-full shadow-inner"></div>
                                    </label>
                                </div>
                            </div>
                        </>
                    )}

                    {activeTab === 'Injection' && (
                        <div className="apple-glass-panel rounded-[16px] overflow-hidden">
                            <div className="p-4 flex items-center justify-between border-b border-white/5 bg-white/[0.01]">
                                <div>
                                    <div className="text-[14px] font-medium text-white/90">DLL Path</div>
                                    <div className="text-[12px] text-white/40">Path to UExplorerCore.dll</div>
                                </div>
                                <button className="bg-white/5 hover:bg-white/10 border border-white/10 text-white text-[12px] font-medium rounded-lg px-4 py-1.5 transition-colors">Browse...</button>
                            </div>
                            <div className="p-4 flex items-center justify-between bg-white/[0.01]">
                                <div>
                                    <div className="text-[14px] font-medium text-white/90">Injection Method</div>
                                    <div className="text-[12px] text-white/40">RemoteThread or Proxied DLL Proxy</div>
                                </div>
                                <select className="bg-black/40 border border-white/10 text-white text-[13px] rounded-lg px-3 py-1.5 outline-none focus:border-primary/50">
                                    <option>CreateRemoteThread</option>
                                    <option>DLL Proxy (xinput1_3)</option>
                                </select>
                            </div>
                        </div>
                    )}

                    {/* Dummy Info */}
                    <div className="pt-8 flex justify-center">
                        <div className="text-center">
                            <div className="text-white/20 mb-2"><Settings className="w-8 h-8 mx-auto" /></div>
                            <div className="text-[11px] font-mono text-white/30 uppercase tracking-widest">UExplorer Pre-Release v0.1</div>
                        </div>
                    </div>
                </div>
            </div>
        </div>
    );
}
