/* SPDX-License-Identifier: LGPL-2.1+ */

#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <security/_pam_macros.h>
#include <security/pam_ext.h>
#include <security/pam_misc.h>
#include <security/pam_modules.h>
#include <security/pam_modutil.h>
#include <sys/file.h>

#include "alloc-util.h"
#include "audit-util.h"
#include "bus-common-errors.h"
#include "bus-error.h"
#include "bus-util.h"
#include "def.h"
#include "fd-util.h"
#include "fileio.h"
#include "format-util.h"
#include "hostname-util.h"
#include "login-util.h"
#include "macro.h"
#include "parse-util.h"
#include "process-util.h"
#include "socket-util.h"
#include "strv.h"
#include "terminal-util.h"
#include "util.h"
#include "path-util.h"
#include "cgroup-util.h"

static int parse_argv(
                pam_handle_t *handle,
                int argc, const char **argv,
                const char **class,
                const char **type,
                bool *debug) {

        unsigned i;

        assert(argc >= 0);
        assert(argc == 0 || argv);

        for (i = 0; i < (unsigned) argc; i++) {
                if (startswith(argv[i], "class=")) {
                        if (class)
                                *class = argv[i] + 6;

                } else if (startswith(argv[i], "type=")) {
                        if (type)
                                *type = argv[i] + 5;

                } else if (streq(argv[i], "debug")) {
                        if (debug)
                                *debug = true;

                } else if (startswith(argv[i], "debug=")) {
                        int k;

                        k = parse_boolean(argv[i] + 6);
                        if (k < 0)
                                pam_syslog(handle, LOG_WARNING, "Failed to parse debug= argument, ignoring.");
                        else if (debug)
                                *debug = k;

                } else
                        pam_syslog(handle, LOG_WARNING, "Unknown parameter '%s', ignoring", argv[i]);
        }

        return 0;
}

static int get_user_data(
                pam_handle_t *handle,
                const char **ret_username,
                struct passwd **ret_pw) {

        const char *username = NULL;
        struct passwd *pw = NULL;
        int r;

        assert(handle);
        assert(ret_username);
        assert(ret_pw);

        r = pam_get_user(handle, &username, NULL);
        if (r != PAM_SUCCESS) {
                pam_syslog(handle, LOG_ERR, "Failed to get user name.");
                return r;
        }

        if (isempty(username)) {
                pam_syslog(handle, LOG_ERR, "User name not valid.");
                return PAM_AUTH_ERR;
        }

        pw = pam_modutil_getpwnam(handle, username);
        if (!pw) {
                pam_syslog(handle, LOG_ERR, "Failed to get user data.");
                return PAM_USER_UNKNOWN;
        }

        *ret_pw = pw;
        *ret_username = username;

        return PAM_SUCCESS;
}

static int get_seat_from_display(const char *display, const char **seat, uint32_t *vtnr) {
        union sockaddr_union sa = {
                .un.sun_family = AF_UNIX,
        };
        _cleanup_free_ char *p = NULL, *tty = NULL;
        _cleanup_close_ int fd = -1;
        struct ucred ucred;
        int v, r;

        assert(display);
        assert(vtnr);

        /* We deduce the X11 socket from the display name, then use
         * SO_PEERCRED to determine the X11 server process, ask for
         * the controlling tty of that and if it's a VC then we know
         * the seat and the virtual terminal. Sounds ugly, is only
         * semi-ugly. */

        r = socket_from_display(display, &p);
        if (r < 0)
                return r;
        strncpy(sa.un.sun_path, p, sizeof(sa.un.sun_path)-1);

        fd = socket(AF_UNIX, SOCK_STREAM|SOCK_CLOEXEC, 0);
        if (fd < 0)
                return -errno;

        if (connect(fd, &sa.sa, SOCKADDR_UN_LEN(sa.un)) < 0)
                return -errno;

        r = getpeercred(fd, &ucred);
        if (r < 0)
                return r;

        r = get_ctty(ucred.pid, NULL, &tty);
        if (r < 0)
                return r;

        v = vtnr_from_tty(tty);
        if (v < 0)
                return v;
        else if (v == 0)
                return -ENOENT;

        if (seat)
                *seat = "seat0";
        *vtnr = (uint32_t) v;

        return 0;
}

static int export_legacy_dbus_address(
                pam_handle_t *handle,
                uid_t uid,
                const char *runtime) {

        _cleanup_free_ char *s = NULL;
        int r = PAM_BUF_ERR;

        /* FIXME: We *really* should move the access() check into the
         * daemons that spawn dbus-daemon, instead of forcing
         * DBUS_SESSION_BUS_ADDRESS= here. */

        s = strjoin(runtime, "/bus");
        if (!s)
                goto error;

        if (access(s, F_OK) < 0)
                return PAM_SUCCESS;

        s = mfree(s);
        if (asprintf(&s, DEFAULT_USER_BUS_ADDRESS_FMT, runtime) < 0)
                goto error;

        r = pam_misc_setenv(handle, "DBUS_SESSION_BUS_ADDRESS", s, 0);
        if (r != PAM_SUCCESS)
                goto error;

        return PAM_SUCCESS;

error:
        pam_syslog(handle, LOG_ERR, "Failed to set bus variable.");
        return r;
}

static int append_session_memory_max(pam_handle_t *handle, sd_bus_message *m, const char *limit) {
        uint64_t val;
        int r;

        if (isempty(limit))
                return 0;

        if (streq(limit, "infinity")) {
                r = sd_bus_message_append(m, "(sv)", "MemoryMax", "t", (uint64_t)-1);
                if (r < 0) {
                        pam_syslog(handle, LOG_ERR, "Failed to append to bus message: %s", strerror(-r));
                        return r;
                }
        } else {
                r = parse_percent(limit);
                if (r >= 0) {
                        r = sd_bus_message_append(m, "(sv)", "MemoryMaxScale", "u", (uint32_t) (((uint64_t) UINT32_MAX * r) / 100U));
                        if (r < 0) {
                                pam_syslog(handle, LOG_ERR, "Failed to append to bus message: %s", strerror(-r));
                                return r;
                        }
                } else {
                        r = parse_size(limit, 1024, &val);
                        if (r >= 0) {
                                r = sd_bus_message_append(m, "(sv)", "MemoryMax", "t", val);
                                if (r < 0) {
                                        pam_syslog(handle, LOG_ERR, "Failed to append to bus message: %s", strerror(-r));
                                        return r;
                                }
                        } else
                                pam_syslog(handle, LOG_WARNING, "Failed to parse systemd.limit: %s, ignoring.", limit);
                }
        }

        return 0;
}

static int append_session_tasks_max(pam_handle_t *handle, sd_bus_message *m, const char *limit)
{
        uint64_t val;
        int r;

        /* No need to parse "infinity" here, it will be set unconditionally later in manager_start_scope() */
        if (isempty(limit) || streq(limit, "infinity"))
                return 0;

        r = safe_atou64(limit, &val);
        if (r >= 0) {
                r = sd_bus_message_append(m, "(sv)", "TasksMax", "t", val);
                if (r < 0) {
                        pam_syslog(handle, LOG_ERR, "Failed to append to bus message: %s", strerror(-r));
                        return r;
                }
        } else
                pam_syslog(handle, LOG_WARNING, "Failed to parse systemd.limit: %s, ignoring.", limit);

        return 0;
}

static int append_session_cg_weight(pam_handle_t *handle, sd_bus_message *m, const char *limit, const char *field) {
        uint64_t val;
        int r;

        if (!isempty(limit)) {
                r = cg_weight_parse(limit, &val);
                if (r >= 0) {
                        r = sd_bus_message_append(m, "(sv)", field, "t", val);
                        if (r < 0) {
                                pam_syslog(handle, LOG_ERR, "Failed to append to bus message: %s", strerror(-r));
                                return r;
                        }
                } else if (streq(field, "CPUWeight"))
                        pam_syslog(handle, LOG_WARNING, "Failed to parse systemd.cpu_weight: %s, ignoring.", limit);
                else
                        pam_syslog(handle, LOG_WARNING, "Failed to parse systemd.io_weight: %s, ignoring.", limit);
        }

        return 0;
}

static const char* getenv_harder(pam_handle_t *handle, const char *key, const char *fallback) {
        const char *v;

        assert(handle);
        assert(key);

        /* Looks for an environment variable, preferrably in the environment block associated with the
         * specified PAM handle, falling back to the process' block instead. Why check both? Because we want
         * to permit configuration of session properties from unit files that invoke PAM services, so that
         * PAM services don't have to be reworked to set systemd-specific properties, but these properties
         * can still be set from the unit file Environment= block. */

        v = pam_getenv(handle, key);
        if (!isempty(v))
                return v;

        /* We use secure_getenv() here, since we might get loaded into su/sudo, which are SUID. Ideally
         * they'd clean up the environment before invoking foreign code (such as PAM modules), but alas they
         * currently don't (to be precise, they clean up the environment they pass to their children, but
         * not their own environ[]). */
        v = secure_getenv(key);
        if (!isempty(v))
                return v;

        return fallback;
}

static int update_environment(pam_handle_t *handle, const char *key, const char *value) {
        int r;

        assert(handle);
        assert(key);

        /* Updates the environment, but only if there's actually a value set. Also, log about errors */

        if (isempty(value))
                return PAM_SUCCESS;

        r = pam_misc_setenv(handle, key, value, 0);
        if (r != PAM_SUCCESS)
                pam_syslog(handle, LOG_ERR, "Failed to set environment variable %s.", key);

        return r;
}

static bool validate_runtime_directory(pam_handle_t *handle, const char *path, uid_t uid) {
        struct stat st;

        assert(path);

        /* Just some extra paranoia: let's not set $XDG_RUNTIME_DIR if the directory we'd set it to isn't actually set
         * up properly for us. */

        if (lstat(path, &st) < 0) {
                pam_syslog(handle, LOG_ERR, "Failed to stat() runtime directory '%s': %s", path, strerror(errno));
                goto fail;
        }

        if (!S_ISDIR(st.st_mode)) {
                pam_syslog(handle, LOG_ERR, "Runtime directory '%s' is not actually a directory.", path);
                goto fail;
        }

        if (st.st_uid != uid) {
                pam_syslog(handle, LOG_ERR, "Runtime directory '%s' is not owned by UID " UID_FMT ", as it should.", path, uid);
                goto fail;
        }

        return true;

fail:
        pam_syslog(handle, LOG_WARNING, "Not setting $XDG_RUNTIME_DIR, as the directory is not in order.");
        return false;
}

_public_ PAM_EXTERN int pam_sm_open_session(
                pam_handle_t *handle,
                int flags,
                int argc, const char **argv) {

        _cleanup_(sd_bus_error_free) sd_bus_error error = SD_BUS_ERROR_NULL;
        _cleanup_(sd_bus_message_unrefp) sd_bus_message *m = NULL, *reply = NULL;
        const char
                *username, *id, *object_path, *runtime_path,
                *service = NULL,
                *tty = NULL, *display = NULL,
                *remote_user = NULL, *remote_host = NULL,
                *seat = NULL,
                *type = NULL, *class = NULL,
                *class_pam = NULL, *type_pam = NULL, *cvtnr = NULL, *desktop = NULL,
                *memory_max = NULL, *tasks_max = NULL, *cpu_weight = NULL, *io_weight = NULL;
        _cleanup_(sd_bus_flush_close_unrefp) sd_bus *bus = NULL;
        int session_fd = -1, existing, r;
        bool debug = false, remote;
        struct passwd *pw;
        uint32_t vtnr = 0;
        uid_t original_uid;

        assert(handle);

        /* Make this a NOP on non-logind systems */
        if (!logind_running())
                return PAM_SUCCESS;

        if (parse_argv(handle,
                       argc, argv,
                       &class_pam,
                       &type_pam,
                       &debug) < 0)
                return PAM_SESSION_ERR;

        if (debug)
                pam_syslog(handle, LOG_DEBUG, "pam-systemd initializing");

        r = get_user_data(handle, &username, &pw);
        if (r != PAM_SUCCESS) {
                pam_syslog(handle, LOG_ERR, "Failed to get user data.");
                return r;
        }

        /* Make sure we don't enter a loop by talking to
         * systemd-logind when it is actually waiting for the
         * background to finish start-up. If the service is
         * "systemd-user" we simply set XDG_RUNTIME_DIR and
         * leave. */

        pam_get_item(handle, PAM_SERVICE, (const void**) &service);
        if (streq_ptr(service, "systemd-user")) {
                _cleanup_free_ char *rt = NULL;

                if (asprintf(&rt, "/run/user/"UID_FMT, pw->pw_uid) < 0)
                        return PAM_BUF_ERR;

                if (validate_runtime_directory(handle, rt, pw->pw_uid)) {
                        r = pam_misc_setenv(handle, "XDG_RUNTIME_DIR", rt, 0);
                        if (r != PAM_SUCCESS) {
                                pam_syslog(handle, LOG_ERR, "Failed to set runtime dir.");
                                return r;
                        }
                }

                r = export_legacy_dbus_address(handle, pw->pw_uid, rt);
                if (r != PAM_SUCCESS)
                        return r;

                return PAM_SUCCESS;
        }

        /* Otherwise, we ask logind to create a session for us */

        pam_get_item(handle, PAM_XDISPLAY, (const void**) &display);
        pam_get_item(handle, PAM_TTY, (const void**) &tty);
        pam_get_item(handle, PAM_RUSER, (const void**) &remote_user);
        pam_get_item(handle, PAM_RHOST, (const void**) &remote_host);

        seat = getenv_harder(handle, "XDG_SEAT", NULL);
        cvtnr = getenv_harder(handle, "XDG_VTNR", NULL);
        type = getenv_harder(handle, "XDG_SESSION_TYPE", type_pam);
        class = getenv_harder(handle, "XDG_SESSION_CLASS", class_pam);
        desktop = getenv_harder(handle, "XDG_SESSION_DESKTOP", NULL);

        tty = strempty(tty);

        if (strchr(tty, ':')) {
                /* A tty with a colon is usually an X11 display,
                 * placed there to show up in utmp. We rearrange
                 * things and don't pretend that an X display was a
                 * tty. */

                if (isempty(display))
                        display = tty;
                tty = NULL;
        } else if (streq(tty, "cron")) {
                /* cron has been setting PAM_TTY to "cron" for a very
                 * long time and it probably shouldn't stop doing that
                 * for compatibility reasons. */
                type = "unspecified";
                class = "background";
                tty = NULL;
        } else if (streq(tty, "ssh")) {
                /* ssh has been setting PAM_TTY to "ssh" for a very
                 * long time and probably shouldn't stop doing that
                 * for compatibility reasons. */
                type ="tty";
                class = "user";
                tty = NULL;
        } else
                /* Chop off leading /dev prefix that some clients specify, but others do not. */
                tty = skip_dev_prefix(tty);

        /* If this fails vtnr will be 0, that's intended */
        if (!isempty(cvtnr))
                (void) safe_atou32(cvtnr, &vtnr);

        if (!isempty(display) && !vtnr) {
                if (isempty(seat))
                        get_seat_from_display(display, &seat, &vtnr);
                else if (streq(seat, "seat0"))
                        get_seat_from_display(display, NULL, &vtnr);
        }

        if (seat && !streq(seat, "seat0") && vtnr != 0) {
                pam_syslog(handle, LOG_DEBUG, "Ignoring vtnr %"PRIu32" for %s which is not seat0", vtnr, seat);
                vtnr = 0;
        }

        if (isempty(type))
                type = !isempty(display) ? "x11" :
                           !isempty(tty) ? "tty" : "unspecified";

        if (isempty(class))
                class = streq(type, "unspecified") ? "background" : "user";

        remote = !isempty(remote_host) && !is_localhost(remote_host);

        (void) pam_get_data(handle, "systemd.memory_max", (const void **)&memory_max);
        (void) pam_get_data(handle, "systemd.tasks_max",  (const void **)&tasks_max);
        (void) pam_get_data(handle, "systemd.cpu_weight", (const void **)&cpu_weight);
        (void) pam_get_data(handle, "systemd.io_weight",  (const void **)&io_weight);

        /* Talk to logind over the message bus */

        r = sd_bus_open_system(&bus);
        if (r < 0) {
                pam_syslog(handle, LOG_ERR, "Failed to connect to system bus: %s", strerror(-r));
                return PAM_SESSION_ERR;
        }

        if (debug) {
                pam_syslog(handle, LOG_DEBUG, "Asking logind to create session: "
                           "uid="UID_FMT" pid="PID_FMT" service=%s type=%s class=%s desktop=%s seat=%s vtnr=%"PRIu32" tty=%s display=%s remote=%s remote_user=%s remote_host=%s",
                           pw->pw_uid, getpid_cached(),
                           strempty(service),
                           type, class, strempty(desktop),
                           strempty(seat), vtnr, strempty(tty), strempty(display),
                           yes_no(remote), strempty(remote_user), strempty(remote_host));
                pam_syslog(handle, LOG_DEBUG, "Session limits: "
                           "memory_max=%s tasks_max=%s cpu_weight=%s io_weight=%s",
                           strna(memory_max), strna(tasks_max), strna(cpu_weight), strna(io_weight));
        }

        r = sd_bus_message_new_method_call(
                        bus,
                        &m,
                        "org.freedesktop.login1",
                        "/org/freedesktop/login1",
                        "org.freedesktop.login1.Manager",
                        "CreateSession");
        if (r < 0) {
                pam_syslog(handle, LOG_ERR, "Failed to create CreateSession method call: %s", strerror(-r));
                return PAM_SESSION_ERR;
        }

        r = sd_bus_message_append(m, "uusssssussbss",
                        (uint32_t) pw->pw_uid,
                        (uint32_t) getpid_cached(),
                        service,
                        type,
                        class,
                        desktop,
                        seat,
                        vtnr,
                        tty,
                        display,
                        remote,
                        remote_user,
                        remote_host);
        if (r < 0) {
                pam_syslog(handle, LOG_ERR, "Failed to append to bus message: %s", strerror(-r));
                return PAM_SESSION_ERR;
        }

        r = sd_bus_message_open_container(m, 'a', "(sv)");
        if (r < 0) {
                pam_syslog(handle, LOG_ERR, "Failed to open message container: %s", strerror(-r));
                return PAM_SYSTEM_ERR;
        }

        r = append_session_memory_max(handle, m, memory_max);
        if (r < 0)
                return PAM_SESSION_ERR;

        r = append_session_tasks_max(handle, m, tasks_max);
        if (r < 0)
                return PAM_SESSION_ERR;

        r = append_session_cg_weight(handle, m, cpu_weight, "CPUWeight");
        if (r < 0)
                return PAM_SESSION_ERR;

        r = append_session_cg_weight(handle, m, io_weight, "IOWeight");
        if (r < 0)
                return PAM_SESSION_ERR;

        r = sd_bus_message_close_container(m);
        if (r < 0) {
                pam_syslog(handle, LOG_ERR, "Failed to close message container: %s", strerror(-r));
                return PAM_SYSTEM_ERR;
        }

        r = sd_bus_call(bus, m, 0, &error, &reply);
        if (r < 0) {
                if (sd_bus_error_has_name(&error, BUS_ERROR_SESSION_BUSY)) {
                        pam_syslog(handle, LOG_DEBUG, "Cannot create session: %s", bus_error_message(&error, r));
                        return PAM_SUCCESS;
                } else {
                        pam_syslog(handle, LOG_ERR, "Failed to create session: %s", bus_error_message(&error, r));
                        return PAM_SYSTEM_ERR;
                }
        }

        r = sd_bus_message_read(reply,
                                "soshusub",
                                &id,
                                &object_path,
                                &runtime_path,
                                &session_fd,
                                &original_uid,
                                &seat,
                                &vtnr,
                                &existing);
        if (r < 0) {
                pam_syslog(handle, LOG_ERR, "Failed to parse message: %s", strerror(-r));
                return PAM_SESSION_ERR;
        }

        if (debug)
                pam_syslog(handle, LOG_DEBUG, "Reply from logind: "
                           "id=%s object_path=%s runtime_path=%s session_fd=%d seat=%s vtnr=%u original_uid=%u",
                           id, object_path, runtime_path, session_fd, seat, vtnr, original_uid);

        r = update_environment(handle, "XDG_SESSION_ID", id);
        if (r != PAM_SUCCESS)
                return r;

        if (original_uid == pw->pw_uid) {
                /* Don't set $XDG_RUNTIME_DIR if the user we now
                 * authenticated for does not match the original user
                 * of the session. We do this in order not to result
                 * in privileged apps clobbering the runtime directory
                 * unnecessarily. */

                if (validate_runtime_directory(handle, runtime_path, pw->pw_uid)) {
                        r = update_environment(handle, "XDG_RUNTIME_DIR", runtime_path);
                        if (r != PAM_SUCCESS)
                                return r;
                }

                r = export_legacy_dbus_address(handle, pw->pw_uid, runtime_path);
                if (r != PAM_SUCCESS)
                        return r;
        }

        r = update_environment(handle, "XDG_SEAT", seat);
        if (r != PAM_SUCCESS)
                return r;

        if (vtnr > 0) {
                char buf[DECIMAL_STR_MAX(vtnr)];
                sprintf(buf, "%u", vtnr);

                r = update_environment(handle, "XDG_VTNR", buf);
                if (r != PAM_SUCCESS)
                        return r;
        }

        r = pam_set_data(handle, "systemd.existing", INT_TO_PTR(!!existing), NULL);
        if (r != PAM_SUCCESS) {
                pam_syslog(handle, LOG_ERR, "Failed to install existing flag.");
                return r;
        }

        if (session_fd >= 0) {
                session_fd = fcntl(session_fd, F_DUPFD_CLOEXEC, 3);
                if (session_fd < 0) {
                        pam_syslog(handle, LOG_ERR, "Failed to dup session fd: %m");
                        return PAM_SESSION_ERR;
                }

                r = pam_set_data(handle, "systemd.session-fd", FD_TO_PTR(session_fd), NULL);
                if (r != PAM_SUCCESS) {
                        pam_syslog(handle, LOG_ERR, "Failed to install session fd.");
                        safe_close(session_fd);
                        return r;
                }
        }

        return PAM_SUCCESS;
}

_public_ PAM_EXTERN int pam_sm_close_session(
                pam_handle_t *handle,
                int flags,
                int argc, const char **argv) {

        _cleanup_(sd_bus_error_free) sd_bus_error error = SD_BUS_ERROR_NULL;
        _cleanup_(sd_bus_flush_close_unrefp) sd_bus *bus = NULL;
        const void *existing = NULL;
        const char *id;
        int r;

        assert(handle);

        /* Only release session if it wasn't pre-existing when we
         * tried to create it */
        pam_get_data(handle, "systemd.existing", &existing);

        id = pam_getenv(handle, "XDG_SESSION_ID");
        if (id && !existing) {

                /* Before we go and close the FIFO we need to tell
                 * logind that this is a clean session shutdown, so
                 * that it doesn't just go and slaughter us
                 * immediately after closing the fd */

                r = sd_bus_open_system(&bus);
                if (r < 0) {
                        pam_syslog(handle, LOG_ERR, "Failed to connect to system bus: %s", strerror(-r));
                        return PAM_SESSION_ERR;
                }

                r = sd_bus_call_method(bus,
                                       "org.freedesktop.login1",
                                       "/org/freedesktop/login1",
                                       "org.freedesktop.login1.Manager",
                                       "ReleaseSession",
                                       &error,
                                       NULL,
                                       "s",
                                       id);
                if (r < 0) {
                        pam_syslog(handle, LOG_ERR, "Failed to release session: %s", bus_error_message(&error, r));
                        return PAM_SESSION_ERR;
                }
        }

        /* Note that we are knowingly leaking the FIFO fd here. This
         * way, logind can watch us die. If we closed it here it would
         * not have any clue when that is completed. Given that one
         * cannot really have multiple PAM sessions open from the same
         * process this means we will leak one FD at max. */

        return PAM_SUCCESS;
}
