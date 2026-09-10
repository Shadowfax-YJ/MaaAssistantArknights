using System.Text.Json;
using MaaWpfGui.Configuration.Single.MaaTask;
using MaaWpfGui.Models.AsstTasks;
using MaaWpfGui.Services;

var checks = 0;
void Check(bool condition, string message)
{
    if (!condition) throw new InvalidOperationException(message);
    ++checks;
}

var gesture = new CollectionDifficultyUnlockGesture();
for (var click = 0; click < 4; ++click)
    Check(!gesture.RegisterClick(click * 500), "Four clicks must leave difficulty locked.");
Check(gesture.RegisterClick(2000), "The fifth consecutive click must unlock.");
Check(!gesture.RegisterClick(2500), "A completed gesture must reset its counter.");
gesture.Reset();
for (var click = 0; click < 4; ++click) gesture.RegisterClick(click * 100);
Check(!gesture.RegisterClick(1301), "A gap longer than one second must restart counting.");
for (var click = 1; click < 4; ++click)
    Check(!gesture.RegisterClick(1301 + click * 1000), "One-second gaps are allowed but still require five clicks.");
Check(gesture.RegisterClick(5301), "The fifth click at the interval boundary must unlock.");
for (var click = 0; click < 4; ++click) gesture.RegisterClick(click * 100);
gesture.Reset();
Check(!gesture.RegisterClick(400), "Leaving the difficulty area or switching tasks must discard partial clicks.");

var oldConfig = JsonSerializer.Deserialize<RoguelikeTask>("{\"Difficulty\":9,\"Theme\":5,\"Mode\":30002}")!;
Check(!oldConfig.AutomationCollectionDifficultyUnlocked, "Existing configs must remain locked by default.");

foreach (var difficulty in Enumerable.Range(0, 16).Concat([-1, int.MaxValue]))
{
    var config = new RoguelikeTask
    {
        Theme = RoguelikeTheme.BlackFlow,
        Mode = RoguelikeMode.BlackFlowAutomationCollection,
        Difficulty = difficulty,
        AutomationCollectionDifficultyUnlocked = true,
    };
    var restored = JsonSerializer.Deserialize<RoguelikeTask>(JsonSerializer.Serialize(config))!;
    Check(restored.AutomationCollectionDifficultyUnlocked && restored.Difficulty == difficulty,
        "Unlock state and selection must survive a config round trip.");
    var task = new AsstRoguelikeTask
    {
        Theme = restored.Theme,
        Mode = restored.Mode,
        Difficulty = restored.Difficulty,
        AutomationCollectionDifficultyUnlocked = restored.AutomationCollectionDifficultyUnlocked,
        InvestmentEnabled = true,
        Squad = "其他分队",
        Roles = "其他组合",
        CoreChar = "其他干员",
    };
    var parameters = task.Serialize().Params;
    Check((int)parameters["difficulty"]! == difficulty, "Core must receive the unlocked selection.");
    Check((string)parameters["squad"]! == "堡垒战术分队" &&
          (string)parameters["roles"]! == "坚不可摧" &&
          (string)parameters["core_char"]! == "凯尔希·思衡托" &&
          !(bool)parameters["investment_enabled"]!, "Only difficulty may be unlocked.");

    task.AutomationCollectionDifficultyUnlocked = false;
    Check((int)task.Serialize().Params["difficulty"]! == 6, "Locked collection mode must still submit difficulty 6.");
    task.Mode = RoguelikeMode.Exp;
    Check((int)task.Serialize().Params["difficulty"]! == difficulty, "Ordinary modes must preserve existing behavior.");
}

Console.WriteLine($"Passed {checks} collection difficulty checks.");
