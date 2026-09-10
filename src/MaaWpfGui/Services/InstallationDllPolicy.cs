// <copyright file="InstallationDllPolicy.cs" company="MaaAssistantArknights">
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

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;

namespace MaaWpfGui.Services;

internal static class InstallationDllPolicy
{
    internal static List<string> FindUnknown(IEnumerable<string> fileNames, bool collectionRuntime)
    {
        var allowed = new HashSet<string>(StringComparer.OrdinalIgnoreCase)
        {
            "hostfxr.dll", "hostpolicy.dll", "libloader.dll", "DirectML.dll",
            "fastdeploy_ppocr.dll", "MaaCore.dll", "onnxruntime_maa.dll", "opencv_world4_maa.dll",
        };

        // The collection publisher disables the hostfxr patch, so these .NET/WPF
        // bootstrap dependencies must remain beside MAA.exe instead of externals.
        if (collectionRuntime)
        {
            allowed.UnionWith([
                "clrjit.dll", "coreclr.dll", "PresentationFramework.dll", "System.Collections.dll",
                "System.IO.FileSystem.dll", "System.IO.Packaging.dll", "System.Memory.dll",
                "System.Private.CoreLib.dll", "System.Runtime.dll", "System.Runtime.Extensions.dll",
                "System.Runtime.InteropServices.dll", "System.Runtime.InteropServices.RuntimeInformation.dll",
                "System.Runtime.Loader.dll", "System.Xaml.dll", "WindowsBase.dll",
            ]);
        }
        return [.. fileNames.Select(file => Path.GetFileName(file)!).Where(name =>
            !allowed.Contains(name) && !name.Contains("maa", StringComparison.OrdinalIgnoreCase))];
    }
}
