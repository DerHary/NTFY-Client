#include "client.hpp"

// Windows publishes the effective Quiet Hours profile through Windows Notification Facility.
// This interface is not part of the public Windows SDK. Resolve it at runtime and accept
// only the known four-byte profiles so an unavailable or changed interface is harmless.
std::optional<bool> WindowsDoNotDisturb()
{
    using Query = LONG (NTAPI*)(const std::uint64_t*, const void*, const void*, ULONG*, void*, ULONG*);
    static const auto query = reinterpret_cast<Query>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryWnfStateData"));
    if (!query) return std::nullopt;

    constexpr std::uint64_t activeProfile = 0x0d83063ea3bf1c75ULL;
    ULONG stamp = 0;
    ULONG profile = 0;
    ULONG size = sizeof(profile);
    if (query(&activeProfile, nullptr, nullptr, &stamp, &profile, &size) < 0 ||
        size != sizeof(profile) || profile > 2) return std::nullopt;
    return profile != 0;
}
