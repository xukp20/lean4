/-
Copyright (c) 2026 Lean FRO. All rights reserved.
Released under Apache 2.0 license as described in the file LICENSE.
-/
module

prelude
public import Init.Data.UInt.Basic

public section

namespace Lean

/-- Scheduling settings for one module elaboration job, separate from semantic options. -/
structure ModuleJobConfig where
  flags : UInt8 := 0
  deriving Inhabited

/-- Check inductive signatures before ordered complete kernel checking. -/
@[inline] def ModuleJobConfig.inductiveUnblock (cfg : ModuleJobConfig) : Bool :=
  cfg.flags &&& 1 != 0

@[inline] def ModuleJobConfig.withInductiveUnblock (cfg : ModuleJobConfig) : ModuleJobConfig :=
  { flags := cfg.flags ||| 1 }

/-- Export a provisional public interface after the final command. -/
@[inline] def ModuleJobConfig.eoiEarlyOlean (cfg : ModuleJobConfig) : Bool :=
  cfg.flags &&& 2 != 0

@[inline] def ModuleJobConfig.withEOIEarlyOLean (cfg : ModuleJobConfig) : ModuleJobConfig :=
  { flags := cfg.flags ||| 2 }

end Lean
