#pragma once

// Inject OCR text only at the model boundary; run the production task's
// normalization, explicit replacements and required-name filter unchanged.
struct BlackFlowItemNameFilterProbe : asst::OCRer
{
    explicit BlackFlowItemNameFilterProbe(const std::string& task)
    {
        set_task_info(task);
    }
    explicit BlackFlowItemNameFilterProbe(std::shared_ptr<asst::OcrTaskInfo> task)
    {
        set_task_info(std::move(task));
    }

    std::optional<Result> resolve(std::string text) const
    {
        Result result;
        result.text = std::move(text);
        result.score = 0.99;
        postproc_trim_(result);
        postproc_replace_(result);
        return filter_and_replace_by_required_(result) ? std::optional<Result>(result) : std::nullopt;
    }
};

template <class Test>
void item_name_regressions(const std::filesystem::path& repo, Test&& test)
{
    using namespace asst;
    for (const std::string task : {
             "MovementInventoryItems", "MovementInventoryAllItems", "MovementPanelItems",
             "TrophyRewardNameOCR", "CultivateSellItems", "CultivateShelfSeed" }) {
        test(("item names keep two-letter exact matching and restore isolated three-letter correction in " + task).c_str(), [&] {
            const auto key = "BlackFlow@Roguelike@" + task;
            BlackFlowItemNameFilterProbe probe(key);
            const auto info = Task.get<OcrTaskInfo>(
                task == "TrophyRewardNameOCR" ? "BlackFlow@Roguelike@TrophyRewardNames" : key);
            require(info != nullptr, "item name task missing");
            probe.set_required(info->text);
            for (const auto& name : info->text) {
                const auto result = probe.resolve(name);
                require(result.has_value(), "exact item disappeared: " + name);
            }
            for (const auto& [raw, target] : std::array { std::pair { "稻子", "种子" } }) {
                if (std::ranges::find(info->text, target) == info->text.end()) continue;
                require(!probe.resolve(raw), std::string(raw) + " became another item in " + task);
            }
            for (const auto& [raw, target] : std::array {
                     std::pair { "小八田", "小八界" }, std::pair { "薄葬花", "薄葬草" },
                     std::pair { "重弾簧", "重弹簧" } }) {
                if (std::ranges::find(info->text, target) == info->text.end()) continue;
                const auto result = probe.resolve(raw);
                require(result && result->text == target, std::string(raw) + " lost isolated three-letter correction");
            }
            if (std::ranges::find(info->text, "白模鸟") != info->text.end() &&
                std::ranges::find(info->text, "白模狗") != info->text.end()) {
                require(!probe.resolve("白摸鸟") && !probe.resolve("白模乌"), "neighboring concept identity was guessed");
            }
        });
    }
    test("short item filtering preserves long-name OCR tolerance and documented aliases", [&] {
        BlackFlowItemNameFilterProbe probe("BlackFlow@Roguelike@MovementInventoryAllItems");
        for (const auto& [raw, expected] : std::array {
                 std::pair { "标准引警", "标准引擎" }, std::pair { "垫底座", "气垫底座" },
                 std::pair { "次性喷气背包", "一次性喷气背包" },
                 std::pair { "简易遥控器", "“简易遥控器”" } }) {
            const auto result = probe.resolve(raw);
            require(result && result->text == expected, std::string(raw) + " lost existing correction");
        }
    });
    test("item recognition opt-in preserves default matcher behavior and protects full three-letter neighborhoods", [&] {
        const auto task = Task.get<OcrTaskInfo>("BlackFlow@Roguelike@MovementInventoryAllItems");
        require(task->fuzzy_match_min_length == 3 && task->fuzzy_match_isolated_length == 3, "item policy was not loaded");
        auto legacy = std::make_shared<OcrTaskInfo>(*task);
        legacy->fuzzy_match_isolated_length = 0;
        BlackFlowItemNameFilterProbe unchanged(legacy), guarded("BlackFlow@Roguelike@MovementInventoryAllItems");
        require(unchanged.resolve("白摸鸟")->text == "白模鸟", "default matcher semantics changed");
        require(!guarded.resolve("白摸鸟") && !guarded.resolve("白模乌"), "neighboring short identity was guessed");
        require(!guarded.resolve("恋家草"), "equidistant isolated candidates were guessed");
        auto inherited = Task.get<OcrTaskInfo>("BlackFlow@Roguelike@TrophyRewardNames");
        require(inherited->fuzzy_match_isolated_length == 3, "task inheritance lost the isolation guard");
        require(Task.get<OcrTaskInfo>("BlackFlow@Roguelike@StageEncounterOcr")->fuzzy_match_isolated_length == 0,
                "unrelated event OCR inherited an item policy");
    });
    test("item store name recognition includes unpurchased neighbors before policy filtering", [&] {
        auto config = std::make_shared<RoguelikeConfig>();
        blackflow::BlackFlowAutomationStoreTaskPlugin store {{}, nullptr, "Roguelike", config, nullptr, nullptr, nullptr};
        const auto task = blackflow::BlackFlowStoreReplayAccess::name_task(store, "BlackFlow@Roguelike@AutomationShopGoods");
        require(task && task->fuzzy_match_min_length == 3 && task->fuzzy_match_isolated_length == 3,
                "automatic store retained the blanket three-letter ban");
        BlackFlowItemNameFilterProbe names(task);
        const auto alpha = names.resolve("沙盘α");
        require(alpha && alpha->text == "沙盘α", "unpurchased alpha was corrected to beta");
        require(!names.resolve("沙盒β"), "beta was fuzzy-corrected despite its alpha neighbor");
        require(names.resolve("小八田")->text == "小八界", "store lost safe isolated part correction");
        require(!names.resolve("白摸鸟"), "complete store identity vocabulary lost concept neighbors");
        const auto policy = Task.get<OcrTaskInfo>("BlackFlow@Roguelike@AutomationShopGoods");
        require(std::ranges::count(policy->text, "沙盘α") == 0, "test requires alpha outside the purchase policy");
    });
    const auto fixture_root = repo / "unit_test/MaaCore/fixtures/blackflow-item-names/20261025";
    test("shared part identity cases use production OCR postprocessing", [&] {
        const auto examples = json::open(fixture_root / "matching-cases.json");
        require(examples.has_value(), "shared name cases missing");
        BlackFlowItemNameFilterProbe probe("BlackFlow@Roguelike@MovementInventoryAllItems");
        for (const auto& item : examples->at("parts").as_array()) {
            const auto raw = item.at("raw").as_string();
            const auto result = probe.resolve(raw);
            if (item.at("name").is_null()) require(!result, "unknown name was accepted: " + raw);
            else require(result && result->text == item.at("name").as_string(), "identity changed: " + raw);
        }
    });
    const auto cases = json::open(fixture_root / "sources.json");
    require(cases.has_value(), "item-name original-image provenance missing");
    for (const auto& item : cases->as_array()) {
        const auto filename = item.at("file").as_string();
        test(("captured item names preserve seed and seed pod " + filename).c_str(), [&] {
            const auto image = MAA_NS::imread(fixture_root / filename);
            require(!image.empty(), "item-name original image missing");
            OCRer current(image);
            current.set_task_info("BlackFlow@Roguelike@MovementInventoryAllItems");
            const auto results = current.analyze();
            require(results && results->size() == 6, "complete six-slot page lost items");
            for (const auto& name : { "种子", "种荚" }) {
                require(std::ranges::count_if(*results, [&](const auto& r) { return r.text == name; }) == 1,
                        std::string(name) + " missing or duplicated by another name");
            }
            // An incomplete/future dictionary must reject the unknown identity,
            // rather than manufacturing a second seed to make the count match.
            auto names = Task.get<OcrTaskInfo>("BlackFlow@Roguelike@MovementInventoryAllItems")->text;
            std::erase(names, "种荚");
            current.set_required(std::move(names));
            const auto incomplete = current.analyze();
            require(incomplete && incomplete->size() == 5, "unknown seed pod was accepted under another name");
            require(std::ranges::count_if(*incomplete, [](const auto& r) { return r.text == "种子"; }) == 1,
                    "unknown seed pod manufactured a second seed");
        });
    }
    test("sacrifice selected item and collectible names remain unrestricted raw OCR", [&] {
        const auto task = Task.get<OcrTaskInfo>("BlackFlow@Roguelike@SacrificeSelectedName");
        require(task && task->text.empty() && !task->fuzzy_match,
                "selected name was narrowed to a policy list");
        BlackFlowItemNameFilterProbe probe(task->name);
        const auto result = probe.resolve("未收录的藏品");
        require(result && result->text == "未收录的藏品", "unknown selected name was rewritten");
    });
}
