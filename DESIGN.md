# Design Notes

Internal notes on how Mini-UnionFS is put together and why. The README covers
usage; this covers the reasoning behind the trickier parts.

## State

Everything the filesystem needs is two absolute paths, held in one struct and
handed to FUSE as `private_data`:

```c
struct mini_unionfs_state {
    char *lower_dir;
    char *upper_dir;
};
```

`main.c` resolves both with `realpath()` before mounting so that every
callback can build absolute paths by straight concatenation
(`upper_dir + path`) without worrying about `..` or relative-path weirdness
creeping in from the CLI args. No per-file-handle state is needed beyond
that — `unionfs_create` stashes the open fd in `fi->fh` so `release` can
close it, but reads/writes elsewhere just reopen the resolved path each
call. Simpler than tracking a global fd table, and FUSE's own kernel-side
caching means it's not a hot path.

## Path resolution

`resolve_path()` in `path.c` is the one function everything else depends on.
Given a virtual path like `/subdir/file.txt`, it checks, in order:

1. Is there a whiteout marker (`upper_dir/subdir/.wh.file.txt`)? → `ENOENT`.
2. Does `upper_dir/subdir/file.txt` exist? → serve from upper.
3. Does `lower_dir/subdir/file.txt` exist? → serve from lower.
4. Otherwise → `ENOENT`.

That ordering is the whole union semantics in four lines. Upper always wins
over lower, and a whiteout always wins over both. Every read-only callback
(`getattr`, `read`) funnels through this. The write-side callbacks
(`open`, `write`, `create`, `truncate`, ...) don't call it directly because
they need to distinguish "exists in upper" from "exists in lower" rather
than just getting back a single resolved path — so they do their own
`access()` checks against both directories. It's some duplication, but
trying to force both cases through one function made the write path harder
to follow than it was worth.

## Copy-on-Write

`cow_copy()` in `rw_ops.c` does the actual copy: open the lower file, stat
it for the mode bits, `mkdir -p` the destination directory in upper, then
copy in 64KB chunks with a `while (written < bytes)` inner loop so a short
`write()` doesn't silently drop data. Straightforward, no fancy sendfile/
splice tricks — this isn't meant to be fast, it's meant to be obviously
correct.

The part that's easy to get wrong is *when* to trigger it. `open()` is
where it happens: if the file is being opened for writing and it exists in
lower but not upper, copy it first, then let the actual `write()` calls
land on the upper copy. `truncate()`, `chmod()`, and `chown()` need the same
check independently, because none of them necessarily go through `open()`
first (e.g. `truncate(2)` on a path, or a shell `> file` that truncates
without ever calling read/write).

One gotcha that cost some debugging time: `(fi->flags & O_ACCMODE) !=
O_RDONLY` is not the same as `fi->flags & (O_WRONLY | O_RDWR)`.
`O_RDONLY` is `0`, so a plain `flags & O_WRONLY` bitwise check misses
`O_RDWR` opens depending on flag values. Masking with `O_ACCMODE` first is
the only reliable way to check "is this open for writing" across
`O_WRONLY`/`O_RDWR`.

## Whiteouts

Deleting `lower_dir/config.txt` through the mount can't touch
`lower_dir` — it's supposed to stay read-only — so `unionfs_unlink`
creates `upper_dir/.wh.config.txt` instead: a zero-length file with mode
`0000`. `resolve_path` and `readdir` both check for this marker before
anything else. The `0000` permissions aren't load-bearing (nothing tries to
open a whiteout as data), it's just a signal that this file isn't meant to
be read.

Three cases `unionfs_unlink` has to handle:

- **Upper-only file** (created after mount, never existed in lower): just
  `unlink()` it for real. No whiteout needed since there's nothing in lower
  to hide.
- **Lower-only file**: write the whiteout marker.
- **Lower file that's been CoW'd into upper**: write the whiteout marker
  *and* remove the upper copy, otherwise the copy would just sit there
  orphaned.

Re-creating a file after deleting it (`rm file; touch file`) needs to clear
the stale whiteout, or the new file would be immediately invisible again.
`unionfs_create` and `unionfs_mkdir` both check for and remove a matching
whiteout before creating.

## Directory listing

`readdir` does two passes: upper first, then lower. A small linked-list
"seen" set (allocated fresh per call — sharing one across concurrent
`readdir`s would be a race) tracks names already emitted so the lower pass
skips anything upper already provided, and separately checks
`has_whiteout()` to skip anything that was deleted. `.wh.*` entries
themselves are filtered from both passes so they never show up in a
listing.

## Directories that only exist in lower

`mkdir` always goes to upper — there's no reason to write to lower. But
`rmdir` on a directory that exists only in lower returns `EPERM` rather
than doing anything. A proper implementation would whiteout the whole
directory (hiding every path under it, lower and upper alike), but that's
a bigger feature than "touch a marker file" and isn't implemented here.
`EPERM` was picked over `ENOENT` because the directory *is* there, it's
just not removable — matches how a read-only-mount `rmdir` typically fails.

## What's deliberately not handled

- No `.wh.` name collisions — a real file legitimately named `.wh.foo` in
  lower would get treated as a whiteout marker. Not something a stacked
  container filesystem needs to worry about in practice (this is the same
  convention overlayfs and Docker's graphdriver use), but worth knowing.
- No locking around concurrent writers to the same path — this is a
  learning project, not something meant to survive concurrent access from
  multiple processes.
- No support for more than two layers (a single lower + single upper).
  Docker-style multi-layer stacking would mean resolve_path walking a list
  instead of two fixed directories — the logic extends cleanly, it just
  isn't built.
- Hard links and symlinks CoW the target into upper the same way regular
  writes do, but there's no attempt to preserve hard-link identity across
  the copy (a hard-linked pair in lower becomes two independent files once
  one side is written to). Matches how CoW filesystems generally behave,
  just worth calling out explicitly.
