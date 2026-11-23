/* jl-git-helper.c --- answer a few read-only git commands from libgit2
 *
 * A long-running helper for Magit (see jl-magit-git-helper.el).  Starting
 * a git process costs ~100 ms on Windows; most of the git calls in a
 * Magit status refresh are tiny metadata lookups.  This program answers
 * those from libgit2 inside one process that Emacs starts once.
 *
 * Protocol (stdin/stdout, binary):
 *   request:  CWD \t ARG \t ARG ... \n
 *             ARGs are exactly what would follow "git" on the command
 *             line, global options (--no-pager, -c k=v, ...) included.
 *   response: "<exit-code> <byte-count>\n" followed by exactly that many
 *             bytes: what `git ARGS' run in CWD prints on stdout.
 *             Exit code -1 means "not emulated"; the body then holds the
 *             reason, and the caller runs git instead.
 *
 * Everything not known to be reproduced byte for byte is refused.
 * Emulated:
 *   rev-parse     --show-toplevel --show-cdup --show-prefix --git-dir
 *                 --absolute-git-dir --is-bare-repository
 *                 --is-inside-work-tree --verify --quiet --short[=N], revs
 *   for-each-ref  --format with refname[:short] objectname[:short]
 *                 objecttype symref upstream[:short|:track|:trackshort]
 *                 HEAD; --include-root-refs; prefix patterns
 *   log           --format/--pretty=[t]format: with %H %h %s %an %aN %ae
 *                 %aE %at %ct %D %d %n %% %xNN; -n; --no-walk[=..];
 *                 --decorate[=full|short]; --use-mailmap; A..B, ^A
 *   worktree list --porcelain [-z]
 *   config        --list [-z]; [--local] [-z] --get[-all] KEY
 *   reflog        [show] --format=FMT refs/NAME   (FMT as log, plus %gd %gs)
 */

#ifndef _WIN32
#define _DEFAULT_SOURCE
#define _DARWIN_C_SOURCE
#endif

#include <git2.h>
#include <git2/sys/errors.h> /* git_error_clear (moved here in libgit2 1.8) */

#include <ctype.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600 /* GetFinalPathNameByHandleW */
#endif
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#define UNSUPPORTED (-1)

/* ------------------------------------------------------------------ */
/* Growable byte buffer                                                */

typedef struct {
  char *p;
  size_t n, cap;
} buf;

static void die_oom(void) {
  fputs("jl-git-helper: out of memory\n", stderr);
  exit(2);
}

static void b_add(buf *b, const void *s, size_t n) {
  if (b->n + n + 1 > b->cap) {
    size_t c = b->cap ? b->cap : 256;
    while (c < b->n + n + 1) c *= 2;
    char *q = realloc(b->p, c);
    if (!q) die_oom();
    b->p = q;
    b->cap = c;
  }
  memcpy(b->p + b->n, s, n);
  b->n += n;
  b->p[b->n] = 0;
}
static void b_puts(buf *b, const char *s) { b_add(b, s, strlen(s)); }
static void b_putc(buf *b, char c) { b_add(b, &c, 1); }
static void b_reset(buf *b) {
  b->n = 0;
  if (b->p) b->p[0] = 0;
}
static void b_printf(buf *b, const char *fmt, ...) {
  char tmp[512];
  va_list ap;
  va_start(ap, fmt);
  int k = vsnprintf(tmp, sizeof tmp, fmt, ap);
  va_end(ap);
  if (k < 0) return;
  if ((size_t)k < sizeof tmp) {
    b_add(b, tmp, (size_t)k);
    return;
  }
  char *big = malloc((size_t)k + 1);
  if (!big) die_oom();
  va_start(ap, fmt);
  vsnprintf(big, (size_t)k + 1, fmt, ap);
  va_end(ap);
  b_add(b, big, (size_t)k);
  free(big);
}

/* ------------------------------------------------------------------ */
/* Per-request allocations, all freed when the request is answered     */

static void **g_allocs;
static size_t g_nallocs, g_capallocs;

static void *amalloc(size_t n) {
  void *p = malloc(n ? n : 1);
  if (!p) die_oom();
  if (g_nallocs == g_capallocs) {
    g_capallocs = g_capallocs ? 2 * g_capallocs : 64;
    g_allocs = realloc(g_allocs, g_capallocs * sizeof *g_allocs);
    if (!g_allocs) die_oom();
  }
  g_allocs[g_nallocs++] = p;
  return p;
}
static char *astrndup(const char *s, size_t n) {
  char *p = amalloc(n + 1);
  memcpy(p, s, n);
  p[n] = 0;
  return p;
}
static char *astrdup(const char *s) { return astrndup(s, strlen(s)); }
static char *acat3(const char *a, const char *b, const char *c) {
  size_t la = strlen(a), lb = strlen(b), lc = strlen(c);
  char *p = amalloc(la + lb + lc + 1);
  memcpy(p, a, la);
  memcpy(p + la, b, lb);
  memcpy(p + la + lb, c, lc);
  p[la + lb + lc] = 0;
  return p;
}
/* Take ownership of a malloc'ed string. */
static char *aown(char *s) {
  if (!s) return NULL;
  char *p = astrdup(s);
  free(s);
  return p;
}
/* Room for element N of an arena array V with capacity *CAP. */
static void *agrow(void *v, size_t n, size_t *cap, size_t elsz) {
  if (n < *cap) return v;
  *cap = *cap ? 2 * *cap : 16;
  void *nv = amalloc(*cap * elsz);
  if (n) memcpy(nv, v, n * elsz);
  return nv;
}
static void afree_all(void) {
  for (size_t i = 0; i < g_nallocs; i++) free(g_allocs[i]);
  g_nallocs = 0;
}

/* ------------------------------------------------------------------ */
/* Small string helpers                                                */

static int ascii_casecmp(const char *a, const char *b) {
  for (;; a++, b++) {
    int ca = tolower((unsigned char)*a), cb = tolower((unsigned char)*b);
    if (ca != cb) return ca - cb;
    if (!ca) return 0;
  }
}
static int starts_with(const char *s, const char *prefix) {
  return !strncmp(s, prefix, strlen(prefix));
}
static int ends_with(const char *s, const char *suffix) {
  size_t ls = strlen(s), lx = strlen(suffix);
  return ls >= lx && !strcmp(s + ls - lx, suffix);
}
/* Drop trailing slashes, but keep "/" and "C:/". */
static char *strip_slash(char *s) {
  size_t n = strlen(s);
  while (n > 1 && s[n - 1] == '/' && !(n == 3 && s[1] == ':')) s[--n] = 0;
  return s;
}
static void rtrim(char *s) {
  size_t n = strlen(s);
  while (n && isspace((unsigned char)s[n - 1])) s[--n] = 0;
}
static int parse_int(const char *s, long *out) {
  char *end;
  if (!*s) return 0;
  long v = strtol(s, &end, 10);
  if (*end) return 0;
  *out = v;
  return 1;
}

static buf *g_why;
static int unsup(const char *fmt, ...) {
  char tmp[512];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(tmp, sizeof tmp, fmt, ap);
  va_end(ap);
  b_reset(g_why);
  b_puts(g_why, tmp);
  return UNSUPPORTED;
}
static int lg_unsup(const char *what) {
  const git_error *e = git_error_last();
  return unsup("%s: %s", what, e && e->message ? e->message : "libgit2 error");
}

/* ------------------------------------------------------------------ */
/* File system, UTF-8 paths on all platforms                           */

typedef void (*dir_cb)(const char *name, void *data);

#ifdef _WIN32
static wchar_t *u8_to_w(const char *s) {
  int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
  if (n <= 0) return NULL;
  wchar_t *w = malloc((size_t)n * sizeof *w);
  if (!w) die_oom();
  MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
  return w;
}
static char *w_to_u8(const wchar_t *w) {
  int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
  if (n <= 0) return NULL;
  char *s = malloc((size_t)n);
  if (!s) die_oom();
  WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL);
  return s;
}
static void slashify(char *s) {
  for (; *s; s++)
    if (*s == '\\') *s = '/';
}
static FILE *u8_fopen(const char *path, const char *mode) {
  wchar_t *wp = u8_to_w(path), *wm = u8_to_w(mode);
  FILE *f = (wp && wm) ? _wfopen(wp, wm) : NULL;
  free(wp);
  free(wm);
  return f;
}
static int u8_exists(const char *path) {
  wchar_t *wp = u8_to_w(path);
  DWORD a = wp ? GetFileAttributesW(wp) : INVALID_FILE_ATTRIBUTES;
  free(wp);
  return a != INVALID_FILE_ATTRIBUTES;
}
/* Like Git for Windows' strbuf_realpath: the final, normalized path. */
static char *u8_realpath(const char *path) {
  wchar_t *wp = u8_to_w(path);
  if (!wp) return NULL;
  HANDLE h = CreateFileW(wp, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                         NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
  free(wp);
  if (h == INVALID_HANDLE_VALUE) return NULL;
  static wchar_t w[32768];
  DWORD n = GetFinalPathNameByHandleW(h, w, 32768, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
  CloseHandle(h);
  if (n == 0 || n >= 32768) return NULL;
  wchar_t *p = w;
  if (!wcsncmp(p, L"\\\\?\\UNC\\", 8)) {
    p += 6;
    *p = L'\\';
  } else if (!wcsncmp(p, L"\\\\?\\", 4)) {
    p += 4;
  }
  char *s = w_to_u8(p);
  if (s) slashify(s);
  return aown(s);
}
static int u8_listdir(const char *dir, dir_cb cb, void *data) {
  char *pat = acat3(dir, "/*", "");
  wchar_t *wp = u8_to_w(pat);
  if (!wp) return -1;
  WIN32_FIND_DATAW fd;
  HANDLE h = FindFirstFileW(wp, &fd);
  free(wp);
  if (h == INVALID_HANDLE_VALUE) return -1;
  do {
    if (wcscmp(fd.cFileName, L".") && wcscmp(fd.cFileName, L"..")) {
      char *n = w_to_u8(fd.cFileName);
      if (n) {
        cb(n, data);
        free(n);
      }
    }
  } while (FindNextFileW(h, &fd));
  FindClose(h);
  return 0;
}
static int is_absolute(const char *p) {
  return p[0] == '/' || (isalpha((unsigned char)p[0]) && p[1] == ':' && p[2] == '/');
}
#else
static FILE *u8_fopen(const char *path, const char *mode) { return fopen(path, mode); }
static int u8_exists(const char *path) {
  struct stat st;
  return stat(path, &st) == 0;
}
static char *u8_realpath(const char *path) { return aown(realpath(path, NULL)); }
static int u8_listdir(const char *dir, dir_cb cb, void *data) {
  DIR *d = opendir(dir);
  if (!d) return -1;
  struct dirent *e;
  while ((e = readdir(d)))
    if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) cb(e->d_name, data);
  closedir(d);
  return 0;
}
static int is_absolute(const char *p) { return p[0] == '/'; }
#endif

/* Whole (small) file, or NULL. */
static char *read_file(const char *path) {
  FILE *f = u8_fopen(path, "rb");
  if (!f) return NULL;
  buf b = {0};
  char tmp[4096];
  size_t k;
  while ((k = fread(tmp, 1, sizeof tmp, f)) > 0) b_add(&b, tmp, k);
  fclose(f);
  char *s = astrndup(b.p ? b.p : "", b.n);
  free(b.p);
  return s;
}

/* ------------------------------------------------------------------ */
/* Request context                                                     */

typedef struct {
  const char *name;
  const char *symref; /* target if symbolic, else NULL */
  git_oid oid;        /* resolved */
} refent;

typedef struct {
  refent *v;
  size_t n, cap;
} reflist;

static void rl_push(reflist *l, const char *name, const char *symref, const git_oid *oid) {
  l->v = agrow(l->v, l->n, &l->cap, sizeof *l->v);
  l->v[l->n].name = astrdup(name);
  l->v[l->n].symref = symref ? astrdup(symref) : NULL;
  l->v[l->n].oid = *oid;
  l->n++;
}
static int refent_cmp(const void *a, const void *b) {
  return strcmp(((const refent *)a)->name, ((const refent *)b)->name);
}

#define MAX_CFG_ARGS 32

typedef struct {
  git_repository *repo;
  git_config *cfg; /* snapshot */
  git_odb *odb;
  const char *cwd;    /* real path of the request directory */
  const char *top;    /* real work tree path; NULL if bare */
  const char *gitdir; /* no trailing slash */
  int abbrev;         /* 0 = not computed yet */
  int ncfg;
  const char *cfgk[MAX_CFG_ARGS], *cfgv[MAX_CFG_ARGS];
  int refs_loaded;
  reflist refs;  /* refs/..., sorted */
  reflist roots; /* HEAD, ORIG_HEAD, ... */
  int head_loaded;
  const char *head_symref; /* branch HEAD points to; NULL if detached */
  git_oid head_oid;
  int head_ok;         /* HEAD resolves to an object */
  int strict_shorten;  /* core.warnAmbiguousRefs; -1 = not read yet */
  git_mailmap *mailmap;
} ctx;

static void ctx_free(ctx *c) {
  if (c->mailmap) git_mailmap_free(c->mailmap);
  if (c->odb) git_odb_free(c->odb);
  if (c->cfg) git_config_free(c->cfg);
  if (c->repo) git_repository_free(c->repo);
}

static int cfg_bool(ctx *c, const char *key, int dflt) {
  int v;
  if (git_config_get_bool(&v, c->cfg, key) == 0) return v;
  git_error_clear();
  return dflt;
}
static int cfg_has(ctx *c, const char *key) {
  git_config_entry *e = NULL;
  int found = git_config_get_entry(&e, c->cfg, key) == 0;
  git_config_entry_free(e);
  git_error_clear();
  return found;
}

static void load_head(ctx *c) {
  if (c->head_loaded) return;
  c->head_loaded = 1;
  git_reference *h = NULL;
  if (git_reference_lookup(&h, c->repo, "HEAD") < 0) {
    git_error_clear();
    return;
  }
  if (git_reference_type(h) == GIT_REFERENCE_SYMBOLIC) {
    c->head_symref = astrdup(git_reference_symbolic_target(h));
    c->head_ok = git_reference_name_to_id(&c->head_oid, c->repo, c->head_symref) == 0;
    git_error_clear();
  } else if (git_reference_target(h)) {
    c->head_oid = *git_reference_target(h);
    c->head_ok = 1;
  }
  git_reference_free(h);
}

/* What HEAD resolves to: its branch, "HEAD" if detached, NULL if unborn. */
static const char *head_name(ctx *c) {
  return !c->head_ok ? NULL : c->head_symref ? c->head_symref : "HEAD";
}

static git_odb *get_odb(ctx *c) {
  if (!c->odb && git_repository_odb(&c->odb, c->repo) < 0) return NULL;
  return c->odb;
}

/* Root refs as `for-each-ref --include-root-refs' lists them
   (FETCH_HEAD and MERGE_HEAD are pseudorefs and excluded). */
static const char *root_ref_names[] = {
    "HEAD",          "AUTO_MERGE",          "BISECT_EXPECTED_REV", "BISECT_HEAD",
    "CHERRY_PICK_HEAD", "MERGE_AUTOSTASH",  "NOTES_MERGE_PARTIAL", "NOTES_MERGE_REF",
    "ORIG_HEAD",     "REBASE_HEAD",         "REVERT_HEAD",         NULL};

static int add_resolved(reflist *l, git_reference *r) {
  const char *name = git_reference_name(r);
  if (git_reference_type(r) == GIT_REFERENCE_SYMBOLIC) {
    git_reference *res = NULL;
    if (git_reference_resolve(&res, r) == 0) {
      rl_push(l, name, git_reference_symbolic_target(r), git_reference_target(res));
      git_reference_free(res);
      return 1;
    }
    git_error_clear(); /* broken symref: git skips it as well */
    return 0;
  }
  if (!git_reference_target(r)) return 0;
  rl_push(l, name, NULL, git_reference_target(r));
  return 1;
}

static int load_refs(ctx *c) {
  if (c->refs_loaded) return 0;
  git_reference_iterator *it;
  git_reference *r;
  int err;
  if (git_reference_iterator_new(&it, c->repo) < 0) return -1;
  while ((err = git_reference_next(&r, it)) == 0) {
    add_resolved(&c->refs, r);
    git_reference_free(r);
  }
  git_reference_iterator_free(it);
  if (err != GIT_ITEROVER) return -1;
  git_error_clear();
  qsort(c->refs.v, c->refs.n, sizeof *c->refs.v, refent_cmp);
  for (int i = 0; root_ref_names[i]; i++) {
    if (git_reference_lookup(&r, c->repo, root_ref_names[i]) == 0) {
      add_resolved(&c->roots, r);
      git_reference_free(r);
    }
    git_error_clear();
  }
  qsort(c->roots.v, c->roots.n, sizeof *c->roots.v, refent_cmp);
  c->refs_loaded = 1;
  return 0;
}

static const refent *find_ref(ctx *c, const char *name) {
  refent key = {name, NULL, {{0}}};
  const refent *e = bsearch(&key, c->refs.v, c->refs.n, sizeof key, refent_cmp);
  if (e) return e;
  for (size_t i = 0; i < c->roots.n; i++)
    if (!strcmp(c->roots.v[i].name, name)) return &c->roots.v[i];
  return NULL;
}

/* ------------------------------------------------------------------ */
/* Abbreviated object names, as git computes them                      */

static unsigned long be32(const unsigned char *p) {
  return ((unsigned long)p[0] << 24) | ((unsigned long)p[1] << 16) |
         ((unsigned long)p[2] << 8) | (unsigned long)p[3];
}

struct pack_count {
  const char *dir;
  unsigned long count;
};
static void count_pack(const char *name, void *data) {
  struct pack_count *pc = data;
  if (!ends_with(name, ".idx")) return;
  char *idx = acat3(pc->dir, "/", name);
  char *pack = astrdup(idx);
  strcpy(pack + strlen(pack) - 4, ".pack");
  if (!u8_exists(pack)) return;
  FILE *f = u8_fopen(idx, "rb");
  if (!f) return;
  unsigned char h[8 + 256 * 4];
  size_t k = fread(h, 1, sizeof h, f);
  fclose(f);
  if (k == sizeof h && !memcmp(h, "\377tOc", 4) && be32(h + 4) == 2)
    pc->count += be32(h + 8 + 255 * 4);
  else if (k >= 256 * 4 && memcmp(h, "\377tOc", 4))
    pc->count += be32(h + 255 * 4); /* version 1 */
}

/* git's repo_approximate_object_count: objects in packs.  (Packs covered
   by a multi-pack-index are counted per pack here, which can only
   matter right at a power-of-four boundary.) */
static unsigned long approx_object_count(ctx *c) {
  struct pack_count pc;
  pc.dir = acat3(git_repository_commondir(c->repo), "objects/pack", "");
  pc.count = 0;
  u8_listdir(pc.dir, count_pack, &pc);
  return pc.count;
}

static int abbrev_len(ctx *c) {
  if (c->abbrev) return c->abbrev;
  int len = -1;
  git_config_entry *e = NULL;
  if (git_config_get_entry(&e, c->cfg, "core.abbrev") == 0 && e->value) {
    long n;
    if (!ascii_casecmp(e->value, "auto"))
      len = -1;
    else if (!ascii_casecmp(e->value, "no") || !ascii_casecmp(e->value, "false") ||
             !ascii_casecmp(e->value, "off"))
      len = 40;
    else if (parse_int(e->value, &n))
      len = n < 4 ? 4 : n > 40 ? 40 : (int)n;
  }
  git_config_entry_free(e);
  git_error_clear();
  if (len < 0) {
    unsigned long count = approx_object_count(c);
    int bits = 0;
    while (count >>= 1) bits++; /* msb */
    len = (bits + 1 + 1) / 2;   /* DIV_ROUND_UP(msb + 1, 2) */
    if (len < 7) len = 7;
  }
  c->abbrev = len;
  return len;
}

/* Shortest unique prefix of at least MINLEN (0 = configured length). */
static const char *abbrev_oid(ctx *c, const git_oid *oid, int minlen) {
  char hex[GIT_OID_MAX_HEXSIZE + 1];
  git_oid_tostr(hex, sizeof hex, oid);
  int len = minlen > 0 ? minlen : abbrev_len(c);
  if (len < 4) len = 4;
  if (len < 40) {
    if (!get_odb(c)) {
      git_error_clear();
      return astrdup(hex);
    }
    while (len < 40) {
      git_oid tmp;
      int r = git_odb_exists_prefix(&tmp, c->odb, oid, (size_t)len);
      git_error_clear();
      if (r != GIT_EAMBIGUOUS) break;
      len++;
    }
  }
  return astrndup(hex, (size_t)(len > 40 ? 40 : len));
}

/* ------------------------------------------------------------------ */
/* Ref name shortening (refs_shorten_unambiguous_ref)                  */

static const char *rev_rules[6][2] = {
    {"", ""},           {"refs/", ""},          {"refs/tags/", ""},
    {"refs/heads/", ""}, {"refs/remotes/", ""}, {"refs/remotes/", "/HEAD"}};

static const char *shorten_ref_strict(ctx *c, const char *refname, int strict) {
  size_t rl = strlen(refname);
  for (int i = 5; i > 0; i--) {
    size_t pl = strlen(rev_rules[i][0]), sl = strlen(rev_rules[i][1]);
    if (rl <= pl + sl) continue;
    if (strncmp(refname, rev_rules[i][0], pl) || strcmp(refname + rl - sl, rev_rules[i][1]))
      continue;
    char *shortname = astrndup(refname + pl, rl - pl - sl);
    int fail = strict ? 6 : i, j;
    for (j = 0; j < fail; j++) {
      if (j == i) continue;
      if (find_ref(c, acat3(rev_rules[j][0], shortname, rev_rules[j][1]))) break;
    }
    if (j == fail) return shortname;
  }
  return refname;
}

/* As %(refname:short) does: strict unless core.warnAmbiguousRefs=false. */
static const char *shorten_ref(ctx *c, const char *refname) {
  if (c->strict_shorten < 0) c->strict_shorten = cfg_bool(c, "core.warnambiguousrefs", 1);
  return shorten_ref_strict(c, refname, c->strict_shorten);
}

/* Upstream of branch REFNAME (full name), or NULL. */
static const char *upstream_of(ctx *c, const char *refname) {
  if (!starts_with(refname, "refs/heads/")) return NULL;
  git_buf b = {0};
  const char *res = NULL;
  if (git_branch_upstream_name(&b, c->repo, refname) == 0) res = astrdup(b.ptr);
  git_buf_dispose(&b);
  git_error_clear();
  return res;
}

/* ------------------------------------------------------------------ */
/* rev-parse                                                           */

/* 0 on success, else the libgit2 error (GIT_ENOTFOUND, ...). */
static int resolve_rev(ctx *c, const char *spec, git_oid *out) {
  git_object *obj = NULL;
  int err = git_revparse_single(&obj, c->repo, spec);
  if (err < 0) {
    git_error_clear();
    return err;
  }
  *out = *git_object_id(obj);
  git_object_free(obj);
  return 0;
}

/* Full hex, or abbreviated to at least SHORTLEN (0 = configured) if >= 0. */
static void put_oid(ctx *c, buf *out, const git_oid *oid, int shortlen) {
  char hex[GIT_OID_MAX_HEXSIZE + 1];
  b_printf(out, "%s\n",
           shortlen >= 0 ? abbrev_oid(c, oid, shortlen) : git_oid_tostr(hex, sizeof hex, oid));
}

static int rev_ok_syntax(const char *s) {
  /* Leave ranges, paths and other special forms to git. */
  return *s && *s != '^' && *s != '-' && !strstr(s, "..") && !strchr(s, ':');
}

static int cmd_rev_parse(ctx *c, int n, char **a, buf *out) {
  int verify = 0, quiet = 0, shortlen = -1, nrevs = 0;
  const char *rev = NULL;
  for (int i = 0; i < n; i++) {
    const char *s = a[i];
    if (!strcmp(s, "--verify")) {
      verify = 1;
    } else if (!strcmp(s, "-q") || !strcmp(s, "--quiet")) {
      quiet = 1;
    } else if (!strcmp(s, "--short")) {
      verify = 1;
      shortlen = 0;
    } else if (starts_with(s, "--short=")) {
      long v;
      if (!parse_int(s + 8, &v) || v < 4) return unsup("rev-parse %s", s);
      verify = 1;
      shortlen = (int)v;
    } else if (!strcmp(s, "--show-toplevel")) {
      if (!c->top) return unsup("bare repository");
      b_printf(out, "%s\n", c->top);
    } else if (!strcmp(s, "--show-cdup") || !strcmp(s, "--show-prefix")) {
      if (!c->top) return unsup("bare repository");
      size_t tl = strlen(c->top);
      const char *rest;
      if (!strcmp(c->cwd, c->top))
        rest = "";
      else if (!strncmp(c->cwd, c->top, tl) && (c->cwd[tl] == '/' || c->top[tl - 1] == '/'))
        rest = c->cwd + tl + (c->cwd[tl] == '/');
      else
        return unsup("directory outside the work tree");
      if (s[7] == 'p') { /* --show-prefix */
        b_puts(out, rest);
        if (*rest) b_putc(out, '/');
      } else {
        if (*rest) {
          b_puts(out, "../");
          for (const char *p = rest; *p; p++)
            if (*p == '/') b_puts(out, "../");
        }
      }
      b_putc(out, '\n');
    } else if (!strcmp(s, "--git-dir")) {
      const char *dotgit = c->top ? acat3(c->top, "/.git", "") : NULL;
      if (c->top && !strcmp(c->cwd, c->top) && !strcmp(c->gitdir, dotgit))
        b_puts(out, ".git\n");
      else
        b_printf(out, "%s\n", c->gitdir);
    } else if (!strcmp(s, "--absolute-git-dir")) {
      b_printf(out, "%s\n", c->gitdir);
    } else if (!strcmp(s, "--is-bare-repository")) {
      b_puts(out, git_repository_is_bare(c->repo) ? "true\n" : "false\n");
    } else if (!strcmp(s, "--is-inside-work-tree")) {
      size_t tl = c->top ? strlen(c->top) : 0;
      int inside = c->top && (!strcmp(c->cwd, c->top) ||
                              (!strncmp(c->cwd, c->top, tl) && c->cwd[tl] == '/'));
      if (inside && starts_with(c->cwd, acat3(c->gitdir, "/", ""))) inside = 0;
      b_puts(out, inside ? "true\n" : "false\n");
    } else if (s[0] == '-') {
      return unsup("rev-parse option %s", s);
    } else {
      if (!rev_ok_syntax(s)) return unsup("rev-parse argument %s", s);
      if (verify) { /* printed at the end, as git does */
        if (nrevs++) return unsup("several revisions with --verify");
        rev = s;
        continue;
      }
      git_oid oid;
      if (resolve_rev(c, s, &oid) < 0) return unsup("cannot resolve %s", s);
      put_oid(c, out, &oid, -1);
    }
  }
  if (verify) {
    if (nrevs != 1) return unsup("--verify needs one revision");
    git_oid oid;
    int err = resolve_rev(c, rev, &oid);
    if (err == GIT_ENOTFOUND) return quiet ? 1 : 128;
    if (err < 0) return unsup("cannot resolve %s", rev);
    put_oid(c, out, &oid, shortlen);
  }
  return 0;
}

/* ------------------------------------------------------------------ */
/* for-each-ref                                                        */

enum {
  A_LIT,
  A_REFNAME,
  A_REFNAME_SHORT,
  A_OBJECTNAME,
  A_OBJECTNAME_SHORT,
  A_OBJECTTYPE,
  A_SYMREF,
  A_UPSTREAM,
  A_UPSTREAM_SHORT,
  A_UPSTREAM_TRACK,
  A_UPSTREAM_TRACKSHORT,
  A_HEAD
};

/* A format token: literal text (kind 0) or a placeholder (A_* or L_*). */
typedef struct {
  int kind;
  const char *lit;
  size_t litlen;
} tok;

static int hexval(int ch) {
  if (ch >= '0' && ch <= '9') return ch - '0';
  if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
  if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
  return -1;
}

/* Literal text, "%%" or a hex escape at P ("%xNN" if XESC, else "%NN").
   Return the characters used, or 0 if P starts a placeholder. */
static size_t literal_tok(const char *p, int xesc, tok *t) {
  if (*p != '%') {
    const char *q = p;
    while (*q && *q != '%') q++;
    *t = (tok){0, p, (size_t)(q - p)};
    return (size_t)(q - p);
  }
  if (p[1] == '%') {
    *t = (tok){0, "%", 1};
    return 2;
  }
  const char *h = p + 1 + (xesc != 0);
  if ((!xesc || p[1] == 'x') && hexval(h[0]) >= 0 && hexval(h[1]) >= 0) {
    char *ch = amalloc(1);
    *ch = (char)(hexval(h[0]) * 16 + hexval(h[1]));
    *t = (tok){0, ch, 1};
    return (size_t)(h + 2 - p);
  }
  return 0;
}

static int parse_ref_format(const char *fmt, tok **toks, int *ntok) {
  static const struct {
    const char *name;
    int atom;
  } atoms[] = {{"refname", A_REFNAME},
               {"refname:short", A_REFNAME_SHORT},
               {"objectname", A_OBJECTNAME},
               {"objectname:short", A_OBJECTNAME_SHORT},
               {"objecttype", A_OBJECTTYPE},
               {"symref", A_SYMREF},
               {"upstream", A_UPSTREAM},
               {"upstream:short", A_UPSTREAM_SHORT},
               {"upstream:track", A_UPSTREAM_TRACK},
               {"upstream:trackshort", A_UPSTREAM_TRACKSHORT},
               {"HEAD", A_HEAD},
               {NULL, 0}};
  tok *t = amalloc((strlen(fmt) + 1) * sizeof *t);
  int k = 0;
  for (const char *p = fmt; *p;) {
    size_t used = literal_tok(p, 0, &t[k]);
    if (used) {
      k++;
      p += used;
    } else if (p[1] == '(') {
      const char *end = strchr(p, ')');
      if (!end) return unsup("bad format");
      char *name = astrndup(p + 2, (size_t)(end - p - 2));
      int i;
      for (i = 0; atoms[i].name; i++)
        if (!strcmp(atoms[i].name, name)) break;
      if (!atoms[i].name) return unsup("for-each-ref atom %%(%s)", name);
      t[k++] = (tok){atoms[i].atom, NULL, 0};
      p = end + 1;
    } else { /* a lone '%' is literal for for-each-ref */
      const char *q = p + 1;
      while (*q && *q != '%') q++;
      t[k++] = (tok){A_LIT, p, (size_t)(q - p)};
      p = q;
    }
  }
  *toks = t;
  *ntok = k;
  return 0;
}

static int pattern_match(const char *pat, const char *name) {
  size_t l = strlen(pat);
  if (strncmp(name, pat, l)) return 0;
  return l && (pat[l - 1] == '/' || name[l] == '\0' || name[l] == '/');
}

static int cmd_for_each_ref(ctx *c, int n, char **a, buf *out) {
  const char *fmt = "%(objectname) %(objecttype)\t%(refname)";
  int roots = 0, dashdash = 0, npat = 0;
  const char **pats = amalloc((size_t)(n + 1) * sizeof *pats);
  for (int i = 0; i < n; i++) {
    const char *s = a[i];
    if (!dashdash && !strcmp(s, "--include-root-refs"))
      roots = 1;
    else if (!dashdash && starts_with(s, "--format="))
      fmt = s + 9;
    else if (!dashdash && !strcmp(s, "--format") && i + 1 < n)
      fmt = a[++i];
    else if (!dashdash && !strcmp(s, "--"))
      dashdash = 1;
    else if (!dashdash && s[0] == '-')
      return unsup("for-each-ref option %s", s);
    else if (strpbrk(s, "*?[\\"))
      return unsup("for-each-ref glob pattern");
    else
      pats[npat++] = s;
  }
  tok *toks;
  int ntok;
  if (parse_ref_format(fmt, &toks, &ntok) < 0) return UNSUPPORTED;
  if (load_refs(c) < 0) return lg_unsup("refs");
  load_head(c);
  if (roots && !c->head_ok) return unsup("unborn HEAD");

  /* Root refs (A-Z and _) sort before "refs/", so they come first. */
  size_t nroots = roots ? c->roots.n : 0, nrows = nroots + c->refs.n;
  const char *head = head_name(c);
  char hex[GIT_OID_MAX_HEXSIZE + 1];
  for (size_t r = 0; r < nrows; r++) {
    const refent *e = r < nroots ? &c->roots.v[r] : &c->refs.v[r - nroots];
    if (npat) {
      int ok = 0;
      for (int p = 0; p < npat && !ok; p++) ok = pattern_match(pats[p], e->name);
      if (!ok) continue;
    }
    int up_done = 0, ab_done = 0;
    const char *up = NULL;
    const refent *u = NULL;
    size_t ahead = 0, behind = 0;
    for (int t = 0; t < ntok; t++) {
      switch (toks[t].kind) {
        case A_LIT:
          b_add(out, toks[t].lit, toks[t].litlen);
          break;
        case A_REFNAME:
          b_puts(out, e->name);
          break;
        case A_REFNAME_SHORT:
          b_puts(out, shorten_ref(c, e->name));
          break;
        case A_OBJECTNAME:
          b_puts(out, git_oid_tostr(hex, sizeof hex, &e->oid));
          break;
        case A_OBJECTNAME_SHORT:
          b_puts(out, abbrev_oid(c, &e->oid, 0));
          break;
        case A_OBJECTTYPE: {
          size_t len;
          git_object_t type;
          if (!get_odb(c)) return lg_unsup("odb");
          if (git_odb_read_header(&len, &type, c->odb, &e->oid) < 0) return lg_unsup("object");
          b_puts(out, git_object_type2string(type));
          break;
        }
        case A_SYMREF:
          if (e->symref) b_puts(out, e->symref);
          break;
        case A_HEAD:
          b_putc(out, head && !strcmp(head, e->name) ? '*' : ' ');
          break;
        default: { /* upstream atoms */
          if (!up_done) {
            up = upstream_of(c, e->name);
            up_done = 1;
          }
          if (!up) break;
          int atom = toks[t].kind;
          if (atom == A_UPSTREAM) {
            b_puts(out, up);
          } else if (atom == A_UPSTREAM_SHORT) {
            b_puts(out, shorten_ref(c, up));
          } else {
            if (!ab_done) {
              ab_done = 1;
              u = find_ref(c, up);
              if (u && git_graph_ahead_behind(&ahead, &behind, c->repo, &e->oid, &u->oid) < 0)
                return lg_unsup("ahead-behind");
            }
            if (atom == A_UPSTREAM_TRACK) {
              if (!u)
                b_puts(out, "[gone]");
              else if (ahead && behind)
                b_printf(out, "[ahead %lu, behind %lu]", (unsigned long)ahead,
                         (unsigned long)behind);
              else if (ahead)
                b_printf(out, "[ahead %lu]", (unsigned long)ahead);
              else if (behind)
                b_printf(out, "[behind %lu]", (unsigned long)behind);
            } else if (u) { /* trackshort */
              b_puts(out, ahead && behind ? "<>" : ahead ? ">" : behind ? "<" : "=");
            }
          }
        }
      }
    }
    b_putc(out, '\n');
  }
  return 0;
}

/* ------------------------------------------------------------------ */
/* log                                                                 */

enum { D_HEAD, D_LOCAL, D_REMOTE, D_TAG, D_STASH };

typedef struct {
  git_oid oid;
  const char *name;
  int type;
} deco;

typedef struct {
  deco *v;
  size_t n, cap;
} decolist;

static void deco_push(decolist *l, const git_oid *oid, const char *name, int type) {
  l->v = agrow(l->v, l->n, &l->cap, sizeof *l->v);
  l->v[l->n].oid = *oid;
  l->v[l->n].name = name;
  l->v[l->n].type = type;
  l->n++;
}

/* Decorations in git's insertion order: refs sorted by name, then HEAD. */
static int load_decorations(ctx *c, decolist *dl) {
  if (load_refs(c) < 0) return lg_unsup("refs");
  load_head(c);
  if (cfg_has(c, "log.excludedecoration") || cfg_has(c, "log.initialdecorationset"))
    return unsup("decoration config");
  for (size_t i = 0; i < c->refs.n; i++) {
    const refent *e = &c->refs.v[i];
    int type;
    if (starts_with(e->name, "refs/heads/"))
      type = D_LOCAL;
    else if (starts_with(e->name, "refs/remotes/"))
      type = D_REMOTE;
    else if (starts_with(e->name, "refs/tags/"))
      type = D_TAG;
    else if (!strcmp(e->name, "refs/stash"))
      type = D_STASH;
    else if (starts_with(e->name, "refs/replace/"))
      return unsup("replace refs");
    else
      continue;
    deco_push(dl, &e->oid, e->name, type);
    if (type == D_TAG) {
      /* Annotated tags decorate what they point to as well. */
      git_oid cur = e->oid;
      for (int depth = 0; depth < 16; depth++) {
        git_tag *tag = NULL;
        size_t len;
        git_object_t ot;
        if (!get_odb(c)) return lg_unsup("odb");
        if (git_odb_read_header(&len, &ot, c->odb, &cur) < 0) return lg_unsup("tag");
        if (ot != GIT_OBJECT_TAG) break;
        if (git_tag_lookup(&tag, c->repo, &cur) < 0) return lg_unsup("tag");
        cur = *git_tag_target_id(tag);
        git_tag_free(tag);
        deco_push(dl, &cur, e->name, D_TAG);
      }
    }
  }
  if (c->head_ok) deco_push(dl, &c->head_oid, "HEAD", D_HEAD);
  return 0;
}

static const char *pretty_ref(const char *name, int full) {
  if (full) return name;
  if (starts_with(name, "refs/heads/")) return name + 11;
  if (starts_with(name, "refs/tags/")) return name + 10;
  if (starts_with(name, "refs/remotes/")) return name + 13;
  return name;
}

/* git's format_decorations, newest-added first, "HEAD -> branch". */
static void put_decorations(ctx *c, buf *out, const decolist *dl, const git_oid *oid, int full) {
  const deco *current = NULL;
  int has_head = 0;
  for (size_t i = dl->n; i-- > 0;)
    if (dl->v[i].type == D_HEAD && git_oid_equal(&dl->v[i].oid, oid)) has_head = 1;
  if (has_head && c->head_symref)
    for (size_t i = dl->n; i-- > 0;)
      if (dl->v[i].type == D_LOCAL && git_oid_equal(&dl->v[i].oid, oid) &&
          !strcmp(dl->v[i].name, c->head_symref)) {
        current = &dl->v[i];
        break;
      }
  const char *sep = "";
  for (size_t i = dl->n; i-- > 0;) {
    const deco *d = &dl->v[i];
    if (d == current || !git_oid_equal(&d->oid, oid)) continue;
    b_puts(out, sep);
    if (d->type == D_TAG) b_puts(out, "tag: ");
    b_puts(out, pretty_ref(d->name, full));
    if (current && d->type == D_HEAD) {
      b_puts(out, " -> ");
      b_puts(out, pretty_ref(current->name, full));
    }
    sep = ", ";
  }
}

/* git's format_subject: first paragraph, lines joined by spaces. */
static void put_subject(buf *out, const char *msg) {
  const char *p = msg;
  for (;;) { /* skip_blank_lines */
    const char *eol = strchr(p, '\n');
    const char *end = eol ? eol : p + strlen(p);
    const char *q = p;
    while (q < end && isspace((unsigned char)*q)) q++;
    if (q < end || !eol) break;
    p = eol + 1;
  }
  int first = 1;
  while (*p && *p != '\n') {
    const char *eol = strchr(p, '\n');
    size_t len = eol ? (size_t)(eol - p) : strlen(p);
    size_t t = len;
    while (t && isspace((unsigned char)p[t - 1])) t--;
    if (!t) break;
    if (!first) b_putc(out, ' ');
    b_add(out, p, t);
    first = 0;
    if (!eol) break;
    p = eol + 1;
  }
}

enum { L_LIT, L_H, L_h, L_s, L_an, L_aN, L_ae, L_aE, L_at, L_ct, L_D, L_d, L_gd, L_gs };

/* A parsed --format for log and reflog. */
typedef struct {
  tok *toks;
  int ntok;
  int tformat; /* terminator after each commit, else separator between */
  int full;    /* --decorate=full */
  decolist dl; /* filled when the format has %D or %d */
} logfmt;

/* The reflog entry a commit is shown for (reflog command only). */
typedef struct {
  const char *shortref; /* for %gd: "<shortref>@{<index>}" */
  size_t index;
  const char *message; /* %gs */
} reflog_sel;

/* --format=/--pretty= argument S: the format string, or NULL if refused. */
static const char *format_arg(const char *s, int *tformat) {
  const char *fmt = s + 9;
  *tformat = 1;
  if (starts_with(fmt, "format:")) {
    *tformat = 0;
    return fmt + 7;
  }
  if (starts_with(fmt, "tformat:")) return fmt + 8;
  if (!strchr(fmt, '%')) {
    unsup("named format %s", fmt);
    return NULL;
  }
  return fmt;
}

static int logfmt_init(ctx *c, logfmt *lf, const char *fmt, int reflog) {
  static const struct {
    const char *ph;
    int kind, reflog_only;
  } phs[] = {{"H", L_H, 0},   {"h", L_h, 0},   {"s", L_s, 0},   {"D", L_D, 0},
             {"d", L_d, 0},   {"an", L_an, 0}, {"aN", L_aN, 0}, {"ae", L_ae, 0},
             {"aE", L_aE, 0}, {"at", L_at, 0}, {"ct", L_ct, 0}, {"gd", L_gd, 1},
             {"gs", L_gs, 1}, {NULL, 0, 0}};
  tok *t = amalloc((strlen(fmt) + 1) * sizeof *t);
  int k = 0, need_deco = 0;
  for (const char *p = fmt; *p;) {
    size_t used = literal_tok(p, 1, &t[k]);
    if (used) {
      k++;
      p += used;
      continue;
    }
    if (p[1] == 'n') {
      t[k++] = (tok){L_LIT, "\n", 1};
      p += 2;
      continue;
    }
    int i;
    for (i = 0; phs[i].ph; i++)
      if (starts_with(p + 1, phs[i].ph) && (reflog || !phs[i].reflog_only)) break;
    if (!phs[i].ph) return unsup("format placeholder %%%.2s", p + 1);
    if (phs[i].kind == L_D || phs[i].kind == L_d) need_deco = 1;
    t[k++] = (tok){phs[i].kind, NULL, 0};
    p += 1 + strlen(phs[i].ph);
  }
  lf->toks = t;
  lf->ntok = k;
  if (need_deco && load_decorations(c, &lf->dl) < 0) return UNSUPPORTED;
  return 0;
}

static int peel_commit(ctx *c, const char *spec, git_oid *out) {
  git_object *obj = NULL, *peeled = NULL;
  if (git_revparse_single(&obj, c->repo, spec) < 0) return lg_unsup(spec);
  int err = git_object_peel(&peeled, obj, GIT_OBJECT_COMMIT);
  git_object_free(obj);
  if (err < 0) return lg_unsup(spec);
  *out = *git_object_id(peeled);
  git_object_free(peeled);
  return 0;
}

static int put_commit(ctx *c, buf *out, git_commit *cm, const logfmt *lf,
                      const reflog_sel *rs) {
  const char *enc = git_commit_message_encoding(cm);
  if (enc && ascii_casecmp(enc, "utf-8") && ascii_casecmp(enc, "utf8"))
    return unsup("commit encoding %s", enc);
  const git_signature *au = git_commit_author(cm);
  const char *mname = NULL, *mmail = NULL;
  char hex[GIT_OID_MAX_HEXSIZE + 1];
  for (int t = 0; t < lf->ntok; t++) {
    const tok *tk = &lf->toks[t];
    switch (tk->kind) {
      case L_LIT:
        b_add(out, tk->lit, tk->litlen);
        break;
      case L_H:
        b_puts(out, git_oid_tostr(hex, sizeof hex, git_commit_id(cm)));
        break;
      case L_h:
        b_puts(out, abbrev_oid(c, git_commit_id(cm), 0));
        break;
      case L_s:
        put_subject(out, git_commit_message_raw(cm));
        break;
      case L_at:
        b_printf(out, "%lld", (long long)au->when.time);
        break;
      case L_ct:
        b_printf(out, "%lld", (long long)git_commit_committer(cm)->when.time);
        break;
      case L_D:
        put_decorations(c, out, &lf->dl, git_commit_id(cm), lf->full);
        break;
      case L_d: {
        size_t before = out->n;
        b_puts(out, " (");
        put_decorations(c, out, &lf->dl, git_commit_id(cm), lf->full);
        if (out->n == before + 2)
          out->n = before, out->p[before] = 0; /* none: drop " (" */
        else
          b_putc(out, ')');
        break;
      }
      case L_gd:
        b_printf(out, "%s@{%lu}", rs->shortref, (unsigned long)rs->index);
        break;
      case L_gs:
        b_puts(out, rs->message);
        break;
      default: { /* author name/email */
        /* In user formats git maps %aN/%aE only, whatever --use-mailmap says. */
        int mapped = tk->kind == L_aN || tk->kind == L_aE;
        if (mapped && !mname) {
          if (!c->mailmap && git_mailmap_from_repository(&c->mailmap, c->repo) < 0)
            return lg_unsup("mailmap");
          if (git_mailmap_resolve(&mname, &mmail, c->mailmap, au->name, au->email) < 0)
            return lg_unsup("mailmap");
        }
        if (tk->kind == L_an || tk->kind == L_aN)
          b_puts(out, mapped ? mname : au->name);
        else
          b_puts(out, mapped ? mmail : au->email);
      }
    }
  }
  return 0;
}

/* One commit with its separator or terminator.  FIRST: no commit before. */
static int emit_commit(ctx *c, buf *out, git_commit *cm, const logfmt *lf, int first,
                       const reflog_sel *rs) {
  if (!lf->tformat && !first) b_putc(out, '\n');
  if (put_commit(c, out, cm, lf, rs) < 0) return UNSUPPORTED;
  if (lf->tformat) b_putc(out, '\n');
  return 0;
}

typedef struct {
  git_commit *commit;
  size_t idx;
} nwent;

static int nwent_cmp(const void *a, const void *b) {
  const nwent *x = a, *y = b;
  git_time_t tx = git_commit_time(x->commit), ty = git_commit_time(y->commit);
  if (tx != ty) return tx > ty ? -1 : 1;
  return x->idx < y->idx ? -1 : x->idx > y->idx;
}

/* log --no-walk: the given commits only, deduplicated, maybe by date. */
static int log_nowalk(ctx *c, buf *out, const logfmt *lf, const char **revs, int nrev,
                      int sorted, long maxcount) {
  nwent *ents = amalloc((size_t)(nrev + 1) * sizeof *ents);
  size_t ne = 0;
  int rc = 0;
  for (int i = 0; i < nrev && !rc; i++) {
    git_oid oid;
    if (revs[i][0] == '^' || strstr(revs[i], "..")) {
      rc = unsup("--no-walk with range");
      break;
    }
    if (peel_commit(c, revs[i], &oid) < 0) {
      rc = UNSUPPORTED;
      break;
    }
    int dup = 0;
    for (size_t j = 0; j < ne && !dup; j++) dup = git_oid_equal(git_commit_id(ents[j].commit), &oid);
    if (dup) continue;
    if (git_commit_lookup(&ents[ne].commit, c->repo, &oid) < 0) {
      rc = lg_unsup("commit");
      break;
    }
    ents[ne].idx = ne;
    ne++;
  }
  if (!rc && sorted) qsort(ents, ne, sizeof *ents, nwent_cmp);
  for (size_t i = 0; !rc && i < ne && (maxcount < 0 || (long)i < maxcount); i++)
    rc = emit_commit(c, out, ents[i].commit, lf, i == 0, NULL);
  for (size_t i = 0; i < ne; i++) git_commit_free(ents[i].commit);
  return rc;
}

/* Push or hide the commits of one revision argument (A, ^A, A..B). */
static int walk_add(ctx *c, git_revwalk *w, const char *s) {
  const char *dots = strstr(s, "..");
  git_oid oid;
  if (dots) {
    const char *left = dots == s ? "HEAD" : astrndup(s, (size_t)(dots - s));
    const char *right = dots[2] ? dots + 2 : "HEAD";
    if (peel_commit(c, left, &oid) < 0) return UNSUPPORTED;
    if (git_revwalk_hide(w, &oid) < 0) return lg_unsup("revwalk");
    s = right;
  } else if (s[0] == '^') {
    if (peel_commit(c, s + 1, &oid) < 0) return UNSUPPORTED;
    return git_revwalk_hide(w, &oid) < 0 ? lg_unsup("revwalk") : 0;
  }
  if (peel_commit(c, s, &oid) < 0) return UNSUPPORTED;
  return git_revwalk_push(w, &oid) < 0 ? lg_unsup("revwalk") : 0;
}

static int cmd_log(ctx *c, int n, char **a, buf *out) {
  const char *fmt = NULL;
  int tformat = 1, nowalk = 0, nowalk_sorted = 1, decorate = 0 /* 1 short, 2 full */,
      nrev = 0;
  long maxcount = -1;
  const char **revs = amalloc((size_t)(n + 1) * sizeof *revs);
  for (int i = 0; i < n; i++) {
    const char *s = a[i];
    long v;
    if (!strcmp(s, "--")) {
      if (i + 1 < n) return unsup("log with paths");
      break;
    } else if (starts_with(s, "--format=") || starts_with(s, "--pretty=")) {
      if (!(fmt = format_arg(s, &tformat))) return UNSUPPORTED;
    } else if (!strcmp(s, "--no-walk") || !strcmp(s, "--no-walk=sorted")) {
      nowalk = 1;
      nowalk_sorted = 1;
    } else if (!strcmp(s, "--no-walk=unsorted")) {
      nowalk = 1;
      nowalk_sorted = 0;
    } else if (!strcmp(s, "--decorate=full")) {
      decorate = 2;
    } else if (!strcmp(s, "--decorate") || !strcmp(s, "--decorate=short")) {
      decorate = 1;
    } else if (!strcmp(s, "--use-mailmap") || !strcmp(s, "--no-prefix") ||
               !strcmp(s, "--no-color") || !strcmp(s, "--color=never") ||
               !strcmp(s, "--no-show-signature")) {
      /* no effect on the output emulated here (%aN/%aE always use the mailmap) */
    } else if (starts_with(s, "-n") && s[2] && parse_int(s + 2, &v)) {
      maxcount = v;
    } else if (!strcmp(s, "-n") && i + 1 < n && parse_int(a[i + 1], &v)) {
      maxcount = v;
      i++;
    } else if (starts_with(s, "--max-count=") && parse_int(s + 12, &v)) {
      maxcount = v;
    } else if (s[0] == '-' && isdigit((unsigned char)s[1]) && parse_int(s + 1, &v)) {
      maxcount = v;
    } else if (s[0] == '-') {
      return unsup("log option %s", s);
    } else {
      if (strstr(s, "...")) return unsup("symmetric range");
      revs[nrev++] = s;
    }
  }
  if (!fmt) return unsup("log without --format");
  logfmt lf = {0};
  lf.tformat = tformat;
  lf.full = decorate == 2;
  if (logfmt_init(c, &lf, fmt, 0) < 0) return UNSUPPORTED;
  if (nowalk) return log_nowalk(c, out, &lf, revs, nrev, nowalk_sorted, maxcount);

  git_revwalk *w;
  if (git_revwalk_new(&w, c->repo) < 0) return lg_unsup("revwalk");
  git_revwalk_sorting(w, GIT_SORT_TIME);
  if (!nrev) revs[nrev++] = "HEAD";
  int rc = 0;
  for (int i = 0; i < nrev && !rc; i++) rc = walk_add(c, w, revs[i]);
  git_oid oid;
  for (long count = 0; !rc && (maxcount < 0 || count < maxcount) &&
                       git_revwalk_next(&oid, w) == 0;
       count++) {
    git_commit *cm;
    if (git_commit_lookup(&cm, c->repo, &oid) < 0) {
      rc = lg_unsup("commit");
      break;
    }
    rc = emit_commit(c, out, cm, &lf, count == 0, NULL);
    git_commit_free(cm);
  }
  git_error_clear();
  git_revwalk_free(w);
  return rc;
}

/* ------------------------------------------------------------------ */
/* reflog [show] --format=FMT refs/...  (Magit's stash list)           */

static int cmd_reflog(ctx *c, int n, char **a, buf *out) {
  const char *fmt = NULL, *ref = NULL;
  int tformat = 1, i = 0;
  if (n && !strcmp(a[0], "show")) i = 1;
  for (; i < n; i++) {
    if (starts_with(a[i], "--format=") || starts_with(a[i], "--pretty=")) {
      if (!(fmt = format_arg(a[i], &tformat))) return UNSUPPORTED;
    } else if (a[i][0] == '-') {
      return unsup("reflog option %s", a[i]);
    } else if (!ref) {
      ref = a[i];
    } else {
      return unsup("several reflogs");
    }
  }
  if (!fmt || !ref || !starts_with(ref, "refs/")) return unsup("reflog form");
  logfmt lf = {0};
  lf.tformat = tformat;
  if (logfmt_init(c, &lf, fmt, 1) < 0) return UNSUPPORTED;
  if (load_refs(c) < 0) return lg_unsup("refs");
  if (!find_ref(c, ref)) return unsup("no ref %s", ref);

  git_reflog *rl;
  if (git_reflog_read(&rl, c->repo, ref) < 0) return lg_unsup("reflog");
  size_t count = git_reflog_entrycount(rl);
  int rc = count ? 0 : unsup("empty reflog");
  reflog_sel rs;
  rs.shortref = shorten_ref_strict(c, ref, 0); /* git shortens it non-strictly */
  for (size_t k = 0; !rc && k < count; k++) {
    const git_reflog_entry *e = git_reflog_entry_byindex(rl, k);
    const char *msg = git_reflog_entry_message(e);
    char *m = astrdup(msg ? msg : "");
    size_t ml = strlen(m);
    while (ml && m[ml - 1] == '\n') m[--ml] = 0;
    rs.index = k;
    rs.message = m;
    git_commit *cm;
    if (git_commit_lookup(&cm, c->repo, git_reflog_entry_id_new(e)) < 0) {
      rc = lg_unsup("reflog commit");
      break;
    }
    rc = emit_commit(c, out, cm, &lf, k == 0, &rs);
    git_commit_free(cm);
  }
  git_reflog_free(rl);
  return rc;
}

/* ------------------------------------------------------------------ */
/* worktree list --porcelain                                           */

typedef struct {
  const char **names;
  size_t n, cap;
} namelist;

static void collect_name(const char *name, void *data) {
  namelist *l = data;
  l->names = agrow(l->names, l->n, &l->cap, sizeof *l->names);
  l->names[l->n++] = astrdup(name);
}

typedef struct {
  const char *dir, *gitfile, *path;
} wtent;

static int g_ignore_case;
static int wtent_cmp(const void *a, const void *b) {
  const char *x = ((const wtent *)a)->path, *y = ((const wtent *)b)->path;
  return g_ignore_case ? ascii_casecmp(x, y) : strcmp(x, y);
}

/* HEAD file → "HEAD <oid>" and "branch"/"detached" lines. */
static int put_head_lines(ctx *c, buf *out, const char *headfile, char term) {
  char *s = read_file(headfile);
  if (!s) return unsup("cannot read %s", headfile);
  rtrim(s);
  char hex[GIT_OID_MAX_HEXSIZE + 1];
  git_oid oid;
  if (starts_with(s, "ref: ")) {
    const char *target = s + 5;
    if (git_reference_name_to_id(&oid, c->repo, target) < 0) {
      git_error_clear();
      memset(&oid, 0, sizeof oid);
    }
    b_printf(out, "HEAD %s%c", git_oid_tostr(hex, sizeof hex, &oid), term);
    b_printf(out, "branch %s%c", target, term);
  } else {
    if (git_oid_fromstr(&oid, s) < 0) return unsup("odd HEAD in %s", headfile);
    b_printf(out, "HEAD %s%c", git_oid_tostr(hex, sizeof hex, &oid), term);
    b_printf(out, "detached%c", term);
  }
  return 0;
}

static int cmd_worktree(ctx *c, int n, char **a, buf *out) {
  if (n < 1 || strcmp(a[0], "list")) return unsup("worktree subcommand");
  int porcelain = 0, z = 0;
  for (int i = 1; i < n; i++) {
    if (!strcmp(a[i], "--porcelain"))
      porcelain = 1;
    else if (!strcmp(a[i], "-z"))
      z = 1;
    else
      return unsup("worktree list option %s", a[i]);
  }
  if (!porcelain) return unsup("worktree list without --porcelain");
  char term = z ? '\0' : '\n';

  char *common = strip_slash(astrdup(git_repository_commondir(c->repo)));
  char *mainpath = u8_realpath(common);
  if (!mainpath) return unsup("cannot resolve %s", common);
  if (ends_with(mainpath, "/.git")) mainpath[strlen(mainpath) - 5] = 0;
  int bare = cfg_bool(c, "core.bare", 0) || git_repository_is_bare(c->repo);
  b_printf(out, "worktree %s%c", mainpath, term);
  if (bare)
    b_printf(out, "bare%c", term);
  else if (put_head_lines(c, out, acat3(common, "/HEAD", ""), term) < 0)
    return UNSUPPORTED;
  b_putc(out, term);

  namelist names = {0};
  char *wtdir = acat3(common, "/worktrees", "");
  u8_listdir(wtdir, collect_name, &names);
  wtent *wts = amalloc((names.n + 1) * sizeof *wts);
  size_t nwt = 0;
  for (size_t i = 0; i < names.n; i++) {
    char *dir = acat3(wtdir, "/", names.names[i]);
    char *gitfile = read_file(acat3(dir, "/gitdir", ""));
    if (!gitfile) continue; /* git skips these too */
    rtrim(gitfile);
    if (!*gitfile) continue;
    if (!is_absolute(gitfile)) return unsup("relative worktree path");
    char *path = astrdup(gitfile);
    if (ends_with(path, "/.git")) path[strlen(path) - 5] = 0;
    wts[nwt].dir = dir;
    wts[nwt].gitfile = gitfile;
    wts[nwt].path = path;
    nwt++;
  }
  /* git sorts linked worktrees by path (fspathcmp), main one first. */
  g_ignore_case = cfg_bool(c, "core.ignorecase", 0);
  qsort(wts, nwt, sizeof *wts, wtent_cmp);
  for (size_t i = 0; i < nwt; i++) {
    const char *dir = wts[i].dir, *gitfile = wts[i].gitfile;
    b_printf(out, "worktree %s%c", wts[i].path, term);
    if (put_head_lines(c, out, acat3(dir, "/HEAD", ""), term) < 0) return UNSUPPORTED;
    char *lockfile = acat3(dir, "/locked", "");
    int locked = u8_exists(lockfile);
    if (locked) {
      char *reason = read_file(lockfile);
      if (!reason) return unsup("cannot read %s", lockfile);
      /* git trims the reason (strbuf_trim) */
      rtrim(reason);
      while (isspace((unsigned char)*reason)) reason++;
      if (*reason) {
        if (!z) return unsup("lock reason without -z");
        b_printf(out, "locked %s%c", reason, term);
      } else {
        b_printf(out, "locked%c", term);
      }
    } else if (!u8_exists(gitfile)) {
      b_printf(out, "prunable gitdir file points to non-existent location%c", term);
    }
    b_putc(out, term);
  }
  return 0;
}

/* ------------------------------------------------------------------ */
/* config                                                              */

/* git_config_parse_key: section and variable lower case. */
static char *canonical_key(const char *key) {
  char *k = astrdup(key);
  char *first = strchr(k, '.'), *last = strrchr(k, '.');
  if (!first) return k;
  for (char *p = k; p < first; p++) *p = (char)tolower((unsigned char)*p);
  for (char *p = last + 1; *p; p++) *p = (char)tolower((unsigned char)*p);
  return k;
}

static int cmd_config(ctx *c, int n, char **a, buf *out) {
  int z = 0, list = 0, getall = 0, get = 0, local = 0;
  const char *key = NULL;
  for (int i = 0; i < n; i++) {
    const char *s = a[i];
    if (!strcmp(s, "-z") || !strcmp(s, "--null"))
      z = 1;
    else if (!strcmp(s, "--list") || !strcmp(s, "-l"))
      list = 1;
    else if (!strcmp(s, "--get-all"))
      getall = 1;
    else if (!strcmp(s, "--get"))
      get = 1;
    else if (!strcmp(s, "--local"))
      local = 1;
    else if (!strcmp(s, "--include") || !strcmp(s, "--includes"))
      ; /* followed anyway; refused below if any include is present */
    else if (s[0] == '-')
      return unsup("config option %s", s);
    else if (!key)
      key = s;
    else
      return unsup("config value pattern");
  }
  if (cfg_bool(c, "extensions.worktreeconfig", 0)) return unsup("worktree config");
  char kdelim = z ? '\n' : '=', term = z ? '\0' : '\n';

  if (list) {
    if (key || local || get || getall) return unsup("config --list combination");
    git_config_iterator *it;
    git_config_entry *e;
    int err;
    if (git_config_iterator_new(&it, c->cfg) < 0) return lg_unsup("config");
    while ((err = git_config_next(&e, it)) == 0) {
      if (starts_with(e->name, "include.") || starts_with(e->name, "includeif.")) {
        git_config_iterator_free(it);
        return unsup("config includes");
      }
      b_puts(out, e->name);
      if (e->value) {
        b_putc(out, kdelim);
        b_puts(out, e->value);
      }
      b_putc(out, term);
    }
    git_config_iterator_free(it);
    if (err != GIT_ITEROVER) return lg_unsup("config");
    git_error_clear();
    for (int i = 0; i < c->ncfg; i++) {
      b_puts(out, canonical_key(c->cfgk[i]));
      if (c->cfgv[i]) {
        b_putc(out, kdelim);
        b_puts(out, c->cfgv[i]);
      }
      b_putc(out, term);
    }
    return 0;
  }

  if (!(get || getall) || !key || (get && getall)) return unsup("config form");
  git_config *cfg = c->cfg, *lvl = NULL;
  if (local) {
    if (git_config_open_level(&lvl, c->cfg, GIT_CONFIG_LEVEL_LOCAL) < 0) {
      git_error_clear();
      return 1; /* no local config: nothing found */
    }
    cfg = lvl;
  }
  /* --get-all prints values as found; --get only the last one.  Output
     already written is dropped if the request ends up refused. */
  const char *last = NULL;
  git_config_iterator *it;
  git_config_entry *e;
  int err;
  if (git_config_multivar_iterator_new(&it, cfg, key, NULL) < 0) {
    if (lvl) git_config_free(lvl);
    return lg_unsup("config");
  }
  while ((err = git_config_next(&e, it)) == 0) {
    if (!e->value) {
      git_config_iterator_free(it);
      if (lvl) git_config_free(lvl);
      return unsup("valueless config entry");
    }
    last = astrdup(e->value);
    if (getall) {
      b_puts(out, last);
      b_putc(out, term);
    }
  }
  git_config_iterator_free(it);
  if (lvl) git_config_free(lvl);
  if (err != GIT_ITEROVER) return lg_unsup("config");
  git_error_clear();
  if (!local) {
    const char *ck = canonical_key(key);
    for (int i = 0; i < c->ncfg; i++)
      if (!strcmp(canonical_key(c->cfgk[i]), ck)) {
        if (!c->cfgv[i]) return unsup("valueless -c entry");
        last = c->cfgv[i];
        if (getall) {
          b_puts(out, last);
          b_putc(out, term);
        }
      }
  }
  if (!last) return 1;
  if (get) {
    b_puts(out, last);
    b_putc(out, term);
  }
  return 0;
}

/* ------------------------------------------------------------------ */
/* Request dispatch                                                    */

/* -c options that leave the emulated outputs unchanged. */
static int allowed_c(const char *kv) {
  const char *eq = strchr(kv, '=');
  if (!eq) return 0;
  char *key = canonical_key(astrndup(kv, (size_t)(eq - kv)));
  const char *val = eq + 1;
  if (!strcmp(key, "core.preloadindex")) return 1;
  if (!strcmp(key, "log.showsignature")) return !ascii_casecmp(val, "false");
  if (!strcmp(key, "diff.noprefix")) return !ascii_casecmp(val, "false");
  if (!strcmp(key, "color.ui") || !strcmp(key, "color.diff"))
    return !ascii_casecmp(val, "false") || !ascii_casecmp(val, "never");
  if (!strcmp(key, "i18n.logoutputencoding"))
    return !ascii_casecmp(val, "utf-8") || !ascii_casecmp(val, "utf8");
  return 0;
}

static const struct {
  const char *name;
  int (*fn)(ctx *, int, char **, buf *);
} commands[] = {{"rev-parse", cmd_rev_parse}, {"for-each-ref", cmd_for_each_ref},
                {"log", cmd_log},             {"reflog", cmd_reflog},
                {"worktree", cmd_worktree},   {"config", cmd_config},
                {NULL, NULL}};

static int g_env_refused;

static int handle(int argc, char **argv, buf *out) {
  ctx c;
  memset(&c, 0, sizeof c);
  c.strict_shorten = -1;
  if (g_env_refused) return unsup("GIT_* environment set, or unusable --system-config");
  if (argc < 2) return unsup("empty request");
  int i = 1;
  for (; i < argc; i++) {
    const char *s = argv[i];
    if (!strcmp(s, "--no-pager") || !strcmp(s, "--literal-pathspecs") ||
        !strcmp(s, "--no-optional-locks"))
      continue;
    if (!strcmp(s, "-c")) {
      if (i + 1 >= argc || !allowed_c(argv[i + 1]))
        return unsup("-c %s", i + 1 < argc ? argv[i + 1] : "");
      if (c.ncfg == MAX_CFG_ARGS) return unsup("too many -c");
      const char *kv = argv[++i], *eq = strchr(kv, '=');
      c.cfgk[c.ncfg] = astrndup(kv, (size_t)(eq - kv));
      c.cfgv[c.ncfg] = eq + 1;
      c.ncfg++;
      continue;
    }
    if (s[0] == '-') return unsup("global option %s", s);
    break;
  }
  if (i >= argc) return unsup("no command");
  const char *cmd = argv[i];
  int n = argc - i - 1;
  char **a = argv + i + 1;
  int ci;
  for (ci = 0; commands[ci].name && strcmp(commands[ci].name, cmd); ci++)
    ;
  if (!commands[ci].name) return unsup("command %s", cmd);

  c.cwd = u8_realpath(argv[0]);
  if (!c.cwd) return unsup("cannot resolve %s", argv[0]);
  int rc;
  if (git_repository_open_ext(&c.repo, c.cwd, 0, NULL) < 0) {
    rc = lg_unsup("open");
    goto done;
  }
  if (git_repository_config_snapshot(&c.cfg, c.repo) < 0) {
    rc = lg_unsup("config");
    goto done;
  }
  const char *wd = git_repository_workdir(c.repo);
  c.top = wd ? strip_slash(astrdup(wd)) : NULL;
  c.gitdir = strip_slash(astrdup(git_repository_path(c.repo)));
  /* Inside the git directory (or a bare repository) git sets up a
     different context (no work tree, no .mailmap, "." as git dir). */
  if (!strcmp(c.cwd, c.gitdir) || starts_with(c.cwd, acat3(c.gitdir, "/", ""))) {
    rc = unsup("inside the git directory");
    goto done;
  }

  rc = commands[ci].fn(&c, n, a, out);
done:
  ctx_free(&c);
  git_error_clear();
  return rc;
}

static int read_line(buf *line) {
  b_reset(line);
  int ch;
  while ((ch = getchar()) != EOF) {
    if (ch == '\n') return 1;
    b_putc(line, (char)ch);
  }
  return line->n > 0;
}

static void usage(void) {
  fputs("usage: jl-git-helper --system-config=PATH\n"
        "  PATH is what `git var GIT_CONFIG_SYSTEM' prints for the git\n"
        "  this helper stands in for.  Requests come on stdin; see the\n"
        "  comment at the top of jl-git-helper.c.\n",
        stderr);
  exit(2);
}

int main(int argc, char **argv) {
#ifdef _WIN32
  _setmode(_fileno(stdin), _O_BINARY);
  _setmode(_fileno(stdout), _O_BINARY);
#endif
  static const char *refused[] = {"GIT_DIR", "GIT_WORK_TREE", "GIT_COMMON_DIR",
                                  "GIT_INDEX_FILE", "GIT_OBJECT_DIRECTORY",
                                  "GIT_ALTERNATE_OBJECT_DIRECTORIES", "GIT_NAMESPACE",
                                  "GIT_CONFIG", "GIT_CONFIG_PARAMETERS", "GIT_CONFIG_COUNT",
                                  "GIT_CONFIG_SYSTEM", "GIT_CONFIG_GLOBAL",
                                  "GIT_CONFIG_NOSYSTEM", "GIT_CEILING_DIRECTORIES",
                                  "GIT_REPLACE_REF_BASE", NULL};
  for (int i = 0; refused[i]; i++)
    if (getenv(refused[i])) g_env_refused = 1;

  git_libgit2_init();

  /* libgit2 must read the same system config file as git.  It looks for
     "gitconfig" in a search path, so point that at git's directory. */
  const char *sysconfig = NULL;
  for (int i = 1; i < argc; i++) {
    if (starts_with(argv[i], "--system-config="))
      sysconfig = argv[i] + 16;
    else
      usage();
  }
  if (!sysconfig) usage();
  {
    char *dir = strdup(sysconfig);
    if (!dir) die_oom();
#ifdef _WIN32
    slashify(dir);
#endif
    char *slash = strrchr(dir, '/');
    if (!slash || strcmp(slash + 1, "gitconfig") ||
        git_libgit2_opts(GIT_OPT_SET_SEARCH_PATH, GIT_CONFIG_LEVEL_SYSTEM,
                         (*dir == '/' && slash == dir) ? "/" : (*slash = 0, dir)) < 0)
      g_env_refused = 1; /* cannot match git's config: answer nothing */
    free(dir);
  }
  buf line = {0}, out = {0}, why = {0};
  g_why = &why;
  char **args = NULL;
  size_t argcap = 0;
  while (read_line(&line)) {
    size_t nargs = 1;
    for (size_t i = 0; i < line.n; i++)
      if (line.p[i] == '\t') nargs++;
    if (nargs + 1 > argcap) {
      argcap = nargs + 16;
      args = realloc(args, argcap * sizeof *args);
      if (!args) die_oom();
    }
    size_t k = 0;
    args[k++] = line.p ? line.p : (char *)"";
    for (size_t i = 0; i < line.n; i++)
      if (line.p[i] == '\t') {
        line.p[i] = 0;
        args[k++] = line.p + i + 1;
      }
    b_reset(&out);
    b_reset(&why);
    int rc = handle((int)k, args, &out);
    const buf *body = rc == UNSUPPORTED ? &why : &out;
    printf("%d %lu\n", rc, (unsigned long)body->n);
    if (body->n) fwrite(body->p, 1, body->n, stdout);
    fflush(stdout);
    afree_all();
  }
  git_libgit2_shutdown();
  return 0;
}
