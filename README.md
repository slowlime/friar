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

2. Compile the executable:

   ```
   $ meson compile -C build-release
   ```

Run `build-release/friar` to launch Friar.

[Meson]: https://mesonbuild.com

## Usage
```
Usage: friar [-h] [--] <input>

  <input>       A path to the Lama bytecode file to interpret.

Options:
  -h, --help    Print this help message.
```

## Tests
You can run Lama's test suite via `./scripts/run-tests.sh`.

## Performance
In the `Sort.lama` benchmark (included in the Lama repository), Friar performs about 2.7 times faster (≈ 123 s) than Lama's recursive interpreter (≈ 328 s), as measured on my machine (Linux, AMD Ryzen 9 7950X, pinned to single hardware thread).
