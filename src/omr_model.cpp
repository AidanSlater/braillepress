#include "omr_model.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <stdexcept>

namespace braille {
namespace {

// Tensor and class order for the homr 0.7.0 CPU models. This is the wire
// format of the model, including the classes the score importer rejects.
constexpr char model_suffix[] = "_pytorch_model_396-f6feedb42ff90087d898b0941a55d040fa6b2903.onnx";
constexpr int height = 256, width = 1280, embedding = 512, cache_count = 32;
constexpr int max_tokens = 608;
const std::vector<std::string> rhythms = [] {
    std::vector<std::string> codes{
        "PAD", "BOS", "EOS", "chord", "barline", "doublebarline", "bolddoublebarline",
        "repeatStart", "repeatEnd", "repeatEndStart", "voltaStart", "voltaStop", "voltaDiscontinue",
        "clef_F3", "clef_F4", "clef_F5", "clef_C1", "clef_C2", "clef_C3", "clef_C4", "clef_C5", "clef_G1", "clef_G2"};
    for (int fifths = -7; fifths <= 7; ++fifths)
        codes.push_back("keySignature_" + std::to_string(fifths));
    for (int beat_type : {1, 2, 3, 4, 6, 8, 12, 16, 32, 48})
        codes.push_back("timeSignature/" + std::to_string(beat_type));
    for (int count = 2; count <= 10; ++count)
        codes.push_back("rest_" + std::to_string(count) + "m");
    for (std::string_view prefix : {"note_", "rest_"}) {
        for (int duration : {0, 1, 2, 3, 4, 5, 6, 8, 10, 12, 16, 32, 64, 128}) {
            auto base = std::string(prefix) + std::to_string(duration);
            for (std::string_view grace : {"", "G"})
                for (std::string_view dots : {"", ".", ".."})
                    codes.push_back(base + std::string(grace) + std::string(dots));
        }
        for (int duration : {7, 11, 13, 18, 20, 21, 22, 24, 26, 28, 30, 34, 36, 40, 48, 56, 96})
            codes.push_back(std::string(prefix) + std::to_string(duration));
    }
    return codes;
}();

constexpr std::array<std::string_view, 54> articulations{
    ".", "_", "accent", "accent_arpeggiate", "accent_arpeggiate_fermata", "accent_arpeggiate_staccato",
    "accent_arpeggiate_tenuto", "accent_breathMark", "accent_breathMark_fermata", "accent_fermata",
    "accent_fermata_staccato", "accent_staccatissimo", "accent_staccato", "accent_staccato_tenuto",
    "accent_tenuto", "accent_tremolo", "accent_trill", "accent_fermata_trill", "arpeggiate",
    "arpeggiate_breathMark_fermata", "arpeggiate_fermata", "arpeggiate_fermata_staccato",
    "arpeggiate_staccatissimo", "arpeggiate_staccato", "arpeggiate_staccato_tenuto", "arpeggiate_tenuto",
    "arpeggiate_tremolo", "arpeggiate_trill", "breathMark", "breathMark_fermata", "breathMark_fermata_tenuto",
    "breathMark_staccato", "breathMark_tenuto", "breathMark_trill", "breathMark_tremolo",
    "breathMark_staccato_tenuto", "fermata", "fermata_staccato", "fermata_staccato_tenuto", "fermata_tenuto",
    "fermata_tremolo", "fermata_trill", "fermata_turn", "spiccato", "staccatissimo", "staccato",
    "staccato_tenuto", "staccato_tremolo", "staccato_trill", "staccato_turn", "tenuto", "tremolo", "trill", "turn"};

std::string pitch(int code) {
    if (code < 2)
        return {};
    int index = code - 2;
    return std::string(1, "BAGFEDC"[index % 7]) + std::to_string(9 - index / 7);
}

int maximum_class(const Ort::Value& tensor, std::size_t classes) {
    auto info = tensor.GetTensorTypeAndShapeInfo();
    if (info.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT || info.GetElementCount() != classes)
        throw std::runtime_error("Unexpected decoder output from the homr model");
    const float* begin = tensor.GetTensorData<float>();
    return static_cast<int>(std::max_element(begin, begin + classes) - begin);
}

std::vector<float> input_pixels(const Image& staff) {
    constexpr float mean = .7931f, deviation = .1738f;
    const double scale = std::min(width / double(staff.width), height / double(staff.height));
    const int columns = std::clamp(int(staff.width * scale), 1, width);
    const int rows = std::clamp(int(staff.height * scale), 1, height);
    std::vector<float> canvas(width * height, (1 - mean) / deviation);
    const int top = (height - rows) / 2;
    // The model was trained with bilinear grayscale resizing on a white canvas.
    for (int y = 0; y < rows; ++y) {
        double source_y = std::clamp((y + .5) * staff.height / rows - .5, 0., staff.height - 1.);
        int y0 = int(source_y), y1 = std::min(y0 + 1, staff.height - 1);
        for (int x = 0; x < columns; ++x) {
            double source_x = std::clamp((x + .5) * staff.width / columns - .5, 0., staff.width - 1.);
            int x0 = int(source_x), x1 = std::min(x0 + 1, staff.width - 1);
            auto upper = std::lerp(staff.at(x0, y0), staff.at(x1, y0), source_x - x0);
            auto lower = std::lerp(staff.at(x0, y1), staff.at(x1, y1), source_x - x0);
            auto value = float(std::round(std::lerp(upper, lower, source_y - y0)));
            canvas[(y + top) * width + x] = (value / 255 - mean) / deviation;
        }
    }
    return canvas;
}

} // namespace

StaffRecognizer::StaffRecognizer(const fs::path& model_dir)
    : environment_(ORT_LOGGING_LEVEL_ERROR, "braille-omr") {
    environment_.DisableTelemetryEvents();
    Ort::SessionOptions options;
    options.SetIntraOpNumThreads(2);
    auto encoder = model_dir / (std::string("encoder") + model_suffix);
    auto decoder = model_dir / (std::string("decoder") + model_suffix);
    if (!fs::is_regular_file(encoder) || !fs::is_regular_file(decoder))
        throw std::runtime_error("BRAILLE_OMR_MODELS must contain the two homr 0.7.0 CPU ONNX models");
    encoder_ = Ort::Session(environment_, encoder.c_str(), options);
    decoder_ = Ort::Session(environment_, decoder.c_str(), options);
}

std::vector<ScoreSymbol> StaffRecognizer::read_staff(const Image& staff) {
    const auto memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    Ort::RunOptions run{nullptr};
    auto pixels = input_pixels(staff);
    constexpr std::array<int64_t, 4> image_shape{1, 1, height, width};
    const char* source = "input";
    const char* destination = "output";
    auto image = Ort::Value::CreateTensor<float>(memory, pixels.data(), pixels.size(), image_shape.data(), image_shape.size());
    auto context = encoder_.Run(run, &source, &image, 1, &destination, 1);
    auto shape = context[0].GetTensorTypeAndShapeInfo();
    if (shape.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT ||
        shape.GetShape() != std::vector<int64_t>{1, 1280, embedding})
        throw std::runtime_error("Unexpected encoder output from the homr model");
    std::array<float, embedding> reduced;
    std::copy_n(context[0].GetTensorData<float>(), reduced.size(), reduced.begin());

    std::vector<const char*> input_names{"rhythms", "pitchs", "lifts", "articulations", "slurs", "context", "cache_len"};
    std::vector<const char*> output_names{"out_rhythms", "out_pitchs", "out_lifts", "out_positions", "out_articulations", "out_slurs"};
    std::array<std::string, cache_count> cache_inputs, cache_outputs;
    std::vector<Ort::Value> inputs;
    constexpr std::array<int64_t, 2> token_shape{1, 1};
    constexpr std::array<int64_t, 1> step_shape{1};
    constexpr std::array<int64_t, 3> reduced_shape{1, 1, embedding};
    constexpr std::array<int64_t, 4> empty_cache{1, 8, 0, 64};
    std::array<int64_t, 5> token{1, 0, 0, 0, 0};
    int64_t step = 0;
    float empty = 0;
    for (auto& code : token)
        inputs.push_back(Ort::Value::CreateTensor<int64_t>(memory, &code, 1, token_shape.data(), token_shape.size()));
    inputs.push_back(std::move(context[0]));
    inputs.push_back(Ort::Value::CreateTensor<int64_t>(memory, &step, 1, step_shape.data(), step_shape.size()));
    auto first_cache_input = inputs.size();
    auto first_cache_output = output_names.size();
    for (int i = 0; i < cache_count; ++i) {
        cache_inputs[i] = "cache_in" + std::to_string(i);
        cache_outputs[i] = "cache_out" + std::to_string(i);
        input_names.push_back(cache_inputs[i].c_str());
        output_names.push_back(cache_outputs[i].c_str());
        inputs.push_back(Ort::Value::CreateTensor<float>(memory, &empty, 0, empty_cache.data(), empty_cache.size()));
    }

    std::vector<ScoreSymbol> symbols;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(2);
    for (; step < max_tokens && std::chrono::steady_clock::now() < deadline; ++step) {
        auto outputs = decoder_.Run(run, input_names.data(), inputs.data(), inputs.size(), output_names.data(), output_names.size());
        int rhythm = maximum_class(outputs[0], rhythms.size());
        if (rhythm == 2)
            return symbols;
        if (rhythm < 2)
            throw std::runtime_error("Unexpected control token in recognized staff");
        int note = maximum_class(outputs[1], 72), lift = maximum_class(outputs[2], 7);
        int position = maximum_class(outputs[3], 3), mark = maximum_class(outputs[4], articulations.size());
        int slur = maximum_class(outputs[5], 5);
        symbols.push_back({rhythms[rhythm], pitch(note), articulations[mark], lift, position, slur});
        token = {rhythm, note, lift, mark, slur};
        if (step == 0)
            inputs[5] = Ort::Value::CreateTensor<float>(memory, reduced.data(), reduced.size(), reduced_shape.data(), reduced_shape.size());
        for (int i = 0; i < cache_count; ++i)
            inputs[first_cache_input + i] = std::move(outputs[first_cache_output + i]);
    }
    if (step < max_tokens)
        throw std::runtime_error("Music decoder timed out after two minutes on this staff");
    throw std::runtime_error("Music decoder did not finish within the staff recognition limit");
}

} // namespace braille
