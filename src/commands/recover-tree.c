/**
 * `recover-tree`: Automated, resilient, whole-volume file recovery.
 *
 * Starting from a given filesystem object (by default, the volume root),
 * recursively walks the APFS filesystem tree via the existing B-tree
 * traversal logic in `get_fs_records()` (see <drat/func/btree.h>), and for
 * every regular file and symlink it finds, reconstructs the file from its
 * FILE EXTENT records and writes it to disk under `--output`, preserving the
 * original directory hierarchy.
 *
 * This command never opens the source container for writing (see
 * <drat/io.c>: `open_container__info_stream()` always calls `fopen(path,
 * "rb")`), and never calls `write_blocks()`. All destination paths are
 * derived from the value of `--output`.
 *
 * Because the source volume may have corrupted B-tree nodes, this command
 * relies on `get_fs_records()` and `get_btree_phys_omap_entry()` returning
 * NULL (rather than aborting the whole process) when they hit an unreadable
 * or invalid node; a single bad directory or file is logged and skipped so
 * that traversal of the rest of the tree can continue.
 */

#include <stdio.h>
#include <sys/errno.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <sysexits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <time.h>

#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#ifdef __APPLE__
#include <iconv.h>
#endif

#include <apfs/object.h>
#include <apfs/nx.h>
#include <apfs/omap.h>
#include <apfs/fs.h>
#include <apfs/j.h>
#include <apfs/jconst.h>
#include <apfs/xf.h>
#include <apfs/dstream.h>
#include <apfs/sibling.h>
#include <apfs/snap.h>

#include <drat/argp.h>
#include <drat/globals.h>
#include <drat/strings.h>
#include <drat/io.h>
#include <drat/print-fs-records.h>

#include <drat/func/boolean.h>
#include <drat/func/cksum.h>
#include <drat/func/btree.h>
#include <drat/func/xf.h>

#include <drat/string/object.h>
#include <drat/string/nx.h>
#include <drat/string/omap.h>
#include <drat/string/btree.h>
#include <drat/string/fs.h>
#include <drat/string/j.h>

/** Command-line options **/

typedef struct {
    int64_t fsoid;
    char*   path;
    char*   output;
} options_t;

#define DRAT_ARG_KEY_FSOID  (DRAT_GLOBAL_ARGS_LAST_KEY - 1)
#define DRAT_ARG_KEY_PATH   (DRAT_GLOBAL_ARGS_LAST_KEY - 2)
#define DRAT_ARG_KEY_OUTPUT (DRAT_GLOBAL_ARGS_LAST_KEY - 3)

#define DRAT_ARG_ERR_INVALID_FSOID  (DRAT_GLOBAL_ARGS_LAST_ERR - 1)
#define DRAT_ARG_ERR_INVALID_PATH   (DRAT_GLOBAL_ARGS_LAST_ERR - 2)
#define DRAT_ARG_ERR_NO_VOLUME      (DRAT_GLOBAL_ARGS_LAST_ERR - 3)
#define DRAT_ARG_ERR_NO_OUTPUT      (DRAT_GLOBAL_ARGS_LAST_ERR - 4)

static const struct argp_option argp_options[] = {
    // char* name,  int key,                char* arg,      int flags,  char* doc
    { "fsoid",      DRAT_ARG_KEY_FSOID,     "fsoid",        0,          "Filesystem object ID to start recovery from (default: volume root)" },
    { "path",       DRAT_ARG_KEY_PATH,      "path",         0,          "Path to start recovery from (default: volume root, `/`)" },
    { "output",     DRAT_ARG_KEY_OUTPUT,    "output dir",   0,          "Directory where recovered files will be written; required" },
    {0}
};

static error_t argp_parser(int key, char* arg, struct argp_state* state) {
    options_t* options = state->input;

    switch (key) {
        case DRAT_ARG_KEY_FSOID:
            if (!parse_number(&options->fsoid, arg)) {
                return DRAT_ARG_ERR_INVALID_FSOID;
            }
            break;
        case DRAT_ARG_KEY_PATH:
            if (arg[0] != '/') {
                return DRAT_ARG_ERR_INVALID_PATH;
            }
            options->path = arg;
            break;
        case DRAT_ARG_KEY_OUTPUT:
            options->output = arg;
            break;
        case ARGP_KEY_END:
            if (globals.volume == -1) {
                return DRAT_ARG_ERR_NO_VOLUME;
            }
            if (!options->output) {
                return DRAT_ARG_ERR_NO_OUTPUT;
            }
            // fall through
        default:
            return ARGP_ERR_UNKNOWN;
    }

    return 0;
}

static const struct argp argp = {
    &argp_options,
    &argp_parser,
    0,
    0,
    &argp_children
};

static void print_usage(FILE* stream) {
    fprintf(
        stream,
        "Usage:\n"
        "  %1$s %2$s --container <container> --volume <volume index> --output <dir>\n"
        "  %1$s %2$s --container <container> --volume <volume index> --output <dir> --path <start path>\n"
        "  %1$s %2$s --container <container> --volume <volume index> --output <dir> --fsoid <start FSOID>\n"
        "Examples:\n"
        "  %1$s %2$s --container /dev/rdisk0s2 --volume 1 --output ~/recovered\n"
        "  %1$s %2$s --container /dev/rdisk0s2 --volume 1 --output ~/recovered --path \"/Users/john/Documents\"\n"
        "\n"
        "Recursively walks the filesystem tree from the given entrypoint (default:\n"
        "the volume root) and recovers every reachable regular file and symlink,\n"
        "preserving directory structure under --output. A directory or file that\n"
        "cannot be read due to corruption is logged and skipped; recovery continues.\n"
        "A report is written to <output dir>/recovery-report.txt.\n",
        globals.program_name,
        globals.command_name
    );
}

/** Bookkeeping **/

typedef struct {
    char**  messages;
    size_t  count;
    size_t  capacity;
} msg_list_t;

static void msg_list_add(msg_list_t* list, const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    if (list->count == list->capacity) {
        list->capacity = list->capacity ? list->capacity * 2 : 64;
        list->messages = realloc(list->messages, list->capacity * sizeof(char*));
    }
    list->messages[list->count++] = strdup(buf);
}

typedef struct {
    uint64_t    dirs_discovered;
    uint64_t    files_discovered;
    uint64_t    files_recovered;
    uint64_t    files_partial;
    uint64_t    files_failed;
    uint64_t    files_skipped;
    uint64_t    bytes_recovered;
    msg_list_t  errors;
} recovery_stats_t;

typedef struct {
    btree_node_phys_t*  fs_omap_btree;
    btree_node_phys_t*  fs_root_btree;
    xid_t               max_xid;
    // When run under `sudo` (required for raw device access), files we
    // create are owned by root by default. If SUDO_UID/SUDO_GID identify
    // the invoking non-root user, restore_owner() chowns everything we
    // write back to them. Set to (uid_t)-1 / (gid_t)-1 to disable (e.g.
    // not running under sudo).
    uid_t               target_uid;
    gid_t               target_gid;
} fs_ctx_t;

static void restore_owner(const fs_ctx_t* ctx, const char* path, bool is_symlink) {
    if (ctx->target_uid == (uid_t)-1) {
        return;
    }
    if (is_symlink) {
        lchown(path, ctx->target_uid, ctx->target_gid);
    } else {
        chown(path, ctx->target_uid, ctx->target_gid);
    }
}

/** Path / filename helpers **/

/**
 * Copy up to `raw_len` bytes (or until a NUL) of `raw` into `out`, replacing
 * any byte that would be unsafe or meaningless as a single path component
 * (`/`, NUL, and other control characters) with `_`. Also neutralises the
 * special names `.` and `..` so a corrupted directory record can never cause
 * a write outside of the destination tree.
 */
static void sanitize_component(char* out, size_t outsz, const uint8_t* raw, size_t raw_len) {
    size_t j = 0;
    for (size_t i = 0; i < raw_len && raw[i] != '\0' && j + 1 < outsz; i++) {
        uint8_t c = raw[i];
        if (c == '/' || c < 0x20) {
            c = '_';
        }
        out[j++] = (char)c;
    }
    out[j] = '\0';

    if (j == 0) {
        snprintf(out, outsz, "_unnamed");
    } else if (strcmp(out, ".") == 0 || strcmp(out, "..") == 0) {
        char tmp[8];
        snprintf(tmp, sizeof(tmp), "_%s", out);
        snprintf(out, outsz, "%s", tmp);
    }
}

/**
 * Create `path` and any missing parent directories (like `mkdir -p`). If
 * `target_uid` is not (uid_t)-1, every path component -- newly-created or
 * pre-existing -- is chowned to `target_uid`/`target_gid`; this is how we
 * hand root-owned output back to the user who invoked `sudo`.
 */
static int mkdir_p(const char* path, uid_t target_uid, gid_t target_gid) {
    char tmp[4096];
    snprintf(tmp, sizeof(tmp), "%s", path);
    size_t len = strlen(tmp);
    if (len > 0 && tmp[len - 1] == '/') {
        tmp[len - 1] = '\0';
    }
    for (char* p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);
            if (target_uid != (uid_t)-1) {
                chown(tmp, target_uid, target_gid);
            }
            *p = '/';
        }
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
        return -1;
    }
    if (target_uid != (uid_t)-1) {
        chown(tmp, target_uid, target_gid);
    }
    return 0;
}

/**
 * Compute a destination path `dir/name` that doesn't currently exist,
 * inserting " (1)", " (2)", etc. before the extension as needed so that an
 * already-recovered file is never overwritten. Returns a malloc'd string, or
 * NULL on failure.
 */
static char* unique_dest_path(const char* dir, const char* name) {
    char base[560];
    char ext[64];

    char* dot = strrchr(name, '.');
    if (dot && dot != name) {
        size_t blen = (size_t)(dot - name);
        if (blen >= sizeof(base)) {
            blen = sizeof(base) - 1;
        }
        memcpy(base, name, blen);
        base[blen] = '\0';
        snprintf(ext, sizeof(ext), "%s", dot);
    } else {
        snprintf(base, sizeof(base), "%s", name);
        ext[0] = '\0';
    }

    for (int n = 0; n < 100000; n++) {
        char* candidate = NULL;
        if (n == 0) {
            if (asprintf(&candidate, "%s/%s%s", dir, base, ext) < 0) {
                return NULL;
            }
        } else {
            if (asprintf(&candidate, "%s/%s (%d)%s", dir, base, n, ext) < 0) {
                return NULL;
            }
        }

        struct stat st;
        if (lstat(candidate, &st) != 0 && errno == ENOENT) {
            return candidate;
        }
        free(candidate);
    }

    return NULL;
}

/**
 * APFS (like HFS+) stores filenames Unicode-normalized to a decomposed form
 * (exposed via iconv's Apple-specific "UTF-8-MAC" target), but a path typed
 * on the command line is typically precomposed ("UTF-8"). Byte-exact
 * `strcmp()` therefore fails to match names containing accented characters
 * (e.g. "Números") even though they're the same string. Normalize both
 * sides before falling back to a mismatch.
 */
static char* normalize_utf8_mac(const char* s) {
#ifdef __APPLE__
    iconv_t cd = iconv_open("UTF-8-MAC", "UTF-8");
    if (cd == (iconv_t)-1) {
        return strdup(s);
    }

    size_t inbytesleft = strlen(s);
    size_t outbytesleft = inbytesleft * 4 + 16;
    char* out = malloc(outbytesleft + 1);
    if (!out) {
        iconv_close(cd);
        return strdup(s);
    }

    char* inptr = (char*)s;
    char* outptr = out;
    size_t r = iconv(cd, &inptr, &inbytesleft, &outptr, &outbytesleft);
    iconv_close(cd);
    if (r == (size_t)-1) {
        free(out);
        return strdup(s);
    }
    *outptr = '\0';
    return out;
#else
    return strdup(s);
#endif
}

static bool names_match(const char* disk_name, const char* query_name) {
    if (strcmp(disk_name, query_name) == 0) {
        return true;
    }
    char* a = normalize_utf8_mac(disk_name);
    char* b = normalize_utf8_mac(query_name);
    bool eq = a && b && strcmp(a, b) == 0;
    free(a);
    free(b);
    return eq;
}

static void format_bytes(uint64_t bytes, char* out, size_t outsz) {
    static const char* units[] = { "B", "KB", "MB", "GB", "TB" };
    double val = (double)bytes;
    int u = 0;
    while (val >= 1024.0 && u < 4) {
        val /= 1024.0;
        u++;
    }
    snprintf(out, outsz, "%.2f %s", val, units[u]);
}

/**
 * Names of well-known macOS/APFS metadata that should not be treated as user
 * data. Matched against the sanitised base name only.
 */
static const char* SKIP_NAMES[] = {
    ".DS_Store",
    ".Spotlight-V100",
    ".fseventsd",
    ".Trashes",
    ".DocumentRevisions-V100",
    ".TemporaryItems",
    ".apdisk",
    ".PKInstallSandboxManager",
    ".PKInstallSandboxManager-SystemSoftware",
    NULL
};

static bool should_skip_name(const char* name) {
    for (const char** s = SKIP_NAMES; *s; s++) {
        if (strcmp(name, *s) == 0) {
            return true;
        }
    }
    return false;
}

/** Size / extent extraction **/

/**
 * Like `get_file_size()` (see <drat/func/j.h>), but reports success/failure
 * via the return value instead of printing to `stderr`, so that callers can
 * distinguish a genuinely empty file from an inode with no size information
 * at all.
 */
static bool try_get_file_size(j_inode_val_t* inode, uint16_t inode_len, uint64_t* out_size) {
    if (inode->internal_flags & INODE_HAS_UNCOMPRESSED_SIZE) {
        *out_size = inode->uncompressed_size;
        return true;
    }

    if (inode_len != sizeof(j_inode_val_t)) {
        xf_pair_t** xf_pairs = get_xf_pairs_array(inode->xfields);
        if (!xf_pairs) {
            return false;
        }

        bool found = false;
        for (xf_pair_t** cursor = xf_pairs; *cursor; cursor++) {
            xf_pair_t* xf_pair = *cursor;
            if (xf_pair->key.x_type == INO_EXT_TYPE_DSTREAM) {
                j_dstream_t* dstream = (j_dstream_t*)&(xf_pair->value);
                *out_size = dstream->size;
                found = true;
                break;
            }
        }
        free_xf_pairs_array(xf_pairs);
        return found;
    }

    return false;
}

typedef struct {
    uint64_t logical_addr;
    uint64_t phys_block_num;
    uint64_t len_bytes;
} extent_t;

static int extent_cmp(const void* a, const void* b) {
    const extent_t* ea = a;
    const extent_t* eb = b;
    if (ea->logical_addr < eb->logical_addr) return -1;
    if (ea->logical_addr > eb->logical_addr) return 1;
    return 0;
}

/** Recovery of individual objects **/

static void recover_regular_file(fs_ctx_t* ctx, oid_t oid, const char* out_dir, const char* name, const char* logical_path, recovery_stats_t* stats) {
    j_rec_t** fs_records = get_fs_records(ctx->fs_omap_btree, ctx->fs_root_btree, oid, ctx->max_xid);
    if (!fs_records) {
        printf("FAIL %s\n     reason: could not read filesystem records for FSOID %#"PRIx64" (corrupted b-tree region)\n", logical_path, oid);
        msg_list_add(&stats->errors, "FAILED %s\n  reason: could not read filesystem records for FSOID %#"PRIx64" (corrupted b-tree region)", logical_path, oid);
        stats->files_failed++;
        return;
    }

    j_inode_val_t* inode = NULL;
    uint16_t inode_val_len = 0;
    int32_t nlink = 1;

    for (j_rec_t** cur = fs_records; *cur; cur++) {
        j_key_t* hdr = (j_key_t*)(*cur)->data;
        if (((hdr->obj_id_and_type & OBJ_TYPE_MASK) >> OBJ_TYPE_SHIFT) == APFS_TYPE_INODE) {
            inode = (j_inode_val_t*)((*cur)->data + (*cur)->key_len);
            inode_val_len = (*cur)->val_len;
            nlink = inode->nlink;
            break;
        }
    }

    if (!inode) {
        printf("FAIL %s\n     reason: no inode record found for FSOID %#"PRIx64"\n", logical_path, oid);
        msg_list_add(&stats->errors, "FAILED %s\n  reason: no inode record found for FSOID %#"PRIx64, logical_path, oid);
        stats->files_failed++;
        free_j_rec_array(fs_records);
        return;
    }

    uint64_t file_size = 0;
    bool have_size = try_get_file_size(inode, inode_val_len, &file_size);

    extent_t* extents = NULL;
    size_t num_extents = 0, cap_extents = 0;

    for (j_rec_t** cur = fs_records; *cur; cur++) {
        j_key_t* hdr = (j_key_t*)(*cur)->data;
        if (((hdr->obj_id_and_type & OBJ_TYPE_MASK) >> OBJ_TYPE_SHIFT) == APFS_TYPE_FILE_EXTENT) {
            j_file_extent_key_t* key = (j_file_extent_key_t*)(*cur)->data;
            j_file_extent_val_t* val = (j_file_extent_val_t*)((*cur)->data + (*cur)->key_len);

            if (num_extents == cap_extents) {
                cap_extents = cap_extents ? cap_extents * 2 : 8;
                extents = realloc(extents, cap_extents * sizeof(extent_t));
            }
            extents[num_extents].logical_addr   = key->logical_addr;
            extents[num_extents].phys_block_num = val->phys_block_num;
            extents[num_extents].len_bytes      = val->len_and_flags & J_FILE_EXTENT_LEN_MASK;
            num_extents++;
        }
    }
    if (num_extents > 1) {
        qsort(extents, num_extents, sizeof(extent_t), extent_cmp);
    }

    if (!have_size) {
        if (num_extents == 0) {
            printf("FAIL %s\n     reason: no file size or extents found (inode data likely corrupt)\n", logical_path);
            msg_list_add(&stats->errors, "FAILED %s\n  reason: no file size or extents found (inode data likely corrupt)", logical_path);
            stats->files_failed++;
            free(extents);
            free_j_rec_array(fs_records);
            return;
        }
        uint64_t derived = 0;
        for (size_t i = 0; i < num_extents; i++) {
            uint64_t end = extents[i].logical_addr + extents[i].len_bytes;
            if (end > derived) derived = end;
        }
        file_size = derived;
        msg_list_add(&stats->errors, "NOTE %s\n  note: file size missing from inode; derived %"PRIu64" bytes from extents", logical_path, file_size);
    }

    char* dest = unique_dest_path(out_dir, name);
    if (!dest) {
        printf("FAIL %s\n     reason: could not compute a unique destination path\n", logical_path);
        msg_list_add(&stats->errors, "FAILED %s\n  reason: could not compute a unique destination path", logical_path);
        stats->files_failed++;
        free(extents);
        free_j_rec_array(fs_records);
        return;
    }

    const char* ml = nlink > 1 ? " [multilinked]" : "";

    // Preserve the file's original permission bits from its inode; fall back
    // to a sane default if the stored mode has no permission bits at all.
    mode_t file_mode = (inode->mode & 07777) ? (mode_t)(inode->mode & 07777) : 0644;

    if (file_size == 0) {
        int fd = open(dest, O_WRONLY | O_CREAT | O_EXCL, file_mode);
        if (fd < 0) {
            printf("FAIL %s\n     reason: could not create output file: %s\n", logical_path, strerror(errno));
            msg_list_add(&stats->errors, "FAILED %s\n  reason: could not create output file: %s", logical_path, strerror(errno));
            stats->files_failed++;
        } else {
            close(fd);
            restore_owner(ctx, dest, false);
            printf("OK   %s (0 bytes)%s\n", logical_path, ml);
            stats->files_recovered++;
        }
        free(dest);
        free(extents);
        free_j_rec_array(fs_records);
        return;
    }

    if (num_extents == 0) {
        // `fs_records` was read successfully (we didn't bail out above with
        // "could not read filesystem records"), so this isn't a corrupted or
        // unreadable region -- it's a legitimate, fully-sparse file (a hole
        // spanning its entire logical size, e.g. an never-customized NVRAM
        // store). Reconstruct it as such: a correctly-sized, all-zero file.
        int fd = open(dest, O_WRONLY | O_CREAT | O_EXCL, file_mode);
        if (fd < 0) {
            printf("FAIL %s\n     reason: could not create output file: %s\n", logical_path, strerror(errno));
            msg_list_add(&stats->errors, "FAILED %s\n  reason: could not create output file: %s", logical_path, strerror(errno));
            stats->files_failed++;
            free(dest);
            free(extents);
            free_j_rec_array(fs_records);
            return;
        }
        if (ftruncate(fd, (off_t)file_size) != 0) {
            printf("FAIL %s\n     reason: ftruncate() failed: %s\n", logical_path, strerror(errno));
            msg_list_add(&stats->errors, "FAILED %s\n  reason: ftruncate() failed: %s", logical_path, strerror(errno));
            stats->files_failed++;
            close(fd);
            unlink(dest);
        } else {
            close(fd);
            restore_owner(ctx, dest, false);
            printf("OK   %s (%"PRIu64" bytes, fully sparse)%s\n", logical_path, file_size, ml);
            stats->files_recovered++;
            stats->bytes_recovered += file_size;
        }
        free(dest);
        free(extents);
        free_j_rec_array(fs_records);
        return;
    }

    int fd = open(dest, O_WRONLY | O_CREAT | O_EXCL, file_mode);
    if (fd < 0) {
        printf("FAIL %s\n     reason: could not create output file: %s\n", logical_path, strerror(errno));
        msg_list_add(&stats->errors, "FAILED %s\n  reason: could not create output file: %s", logical_path, strerror(errno));
        stats->files_failed++;
        free(dest);
        free(extents);
        free_j_rec_array(fs_records);
        return;
    }
    if (ftruncate(fd, (off_t)file_size) != 0) {
        printf("FAIL %s\n     reason: ftruncate() failed: %s\n", logical_path, strerror(errno));
        msg_list_add(&stats->errors, "FAILED %s\n  reason: ftruncate() failed: %s", logical_path, strerror(errno));
        stats->files_failed++;
        close(fd);
        unlink(dest);
        free(dest);
        free(extents);
        free_j_rec_array(fs_records);
        return;
    }

    char* buffer = malloc(globals.block_size);
    uint64_t total_written = 0;
    int bad_blocks = 0;

    // Reconstruct the file strictly by logical address, one physical block
    // at a time; `ftruncate()` above already zero-fills any gaps between
    // extents (sparse regions) and any trailing hole up to `file_size`.
    for (size_t e = 0; e < num_extents; e++) {
        uint64_t logical  = extents[e].logical_addr;
        uint64_t remaining = extents[e].len_bytes;
        uint64_t phys_block = extents[e].phys_block_num;

        while (remaining > 0 && logical < file_size) {
            uint64_t to_write = (uint64_t)globals.block_size;
            if (to_write > remaining) to_write = remaining;
            if (logical + to_write > file_size) to_write = file_size - logical;

            if (read_blocks(buffer, phys_block, 1) != 1) {
                bad_blocks++;
            } else {
                ssize_t written = pwrite(fd, buffer, to_write, (off_t)logical);
                if (written < 0 || (uint64_t)written != to_write) {
                    bad_blocks++;
                } else {
                    total_written += to_write;
                }
            }

            logical += to_write;
            remaining -= to_write;
            phys_block++;
        }
    }
    free(buffer);
    close(fd);
    free(extents);
    free_j_rec_array(fs_records);

    if (bad_blocks == 0 && total_written >= file_size) {
        restore_owner(ctx, dest, false);
        printf("OK   %s (%"PRIu64" bytes)%s\n", logical_path, file_size, ml);
        stats->files_recovered++;
        stats->bytes_recovered += file_size;
    } else if (total_written > 0) {
        restore_owner(ctx, dest, false);
        printf("PART %s (%"PRIu64"/%"PRIu64" bytes; %d bad block(s))%s\n", logical_path, total_written, file_size, bad_blocks, ml);
        msg_list_add(&stats->errors, "PARTIAL %s\n  recovered %"PRIu64" of %"PRIu64" bytes; %d block read error(s)", logical_path, total_written, file_size, bad_blocks);
        stats->files_partial++;
        stats->bytes_recovered += total_written;
    } else {
        printf("FAIL %s\n     reason: all %d extent block(s) failed to read\n", logical_path, bad_blocks);
        msg_list_add(&stats->errors, "FAILED %s\n  reason: all extent blocks failed to read (0 of %"PRIu64" bytes recovered)", logical_path, file_size);
        unlink(dest);
        stats->files_failed++;
    }
    free(dest);
}

static void recover_symlink(fs_ctx_t* ctx, oid_t oid, const char* out_dir, const char* name, const char* logical_path, recovery_stats_t* stats) {
    j_rec_t** fs_records = get_fs_records(ctx->fs_omap_btree, ctx->fs_root_btree, oid, ctx->max_xid);
    if (!fs_records) {
        printf("FAIL %s\n     reason: could not read filesystem records for symlink FSOID %#"PRIx64"\n", logical_path, oid);
        msg_list_add(&stats->errors, "FAILED %s\n  reason: could not read filesystem records for symlink FSOID %#"PRIx64, logical_path, oid);
        stats->files_failed++;
        return;
    }

    char* target = NULL;
    for (j_rec_t** cur = fs_records; *cur; cur++) {
        j_key_t* hdr = (j_key_t*)(*cur)->data;
        if (((hdr->obj_id_and_type & OBJ_TYPE_MASK) >> OBJ_TYPE_SHIFT) == APFS_TYPE_XATTR) {
            j_xattr_key_t* key = (j_xattr_key_t*)(*cur)->data;
            j_xattr_val_t* val = (j_xattr_val_t*)((*cur)->data + (*cur)->key_len);

            if (strcmp((char*)key->name, SYMLINK_EA_NAME) == 0) {
                if (val->flags & XATTR_DATA_EMBEDDED) {
                    size_t len = val->xdata_len;
                    target = malloc(len + 1);
                    if (target) {
                        memcpy(target, val->xdata, len);
                        target[len] = '\0';
                    }
                }
                break;
            }
        }
    }

    if (!target) {
        printf("FAIL %s\n     reason: symlink target not found (xattr missing or stored out-of-line)\n", logical_path);
        msg_list_add(&stats->errors, "FAILED %s\n  reason: symlink target xattr not found, or its data is stored out-of-line (not implemented)", logical_path);
        stats->files_failed++;
        free_j_rec_array(fs_records);
        return;
    }

    char* dest = unique_dest_path(out_dir, name);
    if (!dest || symlink(target, dest) != 0) {
        printf("FAIL %s\n     reason: could not create symlink: %s\n", logical_path, dest ? strerror(errno) : "path allocation failed");
        msg_list_add(&stats->errors, "FAILED %s\n  reason: could not create symlink: %s", logical_path, dest ? strerror(errno) : "path allocation failed");
        stats->files_failed++;
    } else {
        restore_owner(ctx, dest, true);
        printf("OK   %s -> %s (symlink)\n", logical_path, target);
        stats->files_recovered++;
    }
    free(dest);
    free(target);
    free_j_rec_array(fs_records);
}

#define MAX_RECOVERY_DEPTH 256

static void process_directory(fs_ctx_t* ctx, oid_t oid, const char* out_dir, const char* logical_path, int depth, recovery_stats_t* stats) {
    if (depth > MAX_RECOVERY_DEPTH) {
        printf("FAIL %s\n     reason: maximum directory depth exceeded (possible corrupted/cyclic structure)\n", logical_path);
        msg_list_add(&stats->errors, "FAILED %s\n  reason: maximum directory depth exceeded", logical_path);
        stats->files_failed++;
        return;
    }

    j_rec_t** fs_records = get_fs_records(ctx->fs_omap_btree, ctx->fs_root_btree, oid, ctx->max_xid);
    if (!fs_records) {
        printf("FAIL %s\n     reason: could not read filesystem records for directory FSOID %#"PRIx64" (corrupted b-tree region)\n", logical_path, oid);
        msg_list_add(&stats->errors, "FAILED %s\n  reason: could not read filesystem records for directory FSOID %#"PRIx64" (corrupted b-tree region)", logical_path, oid);
        stats->files_failed++;
        return;
    }

    for (j_rec_t** cur = fs_records; *cur; cur++) {
        j_key_t* hdr = (j_key_t*)(*cur)->data;
        if (((hdr->obj_id_and_type & OBJ_TYPE_MASK) >> OBJ_TYPE_SHIFT) != APFS_TYPE_DIR_REC) {
            continue;
        }

        j_drec_hashed_key_t* key = (j_drec_hashed_key_t*)(*cur)->data;
        j_drec_val_t* val = (j_drec_val_t*)((*cur)->data + (*cur)->key_len);

        size_t raw_name_len = key->name_len_and_hash & J_DREC_LEN_MASK;
        char name[600];
        sanitize_component(name, sizeof(name), key->name, raw_name_len);

        if (should_skip_name(name)) {
            stats->files_skipped++;
            continue;
        }

        char* child_logical = NULL;
        if (asprintf(&child_logical, "%s/%s", logical_path, name) < 0) {
            continue;
        }

        oid_t    child_oid   = val->file_id;
        uint16_t drec_type   = val->flags & DREC_TYPE_MASK;

        switch (drec_type) {
            case DT_DIR: {
                stats->dirs_discovered++;
                char* child_out_dir = NULL;
                if (asprintf(&child_out_dir, "%s/%s", out_dir, name) >= 0) {
                    if (mkdir_p(child_out_dir, ctx->target_uid, ctx->target_gid) != 0) {
                        printf("FAIL %s\n     reason: could not create directory: %s\n", child_logical, strerror(errno));
                        msg_list_add(&stats->errors, "FAILED %s\n  reason: could not create output directory: %s", child_logical, strerror(errno));
                        stats->files_failed++;
                    } else {
                        process_directory(ctx, child_oid, child_out_dir, child_logical, depth + 1, stats);
                    }
                    free(child_out_dir);
                }
            } break;

            case DT_REG:
                stats->files_discovered++;
                recover_regular_file(ctx, child_oid, out_dir, name, child_logical, stats);
                break;

            case DT_LNK:
                stats->files_discovered++;
                recover_symlink(ctx, child_oid, out_dir, name, child_logical, stats);
                break;

            default:
                stats->files_skipped++;
                printf("SKIP %s\n     reason: unsupported entry type (%u)\n", child_logical, drec_type);
                break;
        }

        free(child_logical);
    }

    // Now that every child has been written, restore this directory's own
    // original permission bits. This must happen last: doing it up front
    // (at `mkdir_p()` time) risks a restrictive original mode blocking our
    // own writes into it while we're still populating it.
    for (j_rec_t** cur = fs_records; *cur; cur++) {
        j_key_t* hdr = (j_key_t*)(*cur)->data;
        if (((hdr->obj_id_and_type & OBJ_TYPE_MASK) >> OBJ_TYPE_SHIFT) == APFS_TYPE_INODE) {
            j_inode_val_t* dir_inode = (j_inode_val_t*)((*cur)->data + (*cur)->key_len);
            if (dir_inode->mode & 07777) {
                chmod(out_dir, dir_inode->mode & 07777);
            }
            break;
        }
    }

    free_j_rec_array(fs_records);
}

/** Main command **/

int cmd_recover_tree(int argc, char** argv) {
    if (argc == 2) {
        print_usage(stdout);
        return 0;
    }

    globals.volume = -1;
    options_t options = { -1, NULL, NULL };

    bool usage_error = true;
    error_t parse_result = argp_parse(&argp, argc, argv, ARGP_IN_ORDER, 0, &options);
    if (!print_global_args_error(parse_result)) {
        switch (parse_result) {
            case 0:
                usage_error = false;
                break;
            case DRAT_ARG_ERR_INVALID_FSOID:
                fprintf(stderr, "%s: option `--fsoid`" INVALID_HEX_STRING, globals.program_name);
                break;
            case DRAT_ARG_ERR_INVALID_PATH:
                fprintf(stderr, "%s: option `--path` has invalid value; must be an absolute path, i.e. start with `/`.\n", globals.program_name);
                break;
            case DRAT_ARG_ERR_NO_VOLUME:
                fprintf(stderr, "%s: option `--volume` is mandatory.\n", globals.program_name);
                break;
            case DRAT_ARG_ERR_NO_OUTPUT:
                fprintf(stderr, "%s: option `--output` is mandatory.\n", globals.program_name);
                break;
            default:
                print_arg_parse_error();
                return EX_SOFTWARE;
        }
    }
    if (usage_error) {
        print_usage(stderr);
        return EX_USAGE;
    }

    setbuf(stdout, NULL);

    if (open_container__info_stream(stdout) != 0) {
        return EX_NOINPUT;
    }
    printf("\n");

    printf("Finding most recent container superblock:\n");
    printf("- Reading block 0x0 ... ");
    nx_superblock_t* nxsb = malloc(globals.block_size);
    if (!nxsb) {
        fprintf(stderr, "ABORT: Not enough memory to create `nxsb`.\n");
        return EX_OSERR;
    }
    if (read_blocks(nxsb, 0, 1) != 1) {
        fprintf(stderr, "ABORT: Failed to read block 0.\n");
        return EX_IOERR;
    }
    printf("validating ... ");
    printf(is_cksum_valid(nxsb) ? "OK.\n" : "FAILED.\n");

    printf("- Loading checkpoint descriptor area ... ");
    uint32_t xp_desc_blocks = nxsb->nx_xp_desc_blocks & ~(1 << 31);
    char (*xp_desc)[globals.block_size] = malloc(xp_desc_blocks * globals.block_size);
    if (!xp_desc) {
        fprintf(stderr, "ABORT: Not enough memory for %"PRIu32" blocks.\n", xp_desc_blocks);
        return EX_OSERR;
    }
    if (nxsb->nx_xp_desc_blocks >> 31) {
        printf("END: Checkpoint descriptor area is a B-tree, but we haven't implemented handling of this case yet.\n\n");
        return 0;
    }
    if (read_blocks(xp_desc, nxsb->nx_xp_desc_base, xp_desc_blocks) != xp_desc_blocks) {
        printf("FAILED.\n");
        fprintf(stderr, "\nABORT: Failed to read all blocks in the checkpoint descriptor area.\n");
        return EX_IOERR;
    }
    printf("OK.\n");

    printf("- Searching checkpoint descriptor area ... ");
    uint32_t i_latest_nx = 0;
    xid_t xid_latest_nx = 0;
    for (uint32_t i = 0; i < xp_desc_blocks; i++) {
        if (!is_cksum_valid(xp_desc[i])) {
            continue;
        }
        if (is_nx_superblock(xp_desc[i])) {
            if (((nx_superblock_t*)xp_desc[i])->nx_magic != NX_MAGIC) {
                continue;
            }
            xid_t nxsb_xid = ((nx_superblock_t*)xp_desc[i])->nx_o.o_xid;
            if ((nxsb_xid > xid_latest_nx) && (nxsb_xid <= (xid_t)(globals.max_xid))) {
                i_latest_nx = i;
                xid_latest_nx = nxsb_xid;
            }
        }
    }
    if (xid_latest_nx == 0) {
        printf("END: Didn't find any container superblock with maximum XID %#"PRIx64".\n", globals.max_xid);
        return 0;
    }
    memcpy(nxsb, xp_desc[i_latest_nx], sizeof(nx_superblock_t));
    free(xp_desc);
    printf("found most recent container superblock at index %"PRIu32", its XID is %#"PRIx64".\n", i_latest_nx, nxsb->nx_o.o_xid);

    printf("Finding container's omap tree ... ");
    omap_phys_t* nx_omap = malloc(globals.block_size);
    if (read_blocks(nx_omap, nxsb->nx_omap_oid, 1) != 1) {
        fprintf(stderr, "ABORT: Failed to read container omap block.\n");
        return EX_IOERR;
    }
    if ((nx_omap->om_tree_type & OBJ_STORAGETYPE_MASK) != OBJ_PHYSICAL) {
        printf("END: Container omap B-tree is not a Physical object.\n");
        return 0;
    }
    btree_node_phys_t* nx_omap_btree = malloc(globals.block_size);
    if (read_blocks(nx_omap_btree, nx_omap->om_tree_oid, 1) != 1) {
        fprintf(stderr, "ABORT: Failed to read container omap B-tree block.\n");
        return EX_IOERR;
    }
    printf("OK.\n");

    printf("Finding volume %"PRId64"'s superblock ... ", globals.volume);
    oid_t apsb_oid = nxsb->nx_fs_oid[globals.volume - 1];
    if (apsb_oid == 0) {
        printf("END: Volume %"PRId64" does not exist.\n", globals.volume);
        return 0;
    }
    omap_entry_t* fs_entry = get_btree_phys_omap_entry(nx_omap_btree, apsb_oid, nxsb->nx_o.o_xid);
    if (!fs_entry) {
        printf("END: No objects with Virtual OID %#"PRIx64" exist in the container omap.\n", apsb_oid);
        return 0;
    }
    apfs_superblock_t* apsb = malloc(globals.block_size);
    if (read_blocks(apsb, fs_entry->val.ov_paddr, 1) != 1) {
        fprintf(stderr, "ABORT: Failed to read volume superblock.\n");
        return EX_IOERR;
    }
    printf("OK. Volume name: %s\n", apsb->apfs_volname);

    printf("Finding volume's omap tree ... ");
    omap_phys_t* fs_omap = malloc(globals.block_size);
    if (read_blocks(fs_omap, apsb->apfs_omap_oid, 1) != 1) {
        fprintf(stderr, "ABORT: Failed to read volume omap block.\n");
        return EX_IOERR;
    }
    if ((fs_omap->om_tree_type & OBJ_STORAGETYPE_MASK) != OBJ_PHYSICAL) {
        printf("END: Volume's omap tree is not a Physical object.\n");
        return 0;
    }
    btree_node_phys_t* fs_omap_btree = malloc(globals.block_size);
    if (read_blocks(fs_omap_btree, fs_omap->om_tree_oid, 1) != 1) {
        fprintf(stderr, "ABORT: Failed to read volume omap B-tree block.\n");
        return EX_IOERR;
    }
    printf("OK.\n");

    printf("Finding volume's filesystem tree ... ");
    omap_entry_t* fs_root_entry = get_btree_phys_omap_entry(fs_omap_btree, apsb->apfs_root_tree_oid, apsb->apfs_o.o_xid);
    if (!fs_root_entry) {
        printf("END: No objects with Virtual OID %#"PRIx64" exist in the volume omap.\n", apsb->apfs_root_tree_oid);
        return 0;
    }
    btree_node_phys_t* fs_root_btree = malloc(globals.block_size);
    if (read_blocks(fs_root_btree, fs_root_entry->val.ov_paddr, 1) != 1) {
        fprintf(stderr, "ABORT: Failed to read filesystem root B-tree block.\n");
        return EX_IOERR;
    }
    free(fs_root_entry);
    printf("OK.\n\n");

    // Raw device access requires running as root (typically via `sudo`), which
    // would otherwise leave every recovered file/directory owned by root. If
    // invoked via `sudo`, SUDO_UID/SUDO_GID identify the actual user; restore
    // ownership to them as we go.
    uid_t target_uid = (uid_t)-1;
    gid_t target_gid = (gid_t)-1;
    const char* sudo_uid_str = getenv("SUDO_UID");
    const char* sudo_gid_str = getenv("SUDO_GID");
    if (sudo_uid_str && sudo_gid_str) {
        target_uid = (uid_t)strtoul(sudo_uid_str, NULL, 10);
        target_gid = (gid_t)strtoul(sudo_gid_str, NULL, 10);
    }

    fs_ctx_t ctx = { fs_omap_btree, fs_root_btree, (xid_t)globals.max_xid, target_uid, target_gid };

    // Resolve the entrypoint (default: volume root)
    oid_t start_oid = ROOT_DIR_INO_NUM;
    char  start_logical[4096] = "";
    char  start_name[600] = "";

    if (options.path) {
        printf("Navigating to path `%s` ... ", options.path);
        oid_t fsoid = ROOT_DIR_INO_NUM;
        j_rec_t** fs_records = get_fs_records(fs_omap_btree, fs_root_btree, fsoid, globals.max_xid);
        if (!fs_records) {
            printf("END: No records found for the filesystem root.\n");
            return 0;
        }

        char* path = strdup(options.path);
        char* path_element;
        char* cursor = path;
        while ((path_element = strsep(&cursor, "/")) != NULL) {
            if (*path_element == '\0') continue;

            signed int match = -1;
            for (j_rec_t** c = fs_records; *c; c++) {
                j_key_t* hdr = (j_key_t*)(*c)->data;
                if (((hdr->obj_id_and_type & OBJ_TYPE_MASK) >> OBJ_TYPE_SHIFT) == APFS_TYPE_DIR_REC) {
                    j_drec_hashed_key_t* key = (j_drec_hashed_key_t*)(*c)->data;
                    if (names_match((char*)key->name, path_element)) {
                        match = (signed int)(c - fs_records);
                        break;
                    }
                }
            }
            if (match == -1) {
                printf("END: No dentry found for path `%s`.\n", options.path);
                free(path);
                return 0;
            }
            j_drec_val_t* val = (j_drec_val_t*)(fs_records[match]->data + fs_records[match]->key_len);
            fsoid = val->file_id;
            free_j_rec_array(fs_records);
            fs_records = get_fs_records(fs_omap_btree, fs_root_btree, fsoid, globals.max_xid);
            if (!fs_records) {
                printf("END: Could not read records for `%s` (corrupted region).\n", options.path);
                free(path);
                return 0;
            }
        }
        free(path);
        free_j_rec_array(fs_records);

        start_oid = fsoid;
        snprintf(start_logical, sizeof(start_logical), "%s", options.path);
        char* base = strrchr(options.path, '/');
        snprintf(start_name, sizeof(start_name), "%s", base ? base + 1 : options.path);
        printf("its FSOID is %#"PRIx64".\n", start_oid);
    } else if (options.fsoid != -1) {
        start_oid = (oid_t)options.fsoid;
        snprintf(start_logical, sizeof(start_logical), "(fsoid %#"PRIx64")", start_oid);
        snprintf(start_name, sizeof(start_name), "fsoid_%"PRIx64, start_oid);
    } else {
        snprintf(start_logical, sizeof(start_logical), "/");
    }

    if (mkdir_p(options.output, target_uid, target_gid) != 0) {
        fprintf(stderr, "ABORT: Could not create output directory `%s`: %s\n", options.output, strerror(errno));
        return EX_CANTCREAT;
    }

    // Determine whether the entrypoint is a directory or a single file/symlink
    j_rec_t** entry_records = get_fs_records(fs_omap_btree, fs_root_btree, start_oid, globals.max_xid);
    if (!entry_records) {
        fprintf(stderr, "ABORT: Could not read filesystem records for entrypoint FSOID %#"PRIx64".\n", start_oid);
        return EX_DATAERR;
    }
    j_inode_val_t* entry_inode = NULL;
    for (j_rec_t** c = entry_records; *c; c++) {
        j_key_t* hdr = (j_key_t*)(*c)->data;
        if (((hdr->obj_id_and_type & OBJ_TYPE_MASK) >> OBJ_TYPE_SHIFT) == APFS_TYPE_INODE) {
            entry_inode = (j_inode_val_t*)((*c)->data + (*c)->key_len);
            break;
        }
    }
    if (!entry_inode) {
        fprintf(stderr, "ABORT: No inode record found for entrypoint FSOID %#"PRIx64".\n", start_oid);
        free_j_rec_array(entry_records);
        return EX_DATAERR;
    }
    apfs_mode_t entry_mode = entry_inode->mode;
    free_j_rec_array(entry_records);

    // When starting from an explicit `--path`, reproduce the same directory
    // hierarchy under `--output` that a full recovery from the volume root
    // would have produced, rather than dumping the entrypoint flat into the
    // output root. (A bare `--fsoid` has no known path, so it stays flat.)
    char* entry_out_dir = options.output;
    if (options.path) {
        char* dirpart = strdup(options.path);
        char* last_slash = strrchr(dirpart, '/');
        if (last_slash) *last_slash = '\0';   // dirpart is now "" or "/a/b"

        char* computed = NULL;
        if ((entry_mode & S_IFMT) == S_IFDIR) {
            if (asprintf(&computed, "%s%s", options.output, options.path) >= 0) {
                entry_out_dir = computed;
            }
        } else {
            if (asprintf(&computed, "%s%s", options.output, dirpart) >= 0) {
                entry_out_dir = computed;
            }
        }
        free(dirpart);
        if (entry_out_dir != options.output) {
            mkdir_p(entry_out_dir, target_uid, target_gid);
        }
    }

    recovery_stats_t stats = {0};
    time_t t_start = time(NULL);

    printf("Recovering into `%s` ...\n\n", options.output);

    if ((entry_mode & S_IFMT) == S_IFDIR) {
        process_directory(&ctx, start_oid, entry_out_dir, start_logical, 0, &stats);
    } else if ((entry_mode & S_IFMT) == S_IFREG) {
        stats.files_discovered++;
        recover_regular_file(&ctx, start_oid, entry_out_dir, start_name, start_logical, &stats);
    } else if ((entry_mode & S_IFMT) == S_IFLNK) {
        stats.files_discovered++;
        recover_symlink(&ctx, start_oid, entry_out_dir, start_name, start_logical, &stats);
    } else {
        fprintf(stderr, "ABORT: Entrypoint FSOID %#"PRIx64" is neither a directory, regular file, nor symlink (mode %#o).\n", start_oid, entry_mode);
        return EX_DATAERR;
    }
    if (entry_out_dir != options.output) {
        free(entry_out_dir);
    }

    time_t t_end = time(NULL);

    // Write the report
    char* report_path = NULL;
    asprintf(&report_path, "%s/recovery-report.txt", options.output);
    FILE* report = report_path ? fopen(report_path, "w") : NULL;

    char bytes_str[64];
    format_bytes(stats.bytes_recovered, bytes_str, sizeof(bytes_str));

    char t_start_str[64], t_end_str[64];
    strftime(t_start_str, sizeof(t_start_str), "%Y-%m-%d %H:%M:%S %Z", localtime(&t_start));
    strftime(t_end_str, sizeof(t_end_str), "%Y-%m-%d %H:%M:%S %Z", localtime(&t_end));

    FILE* outputs[2] = { stdout, report };
    for (int i = 0; i < 2; i++) {
        FILE* s = outputs[i];
        if (!s) continue;
        fprintf(s, "\n");
        fprintf(s, "APFS Automated Recovery Report\n");
        fprintf(s, "===============================\n");
        fprintf(s, "Container:      %s\n", globals.container_path);
        fprintf(s, "Volume:         %s (index %"PRId64")\n", apsb->apfs_volname, globals.volume);
        fprintf(s, "Entrypoint:     %s\n", start_logical);
        fprintf(s, "Destination:    %s\n", options.output);
        fprintf(s, "Started:        %s\n", t_start_str);
        fprintf(s, "Finished:       %s\n", t_end_str);
        fprintf(s, "\n");
        fprintf(s, "Directories discovered:        %"PRIu64"\n", stats.dirs_discovered);
        fprintf(s, "Files discovered:               %"PRIu64"\n", stats.files_discovered);
        fprintf(s, "Files successfully recovered:  %"PRIu64"\n", stats.files_recovered);
        fprintf(s, "Files partially recovered:     %"PRIu64"\n", stats.files_partial);
        fprintf(s, "Files failed:                   %"PRIu64"\n", stats.files_failed);
        fprintf(s, "Files/entries skipped:          %"PRIu64"\n", stats.files_skipped);
        fprintf(s, "Bytes recovered:                %"PRIu64" (%s)\n", stats.bytes_recovered, bytes_str);
        fprintf(s, "\n");
        fprintf(s, "Errors:\n");
        if (stats.errors.count == 0) {
            fprintf(s, "  None.\n");
        } else {
            for (size_t i = 0; i < stats.errors.count; i++) {
                fprintf(s, "%s\n", stats.errors.messages[i]);
            }
        }
    }

    if (report) fclose(report);
    if (report_path) {
        if (target_uid != (uid_t)-1) {
            chown(report_path, target_uid, target_gid);
        }
        printf("\nReport written to `%s`.\n", report_path);
        free(report_path);
    }

    for (size_t i = 0; i < stats.errors.count; i++) {
        free(stats.errors.messages[i]);
    }
    free(stats.errors.messages);

    free(fs_root_btree);
    free(fs_omap_btree);
    free(fs_omap);
    free(apsb);
    free(fs_entry);
    free(nx_omap_btree);
    free(nx_omap);
    free(nxsb);
    close_container();

    return 0;
}
