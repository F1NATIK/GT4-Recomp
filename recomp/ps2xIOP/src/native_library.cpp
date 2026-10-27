#include "ps2x/iop/native_library.h"
#include <limits>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>

namespace ps2x::iop {
std::optional<NativeImportTable> decodeNativeImportTable(std::span<const uint8_t> bytes) {
    if (bytes.size()<28) return {};
    auto word=[&](size_t p) { return uint32_t(bytes[p])|(uint32_t(bytes[p+1])<<8)|(uint32_t(bytes[p+2])<<16)|(uint32_t(bytes[p+3])<<24); };
    if (word(0)!=0x41e00000) return {};
    NativeImportTable result{{},static_cast<uint16_t>(word(8)),{}};
    bool terminated=false;
    for (size_t p=12;p<20;++p) {
        const auto c=bytes[p];
        if (!c) { terminated=true; continue; }
        if (terminated || c<0x20 || c>0x7e) return {};
        result.name.push_back(static_cast<char>(c));
    }
    if (result.name.empty()) return {};
    std::unordered_set<uint16_t> seen;
    for (size_t p=20;p+8<=bytes.size() && result.ordinals.size()<256;p+=8) {
        const auto first=word(p),second=word(p+4);
        if (!first && !second) return result.ordinals.empty() ? std::nullopt : std::optional{std::move(result)};
        if (first!=0x03e00008 || (second>>16)!=0x2400) return {};
        const auto ordinal=static_cast<uint16_t>(second);
        if (!seen.insert(ordinal).second) return {};
        result.ordinals.push_back(ordinal);
    }
    return {}; // Truncated/malformed/unterminated table.
}
class NativeLibraryRegistry::Impl {
public:
    mutable std::recursive_mutex mutex;
    struct Library { uint64_t id; uint16_t version; std::unordered_map<uint16_t,NativeExport> exports; };
    std::unordered_map<std::string,Library> libraries;
    struct Callback { uint64_t owner; uint64_t id; NativeExportHandler handler; };
    std::unordered_map<uint32_t,Callback> callbacks;
    std::unordered_set<uint64_t> callbackOwners;
    uint64_t nextId=1;
};
NativeLibraryRegistry::NativeLibraryRegistry():impl_(std::make_unique<Impl>()) {}
NativeLibraryRegistry::~NativeLibraryRegistry()=default;
uint64_t NativeLibraryRegistry::registerLibrary(NativeLibrary library) {
    if (library.name.empty() || library.name.size()>8 || library.name.find('\0')!=std::string::npos || library.exports.empty()) return 0;
    Impl::Library staged{0,library.version,{}};
    for (auto &entry:library.exports) {
        if (bool(entry.handler)==bool(entry.contextHandler) || !staged.exports.emplace(entry.ordinal,std::move(entry)).second) return 0;
    }
    std::lock_guard lock(impl_->mutex);
    if (impl_->libraries.contains(library.name) || impl_->nextId==std::numeric_limits<uint64_t>::max()) return 0;
    staged.id=impl_->nextId++;
    const auto id=staged.id;
    impl_->libraries.emplace(std::move(library.name),std::move(staged));
    return id;
}
bool NativeLibraryRegistry::unregisterLibrary(uint64_t id) {
    std::lock_guard lock(impl_->mutex);
    for (auto it=impl_->libraries.begin();it!=impl_->libraries.end();++it) {
        if (it->second.id==id) {
            std::erase_if(impl_->callbacks,[id](const auto& entry) { return entry.second.owner==id; });
            impl_->libraries.erase(it); return true;
        }
    }
    return false;
}
NativeImportBinding NativeLibraryRegistry::bind(std::string_view name,uint16_t version,uint16_t ordinal) const {
    std::lock_guard lock(impl_->mutex);
    const auto it=impl_->libraries.find(std::string(name));
    if (it==impl_->libraries.end() || it->second.version!=version || !it->second.exports.contains(ordinal)) return {};
    return {it->second.id,ordinal};
}
NativeExportResult NativeLibraryRegistry::invoke(NativeImportBinding binding,const std::array<uint32_t,4>& args,
                                                std::optional<NativeCallContext> context) {
    std::lock_guard lock(impl_->mutex);
    for (const auto &[name,library]:impl_->libraries) {
        if (library.id!=binding.registration) continue;
        const auto it=library.exports.find(binding.ordinal);
        if (it==library.exports.end()) return {};
        // Retain callable if it unregisters itself or performs a nested import.
        const auto entry=it->second;
        if (entry.contextHandler) return context?entry.contextHandler(args,*context):NativeExportResult{};
        return entry.handler(args);
    }
    return {};
}
std::optional<std::vector<NativeImportBinding>> NativeLibraryRegistry::bindTable(const NativeImportTable& table) const {
    std::lock_guard lock(impl_->mutex);
    if (table.ordinals.empty()) return {};
    std::vector<NativeImportBinding> staged;
    for (const auto ordinal:table.ordinals) {
        const auto binding=bind(table.name,table.version,ordinal);
        if (!binding.registration) return {};
        staged.push_back(binding);
    }
    return staged;
}
uint64_t NativeLibraryRegistry::registerCallback(uint64_t owner,uint32_t address,NativeExportHandler handler) {
    if (!address || (address&3u) || !handler) return 0;
    std::lock_guard lock(impl_->mutex);
    const bool ownerExists=impl_->callbackOwners.contains(owner) ||
        std::any_of(impl_->libraries.begin(),impl_->libraries.end(),[owner](const auto& entry) { return entry.second.id==owner; });
    if (!ownerExists || impl_->callbacks.contains(address) || impl_->nextId==std::numeric_limits<uint64_t>::max()) return 0;
    const auto id=impl_->nextId++;
    impl_->callbacks.emplace(address,Impl::Callback{owner,id,std::move(handler)});
    return id;
}
uint64_t NativeLibraryRegistry::registerCallbackOwner() {
    std::lock_guard lock(impl_->mutex);
    if (impl_->nextId==std::numeric_limits<uint64_t>::max()) return 0;
    const auto id=impl_->nextId++;
    impl_->callbackOwners.insert(id); return id;
}
bool NativeLibraryRegistry::unregisterCallbackOwner(uint64_t owner) {
    std::lock_guard lock(impl_->mutex);
    if (!impl_->callbackOwners.erase(owner)) return false;
    std::erase_if(impl_->callbacks,[owner](const auto& entry) { return entry.second.owner==owner; });
    return true;
}
NativeCallbackBinding NativeLibraryRegistry::bindCallback(uint32_t address) const {
    std::lock_guard lock(impl_->mutex);
    const auto it=impl_->callbacks.find(address);
    return it==impl_->callbacks.end() ? NativeCallbackBinding{} : NativeCallbackBinding{it->second.owner,it->second.id};
}
NativeExportResult NativeLibraryRegistry::invokeCallback(NativeCallbackBinding binding,const std::array<uint32_t,4>& args) {
    std::lock_guard lock(impl_->mutex);
    for (const auto& [address,callback]:impl_->callbacks) {
        if (callback.id!=binding.callback || callback.owner!=binding.registration) continue;
        const auto handler=callback.handler;
        return handler(args); // Callable retained if provider unloads itself.
    }
    return {};
}
void NativeLibraryRegistry::reset() {
    std::lock_guard lock(impl_->mutex);
    impl_->libraries.clear();
    impl_->callbacks.clear();
    impl_->callbackOwners.clear();
}
}
