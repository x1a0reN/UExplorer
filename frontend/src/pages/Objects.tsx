import { useState } from 'react';
import { Search, Filter, Box, Database, Layers, Hash, Info, ListTree, PackageSearch, Globe } from 'lucide-react';

export default function Objects() {
    const [activeTab, setActiveTab] = useState('Info');

    const objectTabs = [
        { id: 'Info', icon: Info },
        { id: 'Properties', icon: ListTree },
        { id: 'Fields', icon: Database },
        { id: 'Functions', icon: Hash },
        { id: 'Instances', icon: PackageSearch },
        { id: 'World', icon: Globe },
    ];

    return (
        <div className="flex-1 flex overflow-hidden">
            {/* Left Sidebar - Object List (Finder Style) */}
            <div className="w-[320px] flex-none border-r border-white/5 flex flex-col bg-black/40 relative z-10 backdrop-blur-md">

                {/* Search Header */}
                <div className="p-4 border-b border-white/5 space-y-3">
                    <div className="relative group">
                        <Search className="w-4 h-4 text-white/40 absolute left-3 top-2.5" />
                        <input
                            type="text"
                            placeholder="Search Objects..."
                            className="w-full bg-white/5 border border-white/10 text-white text-[13px] rounded-lg pl-9 pr-3 py-2 outline-none focus:border-primary/50 focus:bg-white/10 transition-all font-medium placeholder:text-white/30"
                        />
                    </div>

                    <div className="flex items-center justify-between">
                        <div className="flex gap-1 bg-white/5 p-1 rounded-[8px] border border-white/5">
                            <button className="px-2.5 py-1 rounded-[6px] bg-white/10 text-white text-[11px] font-semibold tracking-tight shadow-sm">All</button>
                            <button className="px-2.5 py-1 rounded-[6px] text-white/50 hover:text-white text-[11px] font-semibold tracking-tight transition-colors">Class</button>
                            <button className="px-2.5 py-1 rounded-[6px] text-white/50 hover:text-white text-[11px] font-semibold tracking-tight transition-colors">Struct</button>
                            <button className="px-2.5 py-1 rounded-[6px] text-white/50 hover:text-white text-[11px] font-semibold tracking-tight transition-colors">Enum</button>
                        </div>
                        <button className="w-7 h-7 flex items-center justify-center rounded-[8px] hover:bg-white/10 text-white/50 hover:text-white transition-colors">
                            <Filter className="w-3.5 h-3.5" />
                        </button>
                    </div>
                </div>

                {/* Virtual List Area */}
                <div className="flex-1 overflow-y-auto p-2 space-y-1 custom-scrollbar">
                    {/* Dummy Items to show style */}
                    {[...Array(20)].map((_, i) => (
                        <div key={i} className={`flex items-center gap-3 px-3 py-2 rounded-[10px] cursor-pointer transition-colors ${i === 0 ? 'bg-primary text-white shadow-sm' : 'hover:bg-white/5 text-white/70'}`}>
                            <div className={`w-8 h-8 rounded-[8px] flex items-center justify-center ${i === 0 ? 'bg-white/10' : 'bg-white/5'} flex-none`}>
                                {i % 3 === 0 ? <Box className="w-4 h-4" /> : i % 3 === 1 ? <Database className="w-4 h-4" /> : <Layers className="w-4 h-4" />}
                            </div>
                            <div className="flex-1 min-w-0">
                                <div className={`text-[13px] font-semibold truncate ${i === 0 ? 'text-white' : 'text-white/90'}`}>UObj_ActorBase_C</div>
                                <div className={`text-[11px] font-mono truncate ${i === 0 ? 'text-white/70' : 'text-white/40'}`}>0x0000021A4B00000</div>
                            </div>
                        </div>
                    ))}
                </div>

                {/* Footer Stats */}
                <div className="p-3 border-t border-white/5 bg-black/40 flex justify-between items-center text-[10px] font-medium text-white/40">
                    <span>134,206 Objects</span>
                    <span>50 matches</span>
                </div>
            </div>

            {/* Right Content Area - Details */}
            <div className="flex-1 flex flex-col min-w-0 bg-[#0A0A0C] relative">
                {/* Top Apple Tab Bar */}
                <div className="h-14 border-b border-white/5 bg-white/[0.02] backdrop-blur-3xl flex items-center px-6 gap-6 z-20">
                    <nav className="flex items-center gap-6">
                        {objectTabs.map(tab => (
                            <button
                                key={tab.id}
                                onClick={() => setActiveTab(tab.id)}
                                className={`relative h-14 flex items-center gap-2 text-[13px] font-semibold tracking-tight transition-colors ${activeTab === tab.id ? 'text-white' : 'text-white/40 hover:text-white/70'}`}
                            >
                                <tab.icon className="w-4 h-4" />
                                {tab.id}
                                {activeTab === tab.id && (
                                    <div className="absolute bottom-0 left-0 right-0 h-[3px] bg-primary rounded-t-full shadow-[0_-2px_8px_rgba(10,132,255,0.5)]"></div>
                                )}
                            </button>
                        ))}
                    </nav>
                </div>

                {/* Content View */}
                <div className="flex-1 overflow-auto p-8 relative">
                    <div className="max-w-4xl space-y-6">
                        {/* Hero Info Box */}
                        <div className="apple-glass-panel rounded-[24px] p-6 relative overflow-hidden">
                            <div className="absolute right-0 top-0 w-32 h-32 bg-primary/20 blur-[60px] rounded-full pointer-events-none -mt-10 -mr-10"></div>
                            <div className="flex gap-6 relative z-10">
                                <div className="w-20 h-20 rounded-[16px] bg-gradient-to-br from-blue-500/20 to-indigo-500/10 border border-white/10 flex items-center justify-center shadow-lg flex-none">
                                    <Box className="w-10 h-10 text-blue-400 stroke-[1.5]" />
                                </div>
                                <div className="flex-1">
                                    <h1 className="text-[24px] font-semibold text-white tracking-tight leading-tight mb-1">UObj_ActorBase_C</h1>
                                    <p className="text-[13px] text-white/50 font-mono mb-4">/Game/Core/Blueprints/UObj_ActorBase_C</p>

                                    <div className="flex flex-wrap gap-2">
                                        <span className="px-2.5 py-1 rounded-[6px] bg-white/5 border border-white/10 text-[11px] font-mono text-white/60">Class: UBlueprintGeneratedClass</span>
                                        <span className="px-2.5 py-1 rounded-[6px] bg-green-500/10 border border-green-500/20 text-[11px] font-mono text-green-400">Index: 12450</span>
                                        <span className="px-2.5 py-1 rounded-[6px] bg-purple-500/10 border border-purple-500/20 text-[11px] font-mono text-purple-400">Size: 0x2A0</span>
                                    </div>
                                </div>
                            </div>
                        </div>

                        {/* Mock Properties Table */}
                        {activeTab === 'Info' && (
                            <div className="apple-glass-panel rounded-[24px] overflow-hidden">
                                <div className="px-6 py-4 border-b border-white/5 bg-white/[0.02]">
                                    <h3 className="text-[14px] font-semibold text-white/90">Detailed Information</h3>
                                </div>
                                <div className="p-2">
                                    <table className="w-full text-left border-collapse">
                                        <tbody>
                                            {[
                                                { key: 'Memory Address', value: '0x0000021A4B00000' },
                                                { key: 'Outer Chain', value: 'Package /Game/Core/Blueprints' },
                                                { key: 'Super Class', value: 'AActor (0x0000021A48E0000)' },
                                                { key: 'Object Flags', value: 'RF_Public | RF_Standalone | RF_WasLoaded' },
                                                { key: 'Internal Index', value: '12450' }
                                            ].map((row, idx) => (
                                                <tr key={idx} className="hover:bg-white/5 transition-colors group">
                                                    <td className="w-1/3 px-4 py-3 text-[13px] font-medium text-white/50 border-b border-white/5 group-last:border-0">{row.key}</td>
                                                    <td className="px-4 py-3 text-[13px] font-mono text-white/80 border-b border-white/5 group-last:border-0">{row.value}</td>
                                                </tr>
                                            ))}
                                        </tbody>
                                    </table>
                                </div>
                            </div>
                        )}
                    </div>
                </div>
            </div>
        </div>
    );
}
