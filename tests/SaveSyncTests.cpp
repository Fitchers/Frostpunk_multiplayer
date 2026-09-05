#define NOMINMAX
#include "../src/FrostBridgeNet/SaveSync.h"
#include <iostream>
#include <queue>

void check(bool value,const char* text) { if(!value) throw std::runtime_error(text); }
struct Mailbox {
    HANDLE mapping; frostsave::Control* control;
    explicit Mailbox(DWORD pid) {
        mapping=OpenFileMappingW(FILE_MAP_ALL_ACCESS,FALSE,frostsave::name(pid).c_str());
        check(mapping!=nullptr,"No save IPC");
        control=static_cast<frostsave::Control*>(MapViewOfFile(mapping,FILE_MAP_ALL_ACCESS,0,0,sizeof(frostsave::Control)));
        control->modReady=1;
    }
    ~Mailbox(){UnmapViewOfFile(control);CloseHandle(mapping);}
};
int main() {
 try {
    using namespace frostsave;
    check(slotName(L"survival")==L"survival_multiplayer","suffix");
    check(slotName(L"survival_multiplayer")==L"survival_multiplayer","double suffix");
    check(slotName(L"Зима")==L"Зима_multiplayer","unicode");
    check(!slotName(L"../x") && !slotName(L"C:\\x") && !slotName(L"x:y") && !slotName(L"") && !slotName(std::wstring(96,L'a')),"unsafe name");
    auto root=std::filesystem::current_path()/L"artifacts"/L"save-tests"/std::to_wstring(GetTickCount64());
    std::filesystem::create_directories(root);
    DWORD first=GetCurrentProcessId()+2000000000U,second=first+1;
    Sync host(first,"Host",true,root),guest(second,"Guest",false,root);
    Mailbox h(first),g(second);
    std::queue<Packet> toHost,toGuest;
    host.send=[&](const Packet& p){toGuest.push(p);};guest.send=[&](const Packet& p){toHost.push(p);};
    host.log=guest.log=[](const std::string& s){std::cout<<s<<'\n';};
    bool kicked=false;host.kick=guest.kick=[&]{kicked=true;};
    State hs{true,true,1},gs{true,true,1};
    host.state=[&]{return hs;};guest.state=[&]{return gs;};
    host.trading=guest.trading=[]{return false;};
    host.connected(true);guest.connected(true);
    auto pump=[&] {
        auto end=GetTickCount64()+8000;
        do {
            while(!toGuest.empty()) { auto p=toGuest.front();toGuest.pop();guest.receive(p); }
            while(!toHost.empty()) { auto p=toHost.front();toHost.pop();host.receive(p); }
            for(auto pair:{std::pair{h.control,&hs},std::pair{g.control,&gs}}) {
                auto c=pair.first;
                if(c->commandSequence!=c->ackSequence) {
                    if(c->commandOperation==save) {
                        std::ofstream file(root/(std::wstring(c->commandName)+L".save"),std::ios::binary);
                        file<<(c==h.control?"host city":"guest city");
                    } else ++pair.second->generation;
                    c->result=1;c->ackSequence=c->commandSequence;
                }
            }
            host.poll();guest.poll();Sleep(10);
        } while((host.active() || guest.active() || !toHost.empty() || !toGuest.empty()) && GetTickCount64()<end);
        check(!host.active() && !guest.active(),"checkpoint stuck");
    };
    host.request(save,L"survival");pump();
    check(std::wstring(h.control->commandName)==L"survival_multiplayer" &&
          std::wstring(g.control->commandName)==L"survival_multiplayer","engine must receive the public name, never fb_ID");
    check(std::filesystem::exists(root/L"survival_multiplayer.save"),"public native filename missing");
    Store hostStore(root,"Host"),guestStore(root,"Guest");
    auto a=hostStore.read(L"survival_multiplayer"),b=guestStore.read(L"survival_multiplayer");
    check(a.checkpoint==b.checkpoint && a.digest!=b.digest,"independent cities/checkpoint identity");
    auto before=h.control->commandSequence;
    host.request(save,L"survival");pump();
    check(before==h.control->commandSequence,"existing save overwritten");
    host.request(save,L"777");pump();
    host.request(save,L"333");pump();
    check(std::wstring(h.control->commandName)==L"333_multiplayer" &&
          std::wstring(g.control->commandName)==L"333_multiplayer","second name must replace first in both engines");
    check(hostStore.read(L"777_multiplayer").checkpoint!=hostStore.read(L"333_multiplayer").checkpoint,
          "different names must keep distinct checkpoints");
    host.request(load,L"survival");pump();
    check(hs.generation==2 && gs.generation==2 && !kicked,"both cities auto loaded");
    // Missing peer file is simulated by renaming ONLY this test's archive.
    std::filesystem::rename(guestStore.folder(L"survival_multiplayer"),guestStore.folder(L"missing_multiplayer"));
    host.request(load,L"survival");pump();
    check(kicked && hs.generation==2 && gs.generation==2,"missing save must reject before native load");
    std::cout<<"PASS: naming, Unicode, traversal rejection, independent cities, two-phase save, no overwrite, auto-load, missing-peer rejection.\n";
 } catch(const std::exception& e) { std::cerr<<e.what()<<'\n';return 1; }
}
