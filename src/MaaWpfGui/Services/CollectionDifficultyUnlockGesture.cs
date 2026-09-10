// <copyright file="CollectionDifficultyUnlockGesture.cs" company="MaaAssistantArknights">
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

namespace MaaWpfGui.Services;

internal sealed class CollectionDifficultyUnlockGesture
{
    private int _clicks;
    private long _lastClick;

    public bool RegisterClick(long timestampMilliseconds)
    {
        var gap = timestampMilliseconds - _lastClick;
        _clicks = _clicks > 0 && gap >= 0 && gap <= 1000 ? _clicks + 1 : 1;
        _lastClick = timestampMilliseconds;
        if (_clicks < 5)
        {
            return false;
        }

        Reset();
        return true;
    }

    public void Reset() => _clicks = 0;
}
