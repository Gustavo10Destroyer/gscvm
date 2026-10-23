# T6 opcode coverage

The T6 opcode table in `upstream/src/arc/engine/t6_code.cpp` defines 125
opcodes. Every one has an explicit handler in `src/gsc/t6/vm.cpp`; nothing falls
through to a silent default. This page records the behavioural choices that are
not directly encoded in the bytecode.

## Stack and call conventions

- GSC evaluates call arguments right-to-left. The first source argument is on
  top of the stack when an import opcode executes.
- `OP_PreScriptCall` / `OP_VoidCodePos` push a marker. Calls pop their arguments,
  then the marker, then (for methods) the receiver, then the pointer for pointer
  calls.
- Frame locals are stored in declaration order. Bytecode local index `i` maps to
  `locals[size - 1 - i]`, matching the compiler's `variable_access()`.
- `OP_SafeCreateLocalVariables` binds the declared parameter names and appends
  the remaining locals. Extra call arguments are available to a `vararg` local.
- `OP_ClearParams` discards the temporary call/return stack. `OP_SafeDecTop`
  tolerates an empty stack; `OP_DecTop` does not.
- `JumpOnFalseExpr` / `JumpOnTrueExpr` leave the tested value on the stack when
  the branch is taken. Plain `JumpOnFalse` / `JumpOnTrue` consume it.
- `OP_Switch` jumps to its `OP_EndSwitch` table. The loader parses the table once
  and resolves every case/default target; a failed match falls through to the
  instruction after the table.
- `waittill` receives event/object on top of the stack. `notify` pushes a
  `VoidCodePos` marker, arguments (reverse), event and object. Delivered wait
  arguments are pushed in reverse order so the first
  `SafeSetWaittillVariableFieldCached` consumes argument 0.

## Deliberate approximations

These are the places where the shipped engine cannot be observed from the
compiler/decompiler alone. They are deterministic and documented rather than
hidden.

- **Array iteration order** is insertion order. The original engine's arrays are
  hash-ordered.
- **String/equality coercion** compares integer and float numerically and
  compares string/istring/hash by their text. Strict comparisons
  (`SuperEqual`) require matching value types.
- **Angles** use the standard pitch/yaw/roll formulas for
  `AnglesToForward`/`Right`/`Up` and `VectorToAngles`; they are not guaranteed to
  be bit-identical to the engine.
- **`waittillmatch`** matches the event and requires at least the compiled
  argument count. The actual match patterns are not compared.
- **Dvar colours** read the component from the host's vector dvar; alpha is 1.0.
- **`Object` / `ThreadObject`** are best-effort: the former yields `self`, the
  latter the current thread handle. The T6 compiler does not normally emit them.
- **Timing** uses one logical clock. `Wait` takes a duration in seconds and
  supports fractional values; `wait 0` is a frame/tick yield. Real-time mode
  resumes at the exact deadline, while tick-based mode quantizes resumption to
  `1 / tick_rate` boundaries. `RealWait` currently advances the same clock as
  `Wait`; the distinction is host-visible only through `tick()`.
- **Developer blocks** are skipped by default (`execute_devblocks = false`), and
  imports carrying the developer flag evaluate to undefined in that mode.

## Unsupported opcodes

Three T6 table entries are rejected with a descriptive error:

| Opcode | Reason |
| --- | --- |
| `OP_SafeSetVariableFieldCached` | Legacy operand-less store; the T6 compiler never emits it and its stack effect is ambiguous in the available evidence. |
| `OP_EvalLocalVariable` | Reserved name-based local read; not emitted by the T6 compiler. |
| `OP_EvalLocalVariableRef` | Reserved name-based local reference; not emitted by the T6 compiler. |

## Complete table

Status meanings: `implemented` has direct VM semantics; `limited` works but has a
documented approximation; `no-op` is intentionally inert; `unsupported` raises
a `RuntimeError`.

| Opcode | Status | Notes |
| --- | --- | --- |
| `OP_End` | implemented |  |
| `OP_Return` | implemented |  |
| `OP_GetUndefined` | implemented |  |
| `OP_GetZero` | implemented |  |
| `OP_GetByte` | implemented |  |
| `OP_GetNegByte` | implemented |  |
| `OP_GetUnsignedShort` | implemented |  |
| `OP_GetNegUnsignedShort` | implemented |  |
| `OP_GetInteger` | implemented | two operands decode as an animtree reference |
| `OP_GetFloat` | implemented |  |
| `OP_GetString` | implemented |  |
| `OP_GetIString` | implemented |  |
| `OP_GetVector` | implemented |  |
| `OP_GetLevelObject` | implemented |  |
| `OP_GetAnimObject` | limited | returns the host anim singleton |
| `OP_GetSelf` | implemented |  |
| `OP_GetLevel` | implemented |  |
| `OP_GetGame` | implemented |  |
| `OP_GetAnim` | limited | returns the host anim singleton |
| `OP_GetAnimation` | implemented |  |
| `OP_GetGameRef` | implemented |  |
| `OP_GetFunction` | implemented |  |
| `OP_CreateLocalVariable` | implemented |  |
| `OP_SafeCreateLocalVariables` | implemented |  |
| `OP_RemoveLocalVariables` | implemented |  |
| `OP_EvalLocalVariableCached` | implemented |  |
| `OP_EvalArray` | implemented |  |
| `OP_EvalLocalArrayRefCached` | implemented |  |
| `OP_EvalArrayRef` | implemented |  |
| `OP_ClearArray` | implemented |  |
| `OP_EmptyArray` | implemented |  |
| `OP_GetSelfObject` | implemented |  |
| `OP_EvalFieldVariable` | implemented |  |
| `OP_EvalFieldVariableRef` | implemented |  |
| `OP_ClearFieldVariable` | implemented |  |
| `OP_SafeSetVariableFieldCached` | unsupported | error: legacy operand-less store, never emitted by the T6 compiler |
| `OP_SafeSetWaittillVariableFieldCached` | implemented |  |
| `OP_ClearParams` | implemented |  |
| `OP_CheckClearParams` | no-op | no-op |
| `OP_EvalLocalVariableRefCached` | implemented |  |
| `OP_SetVariableField` | implemented |  |
| `OP_CallBuiltin` | implemented |  |
| `OP_CallBuiltinMethod` | implemented |  |
| `OP_Wait` | implemented |  |
| `OP_WaitTillFrameEnd` | implemented |  |
| `OP_PreScriptCall` | implemented |  |
| `OP_ScriptFunctionCall` | implemented |  |
| `OP_ScriptFunctionCallPointer` | implemented |  |
| `OP_ScriptMethodCall` | implemented |  |
| `OP_ScriptMethodCallPointer` | implemented |  |
| `OP_ScriptThreadCall` | implemented |  |
| `OP_ScriptThreadCallPointer` | implemented |  |
| `OP_ScriptMethodThreadCall` | implemented |  |
| `OP_ScriptMethodThreadCallPointer` | implemented |  |
| `OP_DecTop` | implemented |  |
| `OP_CastFieldObject` | implemented |  |
| `OP_CastBool` | implemented |  |
| `OP_BoolNot` | implemented |  |
| `OP_BoolComplement` | implemented |  |
| `OP_JumpOnFalse` | implemented |  |
| `OP_JumpOnTrue` | implemented |  |
| `OP_JumpOnFalseExpr` | implemented |  |
| `OP_JumpOnTrueExpr` | implemented |  |
| `OP_Jump` | implemented |  |
| `OP_JumpBack` | implemented |  |
| `OP_Inc` | implemented |  |
| `OP_Dec` | implemented |  |
| `OP_Bit_Or` | implemented |  |
| `OP_Bit_Xor` | implemented |  |
| `OP_Bit_And` | implemented |  |
| `OP_Equal` | implemented |  |
| `OP_NotEqual` | implemented |  |
| `OP_LessThan` | implemented |  |
| `OP_GreaterThan` | implemented |  |
| `OP_LessThanOrEqualTo` | implemented |  |
| `OP_GreaterThanOrEqualTo` | implemented |  |
| `OP_ShiftLeft` | implemented |  |
| `OP_ShiftRight` | implemented |  |
| `OP_Plus` | implemented |  |
| `OP_Minus` | implemented |  |
| `OP_Multiply` | implemented |  |
| `OP_Divide` | implemented |  |
| `OP_Modulus` | implemented |  |
| `OP_SizeOf` | implemented |  |
| `OP_WaitTillMatch` | limited | limited: event is matched and the argument count is enforced; match patterns are not compared |
| `OP_WaitTill` | implemented |  |
| `OP_Notify` | implemented |  |
| `OP_EndOn` | implemented |  |
| `OP_VoidCodePos` | implemented |  |
| `OP_Switch` | implemented |  |
| `OP_EndSwitch` | no-op | no-op table terminator |
| `OP_Vector` | implemented |  |
| `OP_GetHash` | implemented |  |
| `OP_RealWait` | implemented |  |
| `OP_VectorConstant` | implemented |  |
| `OP_IsDefined` | implemented |  |
| `OP_VectorScale` | implemented |  |
| `OP_AnglesToUp` | implemented |  |
| `OP_AnglesToRight` | implemented |  |
| `OP_AnglesToForward` | implemented |  |
| `OP_AngleClamp180` | implemented |  |
| `OP_VectorToAngles` | implemented |  |
| `OP_Abs` | implemented |  |
| `OP_GetTime` | implemented |  |
| `OP_GetDvar` | implemented |  |
| `OP_GetDvarInt` | implemented |  |
| `OP_GetDvarFloat` | implemented |  |
| `OP_GetDvarVector` | implemented |  |
| `OP_GetDvarColorRed` | limited | host dvar vector x component |
| `OP_GetDvarColorGreen` | limited | host dvar vector y component |
| `OP_GetDvarColorBlue` | limited | host dvar vector z component |
| `OP_GetDvarColorAlpha` | limited | always 1.0 (hosts do not expose a fourth component) |
| `OP_FirstArrayKey` | implemented |  |
| `OP_NextArrayKey` | implemented |  |
| `OP_ProfileStart` | no-op | no-op profiling marker |
| `OP_ProfileStop` | no-op | no-op profiling marker |
| `OP_SafeDecTop` | implemented |  |
| `OP_Nop` | no-op | no-op |
| `OP_Abort` | implemented |  |
| `OP_Object` | limited | approximation: pushes self |
| `OP_ThreadObject` | limited | approximation: pushes the current thread id |
| `OP_EvalLocalVariable` | unsupported | error: reserved/name-based form, never emitted by the T6 compiler |
| `OP_EvalLocalVariableRef` | unsupported | error: reserved/name-based form, never emitted by the T6 compiler |
| `OP_DevblockBegin` | implemented |  |
| `OP_DevblockEnd` | no-op | no-op |
