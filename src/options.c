//
// Created by hoangdm on 20/04/2021.
//

#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <pthread.h>

#include "Ulakefs.h"
#include "options.h"
#include "debug.h"
#include "authen.h"
#include <openssl/md5.h>
#include <uuid/uuid.h>
#include <execinfo.h>

#define VERSION "1.0"
#define BT_BUF_SIZE 100

/**
 * \brief The length of a MD5SUM string
 */
#define MD5_HASH_LEN 32

/**
 * \brief The length of the salt
 * \details This is basically the length of a UUID
 */
#define SALT_LEN 36

/**
 * \brief The default maximum number of network connections
 */
#define DEFAULT_NETWORK_MAX_CONNS   10

/**
 * \brief The default HTTP 429 (too many requests) wait time
 */
#define DEFAULT_HTTP_WAIT_SEC       5
/**
 * \brief Data file block size
 * \details We set it to 1024*1024*8 = 8MiB
 */
#define DEFAULT_DATA_BLKSZ         8*1024*1024

/**
 * \brief Maximum segment block count
 * \details This is set to 128*1024 blocks, which uses 128KB. By default,
 * this allows the user to store (128*1024)*(8*1024*1024) = 1TB of data
 */
#define DEFAULT_MAX_SEGBC           128*1024

HttpStruct CONFIG;

/**
 * Set debug path
 */
void set_debug_path(char *new_path, int len)
{
    pthread_rwlock_wrlock(&uopt.dbgpath_lock); // LOCK path

    if (uopt.dbgpath) free(uopt.dbgpath);

    uopt.dbgpath = strndup(new_path, len);

    pthread_rwlock_unlock(&uopt.dbgpath_lock); // UNLOCK path
}

/**
* Check if a debug path is set
*/

static bool get_has_debug_path(void)
{
    pthread_rwlock_rdlock(&uopt.dbgpath_lock); // LOCK path

    bool has_debug_path = (uopt.dbgpath) ? true : false;

    pthread_rwlock_unlock(&uopt.dbgpath_lock); // UNLOCK path

    return has_debug_path;
}

/**
 * Enable or disable internal debugging
 */
bool set_debug_onoff(int value)
{
    bool res = false;

    if (value) {
        bool has_debug_path = get_has_debug_path();
        if (has_debug_path) {
            uopt.debug = 1;
            res = true;
        }
    } else {
        uopt.debug = 0;
        res = true;
    }

    return res;
}

/**
 * Set the maximum number of open files
 */
int set_max_open_files(const char *arg)
{
    struct rlimit rlim;
    unsigned long max_files;
    if (sscanf(arg, "max_files=%ld\n", &max_files) != 1) {
        fprintf(stderr, "%s Converting %s to number failed, aborting!\n",
                __func__, arg);
        exit(1);
    }
    rlim.rlim_cur = max_files;
    rlim.rlim_max = max_files;
    if (setrlimit(RLIMIT_NOFILE, &rlim)) {
        fprintf(stderr, "%s: Setting the maximum number of files failed: %s\n",
                __func__, strerror(errno));
        exit(1);
    }

    return 0;
}

uoptions_t uopt;

void uopt_init() {
    memset(&uopt, 0, sizeof(uopt)); // initialize options with zeros first

    pthread_rwlock_init(&uopt.dbgpath_lock, NULL);
}

/**
 * Take a relative path as argument and return the absolute path by using the
 * current working directory. The return string is malloc'ed with this function.
 */
char *make_absolute(char *relpath) {
    // Already an absolute path
    if (*relpath == '/') return relpath;

    char cwd[PATHLEN_MAX];
    if (!getcwd(cwd, PATHLEN_MAX)) {
        perror("Unable to get current working directory");
        return NULL;
    }

    size_t cwdlen = strlen(cwd);
    if (!cwdlen) {
        fprintf(stderr, "Zero-sized length of CWD!\n");
        return NULL;
    }

    // 2 due to: +1 for '/' between cwd and relpath
    //           +1 for trailing '/'
    int abslen = cwdlen + strlen(relpath) + 2;
    if (abslen > PATHLEN_MAX) {
        fprintf(stderr, "Absolute path too long!\n");
        return NULL;
    }

    char *abspath = malloc(abslen);
    if (abspath == NULL) {
        fprintf(stderr, "%s: malloc failed\n", __func__);
        exit(1); // still at early stage, we can abort
    }

    // the ending required slash is added later by add_trailing_slash()
    snprintf(abspath, abslen, "%s/%s", cwd, relpath);

    return abspath;
}

/**
 * Add a trailing slash at the end of a branch. So functions using this
 * path don't have to care about this slash themselves.
 **/
char *add_trailing_slash(char *path) {
    int len = strlen(path);
    if (path[len - 1] == '/') {
        return path; // no need to add a slash, already there
    }

    path = realloc(path, len + 2); // +1 for '/' and +1 for '\0'
    if (!path) {
        fprintf(stderr, "%s: realloc() failed, aborting\n", __func__);
        exit(1); // still very early stage, we can abort here
    }

    strcat(path, "/");
    return path;
}

/**
 * Add a given branch and its options to the array of available branches.
 * example branch string "branch1=RO" or "/path/path2=RW"
 */
void add_branch(char *branch) {
    uopt.branches = realloc(uopt.branches, (uopt.nbranches+1) * sizeof(branch_entry_t));
    if (uopt.branches == NULL) {
        fprintf(stderr, "%s: realloc failed\n", __func__);
        exit(1); // still at early stage, we can't abort
    }

    char *res;
    char **ptr = (char **)&branch;

    res = strsep(ptr, "=");
    if (!res) return;

    // for string manipulations it is important to copy the string, otherwise
    // make_absolute() and add_trailing_slash() will corrupt our input (parse string)
    uopt.branches[uopt.nbranches].path = strdup(res);
    uopt.branches[uopt.nbranches].rw = 0;

    res = strsep(ptr, "=");
    if (res) {
        if (strcasecmp(res, "rw") == 0) {
            uopt.branches[uopt.nbranches].rw = 1;
        } else if (strcasecmp(res, "ro") == 0) {
            // no action needed here
        } else {
            fprintf(stderr, "Failed to parse RO/RW flag, setting RO.\n");
            // no action needed here either
        }
    }

    uopt.nbranches++;
}

/**
 * These options define our branch paths.
 * example arg string: "branch1=RW:branch2=RO:branch3=RO"
 */
int parse_branches(const char *arg) {
    // the last argument is our mountpoint, don't take it as branch!
    if (uopt.nbranches) return 0;

    // We don't free the buf as parts of it may go to branches
    char *buf = strdup(arg);
    char **ptr = (char **)&buf;
    char *branch;
    while ((branch = strsep(ptr, ROOT_SEP)) != NULL) {
        if (strlen(branch) == 0) continue;

        add_branch(branch);
    }

    free(branch);
    free(buf);

    return uopt.nbranches;
}

/**
  * get_opt_str - get the parameter string
  * @arg	- option argument
  * @opt_name	- option name, used for error messages
  * fuse passes arguments with the argument prefix, e.g.
  * "-o chroot=/path/to/chroot/" will give us "chroot=/path/to/chroot/"
  * and we need to cut off the "chroot=" part
  * NOTE: If the user specifies a relative path of the branches
  *       to the chroot, it is absolutely required
  *       -o chroot=path is provided before specifying branches!
  */
static char * get_opt_str(const char *arg, char *opt_name)
{
    char *str = index(arg, '=');

    if (!str) {
        fprintf(stderr, "-o %s parameter not properly specified, aborting!\n",
                opt_name);
        exit(1); // still early phase, we can abort
    }

    if (strlen(str) < 3) {
        fprintf(stderr, "%s path has not sufficient characters, aborting!\n",
                opt_name);
        exit(1);
    }

    str++; // just jump over the '='

    // copy of the given parameter, just in case something messes around
    // with command line parameters later on
    str = strdup(str);
    if (!str) {
        fprintf(stderr, "strdup failed: %s Aborting!\n", strerror(errno));
        exit(1);
    }
    return str;
}

static void print_help(const char *progname) {
    printf(
            "Usage: %s [options] branch[=RO/RW][:branch...] mountpoint\n"
               "The first argument is a colon separated list of directories to merge\n"
               "When neither RO nor RW is specified, selection defaults to RO.\n"
               "\n"
               "general options:\n"
               "    -d                     Enable debug output\n"
               "    -o opt,[opt...]        mount options\n"
               "    -h   --help            print help\n"
               "    -V   --version         print version\n"
               "\n"
               "UlakeFuse options:\n"
               "    -o chroot=path         chroot into this path. Use this if you \n"
               "                           want to have a union of \"/\" \n"
               "    -o cow                 enable copy-on-write\n"
               "                           mountpoint\n"
               "    -o debug_file          file to write debug information into\n"
               "    -o dirs=branch[=RO/RW][:branch...]\n"
               "                           alternate way to specify directories to merge\n"
               "    -o hide_meta_files     \".ulakefs\" is a secret directory not\n"
               "                           visible by readdir(), and so are\n"
               "                           .fuse_hidden* files\n"
               "    -o max_files=number    Increase the maximum number of open files\n"
               "    -o relaxed_permissions Disable permissions checks, but only if\n"
               "                           running neither as UID=0 or GID=0\n"
               "    -o statfs_omit_ro      do not count blocks of ro-branches\n"
               "\n",
               progname);
}


/**
  * This method is to post-process options once we know all of them
  */
void ulakefs_post_opts(void) {
    // chdir to the given chroot, we
    if (uopt.chroot) {
        int res = chdir(uopt.chroot);
        if (res) {
            fprintf(stderr, "Chdir to %s failed: %s ! Aborting!\n",
                    uopt.chroot, strerror(errno));
            exit(1);
        }
    }
    // Make the paths absolute and add trailing slashes
    int i;
    for (i = 0; i<uopt.nbranches; i++) {
        // if -ochroot= is specified, the path has to be given absolute
        // or relative to the chroot, so no need to make it absolute
        // also won't work, since we are not yet in the chroot here
        if (!uopt.chroot) {
            uopt.branches[i].path = make_absolute(uopt.branches[i].path);
        }
        uopt.branches[i].path = add_trailing_slash(uopt.branches[i].path);

        // Prevent accidental umounts. Especially system shutdown scripts tend
        // to umount everything they can. If we don't have an open file descriptor,
        // this might cause unexpected behaviour.
        char path[PATHLEN_MAX];

        if (!uopt.chroot) {
            BUILD_PATH(path, uopt.branches[i].path);
        } else {
            BUILD_PATH(path, uopt.chroot, uopt.branches[i].path);
        }

        int fd = open(path, O_RDONLY);
        if (fd == -1) {
            fprintf(stderr, "\nFailed to open %s: %s. Aborting!\n\n",
                    path, strerror(errno));
            exit(1);
        }
        uopt.branches[i].fd = fd;
        uopt.branches[i].path_len = strlen(path);
    }
}

int ulakefs_opt_proc(void *data, const char *arg, int key, struct fuse_args *outargs) {
    (void)data;

    int res = 0; // for general purposes

    switch (key) {
        case FUSE_OPT_KEY_NONOPT:
            res = parse_branches(arg);
            if (res > 0) return 0;
            uopt.retval = 1;
            return 1;
        case KEY_DIRS:
            // skip the "dirs="
            res = parse_branches(arg+5);
            if (res > 0) return 0;
            uopt.retval = 1;
            return 1;
        case KEY_CHROOT:
            uopt.chroot = get_opt_str(arg, "chroot");
            return 0;
        case KEY_COW:
            uopt.cow_enabled = true;
            return 0;
        case KEY_DEBUG_FILE:
            uopt.dbgpath = get_opt_str(arg, "debug_file");
            uopt.debug = true;
            return 0;
        case KEY_HELP:
            print_help(outargs->argv[0]);
            fuse_opt_add_arg(outargs, "-ho");
            uopt.doexit = 1;
            return 0;
        case KEY_HIDE_META_FILES:
        case KEY_HIDE_METADIR:
            uopt.hide_meta_files = true;
            return 0;
        case KEY_MAX_FILES:
            set_max_open_files(arg);
            return 0;
        case KEY_NOINITGROUPS:
            return 0;
        case KEY_STATFS_OMIT_RO:
            uopt.statfs_omit_ro = true;
            return 0;
        case KEY_RELAXED_PERMISSIONS:
            uopt.relaxed_permissions = true;
            return 0;
        case KEY_VERSION:
            printf("ulake-fuse version: "VERSION"\n");
            uopt.doexit = 1;
            return 1;
        default:
            uopt.retval = 1;
            return 1;
    }
}

/**
 * Check if the given fname suffixes the hide tag
 */
char *whiteout_tag(const char *fname) {
    DBG("%s\n", fname);

    char *tag = strstr(fname, HIDETAG);

    // check if fname has tag, fname is not only the tag, file name ends with the tag
    // TODO: static strlen(HIDETAG)
    if (tag && tag != fname && strlen(tag) == strlen(HIDETAG)) {
        return tag;
    }

    return NULL;
}

/**
 * copy one or more char arrays into dest and check for maximum size
 *
 * arguments: maximal string length and one or more char* string arrays
 *
 * check if the sum of the strings is larger than PATHLEN_MAX
 *
 * This function requires a NULL as last argument!
 *
 * path already MUST have been allocated!
 */
int build_path(char *path, int max_len, const char *callfunc, int line, ...) {
    va_list ap; // argument pointer
    int len = 0;
    char *str_ptr = path;

    (void)str_ptr; // please the compile to avoid warning in non-debug mode
    (void)line;
    (void)callfunc;

    path[0] = '\0'; // that way can easily strcat even the first element

    va_start(ap, line);
    while (1) {
        char *str = va_arg (ap, char *);
        //char *str = va_arg (ap, char *); // the next path element
        if (!str) break;

        /* Prevent '//' in paths, if len > 0 we are not in the first
         * loop-run. This is rather ugly, but I don't see another way to
         * make sure there really is a '/'. By simply cutting off
         * the initial '/' of the added string, we could run into a bug
         * and would not have a '/' between path elements at all
         * if somewhere else a directory slash has been forgotten... */
        if (len > 0) {
            // walk to the end of path
            while (*path != '\0') path++;

            // we are on '\0', now go back to the last char
            path--;

            if (*path == '/') {
                int count = len;

                // count makes sure nobody tricked us and gave
                // slashes as first path only...
                while (*path == '/' && count > 1) {
                    // possibly there are several slashes...
                    // But we want only one slash
                    path--;
                    count--;
                }

                // now we are *before* '/', walk to slash again
                path++;

                // eventually we walk over the slashes of the
                // next string
                while (*str == '/') str++;
            } else if (*str != '/') {
                // neither path ends with a slash, nor str
                // starts with a slash, prevent a wrong path
                strcat(path, "/");
                len++;
            }
        }

        len += strlen(str);

        // +1 for final \0 not counted by strlen
        if (len + 1 > max_len) {
            va_end(ap);
            USYSLOG (LOG_WARNING, "%s():%d Path too long \n", callfunc, line);
            errno = ENAMETOOLONG;
            RETURN(-errno);
        }

        strcat (path, str);
    }
    va_end(ap);

    if (len == 0) {
        USYSLOG(LOG_ERR, "from: %s():%d : No argument given?\n", callfunc, line);
        errno = EIO;
        RETURN(-errno);
    }

    DBG("from: %s():%d path: %s\n", callfunc, line, str_ptr);
    RETURN(0);
}

/**
 * dirname() in libc might not be thread-save, at least the man page states
 * "may return pointers to statically allocated memory", so we need our own
 * implementation
 */
char *u_dirname(const char *path) {
    DBG("%s\n", path);

    char *ret = strdup(path);
    if (ret == NULL) {
        USYSLOG(LOG_WARNING, "strdup failed, probably out of memory!\n");
        return ret;
    }

    char *ri = strrchr(ret, '/');
    if (ri != NULL) {
        *ri = '\0'; // '/' found, so a full path
    } else {
        strcpy(ret, "."); // '/' not found, so path is only a file
    }

    return ret;
}

/**
 * general elf hash (32-bit) function
 *
 * Algorithm taken from URL: http://www.partow.net/programming/hashfunctions/index.html,
 * but rewritten from scratch due to incompatible license.
 *
 * str needs to NULL terminated
 */
static unsigned int elfhash(const char *str) {
    DBG("%s\n", str);

    unsigned int hash = 0;

    while (*str) {
        hash = (hash << 4) + (*str); // hash * 16 + c

        // 0xF is 1111 in dual system, so highbyte is the highest byte of hash (which is 32bit == 4 Byte)
        unsigned int highbyte = hash & 0xF0000000UL;

        if (highbyte != 0) hash ^= (highbyte >> 24);
        // example (if the condition is met):
        //               hash = 10110000000000000000000010100000
        //           highbyte = 10110000000000000000000000000000
        //   (highbyte >> 24) = 00000000000000000000000010110000
        // after XOR:    hash = 10110000000000000000000000010000

        hash &= ~highbyte;
        //          ~highbyte = 01001111111111111111111111111111
        // after AND:    hash = 00000000000000000000000000010000

        str++;
    }

    return hash;
}

/**
 * Just a hash wrapper function, this way we can easily exchange the default hash algorithm.
 */
unsigned int string_hash(void *s) {
    return elfhash(s);
}

void Config_init(void) {
    /*---------------- Network related --------------*/
    CONFIG.http_username = NULL; // Theo api cua thao

    CONFIG.http_password = NULL;

    CONFIG.proxy = NULL;

    CONFIG.proxy_username = NULL;

    CONFIG.proxy_password = NULL;

    CONFIG.max_conns = DEFAULT_NETWORK_MAX_CONNS;

    CONFIG.http_wait_sec = DEFAULT_HTTP_WAIT_SEC;

    CONFIG.no_range_check = 0;

    CONFIG.insecure_tls = 0;
}
/*--------------- Cache related ---------------*/

char *path_append(const char *path, const char *filename)
{
    int needs_separator = 0;
    if ((path[strnlen(path, MAX_PATH_LEN)-1] != '/') && (filename[0] != '/')) {
        needs_separator = 1;
    }

    char *str;
    size_t ul = strnlen(path, MAX_PATH_LEN);
    size_t sl = strnlen(filename, MAX_FILENAME_LEN);
    str = CALLOC(ul + sl + needs_separator + 1, sizeof(char));
    strncpy(str, path, ul);
    if (needs_separator) {
        str[ul] = '/';
    }
    strncat(str, filename, sl);
    return str;
}

int64_t round_div(int64_t a, int64_t b)
{
    return (a + (b / 2)) / b;
}

void PTHREAD_MUTEX_UNLOCK(pthread_mutex_t *x)
{
    int i;
    i = pthread_mutex_unlock(x);
    if (i) {
        fprintf(stderr, "thread %lu: pthread_mutex_unlock() failed, %d, %s\n",
                pthread_self(), i, strerror(i));
        exit_failure();
    }
}

void PTHREAD_MUTEX_LOCK(pthread_mutex_t *x)
{
    int i;
    i = pthread_mutex_lock(x);
    if (i) {
        fprintf(stderr, "thread %lu: pthread_mutex_lock() failed, %d, %s\n",
                pthread_self(), i, strerror(i));
        exit_failure();
    }
}

void exit_failure(void)
{
    int nptrs;
    void *buffer[BT_BUF_SIZE];

    nptrs = backtrace(buffer, BT_BUF_SIZE);
    fprintf(stderr, "\nOops! HTTPDirFS crashed! :(\n");
    fprintf(stderr, "backtrace() returned the following %d addresses:\n",
            nptrs);
    backtrace_symbols_fd(buffer, nptrs, STDERR_FILENO);

    exit(EXIT_FAILURE);
}

void erase_string(FILE *file, size_t max_len, char *s)
{
    size_t l = strnlen(s, max_len);
    for (size_t k = 0; k < l; k++) {
        fprintf(file, "\b");
    }
    for (size_t k = 0; k < l; k++) {
        fprintf(file, " ");
    }
    for (size_t k = 0; k < l; k++) {
        fprintf(file, "\b");
    }
}

char *generate_salt(void)
{
    char *out;
    out = CALLOC(SALT_LEN + 1, sizeof(char));
    uuid_t uu;
    uuid_generate(uu);
    uuid_unparse(uu, out);
    return out;
}

char *generate_md5sum(const char *str)
{
    MD5_CTX c;
    unsigned char md5[MD5_DIGEST_LENGTH];
    size_t len = strnlen(str, MAX_PATH_LEN);
    char *out = CALLOC(MD5_HASH_LEN + 1, sizeof(char));

    MD5_Init(&c);
    MD5_Update(&c, str, len);
    MD5_Final(md5, &c);

    for(int i = 0; i < MD5_DIGEST_LENGTH; i++) {
        sprintf(out + 2 * i, "%02x", md5[i]);
    }
    return out;
}

void *CALLOC(size_t nmemb, size_t size)
{
    void *ptr = calloc(nmemb, size);
    if (!ptr) {
        fprintf(stderr, "calloc() failed, %s!\n", strerror(errno));
        exit_failure();
    }
    return ptr;
}

char *str_to_hex(char *s)
{
    char *hex = CALLOC(strlen(s) * 2 + 1, sizeof(char));
    for (char *c = s, *h = hex; *c; c++, h+=2) {
        sprintf(h, "%x", *c);
    }
    return hex;
}
