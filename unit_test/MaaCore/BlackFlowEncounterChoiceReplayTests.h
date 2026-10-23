// Included inside the controlled production replay main; these are real producer methods.
    json::array encounter_fixtures;
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
    test("cargo context change resets identity and order",[&]{
        ChoiceScenario s;require(s.begin(),"begin");std::vector<size_t> order{1,2};
        require(s.p.randomize_encounter_choice_order(order),"order");++s.revision;
        require(s.begin() && s.p.m_encounter_choice.order.empty() && !s.p.m_encounter_choice.selected,"stale choice reused");
        require(s.count("candidates")==2,"new page candidates missing");
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
    if(argc>1) std::ofstream(std::filesystem::path(argv[1]).parent_path()/"encounter-payloads.json")<<json::value(encounter_fixtures).to_string();
