#include "unionfs.h"

/* Splits path into its whiteout-marker equivalent: upper_dir/dir/.wh.name */
static void build_whiteout_for_path(const char *upper_dir, const char *path, char *wh_out) {
    const char *basename_ptr = strrchr(path, '/');
    basename_ptr = basename_ptr ? basename_ptr + 1 : path;

    char dir_part[MAX_PATH_LEN];
    strncpy(dir_part, path, MAX_PATH_LEN);
    dir_part[MAX_PATH_LEN - 1] = '\0';
    char *last_slash = strrchr(dir_part, '/');
    
    if (last_slash && last_slash != dir_part) {
        *last_slash = '\0';
    } else {
        dir_part[0] = '\0'; // File is in root
    }

    /* upper_dir and dir_part are both already bounded by MAX_PATH_LEN,
     * so this never truncates in practice — gcc just can't see that
     * from the char* signature. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"
    if (dir_part[0] != '\0') {
        snprintf(wh_out, MAX_PATH_LEN, "%s%s/" WH_PREFIX "%s", upper_dir, dir_part, basename_ptr);
    } else {
        snprintf(wh_out, MAX_PATH_LEN, "%s/" WH_PREFIX "%s", upper_dir, basename_ptr);
    }
#pragma GCC diagnostic pop
}

/* Delete a file. Upper-only files get unlinked for real; anything that
 * originates in lower_dir gets a whiteout marker instead, since lower_dir
 * is never touched. */
int unionfs_unlink(const char *path) {
    struct mini_unionfs_state *data = UNIONFS_DATA;
    char upper[MAX_PATH_LEN], lower[MAX_PATH_LEN], resolved[MAX_PATH_LEN];

    snprintf(upper, MAX_PATH_LEN, "%s%s", data->upper_dir, path);
    snprintf(lower, MAX_PATH_LEN, "%s%s", data->lower_dir, path);

    if (resolve_path(path, resolved) == -ENOENT) {
        return -ENOENT;
    }

    int in_upper = (access(upper, F_OK) == 0);
    int in_lower = (access(lower, F_OK) == 0);

    /* File exists only in upper: plain unlink, nothing to hide. */
    if (in_upper && !in_lower) {
        if (unlink(upper) == -1) return -errno;
        return 0;
    }

    /* File exists in lower (possibly with a CoW copy in upper too). */
    if (in_lower) {
        char wh_path[MAX_PATH_LEN];
        build_whiteout_for_path(data->upper_dir, path, wh_path);

        if (make_parent_dirs(wh_path) < 0) return -errno;

        /* 0000 perms: nobody should be able to open this as real data */
        int fd = open(wh_path, O_CREAT | O_WRONLY | O_TRUNC, 0000);
        if (fd == -1) return -errno;
        close(fd);

        /* Drop any CoW copy so the whiteout is the only trace left */
        if (in_upper) unlink(upper);

        return 0;
    }

    return -ENOENT;
}

int unionfs_mkdir(const char *path, mode_t mode) {
    struct mini_unionfs_state *data = UNIONFS_DATA;
    char upper[MAX_PATH_LEN], wh_check[MAX_PATH_LEN];

    /* Recreating a directory that was previously whited-out should make
     * it visible again, same as unionfs_create does for files. */
    build_whiteout_for_path(data->upper_dir, path, wh_check);
    if (access(wh_check, F_OK) == 0) unlink(wh_check);

    snprintf(upper, MAX_PATH_LEN, "%s%s", data->upper_dir, path);
    if (make_parent_dirs(upper) < 0) return -errno;
    if (mkdir(upper, mode) == -1) return -errno;
    return 0;
}

int unionfs_rmdir(const char *path) {
    struct mini_unionfs_state *data = UNIONFS_DATA;
    char upper[MAX_PATH_LEN], lower[MAX_PATH_LEN];

    snprintf(upper, MAX_PATH_LEN, "%s%s", data->upper_dir, path);
    snprintf(lower, MAX_PATH_LEN, "%s%s", data->lower_dir, path);

    if (access(upper, F_OK) == 0) {
        if (rmdir(upper) == -1) return -errno;
        /* Also present in lower: whiteout it so it doesn't reappear */
        if (access(lower, F_OK) == 0) {
            char wh_path[MAX_PATH_LEN];
            build_whiteout_for_path(data->upper_dir, path, wh_path);
            int fd = open(wh_path, O_CREAT | O_WRONLY | O_TRUNC, 0000);
            if (fd != -1) close(fd);
        }
        return 0;
    }
    /* Lower-only directories can't be removed — there's no upper
     * counterpart to rmdir(), and whiting out a whole directory tree
     * isn't implemented here. */
    if (access(lower, F_OK) == 0) return -EPERM;

    return -ENOENT;
}

int unionfs_chmod(const char *path, mode_t mode, struct fuse_file_info *fi) {
    (void) fi;
    char upper[MAX_PATH_LEN], lower[MAX_PATH_LEN];
    struct mini_unionfs_state *data = UNIONFS_DATA;
    snprintf(upper, MAX_PATH_LEN, "%s%s", data->upper_dir, path);
    snprintf(lower, MAX_PATH_LEN, "%s%s", data->lower_dir, path);

    if (access(upper, F_OK) != 0) {
        if (access(lower, F_OK) == 0) {
            int ret = cow_copy(path);
            if (ret < 0) return ret;
        } else return -ENOENT;
    }
    if (chmod(upper, mode) == -1) return -errno;
    return 0;
}

int unionfs_chown(const char *path, uid_t uid, gid_t gid, struct fuse_file_info *fi) {
    (void) fi;
    char upper[MAX_PATH_LEN], lower[MAX_PATH_LEN];
    struct mini_unionfs_state *data = UNIONFS_DATA;
    snprintf(upper, MAX_PATH_LEN, "%s%s", data->upper_dir, path);
    snprintf(lower, MAX_PATH_LEN, "%s%s", data->lower_dir, path);

    if (access(upper, F_OK) != 0) {
        if (access(lower, F_OK) == 0) {
            int ret = cow_copy(path);
            if (ret < 0) return ret;
        } else return -ENOENT;
    }
    if (lchown(upper, uid, gid) == -1) return -errno;
    return 0;
}

static int unionfs_statfs(const char *path, struct statvfs *stbuf) {
    (void) path;
    if (statvfs(UNIONFS_DATA->upper_dir, stbuf) == -1)
        return -errno;
    return 0;
}

int unionfs_symlink(const char *target, const char *linkpath) {
    struct mini_unionfs_state *data = UNIONFS_DATA;
    char upper_link[MAX_PATH_LEN];

    snprintf(upper_link, MAX_PATH_LEN, "%s%s", data->upper_dir, linkpath);
    if (make_parent_dirs(upper_link) < 0) return -errno;
    if (symlink(target, upper_link) == -1) return -errno;
    return 0;
}

int unionfs_readlink(const char *path, char *buf, size_t size) {
    struct mini_unionfs_state *data = UNIONFS_DATA;
    char upper[MAX_PATH_LEN], lower[MAX_PATH_LEN];
    char linkbuf[MAX_PATH_LEN];
    ssize_t len;

    snprintf(upper, MAX_PATH_LEN, "%s%s", data->upper_dir, path);
    snprintf(lower, MAX_PATH_LEN, "%s%s", data->lower_dir, path);

    if (access(upper, F_OK) == 0) {
        len = readlink(upper, linkbuf, size - 1);
    } else if (access(lower, F_OK) == 0) {
        len = readlink(lower, linkbuf, size - 1);
    } else {
        return -ENOENT;
    }

    if (len == -1) return -errno;
    linkbuf[len] = '\0';
    strncpy(buf, linkbuf, size - 1);
    buf[size - 1] = '\0';
    return 0;
}

int unionfs_rename(const char *from, const char *to, unsigned int flags) {
    (void) flags;
    struct mini_unionfs_state *data = UNIONFS_DATA;
    char upper_from[MAX_PATH_LEN], upper_to[MAX_PATH_LEN];
    char lower_from[MAX_PATH_LEN];
    int ret;

    snprintf(upper_from, MAX_PATH_LEN, "%s%s", data->upper_dir, from);
    snprintf(upper_to, MAX_PATH_LEN, "%s%s", data->upper_dir, to);
    snprintf(lower_from, MAX_PATH_LEN, "%s%s", data->lower_dir, from);

    if (access(upper_from, F_OK) == 0) {
        if (make_parent_dirs(upper_to) < 0) return -errno;
        if (rename(upper_from, upper_to) == -1) return -errno;
        return 0;
    }

    if (access(lower_from, F_OK) == 0) {
        ret = cow_copy(from);
        if (ret < 0) return ret;
        if (make_parent_dirs(upper_to) < 0) return -errno;
        if (rename(upper_from, upper_to) == -1) return -errno;
        return 0;
    }

    return -ENOENT;
}

int unionfs_link(const char *from, const char *to) {
    struct mini_unionfs_state *data = UNIONFS_DATA;
    char upper_from[MAX_PATH_LEN], upper_to[MAX_PATH_LEN];
    char lower_from[MAX_PATH_LEN];
    int ret;

    snprintf(upper_from, MAX_PATH_LEN, "%s%s", data->upper_dir, from);
    snprintf(upper_to, MAX_PATH_LEN, "%s%s", data->upper_dir, to);
    snprintf(lower_from, MAX_PATH_LEN, "%s%s", data->lower_dir, from);

    if (access(upper_from, F_OK) != 0) {
        if (access(lower_from, F_OK) == 0) {
            ret = cow_copy(from);
            if (ret < 0) return ret;
        } else {
            return -ENOENT;
        }
    }

    if (make_parent_dirs(upper_to) < 0) return -errno;
    if (link(upper_from, upper_to) == -1) return -errno;
    return 0;
}

/* FUSE dispatch table — wires every callback to its implementation */
struct fuse_operations unionfs_oper = {
    .getattr    = unionfs_getattr,
    .readdir    = unionfs_readdir,
    .read       = unionfs_read,
    .open       = unionfs_open,
    .write      = unionfs_write,
    .create     = unionfs_create,
    .truncate   = unionfs_truncate,
    .utimens    = unionfs_utimens,
    .release    = unionfs_release,
    .unlink     = unionfs_unlink,
    .mkdir      = unionfs_mkdir,
    .rmdir      = unionfs_rmdir,
    .chmod      = unionfs_chmod,
    .chown      = unionfs_chown,
    .statfs     = unionfs_statfs,
    .symlink    = unionfs_symlink,
    .readlink   = unionfs_readlink,
    .rename     = unionfs_rename,
    .link       = unionfs_link,
};