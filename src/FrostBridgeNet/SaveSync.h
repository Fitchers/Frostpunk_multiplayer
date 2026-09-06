#pragma once
#include "SaveStore.h"
#include <functional>
#include <chrono>
#include <random>
#include <memory>
#include <cstring>

namespace frostsave {
#pragma pack(push,1)
struct Packet {
    std::uint64_t checkpoint=0;
    LONG operation=0;
    LONG phase=0; // prepare=1, ready=2, go=3, result=4, commit=5, fail=6, committed=7
    wchar_t slot[nameCapacity]{};
};
#pragma pack(pop)
static_assert(sizeof(Packet)==208);
struct State { bool loaded=false,paused=false; LONG generation=0; };

class Sync {
    using Clock=std::chrono::steady_clock;
public:
    std::function<void(const Packet&)> send;
    std::function<void(const std::string&)> log;
    std::function<void()> kick;
    std::function<State()> state;
    std::function<bool()> trading;
    Sync(DWORD pid,std::string player,bool host,std::filesystem::path root)
        : store_(std::move(root),std::move(player)),host_(host) {
        if(!pid) return;
        HANDLE map=CreateFileMappingW(INVALID_HANDLE_VALUE,nullptr,PAGE_READWRITE,0,sizeof(Control),name(pid).c_str());
        mapping_=map;
        if(!map) return;
        bool created=GetLastError()!=ERROR_ALREADY_EXISTS;
        control_=static_cast<Control*>(MapViewOfFile(map,FILE_MAP_ALL_ACCESS,0,0,sizeof(Control)));
        if(!control_) return;
        if(created) { ZeroMemory(control_,sizeof(*control_)); control_->layoutVersion=version; MemoryBarrier(); control_->signature=magic; }
        if(control_->signature!=magic || control_->layoutVersion!=version) { UnmapViewOfFile(control_);control_=nullptr;return; }
        event_=control_->eventSequence;
    }
    ~Sync() { if(control_) { InterlockedExchange(&control_->role,0); UnmapViewOfFile(control_); } if(mapping_) CloseHandle(mapping_); }
    bool active() const { return active_; }
    void setHost(bool value) { host_=value; }
    void connected(bool value) { if(control_) InterlockedExchange(&control_->role,value?(host_?1:2):0); }
    void request(LONG operation,const std::wstring& requested) {
        if(!host_) { log("[save] Only the host can save/load a multiplayer checkpoint."); return; }
        if(active_) { log("[save] A checkpoint operation is already running."); return; }
        try {
            auto slot=slotName(requested);
            if(!slot) throw std::runtime_error("Invalid save name (use at most 84 characters; no path separators)");
            Packet packet; packet.operation=operation; packet.phase=1;
            std::random_device random;
            packet.checkpoint=(static_cast<std::uint64_t>(random())<<32)^random();
            if(!packet.checkpoint) packet.checkpoint=1;
            wcsncpy_s(packet.slot,slot->c_str(),_TRUNCATE);
            if(operation==load) packet.checkpoint=store_.read(*slot).checkpoint;
            begin(packet); send(packet);
            log("[save] Waiting for both games; keeping the session paused.");
        } catch(const std::exception& e) { log(std::string("[save] ")+e.what()); }
    }
    void receive(const Packet& packet) {
        if(!packet.checkpoint || (packet.operation!=save && packet.operation!=load) ||
           packet.slot[nameCapacity-1]!=0) return;
        const auto normalized=slotName(packet.slot);
        if(!normalized || *normalized!=packet.slot) return;
        try {
            if(!host_ && packet.phase==1) {
                if(active_) {
                    if(packet.checkpoint==packet_.checkpoint && packet.operation==packet_.operation &&
                        wcscmp(packet.slot,packet_.slot)==0 && ready_) emit(2);
                    return;
                }
                try { begin(packet); }
                catch(const std::exception& error) {
                    Packet failed=packet; failed.phase=6; send(failed);
                    log(std::string("[save] Failed: ")+error.what());
                    if(packet.operation==load) kick();
                    return;
                }
            } else if(!host_ && !active_ && done_ && packet.phase==5 &&
                      packet.checkpoint==packet_.checkpoint && packet.operation==packet_.operation &&
                      wcscmp(packet.slot,packet_.slot)==0) {
                emit(7); // resend lost final acknowledgement without saving again
            } else if(active_ && packet.checkpoint==packet_.checkpoint &&
                      packet.operation==packet_.operation && wcscmp(packet.slot,packet_.slot)==0) {
                if(packet.phase==6) { fail("Peer could not complete the checkpoint operation.",false); return; }
                if(host_ && packet.phase==2 && !dispatched_ && !peerReady_) {
                    peerReady_=true;
                    log("[save] The client confirmed readiness for save/load.");
                }
                else if(!host_ && packet.phase==3 && ready_ && !dispatched_) goRequested_=true;
                else if(host_ && packet.phase==4 && dispatched_) peerDone_=true;
                else if(!host_ && packet.phase==5 && done_) {
                    if(packet_.operation==save) store_.commit(packet_.slot);
                    emit(7); active_=false;
                    log("[save] Checkpoint complete. Each player keeps their own city.");
                } else if(host_ && packet.phase==7 && commitSent_) {
                    if(packet_.operation==save) store_.commit(packet_.slot);
                    active_=false;
                    log("[save] Checkpoint complete on both players.");
                }
            }
        } catch(const std::exception& e) { fail(e.what()); }
    }
    void poll() {
        if(control_) InterlockedExchange64(&control_->heartbeat,static_cast<LONG64>(GetTickCount64()));
        if(control_ && control_->eventSequence!=event_) {
            event_=control_->eventSequence; MemoryBarrier();
            wchar_t slot[nameCapacity]{}; std::memcpy(slot,control_->eventName,sizeof(slot)); slot[nameCapacity-1]=0;
            request(control_->eventOperation,slot);
        }
        if(!active_) return;
        try {
            if(Clock::now()>deadline_) throw std::runtime_error(
                !ready_?"Timeout: local city has not paused or a resource transfer is pending":
                host_ && !peerReady_?"Timeout: client did not confirm readiness; check the client's save log":
                !dispatched_?"Timeout: waiting for the native save slot lock or host command":
                control_->ackSequence!=command_?"Timeout: game did not process the native save command":
                !done_?"Timeout: native save file did not appear or loading did not finish":
                "Timeout: peer did not finish its save/load");
            if(Clock::now()>=retryAt_) {
                retryAt_=Clock::now()+std::chrono::seconds(1);
                if(host_) {
                    if(!peerReady_) emit(1);
                    else if(commitSent_) emit(5);
                    else if(dispatched_ && !peerDone_) emit(3);
                } else {
                    if(done_) emit(4);
                    else if(ready_ && !dispatched_) emit(2);
                }
            }
            const auto current=state();
            if(!ready_ && !trading() && (!current.loaded || current.paused)) {
                ready_=true;
                log("[save] City ready. Waiting for the other player.");
                if(!host_) emit(2);
            }
            if(host_ && ready_ && peerReady_ && !dispatched_ && dispatch()) emit(3);
            if(!host_ && goRequested_ && !dispatched_) dispatch();
            if(dispatched_ && !done_ && control_->ackSequence==command_) {
                if(control_->result!=1) throw std::runtime_error("Game rejected the native save/load request");
                if(packet_.operation==save) {
                    if(!store_.nativeReady(packet_.checkpoint)) { stableSince_=Clock::now(); return; }
                    const auto size=std::filesystem::file_size(store_.nativeFile(packet_.checkpoint));
                    const auto modified=std::filesystem::last_write_time(store_.nativeFile(packet_.checkpoint));
                    if(size!=lastSize_ || modified!=lastModified_) {
                        lastSize_=size;lastModified_=modified;stableSince_=Clock::now();return;
                    }
                    if(Clock::now()-stableSince_<std::chrono::seconds(2)) return;
                    store_.prepare(packet_.slot,packet_.checkpoint); done_=true;
                } else done_=current.loaded && current.generation!=generation_;
                if(done_) store_.release();
                if(done_ && !host_) emit(4);
            }
            if(host_ && done_ && peerDone_ && !commitSent_) { commitSent_=true;emit(5); }
        } catch(const std::exception& e) { fail(e.what()); }
    }
private:
    void begin(const Packet& packet) {
        if(!control_ || !control_->modReady) throw std::runtime_error("Restart Frostpunk with the save-enabled DLL");
        if(packet.operation==save) {
            if(!state().loaded) throw std::runtime_error("Load your city before saving");
            // Fresh profiles have no saves directory before the first save.
            std::filesystem::create_directories(store_.root());
            if(store_.exists(packet.slot)) throw std::runtime_error("Name already used: choose a new save name (no silent overwrite)");
        } else {
            if(!std::filesystem::is_directory(store_.root())) throw std::runtime_error("Native save directory is unavailable");
            const auto record=store_.read(packet.slot);
            if(record.checkpoint!=packet.checkpoint) throw std::runtime_error("Save belongs to a different checkpoint");
        }
        store_.select(packet.slot);
        packet_=packet; active_=true;ready_=peerReady_=dispatched_=done_=peerDone_=commitSent_=goRequested_=false;
        log("[save] Request received; waiting for the city to pause.");
        deadline_=Clock::now()+std::chrono::seconds(120);
        retryAt_=Clock::now()+std::chrono::seconds(1);
    }
    void emit(LONG phase) { Packet p=packet_;p.phase=phase;send(p); }
    bool dispatch() {
        if(!store_.acquire()) return false;
        if(packet_.operation==save) store_.makeRoom(packet_.checkpoint);
        else store_.restore(packet_.slot,store_.read(packet_.slot));
        generation_=state().generation;
        control_->commandOperation=packet_.operation;
        wcsncpy_s(control_->commandName,store_.scratch(packet_.checkpoint).c_str(),_TRUNCATE);
        MemoryBarrier(); command_=InterlockedIncrement(&control_->commandSequence);
        dispatched_=true;stableSince_=Clock::now();lastSize_=0;
        log("[save] Command delivered to the game. Waiting for the save file or loaded city.");
        return true;
    }
    void fail(const std::string& reason,bool notify=true) {
        if(notify && active_) emit(6);
        log("[save] Failed: "+reason);active_=false;
        store_.release();
        if(packet_.operation==load) kick();
    }
    Store store_;
    Control* control_=nullptr;
    HANDLE mapping_=nullptr;
    bool host_=false,active_=false,ready_=false,peerReady_=false,dispatched_=false,done_=false,peerDone_=false,commitSent_=false,goRequested_=false;
    Packet packet_{};
    LONG event_=0,command_=0,generation_=0;
    Clock::time_point deadline_{},stableSince_{},retryAt_{};
    std::uintmax_t lastSize_=0;
    std::filesystem::file_time_type lastModified_{};
};
}
