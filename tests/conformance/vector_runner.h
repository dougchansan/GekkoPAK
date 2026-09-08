// Golden GekkoPAK wire-vector format and replay engine.
//
// A vector is a named sequence of NTR transactions expressed purely in wire
// terms -- command bytes, transfer direction, transfer length, expected
// response bytes -- so the same file can be replayed against any transport that
// claims to speak GekkoPAK. Nothing in a vector refers to a device's internal
// state: a status is checked by issuing the F2 that reads the RESULT register,
// exactly as a real client would.

#ifndef GEKKOPAK_CONFORMANCE_VECTOR_RUNNER_H
#define GEKKOPAK_CONFORMANCE_VECTOR_RUNNER_H

#include <cstdint>
#include <string>
#include <vector>

namespace gekkopak {
namespace conformance {

enum class Direction {
    None, // no data phase
    In,   // console -> cartridge (F4)
    Out,  // cartridge -> console (F2, F5)
};

// One expected byte run inside a response.
struct ExpectRun {
    std::size_t offset = 0;
    std::vector<std::uint8_t> bytes;
};

struct Step {
    std::uint8_t command[8]{};
    Direction direction = Direction::None;
    std::size_t length = 0;              // data-phase length in bytes
    std::vector<std::uint8_t> payload;   // console -> cartridge data, zero-padded to length
    std::vector<ExpectRun> expects;      // expected runs within the response
    bool expect_crc = false;
    std::uint32_t crc = 0;               // FNV-1a over the whole response buffer
    std::string note;                    // free text, reported on failure
    int line = 0;
};

struct Vector {
    std::string name;
    std::string desc;
    std::vector<Step> steps;
};

// A transport under test.
struct Target {
    const char* name;
    // Returns the device to a freshly powered state.
    void (*reset)();
    // Issues one transaction. `out` is filled with exactly `out_len` bytes --
    // the full data phase, zero-padded -- for Direction::Out, and ignored
    // otherwise. Returns false if the target could not carry the transaction at
    // all, which is itself a conformance failure.
    bool (*issue)(const std::uint8_t command[8], const std::uint8_t* in, std::size_t in_len,
                  std::uint8_t* out, std::size_t out_len);
};

// Parses one .vec file. Throws std::runtime_error with a line number on a
// syntax error.
std::vector<Vector> ParseVectorFile(const std::string& path);

struct Failure {
    std::string vector_name;
    std::string target_name;
    int step_index = 0;
    int line = 0;
    std::string detail;
};

// Replays every vector against one target, appending any failures.
void RunVectors(const std::vector<Vector>& vectors, const Target& target,
                std::vector<Failure>* failures);

// Replays every vector against every target and additionally checks that all
// targets produced byte-identical responses for every step. Cross-target
// disagreement is reported separately from a failed expectation: one means the
// implementations have drifted, the other means they are all wrong together.
void RunCrossCheck(const std::vector<Vector>& vectors, const std::vector<Target>& targets,
                   std::vector<Failure>* failures);

std::string HexDump(const std::uint8_t* data, std::size_t len, std::size_t max_bytes = 32);

} // namespace conformance
} // namespace gekkopak

#endif // GEKKOPAK_CONFORMANCE_VECTOR_RUNNER_H
