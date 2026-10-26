// Included inside the controlled production replay main; these are real producer methods.
    json::array encounter_fixtures;
    json::array generic_fixtures;
    json::array sacrifice_empty_fixtures;
    struct ChoiceScenario {
        RoguelikeStageEncounterTaskPlugin p;
        json::array events;
        std::string fail_phase;
        std::uint64_t revision = 20;
        ChoiceScenario() {
            p.m_sacrifice_context_provider = [&] { return std::optional(blackflow::SacrificeContext{1, revision}); };
            p.m_event_detail_observer = [&](auto event, auto phase, json::object details, const cv::Mat& image) {
                if (phase == fail_phase) return false;
                events.emplace_back(json::object{{"event_name",std::string(event)}, {"phase",std::string(phase)},
                    {"details",std::move(details)}, {"has_image",!image.empty()}});
                return true;
            };
            p.m_option_list = {{true,{},"原始选项",""}, {true,{},"另一选项",""}, {true,{},"离开",""}};
            p.m_option_list_image=p.controller.get_image();
        }
        bool begin(std::string_view name="货从口出", int round=0) {return p.prepare_encounter_choice(name,round,p.controller.get_image());}
        int count(std::string_view phase) const {
            int n=0;for (const auto& e:events) n+=e.get("phase",std::string())==phase;return n;
        }
    };
    for (int index : {0,1,2}) test("cargo actual selection and observed transition", [&] {
        ChoiceScenario s;
        s.revision+=index;
        require(s.begin(),"candidates");
        std::vector<size_t> order{1,2};
        require(s.p.randomize_encounter_choice_order(order),"random order");
        const auto planned=order;
        require(s.p.randomize_encounter_choice_order(order) && order==planned && s.count("order")==1,"retry redrew order");
        auto sorted=order; std::ranges::sort(sorted);require(sorted==std::vector<size_t>{1,2},"random eligibility changed");
        require(s.p.plan_encounter_choice(index,index==2?"configured_fallback":"random_group_order"),"plan");
        require(s.p.select_analyzed_option(index),"input");
        require(s.count("confirmed")==0 && s.count("click_issued")==1,"input became confirmation");
        require(s.p.confirm_cargo_choice() && s.count("confirmed")==1,"transition missing");
        require(s.p.confirm_cargo_choice() && s.count("confirmed")==1,"duplicate confirmation");
        require(s.events.at(s.events.size()-1).get("details","option_index",0)==index+1,"actual option lost");
        encounter_fixtures.emplace_back(json::object{{"case","cargo-"+std::to_string(index)}, {"payloads",s.events}});
    });
    for (const auto phase : {"candidates","planned","attempt","click_issued","confirmed"}) test("cargo evidence save failure stops input",[&]{
        ChoiceScenario s;s.fail_phase=phase;
        if(s.begin() && s.p.plan_encounter_choice(0,"random_group_order") && s.p.select_analyzed_option(0))
            (void)s.p.confirm_cargo_choice();
        require(!s.p.parent.enabled && s.p.m_encounter_choice.failed,"save failure ignored");
        require(s.count("confirmed")==0,"failed evidence confirmed");
        if(std::string_view(phase)=="attempt")require(s.p.controller.choice_clicks==0,"clicked before write");
    });
    test("unchanged cargo page is not a confirmed choice",[&]{
        ChoiceScenario s;require(s.begin() && s.p.plan_encounter_choice(0,"random_group_order"),"plan");
        s.p.controller.choice_effective=false;
        require(!s.p.select_analyzed_option(0),"unchanged input succeeded");
        require(!s.p.confirm_cargo_choice() && !s.p.parent.enabled && !s.count("confirmed"),"unchanged page confirmed");
    });
    test("failed cargo input is recorded without confirmation",[&]{
        ChoiceScenario s;require(s.begin() && s.p.plan_encounter_choice(0,"random_group_order"),"plan");
        s.p.controller.input_ok=false;
        require(!s.p.select_analyzed_option(0) && s.count("input_failed")==3 && !s.count("click_issued"),"input failure hidden");
    });
    test("confirmed cargo followup never reuses original indices",[&]{
        ChoiceScenario s;require(s.begin() && s.p.plan_encounter_choice(1,"random_group_order") && s.p.select_analyzed_option(1) && s.p.confirm_cargo_choice(),"initial");
        s.p.m_option_list={{true,{},"领取奖励",""}};std::vector<size_t> order;
        require(s.p.randomize_encounter_choice_order(order) && order.empty(),"old indices reused in reward page");
    });
    test("cargo delayed unacknowledged input cannot switch to another claimed choice",[&]{
        ChoiceScenario s;require(s.begin() && s.p.plan_encounter_choice(0,"random_group_order"),"plan");
        s.p.controller.choice_effective=false;(void)s.p.select_analyzed_option(0);
        require(!s.p.plan_encounter_choice(1,"configured_fallback") && !s.p.parent.enabled && !s.count("confirmed"),"ambiguous input attributed to fallback");
    });
    test("later leave or reward click never borrows an unconfirmed initial choice",[&]{
        ChoiceScenario s;require(s.begin() && s.p.plan_encounter_choice(0,"random_group_order") && s.p.select_analyzed_option(0),"initial");
        const int attempts=s.count("attempt"), issued=s.count("click_issued");
        s.p.m_option_list[0].text="离开";
        require(s.p.select_analyzed_option(0) && s.count("attempt")==attempts && s.count("click_issued")==issued,"unrelated input mislabeled as initial choice");
    });
    test("cargo network overlay wins over background map",[&]{
        ChoiceScenario s;require(s.begin() && s.p.plan_encounter_choice(0,"random_group_order") && s.p.select_analyzed_option(0),"input");
        s.p.controller.page=9;
        require(!s.p.confirm_cargo_choice() && !s.count("confirmed"),"background map confirmed pending request");
    });
    test("cargo direct collection reward confirms without blocking its original close flow",[&]{
        ChoiceScenario s;require(s.begin() && s.p.plan_encounter_choice(0,"random_group_order") && s.p.select_analyzed_option(0),"input");
        s.p.controller.page=13;
        require(s.p.confirm_cargo_choice() && s.count("confirmed")==1 && !s.p.controller.advances,"reward confirmation blocked or advanced page");
        encounter_fixtures.emplace_back(json::object{{"case","cargo-direct-reward"},{"payloads",s.events}});
    });
    for(const auto name:{"得偿","襁褓金乌","普通直接到账事件"})test("generic actual fallback and independent receipt",[&]{
        ChoiceScenario s; s.p.m_option_list[0].enabled=false;
        require(s.begin(name),"generic candidates");
        require(s.p.select_analyzed_option(1),"actual enabled fallback");
        s.p.controller.page=13;
        require(s.p.observe_encounter_choice_result(s.p.controller.get_image()),"receipt observation");
        require(s.count("planned")==1&&s.count("click_issued")==1,"resolved selection missing");
        require(s.count("confirmed")==1,"independent popup receipt missing");
        require(s.events.at(s.events.size()-1).at("details").get("option_index",0)==2,"configured first option substituted for actual fallback");
        require(s.events.at(s.events.size()-1).at("details").get("acquisition_status",std::string())=="unresolved","choice confirmation invented acquisition");
        generic_fixtures.emplace_back(json::object{{"case","generic-"+std::string(name)},{"payloads",s.events}});
    });
    test("generic second stage keeps distinct choice identity and links the first receipt",[&]{
        ChoiceScenario s;require(s.begin("得偿")&&s.p.select_analyzed_option(1),"first input");
        const auto first=s.p.m_encounter_choice.details;
        s.p.m_option_list={{true,{},"领取第一件",""},{true,{},"放弃",""}};
        require(s.begin("得偿")&&s.count("confirmed")==1,"secondary page did not prove transition");
        require(s.p.m_encounter_choice.step==1&&!s.p.m_encounter_choice.selected,"secondary page reused first selection");
        require(s.p.select_analyzed_option(0),"second input");
        auto ids=std::set<std::string>{};for(const auto& e:s.events)if(e.get("phase",std::string())=="planned") ids.insert(e.at("details").get("choice_id",std::string()));
        require(ids.size()==2,"multi-stage choice IDs collided");
    });
    test("generic unchanged input has evidence without a fabricated award",[&]{
        ChoiceScenario s;require(s.begin("普通事件"),"candidates");s.p.controller.choice_effective=false;
        require(!s.p.select_analyzed_option(0)&&!s.count("confirmed"),"unchanged page confirmed");
        require(!s.p.select_analyzed_option(1)&&!s.p.parent.enabled,"ambiguous dispatched input switched to fallback");
    });
    test("generic random groups retain their own eligibility and retry order",[&]{
        ChoiceScenario s;require(s.begin("普通事件"),"candidates");std::vector<size_t> a{1,2},b{3};
        require(s.p.randomize_encounter_choice_order(a)&&s.p.randomize_encounter_choice_order(b),"orders");
        const auto original=a;std::vector<size_t> retry{1,2};require(s.p.randomize_encounter_choice_order(retry),"retry");
        require(b==std::vector<size_t>{3}&&retry==original&&s.count("order")==2,"group order leaked or redrawn");
    });
    test("changed event title with a new candidate page confirms only the old source choice",[&]{
        ChoiceScenario s;require(s.begin("第一阶段")&&s.p.select_analyzed_option(1),"first input");
        s.p.m_option_list={{true,{},"领取第二阶段",""}};
        require(s.begin("第二阶段")&&s.count("confirmed")==1,"old choice lost on title change");
        require(s.p.m_encounter_choice.step==1&&s.p.m_encounter_choice.details.get("parent_choice_status",std::string())=="confirmed","source link missing");
    });
    test("configured next title with unchanged options cannot fabricate a receipt",[&]{
        ChoiceScenario s;require(s.begin("第一阶段")&&s.p.select_analyzed_option(1),"first input");
        require(s.begin("另一配置阶段")&&s.count("confirmed")==0,"configured title alone confirmed input");
        require(s.p.m_encounter_choice.details.get("parent_choice_status",std::string())=="unresolved","unknown source was discarded");
    });
    test("transient missing post-input frame stays pending without stopping or inventing a receipt",[&]{
        ChoiceScenario s;require(s.begin("普通事件")&&s.p.select_analyzed_option(1),"input");
        require(s.p.observe_encounter_choice_result({})&&s.count("result_pending")==1&&s.p.parent.enabled,"empty result capture stopped original routing");
        require(!s.count("confirmed"),"missing frame became a receipt");
    });
    const auto start_cargo_decline = [&](ChoiceScenario& s) {
        s.p.m_option_list = {
            {false,{},"想要一个能节省力气的",""}, {false,{},"想要一个可以保值的",""},
            {false,{},"想要一个树海特色纪念品",""}, {true,{},"没什么想买的",""}};
        require(s.begin() && s.p.plan_encounter_choice(3,"configured_fallback") && s.p.select_analyzed_option(3),"decline input");
        s.p.controller.images_read = 0;
        s.p.controller.page = 14;
    };
    test("002 cargo fourth-option decline confirms the ending without advancing it",[&]{
        ChoiceScenario s; start_cargo_decline(s);
        const int clicks=s.p.controller.choice_clicks;
        require(s.p.confirm_cargo_choice() && s.count("confirmed")==1 && s.p.parent.enabled,"event ending was rejected");
        require(s.p.controller.images_read==2,"event ending was not confirmed on two consecutive frames");
        require(s.p.controller.choice_clicks==clicks && !s.p.controller.advances,"passive confirmation clicked the ending");
        const auto& details=s.events.at(s.events.size()-1).at("details");
        require(details.get("option_index",0)==4 && details.get("option",std::string())=="没什么想买的" &&
                    details.get("confirmation_basis",std::string())=="event_ending_after_decline", "wrong decline attribution");
        require(s.p.confirm_cargo_choice() && s.count("confirmed")==1,"ending duplicated the selected choice");
        if(argc>1) std::ofstream(std::filesystem::path(argv[1]).parent_path()/"cargo-decline-payloads.json") <<
            json::object{{"case","cargo-decline-ending"},{"payloads",s.events}}.to_string();
    });
    for (int gap : {15,16,17}) test("cargo ending counter resets across overlays and unknown frames",[&]{
        ChoiceScenario s; start_cargo_decline(s);
        s.p.controller.frame_pages={14,gap,14,14};
        require(s.p.confirm_cargo_choice() && s.p.controller.images_read==4,"isolated ending frame survived a recognition gap");
        require(!s.p.controller.advances,"transition probe clicked a confirmation");
    });
    for (int page : {15,16,17}) test("cargo loading syncing and unknown pages cannot confirm a decline",[&]{
        ChoiceScenario s; start_cargo_decline(s); s.p.controller.page=page;
        require(!s.p.confirm_cargo_choice() && !s.count("confirmed") && !s.p.parent.enabled,"unknown or covered ending confirmed");
    });
    test("cargo single ending frame cannot establish a decline",[&]{
        ChoiceScenario s; start_cargo_decline(s); s.p.controller.frame_pages={14,17};
        require(!s.p.confirm_cargo_choice() && !s.count("confirmed"),"one transient check established a completed choice");
    });
    test("cargo purchase choice cannot borrow the decline ending anchor",[&]{
        ChoiceScenario s; require(s.begin() && s.p.plan_encounter_choice(0,"random_group_order") && s.p.select_analyzed_option(0),"purchase input");
        s.p.controller.page=14;
        require(!s.p.confirm_cargo_choice() && !s.count("confirmed"),"decline-only anchor accepted a purchase");
    });
    test("cargo unsaved ending evidence stops without clicking the ending",[&]{
        ChoiceScenario s; start_cargo_decline(s); s.fail_phase="confirmed";
        const int clicks=s.p.controller.choice_clicks;
        require(!s.p.confirm_cargo_choice() && !s.p.parent.enabled && !s.count("confirmed"),"unsaved decline was confirmed");
        require(s.p.controller.choice_clicks==clicks && !s.p.controller.advances,"failed save advanced the ending");
    });
    test("cargo context change resets identity and order",[&]{
        ChoiceScenario s;require(s.begin(),"begin");std::vector<size_t> order{1,2};
        require(s.p.randomize_encounter_choice_order(order),"order");++s.revision;
        require(s.begin() && s.p.m_encounter_choice.order.empty() && !s.p.m_encounter_choice.selected,"stale choice reused");
        require(s.count("candidates")==2,"new page candidates missing");
    });
    for (int stage : {0,1,2}) for (int change : {0,1,2}) test("generic choice cannot capture a later node input or result",[&]{
        ChoiceScenario s;
        require(s.begin("普通事件")&&s.p.plan_encounter_choice(0,"configured_option"),"old plan");
        if(stage>0)require(s.p.select_analyzed_option(0),"old input");
        if(stage==2){s.p.controller.page=4;require(s.p.observe_encounter_choice_result(s.p.controller.get_image()),"old receipt");}
        const auto events=s.events.size();const int clicks=s.p.controller.choice_clicks;
        if(change==0)++s.revision;
        if(change==1)s.p.m_sacrifice_context_provider=[&]{return std::optional(blackflow::SacrificeContext{2,s.revision});};
        if(change==2)s.p.m_sacrifice_context_provider=[]{return std::optional<blackflow::SacrificeContext>{};};
        s.p.m_option_list={{true,{},"新节点的独立选项",""}};
        require(s.p.select_analyzed_option(0)&&s.p.observe_encounter_choice_result(s.p.controller.get_image()),"new input stopped");
        require(!s.p.m_encounter_choice.context&&s.events.size()==events&&s.p.controller.choice_clicks==clicks+1,
                "retired choice captured another node or fabricated a receipt");
    });
    test("sacrifice picker confirms actual initial choice and second round remains first",[&]{
        ChoiceScenario s;require(s.p.refresh_sacrifice_context(),"context");
        require(s.p.handle_sacrifice_event() && s.count("event-choice")==1,"initial choice");
        require(!s.count("confirmed"),"planned choice confirmed");
        require(s.p.handle_sacrifice_picker() && s.count("confirmed")==1 && s.count("item-selected")==1,"picker receipt");
        s.p.m_option_list={{true,{},"获得加工品",""}};
        require(s.p.handle_sacrifice_event() && s.count("exchange-reward")==1 && s.count("reward-claim-click-issued")==1,"reward stages missing");
        s.p.controller.page=1;
        s.p.m_option_list={{true,{},"原始选项",""},{true,{},"另一选项",""},{true,{},"离开",""}};
        require(s.p.handle_sacrifice_event() && s.p.m_sacrifice.initial_choice==0,"second round changed policy");
        require(s.p.handle_sacrifice_picker() && s.count("confirmed")==2,"second picker confirmation");
        encounter_fixtures.emplace_back(json::object{{"case","sacrifice-two-rounds"},{"payloads",s.events}});
    });
    test("sacrifice parts prefer thin grass over the random first card and record its actual name", [&] {
        ChoiceScenario s;auto& c=s.p.controller;
        c.sacrifice_cards={{685,185,85,80},{798,185,85,80},{911,185,85,80}};
        c.sacrifice_names={"种子","薄葬草","薄葬草"};c.sacrifice_preferred=1;
        require(s.p.refresh_sacrifice_context()&&s.p.handle_sacrifice_event()&&s.p.handle_sacrifice_picker(),"exchange");
        require(c.sacrifice_selected==1&&c.sacrifice_card_clicks==1&&c.sacrifice_confirmations==1,"preferred card not exchanged");
        require(s.p.m_sacrifice.selected_name=="薄葬草"&&s.count("item-selected")==1,"actual selected name missing");
    });
    test("sacrifice without thin grass keeps random selection", [&] {
        ChoiceScenario s;auto& c=s.p.controller;
        c.sacrifice_cards={{685,185,85,80},{798,185,85,80}};c.sacrifice_names={"种子","生逝"};
        require(s.p.refresh_sacrifice_context()&&s.p.handle_sacrifice_event()&&s.p.handle_sacrifice_picker(),"exchange");
        require(c.sacrifice_selected==0&&c.sacrifice_confirmations==1&&s.p.m_sacrifice.selected_name=="种子","random policy changed");
    });
    test("sacrifice priority repeats in the second round and preserves its first event option", [&] {
        ChoiceScenario s;auto& c=s.p.controller;
        c.sacrifice_cards={{685,185,85,80},{798,185,85,80}};c.sacrifice_names={"种子","薄葬草"};c.sacrifice_preferred=1;
        require(s.p.refresh_sacrifice_context()&&s.p.handle_sacrifice_event()&&s.p.handle_sacrifice_picker(),"first exchange");
        s.p.m_option_list={{true,{},"获得加工品",""}};
        require(s.p.handle_sacrifice_event(),"first reward");c.page=1;c.sacrifice_selected=0;
        s.p.m_option_list={{true,{},"原始选项",""},{true,{},"另一选项",""},{true,{},"离开",""}};
        require(s.p.handle_sacrifice_event()&&s.p.m_sacrifice.initial_choice==0&&s.p.handle_sacrifice_picker(),"second exchange");
        require(c.sacrifice_probes==2&&c.sacrifice_confirmations==2&&s.p.m_sacrifice.selected_name=="薄葬草","priority not reset per round");
    });
    test("sacrifice unconfirmed preferred name retries the same card without confirming", [&] {
        ChoiceScenario s;auto& c=s.p.controller;
        c.sacrifice_cards={{685,185,85,80},{798,185,85,80}};c.sacrifice_names={"种子",""};c.sacrifice_preferred=1;
        require(s.p.refresh_sacrifice_context()&&s.p.handle_sacrifice_event()&&s.p.handle_sacrifice_picker(),"first observation");
        require(c.sacrifice_probes==1&&c.sacrifice_confirmations==0&&s.count("item-selected")==0,"unreadable name became exchange");
        c.sacrifice_names[1]="薄葬草";
        require(s.p.handle_sacrifice_picker()&&c.sacrifice_probes==1&&c.sacrifice_card_clicks==1&&c.sacrifice_confirmations==1,"retry changed selected card");
    });
    test("sacrifice false preferred icon never confirms a mismatched name as thin grass", [&] {
        ChoiceScenario s;auto& c=s.p.controller;
        c.sacrifice_cards={{685,185,85,80},{798,185,85,80}};c.sacrifice_names={"种子","生逝"};c.sacrifice_preferred=1;
        require(s.p.refresh_sacrifice_context()&&s.p.handle_sacrifice_event()&&s.p.handle_sacrifice_picker(),"priority observation");
        require(c.sacrifice_confirmations==0&&s.count("item-selected")==0&&s.p.m_sacrifice.preferred_part_rejected,"mismatch confirmed");
        require(s.p.handle_sacrifice_picker()&&c.sacrifice_probes==1&&c.sacrifice_confirmations==1&&s.p.m_sacrifice.selected_name=="种子","random fallback stuck on rejected priority");
    });
    test("sacrifice user stop during preferred recognition issues no selection or confirmation", [&] {
        ChoiceScenario s;auto& c=s.p.controller;
        c.sacrifice_cards={{685,185,85,80},{798,185,85,80}};c.sacrifice_names={"种子","薄葬草"};c.sacrifice_preferred=1;
        c.stop_on_sacrifice_probe=true;
        require(s.p.refresh_sacrifice_context()&&s.p.handle_sacrifice_event(),"initial choice");
        require(!s.p.handle_sacrifice_picker()&&!c.sacrifice_card_clicks&&!c.sacrifice_confirmations,"input after stop");
    });
    test("sacrifice confirmed empty returns on first dispatch then leaves", [&] {
        ChoiceScenario s;
        require(s.p.refresh_sacrifice_context() && s.p.handle_sacrifice_event(), "initial choice");
        s.p.controller.sacrifice_list_states = {1};
        require(s.p.handle_sacrifice_picker(), "empty picker");
        require(s.p.controller.back_clicks == 1 && s.p.m_sacrifice.phase == blackflow::SacrificePhase::Finished,
                "confirmed empty still waits instead of returning");
        require(s.p.m_sacrifice.picker_attempts.count == 1 && s.count("picker-recovery") == 1,
                "empty recovery not persisted on first dispatch");
        require(s.count("item-selected") == 0 && s.count("exchange-reward") == 0, "empty became exchange");
        sacrifice_empty_fixtures.emplace_back(json::object{{"case","sacrifice-confirmed-empty"},{"payloads",s.events}});
        s.p.controller.page = 1;
        require(s.p.handle_sacrifice_event() && s.p.controller.choice_clicks == 2, "did not leave event");
    });
    for (const auto states : {std::vector<int>{2}, std::vector<int>{1,2}, std::vector<int>{2,1},
                              std::vector<int>{1,0}, std::vector<int>{0,1}}) {
        test("sacrifice unknown or changing list is never confirmed empty", [&] {
            ChoiceScenario s;
            require(s.p.refresh_sacrifice_context() && s.p.handle_sacrifice_event(), "initial choice");
            s.p.controller.sacrifice_list_states = states;
            require(s.p.handle_sacrifice_picker(), "unknown picker");
            require(!s.p.controller.back_clicks && s.p.m_sacrifice.phase == blackflow::SacrificePhase::AwaitPicker,
                    "recognition failure or transient empty caused exit");
            require(s.count("picker-recovery") == 0 && s.count("item-selected") == 0, "unknown became evidence");
        });
    }
    test("sacrifice unknown list retains bounded recovery", [&] {
        ChoiceScenario s;
        require(s.p.refresh_sacrifice_context() && s.p.handle_sacrifice_event(), "initial choice");
        s.p.controller.sacrifice_list_states = {2};
        for (int i = 0; i < blackflow::SacrificePickerAttempts::Limit; ++i) {
            require(s.p.handle_sacrifice_picker() && !s.p.controller.back_clicks, "unknown returned early");
        }
        require(s.p.handle_sacrifice_picker() && s.p.controller.back_clicks == 1, "unknown not bounded");
        require(s.events.at(s.events.size() - 1).get("details", "reason", std::string()) == "retry_limit_reached",
                "unknown recorded as no exchangeable items");
        sacrifice_empty_fixtures.emplace_back(json::object{{"case","sacrifice-unconfirmed-list"},{"payloads",s.events}});
    });
    test("sacrifice empty evidence failure prevents return input", [&] {
        ChoiceScenario s;
        require(s.p.refresh_sacrifice_context() && s.p.handle_sacrifice_event(), "initial choice");
        s.p.controller.sacrifice_list_states = {1};
        s.fail_phase = "picker-recovery";
        require(!s.p.handle_sacrifice_picker() && !s.p.parent.enabled && !s.p.controller.back_clicks,
                "returned without saving empty evidence");
    });
    test("sacrifice empty failed return stops task", [&] {
        ChoiceScenario s;
        require(s.p.refresh_sacrifice_context() && s.p.handle_sacrifice_event(), "initial choice");
        s.p.controller.sacrifice_list_states = {1};
        s.p.controller.back_ok = false;
        require(!s.p.handle_sacrifice_picker() && !s.p.parent.enabled && s.p.controller.back_clicks == 1,
                "failed return continued");
    });
    test("sacrifice user stop after saving empty evidence does not become recovery failure", [&] {
        ChoiceScenario s;
        require(s.p.refresh_sacrifice_context() && s.p.handle_sacrifice_event(), "initial choice");
        s.p.controller.sacrifice_list_states = {1};
        const auto observe = s.p.m_event_detail_observer;
        s.p.m_event_detail_observer = [&](auto event, auto phase, json::object details, const cv::Mat& image) {
            const bool saved = observe(event, phase, std::move(details), image);
            if (phase == "picker-recovery") s.p.controller.stopped = true;
            return saved;
        };
        require(!s.p.handle_sacrifice_picker() && !s.p.controller.back_clicks && s.p.parent.enabled,
                "user stop became failed recovery or issued return input");
    });
    for (const auto phase : {"event-choice","item-selected","exchange-reward","reward-claim-attempt","reward-claim-click-issued"}) test("sacrifice failure stops before subsequent action",[&]{
        ChoiceScenario s;s.fail_phase=phase;require(s.p.refresh_sacrifice_context(),"context");
        if(s.p.handle_sacrifice_event() && s.p.handle_sacrifice_picker()){
            s.p.m_option_list={{true,{},"获得加工品",""}};(void)s.p.handle_sacrifice_event();
        }
        require(!s.p.parent.enabled && s.p.m_sacrifice.evidence_failed,"sacrifice write failure ignored");
        if(std::string_view(phase)=="event-choice")require(!s.p.controller.choice_clicks,"choice issued after failed save");
        require(!s.p.handle_sacrifice_event(),"failure resumed");
    });
    test("civilization choice keeps pre-scan and confirms only after transition",[&]{
        ChoiceScenario s;require(s.p.refresh_sacrifice_context(),"context");
        s.p.m_option_list[0].text="复原文明";
        require(s.p.handle_sacrifice_event(),"schedule scan");
        require(s.p.m_sacrifice.phase==blackflow::SacrificePhase::BeforeCivilization && !s.p.controller.choice_clicks,"scan order");
        require(s.p.finish_sacrifice_civilization() && s.p.handle_sacrifice_event(),"choice after scan");
        require(!s.count("confirmed"),"civilization plan confirmed early");
        s.p.controller.page=4;
        require(s.p.finish_sacrifice_civilization() && s.count("confirmed")==1 && s.count("civilization-consumed")==1,"civilization receipt");
        encounter_fixtures.emplace_back(json::object{{"case","civilization"},{"payloads",s.events}});
    });
    if(argc>3) test("shared encounter payload fixture matches actual producer methods", [&] {
        require(json::open(argv[3]).value()==json::value(encounter_fixtures),"encounter producer payload drifted");
    });
    if(argc>3) test("shared decline payload fixture matches actual producer methods", [&] {
        const auto actual=std::filesystem::path(argv[1]).parent_path()/"cargo-decline-payloads.json";
        const auto expected=std::filesystem::path(argv[3]).parent_path()/"cargo-decline/choice-payloads.json";
        require(json::open(actual).value()==json::open(expected).value(),"decline producer payload drifted");
    });
    if(argc>3) test("shared sacrifice empty payload fixture matches actual producer methods", [&] {
        const auto expected=std::filesystem::path(argv[3]).parent_path().parent_path().parent_path()/"blackflow-sacrifice/20261025/lifecycle-payloads.json";
        require(json::open(expected).value()==json::value(sacrifice_empty_fixtures),"sacrifice empty producer payload drifted");
    });
    if(argc>1) std::ofstream(std::filesystem::path(argv[1]).parent_path()/"encounter-payloads.json")<<json::value(encounter_fixtures).to_string();
    if(argc>1) std::ofstream(std::filesystem::path(argv[1]).parent_path()/"sacrifice-empty-payloads.json")<<json::value(sacrifice_empty_fixtures).to_string();

    if(argc>1) std::ofstream(std::filesystem::path(argv[1]).parent_path()/"generic-payloads.json")<<json::value(generic_fixtures).to_string();
