# Contributing to Rack Evacuation Planner

Thanks for your interest in improving Rack Evacuation Planner. This document
explains how to build and test the project, what a good change looks like, and
the terms under which contributions are accepted.

## Contribution terms

Rack Evacuation Planner is released under the [Apache License, Version 2.0](LICENSE).
Contributions are accepted under exactly the same terms: by opening a pull
request or otherwise submitting a change, you agree that your contribution is
licensed under Apache-2.0, as described in section 5 of the license
("Submission of Contributions"). Inbound and outbound licensing are identical.

There is **no Contributor License Agreement (CLA) to sign and no copyright
assignment**. You keep the copyright on your work; the project receives it under
the same license that already covers the project. Please only submit work you
have the right to submit, and do not copy code from sources whose license is
incompatible with Apache-2.0.

## Building

Requirements:

- CMake 3.21 or newer.
- A C++20 compiler. On Windows this means MSVC from Visual Studio 2022 or newer.
- Ninja, for the `release`, `debug`, `asan` and `ci` presets.

Configure presets live in `CMakePresets.json`. The Ninja presets deliberately do
not hardcode a compiler path; they expect the environment to provide `cl.exe`.
On Windows, start an **x64 Native Tools Command Prompt for VS 2022** (or
equivalent developer shell) before using them. If you would rather not use a
developer shell, the `release-msvc-vs` preset uses the "Visual Studio 17 2022"
generator and configures from an ordinary shell.

Release build:

```sh
cmake --preset release
cmake --build --preset release
```

Debug build:

```sh
cmake --preset debug
cmake --build --preset debug
```

AddressSanitizer build (MSVC `/fsanitize=address`, Debug):

```sh
cmake --preset asan
cmake --build --preset asan
```

Without a developer shell:

```sh
cmake --preset release-msvc-vs
cmake --build build/vs2022 --config Release
```

Plain CMake works too if you prefer to drive it yourself:

```sh
cmake -S . -B build/release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/release
```

## Testing

Every build tree that was configured with `REP_BUILD_TESTS=ON` (the default, and
what all presets set) registers the test suite with CTest:

```sh
ctest --preset release
ctest --preset debug
```

For the sanitizer or Visual Studio trees, point CTest at the build directory:

```sh
ctest --test-dir build/asan --output-on-failure
ctest --test-dir build/vs2022 -C Release --output-on-failure
```

A change is ready when:

- the project configures cleanly and builds in **both Release and Debug**;
- the **complete** test suite passes, with no failures and no tests skipped or
  disabled to make it pass;
- the build produces **zero first-party warnings**. Warnings in this project's
  own targets are errors (`REP_WARNINGS_AS_ERRORS=ON`: `/WX` on MSVC,
  `-Werror` elsewhere), so a warning is a build failure. Fix the warning rather
  than suppressing it; a targeted suppression is acceptable only when the
  warning is genuinely not actionable, and it must come with a comment saying
  why.

Tests are expected to be deterministic and to terminate on their own: a test
that needs an external deadline to finish is a defect in the test, not a
configuration problem. Prefer a design that cannot hang — bounded inputs,
fixed iteration counts, explicit synchronisation with a known participant
count — over one that depends on scheduling luck.

## What a good change looks like

- **Narrow public API.** Add the smallest surface that solves the problem. Public
  headers in `include/rep/` are a long-lived commitment; prefer internal helpers
  and keep new declarations out of the public API unless they must be there.
- **Deterministic behaviour.** The same inputs must produce the same outputs,
  ordering, and digests on every run and every platform. Avoid unordered
  iteration leaking into results, wall-clock or locale dependence, and reliance
  on unspecified evaluation order.
- **Tests that prove behaviour.** A test should exercise a real contract with
  inputs a user could supply and assert on the observable result. Do not restate
  the implementation in the test, assert on trivia, or write a test that passes
  even when the behaviour under test is removed.
- **No placeholder code.** No stubs, no `TODO`-as-implementation, no dead
  branches, no "will finish later" scaffolding. If it is not ready, leave it out
  of the change.
- **Documentation follows behaviour.** When observable behaviour, options, or
  usage change, update the relevant documentation and examples in the same
  change.

## Commit hygiene

- Write a concise, neutral commit message: a short imperative subject line, plus
  a body explaining the reason for the change when it is not obvious.
- Keep one logical change per commit. Refactoring, formatting, and behaviour
  changes belong in separate commits.
- Do **not** add `Co-authored-by` trailers, and do not credit generative or
  automated tooling anywhere in the commit message. Commits should name the
  people who wrote and reviewed the change.
- Do not include generated noise: build output, editor or IDE files, unrelated
  reformatting, or vendored artifacts. Keep diffs reviewable.

## Conduct in issues and reviews

Be professional and constructive. Discuss the code, not the person: explain the
problem, the evidence, and the change you would like to see. Assume good faith,
keep review comments specific and actionable, and accept that maintainers may
decline a change that does not fit the project's direction. Harassment,
personal attacks, and dismissive behaviour are not acceptable in any project
space.
