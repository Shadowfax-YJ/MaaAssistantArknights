#include "BlackFlowRunLog.h"
#include "BlackFlowRunIntegrity.h"

#include <algorithm>
#include <cctype>
#include <ctime>
#include <iomanip>
#include <limits>
#include <random>
#include <sstream>
#include <thread>

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include "BlackFlowCollectionPopup.h"

#include "MaaUtils/Conf.h"
#include "MaaUtils/ImageIo.h"

namespace asst::blackflow
{
namespace
{
std::string new_run_uuid()
{
    std::random_device random;
    unsigned char bytes[16];
    for (auto& byte : bytes) {
        byte = static_cast<unsigned char>(random());
    }
    bytes[6] = (bytes[6] & 0x0f) | 0x40;
    bytes[8] = (bytes[8] & 0x3f) | 0x80;
    std::ostringstream result;
    result << std::hex << std::setfill('0');
    for (int index = 0; index < 16; ++index) {
        if (index == 4 || index == 6 || index == 8 || index == 10) {
            result << '-';
        }
        result << std::setw(2) << static_cast<unsigned int>(bytes[index]);
    }
    return result.str();
}

void set_error(std::string* error, std::string message)
{
    if (error != nullptr) {
        *error = std::move(message);
    }
}

std::string utc_timestamp(std::chrono::system_clock::time_point now = std::chrono::system_clock::now())
{
    const std::time_t wall_clock = std::chrono::system_clock::to_time_t(now);
    std::tm utc {};
#ifdef _WIN32
    gmtime_s(&utc, &wall_clock);
#else
    gmtime_r(&wall_clock, &utc);
#endif
    const auto milliseconds =
        std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % std::chrono::seconds(1);
    std::ostringstream result;
    result << std::put_time(&utc, "%Y-%m-%dT%H:%M:%S") << '.' << std::setfill('0') << std::setw(3)
           << milliseconds.count() << 'Z';
    return result.str();
}

std::string directory_timestamp(std::chrono::system_clock::time_point now = std::chrono::system_clock::now())
{
    const std::time_t wall_clock = std::chrono::system_clock::to_time_t(now);
    std::tm local {};
#ifdef _WIN32
    localtime_s(&local, &wall_clock);
#else
    localtime_r(&wall_clock, &local);
#endif
    const auto microseconds =
        std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()) % std::chrono::seconds(1);
    std::ostringstream result;
    result << std::put_time(&local, "%Y%m%d-%H%M%S") << '-' << std::setfill('0') << std::setw(6)
           << microseconds.count();
    return result.str();
}

std::string safe_stem(std::string_view action)
{
    std::string result;
    result.reserve(std::min<std::size_t>(action.size(), 48));
    for (const unsigned char character : action) {
        if (result.size() >= 48) {
            break;
        }
        if (std::isalnum(character) != 0) {
            result.push_back(static_cast<char>(std::tolower(character)));
        }
        else if (!result.empty() && result.back() != '-') {
            result.push_back('-');
        }
    }
    while (!result.empty() && result.back() == '-') {
        result.pop_back();
    }
    return result.empty() ? "event" : result;
}

std::string thread_id()
{
    std::ostringstream result;
    result << std::this_thread::get_id();
    return result.str();
}

bool is_safe_existing_jpeg(const std::filesystem::path& run_directory, const std::string& relative_path)
{
    if (relative_path.empty() || relative_path.find("..") != std::string::npos) {
        return false;
    }
    const std::filesystem::path path(relative_path);
    if (path.is_absolute()) {
        return false;
    }
    std::string extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return (extension == ".jpg" || extension == ".jpeg") && std::filesystem::is_regular_file(run_directory / path);
}

bool is_safe_relative_directory(const std::filesystem::path& path)
{
    if (path.empty() || path.is_absolute()) {
        return false;
    }
    return std::ranges::none_of(path, [](const std::filesystem::path& component) {
        return component == ".." || component == ".";
    });
}

bool is_collection_node_directory(const std::filesystem::path& path)
{
    if (!is_safe_relative_directory(path)) {
        return false;
    }
    const std::filesystem::path floor = path.parent_path();
    return path.filename().generic_string().starts_with("node-") &&
           (floor.filename().generic_string().starts_with("floor-") || floor.filename() == "tree-hole") &&
           floor.parent_path() == std::filesystem::path(CollectionPopupRootDirectory);
}
} // namespace

BlackFlowRunLog::~BlackFlowRunLog()
{
    reset();
}

bool BlackFlowRunLog::prepare(
    const std::filesystem::path& root_directory,
    std::uint64_t run_revision,
    std::string* error)
{
    std::scoped_lock lock(m_mutex);
    return ensure_started(root_directory, run_revision, error);
}

bool BlackFlowRunLog::ensure_started(
    const std::filesystem::path& root_directory,
    std::uint64_t run_revision,
    std::string* error)
{
    if (m_jsonl.is_open() && m_text.is_open() && m_replay_data.is_open() && m_run_revision == run_revision) {
        return true;
    }
    if (m_jsonl.is_open()) {
        m_jsonl.close();
    }
    if (m_text.is_open()) {
        m_text.close();
    }
    if (m_replay_data.is_open()) {
        m_replay_data.close();
    }
    if (!m_run_directory.empty()) {
        forget_run_integrity(m_run_directory);
    }
    m_run_directory.clear();
    m_sequence = 0;
    m_last_image.reset();
    m_last_image_details = {};
    m_last_image_state.clear();
    m_last_image_action.clear();
    m_last_image_sequence = 0;

    try {
        const auto now = std::chrono::system_clock::now();
        m_run_directory = root_directory / ("run-" + directory_timestamp(now));
        std::filesystem::create_directories(m_run_directory / "images");
        begin_run_integrity(m_run_directory);
        std::filesystem::create_directories(m_run_directory / collection_popup_other_directory());
        m_jsonl.open(m_run_directory / "run-events.jsonl", std::ios::out | std::ios::app | std::ios::binary);
        m_text.open(m_run_directory / "run.log", std::ios::out | std::ios::app | std::ios::binary);
        m_replay_data.open(m_run_directory / "replay-data.js", std::ios::out | std::ios::app | std::ios::binary);
        if (!m_jsonl || !m_text || !m_replay_data) {
            set_error(error, "failed to create BlackFlow run log files");
            return false;
        }
        m_replay_data << "const BLACKFLOW_RUN_EVENTS=[];\n";
        const char* replay_html = R"HTML(<!doctype html><html lang="zh-CN"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>黑流树海局内重放</title>
<style>body{margin:0;background:#0d1117;color:#e6edf3;font:14px/1.5 system-ui,sans-serif}header{position:sticky;top:0;z-index:2;display:flex;gap:8px;align-items:center;padding:10px 14px;background:#161b22;border-bottom:1px solid #30363d}button,input{background:#21262d;color:#e6edf3;border:1px solid #484f58;border-radius:6px;padding:6px 10px}input{flex:1}main{display:grid;grid-template-columns:minmax(420px,3fr) minmax(340px,2fr);gap:12px;padding:12px}.shot,.panel{background:#161b22;border:1px solid #30363d;border-radius:8px;padding:10px}.shot img{display:block;width:100%;height:auto;max-height:78vh;object-fit:contain;background:#05070a}.empty{display:grid;place-items:center;min-height:320px;color:#8b949e}h1{font-size:16px;margin:0 10px 0 0}.meta{color:#8b949e}strong{color:#58a6ff}pre{white-space:pre-wrap;word-break:break-word;max-height:34vh;overflow:auto;background:#0d1117;padding:9px;border-radius:6px}@media(max-width:900px){main{grid-template-columns:1fr}}</style></head>
<body><header><h1>黑流树海局内重放</h1><button id="prev">←</button><button id="next">→</button><input id="seek" type="range" min="0" value="0"><span id="counter"></span></header><main><section class="shot" id="shot"></section><section class="panel"><div id="headline"></div><h3>动作详情</h3><pre id="details"></pre><h3>动作后的关键状态</h3><pre id="state"></pre></section></main>
<script src="replay-data.js"></script><script>const E=BLACKFLOW_RUN_EVENTS,s=document.querySelector('#seek');let i=0;s.max=Math.max(0,E.length-1);function render(){const e=E[i];document.querySelector('#counter').textContent=E.length?`${i+1}/${E.length}`:'0/0';s.value=i;if(!e){document.querySelector('#shot').innerHTML='<div class="empty">当前没有事件；采集中请刷新页面</div>';return}document.querySelector('#headline').innerHTML=`<strong>#${e.sequence} [${e.level}] ${e.action}</strong><br>${e.phase}/${e.outcome}<br><span class="meta">${e.timestamp} · +${e.elapsed_ms}ms · floor ${e.floor} · map ${e.map_generation}/${e.map_revision}</span>`;document.querySelector('#details').textContent=JSON.stringify(e.details,null,2);document.querySelector('#state').textContent=JSON.stringify(e.state,null,2);document.querySelector('#shot').innerHTML=e.image?`<a href="${e.image.path}" target="_blank"><img src="${e.image.path}" alt="事件截图"></a>`:'<div class="empty">本事件没有单独截图</div>'}document.querySelector('#prev').onclick=()=>{i=Math.max(0,i-1);render()};document.querySelector('#next').onclick=()=>{i=Math.min(E.length-1,i+1);render()};s.oninput=()=>{i=Number(s.value);render()};document.onkeydown=e=>{if(e.key==='ArrowLeft')document.querySelector('#prev').click();if(e.key==='ArrowRight')document.querySelector('#next').click()};render();</script></body></html>)HTML";
        std::ofstream replay(m_run_directory / "replay.html", std::ios::out | std::ios::binary);
        replay << replay_html;
        replay.flush();
        m_replay_data.flush();
        if (!m_replay_data || !replay) {
            set_error(error, "failed to create BlackFlow offline replay files");
            return false;
        }
        for (const auto* name : { "run-events.jsonl", "run.log", "replay-data.js", "replay.html" }) {
            run_integrity_record_file(m_run_directory / name);
        }
        m_run_revision = run_revision;
        m_monotonic_start = std::chrono::steady_clock::now();
        const json::object manifest {
            { "schema_version", BlackFlowRunLogSchemaVersion },
            { "logger", "blackflow.run" },
            { "contract_id", "maa.blackflow.raw" },
            { "contract_version", 1 },
            { "archive_schema_version", 1 },
            { "run_uuid", new_run_uuid() },
            { "collection_profile_source", "event.state.profile" },
            { "coverage", "observed-events-only" },
            { "image_layout_version", 1 },
            { "resource_identity", "not-recorded" },
            { "capabilities", json::array { "ordered-events", "image-evidence", "utopia-inspection-v1" } },
            { "collector_version", MAA_VERSION },
            { "run_revision", run_revision },
            { "started_at", utc_timestamp(now) },
            { "timezone", "UTC" },
            { "event_log", "run-events.jsonl" },
            { "human_log", "run.log" },
            { "replay_viewer", "replay.html" },
            { "replay_data", "replay-data.js" },
            { "image_directory", "images" },
            { "collection_popup_directory", std::string(CollectionPopupRootDirectory) },
            { "node_evidence_directory", std::string(CollectionPopupRootDirectory) },
            { "image_codec", "jpeg" },
            { "image_quality", BlackFlowRunLogJpegQuality },
            { "maximum_image_width", BlackFlowRunLogMaximumImageWidth },
            { "maximum_image_height", BlackFlowRunLogMaximumImageHeight },
            { "stitched_event_preserves_full_height", true },
            { "levels", json::array { "TRACE", "DEBUG", "INFO", "WARN", "ERROR" } },
            { "replay_order", "sequence" },
            { "monotonic_clock", "elapsed_ms" },
        };
        std::ofstream output(m_run_directory / "manifest.json", std::ios::out | std::ios::binary);
        if (!output) {
            set_error(error, "failed to create BlackFlow run log manifest");
            return false;
        }
        output << json::value(manifest).format();
        output.flush();
        if (!output) {
            set_error(error, "failed to flush BlackFlow run log manifest");
            return false;
        }
        run_integrity_record_file(m_run_directory / "manifest.json");
        return true;
    }
    catch (const std::exception& exception) {
        set_error(error, "failed to initialize BlackFlow run log: " + std::string(exception.what()));
        return false;
    }
}

bool BlackFlowRunLog::write_image(
    const cv::Mat& image,
    const std::string& stem,
    const std::filesystem::path& relative_directory,
    bool preserve_full_height,
    json::object& image_details,
    std::string* error) const
{
    if (image.empty()) {
        return true;
    }
    const double width_scale = static_cast<double>(BlackFlowRunLogMaximumImageWidth) / image.cols;
    const double height_scale =
        preserve_full_height ? 1.0 : static_cast<double>(BlackFlowRunLogMaximumImageHeight) / image.rows;
    const double scale = std::min({ 1.0, width_scale, height_scale });
    cv::Mat compressed_source;
    if (scale < 1.0) {
        cv::resize(image, compressed_source, cv::Size(), scale, scale, cv::INTER_AREA);
    }
    else {
        compressed_source = image;
    }
    const std::string filename = stem + ".jpg";
    if (!is_safe_relative_directory(relative_directory)) {
        set_error(error, "BlackFlow run log image directory is not a safe relative path");
        return false;
    }
    std::filesystem::create_directories(m_run_directory / relative_directory);
    if (is_collection_node_directory(relative_directory) && !ensure_node_attribution_file(relative_directory, error)) {
        return false;
    }
    const std::filesystem::path path = m_run_directory / relative_directory / filename;
    const std::vector<int> parameters {
        cv::IMWRITE_JPEG_QUALITY,
        BlackFlowRunLogJpegQuality,
        cv::IMWRITE_JPEG_OPTIMIZE,
        1,
    };
    run_integrity_before_write(path);
    if (!MAA_NS::imwrite(path, compressed_source, parameters)) {
        set_error(error, "failed to write BlackFlow run log JPEG: " + path.string());
        return false;
    }
    run_integrity_record_file(path);
    image_details = json::object {
        { "path", (relative_directory / filename).generic_string() },
        { "codec", "jpeg" },
        { "quality", BlackFlowRunLogJpegQuality },
        { "width", compressed_source.cols },
        { "height", compressed_source.rows },
        { "source_width", image.cols },
        { "source_height", image.rows },
    };
    return true;
}

bool BlackFlowRunLog::ensure_node_attribution_file(const std::filesystem::path& relative_directory, std::string* error)
    const
{
    if (!is_collection_node_directory(relative_directory)) {
        set_error(error, "BlackFlow node attribution directory is not a valid node path");
        return false;
    }
    if (std::filesystem::exists(m_run_directory / relative_directory / BlackFlowNodeAttributionFileName)) {
        return true;
    }
    return update_node_attribution_file(relative_directory, {}, std::nullopt, error);
}

bool BlackFlowRunLog::update_node_attribution_file(
    const std::filesystem::path& relative_directory,
    const json::object& attribution,
    std::optional<std::string_view> annotation,
    std::string* error) const
{
    if (!is_safe_relative_directory(relative_directory) ||
        relative_directory.begin()->generic_string() != CollectionPopupRootDirectory) {
        set_error(error, "BlackFlow node attribution directory is not a safe collection path");
        return false;
    }
    const auto path = m_run_directory / relative_directory / BlackFlowNodeAttributionFileName;
    run_integrity_before_write(path);
    json::object metadata;
    if (std::filesystem::exists(path)) {
        auto previous = json::open(path);
        if (!previous.has_value() || !previous->is_object()) {
            set_error(error, "failed to read node attribution metadata; preserving the existing file");
            return false;
        }
        metadata = previous->as_object();
    }
    // 截图归属和身份修正只更新元数据，不能覆盖按发生顺序保存的来源说明。
    // 重复说明代表多次触发（例如丰饶树冢），不能去重。
    json::array annotations;
    if (const auto* existing = metadata.find_value("annotations"); existing != nullptr) {
        if (!existing->is_array() ||
            !std::ranges::all_of(existing->as_array(), [](const auto& value) { return value.is_string(); })) {
            set_error(error, "node attribution annotations must be an array of strings");
            return false;
        }
        annotations = existing->as_array();
    }
    for (const auto& [key, value] : attribution) {
        if (key != "annotations") {
            metadata[key] = value;
        }
    }
    if (annotation.has_value()) {
        annotations.emplace_back(std::string(*annotation));
    }
    metadata["annotations"] = std::move(annotations);
    std::filesystem::create_directories(m_run_directory / relative_directory);
    std::ofstream output(path, std::ios::out | std::ios::binary);
    output << json::value(metadata).format();
    output.flush();
    if (!output) {
        set_error(error, "failed to write node attribution metadata");
        return false;
    }
    run_integrity_record_file(path);
    return true;
}

bool BlackFlowRunLog::ensure_collection_node_directories(const json::object& state, std::string* error) const
{
    try {
        for (const NodeId node : run_log_collection_nodes_to_materialize(state)) {
            const int floor = static_cast<int>(node >> 48U);
            if (!ensure_node_attribution_file(
                    collection_popup_regular_node_directory(floor, node, state.get("tree_hole_outer_floor", 0)),
                    error)) {
                return false;
            }
        }
        if (const json::value* page = state.find_value("page"); page != nullptr && page->is_object()) {
            const int floor = page->get("floor", state.get("floor", 0));
            if (page->get("has_landing", true)) {
                const json::value* raw_node = page->find_value("node");
                const auto node = raw_node == nullptr ? std::nullopt : run_log_node_id(*raw_node);
                if (node.has_value() && *node != 0 && *node != std::numeric_limits<std::uint64_t>::max() &&
                    !ensure_node_attribution_file(
                        collection_popup_regular_node_directory(floor, *node, state.get("tree_hole_outer_floor", 0)),
                        error)) {
                    return false;
                }
            }
            else {
                const std::string name = page->get("node_name", std::string());
                const std::uint64_t revision = page->get("page_revision", std::uint64_t { 0 });
                if (!name.empty() && !ensure_node_attribution_file(
                                         collection_popup_virtual_node_directory(
                                             floor,
                                             name,
                                             revision,
                                             state.get("tree_hole_outer_floor", 0)),
                                         error)) {
                    return false;
                }
            }
        }
        if (const json::value* abstract_node = state.find_value("abstract_node");
            abstract_node != nullptr && abstract_node->is_object() &&
            abstract_node->get("kind", std::string()) == "pursuit") {
            const int floor = abstract_node->get("floor", state.get("floor", 0));
            if (!ensure_node_attribution_file(collection_popup_virtual_node_directory(floor, "追猎", 0), error)) {
                return false;
            }
        }
        return true;
    }
    catch (const std::exception& exception) {
        set_error(error, "failed to create BlackFlow collection node directories: " + std::string(exception.what()));
        return false;
    }
}

bool BlackFlowRunLog::sync_collection_node_identities(const json::object& state, std::string* error) const
{
    const json::value* nodes = state.find_value("exploration_note_nodes");
    if (nodes == nullptr) {
        if (const json::value* notebook = state.find_value("exploration_notebook");
            notebook != nullptr && notebook->is_object()) {
            nodes = notebook->find_value("nodes");
        }
    }
    if (nodes == nullptr || !nodes->is_array()) {
        return true;
    }
    for (const json::value& node : nodes->as_array()) {
        if (!node.is_object() || !node.get("identity_revealed", false) || node.get("identity_from_prediction", false)) {
            continue;
        }
        const json::value* raw_id = node.find_value("id");
        const auto id = raw_id == nullptr ? std::nullopt : run_log_node_id(*raw_id);
        if (!id.has_value() || *id == 0 || *id == std::numeric_limits<std::uint64_t>::max()) {
            continue;
        }
        const int floor = static_cast<int>(*id >> 48U);
        const auto relative_directory =
            collection_popup_regular_node_directory(floor, *id, state.get("tree_hole_outer_floor", 0));
        const auto path = m_run_directory / relative_directory / BlackFlowNodeAttributionFileName;
        // 只修正已有证据目录，不为仅观测到、尚未走过的节点创建证据。
        if (!std::filesystem::exists(path)) {
            continue;
        }
        auto metadata = json::open(path);
        if (!metadata.has_value() || !metadata->is_object()) {
            set_error(error, "failed to read node evidence attribution metadata");
            return false;
        }
        if (metadata->get("kind", std::string()) != "node" || metadata->get("node", std::uint64_t { 0 }) != *id ||
            metadata->get("floor", 0) != floor) {
            continue;
        }
        json::object updated = metadata->as_object();
        updated["node_name"] = node.get("name", std::string());
        updated["node_type"] = node.get("type", std::string("unclassified"));
        updated["identity_source"] = node.get("identity_source", std::string());
        if (const json::value* battle = node.find_value("battle"); battle != nullptr && battle->is_object()) {
            updated["battle"] = *battle;
            if (const json::value* kills = node.find_value("battle_total_kills"); kills != nullptr) {
                updated["battle_total_kills"] = *kills;
            }
        }
        if (json::value(updated).to_string() == metadata->to_string()) {
            continue;
        }
        if (!update_node_attribution_file(relative_directory, updated, std::nullopt, error)) {
            return false;
        }
    }
    return true;
}

bool BlackFlowRunLog::append_node_attribution(
    const std::filesystem::path& root_directory,
    std::uint64_t run_revision,
    const std::filesystem::path& relative_directory,
    std::string_view attribution,
    std::string* error)
{
    std::scoped_lock lock(m_mutex);
    try {
        if (!ensure_started(root_directory, run_revision, error)) {
            return false;
        }
        if (attribution.empty() || attribution.find('\n') != std::string_view::npos ||
            attribution.find('\r') != std::string_view::npos) {
            set_error(error, "BlackFlow node attribution must be one non-empty line");
            return false;
        }
        if (!is_collection_node_directory(relative_directory)) {
            set_error(error, "BlackFlow node attribution directory is not a valid node path");
            return false;
        }
        return update_node_attribution_file(relative_directory, {}, attribution, error);
    }
    catch (const std::exception& exception) {
        set_error(error, "failed to append BlackFlow node attribution: " + std::string(exception.what()));
        return false;
    }
    catch (...) {
        set_error(error, "failed to append BlackFlow node attribution: unknown exception");
        return false;
    }
}

bool BlackFlowRunLog::record(
    const std::filesystem::path& root_directory,
    std::uint64_t run_revision,
    const RunLogEvent& event,
    const cv::Mat* image,
    std::string* error)
{
    std::scoped_lock lock(m_mutex);
    try {
        if (!ensure_started(root_directory, run_revision, error)) {
            return false;
        }
        if (event.action.empty()) {
            set_error(error, "BlackFlow run log action must not be empty");
            return false;
        }
        if (!ensure_collection_node_directories(event.state, error)) {
            return false;
        }

        const std::uint64_t sequence = ++m_sequence;
        const auto elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - m_monotonic_start);
        const std::string timestamp = utc_timestamp();
        json::object image_details;
        if (image != nullptr && !image->empty()) {
            const std::string state = json::value(event.state).to_string();
            double mean_difference = -1.0;
            if (m_last_image != nullptr && !m_last_image->empty() && m_last_image->size() == image->size() &&
                m_last_image->type() == image->type()) {
                const double denominator = static_cast<double>(image->total()) * static_cast<double>(image->channels());
                mean_difference = cv::norm(*m_last_image, *image, cv::NORM_L1) / denominator;
            }
            const bool node_evidence = is_node_evidence_run_log_action(event.action);
            if (!node_evidence && should_reuse_run_log_image(
                                      mean_difference,
                                      state == m_last_image_state,
                                      event.action == m_last_image_action)) {
                image_details = m_last_image_details;
                image_details["reused"] = true;
                image_details["reused_from_sequence"] = m_last_image_sequence;
                image_details["mean_difference"] = mean_difference;
            }
            else {
                std::ostringstream stem;
                stem << std::setfill('0') << std::setw(6) << sequence << '-' << safe_stem(event.action) << '-'
                     << safe_stem(event.phase);
                std::filesystem::path image_directory = "images";
                if (node_evidence) {
                    image_directory = event.action == CollectionPopupRunLogAction
                                          ? event.details.get("collection_popup_directory", std::string())
                                          : event.details.get("node_evidence_directory", std::string());
                    if (image_directory.empty() || image_directory.begin() == image_directory.end() ||
                        image_directory.begin()->generic_string() != CollectionPopupRootDirectory) {
                        set_error(error, "node evidence event has an invalid image directory");
                        return false;
                    }
                }
                const bool preserve_full_height = event.action == NodeEventRunLogAction;
                if (!write_image(*image, stem.str(), image_directory, preserve_full_height, image_details, error)) {
                    return false;
                }
                if (node_evidence) {
                    if (const json::value* attribution = event.details.find_value("attribution");
                        attribution != nullptr && attribution->is_object()) {
                        if (!update_node_attribution_file(
                                image_directory,
                                attribution->as_object(),
                                std::nullopt,
                                error)) {
                            return false;
                        }
                    }
                }
                m_last_image = std::make_shared<cv::Mat>(image->clone());
                m_last_image_details = image_details;
                m_last_image_state = state;
                m_last_image_action = event.action;
                m_last_image_sequence = sequence;
            }
        }
        else {
            // 重放默认保持原始游戏画面；只有未保存 captured 时才回退到识别叠加图。
            std::string artifact_path = event.details.get("captured_image_file", std::string());
            if (!is_safe_existing_jpeg(m_run_directory, artifact_path)) {
                artifact_path = event.details.get("overlay_image_file", std::string());
            }
            if (is_safe_existing_jpeg(m_run_directory, artifact_path)) {
                image_details = json::object {
                    { "path", artifact_path },
                    { "codec", "jpeg" },
                    { "source", "diagnostic_artifact" },
                    { "reused", true },
                };
            }
        }

        // 身份可能在截图后才由事件回调或落点校正确定。与探索笔记一起回写目录描述，
        // 不依赖这个节点之后是否还会产生截图，也不改写历史事件和原始图片。
        if (!sync_collection_node_identities(event.state, error)) {
            return false;
        }

        json::object serialized {
            { "schema_version", BlackFlowRunLogSchemaVersion },
            { "logger", "blackflow.run" },
            { "event_id", "BF-R" + std::to_string(run_revision) + "-" + std::to_string(sequence) },
            { "sequence", sequence },
            { "timestamp", timestamp },
            { "timezone", "UTC" },
            { "elapsed_ms", elapsed.count() },
            { "level", std::string(to_string(event.level)) },
            { "thread_id", thread_id() },
            { "run_revision", run_revision },
            { "action", event.action },
            { "phase", event.phase },
            { "outcome", event.outcome },
            { "task", event.task },
            { "transaction_id", event.transaction_id },
            { "floor", event.state.get("floor", 0) },
            { "map_generation", event.state.get("map_generation", std::uint64_t { 0 }) },
            { "map_revision", event.state.get("map_revision", std::uint64_t { 0 }) },
            { "state", event.state },
            { "details", event.details },
        };
        // 首条事件携带实际程序版本，单独保存 JSONL 也能追溯采集来源。
        if (sequence == 1) {
            serialized["collector_version"] = MAA_VERSION;
        }
        if (!image_details.empty()) {
            serialized["image"] = image_details;
        }

        // JSONL 要求一条事件严格占一行；format() 会插入缩进和换行，只能用于 manifest。
        const std::string compact_event = json::value(serialized).to_string();
        const std::string json_line = compact_event + '\n';
        const std::string replay_line = "BLACKFLOW_RUN_EVENTS.push(" + compact_event + ");\n";
        std::ostringstream text_line;
        text_line << timestamp << " +" << elapsed.count() << "ms #" << sequence << " [" << to_string(event.level)
                  << "] " << event.action << ' ' << event.phase << '/' << event.outcome;
        if (sequence == 1) {
            text_line << " collector_version=" << MAA_VERSION;
        }
        if (const auto difficulty = event.state.find<int>("selected_difficulty")) {
            text_line << " selected_difficulty=" << *difficulty;
        }
        if (!event.task.empty()) {
            text_line << " task=" << event.task;
        }
        if (!event.transaction_id.empty()) {
            text_line << " transaction=" << event.transaction_id;
        }
        if (!image_details.empty()) {
            text_line << " image=" << image_details.get("path", std::string());
        }
        text_line << '\n';
        m_jsonl << json_line;
        m_replay_data << replay_line;
        m_text << text_line.str();
        run_integrity_record_append(m_run_directory / "run-events.jsonl", json_line);
        run_integrity_record_append(m_run_directory / "replay-data.js", replay_line);
        run_integrity_record_append(m_run_directory / "run.log", text_line.str());
        m_jsonl.flush();
        m_text.flush();
        m_replay_data.flush();
        if (!m_jsonl || !m_text || !m_replay_data) {
            set_error(error, "failed to flush BlackFlow run log event");
            return false;
        }
        return true;
    }
    catch (const std::exception& exception) {
        set_error(error, "failed to append BlackFlow run log event: " + std::string(exception.what()));
        return false;
    }
    catch (...) {
        set_error(error, "failed to append BlackFlow run log event: unknown exception");
        return false;
    }
}

std::filesystem::path BlackFlowRunLog::close_current_run() noexcept
{
    std::scoped_lock lock(m_mutex);
    std::filesystem::path completed_run = std::move(m_run_directory);
    if (m_jsonl.is_open()) {
        m_jsonl.flush();
        m_jsonl.close();
    }
    if (m_text.is_open()) {
        m_text.flush();
        m_text.close();
    }
    if (m_replay_data.is_open()) {
        m_replay_data.flush();
        m_replay_data.close();
    }
    m_run_directory.clear();
    m_last_image.reset();
    m_last_image_details = {};
    m_last_image_state.clear();
    m_last_image_action.clear();
    m_last_image_sequence = 0;
    m_run_revision = 0;
    m_sequence = 0;
    return completed_run;
}

void BlackFlowRunLog::reset() noexcept
{
    const auto directory = close_current_run();
    if (!directory.empty()) {
        forget_run_integrity(directory);
    }
}
} // namespace asst::blackflow
