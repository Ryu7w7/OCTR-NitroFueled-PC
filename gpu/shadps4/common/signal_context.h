// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/types.h"

namespace Common {

void* GetXmmPointer(void* ctx, u8 index);

void* GetRip(void* ctx);

/// bbport: stack and frame pointers at the fault (fault-site statistics).
u64 GetRsp(void* ctx);
u64 GetRbp(void* ctx);

void IncrementRip(void* ctx, u64 length);

bool IsWriteError(void* ctx);

} // namespace Common