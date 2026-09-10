using System.IO.Compression;
using System.Security.Cryptography;
using System.Text.Json;
using System.Text.Json.Nodes;
using MaaWpfGui.Services;

if (args.Length == 2 && args[0] == "--validate-installation-root")
{
    var unknown = InstallationDllPolicy.FindUnknown(Directory.GetFiles(args[1], "*.dll"),
        File.Exists(Path.Combine(args[1], BlackFlowUpdate.MetadataFile)));
    if (unknown.Count != 0) throw new InvalidDataException("Startup would reject package DLLs: " + string.Join(", ", unknown));
    Console.WriteLine("PASS actual package startup DLL policy");
    return;
}
if (args.Length == 2 && args[0] == "--validate-package-dlls")
{
    using var package = ZipFile.OpenRead(args[1]);
    var unknown = InstallationDllPolicy.FindUnknown(package.Entries.Select(e => e.FullName)
        .Where(n => !n.Contains('/') && n.EndsWith(".dll", StringComparison.OrdinalIgnoreCase)), true);
    if (unknown.Count != 0) throw new InvalidDataException("Startup would reject package DLLs: " + string.Join(", ", unknown));
    Console.WriteLine("PASS actual ZIP startup DLL policy");
    return;
}

int passed = 0;
string root = Path.Combine(Path.GetTempPath(), "maa-update-tests-" + Guid.NewGuid().ToString("N"));
Directory.CreateDirectory(root);
try
{
    string[] runtimeDlls = ["clrjit.dll", "coreclr.dll", "PresentationFramework.dll", "System.Collections.dll",
        "System.IO.FileSystem.dll", "System.IO.Packaging.dll", "System.Memory.dll", "System.Private.CoreLib.dll",
        "System.Runtime.dll", "System.Runtime.Extensions.dll", "System.Runtime.InteropServices.dll",
        "System.Runtime.InteropServices.RuntimeInformation.dll", "System.Runtime.Loader.dll", "System.Xaml.dll", "WindowsBase.dll"];
    Check(InstallationDllPolicy.FindUnknown(runtimeDlls, true).Count == 0, "collection runtime passes startup DLL policy");
    Check(InstallationDllPolicy.FindUnknown(runtimeDlls, false).Count == runtimeDlls.Length, "ordinary build keeps its DLL policy");
    Check(InstallationDllPolicy.FindUnknown([.. runtimeDlls, "System.Injected.dll", "winhttp.dll"], true)
        .SequenceEqual(new[] { "System.Injected.dll", "winhttp.dll" }), "collection still rejects unknown DLLs including System prefixes");
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
    var requests = new List<Uri>();
    var fallback = await BlackFlowUpdate.CheckAsync(url => {
        requests.Add(url);
        return url.Host == "img.lubiao.wiki" ? Task.FromException<string?>(new IOException("offline")) : Task.FromResult<string?>(manifest.ToJsonString());
    }, "x64");
    Check(fallback.Version == version && requests.Select(x => x.Host).SequenceEqual(new[] { "img.lubiao.wiki", "github.com" }), "CDN feed failure falls back to collection GitHub feed");
    requests.Clear();
    await BlackFlowUpdate.CheckAsync(url => { requests.Add(url); return Task.FromResult<string?>(manifest.ToJsonString()); }, "x64", "GitHub");
    Check(requests.Count == 1 && requests[0].Host == "github.com", "explicit GitHub selection bypasses CDN");
    requests.Clear();
    string target = Path.Combine(root, "download.zip");
    await BlackFlowUpdate.DownloadAsync(release, target, async (url, path) => {
        requests.Add(url);
        await File.WriteAllBytesAsync(path, url.Host == "img.lubiao.wiki" ? new byte[data.Length] : data);
        return true;
    });
    Check(requests.Count == 2 && File.ReadAllBytes(target).SequenceEqual(data), "corrupt CDN package falls back with original hash and identity verification");
    requests.Clear();
    try {
        await BlackFlowUpdate.DownloadAsync(release, target, (url, path) => { requests.Add(url); return Task.FromResult(false); }, "CDN");
        throw new Exception("failed downloads accepted");
    } catch (AggregateException) { Check(requests.Count == 1 && !File.Exists(target), "explicit CDN failure does not switch or leave partial package"); }
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
