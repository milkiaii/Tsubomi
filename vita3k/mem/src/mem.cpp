// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.

#include <mem/functions.h>
#include <mem/state.h>

#include <util/align.h>
#include <util/log.h>

#include <algorithm>
#include <cassert>
#include <cstring>
#include <mutex>
#include <utility>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#else
#include <csignal>
#include <sys/mman.h>
#include <unistd.h>
#endif

constexpr uint32_t STANDARD_PAGE_SIZE = KiB(4);
constexpr size_t TOTAL_MEM_SIZE = GiB(4);
constexpr bool LOG_PROTECT = false;
#ifdef NDEBUG
constexpr bool PAGE_NAME_TRACKING = false;
#else
constexpr bool PAGE_NAME_TRACKING = true;
#endif

// TODO: support multiple handlers
static AccessViolationHandler access_violation_handler;
static void register_access_violation_handler(const AccessViolationHandler &handler);

static Address alloc_inner(MemState &state, uint32_t start_page, uint32_t page_count, const char *name, const bool force);
static void delete_memory(uint8_t *memory);
static uint8_t *allocate_backing_region(uint32_t size);
static void delete_backing_region(uint8_t *memory, uint32_t size);
static void protect_sparse_range(MemState &state, Address addr, uint32_t size, MemPerm perm);

static PagePtr backing_page_entry(const MemState &state, Address address) {
    auto it = state.backing_regions.upper_bound(address / STANDARD_PAGE_SIZE);
    if (it == state.backing_regions.begin())
        return nullptr;
    --it;
    const BackingRegion &region = it->second;
    if (address < region.guest_address || address >= region.guest_address + region.size)
        return nullptr;
    return reinterpret_cast<uint8_t *>(reinterpret_cast<uintptr_t>(region.host_address) - region.guest_address);
}

static uint8_t *page_entry_pointer(PagePtr page_entry, Address address) {
    return page_entry ? reinterpret_cast<uint8_t *>(reinterpret_cast<uintptr_t>(page_entry) + address) : nullptr;
}

uint8_t *guest_memory_pointer(const MemState &state, Address address) {
    if (state.memory_mode == MemoryMode::SoftwarePageTable || state.use_page_table) {
        if (!state.page_table)
            return nullptr;
        uint8_t *const page = state.page_table[address / STANDARD_PAGE_SIZE];
        return page_entry_pointer(page, address);
    }
    return state.memory ? state.memory.get() + address : nullptr;
}

Address guest_memory_address(const MemState &state, const uint8_t *pointer) {
    if (state.memory_mode == MemoryMode::SoftwarePageTable) {
        const uintptr_t host_address = reinterpret_cast<uintptr_t>(pointer);
        for (const auto &entry : state.backing_regions) {
            const BackingRegion &region = entry.second;
            const uintptr_t region_start = reinterpret_cast<uintptr_t>(region.host_address);
            if (host_address >= region_start && host_address < region_start + region.size)
                return region.guest_address + static_cast<Address>(host_address - region_start);
        }
        return 0;
    }
    if (!state.memory)
        return 0;
    return static_cast<Address>(pointer - state.memory.get());
}

bool read_guest_memory(const MemState &state, Address address, void *destination, size_t size) {
    const uint64_t end = static_cast<uint64_t>(address) + size;
    if (end > (uint64_t{ 1 } << 32) || !is_valid_addr_range(state, address, static_cast<Address>(end)))
        return false;

    auto *output = static_cast<uint8_t *>(destination);
    while (size != 0) {
        uint8_t *const source = guest_memory_pointer(state, address);
        if (!source)
            return false;
        const size_t chunk = std::min<size_t>(size, STANDARD_PAGE_SIZE - (address % STANDARD_PAGE_SIZE));
        std::memcpy(output, source, chunk);
        output += chunk;
        address += static_cast<Address>(chunk);
        size -= chunk;
    }
    return true;
}

bool write_guest_memory(MemState &state, Address address, const void *source, size_t size) {
    const uint64_t end = static_cast<uint64_t>(address) + size;
    if (end > (uint64_t{ 1 } << 32) || !is_valid_addr_range(state, address, static_cast<Address>(end)))
        return false;

    const auto *input = static_cast<const uint8_t *>(source);
    while (size != 0) {
        uint8_t *const destination = guest_memory_pointer(state, address);
        if (!destination)
            return false;
        const size_t chunk = std::min<size_t>(size, STANDARD_PAGE_SIZE - (address % STANDARD_PAGE_SIZE));
        std::memcpy(destination, input, chunk);
        input += chunk;
        address += static_cast<Address>(chunk);
        size -= chunk;
    }
    return true;
}

#ifdef _WIN32
static std::string get_error_msg() {
    return std::system_category().message(GetLastError());
}
#else
static std::string get_error_msg() {
    return strerror(errno);
}
#endif

// A guest arena reserved before other large mappings (e.g. the iOS JIT region
// pool) could fragment the address space; the next init() adopts it.
static void *g_prereserved_memory = nullptr;

bool prereserve_guest_memory(const bool force_software_page_table) {
#ifdef _WIN32
    // Not needed on desktop: the address space is large and nothing else
    // competes for it before init() runs.
    return true;
#else
    if (force_software_page_table)
        return true;
    if (g_prereserved_memory)
        return true;
    void *preferred_address = reinterpret_cast<void *>(1ULL << 34);
    void *memory = mmap(preferred_address, TOTAL_MEM_SIZE, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, 0, 0);
    if (memory == MAP_FAILED) {
        LOG_CRITICAL("Guest memory prereservation failed: {}", get_error_msg());
        return false;
    }
    g_prereserved_memory = memory;
    return true;
#endif
}

bool init(MemState &state, const bool use_page_table, const bool force_software_page_table) {
#ifdef _WIN32
    SYSTEM_INFO system_info = {};
    GetSystemInfo(&system_info);
    state.host_page_size = system_info.dwPageSize;
#else
    state.host_page_size = static_cast<int>(sysconf(_SC_PAGESIZE));
#endif

    assert(state.host_page_size >= 4096); // Limit imposed by Unicorn.

    void *preferred_address = reinterpret_cast<void *>(1ULL << 34);
    bool use_software_page_table = force_software_page_table;

#ifndef _WIN32
    if (use_software_page_table && g_prereserved_memory) {
        delete_memory(static_cast<uint8_t *>(g_prereserved_memory));
        g_prereserved_memory = nullptr;
    }
#endif

#ifdef _WIN32
    if (!use_software_page_table) {
        state.memory = Memory(static_cast<uint8_t *>(VirtualAlloc(preferred_address, TOTAL_MEM_SIZE, MEM_RESERVE, PAGE_NOACCESS)), delete_memory);
    }
    if (!state.memory && !use_software_page_table) {
        // fallback
        state.memory = Memory(static_cast<uint8_t *>(VirtualAlloc(nullptr, TOTAL_MEM_SIZE, MEM_RESERVE, PAGE_NOACCESS)), delete_memory);

        if (!state.memory) {
            LOG_WARN("VirtualAlloc could not reserve the 4 GiB guest arena: {}; selecting software page-table mode", get_error_msg());
            use_software_page_table = true;
        }
    }
#else
    if (use_software_page_table) {
        state.memory = Memory(nullptr, delete_memory);
    } else if (g_prereserved_memory) {
        state.memory = Memory(static_cast<uint8_t *>(g_prereserved_memory), delete_memory);
        g_prereserved_memory = nullptr;
    } else {
        // http://man7.org/linux/man-pages/man2/mmap.2.html
        const int prot = PROT_NONE;
        const int flags = MAP_PRIVATE | MAP_ANONYMOUS;
        const int fd = 0;
        const off_t offset = 0;
        // preferred_address is only a hint for mmap, if it can't use it, the kernel will choose itself the address
        void *memory = mmap(preferred_address, TOTAL_MEM_SIZE, prot, flags, fd, offset);
        if (memory == MAP_FAILED) {
            LOG_WARN("mmap could not reserve the 4 GiB guest arena: {}; selecting software page-table mode", get_error_msg());
            state.memory = Memory(nullptr, delete_memory);
            use_software_page_table = true;
        } else {
            state.memory = Memory(static_cast<uint8_t *>(memory), delete_memory);
        }
    }
#endif

    if (use_software_page_table)
        LOG_WARN("Guest 4 GiB reservation unavailable or disabled; using software page-table memory mode");

    const size_t table_length = TOTAL_MEM_SIZE / STANDARD_PAGE_SIZE;
    state.alloc_table = AllocPageTable(new AllocMemPage[table_length]);
    memset(state.alloc_table.get(), 0, sizeof(AllocMemPage) * table_length);

    state.allocator.set_maximum(table_length);
    state.memory_mode = use_software_page_table ? MemoryMode::SoftwarePageTable : MemoryMode::Fastmem;
    state.use_page_table = use_page_table || use_software_page_table;
    if (state.use_page_table) {
        state.page_table = PageTable(new PagePtr[TOTAL_MEM_SIZE / STANDARD_PAGE_SIZE]);
        std::fill_n(state.page_table.get(), TOTAL_MEM_SIZE / STANDARD_PAGE_SIZE,
            use_software_page_table ? nullptr : state.memory.get());
    }

    const auto handler = [&state](uint8_t *addr, bool write) noexcept {
        return handle_access_violation(state, addr, write);
    };
    register_access_violation_handler(handler);

    const Address null_address = alloc_inner(state, 0, state.host_page_size / STANDARD_PAGE_SIZE, "null", true);
    assert(null_address == 0);
    if (!use_software_page_table) {
#ifdef _WIN32
        DWORD old_protect = 0;
        const BOOL ret = VirtualProtect(state.memory.get(), state.host_page_size, PAGE_NOACCESS, &old_protect);
        LOG_CRITICAL_IF(!ret, "VirtualAlloc failed: {}", get_error_msg());
#else
        const int ret = mprotect(state.memory.get(), state.host_page_size, PROT_NONE);
        LOG_CRITICAL_IF(ret == -1, "mprotect failed: {}", get_error_msg());
#endif
    }

    return true;
}

static void delete_memory(uint8_t *memory) {
    if (memory != nullptr) {
#ifdef _WIN32
        const BOOL ret = VirtualFree(memory, 0, MEM_RELEASE);
        assert(ret);
#else
        munmap(memory, TOTAL_MEM_SIZE);
#endif
    }
}

static uint8_t *allocate_backing_region(uint32_t size) {
#ifdef _WIN32
    return static_cast<uint8_t *>(VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
#else
    void *memory = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return memory == MAP_FAILED ? nullptr : static_cast<uint8_t *>(memory);
#endif
}

static void delete_backing_region(uint8_t *memory, uint32_t size) {
    if (!memory)
        return;
#ifdef _WIN32
    const BOOL ret = VirtualFree(memory, 0, MEM_RELEASE);
    assert(ret);
#else
    const int ret = munmap(memory, size);
    assert(ret == 0);
#endif
}

bool is_valid_addr(const MemState &state, Address addr) {
    const uint32_t page_num = addr / STANDARD_PAGE_SIZE;
    return addr && state.allocator.free_slot_count(page_num, page_num + 1) == 0;
}

bool is_valid_addr_range(const MemState &state, Address start, Address end) {
    const uint32_t start_page = start / STANDARD_PAGE_SIZE;
    const uint32_t end_page = (end + STANDARD_PAGE_SIZE - 1) / STANDARD_PAGE_SIZE;
    return state.allocator.free_slot_count(start_page, end_page) == 0;
}

static Address alloc_inner(MemState &state, uint32_t start_page, uint32_t page_count, const char *name, const bool force) {
    int page_num;
    if (force) {
        if (state.allocator.allocate_at(start_page, page_count) < 0) {
            return 0;
        }
        page_num = start_page;
    } else {
        page_num = state.allocator.allocate_from(start_page, page_count, false);
        if (page_num < 0)
            return 0;
    }

    const uint32_t size = page_count * STANDARD_PAGE_SIZE;
    const Address addr = page_num * STANDARD_PAGE_SIZE;

    if (state.memory_mode == MemoryMode::SoftwarePageTable) {
        if (addr != 0) {
            uint8_t *const backing = allocate_backing_region(size);
            if (!backing) {
                state.allocator.free(static_cast<uint32_t>(page_num), page_count);
                return 0;
            }
            std::memset(backing, 0, size);
            {
                const std::lock_guard lock(state.protect_mutex);
                state.backing_regions.emplace(static_cast<uint32_t>(page_num), BackingRegion{ addr, size, backing });
            }
            const uintptr_t page_entry = reinterpret_cast<uintptr_t>(backing) - addr;
            for (uint32_t page = 0; page < page_count; page++)
                state.page_table[page_num + page] = reinterpret_cast<uint8_t *>(page_entry);
        }
    } else {
        const Address commit_start = align_down(addr, state.host_page_size);
        const Address commit_end = align(addr + size, state.host_page_size);
        const uint32_t commit_size = commit_end - commit_start;
        uint8_t *const commit_ptr = &state.memory[commit_start];

        // Make memory chunk available to access
#ifdef _WIN32
        const void *const ret = VirtualAlloc(commit_ptr, commit_size, MEM_COMMIT, PAGE_READWRITE);
        LOG_CRITICAL_IF(!ret, "VirtualAlloc failed: {}", get_error_msg());
#else
        const int ret = mprotect(commit_ptr, commit_size, PROT_READ | PROT_WRITE);
        LOG_CRITICAL_IF(ret == -1, "mprotect failed: {}", get_error_msg());
#endif
        std::memset(&state.memory[addr], 0, size);
    }

    AllocMemPage &page = state.alloc_table[page_num];
    assert(!page.allocated);
    page.allocated = 1;
    page.size = page_count;

    if (PAGE_NAME_TRACKING) {
        state.page_name_map.emplace(page_num, name);
    }

    return addr;
}

Address alloc_aligned(MemState &state, uint32_t size, const char *name, unsigned int alignment, Address start_addr) {
    if (alignment == 0)
        return alloc(state, size, name, start_addr);
    const std::lock_guard<std::mutex> lock(state.generation_mutex);
    size += alignment;
    const uint32_t page_count = align(size, STANDARD_PAGE_SIZE) / STANDARD_PAGE_SIZE;
    const Address addr = alloc_inner(state, start_addr / STANDARD_PAGE_SIZE, page_count, name, false);
    const Address align_addr = align(addr, alignment);
    const uint32_t page_num = addr / STANDARD_PAGE_SIZE;
    const uint32_t align_page_num = align_addr / STANDARD_PAGE_SIZE;

    if (page_num != align_page_num) {
        AllocMemPage &page = state.alloc_table[page_num];
        AllocMemPage &align_page = state.alloc_table[align_page_num];
        const uint32_t remnant_front = align_page_num - page_num;
        state.allocator.free(page_num, remnant_front);
        page.allocated = 0;
        align_page.allocated = 1;
        align_page.size = page.size - remnant_front;
        if (state.memory_mode == MemoryMode::SoftwarePageTable) {
            const std::lock_guard lock(state.protect_mutex);
            auto region = state.backing_regions.extract(page_num);
            if (!region.empty()) {
                region.key() = align_page_num;
                state.backing_regions.insert(std::move(region));
            }
            std::fill_n(state.page_table.get() + page_num, remnant_front, nullptr);
        }
    }

    return align_addr;
}

static void align_to_page(MemState &state, Address &addr, Address &size) {
    const Address end = align(addr + size, STANDARD_PAGE_SIZE);
    addr = align_down(addr, STANDARD_PAGE_SIZE);
    size = end - addr;
}

void unprotect_inner(MemState &state, Address addr, uint32_t size) {
    if (LOG_PROTECT) {
        fmt::print("Unprotect: {} {}\n", log_hex(addr), size);
    }
    if (state.memory_mode == MemoryMode::SoftwarePageTable) {
        protect_sparse_range(state, addr, size, MemPerm::ReadWrite);
        return;
    }
    uint8_t *target = guest_memory_pointer(state, addr);
    uint8_t *aligned_start = reinterpret_cast<uint8_t *>(
        align_down(reinterpret_cast<uintptr_t>(target), state.host_page_size));
    uint8_t *aligned_end = reinterpret_cast<uint8_t *>(align(reinterpret_cast<uintptr_t>(target + size), state.host_page_size));
    size_t aligned_size = aligned_end - aligned_start;

#ifdef _WIN32
    DWORD old_protect = 0;
    const BOOL ret = VirtualProtect(aligned_start, aligned_size, PAGE_READWRITE, &old_protect);
    LOG_CRITICAL_IF(!ret, "VirtualAlloc failed: {}", get_error_msg());
#else
    const int ret = mprotect(aligned_start, aligned_size, PROT_READ | PROT_WRITE);
    LOG_CRITICAL_IF(ret == -1, "mprotect failed: {}", get_error_msg());
#endif
}

void protect_inner(MemState &state, Address addr, uint32_t size, const MemPerm perm) {
    if (state.memory_mode == MemoryMode::SoftwarePageTable) {
        protect_sparse_range(state, addr, size, perm);
        return;
    }
    uint8_t *target = guest_memory_pointer(state, addr);
    uint8_t *aligned_start = reinterpret_cast<uint8_t *>(
        align_down(reinterpret_cast<uintptr_t>(target), state.host_page_size));
    uint8_t *aligned_end = reinterpret_cast<uint8_t *>(align(reinterpret_cast<uintptr_t>(target + size), state.host_page_size));
    size_t aligned_size = aligned_end - aligned_start;

#ifdef _WIN32
    DWORD old_protect = 0;
    const BOOL ret = VirtualProtect(aligned_start, aligned_size, (perm == MemPerm::None) ? PAGE_NOACCESS : ((perm == MemPerm::ReadOnly) ? PAGE_READONLY : PAGE_READWRITE), &old_protect);
    LOG_CRITICAL_IF(!ret, "VirtualAlloc failed: {}", get_error_msg());
#else
    const int ret = mprotect(aligned_start, aligned_size, (perm == MemPerm::None) ? PROT_NONE : ((perm == MemPerm::ReadOnly) ? PROT_READ : (PROT_READ | PROT_WRITE)));
    LOG_CRITICAL_IF(ret == -1, "mprotect failed: {}", get_error_msg());
#endif
}

static void protect_sparse_range(MemState &state, Address addr, uint32_t size, MemPerm perm) {
    const Address page_start = align_down(addr, STANDARD_PAGE_SIZE);
    const Address page_end = align(addr + size, STANDARD_PAGE_SIZE);
    uint8_t *range_start = nullptr;
    uintptr_t range_end = 0;

    const auto flush = [&] {
        if (!range_start)
            return;
        const size_t range_size = range_end - reinterpret_cast<uintptr_t>(range_start);
#ifdef _WIN32
        DWORD old_protect = 0;
        const DWORD protection = (perm == MemPerm::None) ? PAGE_NOACCESS : ((perm == MemPerm::ReadOnly) ? PAGE_READONLY : PAGE_READWRITE);
        const BOOL ret = VirtualProtect(range_start, range_size, protection, &old_protect);
        LOG_CRITICAL_IF(!ret, "VirtualAlloc failed: {}", get_error_msg());
#else
        const int protection = (perm == MemPerm::None) ? PROT_NONE : ((perm == MemPerm::ReadOnly) ? PROT_READ : (PROT_READ | PROT_WRITE));
        const int ret = mprotect(range_start, range_size, protection);
        LOG_CRITICAL_IF(ret == -1, "mprotect failed: {}", get_error_msg());
#endif
        range_start = nullptr;
        range_end = 0;
    };

    for (Address page = page_start; page < page_end; page += STANDARD_PAGE_SIZE) {
        uint8_t *const target = guest_memory_pointer(state, page);
        if (!target) {
            flush();
            continue;
        }
        const uintptr_t host_start = align_down(reinterpret_cast<uintptr_t>(target), state.host_page_size);
        const uintptr_t host_end = align(reinterpret_cast<uintptr_t>(target + STANDARD_PAGE_SIZE), state.host_page_size);
        if (range_start && host_start > range_end)
            flush();
        if (!range_start)
            range_start = reinterpret_cast<uint8_t *>(host_start);
        range_end = std::max(range_end, host_end);
    }
    flush();
}

bool handle_access_violation(MemState &state, uint8_t *addr, bool write) noexcept {
    const uintptr_t memory_addr = reinterpret_cast<uintptr_t>(state.memory.get());
    const uintptr_t fault_addr = reinterpret_cast<uintptr_t>(addr);

    Address vaddr = 0;
    const std::unique_lock<std::mutex> lock(state.protect_mutex);
    if (state.memory_mode == MemoryMode::SoftwarePageTable) {
        for (const auto &entry : state.backing_regions) {
            const BackingRegion &region = entry.second;
            const uintptr_t region_start = reinterpret_cast<uintptr_t>(region.host_address);
            if (fault_addr >= region_start && fault_addr < region_start + region.size) {
                vaddr = region.guest_address + static_cast<Address>(fault_addr - region_start);
                break;
            }
        }
        if (!vaddr) {
            const uint64_t address_value = std::bit_cast<uint64_t>(addr);
            auto it = state.external_mapping.lower_bound(address_value);
            if (it == state.external_mapping.end() || address_value >= it->first + it->second.size)
                return false;
            vaddr = static_cast<Address>(address_value - it->first + it->second.address);
        }
    } else if (fault_addr < reinterpret_cast<uintptr_t>(state.memory.get()) || fault_addr >= reinterpret_cast<uintptr_t>(state.memory.get()) + TOTAL_MEM_SIZE) {
        if (state.use_page_table) {
            // this may come from an external mapping
            uint64_t addr_val = std::bit_cast<uint64_t>(addr);
            auto it = state.external_mapping.lower_bound(addr_val);
            if (it != state.external_mapping.end() && addr_val < it->first + it->second.size) {
                vaddr = static_cast<Address>(addr_val - it->first + it->second.address);
            } else {
                return false;
            }
        } else {
            return false;
        }
    } else {
        vaddr = static_cast<Address>(fault_addr - memory_addr);
    }

    if (!is_valid_addr(state, vaddr)) {
        return false;
    }
    if (LOG_PROTECT) {
        fmt::print("Access: {}\n", log_hex(vaddr));
    }
#ifdef VITA3K_PLATFORM_IOS
    // With StikDebug attached every one of these traps costs a debug-link
    // round trip, so any code path that still installs guest-memory
    // protections must be found and removed.
    const uint64_t trap_count = state.access_violations_handled.fetch_add(1, std::memory_order_relaxed) + 1;
    if (trap_count == 1 || trap_count % 100 == 0)
        LOG_WARN("Guest access-violation trap #{} handled at 0x{:X} (write={}); traps are debugger-delivered on iOS",
            trap_count, vaddr, write);
#else
    state.access_violations_handled.fetch_add(1, std::memory_order_relaxed);
#endif

    auto it = state.protect_tree.lower_bound(vaddr);
    if (it == state.protect_tree.end()) {
        // HACK: keep going
        unprotect_inner(state, align_down(vaddr, state.host_page_size), state.host_page_size);
        LOG_CRITICAL("Unhandled write protected region was valid. Address=0x{:X}", vaddr);
        return true;
    }

    ProtectSegmentInfo &info = it->second;
    if (vaddr < it->first || vaddr >= it->first + info.size) {
        // HACK: keep going
        unprotect_inner(state, align_down(vaddr, state.host_page_size), state.host_page_size);
        LOG_CRITICAL("Unhandled write protected region was valid. Address=0x{:X}", vaddr);
        return true;
    }

    Address previous_beg = it->first;
    for (auto &[block_addr, block] : info.blocks) {
        block.callback(vaddr, write);
    }

    unprotect_inner(state, it->first, info.size);
    state.protect_tree.erase(it);

    return true;
}

bool add_protect(MemState &state, Address addr, const uint32_t size, const MemPerm perm, const ProtectCallback &callback) {
    const std::lock_guard<std::mutex> lock(state.protect_mutex);
    ProtectSegmentInfo protect(size, perm);
    align_to_page(state, addr, protect.size);

    ProtectBlockInfo block;
    block.size = size;
    block.callback = callback;

    protect.blocks.emplace(addr, std::move(block));

    auto it = state.protect_tree.lower_bound(addr);
    if (it == state.protect_tree.end() || it->first + it->second.size <= addr) {
        if (it == state.protect_tree.begin())
            it = state.protect_tree.end();
        else
            --it;
    }

    while (it != state.protect_tree.end() && it->first < addr + size) {
        const Address start = std::min(it->first, addr);
        protect.size = std::max(it->first + it->second.size, addr + protect.size) - start;
        addr = start;
        protect.blocks.merge(it->second.blocks); // transfer blocks to the new protect
        protect.perm = most_restrictive_perm(protect.perm, it->second.perm);

        if (it == state.protect_tree.begin()) {
            state.protect_tree.erase(it);
            break;
        }

        // protect tree is in reverse order, so decrease it
        state.protect_tree.erase(it--);
    }

    protect_inner(state, addr, protect.size, protect.perm);

    state.protect_tree.emplace(addr, std::move(protect));
    return true;
}

bool is_protecting(MemState &state, Address addr, MemPerm *perm) {
    const std::lock_guard<std::mutex> lock(state.protect_mutex);
    auto ite = state.protect_tree.lower_bound(addr);

    if (ite != state.protect_tree.end() && addr < ite->first + ite->second.size) {
        if (perm)
            *perm = ite->second.perm;

        return true;
    }

    return false;
}

void add_external_mapping(MemState &mem, Address addr, uint32_t size, uint8_t *addr_ptr) {
    assert((size & 4095) == 0);
    if (!mem.use_page_table)
        return;

    uint64_t addr_value = std::bit_cast<uint64_t>(addr_ptr);
    uint8_t *page_table_entry = reinterpret_cast<uint8_t *>(reinterpret_cast<uintptr_t>(addr_ptr) - addr);
    uint8_t *original_address = guest_memory_pointer(mem, addr);
    for (int block = 0; block < size / KiB(4); block++) {
        // this is not thread write safe, but hopefully not other thread is busy copying while this happens
        memcpy(addr_ptr + block * KiB(4), original_address + block * KiB(4), KiB(4));
        mem.page_table[addr / KiB(4) + block] = page_table_entry;
    }

    // Set the first entry to the source so protection applies to guest memory.
    mem.page_table[addr / KiB(4)] = mem.memory_mode == MemoryMode::SoftwarePageTable
        ? backing_page_entry(mem, addr)
        : mem.memory.get();
    protect_inner(mem, addr, size, MemPerm::None);
    mem.page_table[addr / KiB(4)] = page_table_entry;

    const std::unique_lock<std::mutex> lock(mem.protect_mutex);
    mem.external_mapping[addr_value] = { addr, size };
}

void remove_external_mapping(MemState &mem, uint8_t *addr_ptr, uint32_t size) {
    uint64_t addr_value = std::bit_cast<uint64_t>(addr_ptr);
    MemExternalMapping mapping;
    if (mem.use_page_table) {
        const std::unique_lock<std::mutex> lock(mem.protect_mutex);
        auto it = mem.external_mapping.find(addr_value);
        assert(it != mem.external_mapping.end());

        mapping = it->second;
        mem.external_mapping.erase(it);
    } else {
        mapping.address = static_cast<Address>(addr_ptr - mem.memory.get());
        mapping.size = size;
    }

    // remove all protections on this range
    unprotect_inner(mem, mapping.address, mapping.size);
    {
        const std::unique_lock<std::mutex> lock(mem.protect_mutex);
        auto prot_it = mem.protect_tree.lower_bound(mapping.address);
        if (prot_it->first + prot_it->second.size <= mapping.address) {
            if (prot_it == mem.protect_tree.begin())
                prot_it = mem.protect_tree.end();
            else
                --prot_it;
        }

        while (prot_it != mem.protect_tree.end() && prot_it->first < mapping.address + mapping.size) {
            if (prot_it == mem.protect_tree.begin()) {
                mem.protect_tree.erase(prot_it);
                break;
            }

            mem.protect_tree.erase(prot_it--);
        }
    }

    if (mem.use_page_table) {
        // unprotect the original memory range
        mem.page_table[mapping.address / KiB(4)] = mem.memory_mode == MemoryMode::SoftwarePageTable
            ? backing_page_entry(mem, mapping.address)
            : mem.memory.get();
        unprotect_inner(mem, mapping.address, mapping.size);
        // copy back and reset the page table
        for (int block = 0; block < mapping.size / KiB(4); block++) {
            // this is not thread write safe, but hopefully not other thread is busy copying while this happens
            const Address guest_page = mapping.address + block * KiB(4);
            uint8_t *destination = mem.memory_mode == MemoryMode::SoftwarePageTable
                ? page_entry_pointer(backing_page_entry(mem, guest_page), guest_page)
                : &mem.memory[guest_page];
            memcpy(destination, addr_ptr + block * KiB(4), KiB(4));
            mem.page_table[guest_page / KiB(4)] = mem.memory_mode == MemoryMode::SoftwarePageTable
                ? backing_page_entry(mem, guest_page)
                : mem.memory.get();
        }
    }
}

Address alloc(MemState &state, uint32_t size, const char *name, Address start_addr) {
    const std::lock_guard<std::mutex> lock(state.generation_mutex);
    const uint32_t page_count = align(size, STANDARD_PAGE_SIZE) / STANDARD_PAGE_SIZE;
    const Address addr = alloc_inner(state, start_addr / STANDARD_PAGE_SIZE, page_count, name, false);
    return addr;
}

Address alloc_at(MemState &state, Address address, uint32_t size, const char *name) {
    auto addr = try_alloc_at(state, address, size, name);
    LOG_CRITICAL_IF(addr == 0, "Failed to allocate at specific page. Memory address:{}, size:{}, name:{}", log_hex(address), log_hex(size), name);
    return addr;
}

Address try_alloc_at(MemState &state, Address address, uint32_t size, const char *name) {
    const std::lock_guard<std::mutex> lock(state.generation_mutex);
    const uint32_t wanted_page = address / STANDARD_PAGE_SIZE;
    size += address % STANDARD_PAGE_SIZE;
    const uint32_t page_count = align(size, STANDARD_PAGE_SIZE) / STANDARD_PAGE_SIZE;
    const Address addr = alloc_inner(state, wanted_page, page_count, name, true);
    return addr ? address : 0;
}

Block alloc_block(MemState &mem, uint32_t size, const char *name, Address start_addr) {
    const Address address = alloc(mem, size, name, start_addr);
    return Block(address, [&mem](Address stack) {
        free(mem, stack);
    });
}

void free(MemState &state, Address address) {
    const std::lock_guard<std::mutex> lock(state.generation_mutex);
    const uint32_t page_num = address / STANDARD_PAGE_SIZE;
    assert(page_num >= 0);

    AllocMemPage &page = state.alloc_table[page_num];
    if (!page.allocated) {
        LOG_CRITICAL("Freeing unallocated page");
    }
    page.allocated = 0;

    state.allocator.free(page_num, page.size);
    if (PAGE_NAME_TRACKING) {
        state.page_name_map.erase(page_num);
    }

    assert(!state.use_page_table || (state.memory_mode == MemoryMode::SoftwarePageTable
            ? state.page_table[address / KiB(4)] == backing_page_entry(state, address)
            : state.page_table[address / KiB(4)] == state.memory.get()));
    const Address region_start = page_num * STANDARD_PAGE_SIZE;
    const Address region_end = region_start + page.size * STANDARD_PAGE_SIZE;

    if (state.memory_mode == MemoryMode::SoftwarePageTable) {
        BackingRegion backing;
        bool has_backing = false;
        {
            const std::lock_guard lock(state.protect_mutex);
            std::fill_n(state.page_table.get() + page_num, page.size, nullptr);
            auto region = state.backing_regions.extract(page_num);
            if (!region.empty()) {
                backing = region.mapped();
                has_backing = true;
            }
        }
        if (has_backing)
            delete_backing_region(backing.host_address, backing.size);
        return;
    }

    Address host_page = align_down(region_start, state.host_page_size);
    Address batch_start = 0;
    uint32_t batch_size = 0;

    while (host_page < region_end) {
        Address host_page_end = host_page + state.host_page_size;
        uint32_t first_guest = host_page / STANDARD_PAGE_SIZE;
        uint32_t last_guest = host_page_end / STANDARD_PAGE_SIZE;

        if (state.allocator.free_slot_count(first_guest, last_guest) == (last_guest - first_guest)) {
            if (batch_size == 0)
                batch_start = host_page;
            batch_size += state.host_page_size;
        } else if (batch_size > 0) {
            uint8_t *memory = &state.memory[batch_start];
#ifdef _WIN32
            const BOOL ret = VirtualFree(memory, batch_size, MEM_DECOMMIT);
            LOG_CRITICAL_IF(!ret, "VirtualFree failed: {}", get_error_msg());
#else
            int ret = mprotect(memory, batch_size, PROT_NONE);
            LOG_CRITICAL_IF(ret == -1, "mprotect failed: {}", get_error_msg());
            ret = madvise(memory, batch_size, MADV_DONTNEED);
            LOG_CRITICAL_IF(ret == -1, "madvise failed: {}", get_error_msg());
#endif
            batch_size = 0;
        }
        host_page = host_page_end;
    }

    if (batch_size > 0) {
        uint8_t *memory = &state.memory[batch_start];
#ifdef _WIN32
        const BOOL ret = VirtualFree(memory, batch_size, MEM_DECOMMIT);
        LOG_CRITICAL_IF(!ret, "VirtualFree failed: {}", get_error_msg());
#else
        int ret = mprotect(memory, batch_size, PROT_NONE);
        LOG_CRITICAL_IF(ret == -1, "mprotect failed: {}", get_error_msg());
        ret = madvise(memory, batch_size, MADV_DONTNEED);
        LOG_CRITICAL_IF(ret == -1, "madvise failed: {}", get_error_msg());
#endif
    }
}

uint32_t mem_available(MemState &state) {
    return state.allocator.free_slot_count(0, state.allocator.max_offset) * STANDARD_PAGE_SIZE;
}

const char *mem_name(Address address, MemState &state) {
    if (PAGE_NAME_TRACKING) {
        return state.page_name_map.find(address / STANDARD_PAGE_SIZE)->second.c_str();
    }
    return "";
}

void deinit_mem(MemState &state) {
    const std::lock_guard<std::mutex> gen_lock(state.generation_mutex);

    {
        const std::lock_guard<std::mutex> prot_lock(state.protect_mutex);
        state.protect_tree.clear();
    }

    state.memory.reset();
    for (const auto &entry : state.backing_regions)
        delete_backing_region(entry.second.host_address, entry.second.size);
    state.backing_regions.clear();
    state.alloc_table.reset();
    state.allocator.reset();
    state.page_name_map.clear();
    state.page_table.reset();
    state.external_mapping.clear();
    state.use_page_table = false;
    state.memory_mode = MemoryMode::Fastmem;
    state.host_page_size = 0;
}

#ifdef _WIN32

static LONG WINAPI exception_handler(PEXCEPTION_POINTERS pExp) noexcept {
    if (pExp->ExceptionRecord->ExceptionCode == EXCEPTION_BREAKPOINT && IsDebuggerPresent()) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    const auto ptr = reinterpret_cast<uint8_t *>(pExp->ExceptionRecord->ExceptionInformation[1]);
    const bool is_writing = pExp->ExceptionRecord->ExceptionInformation[0] == 1;
    const bool is_executing = pExp->ExceptionRecord->ExceptionInformation[0] == 8;

    if (pExp->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && !is_executing) {
        if (access_violation_handler(ptr, is_writing)) {
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }

    return EXCEPTION_CONTINUE_SEARCH;
}

static void register_access_violation_handler(const AccessViolationHandler &handler) {
    access_violation_handler = handler;
    if (!AddVectoredExceptionHandler(1, exception_handler)) {
        LOG_CRITICAL("Failed to register an exception handler");
    }
}

#else

static void signal_handler(int sig, siginfo_t *info, void *uct) noexcept {
    auto context = static_cast<ucontext_t *>(uct);

#ifdef __aarch64__
#ifdef __APPLE__
    const uint32_t esr = context->uc_mcontext->__es.__esr;
#else
    _aarch64_ctx *ctx = reinterpret_cast<_aarch64_ctx *>(context->uc_mcontext.__reserved);
    // get the ESR register
    while (ctx->magic != ESR_MAGIC) {
        if (ctx->magic == 0)
            [[unlikely]]
            raise(SIGTRAP);
        else
            [[likely]]
            ctx = reinterpret_cast<_aarch64_ctx *>(reinterpret_cast<uint8_t *>(ctx) + ctx->size);
    }

    const uint64_t esr = reinterpret_cast<esr_context *>(ctx)->esr;
#endif
    // https://developer.arm.com/documentation/ddi0595/2021-03/AArch64-Registers/ESR-EL1--Exception-Syndrome-Register--EL1-
    const uint32_t exception_class = static_cast<uint32_t>(esr) >> 26;
    const bool is_executing = (exception_class == 0b100000) || (exception_class == 0b100001);
    const bool is_data_abort = (exception_class == 0b100100) || (exception_class == 0b100101);
    const bool is_writing = is_data_abort && (esr & (1 << 6));
#else
#ifdef __APPLE__
    const uint64_t err = context->uc_mcontext->__es.__err;
#else
    const uint64_t err = context->uc_mcontext.gregs[REG_ERR];
#endif
    const bool is_executing = err & 0x10;
    const bool is_writing = err & 0x2;
#endif

    if (!is_executing) {
        if (access_violation_handler(reinterpret_cast<uint8_t *>(info->si_addr), is_writing)) {
            return;
        }
    }

    LOG_CRITICAL("Unhandled access to 0x{:X}", reinterpret_cast<uintptr_t>(info->si_addr));
    raise(SIGTRAP);
    return;
}

static void register_access_violation_handler(const AccessViolationHandler &handler) {
    access_violation_handler = handler;
    struct sigaction sa;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sa.sa_sigaction = signal_handler;
    if (sigaction(SIGSEGV, &sa, NULL) == -1) {
        LOG_CRITICAL("Failed to register an exception handler");
    }
#ifdef __APPLE__
    // When accessing memory region which is PROT_NONE on macOS, it is raising SIGBUS not SIGSEGV.
    // So apply same signal handler to SIGBUS
    if (sigaction(SIGBUS, &sa, NULL) == -1) {
        LOG_CRITICAL("Failed to register an exception handler to SIGBUS");
    }
#endif
}

#endif
