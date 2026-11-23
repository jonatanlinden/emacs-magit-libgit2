#!/usr/bin/env python3
"""Compare jl-git-helper's answers with real git, byte for byte.

Usage: compare.py HELPER TESTDIR    (TESTDIR from make-test-repo.sh)

Every case runs through the helper and through git, in the same directory
and with the same environment (HOME=TESTDIR/home).  A case passes when
the helper either matches git exactly (stdout and exit code) or declines
("unsupported"); declines are listed so expected ones can be checked.
"""

import os
import subprocess
import sys
import time

HELPER, T = sys.argv[1], os.path.realpath(sys.argv[2])
# A directory from make-test-repo.sh has its own HOME; any other directory
# is taken to be a repository and tested with your own environment.
TEST_REPO = os.path.isdir(f"{T}/home")
ENV = dict(os.environ)
if TEST_REPO:
    ENV.update(HOME=f"{T}/home", XDG_CONFIG_HOME=f"{T}/home/.config")
for k in list(ENV):
    if k.startswith("GIT_"):
        del ENV[k]

GLOBAL = ["--no-pager", "--literal-pathspecs",
          "-c", "core.preloadIndex=true", "-c", "log.showSignature=false",
          "-c", "color.ui=false", "-c", "color.diff=false",
          "-c", "diff.noPrefix=false"]
LOGFMT = "--format=%h%x0c%D%x0c%x0c%aN%x0c%at%x0c%x0c%s"
REFFMT = ("--format=%(refname)%00%(objectname)%00%(symref)%00"
          "%(upstream:short)%00%(upstream:track)")

DIRS = {
    "top": f"{T}/repo",
    "sub": f"{T}/repo/sub/dir",
    "link": f"{T}/repo-link",
    "linksub": f"{T}/repo-link/sub",
    "wt": f"{T}/wt-branch",
    "wtdet": f"{T}/wt-detached",
    "gitdir": f"{T}/repo/.git",
    "bare": f"{T}/origin.git",
} if TEST_REPO else {"repo": T}

CASES = [
    # What a Magit status refresh runs (with jl-magit-prefill).
    ["for-each-ref", "--include-root-refs", REFFMT],
    ["rev-parse", "--show-toplevel", "--git-dir", "--is-bare-repository",
     "HEAD", "HEAD~", "HEAD~10"],
    ["rev-parse", "--show-toplevel"],
    ["rev-parse", "--show-cdup"],
    ["config", "--list", "-z"],
    ["config", "--local", "-z", "--get-all", "--include",
     "status.showUntrackedFiles"],
    ["worktree", "list", "--porcelain", "-z"],
    ["log", LOGFMT, "--decorate=full", "-n10", "--use-mailmap",
     "--no-prefix", "--"],
    ["log", LOGFMT, "--decorate=full", "-n256", "--use-mailmap",
     "--no-prefix", "..@{upstream}", "--"],
    ["log", LOGFMT, "--decorate=full", "-n256", "--use-mailmap",
     "--no-prefix", "@{upstream}..", "--"],
    ["log", "--no-walk=unsorted", "--format=%H%x00%h%x00%s",
     "HEAD", "HEAD~", "origin/main", "HEAD"],
    ["log", "--no-walk", "--format=%h %s", "HEAD^{commit}", "--"],
    ["log", "--no-walk", "--format=%s", "origin/main^{commit}", "--"],
    ["reflog", "--format=%gd%x00%aN%x00%at%x00%gs", "refs/stash"],
    # Variations.
    ["reflog", "show", "--format=%h %gd %gs%n", "refs/stash"],
    ["reflog", "--format=%gd", "refs/heads/no-such"],
    ["log", "--format=%H %an <%ae> %aN <%aE> %at %ct%n%d", "-n30"],
    ["log", "--format=%h|%D", "--decorate", "--all", "-n5"],
    ["log", "--format=%h|%D", "--decorate=full", "-n40", "wt-branch",
     "feature", "^v1.0"],
    ["log", "--format=format:%h %s", "-n3"],
    ["log", "--pretty=tformat:%s", "-3"],
    ["log", "--format=%s", "--max-count=2", "side..main"],
    ["for-each-ref", "--format=%(refname:short)|%(objectname:short)|"
     "%(objecttype)|%(HEAD)|%(upstream)|%(upstream:trackshort)"],
    ["for-each-ref", "--format=%(refname)", "refs/heads", "refs/tags/"],
    ["for-each-ref"],
    ["rev-parse", "--verify", "HEAD"],
    ["rev-parse", "--verify", "-q", "no-such-ref"],
    ["rev-parse", "--verify", "no-such-ref"],
    ["rev-parse", "--short", "HEAD"],
    ["rev-parse", "--short=12", "HEAD~2"],
    ["rev-parse", "--verify", "v1.0"],
    ["rev-parse", "--verify", "dup"],
    ["rev-parse", "--verify", "HEAD~10"],
    ["rev-parse", "--show-prefix"],
    ["rev-parse", "--absolute-git-dir"],
    ["rev-parse", "--is-inside-work-tree"],
    ["rev-parse", "--git-dir"],
    ["config", "-z", "--get-all", "foo.multi"],
    ["config", "-z", "--get-all", "FOO.Multi"],
    ["config", "--get", "foo.both"],
    ["config", "-z", "--get-all", "core.preloadIndex"],
    ["config", "-z", "--get-all", "no.such"],
    ["config", "--list"],
    ["config", "-z", "--get-all", "section.SubSection With Case.key"],
    # Must be declined.
    ["status", "--porcelain"],
    ["log", "--graph", "--format=%h"],
    ["rev-parse", "HEAD..main"],
]


def git(cwd, args):
    p = subprocess.run(["git"] + args, cwd=cwd, env=ENV, capture_output=True)
    return p.returncode, p.stdout


def main():
    sysconfig = subprocess.run(["git", "var", "GIT_CONFIG_SYSTEM"], env=ENV,
                               capture_output=True, text=True).stdout.strip()
    helper = subprocess.Popen([HELPER, f"--system-config={sysconfig}"],
                              stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                              env=ENV)

    def ask(cwd, args):
        helper.stdin.write(("\t".join([cwd] + args) + "\n").encode())
        helper.stdin.flush()
        code, size = helper.stdout.readline().split()
        body = helper.stdout.read(int(size)) if int(size) else b""
        return int(code), body

    fails = declines = matches = 0
    times = []
    for dname, cwd in DIRS.items():
        for case in CASES:
            args = GLOBAL + case
            t0 = time.perf_counter()
            hcode, hout = ask(cwd, args)
            times.append(time.perf_counter() - t0)
            gcode, gout = git(cwd, args)
            label = f"[{dname}] git {' '.join(case)}"
            if times[-1] > 0.02:
                print(f"slow      {label}: {1000 * times[-1]:.0f} ms")
            if hcode == -1:
                declines += 1
                print(f"declined  {label}\n          {hout.decode()}")
            elif (hcode, hout) == (gcode, gout):
                matches += 1
            else:
                fails += 1
                print(f"MISMATCH  {label}")
                print(f"  helper exit {hcode}: {hout!r}")
                print(f"  git    exit {gcode}: {gout!r}")
    helper.stdin.close()
    helper.wait()
    times.sort()
    print(f"\n{matches} identical, {declines} declined, {fails} mismatches; "
          f"helper median {1000 * times[len(times) // 2]:.2f} ms, "
          f"max {1000 * times[-1]:.2f} ms")
    sys.exit(1 if fails else 0)


main()
