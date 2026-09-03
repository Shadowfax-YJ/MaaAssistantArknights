#include "BlackFlowMapObservationSource.h"

#include "BlackFlowDiagnosticTimeline.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string_view>

#include <opencv2/imgcodecs.hpp>

#include "Config/Roguelike/BlackFlow/BlackFlowMapPerceptionResource.h"
#include "MaaUtils/ImageIo.h"
#include "Utils/Logger.hpp"
#include "Utils/WorkingDir.hpp"

namespace asst::blackflow
{
namespace
{
void set_error(std::string* error, std::string message)
{
    if (error != nullptr) {
        *error = std::move(message);
    }
}

Rect visual_rect(const perception::Node& node, const cv::Size& bounds)
{
    const int left = static_cast<int>(std::floor(node.visual_center.x - node.visual_half_width));
    const int top = static_cast<int>(std::floor(node.visual_center.y - node.visual_half_height));
    const int right = static_cast<int>(std::ceil(node.visual_center.x + node.visual_half_width));
    const int bottom = static_cast<int>(std::ceil(node.visual_center.y + node.visual_half_height));
    const cv::Rect clipped = cv::Rect(left, top, std::max(1, right - left), std::max(1, bottom - top)) &
                             cv::Rect(0, 0, bounds.width, bounds.height);
    return { clipped.x, clipped.y, clipped.width, clipped.height };
}

bool valid_artifact_set_id(std::string_view id)
{
    return !id.empty() && std::ranges::all_of(id, [](unsigned char character) {
        return std::isalnum(character) != 0 || character == '-' || character == '_';
    });
}

std::string current_run_timestamp()
{
    const auto now = std::chrono::system_clock::now();
    const std::time_t wall_clock = std::chrono::system_clock::to_time_t(now);
    std::tm local_time {};
#ifdef _WIN32
    localtime_s(&local_time, &wall_clock);
#else
    localtime_r(&wall_clock, &local_time);
#endif
    const auto microseconds =
        std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()) % std::chrono::seconds(1);
    std::ostringstream result;
    result << std::put_time(&local_time, "%Y%m%d-%H%M%S") << '-' << std::setfill('0') << std::setw(6)
           << microseconds.count();
    return result.str();
}

bool write_image_if_present(const std::filesystem::path& path, const cv::Mat& image, std::string* error)
{
    if (image.empty()) {
        return true;
    }
    const std::vector<int> parameters { cv::IMWRITE_JPEG_QUALITY, 78, cv::IMWRITE_JPEG_OPTIMIZE, 1 };
    if (!MAA_NS::imwrite(path, image, parameters)) {
        set_error(error, "failed to write BlackFlow diagnostic image: " + path.string());
        return false;
    }
    return true;
}

bool write_captured_preview_if_present(const std::filesystem::path& path, const cv::Mat& image, std::string* error)
{
    if (image.empty()) {
        return true;
    }
    const std::vector<int> parameters { cv::IMWRITE_JPEG_QUALITY, 78, cv::IMWRITE_JPEG_OPTIMIZE, 1 };
    if (!MAA_NS::imwrite(path, image, parameters)) {
        set_error(error, "failed to write BlackFlow diagnostic captured preview: " + path.string());
        return false;
    }
    return true;
}

std::optional<json::object> enrich_routing_snapshot_with_processing_evidence(
    const std::string& routing_snapshot,
    const std::vector<std::string>& processing_snapshots)
{
    const auto parsed_routing = json::parse(routing_snapshot);
    if (!parsed_routing.has_value() || !parsed_routing->is_object()) {
        return std::nullopt;
    }

    std::vector<json::object> processing_entries;
    std::vector<DiagnosticEvidenceStamp> processing_stamps;
    processing_entries.reserve(processing_snapshots.size());
    processing_stamps.reserve(processing_snapshots.size());
    for (const std::string& serialized : processing_snapshots) {
        const auto parsed = json::parse(serialized);
        if (parsed.has_value() && parsed->is_object()) {
            const json::object& evidence = parsed->as_object();
            processing_stamps.emplace_back(
                DiagnosticEvidenceStamp {
                    evidence.get("floor", 0),
                    evidence.get("map_generation", std::uint64_t { 0 }),
                    evidence.get("diagnostic_sequence", std::uint64_t { 0 }),
                    evidence.get("evidence_type", std::string {}),
                });
            processing_entries.emplace_back(evidence);
        }
    }

    json::object routing = parsed_routing->as_object();
    const int decision_floor = routing.get("floor", 0);
    const std::uint64_t decision_map_generation =
        routing.get("map_generation", std::uint64_t { 0 });
    const std::uint64_t decision_sequence = routing.get("diagnostic_sequence", std::uint64_t { 0 });
    std::vector<json::value> evidence_at_step;
    const auto evidence_indexes = diagnostic_latest_evidence_by_type(
        processing_stamps,
        decision_floor,
        decision_map_generation,
        decision_sequence);
    evidence_at_step.reserve(evidence_indexes.size());
    for (const std::size_t evidence_index : evidence_indexes) {
        evidence_at_step.emplace_back(processing_entries[evidence_index]);
    }
    routing["processing_evidence"] = json::array(std::move(evidence_at_step));
    return routing;
}

bool write_text_file(
    const std::filesystem::path& path,
    std::string_view content,
    std::string_view description,
    std::string* error)
{
    std::ofstream output(path, std::ios::out | std::ios::trunc | std::ios::binary);
    if (!output) {
        set_error(error, "failed to create BlackFlow " + std::string(description));
        return false;
    }
    output.write(content.data(), static_cast<std::streamsize>(content.size()));
    output.flush();
    if (!output) {
        set_error(error, "failed to flush BlackFlow " + std::string(description));
        return false;
    }
    return true;
}

bool append_framed_json_array_entry(
    const std::filesystem::path& path,
    std::string_view prefix,
    std::string_view suffix,
    std::string_view serialized_entry,
    std::string_view description,
    std::string* error)
{
    std::fstream output(path, std::ios::in | std::ios::out | std::ios::binary);
    if (!output) {
        set_error(error, "failed to open BlackFlow " + std::string(description));
        return false;
    }

    output.seekg(0, std::ios::end);
    const std::streamoff file_size = output.tellg();
    const std::streamoff empty_size =
        static_cast<std::streamoff>(prefix.size() + suffix.size());
    if (file_size < empty_size) {
        set_error(error, "invalid BlackFlow " + std::string(description) + " frame");
        return false;
    }

    std::string actual_prefix(prefix.size(), '\0');
    output.seekg(0, std::ios::beg);
    output.read(actual_prefix.data(), static_cast<std::streamsize>(actual_prefix.size()));
    std::string actual_suffix(suffix.size(), '\0');
    output.seekg(-static_cast<std::streamoff>(suffix.size()), std::ios::end);
    output.read(actual_suffix.data(), static_cast<std::streamsize>(actual_suffix.size()));
    if (!output || actual_prefix != prefix || actual_suffix != suffix) {
        set_error(error, "invalid BlackFlow " + std::string(description) + " frame");
        return false;
    }

    output.clear();
    output.seekp(-static_cast<std::streamoff>(suffix.size()), std::ios::end);
    if (file_size != empty_size) {
        output.put(',');
    }
    output.write(serialized_entry.data(), static_cast<std::streamsize>(serialized_entry.size()));
    output.write(suffix.data(), static_cast<std::streamsize>(suffix.size()));
    output.flush();
    if (!output) {
        set_error(error, "failed to flush BlackFlow " + std::string(description));
        return false;
    }
    return true;
}

bool write_routing_visualization(const std::filesystem::path& path, std::string* error)
{
    std::ofstream output(path, std::ios::out | std::ios::trunc | std::ios::binary);
    if (!output) {
        set_error(error, "failed to create BlackFlow routing visualization");
        return false;
    }
    output << R"HTML(<!doctype html>
<html lang="zh-CN"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>黑流树海路线规划诊断</title>
<style>
:root{color-scheme:dark;--bg:#0e141d;--panel:#151e2a;--line:#314154;--text:#e8eef7;--muted:#9cacbf;--cyan:#50e6d5;--gold:#ffd45c;--orange:#ff9d4d;--purple:#a982ef}
*{box-sizing:border-box}[hidden]{display:none!important}body{margin:0;background:var(--bg);color:var(--text);font:14px/1.45 system-ui,"Microsoft YaHei",sans-serif}button,select{font:inherit}main{display:grid;grid-template-columns:minmax(620px,1.6fr) minmax(440px,1fr);min-height:100vh}body.panel-hidden main{grid-template-columns:1fr}body.panel-hidden .panel{display:none}
#map-wrap{position:sticky;top:0;height:100vh;background:#111a26;overflow:hidden}#map-background,#map{position:absolute;inset:0;width:100%;height:100%;display:block;object-fit:contain}#map-background[hidden]{display:none!important}#map{z-index:1;pointer-events:auto}#map [data-map-layer]{pointer-events:none}
.map-toolbar{position:absolute;z-index:4;top:12px;left:12px;right:12px;display:flex;align-items:center;gap:6px;flex-wrap:wrap;pointer-events:none}.map-toolbar>*{pointer-events:auto}.layer-toggle,.toolbar-button{min-height:32px;border:1px solid #40536a;border-radius:7px;background:#0d141ee8;color:var(--muted);padding:6px 9px;cursor:pointer;user-select:none}.layer-toggle{display:inline-flex;align-items:center;gap:5px}.layer-toggle:has(input:checked){border-color:var(--cyan);background:#12302f;color:var(--text)}.layer-toggle input{accent-color:var(--cyan);margin:0}.toolbar-button:hover{color:var(--text);border-color:#66819f}
.panel{padding:0 20px 24px;overflow:auto;height:100vh}.navigator{position:sticky;z-index:5;top:0;margin:0 -20px 14px;padding:14px 20px 12px;background:#151e2af2;border-bottom:1px solid var(--line);backdrop-filter:blur(8px)}.nav-line{display:flex;align-items:center;gap:8px;margin-top:8px}.nav-line:first-child{margin-top:0}.floor-tabs,.segment{display:flex;gap:5px;flex-wrap:wrap}.nav-button,.floor-button,.view-button{border:1px solid var(--line);border-radius:6px;background:#0d1520;color:var(--muted);padding:6px 10px;cursor:pointer}.floor-button.active,.view-button.active{background:#173a3b;color:var(--text);border-color:var(--cyan)}.nav-button:disabled{opacity:.35;cursor:default}.step-select,.candidate-select{min-width:0;flex:1;padding:7px 9px;background:#0d1520;color:var(--text);border:1px solid var(--line);border-radius:6px}.nav-label{color:var(--muted);font-size:12px;white-space:nowrap}.view-switch{margin-left:auto}
h1{font-size:19px;margin:0 0 4px}h2{font-size:15px;margin:20px 0 8px}.muted{color:var(--muted)}.summary{margin:0 0 10px}.reason{padding:9px 11px;background:#1b2938;border-left:4px solid var(--cyan);margin:10px 0;white-space:pre-wrap}.cards{display:flex;gap:8px;flex-wrap:wrap}.card{border:1px solid var(--line);background:#111a25;border-radius:6px;padding:8px 10px;min-width:150px}.card strong{display:block}.badge,.metric{display:inline-flex;align-items:center;border:1px solid var(--line);border-radius:12px;padding:2px 7px;margin:2px 4px 2px 0;color:var(--muted);font-size:11px}.metric{background:#101925;color:var(--text)}.metric.decisive{border-color:var(--gold);color:var(--gold)}
.table-wrap{overflow-x:auto;border:1px solid #263649;border-radius:7px}table{border-collapse:collapse;width:100%;font-size:12px}th,td{padding:7px 6px;border-bottom:1px solid #29394b;text-align:left;vertical-align:top}th{color:var(--muted);font-weight:600;white-space:nowrap}tbody tr:last-child td{border-bottom:0}tbody tr.selectable{cursor:pointer}tbody tr.selectable:hover,tbody tr.focused{background:#203246}tbody tr.actual{box-shadow:inset 3px 0 var(--cyan)}.candidate-name{min-width:116px}.compact-number{text-align:center;white-space:nowrap}.score-panel{margin-top:8px;padding:10px;border:1px solid var(--line);border-radius:7px;background:#111a25}.score-title{display:flex;align-items:center;gap:8px;flex-wrap:wrap;margin-bottom:6px}.score-title strong{color:var(--gold)}.score-groups{display:grid;gap:8px}.score-group-label{color:var(--muted);font-size:11px;margin-right:6px}.raw-score{margin-top:8px;color:var(--muted)}.raw-score pre{white-space:pre-wrap;word-break:break-word;background:#0c131c;padding:8px;border-radius:5px}.rejected{white-space:pre-wrap;color:#dcaeae;font-size:12px}
.legend{position:absolute;z-index:3;left:12px;bottom:12px;max-width:min(900px,calc(100% - 24px));background:#0d141ef2;border:1px solid var(--line);border-radius:7px;padding:7px 10px;font-size:12px}.legend summary{cursor:pointer;color:var(--text);font-weight:700}.legend-content{display:grid;grid-template-columns:repeat(3,minmax(180px,1fr));gap:8px 14px;padding-top:8px}.legend-group{border-left:2px solid #40536a;padding-left:8px}.legend-group strong{display:block;color:var(--gold);margin-bottom:3px}.legend span,.evidence-legend span{display:flex;align-items:center;margin:3px 0}.dot{width:11px;height:11px;border-radius:50%;margin-right:6px;border:2px solid #dbe7f4;flex:none}.line-key{width:24px;height:0;margin-right:6px;border-top:3px solid;flex:none}
.map-view-note{margin:0 0 10px;padding:8px 10px;border:1px solid #34506b;border-radius:7px;background:#101c29;color:#c8d8e9}.map-view-note strong{color:var(--cyan)}
.node-tooltip{position:fixed;z-index:30;max-width:380px;padding:9px 11px;border:1px solid #6b829c;border-radius:7px;background:#08111bf5;color:#eef5ff;box-shadow:0 8px 28px #000b;pointer-events:none;white-space:pre-line;font-size:12px}.node-tooltip strong{display:block;color:var(--gold);font-size:13px;margin-bottom:3px}
.recognition-grid{display:grid;grid-template-columns:1fr;gap:10px;margin-top:8px}.recognition-grid figure{margin:0;border:1px solid var(--line);border-radius:7px;padding:7px;background:#111a25}.recognition-grid figcaption{font-weight:700;margin-bottom:6px}.recognition-grid img{display:block;width:100%;height:auto;border-radius:5px}.full-image-link{display:block;color:var(--cyan);font-size:12px;margin-bottom:6px}.recognition-legend,.evidence-legend{display:grid;grid-template-columns:repeat(2,minmax(180px,1fr));gap:2px 12px;margin:8px 0;padding:8px;border:1px solid var(--line);border-radius:7px;background:#101925}.evidence-gallery{display:grid;gap:10px;margin-top:8px}.evidence-source-group{border:1px solid #263649;border-radius:7px;padding:8px;background:#0f1823}.evidence-source-group>h3{font-size:13px;color:var(--gold);margin:0 0 6px}.evidence-gallery details{border:1px solid var(--line);border-radius:7px;padding:7px;background:#111a25;margin-top:6px}.evidence-shot{position:relative;margin-top:7px;overflow:hidden;border-radius:5px}.evidence-shot img{display:block;width:100%;height:auto}.evidence-shot svg{position:absolute;inset:0;width:100%;height:100%;pointer-events:none}.ocr-box{fill:none;stroke:var(--gold);stroke-width:3}.ocr-box.boundary{stroke:var(--purple);stroke-dasharray:10 6}.star-box{fill:none;stroke:#50e6d5;stroke-width:3}.star-box.depleted{stroke:#8290a0;stroke-dasharray:5 3}.ocr-label{fill:var(--gold);font-size:18px;font-weight:700;paint-order:stroke;stroke:#111;stroke-width:4px}
.edge{stroke:#526276;stroke-width:2}.edge.detected-template{stroke:var(--cyan);stroke-width:3}.edge.template-only{stroke:#4da3ff;stroke-dasharray:7 5}.edge.detected-extra{stroke:var(--orange);stroke-width:4}.edge.inferred{stroke:var(--purple);stroke-dasharray:7 5}.node-evidence{fill:none;stroke-width:4}.node-evidence.detected-template{stroke:var(--cyan)}.node-evidence.template-only{stroke:#4da3ff;stroke-dasharray:5 3}.node-evidence.detected-only{stroke:#d875e8}.node{stroke:#dbe7f4;stroke-width:2;fill-opacity:.42}.unknown{fill:#7651ae}.known{fill:#32698d}.combat{fill:#a7454d}.exit{fill:#ad8124}.completed{fill:#46515e}.notebook-retained{stroke:var(--gold);stroke-width:5;stroke-dasharray:5 3}.semantic{stroke:var(--purple);stroke-width:4;stroke-dasharray:4 3}.selected-node{stroke:var(--gold);stroke-width:5}.natural-reveal-suppressed{fill:none;stroke:#f08cff;stroke-width:5;stroke-dasharray:3 4}.reveal-halo{fill:none;stroke:var(--orange);stroke-width:8;opacity:.88}.consistency-missing,.consistency-unexpected{fill:none;stroke-width:6;opacity:.95}.consistency-missing{stroke:#ff5f6d;stroke-dasharray:8 5}.consistency-unexpected{stroke:#55aaff;stroke-dasharray:3 4}
.node-marker-badge{stroke:#f4f8ff;stroke-width:2}.node-marker-badge.marker-resident{fill:#c4473f}.node-marker-badge.marker-fruit{fill:#3f9b5e}.node-marker-badge.marker-informant{fill:#805bc3}.node-marker-badge.marker-other{fill:#456a8b}.node-marker-text{fill:#fff;font-size:11px;font-weight:800;text-anchor:middle;dominant-baseline:central;paint-order:stroke;stroke:#10151d;stroke-width:2px}.node-hitbox{fill:transparent;stroke:none;pointer-events:all;cursor:help}
.route-casing,.route{fill:none;stroke-linecap:round;stroke-linejoin:round}.route-casing{stroke:#07131ae6;stroke-width:10}.route-casing.item{stroke-dasharray:14 8}.route{stroke:var(--cyan);stroke-width:5.5;filter:drop-shadow(0 0 2px #07131a)}.route.item{stroke:var(--orange);stroke-dasharray:14 8}.route-arrow{fill:var(--cyan);stroke:#10151d;stroke-width:1.5;stroke-linejoin:round}.route-arrow.item{fill:var(--orange)}.route-mode{fill:#fff;font-size:13px;font-weight:700;text-anchor:middle;dominant-baseline:central;paint-order:stroke;stroke:#10151d;stroke-width:4px}.route-mode-bg{fill:#10151df2;stroke:var(--cyan);stroke-width:1.5}.route-mode-bg.item{stroke:var(--orange)}.step{fill:#101923;stroke:var(--cyan);stroke-width:4}.step.item{stroke:var(--orange)}.step.start{stroke:#f6d365}.step-number{fill:#fff;font-size:12px;font-weight:800;text-anchor:middle;dominant-baseline:central;paint-order:stroke;stroke:#10151d;stroke-width:3px}
.node-label-bg{fill:#0c131cf2;stroke:#40536a;stroke-width:1}.node-label-type,.node-label-content{fill:#f5f8fb;text-anchor:middle;paint-order:stroke;stroke:#10151d;stroke-width:3px;font-size:10px;font-weight:700}.node-label-content{fill:#b9c9da;font-size:9px;font-weight:500}
@media(max-width:1100px){main{display:block}#map-wrap{position:relative;height:68vh}.panel{height:auto}.navigator{position:relative}.view-switch{margin-left:0}}@media(max-width:760px){.legend-content,.recognition-grid,.recognition-legend,.evidence-legend{grid-template-columns:1fr}}@media(max-width:650px){.panel{padding:0 12px 20px}.navigator{margin:0 -12px 12px;padding:10px 12px}.nav-line{flex-wrap:wrap}.step-select,.candidate-select{flex-basis:70%}.map-toolbar{right:auto;max-width:calc(100% - 24px)}}
</style></head><body><main><section id="map-wrap">
<img id="map-background" alt="本步游戏地图截图">
<svg id="map" viewBox="0 0 1280 720" preserveAspectRatio="xMidYMid meet" role="img" aria-label="游戏地图上的路线规划叠加层"></svg>
<div class="map-toolbar" aria-label="地图图层控制">
<label class="layer-toggle"><input type="checkbox" data-layer="background" checked>游戏截图</label>
<label class="layer-toggle"><input type="checkbox" data-layer="nodes" checked>节点身份</label>
<label class="layer-toggle"><input type="checkbox" data-layer="markers" checked>节点标记</label>
<label class="layer-toggle"><input type="checkbox" data-layer="reveals" checked>预期揭示</label>
<label class="layer-toggle"><input type="checkbox" data-layer="consistency" checked>上步不一致</label>
<label class="layer-toggle"><input type="checkbox" data-layer="route" checked>规划路线</label>
<label class="layer-toggle"><input type="checkbox" data-layer="labels">节点说明</label>
<label class="layer-toggle"><input type="checkbox" data-layer="evidence">识别证据</label>
<button id="clean-map" class="toolbar-button" type="button">底图+路线</button>
<button id="original-map" class="toolbar-button" type="button">只看游戏</button>
<button id="full-map" class="toolbar-button" type="button">全部图层</button>
<button id="panel-toggle" class="toolbar-button" type="button">宽屏地图</button>
</div>
<details class="legend"><summary>图例（按图层分组）</summary><div class="legend-content">
<div class="legend-group"><strong>节点身份层</strong><span><i class="dot" style="background:#7651ae"></i>身份未知</span><span><i class="dot" style="background:#32698d"></i>身份已知</span><span><i class="dot" style="background:#a7454d"></i>作战/凶戾</span><span><i class="dot" style="background:#ad8124"></i>楼层终点</span><span><i class="dot" style="background:#46515e"></i>已完成</span></div>
<div class="legend-group"><strong>节点标记层</strong><span><i class="dot" style="background:#c4473f"></i>居：流窜“居民”</span><span><i class="dot" style="background:#3f9b5e"></i>果：藏果地</span><span><i class="dot" style="background:#805bc3"></i>线：线人与线索（命运所指上为谜题与谜底）</span><span>标记只表示当前观测，不作为探索笔记中的历史身份。</span></div>
<div class="legend-group"><strong>规划路线层</strong><span><i class="line-key" style="border-color:#50e6d5"></i>徒步</span><span><i class="line-key" style="border-color:#ff9d4d;border-style:dashed"></i>加工品移动</span><span><i class="dot" style="background:#101923;border-color:#f6d365"></i>0：路线起点</span><span><i class="dot" style="background:#101923;border-color:#50e6d5"></i>1 起：移动次序/落点</span><span><i class="dot" style="background:transparent;border-color:#ff9d4d"></i>本路线将探明</span></div>
<div class="legend-group"><strong>识别证据层</strong><span><i class="dot" style="background:transparent;border-color:#50e6d5"></i>节点：模板+视觉</span><span><i class="dot" style="background:transparent;border-color:#4da3ff;border-style:dashed"></i>节点：仅模板</span><span><i class="dot" style="background:transparent;border-color:#d875e8"></i>节点：仅视觉</span><span><i class="line-key" style="border-color:#50e6d5"></i>连线：模板+视觉</span><span><i class="line-key" style="border-color:#4da3ff;border-style:dashed"></i>连线：仅模板</span><span><i class="line-key" style="border-color:#ff9d4d"></i>模板外视觉连线</span><span><i class="line-key" style="border-color:#a982ef;border-style:dashed"></i>规则推断连线/身份</span></div>
<div class="legend-group"><strong>揭示核对层</strong><span><i class="dot" style="background:transparent;border-color:#f08cff"></i>弥散虚雾阻止自然揭示</span><span><i class="dot" style="background:transparent;border-color:#ff5f6d"></i>上步漏揭示</span><span><i class="dot" style="background:transparent;border-color:#55aaff"></i>上步额外揭示</span></div>
</div></details></section><section class="panel">
)HTML" << R"HTML(
<div class="navigator"><div class="nav-line"><span class="nav-label">层数</span><div id="floor-tabs" class="floor-tabs"></div><div id="view-tabs" class="segment view-switch"><button class="view-button active" data-view="current" type="button">当前观测</button><button class="view-button" data-view="notebook" type="button">探索笔记</button></div></div><div class="nav-line"><button id="previous-step" class="nav-button" type="button" aria-label="上一步">←</button><select id="decision" class="step-select" aria-label="本层观测和规划步"></select><button id="next-step" class="nav-button" type="button" aria-label="下一步">→</button><span id="step-counter" class="nav-label"></span></div><div class="nav-line"><span class="nav-label">候选</span><select id="candidate" class="candidate-select" aria-label="查看候选路线"></select></div></div>
<div id="map-view-note" class="map-view-note"></div><h1 id="title">黑流树海路线规划依据</h1><div id="summary" class="muted summary"></div><div id="reason" class="reason"></div>
<h2>当前可用加工品</h2><div class="muted">每张卡对应零件箱里的一件实例；规划时才按种类汇总剩余次数。</div><div id="items" class="cards"></div><h2>加工品识别证据</h2><div id="processing-evidence-note" class="muted"></div><div class="table-wrap"><table><thead><tr><th>#</th><th>证据来源</th><th>扫描过程</th><th>识别到的加工品</th><th>次数/装载</th><th>置信度</th><th>名称 OCR 区域</th><th>结果</th></tr></thead><tbody id="processing-evidence"></tbody></table></div><div id="processing-evidence-images" class="evidence-gallery"></div><h2>逐步移动</h2><div id="route-note" class="muted"></div><div class="table-wrap"><table><thead><tr><th>#</th><th>移动方式</th><th>起点 → 落点</th><th>路径</th><th>行动力</th></tr></thead><tbody id="steps"></tbody></table></div>
<h2>候选比较（点击切换）</h2><div class="table-wrap"><table><thead><tr><th>#</th><th>候选</th><th>保底探明</th><th>保底有效计分</th><th>加工品<br>跨层/总</th><th>长度</th><th>安全</th></tr></thead><tbody id="candidates"></tbody></table></div><div id="score-panel" class="score-panel"></div>
<h2>被过滤的候选</h2><div id="rejected" class="rejected"></div>
<details id="image-details" open><summary>本次地图识别证据（与规划图分开显示）</summary>
<div class="recognition-grid"><figure id="recognition-captured-card"><figcaption>原始游戏截图</figcaption><a id="recognition-captured-link" class="full-image-link" target="_blank">在新标签中按原始尺寸打开</a><img id="recognition-captured" alt="本次原始游戏截图"></figure><figure id="recognition-overlay-card"><figcaption>识别叠加图</figcaption><a id="recognition-overlay-link" class="full-image-link" target="_blank">在新标签中按原始尺寸打开</a><img id="overlay" alt="本次识别叠加图"></figure></div>
<div class="recognition-legend"><span><i class="dot" style="background:transparent;border-color:#e9f055"></i>黄色外圈：拓扑采用的节点/连线</span><span><i class="dot" style="background:transparent;border-color:#50b5ff"></i>蓝色外圈：视觉识别证据</span><span><i class="line-key" style="border-color:#50e667"></i>绿色：空格或视觉确认连线</span><span><i class="line-key" style="border-color:#ff9d4d"></i>橙色：保留的模板外节点/连线</span><span>文字格式：坐标 · 节点类型 · 明暗 · [T 模板 / V 视觉]</span><span>此图只用于检查识别，不再叠到路线规划图上。</span></div>
</details></section></main><div id="node-tooltip" class="node-tooltip" hidden></div>
<script src="routing-history-data.js"></script>)HTML"
           << R"HTML(<script src="processing-item-history-data.js"></script><script>
'use strict';
const raw={history:BLACKFLOW_ROUTING_HISTORY,processingEvidence:BLACKFLOW_PROCESSING_ITEM_HISTORY};
const history=Array.isArray(raw)?raw:(raw.history||[]);
const processingEvidence=Array.isArray(raw.processingEvidence)?raw.processingEvidence:[];
const $=id=>document.getElementById(id);
const svgNS='http://www.w3.org/2000/svg';
const svgEl=(name,attrs={})=>{const element=document.createElementNS(svgNS,name);for(const [key,value] of Object.entries(attrs))element.setAttribute(key,String(value));return element};
const td=(row,value,className='')=>{const cell=document.createElement('td');cell.textContent=value===undefined||value===null?'—':String(value);if(className)cell.className=className;row.append(cell)};
const nodeTypeNames={empty:'林间空地',battle_normal:'作战',battle_elite:'紧急作战',battle_savage:'“居民”据点',battle_boss:'险路恶敌',hide_battle:'未知的凶戾',hide_invisible:'未知的诡秘',shop:'诡意行商',scrap_shop:'秘境行商',incident:'不期而遇',light:'羽瞰点',final:'险路尽头',portal:'误入奇境',door:'曲折密道',duel:'狭路相逢',rest:'安全的角落',wish:'得偿所愿',expedition:'先行一步',sacrifice:'失与得',evacuate:'险路小径',employ:'应急助力',unclassified:'未分类'};
const fateEventNames=new Set(['好奇心之死','窥视箱中','调谐仪式']);
const markerTypeNames={resident:'流窜“居民”',savage:'流窜“居民”',fruit_cache:'藏果地',informant:'线人与线索'};
const identitySourceNames={ocr:'地图 OCR',event_name:'事件标题',entered_page:'进入页面',move_preview_ocr:'移动预览',move_preview_stage_name:'战斗情报探查',battle_stage_name:'战斗插件关卡名',map_template_fixed_identity:'地图模板',ideal_source_emergency_prediction:'实托邦中心规则',initial_roaming_resident_prediction:'居民据点规则',node_resolution_becomes_empty:'节点结算',covered_position_without_observed_node:'当前截图空缺'};
const utopiaIdeologyNames={'blackstream-veins':'黑流地脉','tilted-dune':'倾斜沙丘','dewarming-rail':'去温栏','micro-capsule':'微型胶囊','storage-room':'储藏室','fragile-alliance':'易碎同盟','stopping-point':'停止点','hopeful-soil':'希望的沃土','diffused-mist':'弥散虚雾','brave-new-land':'美丽新大地'};
const utopiaPolicyNames={improvement:'改良',correction:'修正',radical:'激进',benefit:'增益'};
const reasonCategoryNames={strategy_end:'终点与任务约束',resource_reserve:'加工品预留',preferred_goal:'优先目标',development:'探索收益',risk_avoidance:'风险规避',safety_fallback:'安全兜底',tie_break:'同分比较',battle_stage_observation:'战斗关卡识别'};
const scoreNames={revealed_node_count:'探明节点',effective_node_count:'有效节点计分',development_score:'发育代价',persistent_processing_move_count:'跨层加工品',processing_move_count:'总加工品',route_length:'路线长度',movement_action_count:'实际移动次数',safe_requirement:'安全需求',battle_count:'战斗（仅记录）',risk_score:'风险',intermediate_interaction_count:'途中交互'};
function mapSection(item){const floor=Number(item?.floor)||0,generation=Number(item?.map_generation)||0,remembrance=Boolean(item?.floor_four_remembrance),key=item?.map_section_key||`floor-${floor}-generation-${generation}${remembrance?'-remembrance':''}`,label=item?.map_section_label||`${remembrance?'追忆 ':''}${floor} 层`;return{floor,generation,remembrance,key,label}}
const mapSections=[...new Map(history.map(item=>{const section=mapSection(item);return[section.key,section]})).values()];
let mapSectionKey=mapSections.length?mapSections.at(-1).key:'',decisionIndex=Math.max(0,history.length-1),candidateIndex=-1,mapView='current',floorDecisionIndexes=[];
function prettyNodeType(type){return nodeTypeNames[String(type)]||String(type||'节点')}
function prettyMarkerType(type){return markerTypeNames[String(type)]||String(type||'')}
function nodePresentation(node){const type=String(node?.node_type||''),name=String(node?.node_name||node?.name||''),stage=String(node?.stage_name||''),fate=Boolean(node?.fate_event)||name==='命运所指'||(type==='incident'&&fateEventNames.has(name)),title=fate?'命运所指':prettyNodeType(type);let content='',contentLabel='';if(type==='battle_normal'||type==='battle_elite'||type==='battle_boss'){content=stage;contentLabel='关卡名'}else if(fate||type==='incident'||type==='duel'){content=name&&name!==title?name:'';contentLabel='事件名'}return{title,content,contentLabel}}
const nodeText=node=>{if(!node||node.id===undefined)return'?';const presentation=nodePresentation(node),detail=presentation.content?` · ${presentation.content}`:'';return`${presentation.title}${detail} (${node.row??'?'},${node.column??'?'})`};
function markerPresentation(node){const type=String(node?.marker_type||''),name=String(node?.marker_display_name||prettyMarkerType(type));if(!type&&!name)return null;if(type==='savage'||type==='resident')return{name:name||'流窜“居民”',short:'居',className:'marker-resident'};if(type==='fruit_cache')return{name:name||'藏果地',short:'果',className:'marker-fruit'};if(type==='informant'){const displayName=nodePresentation(node).title==='命运所指'?'谜题与谜底':(!name||name==='线人'?'线人与线索':name);return{name:displayName,short:'线',className:'marker-informant'}}return{name:name||type,short:(name||type||'标').slice(0,1),className:'marker-other'}}
function mapViewDescription(view){return view==='notebook'?'探索笔记：累计保留本层曾揭示的节点身份、事件名和战斗关卡名；金色虚线圈表示已走过但仍保留历史身份。':'当前观测：严格对应本步骤截图/OCR；已结算节点会显示为林间空地，流窜“居民”、藏果地等标记也以这一拍为准。'}
function nodeTooltipLines(node,view){const marker=markerPresentation(node),presentation=nodePresentation(node),progress=node?.progress==='completed'?'已完成':(node?.progress==='removed'?'已移除':'未完成'),source=identitySourceNames[String(node?.identity_source||'')]||String(node?.identity_source||'未记录');const lines=[`${view==='notebook'?'探索笔记':'当前观测'} · ${presentation.title}`,`节点名称：${presentation.title}`,`坐标：(${node?.row??'?'}, ${node?.column??'?'})`,presentation.content?`${presentation.contentLabel}：${presentation.content}`:'',`状态：${progress}`,`身份来源：${source}`,marker?`节点标记：${marker.name}${Number(node?.marker_score)>0?`（${(Number(node.marker_score)*100).toFixed(1)}%）`:''}`:'节点标记：无',node?.marker_resident_overlap_possible?'居民重合：可能（规划避让；据点推断枚举两种解释）':'',node?.identity_from_prediction?`规则预判：${node.prediction_rule||'是'}`:'',node?.natural_reveal_suppressed?'弥散虚雾：阻止连线自然揭示':''].filter(Boolean);return lines}
function prettyReasonCategory(category){return reasonCategoryNames[String(category)]||'其他策略条件'}
function humanMessage(message){const translations={'move proposal has no current viewport coordinate':'移动候选在当前画面中没有可点击坐标','no eligible safe candidate':'没有符合约束且能安全完成路线的候选','selected by lexicographic policy order':'按照既定优先级逐项比较后选出','maximize newly revealed nodes along the complete safe route':'在保留完整安全路线的前提下，优先探明更多未知节点','maximize distinct non-empty landing nodes along the complete safe route':'在保留完整安全路线的前提下，优先提高不重复有效落点计分；紧急作战、“居民”据点、秘境行商、狭路相逢和非三层误入奇境计 2，三层误入奇境计 3，其他有效节点计 1','preserve processing items that remain available across floors':'优先保留可以带到后续层数使用的加工品','consume floor-expiring processing items after cross-floor usage is tied':'跨层加工品用量相同时，优先使用本层结束后会失效的加工品','minimize processing-item usage':'优先减少加工品的总用量','minimize route length after collection and processing-item value are tied':'探明收益和加工品价值相同时，优先选择更短的路线'};return translations[String(message)]||String(message||'')}
function selectedCandidate(d){return (d.candidate_comparison||[])[candidateIndex]||null}
function selectedMap(d){return mapView==='notebook'?{nodes:d.exploration_note_nodes||d.exploration_notebook?.nodes||[],edges:d.exploration_note_edges||d.exploration_notebook?.edges||[]}:{nodes:d.map_nodes||d.planning_map?.nodes||d.nodes||[],edges:d.map_edges||d.planning_map?.edges||d.edges||[]}}
function movementName(d,movement,candidate=null){if(candidate?.direct_exhaustion)return '直接耗尽';if(movement==='walk')return '徒步';const step=(candidate?.planned_route_steps||[]).find(item=>item.movement===movement);if(step?.movement_name)return step.movement_name;const item=(d.processing_item_catalog||d.processing_items||[]).find(entry=>entry.movement===movement);return item?.name||movement}
function routeSequence(step){const result=[];const add=node=>{if(node&&node.id!==undefined&&String(node.id)!==result.at(-1))result.push(String(node.id))};add(step.source);for(const node of step.path||[])add(node);add(step.landing||step.target);return result}
function readableRouteAngle(tangent){let angle=Math.atan2(tangent.y,tangent.x)*180/Math.PI;if(angle>90)angle-=180;if(angle<-90)angle+=180;return angle}
function routeSegmentGeometry(a,b,laneOffset=0){const dx=b.x-a.x,dy=b.y-a.y,length=Math.hypot(dx,dy);if(length<2)return null;const ux=dx/length,uy=dy/length,nx=-uy,ny=ux,startDistance=Math.min(25,length*.24),arrowTipDistance=Math.min(17,length*.18),arrowLength=Math.min(14,length*.16),endDistance=arrowTipDistance+arrowLength,start={x:a.x+ux*startDistance,y:a.y+uy*startDistance},end={x:b.x-ux*endDistance,y:b.y-uy*endDistance},control={x:(start.x+end.x)/2+nx*laneOffset,y:(start.y+end.y)/2+ny*laneOffset},anchor={x:(start.x+2*control.x+end.x)/4,y:(start.y+2*control.y+end.y)/4},endDx=end.x-control.x,endDy=end.y-control.y,endLength=Math.hypot(endDx,endDy)||1,endTangent={x:endDx/endLength,y:endDy/endLength},endNormal={x:-endDy/endLength,y:endDx/endLength},arrowTip={x:end.x+endTangent.x*arrowLength,y:end.y+endTangent.y*arrowLength},arrowLeft={x:end.x+endNormal.x*8,y:end.y+endNormal.y*8},arrowRight={x:end.x-endNormal.x*8,y:end.y-endNormal.y*8};return{start,end,control,anchor,normal:{x:nx,y:ny},tangent:{x:ux,y:uy},labelAngle:readableRouteAngle({x:ux,y:uy}),arrowTip,arrowPoints:`${arrowTip.x},${arrowTip.y} ${arrowLeft.x},${arrowLeft.y} ${arrowRight.x},${arrowRight.y}`,length:Math.hypot(end.x-start.x,end.y-start.y),path:`M ${start.x} ${start.y} Q ${control.x} ${control.y} ${end.x} ${end.y}`}}
function routeSegmentsVisuallyOverlap(first,second){if(!first.a||!first.b||!second.a||!second.b)return[String(first.from),String(first.to)].sort().join('|')===[String(second.from),String(second.to)].sort().join('|');const firstDx=first.b.x-first.a.x,firstDy=first.b.y-first.a.y,secondDx=second.b.x-second.a.x,secondDy=second.b.y-second.a.y,firstLength=Math.hypot(firstDx,firstDy),secondLength=Math.hypot(secondDx,secondDy);if(firstLength<2||secondLength<2)return false;const angleError=Math.abs(firstDx*secondDy-firstDy*secondDx)/(firstLength*secondLength);if(angleError>.045)return false;const ux=firstDx/firstLength,uy=firstDy/firstLength,nx=-uy,ny=ux,distanceA=Math.abs((second.a.x-first.a.x)*nx+(second.a.y-first.a.y)*ny),distanceB=Math.abs((second.b.x-first.a.x)*nx+(second.b.y-first.a.y)*ny);if(Math.max(distanceA,distanceB)>6)return false;const projectionA=(second.a.x-first.a.x)*ux+(second.a.y-first.a.y)*uy,projectionB=(second.b.x-first.a.x)*ux+(second.b.y-first.a.y)*uy,overlap=Math.min(firstLength,Math.max(projectionA,projectionB))-Math.max(0,Math.min(projectionA,projectionB));return overlap>12}
function routeLaneOffsets(segments){const parent=segments.map((_,index)=>index),find=index=>{while(parent[index]!==index){parent[index]=parent[parent[index]];index=parent[index]}return index},join=(left,right)=>{left=find(left);right=find(right);if(left!==right)parent[right]=left};for(let left=0;left<segments.length;left++)for(let right=left+1;right<segments.length;right++)if(routeSegmentsVisuallyOverlap(segments[left],segments[right]))join(left,right);const groups=new Map();segments.forEach((segment,index)=>{const root=find(index);if(!groups.has(root))groups.set(root,[]);groups.get(root).push(segment)});for(const group of groups.values()){const reference=group[0],dx=(reference.b?.x??1)-(reference.a?.x??0),dy=(reference.b?.y??0)-(reference.a?.y??0),length=Math.hypot(dx,dy)||1,flip=dx<0||(Math.abs(dx)<.001&&dy<0)?-1:1,canonical={x:dx/length*flip,y:dy/length*flip};group.forEach((segment,index)=>{const physicalOffset=(index-(group.length-1)/2)*30,segmentDx=(segment.b?.x??1)-(segment.a?.x??0),segmentDy=(segment.b?.y??0)-(segment.a?.y??0),coordinateDirection=segment.a&&segment.b?(segmentDx*canonical.x+segmentDy*canonical.y>=0?1:-1):(String(segment.from).localeCompare(String(segment.to))<=0?1:-1);segment.laneOffset=physicalOffset*coordinateDirection;segment.physicalLaneOffset=physicalOffset})}return segments}
function layerEnabled(name){return Boolean(document.querySelector(`[data-layer="${name}"]`)?.checked)}
function applyLayerVisibility(){const background=$('map-background'),showBackground=Boolean(background.dataset.available)&&layerEnabled('background');background.hidden=!showBackground;background.style.display=showBackground?'block':'none';for(const name of ['nodes','markers','reveals','consistency','route','labels','evidence'])for(const group of document.querySelectorAll(`[data-map-layer="${name}"]`))group.style.display=layerEnabled(name)?'':'none'}
function setLayers(enabled){for(const input of document.querySelectorAll('[data-layer]'))input.checked=enabled.includes(input.dataset.layer);applyLayerVisibility()}
function positionNodeTooltip(event){const tooltip=$('node-tooltip'),margin=12,maxX=Math.max(margin,window.innerWidth-tooltip.offsetWidth-margin),maxY=Math.max(margin,window.innerHeight-tooltip.offsetHeight-margin);tooltip.style.left=`${Math.min(event.clientX+14,maxX)}px`;tooltip.style.top=`${Math.min(event.clientY+14,maxY)}px`}
function showNodeTooltip(event,node){const tooltip=$('node-tooltip'),lines=nodeTooltipLines(node,mapView),title=document.createElement('strong');title.textContent=lines.shift()||'节点详情';tooltip.replaceChildren(title,document.createTextNode(lines.join('\n')));tooltip.hidden=false;positionNodeTooltip(event)}
function hideNodeTooltip(){$('node-tooltip').hidden=true}
)HTML" << R"HTML(
function drawMap(d,candidate){
 const svg=$('map'),background=$('map-background'),width=Number(d.captured_image_width)||1280,height=Number(d.captured_image_height)||720;
 svg.setAttribute('viewBox',`0 0 ${width} ${height}`);svg.replaceChildren();
 if(d.captured_image_file){background.src=d.captured_image_file;background.dataset.available='true'}else{background.removeAttribute('src');delete background.dataset.available}
 const groups={evidence:svgEl('g',{'data-map-layer':'evidence'}),routeLines:svgEl('g',{'data-map-layer':'route'}),nodes:svgEl('g',{'data-map-layer':'nodes'}),markers:svgEl('g',{'data-map-layer':'markers'}),reveals:svgEl('g',{'data-map-layer':'reveals'}),consistency:svgEl('g',{'data-map-layer':'consistency'}),routeBadges:svgEl('g',{'data-map-layer':'route'}),labels:svgEl('g',{'data-map-layer':'labels'}),interactions:svgEl('g',{'data-map-layer':'nodes'})};
 svg.append(groups.evidence,groups.routeLines,groups.nodes,groups.markers,groups.reveals,groups.consistency,groups.routeBadges,groups.labels,groups.interactions);
 const selected=selectedMap(d),nodes=selected.nodes,edges=selected.edges,byId=new Map(nodes.map(node=>[String(node.id),node])),currentNodes=d.map_nodes||d.planning_map?.nodes||d.nodes||[],visualById=new Map(currentNodes.map(node=>[String(node.id),node]));
 if(!nodes.length){applyLayerVisibility();return}
 const rows=nodes.map(node=>Number(node.row)||0),columns=nodes.map(node=>Number(node.column)||0),minRow=Math.min(...rows),maxRow=Math.max(...rows),minColumn=Math.min(...columns),maxColumn=Math.max(...columns);
 const position=node=>{const visual=Number.isFinite(Number(node.visual_x))?node:visualById.get(String(node.id));if(visual&&Number.isFinite(Number(visual.visual_x))&&Number.isFinite(Number(visual.visual_y)))return{x:Number(visual.visual_x),y:Number(visual.visual_y)};return{x:width*.08+(Number(node.column)-minColumn)*width*.64/Math.max(1,maxColumn-minColumn),y:height*.12+(Number(node.row)-minRow)*height*.72/Math.max(1,maxRow-minRow)}};
 for(const edge of edges){if(edge.knowledge==='absent')continue;const first=byId.get(String(edge.first)),second=byId.get(String(edge.second));if(!first||!second)continue;const a=position(first),b=position(second),source=edge.decision_source||'';let evidenceClass='inferred';if(source==='observed_extra_edge')evidenceClass='detected-extra';else if(source==='map_template_base_edge')evidenceClass=edge.cnn_connected?'detected-template':'template-only';else if(edge.cnn_connected)evidenceClass='detected-only';groups.evidence.append(svgEl('line',{x1:a.x,y1:a.y,x2:b.x,y2:b.y,class:`edge ${evidenceClass}`}))}
 const steps=mapView==='current'?(candidate?(candidate.planned_route_steps||[]):(d.planned_route_steps||[])):[],routeSegments=[];
 steps.forEach((step,stepIndex)=>{const sequence=routeSequence(step);for(let sequenceIndex=1;sequenceIndex<sequence.length;sequenceIndex++){const first=byId.get(sequence[sequenceIndex-1]),second=byId.get(sequence[sequenceIndex]);if(first&&second)routeSegments.push({from:sequence[sequenceIndex-1],to:sequence[sequenceIndex],a:position(first),b:position(second),stepIndex,item:Boolean(step.uses_processing_item)})}});
 routeLaneOffsets(routeSegments);const routeLabelGeometry=new Map();
 for(const segment of routeSegments){const geometry=routeSegmentGeometry(segment.a,segment.b,segment.laneOffset);if(!geometry)continue;const itemClass=segment.item?' item':'';groups.routeLines.append(svgEl('path',{d:geometry.path,class:`route-casing${itemClass}`}),svgEl('path',{d:geometry.path,class:`route${itemClass}`}),svgEl('polygon',{points:geometry.arrowPoints,class:`route-arrow${itemClass}`}));const currentLabelGeometry=routeLabelGeometry.get(segment.stepIndex);if(!currentLabelGeometry||geometry.length>currentLabelGeometry.length)routeLabelGeometry.set(segment.stepIndex,geometry)}
 const selectedTarget=mapView==='current'?String(candidate?.target??d.target??''):'';
 for(const node of nodes){
  const p=position(node),evidenceClass=node.confirmed_by_topology?(node.detected_by_vision?'detected-template':'template-only'):'detected-only',evidenceLabel=node.confirmed_by_topology?(node.detected_by_vision?'模板+视觉':'仅模板'):'仅视觉';
  groups.evidence.append(svgEl('circle',{cx:p.x,cy:p.y,r:28,class:`node-evidence ${evidenceClass}`}));
  let cls=`node ${!node.identity_revealed?'unknown':'known'}`;
  if(String(node.node_type).includes('battle')||node.node_type==='hide_battle')cls='node combat';
  if(node.is_exit)cls='node exit';
  if(node.progress==='completed')cls+=mapView==='notebook'?' notebook-retained':' completed';
  if(node.identity_from_topology&&node.visually_hidden)cls+=' semantic';
  if(String(node.id)===selectedTarget)cls+=' selected-node';
  groups.nodes.append(svgEl('circle',{cx:p.x,cy:p.y,r:22,class:cls}));
  if(node.natural_reveal_suppressed)groups.nodes.append(svgEl('circle',{cx:p.x,cy:p.y,r:29,class:'natural-reveal-suppressed'}));
  const marker=markerPresentation(node);
  if(marker){const markerX=p.x+22,markerY=p.y-22,badge=svgEl('circle',{cx:markerX,cy:markerY,r:11,class:`node-marker-badge ${marker.className}`,'aria-label':marker.name}),text=svgEl('text',{x:markerX,y:markerY,class:'node-marker-text'});text.textContent=marker.short;groups.markers.append(badge,text)}
   const presentation=nodePresentation(node),compactName=presentation.title.length>8?`${presentation.title.slice(0,7)}…`:presentation.title,compactContent=presentation.content.length>12?`${presentation.content.slice(0,11)}…`:presentation.content,labelHeight=compactContent?35:21,labelAbove=p.y>height-labelHeight-37,labelTop=labelAbove?p.y-29-labelHeight:p.y+31,labelWidth=Math.max(58,Math.min(174,Math.max(compactName.length*12+18,compactContent.length*11+18))),labelGroup=svgEl('g',{});
   labelGroup.append(svgEl('rect',{x:p.x-labelWidth/2,y:labelTop,width:labelWidth,height:labelHeight,rx:6,class:'node-label-bg'}));
   const typeLine=svgEl('text',{x:p.x,y:labelTop+14,class:'node-label-type'});typeLine.textContent=compactName;
   labelGroup.append(typeLine);if(compactContent){const contentLine=svgEl('text',{x:p.x,y:labelTop+28,class:'node-label-content'});contentLine.textContent=compactContent;labelGroup.append(contentLine)}groups.labels.append(labelGroup);
  const hitbox=svgEl('circle',{cx:p.x,cy:p.y,r:31,class:'node-hitbox','aria-label':nodeTooltipLines(node,mapView).join('；')});hitbox.addEventListener('mouseenter',event=>showNodeTooltip(event,node));hitbox.addEventListener('mousemove',positionNodeTooltip);hitbox.addEventListener('mouseleave',hideNodeTooltip);groups.interactions.append(hitbox)
 }
 const revealed=mapView==='current'&&candidate?(candidate.revealed_nodes||[]):[],revealedIds=new Set(revealed.map(node=>String(node.id)));
 for(const id of revealedIds){const node=byId.get(id);if(!node)continue;const p=position(node);groups.reveals.append(svgEl('circle',{cx:p.x,cy:p.y,r:31,class:'reveal-halo'}))}
 const check=d.previous_move_reveal_consistency||{};for(const [ids,cls,radius] of [[check.missing_revealed_nodes||[],'consistency-missing',36],[check.unexpected_revealed_nodes||[],'consistency-unexpected',32]])for(const id of ids.map(String)){const node=byId.get(id);if(!node)continue;const p=position(node);groups.consistency.append(svgEl('circle',{cx:p.x,cy:p.y,r:radius,class:cls}))}
 const landingTotals=new Map(),landingOrdinals=new Map(),firstSequence=steps.length?routeSequence(steps[0]):[],startId=firstSequence[0];if(startId!==undefined)landingTotals.set(startId,1);for(const step of steps){const sequence=routeSequence(step),landingId=sequence.at(-1);if(landingId!==undefined)landingTotals.set(landingId,(landingTotals.get(landingId)||0)+1)}
 const appendStepBadge=(nodeId,item,number,start=false)=>{const node=byId.get(nodeId);if(!node)return;const base=position(node),total=landingTotals.get(nodeId)||1,ordinal=landingOrdinals.get(nodeId)||0;landingOrdinals.set(nodeId,ordinal+1);const angle=-Math.PI/2+ordinal*Math.PI*2/total,distance=total>1?25:0,p={x:base.x+Math.cos(angle)*distance,y:base.y+Math.sin(angle)*distance},circle=svgEl('circle',{cx:p.x,cy:p.y,r:13,class:`step${item?' item':''}${start?' start':''}`}),label=svgEl('text',{x:p.x,y:p.y,class:'step-number'});label.textContent=String(number);groups.routeBadges.append(circle,label)};
 if(startId!==undefined)appendStepBadge(startId,false,0,true);steps.forEach((step,index)=>{const sequence=routeSequence(step),item=Boolean(step.uses_processing_item),landingId=sequence.at(-1),geometry=routeLabelGeometry.get(index);if(item&&geometry){const modeText=`${index+1} ${movementName(d,step.movement)}`,labelWidth=Math.max(62,Math.min(190,modeText.length*13+18)),labelHeight=24,labelGroup=svgEl('g',{transform:`translate(${geometry.anchor.x} ${geometry.anchor.y}) rotate(${geometry.labelAngle})`});labelGroup.append(svgEl('rect',{x:-labelWidth/2,y:-labelHeight/2,width:labelWidth,height:labelHeight,rx:6,class:'route-mode-bg item'}));const mode=svgEl('text',{x:0,y:0,class:'route-mode'});mode.textContent=modeText;labelGroup.append(mode);groups.routeBadges.append(labelGroup)}if(landingId!==undefined)appendStepBadge(landingId,item,index+1)});
 applyLayerVisibility()
}
)HTML"
           << R"HTML(
function renderItems(d){const host=$('items');host.replaceChildren();const items=d.processing_items||[];if(!items.length){host.textContent='零件箱没有识别到可用加工品实例';host.className='cards muted';return}host.className='cards';for(const item of items){const card=document.createElement('div');card.className='card';const name=document.createElement('strong');name.textContent=`${item.name||'未命名加工品'} · 实例 ${(Number(item.instance_index)||0)+1}`;card.append(name);const detail=document.createElement('div');detail.textContent=`剩余 ${item.charges??0} 次 · 每次消耗 ${item.action_point_cost??'?'} 行动力`;card.append(detail);if(item.active){const badge=document.createElement('span');badge.className='badge';badge.textContent='零件箱观测时已装载';card.append(badge)}const persistence=document.createElement('span');persistence.className='badge';persistence.textContent=item.expires_on_floor_end?'本层尽量用掉':'能省则省';card.append(persistence);host.append(card)}}
function processingEvidenceAtStep(d){if(Array.isArray(d.processing_evidence))return [...d.processing_evidence].sort((a,b)=>(Number(a.diagnostic_sequence)||0)-(Number(b.diagnostic_sequence)||0));const limit=Number(d.diagnostic_sequence),generation=Number(d.map_generation)||0,eligible=processingEvidence.filter(item=>Number(item.floor)===Number(d.floor)&&(Number(item.map_generation)||0)===generation&&(!Number.isFinite(limit)||!Number.isFinite(Number(item.diagnostic_sequence))||Number(item.diagnostic_sequence)<=limit)),latestByType=new Map();for(const entry of eligible){const type=entry.evidence_type||'unknown',existing=latestByType.get(type);if(!existing||(Number(entry.diagnostic_sequence)||0)>=(Number(existing.diagnostic_sequence)||0))latestByType.set(type,entry)}return[...latestByType.values()].sort((a,b)=>(Number(a.diagnostic_sequence)||0)-(Number(b.diagnostic_sequence)||0))}
function renderProcessingEvidence(d){const body=$('processing-evidence');body.replaceChildren();const evidence=processingEvidenceAtStep(d);$('processing-evidence-note').textContent=evidence.length?`按证据来源分别显示该规划步骤之前最近一次记录，共 ${evidence.length} 类；零件箱和移动方式选择不会互相覆盖。`:'这一步之前还没有加工品 OCR 记录。';let rowNumber=0;const sourceNames={inventory_ocr:'零件箱',movement_panel_ocr:'移动方式选择',loaded_icon:'地图已装载图标'};for(let evidenceIndex=evidence.length-1;evidenceIndex>=0;evidenceIndex--){const entry=evidence[evidenceIndex],combined=[...(entry.items||[]),...(entry.type_boundary_items||[])],items=combined.length?combined:[null];for(const item of items){const row=document.createElement('tr');td(row,++rowNumber,'compact-number');td(row,sourceNames[entry.evidence_type]||'加工品识别');const inventoryPage=item?.scan_page===undefined?'':`；第 ${item.scan_page+1} 屏`;const process=entry.evidence_type==='movement_panel_ocr'?`从徒步所在顶部开始，向后翻 ${entry.forward_swipes??0} 次${item?.scan_page?`；第 ${item.scan_page} 屏`:''}`:(entry.evidence_type==='inventory_ocr'?`零件箱从最左端扫描${inventoryPage}`:'地图右上角装载图标');td(row,process);td(row,item?.name||entry.target_name||'没有识别到名称');const charges=item?.type_boundary?(item.boundary_label||'加工品列表边界'):(item?.remaining_charges===undefined?'未读到剩余次数':`剩余 ${item.remaining_charges} 次`);td(row,item?`${charges}${item.loaded?'；已装载':''}`:(entry.session_active_movement_name?`会话记录：${entry.session_active_movement_name}`:'—'));td(row,item?.name_score===undefined?'—':`${(Number(item.name_score)*100).toFixed(1)}%`,'compact-number');const rect=item?.name_rect;td(row,rect?`x ${rect.x}, y ${rect.y}, 宽 ${rect.width}, 高 ${rect.height}`:'—');td(row,item?.type_boundary?(entry.scan_stop_reason||'已到达加工品列表末尾，停止向后扫描'):(entry.outcome||'—'));body.append(row)}}}
function renderProcessingEvidenceImages(d){const host=$('processing-evidence-images');host.replaceChildren();const evidence=processingEvidenceAtStep(d),sourceOrder=['inventory_ocr','movement_panel_ocr','loaded_icon'],sourceNames={inventory_ocr:'零件箱截图',movement_panel_ocr:'移动方式选择截图',loaded_icon:'地图已装载图标'},legend=document.createElement('div');legend.className='evidence-legend';legend.innerHTML='<span><i class="dot" style="background:transparent;border-color:#ffd45c"></i>黄色实线框：每一个名称 OCR 命中（同种加工品不合并）</span><span><i class="dot" style="background:transparent;border-color:#a982ef;border-style:dashed"></i>紫色虚线框：加工品列表边界</span><span><i class="dot" style="background:transparent;border-color:#50e6d5"></i>青色实线框：零件箱中亮起的剩余次数星</span><span><i class="dot" style="background:transparent;border-color:#8290a0;border-style:dashed"></i>灰色虚线框：已经熄灭的次数星</span><span>每一栏独立取本步骤之前的最近证据。</span><span>截图与框使用同一原始分辨率，可直接对照。</span>';host.append(legend);for(const evidenceType of sourceOrder){const section=document.createElement('section');section.className='evidence-source-group';const heading=document.createElement('h3');heading.textContent=sourceNames[evidenceType];section.append(heading);const entries=evidence.filter(entry=>entry.evidence_type===evidenceType);if(!entries.length){const empty=document.createElement('div');empty.className='muted';empty.textContent='本步骤之前没有这一类证据。';section.append(empty);host.append(section);continue}for(const entry of entries){const images=entry.evidence_images||[],items=[...(entry.items||[]),...(entry.type_boundary_items||[])],overlayItems=entry.evidence_type==='movement_panel_ocr'&&entry.ocr_name_hits?.length?entry.ocr_name_hits:items,details=document.createElement('details');details.open=true;const summary=document.createElement('summary');summary.textContent=`观测 ${entry.accepted_observation_id||'未知'} · ${entry.scan_stop_reason||entry.outcome||'现场截图'}`;details.append(summary);if(!images.length){const empty=document.createElement('div');empty.className='muted';empty.textContent='这条记录没有附图。';details.append(empty)}for(const evidenceImage of images){const shot=document.createElement('div');shot.className='evidence-shot';const image=document.createElement('img');image.src=evidenceImage.file;image.alt=`${sourceNames[evidenceType]} · ${evidenceImage.role||'现场'}`;const overlay=svgEl('svg',{viewBox:`0 0 ${evidenceImage.width} ${evidenceImage.height}`,preserveAspectRatio:'xMidYMid meet'});for(const item of overlayItems){if(item.evidence_image_role!==evidenceImage.role||!item.name_rect)continue;const rect=item.name_rect;overlay.append(svgEl('rect',{x:rect.x,y:rect.y,width:rect.width,height:rect.height,class:`ocr-box${item.type_boundary?' boundary':''}`}));for(const slot of item.star_slots||[]){if(!slot.rect)continue;overlay.append(svgEl('rect',{x:slot.rect.x,y:slot.rect.y,width:slot.rect.width,height:slot.rect.height,class:`star-box${slot.lit?'':' depleted'}`}))}const label=svgEl('text',{x:rect.x,y:Math.max(20,rect.y-5),class:'ocr-label'});label.textContent=`${item.type_boundary?`${item.boundary_label||'加工品列表边界'}：`:''}${item.name||'加工品'} ${item.name_score===undefined?'':`${(Number(item.name_score)*100).toFixed(1)}%`}`;overlay.append(label)}shot.append(image,overlay);details.append(shot)}section.append(details)}host.append(section)}}
function renderSteps(d,candidate){const body=$('steps');body.replaceChildren();const steps=candidate?(candidate.planned_route_steps||[]):(d.planned_route_steps||[]);const deterministic=candidate?candidate.route_deterministic!==false:true;$('route-note').textContent=!deterministic?'该候选包含随机落点，不能绘制唯一的逐步路线。':steps.length?`0 为起点，共 ${steps.length} 步；青色为徒步，橙色虚线为加工品移动。`:'该候选没有可展示的确定路线。';for(let index=0;index<steps.length;index++){const step=steps[index],row=document.createElement('tr');td(row,index+1,'compact-number');td(row,`${step.direct_exhaustion?'直接耗尽':(step.movement_name||movementName(d,step.movement))}${step.uses_inferred_edge?'（含推断连线）':''}`);td(row,step.direct_exhaustion?'不选择节点':`${nodeText(step.source)} → ${nodeText(step.landing||step.target)}`);td(row,step.direct_exhaustion?'移动方式面板 → 确认追猎':((step.path||[]).map(nodeText).join(' → ')||'直达'));td(row,`${step.action_points_before??'?'} - ${step.action_point_cost??'?'} + ${step.action_point_gain??0} = ${step.action_points_after??'?'}`);body.append(row)}}
function scoreMeta(d,label,value,scoreIndex=0,candidate=null){const usesExpected=Number(candidate?.route_outcome_count)>1&&Array.isArray(candidate?.expected_lexicographic_score_sum)&&candidate.expected_lexicographic_score_sum.length>0;if(label.startsWith('processing_semantic.')){const movement=label.slice('processing_semantic.'.length),actual=usesExpected?Number(value):(candidate?.processing_move_counts?.[movement]??Number(value));return{name:`加工品：${movementName(d,movement)}`,value:actual,direction:'越少越优（同类优先用低价值加工品）',group:'processing'}}const actualValues={revealed_node_count:candidate?.revealed_node_count,effective_node_count:candidate?.effective_node_count,persistent_processing_move_count:candidate?.persistent_processing_move_count,processing_move_count:candidate?.processing_move_count,route_length:candidate?.route_length,movement_action_count:candidate?.planned_route_steps?.length,battle_count:candidate?.battle_count,risk_score:candidate?.risk_score};const maximize=['revealed_node_count','effective_node_count'].includes(label),negated=maximize||(label==='processing_move_count'&&d.profile==='automation_collection'),actual=usesExpected?(negated?-Number(value):Number(value)):(actualValues[label]??(maximize?-Number(value):value)),direction=label==='processing_move_count'&&d.profile==='automation_collection'?'越多越优（先保证不多用跨层加工品）':(maximize?'越多越优':'越少越优');return{name:scoreNames[label]||`其他策略条件 ${scoreIndex+1}`,value:actual,direction,group:'general'}}
function metric(host,text,title='',decisive=false){const span=document.createElement('span');span.className=`metric${decisive?' decisive':''}`;span.textContent=text;if(title)span.title=title;host.append(span)}
function comparisonScore(candidate){const sums=candidate?.expected_lexicographic_score_sum||[],count=Math.max(1,Number(candidate?.route_outcome_count)||1);return sums.length?sums.map(value=>Number(value)/count):(candidate?.lexicographic_score||[])}
function firstScoreDifference(candidate,neighbor){const first=comparisonScore(candidate),second=comparisonScore(neighbor);for(let index=0;index<Math.max(first.length,second.length);index++)if(first[index]!==second[index])return index;return -1}
function renderScore(d,candidate){const host=$('score-panel');host.replaceChildren();if(!candidate){host.textContent='没有候选评分。';host.className='score-panel muted';return}host.className='score-panel';const candidates=d.candidate_comparison||[],index=candidates.indexOf(candidate),neighbor=candidates[index+1]||candidates[index-1]||null,difference=firstScoreDifference(candidate,neighbor),labels=candidate.lexicographic_score_labels||[],values=comparisonScore(candidate);const title=document.createElement('div');title.className='score-title';const strong=document.createElement('strong');strong.textContent=`#${candidate.rank??index+1} 排序摘要`;title.append(strong);const hint=document.createElement('span');hint.className='muted';hint.textContent='按优先级从左到右比较；随机路线显示全部安全结果的期望值';title.append(hint);host.append(title);if(neighbor&&difference>=0){const meta=scoreMeta(d,labels[difference]||String(difference),values[difference],difference,candidate);const other=scoreMeta(d,labels[difference]||String(difference),comparisonScore(neighbor)[difference],difference,neighbor);const decisive=document.createElement('div');decisive.className='score-groups';const line=document.createElement('div');const label=document.createElement('span');label.className='score-group-label';label.textContent=`与 #${neighbor.rank??index+2} 的首次差异`;line.append(label);metric(line,`${meta.name} ${meta.value} 对 ${other.value}`,meta.direction,true);decisive.append(line);host.append(decisive)}const groups=document.createElement('div');groups.className='score-groups';const main=document.createElement('div');const mainLabel=document.createElement('span');mainLabel.className='score-group-label';mainLabel.textContent='主要指标';main.append(mainLabel);for(let scoreIndex=0;scoreIndex<labels.length;scoreIndex++){const meta=scoreMeta(d,labels[scoreIndex],values[scoreIndex],scoreIndex,candidate);if(meta.group==='general')metric(main,`${meta.name} ${meta.value}`,meta.direction,scoreIndex===difference)}metric(main,`战斗 ${candidate.battle_count??0}`,'仅记录，不参与排序');groups.append(main);const effective=document.createElement('div');const effectiveLabel=document.createElement('span');effectiveLabel.className='score-group-label';effectiveLabel.textContent='有效计分明细';effective.append(effectiveLabel);const effectiveDetails=candidate.effective_node_details||[];for(const detail of effectiveDetails){const note=detail.knot_tentacle_bonus?'坎诺特的触须落点额外 +1；其余分数按节点类型、楼层和标记计算':`按节点类型、楼层和标记计 ${detail.weight} 分`;metric(effective,`${nodeText(detail.node)} +${detail.weight}`,note)}if(!effectiveDetails.length)metric(effective,`总计 ${candidate.effective_node_count??0}`,'路线含随机结果时只显示保证计分，无法列出唯一明细');groups.append(effective);const processing=document.createElement('div');const processingLabel=document.createElement('span');processingLabel.className='score-group-label';processingLabel.textContent='具体加工品';processing.append(processingLabel);let processingCount=0;for(let scoreIndex=0;scoreIndex<labels.length;scoreIndex++){const meta=scoreMeta(d,labels[scoreIndex],values[scoreIndex],scoreIndex,candidate);if(meta.group==='processing'&&Number(meta.value)!==0){metric(processing,`${meta.name.replace('加工品：','')} ${meta.value}`,meta.direction,scoreIndex===difference);processingCount++}}if(!processingCount)metric(processing,'均未使用','具体能力强弱的逐项比较均为 0');groups.append(processing);host.append(groups);const rawDetails=document.createElement('details');rawDetails.className='raw-score';const summary=document.createElement('summary');summary.textContent=`完整排序顺序（${values.length} 项）`;const pre=document.createElement('pre');pre.textContent=labels.map((label,scoreIndex)=>{const meta=scoreMeta(d,label,values[scoreIndex],scoreIndex,candidate);return `${scoreIndex+1}. ${meta.name}：${meta.value}（${meta.direction}）`}).join('\n');rawDetails.append(summary,pre);host.append(rawDetails)}
)HTML"
           << R"HTML(
function renderCandidates(d){const candidates=d.candidate_comparison||[],body=$('candidates'),select=$('candidate'),nodes=d.map_nodes||d.planning_map?.nodes||d.nodes||[],byId=new Map(nodes.map(node=>[String(node.id),node]));body.replaceChildren();select.replaceChildren();candidates.forEach((candidate,index)=>{const target=byId.get(String(candidate.target))||candidate,name=`${nodePresentation(target).title} / ${movementName(d,candidate.movement,candidate)}`,option=document.createElement('option');option.value=String(index);option.textContent=`#${candidate.rank??index+1} ${name}${candidate.selected?'（实际选择）':''}`;select.append(option);const row=document.createElement('tr');row.className=`selectable ${index===candidateIndex?'focused':''} ${candidate.selected?'actual':''}`;row.tabIndex=0;row.title='点击查看此候选的路线、逐步移动、将探明节点和排序摘要';const choose=()=>{candidateIndex=index;renderDecision()};row.addEventListener('click',choose);row.addEventListener('keydown',event=>{if(event.key==='Enter'||event.key===' '){event.preventDefault();choose()}});td(row,candidate.rank??index+1,'compact-number');td(row,name,'candidate-name');td(row,candidate.revealed_node_count,'compact-number');td(row,candidate.effective_node_count,'compact-number');td(row,`${candidate.persistent_processing_move_count??0} / ${candidate.processing_move_count??0}`,'compact-number');td(row,candidate.route_length??candidate.estimated_duration,'compact-number');td(row,candidate.safe_requirement,'compact-number');body.append(row)});if(!candidates.length){const option=document.createElement('option');option.textContent='无候选详情';select.append(option);select.disabled=true}else{select.disabled=false;select.value=String(candidateIndex)}}
function consistencyText(d){const check=d.previous_move_reveal_consistency;if(!check)return'';const label=check.scope==='floor_entry'?'新层初始揭示核对':'上一步揭示核对',nodes=new Map((d.map_nodes||[]).map(node=>[String(node.id),node])),names=ids=>(ids||[]).map(id=>nodeText(nodes.get(String(id)))).join('、')||'无',expected=(check.expected_revealed_nodes||[]).length,observed=(check.observed_revealed_nodes||[]).length;if(check.consistency==='一致')return`\n${label}：一致（预期 ${expected}，实际 ${observed}）`;if(check.consistency==='已校正')return`\n${label}：已按实测校正「弥散虚雾」\n自然揭示受阻：${names(check.natural_reveal_suppressed_nodes)}`;return`\n${label}：不一致（预期 ${expected}，实际 ${observed}）\n漏揭示：${names(check.missing_revealed_nodes)}\n额外揭示：${names(check.unexpected_revealed_nodes)}`}
function coordinateText(position){return position?`(${position.row},${position.column})`:'未定位'}
function utopiaText(d){if(!d.utopia_ideology&&!d.utopia_policy)return'';const ideology=utopiaIdeologyNames[d.utopia_ideology]||'理念未识别',policy=utopiaPolicyNames[d.utopia_policy]||'方针未识别',status=d.utopia_status==='recognized'?'已定位':(d.utopia_status==='abstained'?'本次模型拒识':'未定位'),domain=(d.ideal_domain||[]).map(coordinateText).join('、')||'无',residentExact=d.resident_settlement_exact?`确定 ${coordinateText(d.resident_settlement_exact)}`:((d.resident_settlement_candidates||[]).length?`候选 ${(d.resident_settlement_candidates||[]).map(coordinateText).join('、')}`:'无');return`\n实托邦：${policy} · ${ideology}\n理想源：${status} ${coordinateText(d.ideal_source)}；理想域：${domain}\n“居民”据点规则预判：${residentExact}`}
function renderRecognitionEvidence(d){const details=$('image-details'),captured=$('recognition-captured'),capturedLink=$('recognition-captured-link'),capturedCard=$('recognition-captured-card'),overlay=$('overlay'),overlayLink=$('recognition-overlay-link'),overlayCard=$('recognition-overlay-card'),hasCaptured=Boolean(d.captured_image_file),hasOverlay=Boolean(d.overlay_image_file);capturedCard.hidden=!hasCaptured;overlayCard.hidden=!hasOverlay;if(hasCaptured){captured.src=d.captured_image_file;capturedLink.href=d.captured_image_file}else{captured.removeAttribute('src');capturedLink.removeAttribute('href')}if(hasOverlay){overlay.src=d.overlay_image_file;overlayLink.href=d.overlay_image_file}else{overlay.removeAttribute('src');overlayLink.removeAttribute('href')}details.hidden=!hasCaptured&&!hasOverlay}
function renderDecision(){const d=history[decisionIndex]||{},candidates=d.candidate_comparison||[];if(candidateIndex<0||candidateIndex>=candidates.length){const actual=candidates.findIndex(candidate=>candidate.selected);candidateIndex=actual>=0?actual:(candidates.length?0:-1)}const candidate=selectedCandidate(d),viewName=mapView==='current'?'当前观测':'探索笔记',section=mapSection(d);$('map-view-note').textContent=mapViewDescription(mapView);$('title').textContent=`${section.label}路线规划`;$('summary').textContent=`${viewName} · 地图世代 ${section.generation} · 模板 ${d.topology_template_id||'未匹配'} · 基础边 / 额外边 ${d.topology_base_edge_count??'?'} / ${d.topology_extra_edge_count??'?'} · 行动力 ${d.action_points_before??'?'} → ${d.action_points_after??'?'} · 可用候选 / 全部候选 ${d.eligible_candidates??'?'} / ${d.total_candidates??'?'}`;const endpoint=d.endpoint_fallback_active?'当前无安全终点路线，转为可逆探索；后续观测会重新尝试终点':(d.endpoint_required?'必须保有到终点的安全路线':'允许继续探索');const reserve=d.reserved_full_map_charges??d.reserved_movement_charges??0;const immediateForbidden=d.forbidden_node_types||[],futureForbidden=d.future_forbidden_node_types||immediateForbidden;const futureForbiddenText=futureForbidden.map(type=>type==='hide_battle'&&Number(d.floor)===1?'未知的凶戾（抵达前仍未点亮）':prettyNodeType(type));const residentLookahead=!d.mobile_marker_lookahead_active?'未启用':(d.mobile_marker_lookahead_fallback_active?'严格检验无解，已回退为只避当前一步':`一拍分支检验通过（检查 ${d.mobile_marker_outcomes_checked??0} 种结果）`);const constraints=`终点：${endpoint}\n首步禁止落点：${immediateForbidden.map(prettyNodeType).join('、')||'无'}；后续禁止落点：${futureForbiddenText.join('、')||'无'}\n流窜居民：${residentLookahead}\n全图飞现有次数 / 本层最低预留：${d.automation_collection_full_map_count??'?'} / ${reserve}`;const reason=d.planning_error?`规划失败：${humanMessage(d.planning_error)}`:`决胜：${prettyReasonCategory(d.reason_category)}\n${humanMessage(d.reason_detail)}`;$('reason').textContent=`${reason}\n${constraints}${utopiaText(d)}${consistencyText(d)}`;renderItems(d);renderProcessingEvidence(d);renderProcessingEvidenceImages(d);renderCandidates(d);renderSteps(d,candidate);renderScore(d,candidate);drawMap(d,candidate);renderRecognitionEvidence(d);$('rejected').textContent=(d.rejected_candidates||[]).map(humanMessage).join('\n')||'无';refreshStepControls()}
function renderFloorTabs(){const host=$('floor-tabs');host.replaceChildren();for(const section of mapSections){const button=document.createElement('button');button.type='button';button.className=`floor-button${section.key===mapSectionKey?' active':''}`;button.textContent=section.label;button.title=`地图世代 ${section.generation}`;button.addEventListener('click',()=>{if(section.key===mapSectionKey)return;mapSectionKey=section.key;candidateIndex=-1;renderFloorTabs();refreshDecisionOptions(true);renderDecision()});host.append(button)}}
function refreshDecisionOptions(selectLatest=false){const decision=$('decision');decision.replaceChildren();floorDecisionIndexes=[];history.forEach((item,index)=>{if(mapSection(item).key===mapSectionKey)floorDecisionIndexes.push(index)});for(let step=0;step<floorDecisionIndexes.length;step++){const index=floorDecisionIndexes[step],item=history[index],option=document.createElement('option');option.value=String(index);option.textContent=`步骤 ${step+1} · 行动力 ${item.action_points_before??'?'} · ${prettyReasonCategory(item.reason_category)}`;decision.append(option)}if(selectLatest||!floorDecisionIndexes.includes(decisionIndex))decisionIndex=floorDecisionIndexes.length?floorDecisionIndexes.at(-1):Math.max(0,history.length-1);decision.value=String(decisionIndex);refreshStepControls()}
function refreshStepControls(){const position=floorDecisionIndexes.indexOf(decisionIndex);$('previous-step').disabled=position<=0;$('next-step').disabled=position<0||position>=floorDecisionIndexes.length-1;$('step-counter').textContent=position>=0?`${position+1} / ${floorDecisionIndexes.length}`:'—'}
function changeStep(delta){const position=floorDecisionIndexes.indexOf(decisionIndex),next=position+delta;if(next<0||next>=floorDecisionIndexes.length)return;decisionIndex=floorDecisionIndexes[next];$('decision').value=String(decisionIndex);candidateIndex=-1;renderDecision()}
for(const input of document.querySelectorAll('[data-layer]'))input.addEventListener('change',applyLayerVisibility);
$('clean-map').addEventListener('click',()=>setLayers(['background','route']));
$('original-map').addEventListener('click',()=>setLayers(['background']));
$('full-map').addEventListener('click',()=>setLayers(['background','nodes','markers','reveals','consistency','route','labels','evidence']));
$('panel-toggle').addEventListener('click',()=>{document.body.classList.toggle('panel-hidden');$('panel-toggle').textContent=document.body.classList.contains('panel-hidden')?'显示详情':'宽屏地图'});
$('previous-step').addEventListener('click',()=>changeStep(-1));$('next-step').addEventListener('click',()=>changeStep(1));
$('decision').addEventListener('change',event=>{decisionIndex=Number(event.target.value);candidateIndex=-1;renderDecision()});
$('candidate').addEventListener('change',event=>{candidateIndex=Number(event.target.value);renderDecision()});
for(const button of document.querySelectorAll('[data-view]'))button.addEventListener('click',()=>{mapView=button.dataset.view;for(const current of document.querySelectorAll('[data-view]'))current.classList.toggle('active',current===button);renderDecision()});
document.addEventListener('keydown',event=>{if(['INPUT','SELECT','BUTTON','TEXTAREA'].includes(event.target.tagName))return;if(event.key==='ArrowLeft')changeStep(-1);else if(event.key==='ArrowRight')changeStep(1)});
renderFloorTabs();refreshDecisionOptions(true);renderDecision();
</script></body></html>)HTML";
    if (!output) {
        set_error(error, "failed to write BlackFlow routing visualization");
        return false;
    }
    return true;
}

bool initialize_routing_history_artifacts(
    const std::filesystem::path& run_directory,
    std::string* error)
{
    constexpr std::string_view RoutingDataPrefix = "const BLACKFLOW_ROUTING_HISTORY=[";
    constexpr std::string_view ProcessingDataPrefix = "const BLACKFLOW_PROCESSING_ITEM_HISTORY=[";
    constexpr std::string_view DataSuffix = "];\n";
    if (!write_text_file(run_directory / "routing-history.json", "[]", "routing history", error) ||
        !write_text_file(
            run_directory / "processing-item-history.json",
            "[]",
            "processing item history",
            error) ||
        !write_text_file(
            run_directory / "routing-history-data.js",
            std::string(RoutingDataPrefix) + std::string(DataSuffix),
            "routing visualization data",
            error) ||
        !write_text_file(
            run_directory / "processing-item-history-data.js",
            std::string(ProcessingDataPrefix) + std::string(DataSuffix),
            "processing item visualization data",
            error) ||
        !write_routing_visualization(run_directory / "routing.html", error)) {
        return false;
    }
    return true;
}
} // namespace

bool BlackFlowMapObservationSource::prepare(std::string* error)
{
    if (m_analyzer != nullptr) {
        return true;
    }
    if (!m_initialization_error.empty()) {
        set_error(error, m_initialization_error);
        return false;
    }
    try {
        std::string current_error;
        m_analyzer = BlackFlowMapPerception.acquire(current_error);
        if (m_analyzer == nullptr) {
            m_initialization_error = current_error.empty() ? "BlackFlow map perception is unavailable" : current_error;
            set_error(error, m_initialization_error);
            return false;
        }
        m_initialization_error.clear();
        return true;
    }
    catch (const std::exception& exception) {
        m_initialization_error = "BlackFlow map perception initialization failed: " + std::string(exception.what());
    }
    catch (...) {
        m_initialization_error = "BlackFlow map perception initialization failed: unknown exception";
    }
    set_error(error, m_initialization_error);
    return false;
}

void BlackFlowMapObservationSource::release() noexcept
{
    m_analyzer.reset();
    m_initialization_error.clear();
    m_last_result = {};
    m_last_attempt_id.clear();
    m_accumulated_screenshot_us = 0;
    m_accumulated_recognition_us = 0;
    m_last_attempt_count = 0;
    m_last_retry_count = 0;
    m_diagnostic_run_revision = -1;
    m_diagnostic_event_sequence = 0;
    m_diagnostic_run_timestamp.clear();
    m_processing_item_history.clear();
    m_routing_history_artifacts_initialized = false;
    m_run_log.reset();
}

bool BlackFlowMapObservationSource::recognize(
    const cv::Mat& image,
    const BlackFlowObservationRequest& request,
    BlackFlowMapObservation& observation,
    FactStore& observed_facts,
    std::string* error)
{
    observed_facts.clear();
    const int floor = request.floor;
    const int attempt_count = std::max(1, request.attempt_count);
    if (attempt_count == 1) {
        m_accumulated_screenshot_us = 0;
        m_accumulated_recognition_us = 0;
    }

    BlackFlowMapObservation next;
    next.sequence = ++m_sequence;
    next.viewport_revision = next.sequence;
    next.observation_id = "BF-O" + std::to_string(next.sequence);
    next.floor = floor;
    next.floor_from_ocr = true;
    next.coverage = ObservationCoverage::FullMap;
    next.covered_positions = BlackFlowObservationAdapter::expected_grid_positions(floor);
    next.attempt_count = attempt_count;
    next.retry_count = attempt_count - 1;
    m_last_attempt_id = next.observation_id;
    m_last_attempt_count = next.attempt_count;
    m_last_retry_count = next.retry_count;

    perception::MapRecognitionResult result;
    result.floor = floor;
    bool timing_recorded = false;
    const auto record_timing = [&]() {
        if (timing_recorded) {
            return;
        }
        m_accumulated_screenshot_us += std::max<std::int64_t>(0, request.capture_us) + result.normalization_us;
        m_accumulated_recognition_us += result.recognition_us;
        next.screenshot_us = m_accumulated_screenshot_us;
        next.recognition_us = m_accumulated_recognition_us;
        timing_recorded = true;
    };

    try {
        if (m_analyzer == nullptr && !prepare(error)) {
            result.error = m_initialization_error;
            result.captured_bgr = image.clone();
            record_timing();
            m_last_result = std::move(result);
            observation = std::move(next);
            return false;
        }

        if (topology_cache_requires_reset(m_topology_map_generation, request.map_generation)) {
            m_analyzer->reset_topology_cache();
            Log.info(
                "BlackFlow resets cached topology for a new map generation",
                "previous generation",
                *m_topology_map_generation,
                "current generation",
                request.map_generation,
                "floor",
                floor);
        }
        m_topology_map_generation = request.map_generation;

        result = m_analyzer->recognize(
            image,
            floor,
            request.difficulty,
            request.utopia_ideology,
            request.utopia_policy,
            m_diagnostics.level != DiagnosticLevel::Normal);
        record_timing();
        if (!result.ok) {
            set_error(error, result.error);
            m_last_result = std::move(result);
            observation = std::move(next);
            return false;
        }

        next.recognition_ok = true;
        next.graph_connected = result.edge_detection.graph_connected;
        next.current_marker_temporary_id = result.node_detection.current_marker_node_id;
        next.current_marker_score = result.node_detection.current_marker_score;
        next.covered_positions.clear();
        next.covered_positions.reserve(static_cast<std::size_t>(result.rows * result.columns));
        for (int row = 0; row < result.rows; ++row) {
            for (int column = 0; column < result.columns; ++column) {
                next.covered_positions.emplace_back(GridPosition { row, column });
            }
        }

        next.nodes.reserve(result.node_detection.nodes.size());
        for (const auto& node : result.node_detection.nodes) {
            next.nodes.emplace_back(
                PerceptionNodeObservation {
                    node.id,
                    { node.row, node.column },
                    node.exists,
                    node.type,
                    node.display_name,
                    std::max(node.confidence, node.existence_confidence),
                    node.existence_source,
                    node.identity_source,
                    node.detected_by_vision,
                    node.confirmed_by_topology,
                    node.visually_hidden,
                    node.identity_from_topology,
                    node.identity_from_prediction,
                    node.prediction_rule,
                    node.natural_reveal_suppressed,
                    visual_rect(node, result.normalized_bgr.size()),
                    std::nullopt,
                    node.marker_type,
                    node.marker_display_name,
                    node.marker_score,
                    false,
                    std::nullopt,
                });
        }

        next.topology_template_id = result.topology_template_id;
        next.topology_source_digest = result.topology_source_digest;
        next.topology_base_edge_count = result.topology_base_edge_count;
        next.topology_extra_edge_count = result.topology_extra_edge_count;
        next.topology_match_score = result.topology_match_score;
        next.utopia_status = result.utopia_status;
        next.utopia_reason = result.utopia_reason;
        next.utopia_ideology = result.utopia_ideology;
        next.utopia_policy = result.utopia_policy;
        if (result.ideal_source.has_value()) {
            next.ideal_source = GridPosition { result.ideal_source->row, result.ideal_source->column };
        }
        for (const auto& position : result.ideal_domain) {
            next.ideal_domain.emplace_back(GridPosition { position.row, position.column });
        }
        for (const auto& position : result.observed_ideal_domain) {
            next.observed_ideal_domain.emplace_back(GridPosition { position.row, position.column });
        }
        next.ideal_source_score_margin = result.ideal_source_score_margin;
        next.ideal_source_heads_agree = result.ideal_source_heads_agree;

        next.edges.reserve(result.edge_detection.edges.size());
        for (const auto& edge : result.edge_detection.edges) {
            next.edges.emplace_back(
                PerceptionEdgeObservation {
                    edge.node_a,
                    edge.node_b,
                    edge.connected,
                    edge.cnn_connected,
                    edge.forced_by_connectivity_constraint,
                    edge.calibrated_probability,
                    edge.decision_source,
                });
        }

        observation = std::move(next);
        m_last_result = std::move(result);
        return true;
    }
    catch (const std::exception& exception) {
        result.ok = false;
        result.error = "BlackFlow map recognition failed: " + std::string(exception.what());
    }
    catch (...) {
        result.ok = false;
        result.error = "BlackFlow map recognition failed: unknown exception";
    }
    if (result.captured_bgr.empty() && !image.empty()) {
        result.captured_bgr = image.clone();
    }
    record_timing();
    set_error(error, result.error);
    m_last_result = std::move(result);
    observation = std::move(next);
    return false;
}

void BlackFlowMapObservationSource::reset_run()
{
    if (m_analyzer != nullptr) {
        m_analyzer->reset_topology_cache();
    }
    m_last_result = {};
    m_last_attempt_id.clear();
    m_accumulated_screenshot_us = 0;
    m_accumulated_recognition_us = 0;
    m_last_attempt_count = 0;
    m_last_retry_count = 0;
    m_topology_map_generation.reset();
    m_diagnostic_run_revision = -1;
    m_diagnostic_event_sequence = 0;
    m_diagnostic_run_timestamp.clear();
    m_processing_item_history.clear();
    m_routing_history_artifacts_initialized = false;
    m_run_log.reset();
}

void BlackFlowMapObservationSource::configure_diagnostics(const DiagnosticSettings& settings)
{
    m_diagnostics = settings;
}

bool BlackFlowMapObservationSource::persist_diagnostics(const DiagnosticArtifactRequest& request, std::string* error)
{
    if (!valid_artifact_set_id(request.artifact_set_id)) {
        set_error(error, "BlackFlow diagnostic artifact set id is invalid");
        return false;
    }
    try {
        const std::int64_t run_revision = request.snapshot.get("run_revision", std::int64_t { 0 });
        const int floor = request.snapshot.get("floor", m_last_result.floor);
        if (m_diagnostic_run_revision != run_revision || m_diagnostic_run_timestamp.empty()) {
            m_diagnostic_run_revision = run_revision;
            m_diagnostic_event_sequence = 0;
            m_diagnostic_run_timestamp = current_run_timestamp();
            m_processing_item_history.clear();
            m_routing_history_artifacts_initialized = false;
        }
        if (!m_run_log.prepare(UserDir.get() / "debug" / "BlackFlow", run_revision, error)) {
            return false;
        }
        const auto run_directory = m_run_log.run_directory();
        const std::string floor_directory_name = "floor-" + std::to_string(std::max(0, floor));
        const auto directory = run_directory / floor_directory_name;
        std::filesystem::create_directories(directory);
        json::object snapshot = request.snapshot;
        const std::string snapshot_name = request.artifact_set_id + ".snapshot.json";
        const std::string captured_name = request.artifact_set_id + ".captured.jpg";
        const std::string overlay_name = request.artifact_set_id + ".overlay.jpg";
        snapshot["artifact_set_id"] = request.artifact_set_id;
        snapshot["diagnostic_sequence"] = ++m_diagnostic_event_sequence;
        snapshot["snapshot_file"] = snapshot_name;
        snapshot["accepted_observation_id"] = request.observation_id;
        snapshot["recognition_attempt_id"] = m_last_attempt_id;
        snapshot["recognition_attempt_count"] = m_last_attempt_count;
        snapshot["recognition_retry_count"] = m_last_retry_count;
        snapshot["recognition_screenshot_us"] = m_accumulated_screenshot_us;
        snapshot["recognition_us"] = m_accumulated_recognition_us;
        snapshot["recognition_floor"] = m_last_result.floor;
        snapshot["recognition_floor_source"] = "next_level_ocr";
        snapshot["recognition_topology_template_id"] = m_last_result.topology_template_id;
        snapshot["recognition_topology_source_digest"] = m_last_result.topology_source_digest;
        snapshot["recognition_topology_base_edge_count"] = m_last_result.topology_base_edge_count;
        snapshot["recognition_topology_extra_edge_count"] = m_last_result.topology_extra_edge_count;
        snapshot["recognition_topology_match_score"] = m_last_result.topology_match_score;
        snapshot["captured_image_width"] = m_last_result.captured_bgr.cols;
        snapshot["captured_image_height"] = m_last_result.captured_bgr.rows;
        std::vector<json::value> ocr_diagnostics;
        ocr_diagnostics.reserve(m_last_result.node_detection.ocr_diagnostics.size());
        for (const auto& hit : m_last_result.node_detection.ocr_diagnostics) {
            ocr_diagnostics.emplace_back(
                json::object {
                    { "raw_text", hit.raw_text },
                    { "normalized_text", hit.normalized_text },
                    { "score", hit.score },
                    { "x", hit.rect.x },
                    { "y", hit.rect.y },
                    { "width", hit.rect.width },
                    { "height", hit.rect.height },
                });
        }
        snapshot["recognition_ocr_diagnostics"] = json::array(std::move(ocr_diagnostics));
        std::vector<json::value> node_diagnostics;
        node_diagnostics.reserve(m_last_result.node_detection.nodes.size());
        for (const auto& node : m_last_result.node_detection.nodes) {
            node_diagnostics.emplace_back(
                json::object {
                    { "row", node.row },
                    { "column", node.column },
                    { "exists", node.exists },
                    { "type", node.type },
                    { "identity_source", node.identity_source },
                    { "ocr_raw_text", node.ocr_raw_text },
                    { "ocr_normalized_text", node.ocr_normalized_text },
                    { "ocr_runner_up", node.ocr_runner_up },
                    { "ocr_score", node.ocr_score },
                    { "ocr_similarity", node.ocr_similarity },
                    { "ocr_runner_up_similarity", node.ocr_runner_up_similarity },
                    { "ocr_exact_match", node.ocr_exact_match },
                });
        }
        snapshot["recognition_node_diagnostics"] = json::array(std::move(node_diagnostics));
        if (!m_last_result.error.empty()) {
            snapshot["recognition_error"] = m_last_result.error;
        }
        if (request.include_captured_image && !m_last_result.captured_bgr.empty()) {
            snapshot["captured_image_file"] =
                (std::filesystem::path(floor_directory_name) / captured_name).generic_string();
        }
        if (request.include_images && !m_last_result.captured_bgr.empty()) {
            snapshot["overlay_image_file"] =
                (std::filesystem::path(floor_directory_name) / overlay_name).generic_string();
        }
        std::vector<json::value> evidence_images;
        evidence_images.reserve(request.evidence_images.size());
        for (std::size_t index = 0; index < request.evidence_images.size(); ++index) {
            const auto& evidence = request.evidence_images[index];
            if (evidence.image == nullptr || evidence.image->empty()) {
                continue;
            }
            const std::string evidence_name =
                request.artifact_set_id + ".evidence-" + std::to_string(index + 1) + ".jpg";
            if (!write_image_if_present(directory / evidence_name, *evidence.image, error)) {
                return false;
            }
            evidence_images.emplace_back(
                json::object {
                    { "role", evidence.role },
                    { "file", (std::filesystem::path(floor_directory_name) / evidence_name).generic_string() },
                    { "width", evidence.image->cols },
                    { "height", evidence.image->rows },
                });
        }
        snapshot["evidence_images"] = json::array(std::move(evidence_images));
        std::ofstream snapshot_file(directory / snapshot_name, std::ios::binary);
        if (!snapshot_file) {
            set_error(error, "failed to create BlackFlow diagnostic snapshot");
            return false;
        }
        const std::string serialized_snapshot = json::value(snapshot).format();
        snapshot_file << serialized_snapshot;
        if (request.include_captured_image &&
            !write_captured_preview_if_present(directory / captured_name, m_last_result.captured_bgr, error)) {
            return false;
        }
        if (request.include_images) {
            cv::Mat overlay = m_last_result.overlay_bgr;
            if (overlay.empty()) {
                overlay = m_analyzer != nullptr ? m_analyzer->draw_overlay(m_last_result)
                                                : m_last_result.captured_bgr.clone();
            }
            if (!write_image_if_present(directory / overlay_name, overlay, error)) {
                return false;
            }
        }
        const bool routing_history_event =
            request.trigger == DiagnosticTrigger::RoutingDecision ||
            request.trigger == DiagnosticTrigger::BattleStageObservation ||
            request.trigger == DiagnosticTrigger::NodeIdentityResolved;
        const bool processing_history_event =
            request.trigger == DiagnosticTrigger::ProcessingItemObservation;
        if (routing_history_event || processing_history_event) {
            if (!m_routing_history_artifacts_initialized) {
                if (!initialize_routing_history_artifacts(run_directory, error)) {
                    return false;
                }
                m_routing_history_artifacts_initialized = true;
            }
            if (routing_history_event) {
                const auto enriched = enrich_routing_snapshot_with_processing_evidence(
                    serialized_snapshot,
                    m_processing_item_history);
                if (!enriched.has_value()) {
                    set_error(error, "failed to enrich BlackFlow routing history entry");
                    return false;
                }
                const std::string formatted_entry = json::value(*enriched).format();
                const std::string compact_entry = json::value(*enriched).to_string();
                if (!append_framed_json_array_entry(
                        run_directory / "routing-history.json",
                        "[",
                        "]",
                        formatted_entry,
                        "routing history",
                        error) ||
                    !append_framed_json_array_entry(
                        run_directory / "routing-history-data.js",
                        "const BLACKFLOW_ROUTING_HISTORY=[",
                        "];\n",
                        compact_entry,
                        "routing visualization data",
                        error)) {
                    return false;
                }
            }
            else {
                const std::string compact_entry = json::value(snapshot).to_string();
                if (!append_framed_json_array_entry(
                        run_directory / "processing-item-history.json",
                        "[",
                        "]",
                        serialized_snapshot,
                        "processing item history",
                        error) ||
                    !append_framed_json_array_entry(
                        run_directory / "processing-item-history-data.js",
                        "const BLACKFLOW_PROCESSING_ITEM_HISTORY=[",
                        "];\n",
                        compact_entry,
                        "processing item visualization data",
                        error)) {
                    return false;
                }
                m_processing_item_history.emplace_back(serialized_snapshot);
            }
        }
        RunLogLevel level = RunLogLevel::Info;
        switch (request.trigger) {
        case DiagnosticTrigger::RoutineObservation:
        case DiagnosticTrigger::RoutingDecision:
        case DiagnosticTrigger::BattleStageObservation:
        case DiagnosticTrigger::NodeIdentityResolved:
        case DiagnosticTrigger::ProcessingItemObservation:
            break;
        default:
            level = RunLogLevel::Warning;
            break;
        }
        std::string action = "diagnostic." + std::string(to_string(request.trigger));
        for (char& character : action) {
            if (character >= 'A' && character <= 'Z') {
                character = static_cast<char>(character - 'A' + 'a');
            }
        }
        const RunLogEvent event {
            .level = level,
            .action = std::move(action),
            .phase = "persisted",
            .outcome = "success",
            .task = "BlackFlowDiagnostics",
            .transaction_id = request.transaction_id,
            .state = request.snapshot,
            .details = json::object {
                { "trigger", std::string(to_string(request.trigger)) },
                { "artifact_set_id", request.artifact_set_id },
                { "snapshot_file",
                  (std::filesystem::path(floor_directory_name) / snapshot_name).generic_string() },
                { "captured_image_file", snapshot.get("captured_image_file", std::string()) },
                { "overlay_image_file", snapshot.get("overlay_image_file", std::string()) },
                { "evidence_images", snapshot.at("evidence_images") },
            },
        };
        // captured/overlay JPEG 已是该诊断事件的权威证据，重放日志直接引用它，
        // 避免再向 images/ 写入一份内容相同的 JPEG。
        return record_run_event(
            static_cast<std::uint64_t>(std::max<std::int64_t>(0, run_revision)),
            event,
            nullptr,
            error);
    }
    catch (const std::exception& exception) {
        set_error(error, "BlackFlow diagnostic persistence failed: " + std::string(exception.what()));
        return false;
    }
    catch (...) {
        set_error(error, "BlackFlow diagnostic persistence failed: unknown exception");
        return false;
    }
}

bool BlackFlowMapObservationSource::record_run_event(
    std::uint64_t run_revision,
    const RunLogEvent& event,
    const cv::Mat* image,
    std::string* error)
{
    return m_run_log.record(UserDir.get() / "debug" / "BlackFlow", run_revision, event, image, error);
}

bool BlackFlowMapObservationSource::record_node_attribution(
    std::uint64_t run_revision,
    const std::filesystem::path& relative_directory,
    std::string_view attribution,
    std::string* error)
{
    return m_run_log.append_node_attribution(
        UserDir.get() / "debug" / "BlackFlow",
        run_revision,
        relative_directory,
        attribution,
        error);
}
} // namespace asst::blackflow
