module

prelude
public import Init.Data.Queue
public import Std.Sync.Mutex
import Init.System.Promise

namespace Lake

private structure CompilerQueue where
  running : Nat := 0
  waiters : Std.Queue (IO.Promise Unit) := ∅

/-- A per-build limit on active Lean compiler processes. -/
public structure CompilerBudget where
  private mk ::
  threads : Nat
  private maxProcs : Nat
  private state : Std.Mutex CompilerQueue

public def CompilerBudget.new (threads maxProcs : Nat) : BaseIO CompilerBudget := do
  return ⟨threads, maxProcs, ← Std.Mutex.new {}⟩

/-- Register a compiler and return its permission task without waiting for a slot. -/
public def CompilerBudget.acquire (budget : CompilerBudget) : BaseIO (Task (Option Unit)) := do
  let permit ← IO.Promise.new
  let granted ← budget.state.atomically do
    let state ← get
    if state.running < budget.maxProcs then
      set {state with running := state.running + 1}
      return true
    else
      set {state with waiters := state.waiters.enqueue permit}
      return false
  if granted then
    permit.resolve ()
  return permit.result?

/-- Hand the slot to the earliest waiter, or make it available to future callers. -/
public def CompilerBudget.release (budget : CompilerBudget) : BaseIO Unit := do
  let next ← budget.state.atomically do
    let state ← get
    if let some (permit, waiters) := state.waiters.dequeue? then
      set {state with waiters}
      return some permit
    else
      set {state with running := state.running - 1}
      return none
  if let some permit := next then
    permit.resolve ()

end Lake
