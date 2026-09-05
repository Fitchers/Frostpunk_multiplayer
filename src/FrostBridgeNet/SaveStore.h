#pragma once
#include "../SaveControl.h"
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <cstdint>
#include <sstream>
#include <mutex>
#include <set>

namespace frostsave {
// Each player keeps a private archive. This also isolates two local test games.
// The native slot has the user-visible name. A profile/slot mutex serializes
// local instances; private archives retain each player's independent city.
inline std::uint64_t checksum(const std::filesystem::path& path) {
    std::ifstream in(path,std::ios::binary);
    if(!in) throw std::runtime_error("Cannot read save file");
    std::uint64_t hash=14695981039346656037ULL;
    char data[65536];
    while(in.read(data,sizeof(data)) || in.gcount())
        for(std::streamsize i=0;i<in.gcount();++i) { hash^=static_cast<unsigned char>(data[i]); hash*=1099511628211ULL; }
    if(!in.eof()) throw std::runtime_error("Save read failed");
    return hash;
}
inline std::wstring playerKey(const std::string& player) {
    std::uint64_t hash=14695981039346656037ULL;
    for(unsigned char ch:player) { hash^=ch; hash*=1099511628211ULL; }
    std::wostringstream out; out<<std::hex<<hash; return out.str();
}
struct Record {
    std::uint64_t magic=0x31564153425046ULL;
    std::uint64_t checkpoint=0;
    std::uint64_t bytes=0;
    std::uint64_t digest=0; // accidental-corruption check, not authentication
};
class Store {
public:
    Store(std::filesystem::path nativeRoot,std::string player)
        : native_(std::move(nativeRoot)),key_(playerKey(player)),archive_(native_/L"FrostBridge"/key_) {}
    const std::filesystem::path& root() const { return native_; }
    ~Store() { release(); }
    Store(const Store&)=delete;
    Store& operator=(const Store&)=delete;
    void select(const std::wstring& slot) { folder(slot); slot_=slot; }
    std::wstring scratch(std::uint64_t id) const {
        (void)id;
        if(slot_.empty()) throw std::runtime_error("No selected native save slot");
        return slot_;
    }
    bool acquire() {
        if(locked_) return true;
        std::lock_guard guard(localMutex_);
        const auto identity=native_.wstring()+L"/"+slot_;
        if(localOwners_.contains(identity)) return false;
        if(!mutex_) {
            std::string bytes(reinterpret_cast<const char*>(identity.data()),identity.size()*sizeof(wchar_t));
            const auto mutexName=L"Local\\FrostBridgeSaveSlot-"+playerKey(bytes);
            mutex_=CreateMutexW(nullptr,FALSE,mutexName.c_str());
            if(!mutex_) throw std::runtime_error("Cannot lock native save slot");
        }
        const DWORD wait=WaitForSingleObject(mutex_,0);
        locked_=wait==WAIT_OBJECT_0 || wait==WAIT_ABANDONED;
        if(locked_) localOwners_.insert(identity);
        if(wait==WAIT_FAILED) throw std::runtime_error("Cannot acquire native save slot");
        return locked_;
    }
    void release() {
        std::lock_guard guard(localMutex_);
        if(locked_) { localOwners_.erase(native_.wstring()+L"/"+slot_);ReleaseMutex(mutex_); }
        locked_=false;
        if(mutex_) CloseHandle(mutex_);
        mutex_=nullptr;
    }
    std::filesystem::path nativeFile(std::uint64_t id) const { return native_/(scratch(id)+L".save"); }
    std::filesystem::path folder(const std::wstring& slot) const {
        auto valid=slotName(slot);
        if(!valid || *valid!=slot) throw std::runtime_error("Invalid multiplayer slot");
        return archive_/slot;
    }
    bool exists(const std::wstring& slot) const { return std::filesystem::exists(folder(slot)); }
    bool nativeReady(std::uint64_t id) const {
        HANDLE file=CreateFileW(nativeFile(id).c_str(),GENERIC_READ,0,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(file==INVALID_HANDLE_VALUE) return false;
        LARGE_INTEGER size{}; bool valid=GetFileSizeEx(file,&size) && size.QuadPart>0;
        CloseHandle(file); return valid;
    }
    void makeRoom(std::uint64_t id) {
        auto file=nativeFile(id);
        if(!std::filesystem::exists(file)) return;
        Record owner{};
        std::ifstream in(alias(),std::ios::binary);
        if(!in.read(reinterpret_cast<char*>(&owner),sizeof(owner)) || owner.magic!=Record{}.magic ||
           owner.bytes!=std::filesystem::file_size(file) || owner.digest!=checksum(file))
            throw std::runtime_error("A different save already uses this name; choose a new name");
        // Recoverable move: never silently overwrite an unrelated/native save.
        auto backup=native_/L"FrostBridge"/L"native-backups";
        std::filesystem::create_directories(backup);
        std::filesystem::rename(file,backup/(slot_+L"_"+std::to_wstring(GetTickCount64())+L"_"+key_+L".save"));
    }
    void prepare(const std::wstring& slot,std::uint64_t id) {
        if(exists(slot)) throw std::runtime_error("Save name already exists; choose a new name");
        const auto target=folder(slot);
        std::filesystem::create_directories(archive_);
        if(!std::filesystem::create_directory(target)) throw std::runtime_error("Save slot is reserved");
        // An interrupted save remains uncommitted and can never be loaded.
        std::filesystem::copy_file(nativeFile(id),target/(slot+L".save"));
        Record record; record.checkpoint=id; record.bytes=std::filesystem::file_size(target/(slot+L".save"));
        record.digest=checksum(target/(slot+L".save"));
        std::ofstream out(target/L"pending",std::ios::binary);
        out.write(reinterpret_cast<const char*>(&record),sizeof(record)); out.flush();
        if(!out) throw std::runtime_error("Cannot write checkpoint metadata");
        markAlias(record);
    }
    void commit(const std::wstring& slot) {
        std::filesystem::rename(folder(slot)/L"pending",folder(slot)/L"complete");
    }
    Record read(const std::wstring& slot) const {
        Record r{};
        std::ifstream in(folder(slot)/L"complete",std::ios::binary);
        if(!in.read(reinterpret_cast<char*>(&r),sizeof(r)) || in.peek()!=std::char_traits<char>::eof() ||
           r.magic!=Record{}.magic || !r.checkpoint || !r.bytes)
            throw std::runtime_error("Multiplayer save missing or incomplete");
        auto file=folder(slot)/(slot+L".save");
        if(std::filesystem::file_size(file)!=r.bytes || checksum(file)!=r.digest)
            throw std::runtime_error("Multiplayer save is damaged");
        return r;
    }
    void restore(const std::wstring& slot,const Record& record) {
        if(std::filesystem::exists(nativeFile(record.checkpoint))) {
            if(checksum(nativeFile(record.checkpoint))==record.digest) { markAlias(record);return; }
            makeRoom(record.checkpoint);
        }
        std::filesystem::copy_file(folder(slot)/(slot+L".save"),nativeFile(record.checkpoint));
        markAlias(record);
    }
private:
    std::filesystem::path alias() const { return native_/L"FrostBridge"/L"aliases"/(slot_+L".bin"); }
    void markAlias(const Record& record) {
        std::filesystem::create_directories(alias().parent_path());
        std::ofstream out(alias(),std::ios::binary|std::ios::trunc);
        out.write(reinterpret_cast<const char*>(&record),sizeof(record));out.flush();
        if(!out) throw std::runtime_error("Cannot record native save ownership");
    }
    std::filesystem::path native_;
    std::wstring key_;
    std::filesystem::path archive_;
    std::wstring slot_;
    HANDLE mutex_=nullptr;
    bool locked_=false;
    inline static std::mutex localMutex_;
    inline static std::set<std::wstring> localOwners_;
};
}
