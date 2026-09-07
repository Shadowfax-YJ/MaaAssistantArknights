#include "HudOcr.h"

#include "Config/TaskData.h"
#include "HudOcrRules.h"
#include "Utils/Logger.hpp"
#include "Vision/OCRer.h"

std::optional<int> asst::blackflow::perception::recognize_ingots(const cv::Mat& image)
{
    const auto task = Task.get<OcrTaskInfo>("BlackFlow@Roguelike@CurrentIngots");
    if (task == nullptr || image.empty()) {
        return std::nullopt;
    }
    OCRer analyzer(image);
    analyzer.set_task_info(task);
    if (const auto number_task = Task.get<OcrTaskInfo>("NumberOcrReplace")) {
        analyzer.set_replace(number_task->replace_map);
    }
    // 数字模型不可靠时用文本模型复核同一张图，不沿用低置信度的数值。
    for (const bool use_char_model : { true, false }) {
        analyzer.set_use_char_model(use_char_model);
        const auto results = analyzer.analyze();
        if (!results.has_value() || results->size() != 1) {
            continue;
        }
        const auto& result = results->front();
        if (const auto value = parse_ingots_ocr(result.text, result.score)) {
            return value;
        }
        Log.debug("BlackFlow rejected ingots OCR", result.text, "confidence", result.score);
    }
    return std::nullopt;
}
