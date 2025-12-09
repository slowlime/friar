# Friar
A Lama bytecode interpreter.

## Build
### The Lama runtime
Friar uses the Lama runtime library.
If you haven't compiled it yet, perform the following steps:

1. Initialize the Git submodule:

   ```
   $ git submodule update --init
   ```

2. Compile the runtime:

   ```
   $ make -C third-party/lama/runtime
   ```

The compiled runtime library will be located at `third-party/lama/runtime/runtime.a`.
Once you have the runtime library, proceed to building Friar proper.

### Friar
Friar uses the [Meson] build system; make sure you have it installed.
The minimum supported Meson version is 1.1.

1. Create a build directory:

   ```
   $ meson setup build-release -Doptimization=3
   ```

   - Friar defines a number of options you can change by passing `-D<name>=<value>` flags.
     To list all available options, run `meson configure`.

2. Compile the executable:

   ```
   $ meson compile -C build-release
   ```

Run `build-release/friar` to launch Friar.

[Meson]: https://mesonbuild.com

## Usage
```
Usage: friar [-h] [--mode=MODE] [--] <input>

  <input>       A path to the Lama bytecode file to interpret.

Options:
  -h, --help    Print this help message.

  -t, --time    Measure the execution time.

  --mode=MODE   Select the execution mode. Available choices:
                - disas: disassemble the bytecode and exit.
                - verify: only perform bytecode verification.
                - idiom: search for bytecode idioms.
                - run: execute the bytecode (default).
```

## Tests
You can run Lama's test suite via `./scripts/run-tests.sh`.

## Performance
Below are the results of running the `Sort.lama` benchmark (included in the Lama repository) on different interpreters.
Each measurement is averaged over 5 runs, performed on my machine (Linux, AMD Ryzen 9 7950X, pinned to single hardware thread).

Interpreter                  | Total runtime (s) | Relative speedup over previous
:----------------------------|:------------------|:------------------------------
`lamac -i`                   | 316               | N/A
`friar` (dynamic validation) | 167.3             | 89.0%
`friar` (static validation)  | 121.4             | 37.8%
`lamac -s`                   | 93.4              | 30.3%

The two `friar` entries are both compiled with `-Doptimization=3` and only differ in the value of the `-Ddynamic_verification` option:

- When `=true`, bytecode validity is checked "on the fly" during interpretation.
  While this allows to run more esoterically constructed programs, doing all these checks slows the overall performance down.

- When `=false`, before interpretation, bytecode is statically validated to be free of common errors: lack of stack operands, wrong instruction encoding, etc.
  This allows the interpreter itself to assume bytecode validity and elide these checks, improving the performance by ~38%.

  - The verifier requires that stack heights are equal at control-flow merge points.
    In addition, all statically reachable instructions, including those that are never executed dynamically, need to be valid.
    This renders some (otherwise correct) programs unable to pass validation.
    However, since `lamac` never emits such programs, it isn't an issue in practice.

  - The `-t` option allows to measure individual stages of `friar` execution.
    It shows that bytecode validation takes a negligible amount of time (on the order of 10 μs).

## Bytecode frequency analyzer
Friar includes a bytecode frequency analyzer (`--mode=idiom`), which looks for sequences of one or two instructions (called "idioms" for conciseness) and shows the number of times they occur statically in the bytecode.

<details>

<summary>Here's what it looks like (for <code>Sort.lama</code>)</summary>

```
31  drop
28  dup
21  elem
16  const 1
13  const 1; elem
11  const 0
11  drop; dup
11  dup; const 1
10  drop; drop
 8  const 0; elem
 7  dup; const 0
 7  elem; drop
 7  ld A(0)
 5  end
 4  sexp 0 2
 4  dup; dup
 3  jmp 762
 3  dup; array 2
 3  elem; st L(0)
 3  ld L(0)
 3  ld L(3)
 3  st L(0)
 3  st L(0); drop
 3  call 351 1
 3  array 2
 3  call Barray 2
 3  call Barray 2; jmp 762
 2  binop ==
 2  sexp 0 2; call Barray 2
 2  jmp 350
 2  jmp 116
 2  dup; tag 0 2
 2  elem; const 0
 2  elem; const 1
 2  ld L(1)
 2  begin 1 0
 2  call 43 1
 2  call 151 1
 2  tag 0 2
 1  binop -
 1  binop -; call 43 1
 1  binop >
 1  binop >; cjmpz 600
 1  binop ==; cjmpz 274
 1  binop ==; cjmpz 191
 1  const 0; binop ==
 1  const 0; jmp 116
 1  const 0; line 9
 1  const 1; binop -
 1  const 1; binop ==
 1  const 1; line 6
 1  const 10000
 1  const 10000; call 43 1
 1  sexp 0 2; jmp 116
 1  sexp 0 2; call 351 1
 1  jmp 262
 1  jmp 336
 1  jmp 386
 1  jmp 715
 1  jmp 734
 1  drop; const 0
 1  drop; jmp 262
 1  drop; jmp 336
 1  drop; jmp 386
 1  drop; jmp 715
 1  drop; jmp 734
 1  drop; ld L(5)
 1  drop; line 5
 1  drop; line 15
 1  drop; line 16
 1  dup; drop
 1  elem; sexp 0 2
 1  elem; dup
 1  elem; st L(1)
 1  elem; st L(2)
 1  elem; st L(3)
 1  elem; st L(4)
 1  elem; st L(5)
 1  ld L(0); sexp 0 2
 1  ld L(0); jmp 350
 1  ld L(0); call 151 1
 1  ld L(1); binop >
 1  ld L(1); ld L(3)
 1  ld L(2)
 1  ld L(2); call 351 1
 1  ld L(3); ld L(0)
 1  ld L(3); ld L(1)
 1  ld L(3); ld L(4)
 1  ld L(4)
 1  ld L(4); sexp 0 2
 1  ld L(5)
 1  ld L(5); ld L(3)
 1  ld A(0); const 1
 1  ld A(0); dup
 1  ld A(0); ld A(0)
 1  ld A(0); cjmpz 106
 1  ld A(0); call 351 1
 1  ld A(0); call 151 1
 1  ld A(0); call Barray 2
 1  st L(1)
 1  st L(1); drop
 1  st L(2)
 1  st L(2); drop
 1  st L(3)
 1  st L(3); drop
 1  st L(4)
 1  st L(4); drop
 1  st L(5)
 1  st L(5); drop
 1  cjmpz 274
 1  cjmpz 274; dup
 1  cjmpz 600
 1  cjmpz 600; const 1
 1  cjmpz 106
 1  cjmpz 106; ld A(0)
 1  cjmpz 191
 1  cjmpz 191; dup
 1  cjmpnz 280
 1  cjmpnz 637
 1  cjmpnz 637; drop
 1  cjmpnz 392
 1  cjmpnz 428
 1  cjmpnz 428; drop
 1  cjmpnz 197
 1  begin 1 0; line 18
 1  begin 1 0; line 24
 1  begin 1 1
 1  begin 1 1; line 14
 1  begin 1 6
 1  begin 1 6; line 3
 1  begin 2 0
 1  begin 2 0; line 25
 1  call 117 1
 1  tag 0 2; cjmpnz 392
 1  tag 0 2; cjmpnz 428
 1  array 2; cjmpnz 280
 1  array 2; cjmpnz 637
 1  array 2; cjmpnz 197
 1  fail 7 17
 1  fail 14 9
 1  line 3
 1  line 3; ld A(0)
 1  line 5
 1  line 5; ld L(3)
 1  line 6
 1  line 6; ld L(1)
 1  line 7
 1  line 7; ld L(2)
 1  line 9
 1  line 9; ld A(0)
 1  line 14
 1  line 14; ld A(0)
 1  line 15
 1  line 15; ld L(0)
 1  line 16
 1  line 16; ld L(0)
 1  line 18
 1  line 18; line 20
 1  line 20
 1  line 20; ld A(0)
 1  line 24
 1  line 24; ld A(0)
 1  line 25
 1  line 25; line 27
 1  line 27
 1  line 27; const 10000
```

</details>
