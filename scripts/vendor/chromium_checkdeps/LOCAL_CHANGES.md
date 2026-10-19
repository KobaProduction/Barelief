# Project adaptations

Pinned upstream revision and BSD license are recorded in ORIGIN and LICENSE.
No rule engine is reimplemented. Two upstream files have small local changes:

- `builddeps.py`: use standard `git` on Windows, and include nonignored untracked
  source directories so newly added modules are checked before staging.
- `cpp_checker.py`: accept angle-bracket includes and additional C/C++ suffixes.
  Normalize Qt module paths and flat `QObject`-style forwarding headers to the
  virtual `Qt/` prefix, so a single `-Qt` rule protects portable modules.

Upstream limitations remain: literal include scanning, not compiler AST or
macro expansion; flat non-Qt includes are skipped; `#if 0` bodies are skipped.
Public namespaces and repository-relative paths are checked as written, without
resolving all CMake include roots. This is an include boundary gate, not a
semantic dependency graph or proof of Clean Architecture.
