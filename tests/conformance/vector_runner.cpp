#include "vector_runner.h"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <stdexcept>

#include "gekkopak/protocol.h"

namespace gekkopak {
namespace conformance {
namespace {

std::vector<std::string> Split(const std::string& line) {
    std::vector<std::string> out;
    std::istringstream in(line);
    std::string token;
    while (in >> token) {
        out.push_back(token);
    }
    return out;
}

std::uint32_t ParseNumber(const std::string& text, int line) {
    try {
        std::size_t consumed = 0;
        const unsigned long value = std::stoul(text, &consumed, 0);
        if (consumed != text.size()) {
            throw std::invalid_argument("trailing characters");
        }
        return static_cast<std::uint32_t>(value);
    } catch (const std::exception&) {
        throw std::runtime_error("line " + std::to_string(line) + ": bad number '" + text + "'");
    }
}

std::uint8_t ParseHexByte(const std::string& text, int line) {
    if (text.size() != 2) {
        throw std::runtime_error("line " + std::to_string(line) + ": expected a hex byte pair, got '" +
                                 text + "'");
    }
    std::size_t consumed = 0;
    const unsigned long value = std::stoul(text, &consumed, 16);
    if (consumed != 2) {
        throw std::runtime_error("line " + std::to_string(line) + ": bad hex byte '" + text + "'");
    }
    return static_cast<std::uint8_t>(value);
}

std::vector<std::uint8_t> ParseHexBytes(const std::vector<std::string>& tokens, std::size_t first,
                                        int line) {
    std::vector<std::uint8_t> out;
    for (std::size_t i = first; i < tokens.size(); ++i) {
        out.push_back(ParseHexByte(tokens[i], line));
    }
    return out;
}

Step& CurrentStep(Vector* vector, int line, const char* keyword) {
    if (vector == nullptr || vector->steps.empty()) {
        throw std::runtime_error("line " + std::to_string(line) + ": '" + keyword +
                                 "' before any cmd");
    }
    return vector->steps.back();
}

} // namespace

std::vector<Vector> ParseVectorFile(const std::string& path) {
    std::ifstream in(path);
    if (!in) {
        throw std::runtime_error("cannot open vector file: " + path);
    }

    std::vector<Vector> vectors;
    Vector* current = nullptr;
    std::string raw;
    int line = 0;

    while (std::getline(in, raw)) {
        ++line;
        const std::size_t hash = raw.find('#');
        if (hash != std::string::npos) {
            raw = raw.substr(0, hash);
        }
        const std::vector<std::string> tokens = Split(raw);
        if (tokens.empty()) {
            continue;
        }
        const std::string& keyword = tokens[0];

        if (keyword == "vector") {
            if (tokens.size() != 2) {
                throw std::runtime_error("line " + std::to_string(line) + ": vector needs a name");
            }
            vectors.push_back(Vector{});
            current = &vectors.back();
            current->name = tokens[1];
        } else if (current == nullptr) {
            throw std::runtime_error("line " + std::to_string(line) + ": '" + keyword +
                                     "' outside a vector");
        } else if (keyword == "desc") {
            const std::size_t pos = raw.find("desc");
            current->desc = raw.substr(pos + 4);
        } else if (keyword == "end") {
            current = nullptr;
        } else if (keyword == "cmd") {
            if (tokens.size() != 9) {
                throw std::runtime_error("line " + std::to_string(line) +
                                         ": cmd needs exactly 8 hex bytes");
            }
            Step step;
            for (std::size_t i = 0; i < 8; ++i) {
                step.command[i] = ParseHexByte(tokens[i + 1], line);
            }
            step.line = line;
            current->steps.push_back(step);
        } else if (keyword == "data") {
            Step& step = CurrentStep(current, line, "data");
            if (tokens.size() < 2) {
                throw std::runtime_error("line " + std::to_string(line) + ": data needs a direction");
            }
            if (tokens[1] == "none") {
                step.direction = Direction::None;
                step.length = 0;
            } else if (tokens[1] == "in" || tokens[1] == "out") {
                if (tokens.size() != 3) {
                    throw std::runtime_error("line " + std::to_string(line) +
                                             ": data in/out needs a length");
                }
                step.direction = tokens[1] == "in" ? Direction::In : Direction::Out;
                step.length = ParseNumber(tokens[2], line);
                step.payload.assign(step.direction == Direction::In ? step.length : 0, 0);
            } else {
                throw std::runtime_error("line " + std::to_string(line) + ": unknown direction '" +
                                         tokens[1] + "'");
            }
        } else if (keyword == "payload@") {
            Step& step = CurrentStep(current, line, "payload@");
            if (step.direction != Direction::In) {
                throw std::runtime_error("line " + std::to_string(line) +
                                         ": payload@ on a step with no console->cartridge phase");
            }
            const std::uint32_t offset = ParseNumber(tokens.at(1), line);
            const std::vector<std::uint8_t> bytes = ParseHexBytes(tokens, 2, line);
            if (offset + bytes.size() > step.payload.size()) {
                throw std::runtime_error("line " + std::to_string(line) +
                                         ": payload@ runs past the declared length");
            }
            for (std::size_t i = 0; i < bytes.size(); ++i) {
                step.payload[offset + i] = bytes[i];
            }
        } else if (keyword == "expect@") {
            Step& step = CurrentStep(current, line, "expect@");
            ExpectRun run;
            run.offset = ParseNumber(tokens.at(1), line);
            run.bytes = ParseHexBytes(tokens, 2, line);
            step.expects.push_back(run);
        } else if (keyword == "expectcrc") {
            Step& step = CurrentStep(current, line, "expectcrc");
            step.expect_crc = true;
            step.crc = ParseNumber(tokens.at(1), line);
        } else if (keyword == "note") {
            Step& step = CurrentStep(current, line, "note");
            const std::size_t pos = raw.find("note");
            step.note = raw.substr(pos + 4);
        } else {
            throw std::runtime_error("line " + std::to_string(line) + ": unknown keyword '" +
                                     keyword + "'");
        }
    }

    if (current != nullptr) {
        throw std::runtime_error("vector '" + current->name + "' is missing its 'end'");
    }
    return vectors;
}

std::string HexDump(const std::uint8_t* data, std::size_t len, std::size_t max_bytes) {
    std::ostringstream out;
    const std::size_t shown = len < max_bytes ? len : max_bytes;
    for (std::size_t i = 0; i < shown; ++i) {
        char buf[4];
        std::snprintf(buf, sizeof(buf), "%02X", data[i]);
        if (i != 0) {
            out << ' ';
        }
        out << buf;
    }
    if (shown < len) {
        out << " ... (" << len << " bytes)";
    }
    return out.str();
}

namespace {

// Replays one step and returns the response bytes the transport produced.
std::vector<std::uint8_t> IssueStep(const Target& target, const Step& step, bool* carried) {
    std::vector<std::uint8_t> response(step.direction == Direction::Out ? step.length : 0, 0);
    const std::uint8_t* in = step.direction == Direction::In ? step.payload.data() : nullptr;
    const std::size_t in_len = step.direction == Direction::In ? step.payload.size() : 0;
    std::uint8_t* out = response.empty() ? nullptr : response.data();
    *carried = target.issue(step.command, in, in_len, out, response.size());
    return response;
}

void CheckExpectations(const Vector& vector, const Target& target, std::size_t step_index,
                       const Step& step, const std::vector<std::uint8_t>& response,
                       std::vector<Failure>* failures) {
    for (const ExpectRun& run : step.expects) {
        if (run.offset + run.bytes.size() > response.size()) {
            failures->push_back(Failure{vector.name, target.name, static_cast<int>(step_index),
                                        step.line, "expect@ runs past the response"});
            continue;
        }
        for (std::size_t i = 0; i < run.bytes.size(); ++i) {
            if (response[run.offset + i] != run.bytes[i]) {
                std::ostringstream detail;
                detail << "byte " << (run.offset + i) << " expected "
                       << HexDump(run.bytes.data(), run.bytes.size())
                       << " at offset " << run.offset << ", got "
                       << HexDump(response.data() + run.offset,
                                  response.size() - run.offset < run.bytes.size()
                                      ? response.size() - run.offset
                                      : run.bytes.size());
                failures->push_back(Failure{vector.name, target.name, static_cast<int>(step_index),
                                            step.line, detail.str()});
                break;
            }
        }
    }

    if (step.expect_crc) {
        const std::uint32_t actual = protocol::Fnv1a(response.data(), response.size());
        if (actual != step.crc) {
            char buf[128];
            std::snprintf(buf, sizeof(buf), "response FNV-1a 0x%08X, expected 0x%08X", actual,
                          step.crc);
            failures->push_back(Failure{vector.name, target.name, static_cast<int>(step_index),
                                        step.line, buf});
        }
    }
}

} // namespace

void RunVectors(const std::vector<Vector>& vectors, const Target& target,
                std::vector<Failure>* failures) {
    for (const Vector& vector : vectors) {
        target.reset();
        for (std::size_t i = 0; i < vector.steps.size(); ++i) {
            const Step& step = vector.steps[i];
            bool carried = false;
            const std::vector<std::uint8_t> response = IssueStep(target, step, &carried);
            if (!carried) {
                failures->push_back(Failure{vector.name, target.name, static_cast<int>(i),
                                            step.line, "target could not carry the transaction"});
                continue;
            }
            CheckExpectations(vector, target, i, step, response, failures);
        }
    }
}

void RunCrossCheck(const std::vector<Vector>& vectors, const std::vector<Target>& targets,
                   std::vector<Failure>* failures) {
    if (targets.size() < 2) {
        return;
    }
    for (const Vector& vector : vectors) {
        for (const Target& target : targets) {
            target.reset();
        }
        for (std::size_t i = 0; i < vector.steps.size(); ++i) {
            const Step& step = vector.steps[i];
            std::vector<std::vector<std::uint8_t>> responses;
            responses.reserve(targets.size());
            for (const Target& target : targets) {
                bool carried = false;
                responses.push_back(IssueStep(target, step, &carried));
                if (!carried) {
                    failures->push_back(Failure{vector.name, target.name, static_cast<int>(i),
                                                step.line,
                                                "target could not carry the transaction"});
                }
            }
            for (std::size_t t = 1; t < responses.size(); ++t) {
                if (responses[t] == responses[0]) {
                    continue;
                }
                std::ostringstream detail;
                detail << "disagrees with " << targets[0].name << ": " << targets[0].name << "="
                       << HexDump(responses[0].data(), responses[0].size()) << " vs "
                       << targets[t].name << "="
                       << HexDump(responses[t].data(), responses[t].size());
                failures->push_back(Failure{vector.name, targets[t].name, static_cast<int>(i),
                                            step.line, detail.str()});
            }
        }
    }
}

} // namespace conformance
} // namespace gekkopak
