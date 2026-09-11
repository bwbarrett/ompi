/*
 * Copyright (c) 2004-2005 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2005 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2015-2017 Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * Copyright (c) 2016-2017 Intel, Inc. All rights reserved.
 * Copyright (c) 2026      Amazon.com, Inc. or its affiliates. All Rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 * SPDX-License-Identifier: BSD-3-Clause-Open-MPI
 */

#include "opal_config.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#ifdef HAVE_UNISTD_H
#    include <unistd.h>
#endif /* HAVE_UNISTD_H */
#include <stdlib.h>
#if HAVE_SYS_STAT_H
#    include <sys/stat.h>
#endif /* HAVE_SYS_STAT_H */
#ifdef HAVE_SYS_TYPES_H
#    include <sys/types.h>
#endif /* HAVE_SYS_TYPES_H */
#ifdef HAVE_DIRENT_H
#    include <dirent.h>
#endif /* HAVE_DIRENT_H */

#include "opal/constants.h"
#include "opal/util/argv.h"
#include "opal/util/os_dirpath.h"
#include "opal/util/os_path.h"
#include "opal/util/output.h"
#include "opal/util/show_help.h"

static const char path_sep[] = OPAL_PATH_SEP;

/*
 * The final directory in a create() already existed: make sure it
 * really is a directory and that it carries at least the requested
 * mode bits.
 *
 * The inspection and the mode change are both done through a single
 * descriptor (open()/fstat()/fchmod()), so the object that gets
 * inspected is the object that gets modified.  A path-based
 * stat()/chmod() pair is the classic TOCTOU race: the name can be
 * swapped between the two calls, redirecting the chmod onto an object
 * that was never inspected.  The paths that reach this code are
 * session/rendezvous directories with predictable names, frequently
 * rooted under a world-writable /tmp, so the race is not theoretical.
 *
 * O_DIRECTORY: a plain file sitting at the requested name is an error
 * rather than something we chmod and report as usable.
 *
 * O_NOFOLLOW: refuse a symlink planted at the final component; following
 * one would point the chmod (and any later use) at the link's target,
 * which an attacker gets to choose.
 *
 * @retval OPAL_SUCCESS       It is a directory; mode is now adequate.
 * @retval OPAL_ERR_NOT_FOUND It does not exist (vanished under us).
 * @retval OPAL_ERR_PERM      It exists but the mode could not be set.
 * @retval OPAL_ERROR         It exists but is not a usable directory.
 */
static int dirpath_ensure_mode(const char *path, const mode_t mode)
{
    struct stat buf;
    int fd;

    fd = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    if (0 > fd) {
        if (ENOENT == errno) {
            return OPAL_ERR_NOT_FOUND;
        }
        /* ELOOP (symlink) and ENOTDIR (plain file) land here: something
         * that is not a directory is occupying the name. */
        return OPAL_ERROR;
    }

    if (0 != fstat(fd, &buf)) {
        close(fd);
        return OPAL_ERROR;
    }
    if (mode == (mode & buf.st_mode)) { /* already has the requested bits */
        close(fd);
        return OPAL_SUCCESS;
    }
    if (0 != fchmod(fd, buf.st_mode | mode)) {
        opal_show_help("help-opal-util.txt", "dir-mode", true, path, mode, strerror(errno));
        close(fd);
        return OPAL_ERR_PERM;
    }
    close(fd);
    return OPAL_SUCCESS;
}

int opal_os_dirpath_create(const char *path, const mode_t mode)
{
    struct stat buf;
    char **parts, *tmp;
    int i, len;
    int ret;

    if (NULL == path) { /* protect ourselves from errors */
        return (OPAL_ERR_BAD_PARAM);
    }

    /* If the path already exists, make sure it is a directory and that
     * it carries at least the requested mode.  dirpath_ensure_mode()
     * does the check and the chmod through a single descriptor so the
     * two cannot race (see the helper's comment).  OPAL_ERR_NOT_FOUND
     * means nothing is there yet, so fall through and create it. */
    ret = dirpath_ensure_mode(path, mode);
    if (OPAL_ERR_NOT_FOUND != ret) {
        return ret;
    }

    /* quick -- try to make directory */
    if (0 == mkdir(path, mode)) {
        return (OPAL_SUCCESS);
    }

    /* didn't work, so now have to build our way down the tree */
    /* Split the requested path up into its individual parts */

    parts = opal_argv_split(path, path_sep[0]);

    /* Ensure to allocate enough space for tmp: the strlen of the
       incoming path + 1 (for \0) */

    tmp = (char *) malloc(strlen(path) + 1);
    tmp[0] = '\0';

    /* Iterate through all the subdirectory names in the path,
       building up a directory name.  Check to see if that dirname
       exists.  If it doesn't, create it. */

    len = opal_argv_count(parts);
    for (i = 0; i < len; ++i) {
        if (i == 0) {
            /* If in POSIX-land, ensure that we never end a directory
               name with path_sep */

            if ('/' == path[0]) {
                strcat(tmp, path_sep);
            }
            strcat(tmp, parts[i]);
        }

        /* If it's not the first part, ensure that there's a
           preceding path_sep and then append this part */

        else {
            if (path_sep[0] != tmp[strlen(tmp) - 1]) {
                strcat(tmp, path_sep);
            }
            strcat(tmp, parts[i]);
        }

        /* Now that we have the name, try to create it */
        mkdir(tmp, mode);
        ret = errno; // save the errno for an error msg, if needed
        if (0 != stat(tmp, &buf)) {
            opal_show_help("help-opal-util.txt", "mkdir-failed", true, tmp, strerror(ret));
            opal_argv_free(parts);
            free(tmp);
            return OPAL_ERROR;
        }
        if (i == (len - 1)) {
            /* This is the directory the caller is going to use, so it
             * must carry the requested mode.  Ensure that through a
             * descriptor rather than a second path lookup, so the check
             * and the chmod cannot be raced against a swapped name. */
            ret = dirpath_ensure_mode(tmp, mode);
            if (OPAL_SUCCESS != ret) {
                if (OPAL_ERR_NOT_FOUND == ret) {
                    /* it vanished between the stat above and the open */
                    opal_show_help("help-opal-util.txt", "mkdir-failed", true, tmp,
                                   strerror(ENOENT));
                    ret = OPAL_ERROR;
                }
                opal_argv_free(parts);
                free(tmp);
                return ret;
            }
        }
    }

    /* All done */

    opal_argv_free(parts);
    free(tmp);
    return OPAL_SUCCESS;
}

/**
 * This function attempts to remove a directory along with all the
 * files in it.  If the recursive variable is non-zero, then it will
 * try to recursively remove all directories.  If provided, the
 * callback function is executed prior to the directory or file being
 * removed.  If the callback returns non-zero, then no removal is
 * done.
 */

/*
 * Empty out the directory that "fd" refers to (and, when recursive,
 * any subdirectories under it).  Takes ownership of "fd": it is closed
 * by the closedir() below before this function returns.
 *
 * Every entry is inspected and removed *relative to the open directory
 * descriptor* (fstatat()/openat()/unlinkat()) rather than by rebuilding
 * and re-resolving its path, so no entry can be swapped between the
 * point where it is classified and the point where it is acted upon:
 * the thing we looked at is always the thing we remove.  This is what
 * closes the stat()/unlink() TOCTOU race of the old path-based code.
 *
 * AT_SYMLINK_NOFOLLOW / O_NOFOLLOW keep symlinks as entries to be
 * unlinked rather than paths to be followed: unlinkat() removes the
 * link itself (never its target), and an entry swapped for a symlink
 * just before we descend into it is refused by O_NOFOLLOW instead of
 * sending the recursion outside the tree we were asked to destroy.
 *
 * "path" is the printable path of this directory; it is used only for
 * the caller's callback and for building child display paths.
 */
static int dirpath_destroy_at(int fd, const char *path, bool recursive,
                              opal_os_dirpath_destroy_callback_fn_t cbfunc)
{
    int rc, childfd, exit_status = OPAL_SUCCESS;
    bool is_dir;
    DIR *dp;
    struct dirent *ep;
    char *filenm;
    struct stat buf;

    /* fdopendir() takes ownership of fd; closedir() will close it */
    dp = fdopendir(fd);
    if (NULL == dp) {
        close(fd);
        return OPAL_ERROR;
    }

    while (NULL != (ep = readdir(dp))) {
        /* skip:
         *  - . and ..
         */
        if ((0 == strcmp(ep->d_name, ".")) || (0 == strcmp(ep->d_name, ".."))) {
            continue;
        }

        /* Check to see if it is a directory.  Resolve the entry
         * relative to the descriptor we already hold, not by name, and
         * do not follow a symlink sitting at the entry.
         */
        is_dir = false;
        if (0 != fstatat(dirfd(dp), ep->d_name, &buf, AT_SYMLINK_NOFOLLOW)) {
            /* Handle a race condition. The entry might have been deleted
             * by another process running on the same node. That
             * typically occurs when one task is removing the
             * job_session_dir and another task is still removing its
             * proc_session_dir.
             */
            continue;
        }
        if (S_ISDIR(buf.st_mode)) {
            is_dir = true;
        }

        /*
         * If not recursively descending, then if we find a directory then fail
         * since we were not told to remove it.
         */
        if (is_dir && !recursive) {
            /* Set the error indicating that we found a directory,
             * but continue removing files
             */
            exit_status = OPAL_ERROR;
            continue;
        }

        /* Will the caller allow us to remove this file/directory? */
        if (NULL != cbfunc) {
            /*
             * Caller does not wish to remove this file/directory,
             * continue with the rest of the entries
             */
            if (!(cbfunc(path, ep->d_name))) {
                continue;
            }
        }

        /* Directories are recursively destroyed */
        if (is_dir) {
            /* Descend through the descriptor we already hold, refusing a
             * symlink planted at the entry. */
            childfd = openat(dirfd(dp), ep->d_name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
            if (0 > childfd) {
                if (ENOENT != errno) {
                    /* the entry is no longer an ordinary directory --
                     * leave it alone rather than chase it */
                    exit_status = OPAL_ERROR;
                }
                continue;
            }
            /* Build the printable child path for the callback and any
             * error messages in the deeper level. */
            filenm = opal_os_path(false, path, ep->d_name, NULL);
            rc = dirpath_destroy_at(childfd, (NULL == filenm) ? ep->d_name : filenm,
                                    recursive, cbfunc);
            free(filenm);
            if (OPAL_SUCCESS != rc) {
                exit_status = rc;
                closedir(dp);
                return exit_status;
            }
            /* Remove the now-empty subdirectory.  This fails harmlessly
             * if the callback chose to preserve something inside it. */
            unlinkat(dirfd(dp), ep->d_name, AT_REMOVEDIR);
        } else {
            /* Files are removed right here */
            if (0 != unlinkat(dirfd(dp), ep->d_name, 0)) {
                exit_status = OPAL_ERROR;
            }
        }
    }

    /* Done with this directory */
    closedir(dp);

    return exit_status;
}

int opal_os_dirpath_destroy(const char *path, bool recursive,
                            opal_os_dirpath_destroy_callback_fn_t cbfunc)
{
    int fd, exit_status;

    if (NULL == path) { /* protect against error */
        return OPAL_ERROR;
    }

    /* Open the base directory.  O_NOFOLLOW refuses to descend through a
     * symlinked base path: the session directories sit under a
     * world-writable root with predictable names, so a link planted
     * there could otherwise redirect this whole recursive removal at a
     * tree of an attacker's choosing.  Enumerating and removing through
     * the resulting descriptor (in dirpath_destroy_at()) is what closes
     * the check/use race. */
    fd = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    if (0 > fd) {
        /* Preserve the documented contract: a directory that does not
         * exist is reported as OPAL_ERR_NOT_FOUND; any other failure to
         * open it (including a non-directory or a symlink at the base)
         * is a generic error. */
        if (ENOENT == errno) {
            return OPAL_ERR_NOT_FOUND;
        }
        return OPAL_ERROR;
    }

    exit_status = dirpath_destroy_at(fd, path, recursive, cbfunc);

    /*
     * If the directory is empty, then remove it
     */
    if (opal_os_dirpath_is_empty(path)) {
        rmdir(path);
    }

    return exit_status;
}

bool opal_os_dirpath_is_empty(const char *path)
{
    DIR *dp;
    struct dirent *ep;

    if (NULL != path) { /* protect against error */
        dp = opendir(path);
        if (NULL != dp) {
            while ((ep = readdir(dp))) {
                if ((0 != strcmp(ep->d_name, ".")) && (0 != strcmp(ep->d_name, ".."))) {
                    closedir(dp);
                    return false;
                }
            }
            closedir(dp);
            return true;
        }
        return false;
    }

    return true;
}

int opal_os_dirpath_access(const char *path, const mode_t in_mode)
{
    struct stat buf;
    mode_t loc_mode = S_IRWXU; /* looking for full rights */

    /*
     * If there was no mode specified, use the default mode
     */
    if (0 != in_mode) {
        loc_mode = in_mode;
    }

    if (0 == stat(path, &buf)) {                    /* exists - check access */
        if ((buf.st_mode & loc_mode) == loc_mode) { /* okay, I can work here */
            return (OPAL_SUCCESS);
        } else {
            /* Don't have access rights to the existing path */
            return (OPAL_ERROR);
        }
    } else {
        /* We could not find the path */
        return (OPAL_ERR_NOT_FOUND);
    }
}
