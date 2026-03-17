# PL-PICO Language Reference Card
## RP2040 / Cortex-M0+ Dialect

This document defines **PL-PICO**, a variant of PL-11 adapted for the Raspberry Pi Pico
(RP2040 microcontroller, ARM Cortex-M0+, 32-bit). PL-PICO extends PL-11 with 32-bit-native
types, unsigned variants, hardware-access primitives, OS-programming constructs (VOLATILE,
INTERRUPT, BARRIER), aggregate types (RECORD), and procedure pointers.

Everything that exists in PL-11 is preserved unless a section explicitly states otherwise.

---

## 1. Character Set, Comments, Case Rules

- Keywords are **case-insensitive**: `BEGIN`, `begin`, and `Begin` are identical.
- Identifiers are normalized to uppercase internally.
- String literals are **single-quoted**: `'hello\n'`. Double-quote strings are not valid.
- Escape sequences inside strings: `\n` `\t` `\\` (C conventions).

Two comment forms:

```
% This is a line comment — extends to end of line

COMMENT This is a keyword comment — extends to the next semicolon;

COMMENT
    Multi-line keyword comment.
    The semicolon below ends the comment.
;
```

The terminating `;` of a `COMMENT` is consumed by the comment and is not a statement separator.

---

## 2. Program Structure

A PL-PICO program is a **block**: declarations followed by statements, wrapped in `BEGIN`/`END`.

```
BEGIN
    % declarations first
    INT32 I, J;

    % then executable statements
    42 => I;
    I + 1 => J
END
```

Blocks can be nested anywhere a statement is expected. Variables declared in a block are local
to that block and inaccessible outside it. Declarations must precede all executable statements
within the same block.

Statements are **semicolon-separated**. The last statement before `END` does not require a
trailing semicolon, but one is permitted.

---

## 3. Data Types

### 3.1 Integer types

| Type keyword | Width | Signed | Value range |
|---|---|---|---|
| `INT8` | 8 bits | yes | −128 … 127 |
| `INT16` | 16 bits | yes | −32 768 … 32 767 |
| `INT32` | 32 bits | yes | −2 147 483 648 … 2 147 483 647 |
| `INT64` | 64 bits | yes | −9 223 372 036 854 775 808 … 9 223 372 036 854 775 807 |
| `UINT8` | 8 bits | no | 0 … 255 |
| `UINT16` | 16 bits | no | 0 … 65 535 |
| `UINT32` | 32 bits | no | 0 … 4 294 967 295 |
| `UINT64` | 64 bits | no | 0 … 18 446 744 073 709 551 615 |
| `INTEGER` | 32 bits | yes | Alias for `INT32` |

**`INTEGER` (unqualified) is `INT32`** — the 32-bit native word of the RP2040.

**PL-11 mapping:**

| PL-11 type | PL-PICO replacement | Notes |
|---|---|---|
| `BYTE` | `INT8` | 8-bit signed |
| `WORD` / `INTEGER` | `INT32` | **was 16-bit in PL-11; now 32-bit on RP2040** |
| `LONG` | `INT32` | Same native size as INT32; `LONG` is accepted as an alias |

**INT64** is supported in source but computed in software on the M0+ (no 64-bit hardware
multiply/divide). Use only where necessary.

Machine addresses must be held in `UINT32`; all pointer arithmetic uses `UINT32` values.

### 3.2 REAL (floating-point)

| Type keyword | Alias | Width | Notes |
|---|---|---|---|
| `REAL` | `FLOAT` | 32-bit | **Software float — no FPU on Cortex-M0+** |
| `REAL*8` | `FLOAT*8` | 64-bit | Software double |

Literals: `3.14`, `2.0E-3`, `-1.5E10`.

All floating-point operations are handled by the compiler-supplied soft-float library. They
are significantly slower than integer operations — avoid in interrupt service routines.

### 3.3 CHARACTER (strings)

```
CHARACTER*20 NAME;      % fixed-length string, 20 characters
CHARACTER*1  CH;
```

String literals: `'HELLO'`, `'A'`. Always single-quoted. Length fixed at declaration.
Assignment pads with spaces (source shorter) or truncates (source longer).

### 3.4 BIT (bit strings)

```
BIT*32 FLAGS;           % 32-bit flag word
BIT*8  MASK;
```

Bit literals: `'10100000000000000000000000000011'B` (binary), `#FF` (hexadecimal),
`0xFF` (C-style hex — see §4).

All bitwise operators apply: `AND`, `OR`, `NOT`, `XOR`, `SHL`, `SHR`, `SHRA`.

### 3.5 No separate pointer type

Addresses are held in `UINT32` variables. `REF`, `IND`, `IND8`, `IND16`, and `MMIO`
(§12) provide address-of and dereference operations. On the RP2040, all addresses are
32-bit and fit exactly in `UINT32`.

---

## 4. Literal Formats

| Form | Example | Notes |
|---|---|---|
| Decimal | `42`, `-7`, `0` | |
| Octal | `0177` | Leading `0` |
| Binary / bit string | `'1010'B` | |
| Hexadecimal (PL-11 style) | `#FF`, `#DEADBEEF` | `#` prefix |
| Hexadecimal (C style) | `0xFF`, `0xDEADBEEF` | `0x` prefix — **new in PL-PICO** |
| Real | `3.14`, `2.0E-3`, `-1.5E10` | |
| String | `'HELLO'` | Single-quoted only |

Both `#` and `0x` hex forms are interchangeable. The `0x` form is preferred in OS code
to match C conventions and hardware register tables.

---

## 5. Variable and Constant Declarations

### 5.1 Simple variables

```
INT32   I, J, K;
UINT32  ADDR, FLAGS;
INT16   HALF;
INT8    CH;
UINT8   BYTE_VAL;
REAL    X, Y;
```

Multiple names of the same type on one line, comma-separated.

### 5.2 VOLATILE qualifier

The `VOLATILE` qualifier marks a variable as potentially modified by hardware or by code
on the other core. The compiler must not cache the value in a register across accesses.

```
VOLATILE UINT32 TICK_COUNT;
VOLATILE INT32  SHARED_FLAG;
```

Use `VOLATILE` for:
- Variables shared between Core 0 and Core 1
- Variables modified inside an `INTERRUPT PROCEDURE`
- Memory-mapped I/O register aliases (prefer `MMIO()` instead — see §12)

### 5.3 ALIGN qualifier

Forces a variable or RECORD to be placed at an n-byte boundary in memory. Required for
DMA buffers and structures accessed by hardware engines.

```
ALIGN(4)  UINT32 DMA_BUFFER;
ALIGN(16) UINT32 ALIGNED_BLOCK;
```

`ALIGN(n)` must appear before the type keyword. `n` must be a power of two.

### 5.4 Named constants

```
CONSTANT INT32  MAX_THREADS  = 16;
CONSTANT UINT32 STACK_CANARY = 0xDEADBEEF;
CONSTANT REAL   PI           = 3.14159265;
```

Constants are substituted at compile time and cannot be assigned to.

### 5.5 Arrays — two equivalent syntaxes

**UNH syntax** (preferred):
```
INT32   50    ARRAY SCORES;        % 1D array of 50 INT32 values
INT32   3, 4  ARRAY MATRIX;        % 3×4 array
UINT8   512   ARRAY BUFFER;
UINT32  4     ARRAY ROW, COL;      % two separate 4-element arrays
```

**Traditional syntax**:
```
INT32   SCORES(50);
INT32   MATRIX(3, 4);
CHARACTER*8 NAMES(25);
```

Indexing is always **1-based**; the first element is index 1.
```
SCORES(1)           % first element
MATRIX(I, J)        % row I, column J
```

Multi-dimensional arrays are stored row-major. Array dimensions must be integer literals.

---

## 6. Assignment

Assignment is a **statement**, not an expression. Syntax places the **value on the left**
and the **target on the right**, separated by `=>`:

```
value_expression => target
```

```
42            => X;           % literal to variable
A + B         => SUM;         % expression to variable
X             => VALS(I);     % variable to array element
R0            => R1;          % register to register
-1            => R0;          % negative literal to register
0xDEADBEEF    => STACK_CANARY;% hex literal to variable
REC.FIELD + 1 => REC.FIELD;  % RECORD field (§18)
```

### 6.1 Compound modification (shorthand assignment)

When a statement has the form `variable OP expression ;`, it is equivalent to
`variable OP expression => variable ;`.

```
R0 + 1;                 % same as: R0 + 1 => R0
COUNT - 1;              % same as: COUNT - 1 => COUNT
FLAGS AND #FE;          % bitwise AND into FLAGS
FLAGS OR 0x80;          % bitwise OR using 0x literal
```

**Restriction:** the left-hand identifier must be a simple scalar variable, a register, or
a RECORD field. Array elements require the explicit `=>` form.

---

## 7. Operators and Precedence

### 7.1 Arithmetic

| Operator | Meaning | Types |
|---|---|---|
| `+` | Addition | INT*, UINT*, REAL |
| `-` | Subtraction | INT*, UINT*, REAL |
| `*` | Multiplication | INT*, UINT*, REAL |
| `/` | Division (truncates toward zero for integers) | INT*, UINT*, REAL |
| `MOD` | Modulus (remainder, sign of dividend) | INT*, UINT* |
| `-` (unary) | Negation | INT*, REAL |

### 7.2 Relational

All produce a numeric result: non-zero = true, zero = false. No separate boolean type.

| Operator | Meaning |
|---|---|
| `=` | Equal |
| `/=` | Not equal |
| `<` | Less than |
| `>` | Greater than |
| `<=` | Less than or equal |
| `>=` | Greater than or equal |

### 7.3 Logical and bitwise

| Operator | Meaning | Types |
|---|---|---|
| `AND` | Bitwise AND / logical AND | BIT, INT*, UINT* |
| `OR` | Bitwise OR / logical OR | BIT, INT*, UINT* |
| `NOT` | Bitwise NOT / logical NOT | BIT, INT*, UINT* |
| `XOR` | Exclusive OR | BIT, INT*, UINT* |
| `SHL` | Shift left | BIT, INT*, UINT* |
| `SHR` | Shift right (logical, zero-fill) | BIT, UINT* |
| `SHRA` | Shift right (arithmetic, sign-extend) | INT* |

### 7.4 Precedence (high to low)

| Level | Operators |
|---|---|
| 1 (highest) | unary `-`, `NOT` |
| 2 | `*`, `/`, `MOD`, `AND`, `SHL`, `SHR`, `SHRA` |
| 3 | `+`, `-`, `OR`, `XOR` |
| 4 (lowest) | `=`, `/=`, `<`, `>`, `<=`, `>=` |

Parentheses override precedence: `(A + B) * C`.

---

## 8. Control Flow

### 8.1 IF-THEN-ELSE

```
IF condition THEN statement
IF condition THEN statement ELSE statement
```

Condition: any expression; zero = false, non-zero = true.
Use `BEGIN`…`END` for multi-statement branches.

```
IF X > 0 THEN
    BEGIN
        X * 2 => Y;
        Y - 1 => Z
    END
ELSE
    0 => Z
```

Dangling `ELSE` attaches to the nearest preceding `IF`.

### 8.2 WHILE

```
WHILE condition DO statement
WHILE condition DO statement UNTIL break_condition
```

Pre-test loop. Optional `UNTIL break_condition` is checked after each iteration.

### 8.3 FOR

```
FOR var FROM start [STEP step] TO     end DO statement [UNTIL break_condition]
FOR var FROM start [STEP step] DOWNTO end DO statement [UNTIL break_condition]
FOR var          [STEP step] TO     end DO statement [UNTIL break_condition]
FOR var          [STEP step] DOWNTO end DO statement [UNTIL break_condition]
```

- `FROM start` — initial value; omitted = use current variable value.
- `STEP step` — per-iteration increment; default +1 (TO) or −1 (DOWNTO).
- `TO` / `DOWNTO` — loop continues while `var ≤ end` or `var ≥ end`.
- `UNTIL break_condition` — optional post-body early exit.
- Loop variable must not be modified inside the body.

```
FOR I FROM 1 TO N DO
    SUM + I => SUM;
```

### 8.4 REPEAT-UNTIL

```
REPEAT
    statement ; statement ; ... statement
UNTIL condition
```

Post-test loop: body runs at least once, repeats until `condition` is non-zero.

### 8.5 DO

```
DO ;
DO statement
DO statement UNTIL break_condition
DO ;         UNTIL break_condition
```

General loop form. `DO ;` is an infinite empty loop (idle spin-wait).

### 8.6 CASE

```
CASE expression OF
    constant1 : statement1 ;
    constant2 : statement2 ;
    constantN : statementN
END
```

`expression` must be an integer type. Each `constant` is an integer literal.
If no case matches, behaviour is undefined — guard with an IF when out-of-range is possible.

### 8.7 GOTO and Labels

```
GOTO LABEL1;
...
LABEL1:
    statement
```

Function-scoped. Cannot jump into a nested block from outside.

---

## 9. Procedures and Functions

### 9.1 Declaration

```
PROCEDURE name ;
BEGIN
    declarations
    statements
END ;

PROCEDURE name ( parameter_list ) ;
BEGIN
    declarations
    statements
END ;
```

### 9.2 Parameter modes

| Keyword | Mode | Meaning |
|---|---|---|
| `IN` | Value | Copy in; callee cannot modify caller's variable |
| `OUT` | Result | Callee writes result back to caller on return |
| `IN OUT` | Value-result | Copy in; copy out on return |

`IN` is the default when the mode keyword is omitted.

```
PROCEDURE ADD (IN INT32 A, IN INT32 B, OUT INT32 RESULT);
BEGIN
    A + B => RESULT
END;

PROCEDURE SWAP (IN OUT INT32 X, IN OUT INT32 Y);
BEGIN
    INT32 TEMP;
    X    => TEMP;
    Y    => X;
    TEMP => Y
END;
```

### 9.3 Calling procedures

```
CALL INIT;
CALL ADD(3, 4, RESULT);
CALL SWAP(A, B);
```

### 9.4 Functions (value-returning procedures)

Declare a return type before `PROCEDURE`. Functions are called as expressions (no `CALL`).

```
INT32 PROCEDURE MAX (IN INT32 A, IN INT32 B);
BEGIN
    IF A > B THEN RETURN(A) ELSE RETURN(B)
END;

MAX(X, Y) + 1 => Z;
```

### 9.5 Recursion

```
INT32 PROCEDURE FACTORIAL (IN INT32 N);
BEGIN
    IF N <= 1 THEN RETURN(1)
    ELSE RETURN(N * FACTORIAL(N - 1))
END;
```

### 9.6 FORWARD declarations (mutual recursion)

```
PROCEDURE FORWARD ODD;

INT32 PROCEDURE EVEN (IN INT32 N);
BEGIN
    IF N = 0 THEN RETURN(1) ELSE RETURN(ODD(N-1))
END;

INT32 PROCEDURE ODD (IN INT32 N);
BEGIN
    IF N = 0 THEN RETURN(0) ELSE RETURN(EVEN(N-1))
END;
```

---

## 10. Scope Rules

- **Lexical (static) scoping**: a name refers to the innermost enclosing declaration.
- A declaration in an inner block **shadows** the same name in an outer block.
- Variables declared in the outermost block are **global** and visible everywhere.
- Declarations are mutually visible within the same block (order does not matter).
- No name overloading: two declarations in the same scope cannot share a name.

---

## 11. ARM Cortex-M0+ Register Names

The RP2040 uses the ARM Cortex-M0+ register file. PL-PICO replaces the PDP-11 register
set (R0–R15, R6=SP, R7=PC) with the ARM register set:

| Register | Role | Notes |
|---|---|---|
| `R0` | Argument / result | Also first argument in AAPCS calling convention |
| `R1` | Argument / result | |
| `R2` | Argument | |
| `R3` | Argument | |
| `R4`–`R7` | Callee-saved | Must be preserved across procedure calls |
| `R8`–`R12` | Callee-saved | **M0+ limitation:** only accessible via MOV to R0–R7 for STMIA/LDMIA |
| `SP` / `R13` | Stack pointer | **PSP** in Thread mode; **MSP** in Handler (ISR) mode |
| `LR` / `R14` | Link register | Return address; holds EXC_RETURN code inside ISRs |
| `PC` / `R15` | Program counter | |
| `APSR` | Application status | N, Z, C, V flags |
| `PSP` | Process stack pointer | Thread-mode stack |
| `MSP` | Main stack pointer | Handler/ISR stack |
| `XPSR` | Extended program status | Combines APSR + IPSR + EPSR |

```
R0 + R1 => R2;           % register arithmetic
42      => R4;            % immediate to register
R13     => SAVED_SP;      % read stack pointer
```

**Note:** Registers R8–R12 have restricted instruction access on M0+. They can be read
or written with MOV but cannot be listed directly in STMIA/LDMIA — the compiler must
stage them through R0–R7 via ASM or PUSH/POP.

---

## 12. Address Operators: REF, IND, IND8, IND16, MMIO

### 12.1 REF — address of a variable

```
REF(var)           % produces the UINT32 machine address of var
```

```
UINT32 PTR;
REF(X) => PTR;     % store address of X in PTR
```

### 12.2 IND — dereference a 32-bit word

```
IND(expr)          % dereference address in expr (rvalue and lvalue)
```

```
UINT32 X, PTR;
42        => X;
REF(X)    => PTR;
IND(PTR) + 1 => R0;   % read X through PTR, add 1
99        => IND(PTR); % write 99 to the location PTR points at
```

### 12.3 IND8 — dereference a single byte (8-bit)

```
IND8(addr_expr)    % read/write one byte at address
```

```
UINT32 ADDR;
UINT8  B;
IND8(ADDR)     => B;   % load one byte
0xFF           => IND8(ADDR);   % store one byte
```

### 12.4 IND16 — dereference a 16-bit halfword

```
IND16(addr_expr)   % read/write two bytes (halfword) at address
```

```
IND16(UART_DR) => STATUS;
```

### 12.5 MMIO — volatile hardware register access

`MMIO(addr)` is a **volatile** version of `IND`. It suppresses all compiler reordering
and register caching. Use for any hardware-mapped register.

```
MMIO(addr_expr)    % volatile 32-bit read/write
```

```
CONSTANT UINT32 SIO_CPUID   = 0xD0000000;
CONSTANT UINT32 UART0_DR    = 0x40034000;
CONSTANT UINT32 UART0_FR    = 0x40034018;

% Read the hardware CPUID register
UINT32 CORE_ID;
MMIO(SIO_CPUID) => CORE_ID;

% Poll UART TX FIFO not full (bit 5), then send a character
WHILE MMIO(UART0_FR) AND 0x20 /= 0 DO ;
0x41 => MMIO(UART0_DR);
```

**Rule:** use `MMIO()` for hardware registers; use `IND()` for software pointers. They
are equivalent in semantics but `MMIO()` documents intent and is never optimized away.

---

## 13. PUSH and POP

On the RP2040, each stack word is **4 bytes** (32 bits). SP = R13 = PSP in Thread mode.

```
PUSH expression       % SP := SP - 4 ; mem[SP] := (INT32)expr
POP  variable         % variable := mem[SP] ; SP := SP + 4
```

```
PUSH R4;             % save R4 (4 bytes)
CALL SUBROUTINE;
POP  R4;             % restore R4
```

### 13.1 Multi-register list syntax

The PL-PICO assembler accepts a brace-enclosed register list for PUSH and POP, matching
ARM PUSH/POP instruction encoding:

```
PUSH {R4, R5, R6, R7};      % push four registers (lowest reg at lowest addr)
POP  {R4, R5, R6, R7};      % restore in reverse order
PUSH {R4, LR};               % save R4 and link register
POP  {R4, PC};               % restore R4 and return
```

Register order within `{...}` is ignored; the hardware always stores the lowest-numbered
register at the lowest address.

---

## 14. ASM — Inline Assembly

```
ASM('instruction');
ASM('multi; line; instructions');
```

Bypasses all type checking. Use for Cortex-M0+ operations not expressible in PL-PICO.

```
ASM('cpsid i');          % disable interrupts (PRIMASK=1)
ASM('cpsie i');          % enable interrupts
ASM('wfi');              % wait-for-interrupt (idle)
ASM('dsb');              % data synchronisation barrier
ASM('isb');              % instruction synchronisation barrier
ASM('dmb');              % data memory barrier
ASM('bkpt #0');          % software breakpoint (debugger trap)
ASM('mrs r0, psp');      % read process stack pointer
ASM('msr psp, r0');      % write process stack pointer
```

ARM assembly syntax follows the GNU assembler (`.syntax unified`, `.thumb`).

---

## 15. VOLATILE Qualifier

See §5.2 for declaration syntax. Semantics:

- The compiler must read/write the variable from/to memory on every access.
- No value may be held in a register across a sequence point.
- Reads and writes are not reordered relative to other VOLATILE or MMIO accesses.

```
VOLATILE UINT32 TICK_MS;       % updated by SysTick ISR — never cache
VOLATILE INT32  CORE1_READY;   % inter-core flag
```

`VOLATILE` interacts with `RECORD` — when a record is declared volatile, all fields
inherit the volatile qualifier:

```
VOLATILE RECORD(TCB) CURRENT_TCB;
```

---

## 16. INTERRUPT Procedure Declarations

An `INTERRUPT PROCEDURE` is an ISR (interrupt service routine). It uses the ARM
exception calling convention:

- Entry: CPU has already saved `{R0–R3, R12, LR, PC, XPSR}` onto the active stack.
- Body: executes with MSP (handler mode).
- Exit: generated `BX LR` uses EXC_RETURN code to restore registers and stack.

```
INTERRUPT PROCEDURE name ;
BEGIN
    declarations
    statements
END ;
```

Restrictions:
- May not have parameters or a return value.
- Must not call procedures that block or sleep.
- Any variable read or written by both the ISR and mainline code must be `VOLATILE`.
- Floating-point operations inside an ISR are illegal on M0+ (no FPU state save).

```
VOLATILE UINT32 TICK_COUNT;

INTERRUPT PROCEDURE SYSTICK_HANDLER ;
BEGIN
    TICK_COUNT + 1 => TICK_COUNT
END ;
```

The procedure name must match the vector table entry. For picoOS the relevant
names are:

| Handler name | Exception |
|---|---|
| `PENDSV_HANDLER` | PendSV (context switch) |
| `SYSTICK_HANDLER` | SysTick timer tick |
| `SVCall_HANDLER` | Supervisor call |
| `HARDFAULT_HANDLER` | Hard fault |

---

## 17. BARRIER and DMBARRIER

Memory barriers ensure ordering of memory operations across cores and peripherals.

```
BARRIER              % maps to ARM DSB + ISB (data and instruction sync)
DMBARRIER            % maps to ARM DMB (data memory barrier, multi-master)
```

Use cases:

| Barrier | When to use |
|---|---|
| `BARRIER` | Before/after any flash write; after enabling/disabling an interrupt; after `MSR` writes to system registers |
| `DMBARRIER` | Whenever Core 0 and Core 1 share data without a mutex (e.g., flag protocol); before any DMA transfer |

```
% Publish a value from Core 0 to Core 1
DONE_VALUE => RESULT;
DMBARRIER;               % all writes before this line are visible after it
1 => CORE1_READY;        % Core 1 polls CORE1_READY

% Core 1 consumer side
WHILE CORE1_READY = 0 DO ;
DMBARRIER;               % all reads after this line see the published writes
RESULT => LOCAL_COPY;
```

---

## 18. RECORD Types

`RECORD` declares a named aggregate type (equivalent to a C `struct`). Fields are laid
out contiguously in declaration order with natural alignment.

### 18.1 Record type declaration

```
RECORD TYPE_NAME
    field1 : type1 ;
    field2 : type2 ;
    ...
END ;
```

```
RECORD POINT
    X : INT32 ;
    Y : INT32
END ;
```

### 18.2 Record variable declaration

```
RECORD(TYPE_NAME) var_name;
```

```
RECORD(POINT) P, Q;
```

### 18.3 Field access

```
record_var.field_name
```

```
10 => P.X;
20 => P.Y;
P.X + P.Y => SUM;
P.X => Q.X;
```

### 18.4 Nested RECORD types

```
RECORD RECT
    TOP_LEFT     : RECORD(POINT) ;
    BOTTOM_RIGHT : RECORD(POINT)
END ;

RECORD(RECT) R;
0  => R.TOP_LEFT.X;
0  => R.TOP_LEFT.Y;
80 => R.BOTTOM_RIGHT.X;
24 => R.BOTTOM_RIGHT.Y;
```

### 18.5 VOLATILE RECORD

```
VOLATILE RECORD(POINT) SENSOR_DATA;   % all fields are volatile
```

### 18.6 Layout and offset interop

The picoOS TCB layout (first five fields fixed for `sched_asm.S`):

```
RECORD TCB
    TID        : UINT32 ;   % offset  0
    PID        : UINT32 ;   % offset  4
    STACK_BASE : UINT32 ;   % offset  8  (holds address of lowest stack byte)
    STACK_SIZE : UINT32 ;   % offset 12
    SAVED_SP   : UINT32     % offset 16  ← PendSV reads and writes this
END ;
```

`OFFSET_OF(TCB, SAVED_SP)` must return 16. See §21 for the intrinsics.

---

## 19. Function and Procedure Pointers

### 19.1 PROC_TYPE — named procedure/function signature

```
PROC_TYPE type_name ( param_type_list )
PROC_TYPE type_name ( param_type_list ) RETURNS return_type
```

```
PROC_TYPE THREAD_ENTRY (IN UINT32 ARG) ;
PROC_TYPE COMPARATOR   (IN INT32 A, IN INT32 B) RETURNS INT32 ;
```

### 19.2 PROC_PTR — procedure pointer variable

```
PROC_PTR(PROC_TYPE_NAME) var_name;
```

```
PROC_PTR(THREAD_ENTRY) ENTRY_FN;
PROC_PTR(COMPARATOR)   CMP;
```

### 19.3 Assigning a procedure address

```
REF(SOME_PROCEDURE) => ENTRY_FN;
```

### 19.4 IND_PROC / IND_FUNC — indirect call through pointer

```
CALL IND_PROC(ptr_var) (arg_list) ;
result_expr IND_FUNC(ptr_var) (arg_list)
```

```
CALL IND_PROC(ENTRY_FN)(0xDEAD);

INT32 R;
IND_FUNC(CMP)(A, B) => R;
```

### 19.5 Procedure pointer arrays (vtables)

```
CONSTANT INT32 N_OPS = 4;
PROC_PTR(THREAD_ENTRY) VTABLE(N_OPS);

REF(PRODUCER_THREAD) => VTABLE(1);
REF(CONSUMER_THREAD) => VTABLE(2);

% Call the second entry
CALL IND_PROC(VTABLE(2))(ARG);
```

---

## 20. PRINT Statement

```
PRINT ( 'format_string' )
PRINT ( 'format_string' , expr { , expr } )
```

`PRINT` maps to C `printf`. The number of format specifiers must match the number of
extra arguments.

| Specifier | Argument type | Output |
|---|---|---|
| `%d` | `INT8`, `INT16`, `INT32` | Signed decimal integer |
| `%l` | `INT64` | Signed decimal 64-bit integer |
| `%u` | `UINT8`, `UINT16`, `UINT32` | **Unsigned decimal integer** *(new)* |
| `%ul` | `UINT64` | **Unsigned decimal 64-bit integer** *(new)* |
| `%x` | `UINT32`, `INT32` | **Lowercase hexadecimal** *(new)* |
| `%X` | `UINT32`, `INT32` | **Uppercase hexadecimal** *(new)* |
| `%f` | `REAL` | Floating-point decimal |
| `%c` | `INT8`, `INT32` | Single character (by ASCII code) |
| `%s` | `CHARACTER` | String |
| `%%` | *(none)* | Literal `%` |

Escape sequences: `\n`, `\t`, `\\`.

```
PRINT('TICK = %u\n', TICK_COUNT);
PRINT('ADDR = 0x%X\n', STACK_CANARY);
PRINT('TID = %d, PID = %d\n', T.TID, T.PID);
```

Width/precision modifiers (`%8d`, `%.2f`) are not supported.

---

## 21. Built-In Functions

### 21.1 CORE

Returns the index of the currently executing core as a `UINT32`.

```
UINT32 PROCEDURE CORE ;
```

```
UINT32 MY_CORE;
CORE() => MY_CORE;        % 0 on Core 0, 1 on Core 1
IF CORE() = 1 THEN CALL CORE1_IDLE;
```

### 21.2 SIZE_OF

Returns the size in bytes of a type or variable as a compile-time `UINT32` constant.

```
UINT32 PROCEDURE SIZE_OF (type_or_var)
```

```
UINT32 SZ;
SIZE_OF(INT32)   => SZ;   % 4
SIZE_OF(TCB)     => SZ;   % size of the TCB record
SIZE_OF(BUFFER)  => SZ;   % total bytes of an array or variable
```

### 21.3 OFFSET_OF

Returns the byte offset of a field within a RECORD as a compile-time `UINT32` constant.

```
UINT32 PROCEDURE OFFSET_OF (record_type, field_name)
```

```
UINT32 OFF;
OFFSET_OF(TCB, TID)      => OFF;   % 0
OFFSET_OF(TCB, PID)      => OFF;   % 4
OFFSET_OF(TCB, SAVED_SP) => OFF;   % 16  ← used by sched_asm.S
```

These match the hard-coded byte offsets in `sched_asm.S`:
```
str r0, [r1, #16]   % current_tcb->saved_sp = r0
ldr r0, [r0, #16]   % r0 = next_tcb->saved_sp
```

---

## 22. Reserved Words

```
ALIGN      AND        ARRAY      ASM        BARRIER    BEGIN
BIT        BYTE       CALL       CASE       CHARACTER  COMMENT
CONSTANT   CORE       DMBARRIER  DO         DOWNTO     ELSE
END        FLOAT      FOR        FORWARD     FROM       GOTO
IF         IN         IND        IND8       IND16      INT8
INT16      INT32      INT64      INTEGER    INTERRUPT  LONG
MMIO       MOD        NOT        OF         OR         OUT
PC         POP        PRINT      PROC_PTR   PROC_TYPE  PROCEDURE
PUSH       REAL       RECORD     REF        REPEAT     RETURN
SHL        SHR        SHRA       SIZE_OF    OFFSET_OF  SP
STEP       THEN       TO         UINT8      UINT16     UINT32
UINT64     UNTIL      VOLATILE   WHILE      WORD       XOR
IND_PROC   IND_FUNC   LR
```

All keywords are case-insensitive. `BYTE`, `WORD`, `INTEGER`, and `LONG` are accepted as
PL-11 compatibility aliases — do not use them in new PL-PICO code.

---

## 23. PL-11 → PL-PICO Type Mapping

| PL-11 | PL-PICO | Notes |
|---|---|---|
| `BYTE` | `INT8` | 8-bit signed; BYTE still accepted as alias |
| `WORD` | `INT32` | **Was 16-bit; now 32-bit** — behaviour differs for overflow |
| `INTEGER` | `INT32` | Alias mapping unchanged but wider |
| `LONG` | `INT32` | Same native width on RP2040; LONG accepted as alias |
| *(none)* | `INT16` | New — use when 16-bit precision required |
| *(none)* | `UINT8/16/32/64` | New — essential for hardware bitmasks and addresses |
| *(none)* | `INT64` | New — software-emulated; use sparingly |
| `R0–R15` | `R0–R12, LR, SP, PC` | ARM register file replaces PDP-11 |
| `R6 = SP` | `R13 = SP (PSP/MSP)` | Thread uses PSP; handler uses MSP |
| `R7 = PC` | `R15 = PC` | |
| `PUSH` / `POP` | `PUSH` / `POP` | 4-byte words instead of 2-byte |
| *(none)* | `VOLATILE` | New — required for ISR-shared and MMIO variables |
| *(none)* | `INTERRUPT PROCEDURE` | New — ISR with ARM exception entry/exit |
| `IND(addr)` | `IND(addr)` | Unchanged; now 32-bit native |
| *(none)* | `IND8(addr)` | New — byte access |
| *(none)* | `IND16(addr)` | New — halfword access |
| *(none)* | `MMIO(addr)` | New — volatile hardware register access |
| *(none)* | `BARRIER` / `DMBARRIER` | New — ARM memory barrier instructions |
| *(none)* | `RECORD` | New — named aggregate type (struct) |
| *(none)* | `PROC_PTR` / `PROC_TYPE` | New — procedure pointer types |
| *(none)* | `CORE()` | New — current core index built-in |
| *(none)* | `SIZE_OF()` / `OFFSET_OF()` | New — compile-time layout intrinsics |
| `%d` (WORD) | `%d` (INT32) | Same specifier, wider type |
| *(none)* | `%u`, `%ul`, `%x`, `%X` | New PRINT format specifiers |
| `#FF` hex | `#FF` or `0xFF` | Both accepted; `0x` form preferred |

---

## 24. Key Differences from PL-11

1. **Native word is 32 bits.** `INTEGER` and `WORD` alias to `INT32` (not 16-bit).
   Programs that relied on 16-bit wraparound must be reviewed.

2. **Unsigned types exist.** `UINT8/16/32/64` are essential for addresses, bitmasks,
   and hardware registers. Use them — the PDP-11 had none.

3. **ARM register file.** R6 is no longer SP and R7 is no longer PC. SP = R13, LR = R14,
   PC = R15. R8–R12 have restricted M0+ instruction encoding.

4. **PUSH/POP are 4-byte.** Each stack operation moves SP by 4, not 2.

5. **VOLATILE is mandatory** for any variable shared between mainline code and an ISR,
   or between Core 0 and Core 1.

6. **MMIO() is distinct from IND().** Use MMIO for hardware registers; IND for software
   pointers. Both are 32-bit dereferences but MMIO is always volatile and carries
   hardware-access intent.

7. **RECORD aggregates.** Struct-like types with named fields, layout intrinsics, and
   dot-notation access. Critical for kernel data structures.

8. **Procedure pointers.** PROC_PTR/PROC_TYPE allow function tables, thread entry points,
   and device driver vtables.

9. **No FPU.** `REAL` arithmetic is entirely software-emulated. Avoid in ISRs; avoid in
   tight loops where performance matters.

10. **Memory barriers are explicit.** PL-11 ran single-core; PL-PICO targets dual-core
    hardware with a shared bus. Correct inter-core protocols require DMBARRIER.

---

## Appendix A: Reserved Words (Alphabetical)

```
ALIGN      AND        ARRAY      ASM
BARRIER    BEGIN      BIT        BYTE
CALL       CASE       CHARACTER  COMMENT
CONSTANT   CORE
DMBARRIER  DO         DOWNTO
ELSE       END
FLOAT      FOR        FORWARD    FROM
GOTO
IF         IN         IND        IND8       IND16
IND_FUNC   IND_PROC   INT8       INT16      INT32
INT64      INTEGER    INTERRUPT
LONG       LR
MMIO       MOD
NOT
OF         OFFSET_OF  OR         OUT
PC         POP        PRINT      PROC_PTR   PROC_TYPE
PROCEDURE  PUSH
REAL       RECORD     REF        REPEAT     RETURN
SHL        SHR        SHRA       SIZE_OF    SP
STEP
THEN       TO
UINT8      UINT16     UINT32     UINT64     UNTIL
VOLATILE
WHILE      WORD
XOR
```

---

## Appendix B: ARM Cortex-M0+ Register Names

```
% General purpose
R0   R1   R2   R3   R4   R5   R6   R7
R8   R9   R10  R11  R12

% Special
SP   (= R13 — stack pointer; PSP in Thread mode, MSP in Handler mode)
LR   (= R14 — link register; return address)
PC   (= R15 — program counter)

% System
PSP  (process stack pointer)
MSP  (main stack pointer)
XPSR (extended program status register)
APSR (application status: N, Z, C, V flags)
PRIMASK (interrupt mask — bit 0 set by CPSID i, cleared by CPSIE i)
```

M0+ note: R8–R12 can be read/written with MOV but cannot be used directly in
STMIA/LDMIA — they must be staged through R0–R7.

---

## Appendix C: Operator Quick Reference

| Operator | Class | Prec | Notes |
|---|---|---|---|
| `REF(v)` | Address | — | UINT32 address of variable v |
| `IND(e)` | Indirect | — | Dereference 32-bit address (read/write) |
| `IND8(e)` | Indirect | — | Dereference 8-bit address |
| `IND16(e)` | Indirect | — | Dereference 16-bit address |
| `MMIO(e)` | HW reg | — | Volatile 32-bit hardware access |
| `-` (unary) | Arith | 1 | Right-assoc |
| `NOT` | Logical | 1 | Bitwise/logical NOT |
| `*` `/` `MOD` | Arith | 2 | Left-assoc |
| `AND` `SHL` `SHR` `SHRA` | Bit | 2 | Left-assoc |
| `+` `-` | Arith | 3 | Left-assoc |
| `OR` `XOR` | Bit/Log | 3 | Left-assoc |
| `=` `/=` `<` `>` `<=` `>=` | Relational | 4 | Produce 0/non-zero |
| `value => target` | Assignment | stmt | Statement only |
| `var OP expr ;` | Modify | stmt | Compound shorthand |
| `CORE()` | Built-in | — | Current core index (UINT32) |
| `SIZE_OF(t)` | Built-in | — | Compile-time size in bytes |
| `OFFSET_OF(r,f)` | Built-in | — | Compile-time field offset |
| `IND_PROC(p)(args)` | Indirect call | stmt | Call via PROC_PTR |
| `IND_FUNC(p)(args)` | Indirect call | expr | Call function via PROC_PTR |

---

## Appendix D: RP2040 Key Hardware Addresses

```
% Single-cycle I/O (SIO) — core-local, no bus arbitration
CONSTANT UINT32 SIO_BASE       = 0xD0000000;
CONSTANT UINT32 SIO_CPUID      = 0xD0000000; % current core number (0 or 1)
CONSTANT UINT32 SIO_GPIO_OUT   = 0xD0000010; % GPIO output level
CONSTANT UINT32 SIO_GPIO_OE    = 0xD0000020; % GPIO output enable

% UART0
CONSTANT UINT32 UART0_BASE     = 0x40034000;
CONSTANT UINT32 UART0_DR       = 0x40034000; % data register
CONSTANT UINT32 UART0_FR       = 0x40034018; % flag register (bits: TXFF=5, RXFE=4)
CONSTANT UINT32 UART0_CR       = 0x40034030; % control register

% SysTick timer (ARM standard)
CONSTANT UINT32 SYST_CSR       = 0xE000E010; % control and status
CONSTANT UINT32 SYST_RVR       = 0xE000E014; % reload value
CONSTANT UINT32 SYST_CVR       = 0xE000E018; % current value

% Cortex-M system control
CONSTANT UINT32 SCB_ICSR       = 0xE000ED04; % interrupt control and state
CONSTANT UINT32 SCB_VTOR       = 0xE000ED08; % vector table offset
CONSTANT UINT32 NVIC_ISER      = 0xE000E100; % interrupt set-enable

% Flash (XIP region — read only, written via SSI peripheral)
CONSTANT UINT32 XIP_BASE       = 0x10000000; % start of flash in address space
CONSTANT UINT32 FS_FLASH_OFFSET = 0x00100000; % picoOS FS at +1 MB
```

Usage:

```
UINT32 WHICH_CORE;
MMIO(SIO_CPUID) => WHICH_CORE;

% Wait until UART TX FIFO not full, then transmit 'A'
WHILE MMIO(UART0_FR) AND 0x20 /= 0 DO ;
0x41 => MMIO(UART0_DR);
```

---

## Appendix E: Annotated OS Code Snippet

This example shows VOLATILE, MMIO, INTERRUPT, and RECORD working together in a realistic
kernel fragment. It mirrors the picoOS structures in `src/kernel/`.

```
% ── Type definitions ────────────────────────────────────────────────────────

% Thread Control Block — first five fields MUST match sched_asm.S offsets.
RECORD TCB
    TID        : UINT32 ;   % offset  0  — thread ID
    PID        : UINT32 ;   % offset  4  — owning process ID
    STACK_BASE : UINT32 ;   % offset  8  — lowest address of thread stack
    STACK_SIZE : UINT32 ;   % offset 12  — stack size in bytes
    SAVED_SP   : UINT32     % offset 16  — saved stack pointer (PendSV RW)
END ;

% Spinlock — 0 = free, non-zero = held
RECORD SPINLOCK
    LOCK : VOLATILE UINT32
END ;

% Mutex
RECORD KMUTEX
    SPIN      : RECORD(SPINLOCK) ;
    OWNER_TID : VOLATILE INT32 ;     % TID of holder, -1 = free
    COUNT     : VOLATILE UINT32 ;    % 0 or 1 (non-recursive)
    WAITERS   : UINT32               % head of blocked-thread list (address)
END ;

% Message queue
CONSTANT UINT32 MQ_MAX_MSG  = 16;
CONSTANT UINT32 MQ_MSG_SIZE = 64;

RECORD MQUEUE
    SPIN         : RECORD(SPINLOCK) ;
    MSG_SIZE     : UINT32 ;
    HEAD         : UINT32 ;
    TAIL         : UINT32 ;
    COUNT        : UINT32 ;
    SEND_WAITERS : UINT32 ;
    RECV_WAITERS : UINT32
END ;

% ── Global state ─────────────────────────────────────────────────────────────

VOLATILE UINT32 TICK_MS;            % incremented by SysTick ISR
VOLATILE INT32  SCHED_PENDING;      % set when a reschedule is needed

% ── SysTick interrupt service routine ────────────────────────────────────────

INTERRUPT PROCEDURE SYSTICK_HANDLER ;
BEGIN
    TICK_MS + 1 => TICK_MS;         % safe: only ISR writes, mainline reads

    % Request a context switch every 5 ms
    IF TICK_MS MOD 5 = 0 THEN
        BEGIN
            1 => SCHED_PENDING;
            % Set PendSV pending bit in ICSR (bit 28)
            0x10000000 => MMIO(0xE000ED04)
        END
END ;

% ── Scheduler helper: busy-wait with sleep ────────────────────────────────────

PROCEDURE SLEEP_MS (IN UINT32 MS) ;
BEGIN
    UINT32 DEADLINE;
    TICK_MS + MS => DEADLINE;
    WHILE TICK_MS < DEADLINE DO
        ASM('wfi')      % sleep until next interrupt, then recheck
END ;

% ── Spinlock acquire / release ────────────────────────────────────────────────

PROCEDURE SPIN_ACQUIRE (IN OUT RECORD(SPINLOCK) S) ;
BEGIN
    DO ;
    UNTIL S.LOCK = 0;
    1 => S.LOCK;
    BARRIER                         % prevent stores after acquire moving up
END ;

PROCEDURE SPIN_RELEASE (IN OUT RECORD(SPINLOCK) S) ;
BEGIN
    BARRIER;                        % prevent loads before release moving down
    0 => S.LOCK
END ;

% ── Inter-core data publish (Core 0 writer / Core 1 reader) ──────────────────

VOLATILE UINT32 SHARED_RESULT;
VOLATILE UINT32 RESULT_READY;

PROCEDURE CORE0_PUBLISH (IN UINT32 VALUE) ;
BEGIN
    VALUE => SHARED_RESULT;
    DMBARRIER;                      % all stores visible before flag set
    1 => RESULT_READY
END ;

INT32 PROCEDURE CORE1_CONSUME () ;
BEGIN
    WHILE RESULT_READY = 0 DO ;
    DMBARRIER;                      % all loads after this see published stores
    RETURN(SHARED_RESULT)
END ;

% ── Verify layout intrinsics ─────────────────────────────────────────────────

BEGIN
    UINT32 OFF;
    OFFSET_OF(TCB, SAVED_SP) => OFF;
    % OFF must equal 16 — matches the hard-coded #16 in sched_asm.S:
    %   str r0, [r1, #16]   % current_tcb->saved_sp = r0
    %   ldr r0, [r0, #16]   % r0 = next_tcb->saved_sp
    IF OFF /= 16 THEN
        PRINT('FATAL: TCB layout mismatch — OFFSET_OF(TCB,SAVED_SP) = %u\n', OFF);

    PRINT('Core %u running\n', CORE());
    CALL SLEEP_MS(1000);
    PRINT('1 second elapsed, TICK_MS = %u\n', TICK_MS)
END
```
