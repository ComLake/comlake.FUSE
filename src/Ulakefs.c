//
// Created by hoangdm on 20/04/2021.
//

#include <fuse.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include "Ulakefs.h"
#include "options.h"
#include "debug.h"

static struct fuse_opt ulakefs_opts[] = {
        FUSE_OPT_KEY("chroot=%s,", KEY_CHROOT),
        FUSE_OPT_KEY("cow", KEY_COW),
        FUSE_OPT_KEY("debug_file=%s", KEY_DEBUG_FILE),
        FUSE_OPT_KEY("dirs=%s", KEY_DIRS),
        FUSE_OPT_KEY("--help", KEY_HELP),
        FUSE_OPT_KEY("-h", KEY_HELP),
        FUSE_OPT_KEY("hide_meta_dir", KEY_HIDE_METADIR),
        FUSE_OPT_KEY("hide_meta_files", KEY_HIDE_META_FILES),
        FUSE_OPT_KEY("max_files=%s", KEY_MAX_FILES),
        FUSE_OPT_KEY("noinitgroups", KEY_NOINITGROUPS),
        FUSE_OPT_KEY("relaxed_permissions", KEY_RELAXED_PERMISSIONS),
        FUSE_OPT_KEY("statfs_omit_ro", KEY_STATFS_OMIT_RO),
        FUSE_OPT_KEY("--version", KEY_VERSION),
        FUSE_OPT_KEY("-V", KEY_VERSION),
        FUSE_OPT_END
};

int main(int argc, char *argv[]) {
    struct fuse_args args = FUSE_ARGS_INIT(argc, argv);

    init_syslog();
    uopt_init();

    if (fuse_opt_parse(&args, NULL, ulakefs_opts, ulakefs_opt_proc) == -1) RETURN(1);

    if (uopt.debug)	debug_init();

    if (!uopt.doexit) {
        if (uopt.nbranches == 0) {
            printf("You need to specify at least one branch!\n");
            RETURN(1);
        }
    }

    // enable fuse permission checks, we need to set this, even we we are
    // not root, since we don't have our own access() function
    uid_t uid = getuid();
    gid_t gid = getgid();
    bool default_permissions = true;

    if (uid != 0 && gid != 0 && uopt.relaxed_permissions) {
        default_permissions = false;
    } else if (uopt.relaxed_permissions) {
        // protect the user of a very critical security issue
        fprintf(stderr, "Relaxed permissions disallowed for root!\n");
        exit(1);
    }

    if (default_permissions) {
        if (fuse_opt_add_arg(&args, "-odefault_permissions")) {
            fprintf(stderr, "Severe failure, can't enable permission checks, aborting!\n");
            exit(1);
        }
    }
    ulakefs_post_opts();

#ifdef FUSE_CAP_BIG_WRITES
    /* libfuse > 0.8 supports large IO, also for reads, to increase performance
     * We support any IO sizes, so lets enable that option */
    if (fuse_opt_add_arg(&args, "-obig_writes")) {
        fprintf(stderr, "Failed to enable big writes!\n");
        exit(1);
    }
#endif

    umask(0);
    int res = fuse_main(args.argc, args.argv, &ulakefs_oper, NULL);
    RETURN(uopt.doexit ? uopt.retval : res);
}