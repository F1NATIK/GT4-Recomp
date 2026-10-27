#pragma once
#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>
#include <span>
#include <optional>

namespace ps2x::iop {
// Native IOP-to-IOP imports. Arguments contain IOP values/pointers, never EE PCs.
struct NativeExportResult { bool handled=false; int32_t value=-1; };
using NativeExportHandler=std::function<NativeExportResult(const std::array<uint32_t,4>&)>;
// Explicit non-argument register inputs for audited native calls (USBD4 saves
// its caller's GP). Missing context never silently invents a register value.
struct NativeCallContext { uint32_t gp=0; };
using NativeContextExportHandler=std::function<NativeExportResult(const std::array<uint32_t,4>&,NativeCallContext)>;
struct NativeExport { uint16_t ordinal; NativeExportHandler handler; NativeContextExportHandler contextHandler{}; };
struct NativeLibrary { std::string name; uint16_t version; std::vector<NativeExport> exports; };
struct NativeImportBinding { uint64_t registration=0; uint16_t ordinal=0; };
struct NativeCallbackBinding { uint64_t registration=0; uint64_t callback=0; };
struct NativeImportTable { std::string name; uint16_t version; std::vector<uint16_t> ordinals; };
// Parse a bounded original IRX import table copy; never patch source stubs.
std::optional<NativeImportTable> decodeNativeImportTable(std::span<const uint8_t> bytes);
class NativeLibraryRegistry {
public:
    NativeLibraryRegistry();
    ~NativeLibraryRegistry();
    NativeLibraryRegistry(const NativeLibraryRegistry&)=delete;
    NativeLibraryRegistry& operator=(const NativeLibraryRegistry&)=delete;
    // Exact versions only until LOADCORE's wider compatibility rules are modeled.
    uint64_t registerLibrary(NativeLibrary library);
    bool unregisterLibrary(uint64_t registration);
    NativeImportBinding bind(std::string_view name,uint16_t version,uint16_t ordinal) const;
    std::optional<std::vector<NativeImportBinding>> bindTable(const NativeImportTable& table) const;
    NativeExportResult invoke(NativeImportBinding binding,const std::array<uint32_t,4>& arguments,
                              std::optional<NativeCallContext> context=std::nullopt);
    // Trusted native providers explicitly map their modeled IOP callback addresses.
    // These are independent of EE code addresses; no guest-function fallback exists.
    uint64_t registerCallback(uint64_t owner,uint32_t address,NativeExportHandler handler);
    // Modules with no original export library still own native callbacks.
    // These tokens publish no invented import library or numeric exports.
    uint64_t registerCallbackOwner();
    bool unregisterCallbackOwner(uint64_t owner);
    NativeCallbackBinding bindCallback(uint32_t address) const;
    NativeExportResult invokeCallback(NativeCallbackBinding binding,const std::array<uint32_t,4>& arguments);
    void reset(); // Invalidates existing bindings; registration IDs are never reused.
private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
}
