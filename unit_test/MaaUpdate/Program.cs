using System.IO.Compression;
using System.Security.Cryptography;
using System.Text.Json;
using System.Text.Json.Nodes;
using MaaWpfGui.Services;

int passed = 0;
string root = Path.Combine(Path.GetTempPath(), "maa-update-tests-" + Guid.NewGuid().ToString("N"));
Directory.CreateDirectory(root);
try
{
    string version = "v1.2.3";
    string name = $"MAA-BlackFlow-Data-Collection-{version}-win-x64.zip";
    string zipPath = Path.Combine(root, name);
    CreateZip(zipPath, BlackFlowUpdate.Channel, version);
    byte[] data = File.ReadAllBytes(zipPath);
    string hash = Convert.ToHexString(SHA256.HashData(data));
    var manifest = JsonSerializer.SerializeToNode(new {
        schema_version = 1, channel = BlackFlowUpdate.Channel, version,
        release_url = "https://github.com/example/releases/tag/blackflow-v1.2.3", release_notes = "notes",
        assets = new Dictionary<string, object> { ["win-x64"] = new { name, size = data.Length, sha256 = hash, url = "https://github.com/example/releases/download/blackflow-v1.2.3/" + name } },
    })!;
    var release = BlackFlowUpdate.ParseRelease(manifest.ToJsonString(), "x64");
    Check(release.Version == version && release.Package.Name == name, "parse channel release");
    Check(BlackFlowUpdate.VersionNumber("v1.10.0") > BlackFlowUpdate.VersionNumber("v1.9.9+commit"), "numeric version ordering");
    await BlackFlowUpdate.VerifyFileAsync(zipPath, release.Package);
    BlackFlowUpdate.ValidatePackage(zipPath, version);
    Check(true, "real zip integrity and identity");
    foreach (var mutation in new Action<JsonNode>[] {
        n => n["channel"] = "maa", n => n["schema_version"] = 2,
        n => n["assets"]!["win-x64"]!["url"] = "http://example.com/update.zip",
        n => n["assets"]!["win-x64"]!["name"] = "../update.zip",
        n => n["assets"]!["win-x64"]!["sha256"] = "bad",
        n => n["version"] = "v1.2.3-beta",
    })
    {
        var changed = manifest.DeepClone();
        mutation(changed);
        Reject(() => BlackFlowUpdate.ParseRelease(changed.ToJsonString(), "x64"), "reject foreign/invalid feed");
    }

    var badHash = release.Package with { Sha256 = new string('0', 64) };
    try { await BlackFlowUpdate.VerifyFileAsync(zipPath, badHash); throw new Exception("Hash mismatch accepted"); }
    catch (InvalidDataException) { Check(true, "reject hash mismatch"); }
    Reject(() => BlackFlowUpdate.ValidatePackage(zipPath, "v1.2.4"), "reject wrong package version");
    CreateZip(Path.Combine(root, "foreign.zip"), "maa", version);
    Reject(() => BlackFlowUpdate.ValidatePackage(Path.Combine(root, "foreign.zip")), "reject upstream package identity");
    CreateZip(Path.Combine(root, "traversal.zip"), BlackFlowUpdate.Channel, version, "../outside.txt");
    Reject(() => BlackFlowUpdate.ValidatePackage(Path.Combine(root, "traversal.zip")), "reject archive traversal before extraction");
    CreateZip(Path.Combine(root, "ota.zip"), BlackFlowUpdate.Channel, version, "changes.json");
    Reject(() => BlackFlowUpdate.ValidatePackage(Path.Combine(root, "ota.zip")), "reject OTA mixing");
    Console.WriteLine($"Passed {passed} BlackFlow update checks.");
}
finally { Directory.Delete(root, true); }

void Check(bool value, string label) { if (!value) throw new Exception(label); passed++; Console.WriteLine("PASS " + label); }
void Reject(Action action, string label) { try { action(); } catch (InvalidDataException) { Check(true, label); return; } throw new Exception(label); }
void CreateZip(string path, string channel, string version, string? extra = null)
{
    using var zip = ZipFile.Open(path, ZipArchiveMode.Create);
    foreach (var name in new[] { "MAA.exe", "MaaCore.dll", "MAA.Updater.exe", "filelist.txt", "resource/blackflow-gui-defaults.json", "resource/tasks/test.json" })
    {
        using var writer = new StreamWriter(zip.CreateEntry(name).Open()); writer.Write("fixture");
    }
    using (var writer = new StreamWriter(zip.CreateEntry(BlackFlowUpdate.MetadataFile).Open()))
        writer.Write(JsonSerializer.Serialize(new { schema_version = 1, channel, version }));
    if (extra != null) zip.CreateEntry(extra);
}
