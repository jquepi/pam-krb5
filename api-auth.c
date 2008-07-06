/*
 * Implements the PAM auth group API (pam_sm_authenticate and pam_sm_setcred).
 *
 * The former does and checks the authentication, and the latter creates the
 * final ticket cache and sets its permissions appropriately.  pam_sm_setcred
 * can also refresh an existing ticket cache or destroy a ticket cache,
 * depending on the flags passed in.
 *
 * Copyright 2005, 2006, 2007, 2008 Russ Allbery <rra@debian.org>
 * Copyright 2005 Andres Salomon <dilinger@debian.org>
 * Copyright 1999, 2000 Frank Cusack <fcusack@fcusack.com>
 * See LICENSE for licensing terms.
 */

/* Get the prototypes for the authentication functions. */
#define PAM_SM_AUTH

#include "config.h"

#include <errno.h>
#include <fcntl.h>
#include <krb5.h>
#include <limits.h>
#include <pwd.h>
#ifdef HAVE_SECURITY_PAM_APPL_H
# include <security/pam_appl.h>
# include <security/pam_modules.h>
#elif HAVE_PAM_PAM_APPL_H
# include <pam/pam_appl.h>
# include <pam/pam_modules.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#include "internal.h"

/*
 * Given a cache name and an existing cache, initialize a new cache, store the
 * credentials from the existing cache in it, and return a pointer to the new
 * cache in the cache argument.  Returns a PAM success or error code.
 */
static int
cache_init_from_cache(struct pam_args *args, const char *ccname,
                      krb5_ccache old, krb5_ccache *cache)
{
    struct context *ctx;
    krb5_creds creds;
    krb5_cc_cursor cursor;
    int pamret;
    krb5_error_code status;

    memset(&creds, 0, sizeof(creds));
    if (args == NULL || args->ctx == NULL || args->ctx->context == NULL)
        return PAM_SERVICE_ERR;
    if (old == NULL)
        return PAM_SERVICE_ERR;
    ctx = args->ctx;
    status = krb5_cc_start_seq_get(ctx->context, old, &cursor);
    if (status != 0) {
        pamk5_debug_krb5(args, "krb5_cc_start_seq_get", status);
        return PAM_SERVICE_ERR;
    }
    status = krb5_cc_next_cred(ctx->context, old, &cursor, &creds);
    if (status != 0) {
        pamk5_debug_krb5(args, "krb5_cc_next_cred", status);
        pamret = PAM_SERVICE_ERR;
        goto done;
    }
    pamret = pamk5_cache_init(args, ccname, &creds, cache);
    if (pamret != PAM_SUCCESS) {
        krb5_free_cred_contents(ctx->context, &creds);
        pamret = PAM_SERVICE_ERR;
        goto done;
    }
    krb5_free_cred_contents(ctx->context, &creds);

    /*
     * There probably won't be any additional credentials, but check for them
     * and copy them just in case.
     */
    while (krb5_cc_next_cred(ctx->context, old, &cursor, &creds) == 0) {
        status = krb5_cc_store_cred(ctx->context, *cache, &creds);
        krb5_free_cred_contents(ctx->context, &creds);
        if (status != 0) {
            pamk5_debug_krb5(args, "krb5_cc_store_cred", status);
            pamret = PAM_SERVICE_ERR;
            goto done;
        }
    }
    pamret = PAM_SUCCESS;

done:
    krb5_cc_end_seq_get(ctx->context, ctx->cache, &cursor);
    if (pamret != PAM_SUCCESS && *cache != NULL)
        krb5_cc_destroy(ctx->context, *cache);
    return pamret;
}


/*
 * Authenticate a user via Kerberos 5.
 *
 * It would be nice to be able to save the ticket cache temporarily as a
 * memory cache and then only write it out to disk during the session
 * initialization.  Unfortunately, OpenSSH 4.2 does PAM authentication in a
 * subprocess and therefore has no saved module-specific data available once
 * it opens a session, so we have to save the ticket cache to disk and store
 * in the environment where it is.
 */
int
pam_sm_authenticate(pam_handle_t *pamh, int flags, int argc,
                    const char **argv)
{
    struct context *ctx = NULL;
    struct pam_args *args;
    krb5_creds *creds = NULL;
    int pamret;
    char cache_name[] = "/tmp/krb5cc_pam_XXXXXX";
    int set_context = 0;

    args = pamk5_args_parse(pamh, flags, argc, argv);
    if (args == NULL) {
        pamk5_error(NULL, "cannot allocate memory: %s", strerror(errno));
        pamret = PAM_SERVICE_ERR;
        goto done;
    }
    ENTRY(args, flags);
    pamret = pamk5_context_new(args);
    if (pamret != PAM_SUCCESS)
        goto done;
    ctx = args->ctx;

    /* Check whether we should ignore this user. */
    if (pamk5_should_ignore(args, ctx->name)) {
        pamret = PAM_USER_UNKNOWN;
        goto done;
    }

    /* Do the actual authentication. */
    pamret = pamk5_password_auth(args, NULL, &creds);
    if (pamret == PAM_NEW_AUTHTOK_REQD && args->defer_pwchange) {
        pamk5_debug(args, "expired account, deferring failure");
        ctx->expired = 1;
        pamret = PAM_SUCCESS;
    }
    if (pamret != PAM_SUCCESS)
        goto done;

    /* Check .k5login. */
    if (!ctx->expired) {
        pamret = pamk5_authorized(args);
        if (pamret != PAM_SUCCESS) {
            pamk5_debug(args, "failed authorization check");
            goto done;
        }
    }

    /* Reset PAM_USER in case we canonicalized. */
    if (!ctx->expired) {
        pamret = pam_set_item(args->pamh, PAM_USER, ctx->name);
        if (pamret != PAM_SUCCESS)
            pamk5_debug_pam(args, "cannot set PAM_USER", pamret);
    }

    /* Now that we know we're successful, we can store the context. */
    pamret = pam_set_data(pamh, "pam_krb5", ctx, pamk5_context_destroy);
    if (pamret != PAM_SUCCESS) {
        pamk5_context_free(ctx);
        pamret = PAM_SERVICE_ERR;
        goto done;
    }
    set_context = 1;

    /*
     * If we have an expired account or if we're not creating a ticket cache,
     * we're done.
     */
    if (args->no_ccache || ctx->expired)
        goto done;

    /* Store the obtained credentials in a temporary cache. */
    pamret = pamk5_cache_mkstemp(args, cache_name);
    if (pamret != PAM_SUCCESS)
        goto done;
    pamret = pamk5_cache_init(args, cache_name, creds, &ctx->cache);
    if (pamret != PAM_SUCCESS)
        goto done;
    pamret = pamk5_set_krb5ccname(args, cache_name, "PAM_KRB5CCNAME");

done:
    if (creds != NULL) {
        krb5_free_cred_contents(ctx->context, creds);
        free(creds);
    }
    EXIT(args, pamret);

    /*
     * Clear the context on failure so that the account management module
     * knows that we didn't authenticate with Kerberos.  Only clear the
     * context if we set it.  Otherwise, we may be blowing away the context of
     * a previous successful authentication.
     */
    if (pamret != PAM_SUCCESS) {
        if (set_context)
            pam_set_data(pamh, "pam_krb5", NULL, NULL);
        else
            pamk5_context_free(ctx);
    }
    pamk5_args_free(args);
    return pamret;
}


/*
 * Determine the name of a new ticket cache.  Handles ccache and ccache_dir
 * PAM options and returns newly allocated memory.
 *
 * The ccache option, if set, contains a string with possible %u and %p
 * escapes.  The former is replaced by the UID and the latter is replaced by
 * the PID (a suitable unique string).
 */
static char *
build_ccache_name(struct pam_args *args, uid_t uid)
{
    char *cache_name;

    if (args->ccache == NULL) {
        size_t ccache_size = 1 + strlen(args->ccache_dir) +
            strlen("/krb5cc_4294967295_XXXXXX");

        cache_name = malloc(ccache_size);
        if (!cache_name) {
            pamk5_error(args, "malloc failure: %s", strerror(errno));
            return NULL;
        }
        snprintf(cache_name, ccache_size, "%s/krb5cc_%d_XXXXXX",
                 args->ccache_dir, (int) uid);
    } else {
        size_t len = 0, delta;
        char *p, *q;

        for (p = args->ccache; *p != '\0'; p++) {
            if (p[0] == '%' && p[1] == 'u') {
                len += snprintf(NULL, 0, "%ld", (long) uid);
                p++;
            } else if (p[0] == '%' && p[1] == 'p') {
                len += snprintf(NULL, 0, "%ld", (long) getpid());
                p++;
            } else {
                len++;
            }
        }
        len++;
        cache_name = malloc(len);
        if (cache_name == NULL) {
            pamk5_error(args, "malloc failure: %s", strerror(errno));
            return NULL;
        }
        for (p = args->ccache, q = cache_name; *p != '\0'; p++) {
            if (p[0] == '%' && p[1] == 'u') {
                delta = snprintf(q, len, "%ld", (long) uid);
                q += delta;
                len -= delta;
                p++;
            } else if (p[0] == '%' && p[1] == 'p') {
                delta = snprintf(q, len, "%ld", (long) getpid());
                q += delta;
                len -= delta;
                p++;
            } else {
                *q = *p;
                q++;
                len--;
            }
        }
        *q = '\0';
    }
    return cache_name;
}


/*
 * Create a new context for a session if we've lost the context created during
 * authentication (such as when running under OpenSSH).  Return PAM_IGNORE if
 * we're ignoring this user or if apparently our pam_authenticate never
 * succeeded.
 */
static int
create_session_context(struct pam_args *args)
{
    struct context *ctx = NULL;
    PAM_CONST char *user;
    const char *tmpname;
    int status, pamret;

    /* If we're going to ignore the user anyway, don't even bother. */
    if (args->ignore_root || args->minimum_uid > 0) {
        pamret = pam_get_user(args->pamh, &user, NULL);
        if (pamret == PAM_SUCCESS && pamk5_should_ignore(args, user)) {
            pamret = PAM_IGNORE;
            goto fail;
        }
    }

    /*
     * Create the context and locate the temporary ticket cache.  Load the
     * ticket cache back into the context and flush out the other data that
     * would have been set if we'd kept our original context.
     */
    pamret = pamk5_context_new(args);
    if (pamret != PAM_SUCCESS) {
        pamk5_debug(args, "creating session context failed");
        goto fail;
    }
    ctx = args->ctx;
    tmpname = pamk5_get_krb5ccname(args, "PAM_KRB5CCNAME");
    if (tmpname == NULL) {
        pamk5_debug(args, "unable to get PAM_KRB5CCNAME, assuming"
                    " non-Kerberos login");
        pamret = PAM_IGNORE;
        goto fail;
    }
    pamk5_debug(args, "found initial ticket cache at %s", tmpname);
    if (krb5_cc_resolve(ctx->context, tmpname, &ctx->cache) != 0) {
        pamk5_debug(args, "cannot resolve cache %s", tmpname);
        pamret = PAM_SERVICE_ERR;
        goto fail;
    }
    status = krb5_cc_get_principal(ctx->context, ctx->cache, &ctx->princ);
    if (status != 0) {
        pamk5_debug_krb5(args, "cannot retrieve principal", status);
        pamret = PAM_SERVICE_ERR;
        goto fail;
    }

    /*
     * We've rebuilt the context.  Push it back into the PAM state for any
     * further calls to session or account management, which OpenSSH does keep
     * the context for.
     */
    pamret = pam_set_data(args->pamh, "pam_krb5", ctx, pamk5_context_destroy);
    if (pamret != PAM_SUCCESS) {
        pamk5_debug_pam(args, "cannot set context data", pamret);
        goto fail;
    }
    return PAM_SUCCESS;

fail:
    if (args->ctx != NULL)
        pamk5_context_free(args->ctx);
    args->ctx = NULL;
    return pamret;
}


/*
 * Should be called after a successful authentication.  Sets user credentials
 * by creating the permanent ticket cache and setting the proper ownership.
 * This function is also called by pam_sm_open_session; they both do the same
 * thing.
 */
int
pam_sm_setcred(pam_handle_t *pamh, int flags, int argc, const char **argv)
{
    struct context *ctx = NULL;
    struct pam_args *args;
    krb5_ccache cache = NULL;
    char *cache_name = NULL;
    int reinit = 0, status = 0;
    int pamret, allow;
    struct passwd *pw = NULL;
    uid_t uid;
    gid_t gid;

    args = pamk5_args_parse(pamh, flags, argc, argv);
    if (args == NULL) {
        pamk5_error(NULL, "cannot allocate memory: %s", strerror(errno));
        pamret = PAM_SERVICE_ERR;
        goto done;
    }
    pamret = pamk5_context_fetch(args);
    ENTRY(args, flags);

    /*
     * Special case.  Just free the context data, which will destroy the
     * ticket cache as well.
     */
    if (flags & PAM_DELETE_CRED) {
        pamret = pam_set_data(pamh, "pam_krb5", NULL, NULL);
        args->ctx = NULL;
        goto done;
    }

    /* If configured not to create a cache, we have nothing to do. */
    if (args->no_ccache) {
        pamret = PAM_SUCCESS;
        goto done;
    }

    /*
     * Reinitialization requested, which means that rather than creating a new
     * ticket cache and setting KRB5CCNAME, we should figure out the existing
     * ticket cache and just refresh its tickets.
     */
    if (flags & (PAM_REINITIALIZE_CRED | PAM_REFRESH_CRED))
        reinit = 1;
    if (reinit && (flags & PAM_ESTABLISH_CRED)) {
        pamk5_error(args, "requested establish and refresh at the same time");
        pamret = PAM_SERVICE_ERR;
        goto done;
    }
    allow = PAM_REINITIALIZE_CRED | PAM_REFRESH_CRED | PAM_ESTABLISH_CRED;
    if (!(flags & allow)) {
        pamret = PAM_SERVICE_ERR;
        goto done;
    }

    /*
     * If we weren't able to obtain a context, we were probably run by OpenSSH
     * with its broken PAM handling, so we're going to cobble up a new context
     * for ourselves.
     */
    if (args->ctx == NULL) {
        pamk5_debug(args, "no context found, creating one");
        pamret = create_session_context(args);
        if (args->ctx == NULL)
            goto done;
    }
    ctx = args->ctx;

    /*
     * Some programs (xdm, for instance) appear to call setcred over and over
     * again, so avoid doing useless work.
     */
    if (ctx->initialized) {
        pamret = PAM_SUCCESS;
        goto done;
    }

    /*
     * Get the uid.  The user is not required to be a local account for
     * pam_authenticate, but for either pam_setcred (other than DELETE) or for
     * pam_open_session, the user must be a local account.
     */
    pw = pamk5_compat_getpwnam(args, ctx->name);
    if (pw == NULL) {
        pamk5_debug(args, "getpwnam failed for %s", ctx->name);
        pamret = PAM_USER_UNKNOWN;
        goto done;
    }
    uid = pw->pw_uid;
    gid = pw->pw_gid;

    /* Get the cache name.  If reinitializing, this is our existing cache. */
    if (reinit) {
        const char *name, *k5name;

        name = pamk5_get_krb5ccname(args, "KRB5CCNAME");
        if (name == NULL)
            name = krb5_cc_default_name(ctx->context);
        if (name == NULL) {
            pamk5_debug(args, "unable to get ticket cache name");
            pamret = PAM_SERVICE_ERR;
            goto done;
        }
        if (strncmp(name, "FILE:", strlen("FILE:")) == 0)
            name += strlen("FILE:");

        /*
         * If the cache we have in the context and the cache we're
         * reinitializing are the same cache, don't do anything; otherwise,
         * we'll end up destroying the cache.  This should never happen; this
         * case triggering is a sign of a bug, probably in the calling
         * application.
         */
        if (ctx->cache != NULL) {
            k5name = krb5_cc_get_name(ctx->context, ctx->cache);
            if (k5name != NULL) {
                if (strncmp(k5name, "FILE:", strlen("FILE:")) == 0)
                    k5name += strlen("FILE:");
                if (strcmp(name, k5name) == 0) {
                    pamret = PAM_SUCCESS;
                    goto done;
                }
            }
        }

        cache_name = strdup(name);
        if (!cache_name) {
            pamk5_error(args, "malloc failure: %s", strerror(errno));
            pamret = PAM_BUF_ERR;
            goto done;
        }
        pamk5_debug(args, "refreshing ticket cache %s", cache_name);

        /*
         * If we're refreshing the cache, we didn't really create it and the
         * user's open session created by login is probably still managing
         * it.  Thus, don't remove it when PAM is shut down.
         */
        ctx->dont_destroy_cache = 1;
    } else {
        char *cache_name_tmp;
        size_t len;

        cache_name = build_ccache_name(args, uid);
        if (cache_name == NULL) {
            pamret = PAM_BUF_ERR;
            goto done;
        }
        len = strlen(cache_name);
        if (len > 6 && strncmp("XXXXXX", cache_name + len - 6, 6) == 0) {
            if (strncmp(cache_name, "FILE:", strlen("FILE:")) == 0)
                cache_name_tmp = cache_name + strlen("FILE:");
            else
                cache_name_tmp = cache_name;
            pamret = pamk5_cache_mkstemp(args, cache_name_tmp);
            if (pamret != PAM_SUCCESS)
                goto done;
        }
        pamk5_debug(args, "initializing ticket cache %s", cache_name);
    }

    /*
     * Initialize the new ticket cache and point the environment at it.  Only
     * chown the cache if the cache is of type FILE or has no type (making the
     * assumption that the default cache type is FILE; otherwise, due to the
     * type prefix, we'd end up with an invalid path.
     */
    pamret = cache_init_from_cache(args, cache_name, ctx->cache, &cache);
    if (pamret != PAM_SUCCESS)
        goto done;
    if (strncmp(cache_name, "FILE:", strlen("FILE:")) == 0)
        status = chown(cache_name + strlen("FILE:"), uid, gid);
    else if (strchr(cache_name, ':') == NULL)
        status = chown(cache_name, uid, gid);
    if (status == -1) {
        pamk5_debug(args, "chown of ticket cache failed: %s", strerror(errno));
        pamret = PAM_SERVICE_ERR;       
        goto done;
    }
    pamret = pamk5_set_krb5ccname(args, cache_name, "KRB5CCNAME");
    if (pamret != PAM_SUCCESS)
        goto done;

    /*
     * If we had a temporary ticket cache, delete the environment variable so
     * that we won't get confused and think we still have a temporary ticket
     * cache when called again.
     */
    if (pam_getenv(pamh, "PAM_KRB5CCNAME") != NULL) {
        pamret = pam_putenv(pamh, "PAM_KRB5CCNAME");
        if (pamret != PAM_SUCCESS)
            goto done;
    }

    /* Detroy the temporary cache and put the new cache in the context. */
    krb5_cc_destroy(ctx->context, ctx->cache);
    ctx->cache = cache;
    cache = NULL;
    ctx->initialized = 1;
    if (args->retain)
        ctx->dont_destroy_cache = 1;

done:
    if (cache != NULL)
        krb5_cc_destroy(ctx->context, cache);
    if (cache_name != NULL)
        free(cache_name);
    EXIT(args, pamret);
    pamk5_args_free(args);
    return pamret;
}
