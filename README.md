# jl-git-helper: Magit's metadata git calls from libgit2

On Windows, starting a git process costs about 100 ms, and most git
processes in a Magit status refresh only look up metadata. `jl-git-helper`
is a small C program built on libgit2. Emacs starts it once and keeps it
running, and it answers those lookups without starting git.

It hooks into `magit-process-file`, so it covers every place Magit runs git
synchronously. It reproduces git's stdout byte for byte for a short list of
read-only commands and declines everything else; declined calls run git
exactly as before. Magit and `jl-magit-prefill` need no changes.

In a status refresh, these go to the helper:
`for-each-ref`, `rev-parse` (toplevel, cdup, git dir, revisions), `config
--list` / `--get-all`, `worktree list`, the recent-commits and unpulled logs,
`log --no-walk` subjects, and the stash `reflog`.

These stay with git: `status`, `diff`, `diff --cached` and
`update-index --refresh`, since they need fsmonitor and the untracked cache,
which libgit2 doesn't support.

## Files

| File | What |
|---|---|
| `helper/jl-git-helper.c`, `helper/Makefile` | the helper |
| `jl-magit-git-helper.el` | Emacs side: starts the helper, routes git calls to it |
| `jl-magit-trace.el` | `M-x jl/magit-trace-refresh`: git processes, time and helper use for one refresh |
| `helper/test/` | comparisons against real git (see Testing) |

## Build

macOS (Homebrew):

```sh
brew install libgit2 pkg-config
cd helper && make
```

Linux: install `libgit2-dev` (Debian/Ubuntu) or `libgit2-devel` (Fedora), then `make`.

Windows: in an MSYS2 **MINGW64** shell:

```sh
pacman -S --needed make mingw-w64-x86_64-gcc mingw-w64-x86_64-pkgconf mingw-w64-x86_64-libgit2
cd helper && make dist
```

`dist/` then holds `jl-git-helper.exe` and the MSYS2 DLLs it needs, so it
runs from a normal Windows Emacs without MSYS2 on `PATH`.

## Install

1. Put the executable (on Windows, everything in `dist/`) in
   `~/.emacs.d/jl-git-helper/`, or set `jl/magit-git-helper-program`.
2. Put `jl-magit-git-helper.el` on the `load-path` and load it after Magit:

   ```elisp
   (use-package jl-magit-git-helper
     :ensure nil
     :load-path "lisp"
     :after magit
     :custom (jl/magit-git-helper-mode 'verify)) ; t once verified
   ```

3. **On each new machine, start in verify mode.** In verify mode git still
   runs and its answer is used; the helper's answer is only compared with
   it. Use Magit for a while, then run `M-x jl/magit-git-helper-report`. If
   it reports 0 mismatches, set the mode to `t`. Any differences are listed
   in `*jl-git-helper log*`.

Requires git 2.42 or newer: the helper asks `git var GIT_CONFIG_SYSTEM` once
for the system config file, so libgit2 reads the same file git does.

## When the helper declines

Each declined call runs git, so you get today's speed, never a wrong answer.
The helper declines:

- any command or option not on its list, including log buffers with
  `--graph` and anything that writes;
- calls whose stderr Magit would see, such as a failing call where Magit
  wants the error text;
- a remote `default-directory` (Tramp), a directory inside `.git`, and bare
  repositories;
- calls made while `GIT_DIR`, `GIT_INDEX_FILE`, `GIT_CONFIG*` or similar
  are set (magit-wip uses `GIT_INDEX_FILE`, for example);
- repos with config includes, `extensions.worktreeConfig`, replace refs,
  non-UTF-8 commit encodings, or relative worktree paths.

If the helper doesn't answer within `jl/magit-git-helper-timeout`, it is
stopped and git is used. `M-x jl/magit-git-helper-stop` resets it.

## Testing

```sh
sh helper/test/make-test-repo.sh /tmp/jlgh               # repo with the edge cases
python3 -I helper/test/compare.py helper/jl-git-helper /tmp/jlgh
python3 -I helper/test/compare.py helper/jl-git-helper ~/some/real/repo
emacs --batch -Q -l helper/test/status-compare.el REPO "$PWD/helper/jl-git-helper" \
      ~/.emacs.d/elpa ~/.emacs.d/lisp "$PWD"
```

- `compare.py` runs each case through both the helper and git, and fails on
  any difference in stdout or exit code.
- `status-compare.el` renders the Magit status and log buffers three ways
  (git only, with the helper, verify mode) and fails unless all three are
  identical.

As of 2026-11-23 on macOS (git 2.55, libgit2 1.9.6): 248 identical answers
and 0 mismatches on the test repo; identical status and log buffers; a
status refresh went from 14 git processes to 4. **Not yet built or run on
Windows.**
