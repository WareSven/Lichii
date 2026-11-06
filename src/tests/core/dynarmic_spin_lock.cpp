// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <atomic>
#include <barrier>
#include <cstddef>
#include <cstdint>
#include <thread>
#include <catch2/catch_test_macros.hpp>
#include "dynarmic/common/spin_lock.h"
#include "dynarmic/common/spin_lock_x64.h"
#include "dynarmic/interface/exclusive_monitor.h"

namespace {

constexpr std::size_t WorkerCount = 6;
constexpr std::uint64_t Iterations = 1'000;
constexpr std::uint64_t PayloadMask = 0x749a'5c13'82df'b601;

class EmittedLock {
public:
    explicit EmittedLock(bool extended_registers) {
#ifdef _WIN32
        const auto argument = code.rcx;
#else
        const auto argument = code.rdi;
#endif
        const auto pointer = extended_registers ? code.r10 : code.rdx;
        const auto temporary = extended_registers ? code.r11d : code.eax;
        lock = code.getCurr<LockFunction>();
        code.mov(pointer, argument);
        Dynarmic::EmitSpinLockLock(code, pointer, temporary, false);
        code.mov(code.eax, temporary);
        code.ret();
        unlock = code.getCurr<UnlockFunction>();
        code.mov(pointer, argument);
        Dynarmic::EmitSpinLockUnlock(code, pointer, temporary);
        code.ret();
        code.ready(Xbyak::CodeArray::PROTECT_RE);
        code.setProtectMode(Xbyak::CodeArray::PROTECT_RE);
    }

    using LockFunction = std::uint32_t (*)(volatile int*);
    using UnlockFunction = void (*)(volatile int*);
    Xbyak::CodeGenerator code{4096, Xbyak::DontSetProtectRWE};
    LockFunction lock{};
    UnlockFunction unlock{};
};

template <typename Lock, typename Unlock>
void CheckContention(Lock lock, Unlock unlock) {
    std::barrier start{static_cast<std::ptrdiff_t>(WorkerCount)};
    std::atomic<unsigned> active{};
    std::atomic<unsigned> failures{};
    // Relaxed atomics keep a broken-lock test defined; the lock must publish the pair.
    std::atomic<std::uint64_t> sequence{};
    std::atomic<std::uint64_t> payload{PayloadMask};
    std::array<std::thread, WorkerCount> workers;
    for (auto& worker : workers) {
        worker = std::thread{[&] {
            start.arrive_and_wait();
            for (std::uint64_t i = 0; i < Iterations; ++i) {
                lock();
                if (active.fetch_add(1, std::memory_order_relaxed) != 0) {
                    failures.fetch_add(1, std::memory_order_relaxed);
                }
                const auto previous = sequence.load(std::memory_order_relaxed);
                if (payload.load(std::memory_order_relaxed) != (previous ^ PayloadMask)) {
                    failures.fetch_add(1, std::memory_order_relaxed);
                }
                if (i % 128 == 0) {
                    std::this_thread::yield();
                }
                sequence.store(previous + 1, std::memory_order_relaxed);
                payload.store((previous + 1) ^ PayloadMask, std::memory_order_relaxed);
                active.fetch_sub(1, std::memory_order_relaxed);
                unlock();
            }
        }};
    }
    for (auto& worker : workers) {
        worker.join();
    }
    CHECK(failures.load() == 0);
    CHECK(active.load() == 0);
    CHECK(sequence.load() == WorkerCount * Iterations);
    CHECK(payload.load() == ((WorkerCount * Iterations) ^ PayloadMask));
}

} // namespace

TEST_CASE("Dynarmic emitted PAUSE locks exclude and publish with low and extended registers",
          "[dynarmic][spin_lock]") {
    for (const bool extended_registers : {false, true}) {
        CAPTURE(extended_registers);
        EmittedLock code{extended_registers};
        alignas(64) volatile int storage{};
        CHECK(code.lock(&storage) == 0);
        CHECK(storage == 1);
        code.unlock(&storage);
        CHECK(storage == 0);
        CheckContention([&] { code.lock(&storage); }, [&] { code.unlock(&storage); });
        CHECK(storage == 0);
    }
}

TEST_CASE("Dynarmic native PAUSE lock excludes and publishes under contention",
          "[dynarmic][spin_lock]") {
    Dynarmic::SpinLock lock;
    CheckContention([&] { lock.Lock(); }, [&] { lock.Unlock(); });
    CHECK(lock.storage == 0);
}

TEST_CASE("Dynarmic exclusive monitor preserves reservations across cores",
          "[dynarmic][spin_lock]") {
    Dynarmic::ExclusiveMonitor monitor{WorkerCount};
    constexpr Dynarmic::VAddr address = 0x1000;
    std::uint64_t value = 7;
    CHECK(monitor.ReadAndMark<std::uint64_t>(0, address, [&] { return value; }) == 7);
    CHECK(monitor.ReadAndMark<std::uint64_t>(1, address, [&] { return value; }) == 7);
    std::uint64_t observed{};
    CHECK(monitor.DoExclusiveOperation<std::uint64_t>(1, address, [&](auto saved) {
        observed = saved;
        ++value;
        return true;
    }));
    CHECK(observed == 7);
    bool called = false;
    CHECK_FALSE(monitor.DoExclusiveOperation<std::uint64_t>(0, address, [&](auto) {
        called = true;
        return true;
    }));
    CHECK_FALSE(called);
    CHECK(value == 8);
    CHECK(monitor.ReadAndMark<std::uint64_t>(0, address, [&] { return value; }) == 8);
    CHECK(monitor.DoExclusiveOperation<std::uint64_t>(0, address, [&](auto saved) {
        observed = saved;
        return true;
    }));
    CHECK(observed == 8);
}
