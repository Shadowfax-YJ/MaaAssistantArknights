// <copyright file="MiniGameEntryPolicyTests.cs" company="MaaAssistantArknights">
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

using MaaWpfGui.Constants.Enums;
using MaaWpfGui.Services;
using Xunit;

namespace MaaWpfGui.Tests;

public class MiniGameEntryPolicyTests
{
    [Theory]
    [InlineData(ClientType.Official, true)]
    [InlineData(ClientType.Bilibili, true)]
    [InlineData(ClientType.Txwy, false)]
    [InlineData(ClientType.EN, false)]
    [InlineData(ClientType.JP, false)]
    [InlineData(ClientType.KR, false)]
    public void StableBlackFlowEntryIsAvailableOnlyForSupportedClients(string clientType, bool expectedVisible)
    {
        var entries = MiniGameEntryPolicy.CreateDefaultEntries(clientType, key => $"localized:{key}");
        var blackFlowEntries = entries.Where(entry => entry.Value == MiniGameEntryPolicy.StableBlackFlowTask).ToList();

        Assert.Equal(expectedVisible ? 1 : 0, blackFlowEntries.Count);
        Assert.NotEqual(MiniGameEntryPolicy.StableBlackFlowTask, entries[0].Value);

        if (!expectedVisible)
        {
            return;
        }

        var entry = Assert.Single(blackFlowEntries);
        Assert.Equal("MiniGame@BlackFlow", entry.DisplayKey);
        Assert.Equal("localized:MiniGame@BlackFlow", entry.Display);
        Assert.Equal("MiniGame@BlackFlowTip", entry.TipKey);
    }

    [Theory]
    [InlineData("MiniGame@BlackFlow")]
    [InlineData("MiniGame@BlackFlow@Begin")]
    [InlineData("MiniGame@BlackFlow@ConfirmRefresh")]
    [InlineData(" BlackFlowTemporary ")]
    [InlineData("BlackFlowTemporary@Begin")]
    [InlineData("BlackFlowTemporary@InvestSystem")]
    public void ReservedBlackFlowCacheEntriesAreRejected(string value)
    {
        Assert.False(MiniGameEntryPolicy.CanAddDynamicEntry(value));
    }

    [Theory]
    [InlineData("MiniGame@SecretFront")]
    [InlineData("MiniGame@BlackFlows@Begin")]
    [InlineData("BlackFlowTemporaryBackup@Begin")]
    [InlineData("minigame@blackflow@begin")]
    public void UnrelatedDynamicEntriesRemainAvailable(string value)
    {
        Assert.True(MiniGameEntryPolicy.CanAddDynamicEntry(value));
    }

    [Fact]
    public void RebuildingAfterClientChangeReevaluatesBlackFlowVisibility()
    {
        var officialEntries = MiniGameEntryPolicy.CreateDefaultEntries(ClientType.Official, key => key);
        var globalEntries = MiniGameEntryPolicy.CreateDefaultEntries(ClientType.EN, key => key);
        var bilibiliEntries = MiniGameEntryPolicy.CreateDefaultEntries(ClientType.Bilibili, key => key);

        Assert.Contains(officialEntries, entry => entry.Value == MiniGameEntryPolicy.StableBlackFlowTask);
        Assert.DoesNotContain(globalEntries, entry => entry.Value == MiniGameEntryPolicy.StableBlackFlowTask);
        Assert.Contains(bilibiliEntries, entry => entry.Value == MiniGameEntryPolicy.StableBlackFlowTask);
    }
}
