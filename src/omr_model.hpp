#pragma once

#include "braille.hpp"

#include <onnxruntime_cxx_api.h>

namespace braille {

struct ScoreSymbol {
    std::string rhythm, pitch;
    std::string_view articulation;
    int lift, position, slur;
};

class StaffRecognizer {
public:
    explicit StaffRecognizer(const fs::path& model_dir);
    std::vector<ScoreSymbol> read_staff(const Image& staff);

private:
    Ort::Env environment_;
    Ort::Session encoder_{nullptr}, decoder_{nullptr};
};

} // namespace braille
