#include "ps2x/iop/gt3/mcman_startup_services.h"
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <algorithm>

namespace ps2x::iop::gt3 {
namespace {
// Handles are native identifiers, not pointers or original kernel IDs. Never
// reuse them across reset/provider replacement, including another runtime.
std::atomic<uint32_t> nextSemaphore{1};
uint32_t semaphoreId() {
    auto id=nextSemaphore.load();
    while (id<INT32_MAX) if (nextSemaphore.compare_exchange_weak(id,id+1)) return id;
    return 0;
}
}
struct McmanStartupServices::State {
    IopHost& host; NativeLibraryRegistry* registry;
    std::recursive_mutex mutex;
    bool active=false;
    std::array<uint64_t,4> registrations{};
    struct Semaphore { uint32_t attr,option,initial,maximum,count; };
    std::unordered_map<uint32_t,Semaphore> semaphores;
    std::array<NativeCallbackBinding,3> hooks{};
    struct Device { uint32_t address; std::array<NativeCallbackBinding,17> operations; };
    std::unordered_map<std::string,Device> devices;
    explicit State(IopHost& h):host(h),registry(h.nativeLibraries()) {}
    bool string(uint32_t address,std::string& value,size_t limit=256) {
        if (!address || address>UINT32_MAX-limit) return false;
        for (size_t i=0;i<limit;++i) {
            char c=0; if (!host.readIopMemory(address+uint32_t(i),&c,1)) return false;
            if (!c) return true; value+=c;
        }
        return false;
    }
    NativeExportResult semaphore(uint16_t ordinal,const std::array<uint32_t,4>& args) {
        std::lock_guard lock(mutex); if (!active) return {};
        if (ordinal==4) {
            std::array<uint32_t,4> data{};
            if (!host.readIopMemory(args[0],data.data(),sizeof(data)) || data[0]>1 ||
                data[1] || !data[3] || data[3]>INT32_MAX || data[2]>data[3] || semaphores.size()>=256) return {};
            const auto id=semaphoreId(); if (!id) return {};
            semaphores.emplace(id,Semaphore{data[0],data[1],data[2],data[3],data[2]});
            return {true,int32_t(id)};
        }
        auto it=semaphores.find(args[0]); if (it==semaphores.end()) return {};
        if (ordinal==5) { semaphores.erase(it); return {true,0}; }
        if (ordinal==6 || ordinal==7) {
            if (it->second.count==it->second.maximum) return {};
            ++it->second.count; return {true,0};
        }
        if (ordinal==8) {
            // Registry invokes handlers under a recursive lock. Blocking here
            // would prevent another thread from signaling through the registry.
            if (!it->second.count) return {};
            --it->second.count; return {true,0};
        }
        return {};
    }
    NativeExportResult setHook(size_t index,const std::array<uint32_t,4>& args) {
        std::lock_guard lock(mutex); if (!active) return {};
        const auto callback=registry->bindCallback(args[0]);
        if (args[0] && !callback.callback) return {};
        hooks[index]=callback; return {true,0}; // Setters are void in the original ABI.
    }
    NativeExportResult device(uint16_t ordinal,const std::array<uint32_t,4>& args) {
        std::lock_guard lock(mutex); if (!active) return {};
        std::string name;
        if (ordinal==21) {
            if (!string(args[0],name,16)) return {};
            const auto it=devices.find(name); if (it==devices.end()) return {true,-1};
            const auto driver=it->second;
            devices.erase(it); // Remove before callback; avoid reentrant stale use.
            return registry->invokeCallback(driver.operations[1],{driver.address,0,0,0});
        }
        std::array<uint32_t,5> descriptor{}; std::array<uint32_t,17> addresses{};
        if (ordinal!=20 || !host.readIopMemory(args[0],descriptor.data(),sizeof(descriptor)) ||
            !string(descriptor[0],name,16) || name.empty() || devices.contains(name) ||
            !host.readIopMemory(descriptor[4],addresses.data(),sizeof(addresses))) return {};
        Device driver{args[0],{}};
        for (size_t i=0;i<addresses.size();++i) {
            driver.operations[i]=registry->bindCallback(addresses[i]);
            if (!driver.operations[i].callback) return {};
        }
        const auto initialized=registry->invokeCallback(driver.operations[0],{args[0],0,0,0});
        if (!active || !initialized.handled || initialized.value) return {};
        devices.emplace(name,driver); return {true,0};
    }
};
McmanStartupServices::McmanStartupServices(IopHost& h):state_(std::make_shared<State>(h)) {}
McmanStartupServices::~McmanStartupServices() { reset(); }
bool McmanStartupServices::start() {
    auto& s=*state_; if (!s.registry || s.active) return false;
    std::weak_ptr<State> weak=state_;
    try {
        NativeLibrary sema{"thsemap",0x101,{}};
        for (uint16_t ordinal:{4,5,6,7,8}) sema.exports.push_back({ordinal,[weak,ordinal](const auto& args) {
            auto state=weak.lock(); return state?state->semaphore(ordinal,args):NativeExportResult{};
        }});
        s.registrations[0]=s.registry->registerLibrary(std::move(sema));
        NativeLibrary security{"secrman",0x103,{}};
        for (uint16_t ordinal:{4,5}) security.exports.push_back({ordinal,[weak,ordinal](const auto& args) {
            auto state=weak.lock(); return state?state->setHook(ordinal-4,args):NativeExportResult{};
        }});
        // Authentication export6 intentionally absent: registering callbacks
        // does not establish cryptography or an authenticated card.
        s.registrations[1]=s.registry->registerLibrary(std::move(security));
        s.registrations[2]=s.registry->registerLibrary({"modload",0x101,{{13,[weak](const auto& args) {
            auto state=weak.lock(); return state?state->setHook(2,args):NativeExportResult{};
        }}}});
        NativeLibrary ioman{"ioman",0x104,{}};
        for (uint16_t ordinal:{20,21}) ioman.exports.push_back({ordinal,[weak,ordinal](const auto& args) {
            auto state=weak.lock(); return state?state->device(ordinal,args):NativeExportResult{};
        }});
        s.registrations[3]=s.registry->registerLibrary(std::move(ioman));
        if (std::ranges::any_of(s.registrations,[](auto id) { return !id; })) { reset(); return false; }
        s.active=true; return true;
    } catch (...) { reset(); throw; }
}
void McmanStartupServices::reset() {
    auto& s=*state_;
    for (auto id:s.registrations) if (id) s.registry->unregisterLibrary(id);
    std::lock_guard lock(s.mutex);
    s.active=false; s.registrations={}; s.semaphores.clear(); s.hooks={}; s.devices.clear();
}
NativeExportResult McmanStartupServices::callHook(Hook hook,const std::array<uint32_t,4>& args) {
    auto& s=*state_; NativeCallbackBinding binding;
    { std::lock_guard lock(s.mutex); if (!s.active || size_t(hook)>=s.hooks.size()) return {}; binding=s.hooks[size_t(hook)]; }
    return s.registry->invokeCallback(binding,args);
}
NativeExportResult McmanStartupServices::callDevice(std::string_view name,uint16_t operation,const std::array<uint32_t,4>& args) {
    auto& s=*state_; NativeCallbackBinding binding;
    { std::lock_guard lock(s.mutex);
      const auto it=s.devices.find(std::string(name));
      if (!s.active || it==s.devices.end() || operation>=it->second.operations.size()) return {};
      binding=it->second.operations[operation]; }
    return s.registry->invokeCallback(binding,args);
}
}
