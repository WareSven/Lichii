// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <functional>
#include <mutex>
#include <thread>
#include <catch2/catch_test_macros.hpp>

#include "common/common_types.h"
#include "dynarmic/backend/x64/exclusive_monitor_friend.h"
#include "dynarmic/interface/A64/a64.h"
#include "dynarmic/interface/exclusive_monitor.h"

namespace {
using namespace std::chrono_literals;
constexpr u64 DataAddress = 0x2000;

struct Environment final : Dynarmic::A64::UserCallbacks {
    std::array<std::atomic<u64>, 2>& memory;
    unsigned size;
    bool acquire;
    Dynarmic::A64::Jit* jit{};
    std::function<void()> on_read;
    bool pause_before_load{};
    bool unexpected{};
    unsigned reads{};

    Environment(std::array<std::atomic<u64>, 2>& memory_, unsigned size_, bool acquire_)
        : memory{memory_}, size{size_}, acquire{acquire_} {}

    std::optional<u32> MemoryReadCode(u64 address) override {
        const u32 width = size == 8 ? 3 : size == 4 ? 2 : size == 2 ? 1 : 0;
        switch (address) {
        case 0:
            if (size == 16) {
                return 0xc87f1020; // LDXP X0,X4,[X1]
            }
            return 0x085f7c20 | (width << 30) | (acquire ? 0x8000 : 0); // LD[A]XR X0,[X1]
        case 8:
            if (size == 16) {
                return 0xc8221023; // STXP W2,X3,X4,[X1]
            }
            return 0x08027c23 | (width << 30); // STXR W2,X3,[X1]
        default:
            return 0xd503201f; // NOP; Step executes only the requested instruction.
        }
    }

    u64 MemoryRead(u64 address, size_t) override {
        ++reads;
        if (on_read && pause_before_load) {
            on_read();
        }
        const u64 value = memory.at((address - DataAddress) / 8).load();
        if (on_read && !pause_before_load) {
            on_read();
        }
        return value;
    }
    Dynarmic::A64::Vector MemoryRead128(u64) override {
        ++reads;
        const Dynarmic::A64::Vector value{memory[0].load(), memory[1].load()};
        if (on_read) {
            on_read();
        }
        return value;
    }
    void MemoryWrite(u64, u64, size_t) override {
        unexpected = true;
    }
    void MemoryWrite128(u64, Dynarmic::A64::Vector) override {
        unexpected = true;
    }
    bool Store(u64 address, u64 value, u64 expected) {
        return memory.at((address - DataAddress) / 8).compare_exchange_strong(expected, value);
    }
    bool MemoryWriteExclusive8(u64 a, u8 v, u8 e) override {
        return Store(a, v, e);
    }
    bool MemoryWriteExclusive16(u64 a, u16 v, u16 e) override {
        return Store(a, v, e);
    }
    bool MemoryWriteExclusive32(u64 a, u32 v, u32 e) override {
        return Store(a, v, e);
    }
    bool MemoryWriteExclusive64(u64 a, u64 v, u64 e) override {
        return Store(a, v, e);
    }
    bool MemoryWriteExclusive128(u64, Dynarmic::A64::Vector value,
                                 Dynarmic::A64::Vector expected) override {
        if (memory[0] != expected[0] || memory[1] != expected[1]) {
            return false;
        }
        memory[0] = value[0];
        memory[1] = value[1];
        return true;
    }
    void CallSVC(u32) override {
        unexpected = true;
        jit->HaltExecution();
    }
    void ExceptionRaised(u64, Dynarmic::A64::Exception) override {
        unexpected = true;
        jit->HaltExecution();
    }
    void AddTicks(u64) override {}
    u64 GetTicksRemaining() override {
        return 1000;
    }
    u64 GetCNTPCT() override {
        return 0;
    }
};

Dynarmic::A64::UserConfig Config(Environment& env, Dynarmic::ExclusiveMonitor& monitor,
                                 const std::atomic<bool>* flag, void* fastmem) {
    Dynarmic::A64::UserConfig config;
    config.callbacks = &env;
    config.global_monitor = &monitor;
    config.exclusive_read_release_early = flag;
    config.fastmem_pointer = reinterpret_cast<uintptr_t>(fastmem);
    config.fastmem_address_space_bits = 12;
    config.silently_mirror_fastmem = false;
    config.fastmem_exclusive_access = true;
    config.recompile_on_exclusive_fastmem_failure = false;
    config.enable_cycle_counting = false;
    return config;
}

struct Gate {
    std::mutex mutex;
    std::condition_variable cv;
    bool entered{};
    bool done{};
    bool during_read{};
    bool worker_started{};

    void Read() {
        std::unique_lock lock{mutex};
        entered = true;
        cv.notify_all();
        if (cv.wait_for(lock, 2s, [&] { return worker_started; })) {
            during_read = cv.wait_for(lock, 250ms, [&] { return done; });
        }
    }
    template <typename F>
    void Work(F action) {
        {
            std::unique_lock lock{mutex};
            if (!cv.wait_for(lock, 2s, [&] { return entered; })) {
                return;
            }
            worker_started = true;
            cv.notify_all();
        }
        action();
        std::lock_guard lock{mutex};
        done = true;
        cv.notify_all();
    }
};

int LockState(Dynarmic::ExclusiveMonitor& monitor) {
    auto* storage = const_cast<int*>(Dynarmic::GetExclusiveMonitorLockPointer(&monitor));
    return std::atomic_ref<int>{*storage}.load();
}

void Read(Dynarmic::A64::Jit& jit, u64 address = DataAddress) {
    jit.SetPC(0);
    jit.SetRegister(1, address);
    jit.Step();
}

u64 Store(Dynarmic::A64::Jit& jit) {
    jit.SetPC(8);
    jit.SetRegister(3, 9);
    jit.Step();
    return jit.GetRegister(2);
}
} // namespace

TEST_CASE("Exclusive read releases its monitor while an inline fallback waits",
          "[dynarmic][exclusive_read_release]") {
    for (const unsigned size : {1U, 2U, 4U, 8U}) {
        for (const bool early : {false, true}) {
            CAPTURE(size, early);
            std::array<std::atomic<u64>, 2> memory{7, 11};
            std::array<u8, 4096> fastmem{};
            Dynarmic::ExclusiveMonitor monitor{2};
            std::atomic<bool> flag{early};
            Environment env{memory, size, true};
            Dynarmic::A64::Jit jit{Config(env, monitor, &flag, fastmem.data())};
            env.jit = &jit;
            Gate gate;
            env.on_read = [&] { gate.Read(); };
            bool stored{};
            std::thread other{[&] {
                gate.Work([&] {
                    monitor.ReadAndMark<u64>(1, DataAddress + 8, [&] { return memory[1].load(); });
                    stored =
                        monitor.DoExclusiveOperation<u64>(1, DataAddress + 8, [&](u64 expected) {
                            return memory[1].compare_exchange_strong(expected, 12);
                        });
                });
            }};
            Read(jit);
            other.join();
            REQUIRE(gate.worker_started);
            REQUIRE(gate.during_read == early);
            REQUIRE(stored);
            REQUIRE(env.reads == 1);
            REQUIRE_FALSE(env.unexpected);
            REQUIRE(jit.GetRegister(0) == 7);
            REQUIRE(Store(jit) == 0);
            REQUIRE(memory[0] == 9);
        }
    }
}

TEST_CASE("Exclusive read snapshot cannot resurrect an invalidated reservation",
          "[dynarmic][exclusive_read_release]") {
    for (const unsigned scenario : {0U, 1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U, 9U}) {
        const unsigned operation = scenario % 5;
        const bool before_load = scenario >= 5;
        CAPTURE(operation, before_load);
        std::array<std::atomic<u64>, 2> memory{7, 11};
        std::array<u8, 4096> fastmem{};
        Dynarmic::ExclusiveMonitor monitor{2};
        std::atomic<bool> flag{true};
        Environment env{memory, 8, false};
        env.pause_before_load = before_load;
        Dynarmic::A64::Jit jit{Config(env, monitor, &flag, fastmem.data())};
        env.jit = &jit;
        monitor.ReadAndMark<u64>(1, DataAddress, [&] { return memory[0].load(); });
        Gate gate;
        env.on_read = [&] { gate.Read(); };
        bool stored = true;
        std::thread other{[&] {
            gate.Work([&] {
                if (operation == 2) {
                    monitor.ClearProcessor(0);
                } else if (operation == 3) {
                    monitor.Clear();
                } else {
                    stored = monitor.DoExclusiveOperation<u64>(1, DataAddress, [&](u64 expected) {
                        return memory[0].compare_exchange_strong(expected, operation == 0 ? 7 : 8);
                    });
                    if (operation == 1) {
                        monitor.ReadAndMark<u64>(1, DataAddress, [&] { return memory[0].load(); });
                        stored &=
                            monitor.DoExclusiveOperation<u64>(1, DataAddress, [&](u64 expected) {
                                return memory[0].compare_exchange_strong(expected, 7);
                            });
                    }
                }
            });
        }};
        Read(jit);
        other.join();
        REQUIRE(gate.during_read);
        REQUIRE(stored);
        REQUIRE(jit.GetRegister(0) == (operation == 4 && before_load ? 8 : 7));
        REQUIRE(Store(jit) == 1);
        REQUIRE(memory[0] == (operation == 4 ? 8 : 7));
        env.on_read = {};
        Read(jit);
        REQUIRE(Store(jit) == 0);
        REQUIRE_FALSE(env.unexpected);
    }
}

TEST_CASE("An early exclusive read cannot unlock a subsequent monitor owner",
          "[dynarmic][exclusive_read_release]") {
    std::array<std::atomic<u64>, 2> memory{7, 11};
    std::array<u8, 4096> fastmem{};
    Dynarmic::ExclusiveMonitor monitor{2};
    std::atomic<bool> flag{true};
    Environment env{memory, 8, false};
    Dynarmic::A64::Jit jit{Config(env, monitor, &flag, fastmem.data())};
    env.jit = &jit;
    std::mutex mutex;
    std::condition_variable cv;
    bool callback_entered{}, other_owns_lock{}, release_other{};
    bool acquired_during_read{}, worker_completed{};
    env.on_read = [&] {
        std::unique_lock lock{mutex};
        flag.store(false);
        callback_entered = true;
        cv.notify_all();
        acquired_during_read = cv.wait_for(lock, 2s, [&] { return other_owns_lock; });
    };
    std::thread other{[&] {
        {
            std::unique_lock lock{mutex};
            if (!cv.wait_for(lock, 2s, [&] { return callback_entered; })) {
                return;
            }
        }
        monitor.ReadAndMark<u64>(1, DataAddress + 8, [&] {
            std::unique_lock lock{mutex};
            other_owns_lock = true;
            cv.notify_all();
            worker_completed = cv.wait_for(lock, 2s, [&] { return release_other; });
            return memory[1].load();
        });
    }};
    Read(jit);
    const int subsequent_owner_lock = LockState(monitor);
    {
        std::lock_guard lock{mutex};
        release_other = true;
        cv.notify_all();
    }
    other.join();
    REQUIRE(acquired_during_read);
    REQUIRE(worker_completed);
    REQUIRE(subsequent_owner_lock == 1);
    REQUIRE(LockState(monitor) == 0);
    REQUIRE(Store(jit) == 0);
    REQUIRE_FALSE(env.unexpected);
}

TEST_CASE("Exclusive read captures a live toggle once per operation",
          "[dynarmic][exclusive_read_release]") {
    for (const bool initial : {false, true}) {
        CAPTURE(initial);
        std::array<std::atomic<u64>, 2> memory{7, 11};
        std::array<u8, 4096> fastmem{};
        Dynarmic::ExclusiveMonitor monitor{2};
        std::atomic<bool> flag{initial};
        Environment env{memory, 8, false};
        Dynarmic::A64::Jit jit{Config(env, monitor, &flag, fastmem.data())};
        env.jit = &jit;
        int callback_lock = -1;
        env.on_read = [&] {
            callback_lock = LockState(monitor);
            flag.store(!initial);
        };
        Read(jit);
        REQUIRE(callback_lock == (initial ? 0 : 1));
        REQUIRE(LockState(monitor) == 0);
        REQUIRE(Store(jit) == 0);
        REQUIRE_FALSE(env.unexpected);
    }
}

TEST_CASE("A null exclusive read experiment preserves the legacy callback lock",
          "[dynarmic][exclusive_read_release]") {
    std::array<std::atomic<u64>, 2> memory{7, 11};
    std::array<u8, 4096> fastmem{};
    Dynarmic::ExclusiveMonitor monitor{2};
    Environment env{memory, 8, false};
    Dynarmic::A64::Jit jit{Config(env, monitor, nullptr, fastmem.data())};
    env.jit = &jit;
    Gate gate;
    env.on_read = [&] { gate.Read(); };
    std::thread other{[&] { gate.Work([&] { monitor.ClearProcessor(1); }); }};
    Read(jit);
    other.join();
    REQUIRE(gate.worker_started);
    REQUIRE_FALSE(gate.during_read);
    REQUIRE(Store(jit) == 0);
}

TEST_CASE("The exclusive read experiment leaves 128-bit reads locked",
          "[dynarmic][exclusive_read_release]") {
    std::array<std::atomic<u64>, 2> memory{7, 11};
    std::array<u8, 4096> fastmem{};
    Dynarmic::ExclusiveMonitor monitor{2};
    std::atomic<bool> flag{true};
    Environment env{memory, 16, false};
    Dynarmic::A64::Jit jit{Config(env, monitor, &flag, fastmem.data())};
    env.jit = &jit;
    int callback_lock = -1;
    env.on_read = [&] {
        callback_lock = LockState(monitor);
        flag.store(false);
    };
    Read(jit);
    REQUIRE(callback_lock == 1);
    REQUIRE(LockState(monitor) == 0);
    REQUIRE(jit.GetRegister(0) == 7);
    REQUIRE(jit.GetRegister(4) == 11);
    REQUIRE(Store(jit) == 0);
    REQUIRE(memory[0] == 9);
    REQUIRE(memory[1] == 11);
    REQUIRE_FALSE(env.unexpected);
}

TEST_CASE("Exclusive read live toggles also preserve direct fastmem pairs",
          "[dynarmic][exclusive_read_release]") {
    std::array<std::atomic<u64>, 2> memory{};
    alignas(16) std::array<u8, 4096> fastmem{};
    Dynarmic::ExclusiveMonitor monitor{2};
    std::atomic<bool> flag{false};
    Environment env{memory, 8, false};
    Dynarmic::A64::Jit jit{Config(env, monitor, &flag, fastmem.data())};
    env.jit = &jit;
    for (const bool early : {false, true, false}) {
        flag.store(early);
        const u64 input = 7;
        std::memcpy(fastmem.data() + 0x100, &input, sizeof(input));
        Read(jit, 0x100);
        REQUIRE(jit.GetRegister(0) == input);
        REQUIRE(LockState(monitor) == 0);
        REQUIRE(Store(jit) == 0);
        u64 output{};
        std::memcpy(&output, fastmem.data() + 0x100, sizeof(output));
        REQUIRE(output == 9);
    }
    REQUIRE(env.reads == 0);
    REQUIRE_FALSE(env.unexpected);
}
