//
// Created by hoangdm on 18/05/2021.
//

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <dirent.h>
#include <errno.h>
#include <sys/statvfs.h>
#include <stdbool.h>
#include "Ulakefs.h"
#include "options.h"
#include "debug.h"
#include "hashtable.h"
#include "general.h"
#include "readrmdir.h"

/**
  * Hide metadata. This causes a slight slowdown this is optional
  *
  */
static bool hide_meta_files(int branch, const char *path, struct dirent *de)
{

    if (uopt.hide_meta_files == false) RETURN(false);

    fprintf(stderr, "uopt.branches[branch].path = %s path = %s\n", uopt.branches[branch].path, path);
    fprintf(stderr, "METANAME = %s, de->d_name = %s\n", METANAME, de->d_name);

    // TODO Would it be faster to add hash comparison?

    // HIDE out .ulakefs directory
    if (strcmp(uopt.branches[branch].path, path) == 0
        && strcmp(METANAME, de->d_name) == 0) {
        RETURN(true);
    }

    // HIDE fuse META files
    if (strncmp(FUSE_META_FILE, de->d_name, FUSE_META_LENGTH) == 0) {
        RETURN(true);
    }

    RETURN(false);
}

/**
 * Check if fname has a hiding tag and return its status.
 * Also, add this file and to the hiding hash table.
 * Warning: If fname has the tag, fname gets modified.
 */
static bool is_hiding(struct hashtable *hides, char *fname) {
    DBG("%s\n", fname);

    char *tag;

    tag = whiteout_tag(fname);
    if (tag) {
        // even more important, ignore the file without the tag!
        // hint: tag is a pointer to the flag-suffix within de->d_name
        *tag = '\0'; // this modifies fname!

        // add to hides (only if not there already)
        if (!hashtable_search(hides, fname)) {
            char *key = strdup(fname);
            hashtable_insert(hides, key, key);
        }

        RETURN(true);
    }

    RETURN(false);
}

/**
 * Read whiteout files
 */
static void read_whiteouts(const char *path, struct hashtable *whiteouts, int branch) {
    DBG("%s\n", path);

    char p[PATHLEN_MAX];
    if (BUILD_PATH(p, uopt.branches[branch].path, METADIR, path)) return;

    DIR *dp = opendir(p);
    if (dp == NULL) return;

    struct dirent *de;
    while ((de = readdir(dp)) != NULL) {
        is_hiding(whiteouts, de->d_name);
    }

    closedir(dp);
}

/**
 * Readdir function
 */

int ulakefs_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi) {
    DBG("%s\n", path);

    (void)offset;
    (void)fi;
    int i = 0;
    int rc = 0;

    // we will store already added files here to handle same file names across different branches
    struct hashtable *files = create_hashtable(16, string_hash, string_equal);

    struct hashtable *whiteouts = NULL;

    if (uopt.cow_enabled) whiteouts = create_hashtable(16, string_hash, string_equal);

    bool subdir_hidden = false;

    for (i = 0; i < uopt.nbranches; i++) {
        if (subdir_hidden) break;

        char p[PATHLEN_MAX];
        if (BUILD_PATH(p, uopt.branches[i].path, path)) {
            rc = -ENAMETOOLONG;
            goto out;
        }

        // check if branches below this branch are hidden
        int res = path_hidden(path, i);
        if (res < 0) {
            rc = res; // error
            goto out;
        }

        if (res > 0) subdir_hidden = true;

        DIR *dp = opendir(p);
        if (dp == NULL) {
            if (uopt.cow_enabled) read_whiteouts(path, whiteouts, i);
            continue;
        }

        struct dirent *de;
        while ((de = readdir(dp)) != NULL) {
            // already added in some other branch
            if (hashtable_search(files, de->d_name) != NULL) continue;

            // check if we need file hiding
            if (uopt.cow_enabled) {
                // file should be hidden from the user
                if (hashtable_search(whiteouts, de->d_name) != NULL) continue;
            }

            if (hide_meta_files(i, p, de) == true) continue;

            // fill with something dummy, we're interested in key existence only
            char *key = strdup(de->d_name);
            hashtable_insert(files, key, key);

            struct stat st;
            memset(&st, 0, sizeof(st));
            st.st_ino = de->d_ino;
            st.st_mode = de->d_type << 12;

            if (filler(buf, de->d_name, &st, 0)) break;
        }

        closedir(dp);
        if (uopt.cow_enabled) read_whiteouts(path, whiteouts, i);
    }

    out:
    hashtable_destroy(files, 0);

    if (uopt.cow_enabled) hashtable_destroy(whiteouts, 0);

    RETURN(rc);
}

/**
 * check if a directory on all paths is empty
 * return 0 if empty, 1 if not and negative value on error
 */
int dir_not_empty(const char *path) {

    DBG("%s\n", path);

    int i = 0;
    int rc = 0;
    int not_empty = 0;

    struct hashtable *whiteouts = NULL;

    if (uopt.cow_enabled) whiteouts = create_hashtable(16, string_hash, string_equal);

    bool subdir_hidden = false;

    for (i = 0; i < uopt.nbranches; i++) {
        if (subdir_hidden) break;

        char p[PATHLEN_MAX];
        if (BUILD_PATH(p, uopt.branches[i].path, path)) {
            rc = -ENAMETOOLONG;
            goto out;
        }

        // check if branches below this branch are hidden
        int res = path_hidden(path, i);
        if (res < 0) {
            rc = res; // error
            goto out;
        }

        if (res > 0) subdir_hidden = true;

        DIR *dp = opendir(p);
        if (dp == NULL) {
            if (uopt.cow_enabled) read_whiteouts(path, whiteouts, i);
            continue;
        }

        struct dirent *de;
        while ((de = readdir(dp)) != NULL) {
            // Ignore . and ..
            if ((strcmp(de->d_name, ".") == 0) ||  (strcmp(de->d_name, "..") == 0)) {
                continue;
            }

            // check if we need file hiding
            if (uopt.cow_enabled) {
                // file should be hidden from the user
                if (hashtable_search(whiteouts, de->d_name) != NULL) continue;
            }

            if (hide_meta_files(i, p, de) == true) continue;

            // When we arrive here, a valid entry was found
            not_empty = 1;
            closedir(dp);
            goto out;
        }

        closedir(dp);
        if (uopt.cow_enabled) read_whiteouts(path, whiteouts, i);
    }

    out:
    if (uopt.cow_enabled) hashtable_destroy(whiteouts, 0);

    if (rc) RETURN(rc);

    RETURN(not_empty);
}

/**
  * If the branch that has the directory to be removed is in read-write mode,
  * we can delete the file.
  */
static int rmdir_rw(const char *path, int branch_rw) {
    DBG("%s\n", path);

    char p[PATHLEN_MAX];
    if (BUILD_PATH(p, uopt.branches[branch_rw].path, path)) return ENAMETOOLONG;

    int res = rmdir(p);
    if (res == -1) return errno;

    return 0;
}

/**
  * If the branch that has the directory to be removed is in read-only mode,
  * we create a file with a HIDE tag in an upper level branch.
  * To other fuse functions this tag means, not to expose the
  * lower level directory.
  */
static int rmdir_ro(const char *path, int branch_ro) {
    DBG("%s\n", path);

    // find a writable branch above branch_ro
    int branch_rw = find_lowest_rw_branch(branch_ro);

    if (branch_rw < 0) return -EACCES;

    DBG("Calling hide_dir\n");
    if (hide_dir(path, branch_rw) == -1) {
        switch (errno) {
            case (EEXIST):
            case (ENOTDIR):
            case (ENOTEMPTY):
                // catch errors not allowed for rmdir()
                USYSLOG (LOG_ERR, "%s: Creating the whiteout failed: %s\n",
                         __func__, strerror(errno));
                errno = EFAULT;
        }
        return errno;
    }

    return 0;
}

/**
  * rmdir() call
  */
int ulakefs_rmdir(const char *path) {
    DBG("%s\n", path);

    if (dir_not_empty(path)) return -ENOTEMPTY;

    int i = find_rorw_branch(path);
    if (i == -1) return -errno;

    int res;
    if (!uopt.branches[i].rw) {
        // read-only branch
        if (!uopt.cow_enabled) {
            res = EROFS;
        } else {
            res = rmdir_ro(path, i);
        }
    } else {
        // read-write branch
        res = rmdir_rw(path, i);
        if (res == 0) {
            // No need to be root, whiteouts are created as root!
            maybe_whiteout(path, i, WHITEOUT_DIR);
        }
    }

    return -res;
}

/**
  * If the branch that has the file to be unlinked is in read-only mode,
  * we create a file with a HIDE tag in an upper level branch.
  * To other fuse functions this tag means, not to expose the
  * lower level file.
  */
static int unlink_ro(const char *path, int branch_ro) {
    DBG("%s\n", path);

    // find a writable branch above branch_ro
    int branch_rw = find_lowest_rw_branch(branch_ro);

    if (branch_rw < 0) RETURN(EACCES);

    if (hide_file(path, branch_rw) == -1) {
        // creating the file with the hide tag failed
        // TODO: open() error messages are not optimal on unlink()
        RETURN(errno);
    }

    RETURN(0);
}

/**
  * If the branch that has the file to be unlinked is in read-write mode,
  * we can really delete the file.
  */
static int unlink_rw(const char *path, int branch_rw) {
    DBG("%s\n", path);

    char p[PATHLEN_MAX];
    if (BUILD_PATH(p, uopt.branches[branch_rw].path, path)) RETURN(ENAMETOOLONG);

    int res = unlink(p);
    if (res == -1) RETURN(errno);

    RETURN(0);
}

/**
  * unlink() call
  */
int ulakefs_unlink(const char *path) {
    DBG("%s\n", path);
    int i = find_rorw_branch(path);
    if (i == -1) RETURN(errno);

    int res;
    if (!uopt.branches[i].rw) {
        // read-only branch
        if (!uopt.cow_enabled) {
            res = EROFS;
        } else {
            res = unlink_ro(path, i);
        }
    } else {
        // read-write branch
        res = unlink_rw(path, i);
        if (res == 0) {
            // No need to be root, whiteouts are created as root!
            maybe_whiteout(path, i, WHITEOUT_FILE);
        }
    }

    RETURN(-res);
}
