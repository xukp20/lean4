/*
Copyright (c) 2026 Xukangping. All rights reserved.
Released under Apache 2.0 license as described in the file LICENSE.
*/
#pragma once

namespace lean {
// Best-effort default worker count, always positive. Explicit settings take precedence.
unsigned available_parallelism();
}
