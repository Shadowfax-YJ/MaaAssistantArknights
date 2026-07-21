// <copyright file="MiniGameEntryPolicy.cs" company="MaaAssistantArknights">
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
using MaaWpfGui.Constants.Enums;
using MaaWpfGui.Models;

namespace MaaWpfGui.Services;

internal static class MiniGameEntryPolicy
{
    internal const string StableBlackFlowTask = "MiniGame@BlackFlow@Begin";

    internal static List<MiniGameEntry> CreateDefaultEntries(string clientType, Func<string, string> localize)
    {
        ArgumentNullException.ThrowIfNull(localize);

        var entries = new List<MiniGameEntry>
        {
            new() { Display = localize("MiniGameNameSsStore"), DisplayKey = "MiniGameNameSsStore", Value = "SS@Store@Begin", TipKey = "MiniGameNameSsStoreTip" },
            new() { Display = localize("MiniGameNameGreenTicketStore"), DisplayKey = "MiniGameNameGreenTicketStore", Value = "GreenTicket@Store@Begin", TipKey = "MiniGameNameGreenTicketStoreTip" },
            new() { Display = localize("MiniGameNameYellowTicketStore"), DisplayKey = "MiniGameNameYellowTicketStore", Value = "YellowTicket@Store@Begin", TipKey = "MiniGameNameYellowTicketStoreTip" },
            new() { Display = localize("MiniGameNameRAStore"), DisplayKey = "MiniGameNameRAStore", Value = "RA@Store@Begin", TipKey = "MiniGameNameRAStoreTip" },
            new() { Display = localize("MiniGame@SecretFront"), DisplayKey = "MiniGame@SecretFront", Value = "MiniGame@SecretFront", TipKey = "MiniGame@SecretFrontTip" },
        };

        if (clientType is ClientType.Official or ClientType.Bilibili)
        {
            entries.Add(new MiniGameEntry {
                Display = localize("MiniGame@BlackFlow"),
                DisplayKey = "MiniGame@BlackFlow",
                Value = StableBlackFlowTask,
                TipKey = "MiniGame@BlackFlowTip",
            });
        }

        return entries;
    }

    internal static bool CanAddDynamicEntry(string? taskName)
    {
        var normalized = taskName?.Trim() ?? string.Empty;
        return !string.Equals(normalized, "MiniGame@BlackFlow", StringComparison.Ordinal)
            && !normalized.StartsWith("MiniGame@BlackFlow@", StringComparison.Ordinal)
            && !string.Equals(normalized, "BlackFlowTemporary", StringComparison.Ordinal)
            && !normalized.StartsWith("BlackFlowTemporary@", StringComparison.Ordinal);
    }
}
