// <copyright file="BlackFlowUpdate.cs" company="MaaAssistantArknights">
// Part of the MaaWpfGui project, maintained by the MaaAssistantArknights team (Maa Team)
// Copyright (C) 2021-2025 MaaAssistantArknights Contributors
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Affero General Public License v3.0 only as published by
// the Free Software Foundation, either version 3 of the License, or
// any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY
// </copyright>

#nullable enable
using System;
using System.Collections.Generic;
using System.IO;
using System.IO.Compression;
using System.Security.Cryptography;
using System.Text.Json;
using System.Text.RegularExpressions;
using System.Threading.Tasks;

namespace MaaWpfGui.Services;

/// <summary>Contract and package validation for the separate BlackFlow release channel.</summary>
internal static class BlackFlowUpdate
{
    public const string Channel = "blackflow-data-collection";
    public const string MetadataFile = "blackflow-update.json";
    public const string CdnRoot = "https://img.lubiao.wiki/maa/blackflow";
    public const string GitHubRoot = "https://github.com/Shadowfax-YJ/MaaAssistantArknights/releases/download";
    public const string FeedUrl = CdnRoot + "/latest.json";

#if BLACKFLOW_DATA_COLLECTION
    public static bool IsEnabled => true;
#else
    public static bool IsEnabled => false;
#endif

    public sealed record Asset(string Name, Uri Url, long Size, string Sha256);

    public sealed record Release(string Version, Uri ReleaseUrl, string Notes, Asset Package);

    public static Uri[] SourceUrls(string cdn, string github, string source = "Auto") => source switch {
        "CDN" => [new(cdn)],
        "GitHub" => [new(github)],
        _ => [new(cdn), new(github)],
    };

    public static async Task<Release> CheckAsync(Func<Uri, Task<string?>> fetch, string architecture, string source = "Auto")
    {
        var errors = new List<Exception>();
        foreach (var url in SourceUrls(FeedUrl, GitHubRoot + "/blackflow-updates/latest.json", source))
        {
            try
            {
                return ParseRelease(await fetch(url) ?? throw new IOException("Empty update response"), architecture);
            }
            catch (Exception ex)
            {
                errors.Add(ex);
            }
        }

        throw new AggregateException("BlackFlow update sources are unavailable", errors);
    }

    public static async Task DownloadAsync(Release release, string path, Func<Uri, string, Task<bool>> download, string source = "Auto")
    {
        var errors = new List<Exception>();
        foreach (var url in SourceUrls(
                     $"{CdnRoot}/{release.Version}/{release.Package.Name}",
                     $"{GitHubRoot}/blackflow-{release.Version}/{release.Package.Name}", source))
        {
            try
            {
                if (File.Exists(path))
                {
                    File.Delete(path);
                }

                if (!await download(url, path))
                {
                    throw new IOException("Package download failed: " + url.Host);
                }

                await VerifyFileAsync(path, release.Package);
                ValidatePackage(path, release.Version);
                return;
            }
            catch (Exception ex)
            {
                errors.Add(ex);
            }
        }

        if (File.Exists(path))
        {
            File.Delete(path);
        }

        throw new AggregateException("No source supplied a valid BlackFlow package", errors);
    }

    public static Version VersionNumber(string version)
    {
        string value = version.Split('+')[0];
        if (!Regex.IsMatch(value, @"^v?(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)$", RegexOptions.CultureInvariant))
        {
            throw new InvalidDataException("Invalid BlackFlow version: " + version);
        }

        return Version.Parse(value.TrimStart('v'));
    }

    public static Release ParseRelease(string json, string architecture)
    {
        using var document = JsonDocument.Parse(json);
        var root = document.RootElement;
        if (root.GetProperty("schema_version").GetInt32() != 1 || root.GetProperty("channel").GetString() != Channel)
        {
            throw new InvalidDataException("This feed is not a BlackFlow release feed.");
        }

        string version = root.GetProperty("version").GetString() ?? string.Empty;
        _ = VersionNumber(version);
        if (architecture is not ("x64" or "arm64"))
        {
            throw new InvalidDataException("Unsupported Windows architecture.");
        }

        var entry = root.GetProperty("assets").GetProperty("win-" + architecture);
        string expectedName = $"MAA-BlackFlow-Data-Collection-{version}-win-{architecture}.zip";
        string name = entry.GetProperty("name").GetString() ?? string.Empty;
        string sha256 = entry.GetProperty("sha256").GetString() ?? string.Empty;
        long size = entry.GetProperty("size").GetInt64();
        if (name != expectedName || size <= 0 || !Regex.IsMatch(sha256, "^[0-9a-fA-F]{64}$", RegexOptions.CultureInvariant))
        {
            throw new InvalidDataException("Invalid BlackFlow package metadata.");
        }

        return new Release(
            version,
            HttpsUrl(root.GetProperty("release_url").GetString()),
            root.GetProperty("release_notes").GetString() ?? string.Empty,
            new Asset(name, HttpsUrl(entry.GetProperty("url").GetString()), size, sha256));
    }

    public static async Task VerifyFileAsync(string path, Asset asset)
    {
        await using var stream = File.OpenRead(path);
        if (stream.Length != asset.Size)
        {
            throw new InvalidDataException("BlackFlow update package size mismatch.");
        }

        string actual = Convert.ToHexString(await SHA256.HashDataAsync(stream));
        if (!actual.Equals(asset.Sha256, StringComparison.OrdinalIgnoreCase))
        {
            throw new InvalidDataException("BlackFlow update package SHA256 mismatch.");
        }
    }

    public static void ValidatePackage(string path, string? expectedVersion = null)
    {
        using var zip = ZipFile.OpenRead(path);
        var names = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        foreach (var entry in zip.Entries)
        {
            string name = entry.FullName.Replace('\\', '/').TrimEnd('/');
            if (name.Length == 0 || name.StartsWith('/') || name.Contains(':') ||
                Array.Exists(name.Split('/'), part => part is ".." or "." or "" || part.EndsWith('.') || part.EndsWith(' ')) || !names.Add(name))
            {
                throw new InvalidDataException("Unsafe or duplicate update archive path.");
            }

            if (((entry.ExternalAttributes >> 16) & 0xF000) == 0xA000)
            {
                throw new InvalidDataException("Update archives cannot contain symbolic links.");
            }
        }

        foreach (string required in new[] { "MAA.exe", "MaaCore.dll", "MAA.Updater.exe", "filelist.txt", "resource/blackflow-gui-defaults.json", MetadataFile })
        {
            if (zip.GetEntry(required) is not { Length: > 0 })
            {
                throw new InvalidDataException("Incomplete BlackFlow update package: " + required);
            }
        }

        // This channel distributes complete, tested program/resource pairs, never upstream OTA payloads.
        if (names.Contains("changes.json") || names.Contains("removelist.txt"))
        {
            throw new InvalidDataException("BlackFlow requires a full package.");
        }

        var metadata = zip.GetEntry(MetadataFile) ?? throw new InvalidDataException("Missing package identity.");
        if (metadata.Length > 16384)
        {
            throw new InvalidDataException("Invalid package identity.");
        }

        using var data = metadata.Open();
        using var document = JsonDocument.Parse(data);
        var root = document.RootElement;
        if (root.GetProperty("channel").GetString() != Channel || root.GetProperty("schema_version").GetInt32() != 1)
        {
            throw new InvalidDataException("Package belongs to a different update channel.");
        }

        string version = root.GetProperty("version").GetString() ?? string.Empty;
        _ = VersionNumber(version);
        if (expectedVersion != null && VersionNumber(version) != VersionNumber(expectedVersion))
        {
            throw new InvalidDataException("Package and release versions do not match.");
        }
    }

    private static Uri HttpsUrl(string? value)
    {
        if (!Uri.TryCreate(value, UriKind.Absolute, out var uri) || uri.Scheme != Uri.UriSchemeHttps || uri.UserInfo.Length != 0)
        {
            throw new InvalidDataException("Release URLs must use HTTPS.");
        }

        return uri;
    }
}
