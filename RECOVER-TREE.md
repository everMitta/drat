# Fork notes: `recover-tree` — automated full-volume recovery

This is a fork of [jivanpal/drat](https://github.com/jivanpal/drat), extended
with a new command, `recover-tree`, for automated, resilient recovery of an
entire APFS volume that cannot be mounted due to filesystem-tree corruption.

## Why this exists

Drat's built-in `recover` command resolves a single file or directory by path
or FSOID and writes its data out, but there's no built-in way to walk an
entire volume and recover every file automatically. When recovering hundreds
or thousands of files from a corrupted volume, doing that one `recover`
invocation at a time isn't practical.

## What changed

- **`src/commands/recover-tree.c`** (new): a new `recover-tree` command that:
  - Starts at a given filesystem object (default: the volume root, FSOID
    `0x2`) and recursively walks the filesystem tree using Drat's existing
    `get_fs_records()` B-tree traversal (`include/drat/func/btree.c`) — no
    new tree-walking logic was written.
  - For every directory record, recovers regular files and symlinks, and
    recurses into subdirectories, preserving the original directory
    hierarchy under `--output`.
  - Reconstructs each file strictly by logical offset (not physical block
    order), correctly handling multiple and sparse extents via
    `ftruncate()` + `pwrite()`.
  - Attempts recovery of multilinked inodes rather than skipping them.
  - Skips known macOS/filesystem metadata (`.DS_Store`, `.Spotlight-V100`,
    `.fseventsd`, `.Trashes`, `.DocumentRevisions-V100`, `.TemporaryItems`,
    etc.) by name.
  - Never overwrites an existing recovered file — name collisions get
    `name (1).ext`, `name (2).ext`, etc.
  - Sanitizes on-disk names before using them as output path components
    (rejects `/`, control bytes, and literal `.`/`..`), so a corrupted
    directory record can't cause a write outside the destination tree.
  - Logs every failure (unreadable region, missing extents, etc.) and
    continues — a single bad file or directory never aborts the whole run.
  - Writes `<output>/recovery-report.txt` with discovered/recovered/
    partial/failed/skipped counts, total bytes recovered, and a list of
    every error with its reason.
  - Only ever opens the source container read-only
    (`open_container__info_stream()`, unchanged, still calls
    `fopen(path, "rb")`); the new code never calls `write_blocks()`.

- **`include/drat/func/btree.c`** (modified): `get_fs_records()` and
  `get_btree_phys_omap_entry()` previously called `exit(-1)` whenever they
  hit an unreadable block, a missing object-map entry, or a checksum failure
  while descending the tree. On a volume with genuine metadata corruption
  (exactly the situation this tool is for), that meant the *first* bad node
  anywhere in the tree killed the entire process. These paths now return
  `NULL` (or, mid-walk, whatever records were already collected) instead, so
  `recover-tree` can log the failure for that one object and move on. This
  is the change that makes whole-volume recovery of a corrupted volume
  possible at all. Genuine out-of-memory aborts are untouched.

- **Permission bits are now preserved.** Earlier versions wrote every
  recovered file with a hardcoded `0644`, regardless of its original mode —
  this silently strips the executable bit from every recovered binary and
  script, which breaks anything that was an app bundle (it won't launch) or
  a helper tool. `recover-tree` now reads the original mode from
  `j_inode_val_t::mode` (already present on every inode) and uses it both
  when creating files and, once a directory's children have all been
  written, via a final `chmod()` on the directory itself.

- **Fully-sparse files are now reconstructed correctly instead of being
  marked failed.** A file can legitimately have a non-zero declared size but
  zero `FILE_EXTENT` records — e.g. an NVRAM/EFI-vars store that was created
  but never customized, where the entire logical size is one big hole. That
  case is distinguishable from real corruption: `get_fs_records()` returned
  successfully (a corrupted/unreadable region would have made it return
  `NULL`, which is handled separately and still logged as failed). Such
  files are now written as a correctly-sized, zero-filled (sparse) file
  instead of being reported as unrecoverable.

- **`--path`/`--fsoid` navigation fix**: APFS/HFS+ store filenames
  Unicode-normalized to a decomposed form (what macOS's `iconv` calls
  `UTF-8-MAC`), but a path typed on the command line is normally precomposed
  (`UTF-8`). A byte-exact `strcmp()` — which is what Drat's existing
  `recover`/`list` commands use — silently fails to match any name
  containing a precomposed accented character. `recover-tree`'s path
  matching normalizes both sides with `iconv("UTF-8-MAC", "UTF-8", ...)`
  before comparing. This only affects the optional `--path`/`--fsoid`
  single-object entrypoint; the default full-tree recovery never does string
  matching, since it walks by FSOID.

- **`Makefile`**: links `-liconv` on macOS (needed for the fix above).

## Usage

Build:

    make binaries

Recover an entire volume (default entrypoint is the volume root):

    sudo ./drat recover-tree \
      --container /dev/rdiskNsN \
      --volume <volume index> \
      --output /path/to/destination

Recover starting from a specific subtree or file (hierarchy under it is
still preserved relative to `--output`):

    sudo ./drat recover-tree \
      --container /dev/rdiskNsN \
      --volume <volume index> \
      --output /path/to/destination \
      --path "/some/subdirectory"

    sudo ./drat recover-tree \
      --container /dev/rdiskNsN \
      --volume <volume index> \
      --output /path/to/destination \
      --fsoid 0x605

Notes:

- Always use the **raw** device (`/dev/rdiskN...`, not `/dev/diskN...`) for
  performance — same convention as Drat's other commands.
- Requires read access to the raw device, hence `sudo` on macOS. Recovered
  files are automatically chowned back to the invoking user (detected via
  `SUDO_UID`/`SUDO_GID`) rather than being left owned by root.
- The source device is only ever opened read-only; it is never mounted,
  repaired, or written to by this command (or by Drat in general, aside
  from the disabled `modify` command).
- Check `<output>/recovery-report.txt` after the run for full stats and a
  list of any files that failed, with reasons.
- If disk space is a concern, list the volume root first
  (`drat list --path /`) to see top-level directories before committing to
  a full recursive run, or target a specific subtree with `--path`.

## Known limitations

- Symlink targets stored out-of-line (in a dstream, rather than embedded in
  the xattr) aren't resolved; these are logged as failed.
- Traversal order follows the filesystem tree's key order (a hash of each
  directory entry's name), not alphabetical order — so progress in the
  console log won't necessarily match a top-to-bottom `Finder`-style
  listing of the source volume.
- **Recovered macOS `.app` bundles will not run as-is if the source region
  they lived in had any corruption**, even if the app's own main executable
  recovered perfectly. Every file inside a signed bundle is covered by the
  bundle's code signature (`Contents/_CodeSignature/CodeResources`); if even
  one sealed resource is missing or unreadable, the bundle's original
  signature can never validate again, and macOS will refuse to launch it
  ("app is damaged"/AMFI errors). This is not something Drat can fix — the
  data those specific resources contained is genuinely gone. Recovery
  options at that point are outside Drat's scope, but as a pointer: you can
  strip the broken signature and re-sign the bundle ad-hoc
  (`codesign --force --deep --sign - MyApp.app`) to make its own executables
  launchable again, but restricted entitlements (`com.apple.security.
  hypervisor`, `network.server`, `app-sandbox`, etc.) generally cannot be
  legitimately re-granted without Apple's real signing certificate — AMFI
  will reject an ad-hoc-signed binary that claims them ("adhoc signed but
  contains restricted entitlements"). If the app hosts *data* that's
  actually portable (e.g. a virtual machine bundle, a document, a project
  file) rather than being the valuable thing itself, it's usually far
  simpler to open that recovered data with a separately, properly installed
  copy of the same app instead of trying to resurrect the recovered copy of
  the app itself.
