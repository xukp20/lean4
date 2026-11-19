/-
Copyright (c) 2026 Lean FRO. All rights reserved.
Released under Apache 2.0 license as described in the file LICENSE.
-/
module

prelude
public import Init.Data.UInt.Basic
public import Init.Data.String.Basic

public section

namespace Lean

/-- Scheduling settings for one module elaboration job, separate from semantic options. -/
structure ModuleJobConfig where
  flags : UInt8 := 0
  /-- Profiler output directory; absent means no native profiler override. -/
  profileTrace? : Option String := none
  /-- 0 = macro, 1 = tactic, 2 = defeq. These are non-semantic job settings. -/
  profileLevel : UInt32 := 1
  /-- Minimum retained fine-grained event duration, in microseconds. -/
  profileSlowUs : UInt64 := 200
  deriving Inhabited

/-- Export a provisional public interface after the final command. -/
@[inline] def ModuleJobConfig.eoiEarlyOlean (cfg : ModuleJobConfig) : Bool :=
  cfg.flags &&& 2 != 0

@[inline] def ModuleJobConfig.withEOIEarlyOLean (cfg : ModuleJobConfig) : ModuleJobConfig :=
  { cfg with flags := cfg.flags ||| 2 }

end Lean
