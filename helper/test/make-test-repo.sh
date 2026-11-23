#!/bin/sh
# Build a repository with the edge cases jl-git-helper has to get right.
# Usage: make-test-repo.sh DIR   (DIR must not exist)
# Uses DIR/home as HOME, so your own git config is neither read nor touched.
set -eu

T=$1
mkdir -p "$T/home"
T=$(cd "$T" && pwd -P)
export HOME="$T/home" XDG_CONFIG_HOME="$T/home/.config"
unset GIT_DIR GIT_WORK_TREE
git config --global user.name "Test User"
git config --global user.email test@example.com
git config --global init.defaultBranch main
git config --global foo.multi global-value
git config --global foo.both global

n=0
commit() { # commit FILE MESSAGE [author]
  n=$((n + 1))
  echo "$n" >> "$T/repo/$1"
  git -C "$T/repo" add "$1"
  if [ $# -ge 3 ]; then
    GIT_COMMITTER_DATE="2026-01-01T00:00:$(printf %02d $((n % 60)))Z" \
      git -C "$T/repo" commit -q --cleanup=verbatim --author="$3" -m "$2"
  else
    GIT_COMMITTER_DATE="2026-01-01T00:00:$(printf %02d $((n % 60)))Z" \
      git -C "$T/repo" commit -q --cleanup=verbatim -m "$2"
  fi
}

git init -q --bare "$T/origin.git"
git init -q "$T/repo"
cd "$T/repo"
git remote add origin "$T/origin.git"

commit a "Initial commit"
git tag -a v1.0 -m "Release 1.0"
commit a "Multi-line
subject continues
here

Body text"
git tag light
commit b "Unicode author" "Jöns Ångström <jons@example.com>"
printf 'Jonas Angstrom <jonas@example.com> <jons@example.com>\n' > .mailmap
git add .mailmap
commit a "
  leading blank line and indent"
commit a "Trailing spaces    "
for i in 1 2 3 4 5 6 7 8; do commit c "Filler $i"; done
git tag -a nested-inner -m inner
git tag -a nested-outer -m outer nested-inner
git push -q -u origin main
git push -q origin --tags
git remote set-head origin main

# Same committer timestamp on both sides of a merge.
git checkout -q -b side
GIT_COMMITTER_DATE="2026-01-02T00:00:00Z" commit d "Side commit"
git checkout -q main
GIT_COMMITTER_DATE="2026-01-02T00:00:00Z" commit c "Main commit, same time"
GIT_COMMITTER_DATE="2026-01-02T00:00:00Z" git merge -q --no-edit side

# Upstream moves on elsewhere: main is ahead 3 and behind 1.
git clone -q "$T/origin.git" "$T/other"
git -C "$T/other" commit -q --allow-empty -m "Commit on origin"
git -C "$T/other" push -q origin main
git fetch -q

# Branch whose upstream is gone, branch tracking a local branch,
# a tag with the same name as a branch.
git checkout -q -b feature
commit e "Feature work"
git push -q -u origin feature
git push -q origin --delete feature
git checkout -q -b local-tracking --track main
git checkout -q main
git branch dup
git tag dup HEAD~2

echo stash >> a
git stash -q
echo stash >> b
git stash -q -m "Second stash, custom message"

# Worktrees: on a branch, detached, locked with and without a reason,
# and one whose directory was removed (prunable).
git worktree add -q "$T/wt-branch" -b wt-branch
git worktree add -q --detach "$T/wt-detached" HEAD~3
git worktree add -q "$T/wt-locked" -b wt-locked
git worktree lock --reason "on a USB stick" "$T/wt-locked"
git worktree add -q "$T/wt-locked-plain" -b wt-locked-plain
git worktree lock "$T/wt-locked-plain"
git worktree add -q "$T/wt-gone" -b wt-gone
rm -rf "$T/wt-gone"

# Config at several levels.
git config foo.both local
git config --add foo.multi local-1
git config --add foo.multi local-2
git config status.showUntrackedFiles no
git config 'Section.SubSection With Case.Key' value
printf '[flags]\n\tvalueless\n' >> .git/config

mkdir -p sub/dir
ln -s "$T/repo" "$T/repo-link"
echo "$T"
