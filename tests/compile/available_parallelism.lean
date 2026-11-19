module
import Init.System.Platform

/-! The available-parallelism runtime primitive must work in interpreted and native code. -/
public def main : IO Unit := do
  unless System.Platform.Internal.getAvailableParallelism () > 0 do
    throw <| IO.userError "available parallelism must be positive"
