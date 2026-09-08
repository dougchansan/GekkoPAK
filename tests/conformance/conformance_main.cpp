// GekkoPAK golden-vector conformance runner.
//
// Replays tests/conformance/vectors/*.vec against every host-buildable
// GekkoPAK transport and reports two kinds of failure separately:
//
//   - a target that does not match the golden response (the protocol changed,
//     or that target is wrong);
//   - two targets that do not match each other (the implementations have
//     drifted apart, which is the failure this whole exercise exists to catch).

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <string>
#include <vector>

#include "targets.h"
#include "vector_runner.h"

#ifndef GEKKOPAK_VECTOR_DIR
#define GEKKOPAK_VECTOR_DIR "tests/conformance/vectors"
#endif

using namespace gekkopak::conformance;

namespace {

// Kept as an explicit list rather than a directory scan: a vector file that
// silently stops being run is worse than one that fails.
const char* kVectorFiles[] = {
    "lifecycle.vec",
    "errors.vec",
    "block.vec",
};

void Report(const std::vector<Failure>& failures, const char* heading) {
    if (failures.empty()) {
        return;
    }
    std::printf("\n%s (%zu):\n", heading, failures.size());
    for (const Failure& f : failures) {
        std::printf("  %-14s %-14s step %d (line %d): %s\n", f.vector_name.c_str(),
                    f.target_name.c_str(), f.step_index, f.line, f.detail.c_str());
    }
}

} // namespace

int main(int argc, char** argv) {
    const std::string dir = argc > 1 ? argv[1] : GEKKOPAK_VECTOR_DIR;

    std::vector<Vector> vectors;
    try {
        for (const char* name : kVectorFiles) {
            const std::string path = dir + "/" + name;
            std::vector<Vector> parsed = ParseVectorFile(path);
            std::printf("loaded %-16s %zu vectors\n", name, parsed.size());
            vectors.insert(vectors.end(), parsed.begin(), parsed.end());
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "vector parse failed: %s\n", e.what());
        return 2;
    }

    std::size_t steps = 0;
    for (const Vector& v : vectors) {
        steps += v.steps.size();
    }
    const std::vector<Target>& targets = AllTargets();
    std::printf("\n%zu vectors, %zu wire transactions, %zu targets\n", vectors.size(), steps,
                targets.size());

    std::vector<Failure> golden_failures;
    for (const Target& target : targets) {
        const std::size_t before = golden_failures.size();
        RunVectors(vectors, target, &golden_failures);
        std::printf("  %-14s %s\n", target.name,
                    golden_failures.size() == before
                        ? "PASS"
                        : ("FAIL (" + std::to_string(golden_failures.size() - before) + ")").c_str());
    }

    std::vector<Failure> drift_failures;
    RunCrossCheck(vectors, targets, &drift_failures);
    std::printf("  %-14s %s\n", "cross-check",
                drift_failures.empty()
                    ? "PASS (all targets byte-identical)"
                    : ("FAIL (" + std::to_string(drift_failures.size()) + ")").c_str());

    Report(golden_failures, "Golden mismatches");
    Report(drift_failures, "Cross-target drift");

    if (!golden_failures.empty() || !drift_failures.empty()) {
        std::printf("\nCONFORMANCE FAIL\n");
        return 1;
    }
    std::printf("\nCONFORMANCE PASS\n");
    return 0;
}
