// DLL-wide COM bookkeeping for somike_apo.dll.
#pragma once

#include <windows.h>

namespace somike::apo {

void ModuleAddRef() noexcept;
void ModuleRelease() noexcept;
void ModuleLock(bool lock) noexcept;
[[nodiscard]] bool ModuleCanUnload() noexcept;

}  // namespace somike::apo
