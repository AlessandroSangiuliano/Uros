# Migcom — Description of the tests

## What is tested ✅
- Running the `migcom` binary with a minimal input (a `.mig` file) to check that it does not crash and ends with exit code 0.
- Smoke test after refactors/builds, to make sure the tool's basic functionality is intact.

## How I test it 🔧
- Automatic test registered in CTest (file: the module's `CMakeLists.txt`).
- Command run by the test:

  ```sh
  /path/to/build/src/lib/migcom/migcom -V -user /dev/null -server /dev/null -sheader /dev/null -dheader /dev/null < tests/minimal.mig
  ```

- Steps to run it manually:
  1. Configure the build: `cmake -S . -B build`
  2. Build `migcom`: `cmake --build build --target migcom -j N`
  3. Run the directory's test: `ctest --test-dir build/src/lib/migcom -V`

## Tests included in the module 🧪
- `minimal.mig`: minimal test that checks that `migcom` recognizes a simple `subsystem` and ends correctly (smoke test).
- `routine-simple.mig`: checks the parsing of a `simpleroutine` with `request_port` and an `in` argument; checks the validation of the port's type and the correctness of the routine's signature.
- `routine-complex.mig`: exercises the parsing of a routine with `request_port`, `in` and `inout` arguments; checks the handling of several categories of arguments and the parser's robustness.
- `skip-only.mig`: checks that `skip;` statements, comments and whitespace are ignored without producing errors.

All the tests above must end with exit code `0` (`Passed` in CTest).

## Why I test it (reason) 💡
- Provides a simple, quick check that the main process (parsing, file generation, and correct termination) has not been broken by changes.
- Useful as a first integration gate before running deeper tests or integration tests.

## Expected results 🎯
- The command must end with exit code `0` (`Passed` in CTest).
- No crash or dump; diagnostic output (with `-V`) is acceptable, but not fatal errors.

## Extending the tests ➕
- Add more `.mig` files in `tests/` that check more complex cases (polymorphic arguments, various KPDs, static templates, different options of the command).
- For each new test, add an `add_test(...)` line in the module's `CMakeLists.txt` to register it in CTest.

## Practical notes and tips ✨
- The current test redirects output/files to `/dev/null` to make it deterministic and fast.
- For tests that compare output, temporary files can be generated and `diff` used to check the expectations.

---

If you like, I can now:
1. Add further tests with representative cases, or
2. Promote the module's tests so that they also run from the project-level `ctest` (root). 

Tell me which option you prefer.