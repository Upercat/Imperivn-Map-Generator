// Deterministic random-map pipeline. Phase implementations consume and update one explicit context.
// run() preserves the original entry point; advance() permits pausing at phase boundaries.
#pragma once
#include "rm_phase_prepare.hpp"
#include "rm_phase_landscape.hpp"
#include "rm_phase_objects.hpp"
#include "rm_phase_terrain.hpp"
#include "rm_phase_output.hpp"

namespace iberivm::rm {

class Generator {
public:
    enum class Phase { Initialize, Template, Anchors, Landscape, Objects, Heights, Terrain, Shipyards, Output, Complete, Failed };
    using ObjectObserver = iberivm::rm::ObjectObserver;

    Generator(const CaseSettings& cs, const GameFS& fs, rmdata::ClassData& classes,
              const rmdata::SettlementLibraryFiles& libfiles, StageSink sink,
              GenerationOptions options = {}, GenerationResources* resources = nullptr)
        : state_(cs, fs, classes, libfiles, std::move(sink), options, resources) {}

    void set_object_observer(ObjectObserver observer) { state_.object_observer_ = std::move(observer); }
    const DrawLog& rng() const { return state_.rng_; }
    void set_class_attrs(std::string text) { state_.class_attrs_json_ = std::move(text); }
    void set_class_attrs_table(ClassAttrs table) { state_.class_attrs_ref_ = nullptr; state_.class_attrs_table_ = std::move(table); }
    // The referenced table must outlive this generator.
    void set_class_attrs_table_ref(const ClassAttrs& table) { state_.class_attrs_ref_ = &table; state_.class_attrs_table_.reset(); }
    static float bits_float(std::uint32_t bits) { return GenerationState::bits_float(bits); }

    // Read-only inspection: the next phase receives this same context, with no grid copies or reseeding.
    const GenerationState& state() const { return state_; }
    Phase next_phase() const { return next_; }

    // True when one phase ran, false when already complete. A failed phase cannot be retried on partial state.
    bool advance() {
        if (next_ == Phase::Complete) return false;
        if (next_ == Phase::Failed) throw std::logic_error("generation already failed");
        try {
            switch (next_) {
            case Phase::Initialize:
                generation::initialize(state_);
                next_ = state_.empty_template ? Phase::Output : Phase::Template;
                break;
            case Phase::Template: generation::load_template(state_); next_ = Phase::Anchors; break;
            case Phase::Anchors: generation::place_anchors(state_); next_ = Phase::Landscape; break;
            case Phase::Landscape: generation::build_landscape(state_); next_ = Phase::Objects; break;
            case Phase::Objects: generation::place_objects(state_); next_ = Phase::Heights; break;
            case Phase::Heights: generation::build_heights(state_); next_ = Phase::Terrain; break;
            case Phase::Terrain: generation::paint_terrain(state_); next_ = Phase::Shipyards; break;
            case Phase::Shipyards: generation::place_shipyards(state_); next_ = Phase::Output; break;
            case Phase::Output: generation::write_output(state_); next_ = Phase::Complete; break;
            default: throw std::logic_error("invalid generation phase");
            }
        } catch (...) {
            next_ = Phase::Failed;
            throw;
        }
        return true;
    }

    void run() { while (advance()) {} }

private:
    GenerationState state_;
    Phase next_ = Phase::Initialize;
};

}  // namespace iberivm::rm
