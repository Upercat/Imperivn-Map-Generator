#pragma once
#include <cstdint>
namespace iberivm {
// Native EngineRandom (struct:engine-random, 0x14 bytes, vtable 0x007D8EE8). The game owns a
// primary instance at game+0x12A0 and a secondary at game+0x12B4. Only the 32-bit LCG state
// (+4) influences draws; the constructor 0x0063FEB0 never writes it
// (claim:engine-random-constructor-leaves-state-uninitialized), so callers must set it.
class GameRng {
    std::uint32_t state_=0;
public:
    GameRng()=default;
    explicit GameRng(std::uint32_t state):state_(state) {}
    // Slot +0xC 0x0063FED0 (function:engine-random-set-state).
    void set_state(std::uint32_t state) { state_=state; }
    // Slot +0x10, the shared `return [this+4]` stub 0x00403A00.
    std::uint32_t state() const { return state_; }
    // Slot +0x18 0x0063FEE0: state = _time32(NULL) & 0xFFF
    // (function:engine-random-auto-seed-from-time). The sim never reads wall-clock time,
    // so the caller supplies the time value.
    void auto_seed_from_time(std::uint32_t time_value) { set_state(time_value&0xfffu); }
    // Slot +0x14 0x0063FF00, inclusive Rnd(a, b)
    // (claim:engine-random-next-inclusive-range-draw-semantics): a == b returns a without
    // advancing; a > b swaps the bounds (signed compare); otherwise the state advances and the
    // result is lo + high32(__allmul(span sign-extended, state zero-extended)) with
    // span = hi - lo + 1 computed in 32 bits.
    std::int32_t next(std::int32_t a,std::int32_t b) {
        if (a==b) return a;
        const std::int32_t lo = a<b ? a : b;
        const std::uint32_t range = a<b ? std::uint32_t(b)-std::uint32_t(a) : std::uint32_t(a)-std::uint32_t(b);
        state_=state_*0x19660du+0x3c6ef35fu;
        const std::uint32_t span=range+1u;
        const std::uint64_t span64=std::uint64_t(std::int64_t(std::int32_t(span)));
        const auto high=std::uint32_t((span64*std::uint64_t(state_))>>32);
        return std::int32_t(std::uint32_t(lo)+high);
    }
};
}
