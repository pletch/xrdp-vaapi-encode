/**
 * xrdp: A Remote Desktop Protocol server.
 *
 * Copyright (C) Jay Sorg and contributors 2004-2026
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/**
 *
 * @file session_wayland.c
 * @brief Wayland session objects
 *
 */

#if defined(HAVE_CONFIG_H)
#include "config_ac.h"
#endif

#include <ctype.h>
#include <signal.h>
#include <string.h>
#include <sys/wait.h>

#include "arch.h"
#include "session_wayland.h"
#include "session_parameters.h"

#include "sesman_auth.h"
#include "sesman_config.h"
#include "env.h"
#include "list.h"
#include "log.h"
#include "login_info.h"
#include "os_calls.h"
#include "sesexec.h"
#include "sessionrecord.h"
#include "string_calls.h"
#include "trans.h"
#include "xrdp_sockets.h"

/******************************************************************************/
/* Wayland sessions (SCP_SESSION_TYPE_WAYLAND, _WAYLAND_REMOTEAPP)
 *
 * A Wayland session's three processes are:
 *   the compositor (startwayland.sh), which runs the desktop and exits
 *             with it: the session lasts as long as it does
 *   the backend, wlxrdp, which xrdp connects to (xup, like xorgxrdp)
 *   chansrv, with WAYLAND_DISPLAY set
 * The compositor picks its own socket name; the session script reports the
 * socket's absolute path through a file. The name becomes the session's
 * display string; the path is WAYLAND_DISPLAY for the backend and chansrv,
 * which then need no XDG_RUNTIME_DIR (sesexec does not set one). */

#define WAYLAND_NAME_WAIT_MS 30000
#define WAYLAND_BACKEND_WAIT_MS 10000
/* how long a session process has to end on SIGTERM before SIGKILL */
#define WAYLAND_STOP_WAIT_MS 5000

/******************************************************************************/
static void
start_wayland_compositor(struct session_data *sd,
                         const struct login_info *login_info,
                         void *closure)
{
    const char *name_file = (const char *)closure;
    const struct session_parameters *sp = sd->params;
    const char *script = XRDP_CFG_PATH "/startwayland.sh";
    char size[32];

    env_set_user(login_info->uid,
                 g_cfg->env_names,
                 g_cfg->env_values);
    auth_set_env(login_info->auth_info);
    g_setenv_log("XRDP_WAYLAND_NAME_FILE", name_file, 1);
    if (sp->type == SCP_SESSION_TYPE_WAYLAND_REMOTEAPP)
    {
        /* sway, no desktop: chansrv manages the windows */
        g_setenv_log("XRDP_WAYLAND_REMOTEAPP", "1", 1);
    }
    if (sp->width > 0 && sp->height > 0)
    {
        g_snprintf(size, sizeof(size), "%dx%d", sp->width, sp->height);
        g_setenv_log("XRDP_WAYLAND_SIZE", size, 1);
    }
    if (sp->shell[0] != '\0')
    {
        LOG(LOG_LEVEL_WARNING, "Wayland sessions ignore the requested "
            "shell '%s'", sp->shell);
    }
    LOG_DEVEL_LEAKING_FDS("wayland compositor", 3, -1);

    LOG(LOG_LEVEL_INFO, "Starting the Wayland compositor: %s", script);
    g_execlp3(script, script, 0);

    LOG(LOG_LEVEL_ERROR, "Can't start %s [%s]", script, g_get_strerror());
}

/******************************************************************************/
static void
start_wayland_backend(struct session_data *sd,
                      const struct login_info *login_info,
                      void *closure /* unused */)
{
    const char *exe_path = XRDP_LIBEXEC_PATH "/wlxrdp";
    char socket_path[XRDP_SOCKETS_MAXPATH];

    env_set_user(login_info->uid,
                 g_cfg->env_names,
                 g_cfg->env_values);
    g_snprintf(socket_path, sizeof(socket_path), XRDP_X11RDP_STR,
               login_info->uid, sd->display);
    LOG_DEVEL_LEAKING_FDS("wayland backend", 3, -1);

    LOG(LOG_LEVEL_INFO, "Starting the Wayland backend: %s -s %s",
        exe_path, socket_path);
    const char *argv[] = { exe_path, "-s", socket_path, NULL };
    g_execvp(exe_path, (char **)argv);

    LOG(LOG_LEVEL_ERROR, "Can't start %s [%s]", exe_path, g_get_strerror());
}

/******************************************************************************/
/* Wait for a file to appear, while the process that makes it lives.
 * Returns 0 once it exists. */
static int
wait_for_file(const char *path, pid_t maker, unsigned int timeout_ms)
{
    unsigned int start = g_get_elapsed_ms();

    while (!g_file_exist(path))
    {
        if (g_get_elapsed_ms() - start >= timeout_ms)
        {
            return 1;
        }
        if (!g_pid_is_active(maker))
        {
            return 1;
        }
        g_sleep(100);
    }
    return 0;
}

/******************************************************************************/
/* A line of the name file: an absolute path of plain characters. Returns
 * a pointer past it (and its newline), or NULL if it is not one. */
static char *
wayland_path_line(char *p)
{
    char *start = p;

    for (; *p != '\0' && *p != '\n'; p++)
    {
        if (!(isalnum((unsigned char) p[0]) || *p == '/' ||
                *p == '-' || *p == '_' || *p == '.'))
        {
            return NULL;
        }
    }
    if (*p == '\n')
    {
        *p++ = '\0';
    }
    if (start[0] != '/' || strstr(start, "/..") != NULL)
    {
        return NULL;
    }
    return p;
}

/******************************************************************************/
/* The compositor's socket, as the session script wrote it: an absolute
 * path into path[], its last component (the display string) into name[].
 * An optional second line is the compositor's IPC socket (sway's, for
 * RemoteApp), into ipc[]; "" if there is none. */
static int
read_wayland_socket(const char *name_file, char path[], unsigned int path_len,
                    char name[], unsigned int len,
                    char ipc[], unsigned int ipc_len)
{
    char buff[XRDP_SOCKETS_MAXPATH * 2];
    const char *base;
    char *second;
    int fd;
    int n;

    ipc[0] = '\0';
    fd = g_file_open_ro(name_file);
    if (fd < 0)
    {
        return 1;
    }
    n = g_file_read(fd, buff, sizeof(buff) - 1);
    g_file_close(fd);
    g_file_delete(name_file);
    if (n <= 0)
    {
        return 1;
    }
    buff[n] = '\0';
    second = wayland_path_line(buff);
    if (second == NULL)
    {
        return 1;
    }
    if (*second != '\0' &&
            (wayland_path_line(second) == NULL ||
             strlcpy(ipc, second, ipc_len) >= ipc_len))
    {
        return 1;
    }
    base = strrchr(buff, '/') + 1;
    if (base[0] == '\0' ||
            strlcpy(name, base, len) >= len ||
            strlcpy(path, buff, path_len) >= path_len)
    {
        return 1;
    }
    return 0;
}

/******************************************************************************/
/* Is the compositor ending? At a logout it closes its clients as it exits,
 * so the backend's exit can reach us a few ms before its own: wait up to
 * WAYLAND_RESTART_GRACE_MS for it to exit, without reaping it (the SIGCHLD
 * loop does that). Only a backend lost while the compositor lives on is
 * worth restarting. */
#define WAYLAND_RESTART_GRACE_MS 1000

static int
wayland_compositor_ending(pid_t pid)
{
    unsigned int start = g_get_elapsed_ms();
    siginfo_t si;

    for (;;)
    {
        g_memset(&si, 0, sizeof(si));
        if (waitid(P_PID, (id_t)pid, &si,
                   WEXITED | WNOHANG | WNOWAIT) == 0 && si.si_pid == pid)
        {
            return 1;
        }
        if (g_get_elapsed_ms() - start >= WAYLAND_RESTART_GRACE_MS)
        {
            return 0;
        }
        g_sleep(20);
    }
}

/******************************************************************************/
/* The backend exited while the compositor lives: start another on the
 * same socket, at most WAYLAND_BACKEND_RESTARTS times a minute. */
#define WAYLAND_BACKEND_RESTARTS 5

static void
wayland_restart_backend(struct session_data_wayland *self)
{
    time_t now = time(NULL);

    if (now - self->backend_restart_window >= 60)
    {
        self->backend_restart_window = now;
        self->backend_restarts = 0;
    }
    if (g_login_info == NULL)
    {
        return; /* no user to start it as: sesexec is ending */
    }
    if (++self->backend_restarts > WAYLAND_BACKEND_RESTARTS)
    {
        /* Without a backend nothing can connect to the session, and
           nothing would start one: end it, as an X11 session ends with its
           X server. */
        LOG(LOG_LEVEL_ERROR, "The Wayland backend on display %s keeps "
            "exiting; ending the session", self->base.display);
        g_sigterm(self->compositor_pid);
        return;
    }
    self->backend_pid = session_base_fork_child(&self->base, g_login_info,
                        self->compositor_pid,
                        start_wayland_backend, NULL);
    if (self->backend_pid > 0)
    {
        LOG(LOG_LEVEL_WARNING, "Restarted the Wayland backend on display %s "
            "(pid %d)", self->base.display, self->backend_pid);
    }
    else
    {
        LOG(LOG_LEVEL_ERROR, "The Wayland backend on display %s could not be "
            "restarted; ending the session", self->base.display);
        g_sigterm(self->compositor_pid);
    }
}

/******************************************************************************/
/* A session process started before the session is up: SIGTERM it and reap
 * it, with SIGKILL if it has not ended within WAYLAND_STOP_WAIT_MS (an
 * unbounded wait would hang the login with it). pid <= 0: nothing. */
static void
wayland_stop_child(pid_t pid)
{
    unsigned int start = g_get_elapsed_ms();

    if (pid <= 0)
    {
        return;
    }
    g_sigterm(pid);
    while (waitpid(pid, NULL, WNOHANG) == 0)
    {
        if (g_get_elapsed_ms() - start >= WAYLAND_STOP_WAIT_MS)
        {
            LOG(LOG_LEVEL_WARNING, "pid %d did not end on SIGTERM; killing it",
                pid);
            kill(pid, SIGKILL);
            g_waitpid(pid);
            return;
        }
        g_sleep(100);
    }
}

/******************************************************************************/
static enum scp_screate_status
start(struct session_data *baseobj,
      struct login_info *login_info,
      const struct session_parameters *s)
{
    char name_file[XRDP_SOCKETS_MAXPATH];
    char socket_path[XRDP_SOCKETS_MAXPATH];
    char wayland_socket[XRDP_SOCKETS_MAXPATH];
    char ipc_socket[XRDP_SOCKETS_MAXPATH];
    pid_t compositor_pid;
    pid_t backend_pid = -1;
    pid_t chansrv_pid;
    enum scp_screate_status status = E_SCP_SCREATE_X_SERVER_FAIL;

    // Downcast the base object pointer to a pointer to the Wayland object
    struct session_data_wayland *self = (struct session_data_wayland *)baseobj;

    g_snprintf(name_file, sizeof(name_file),
               XRDP_SOCKET_PATH "/xrdp_wayland_name_%d",
               login_info->uid, g_getpid());
    g_file_delete(name_file);

    /* The compositor leads the session's process group, as the X server
     * does for X11 sessions */
    compositor_pid = session_base_fork_child(baseobj, login_info, 0,
        start_wayland_compositor, name_file);
    if (compositor_pid <= 0)
    {
        return E_SCP_SCREATE_X_SERVER_FAIL;
    }
    if (wait_for_file(name_file, compositor_pid, WAYLAND_NAME_WAIT_MS) != 0 ||
            read_wayland_socket(name_file,
                                wayland_socket, sizeof(wayland_socket),
                                baseobj->display, sizeof(baseobj->display),
                                ipc_socket, sizeof(ipc_socket)) != 0)
    {
        LOG(LOG_LEVEL_ERROR, "The Wayland compositor did not report its "
            "display (see the session's startwayland.sh output)");
        goto fail;
    }
    LOG(LOG_LEVEL_INFO, "Wayland compositor (pid %d) is running on %s (%s)",
        compositor_pid, baseobj->display, wayland_socket);

    /* The rest of the session's processes connect to it; chansrv also to
       sway's IPC in a RemoteApp session */
    if (!list_add_strdup(g_cfg->env_names, "WAYLAND_DISPLAY") ||
            !list_add_strdup(g_cfg->env_values, wayland_socket) ||
            (ipc_socket[0] != '\0' &&
             (!list_add_strdup(g_cfg->env_names, "SWAYSOCK") ||
              !list_add_strdup(g_cfg->env_values, ipc_socket))))
    {
        status = E_SCP_SCREATE_NO_MEMORY;
        goto fail;
    }
    if (s->type == SCP_SESSION_TYPE_WAYLAND_REMOTEAPP &&
            ipc_socket[0] == '\0')
    {
        LOG(LOG_LEVEL_WARNING, "The RemoteApp compositor did not report an "
            "IPC socket: RemoteApp windows will not be managed");
    }

    /* The backend's socket appearing is the sign it is ready, so one left
       by an earlier session on this display name (one that ended without
       its cleanup) must not be mistaken for it */
    g_snprintf(socket_path, sizeof(socket_path), XRDP_X11RDP_STR,
               login_info->uid, baseobj->display);
    g_file_delete(socket_path);
    backend_pid = session_base_fork_child(baseobj, login_info, compositor_pid,
                                          start_wayland_backend, NULL);
    if (backend_pid <= 0 ||
            wait_for_file(socket_path, backend_pid,
                          WAYLAND_BACKEND_WAIT_MS) != 0)
    {
        LOG(LOG_LEVEL_ERROR, "The Wayland backend did not start");
        goto fail;
    }

    utmp_login(compositor_pid, baseobj->display, login_info);
    LOG(LOG_LEVEL_INFO, "Starting the xrdp channel server for display %s",
        baseobj->display);
    chansrv_pid = session_base_fork_child(baseobj, login_info, compositor_pid,
                                          session_base_start_chansrv, NULL);

    self->compositor_pid = compositor_pid;
    self->backend_pid = backend_pid;
    // Set the base class member variables we are responsible for
    baseobj->chansrv_pid = chansrv_pid;
    baseobj->start_time = time(NULL);

    if (session_base_process_startup_wait_time(baseobj) != 0)
    {
        LOG(LOG_LEVEL_ERROR, "Session failed during startup wait time");
        return E_SCP_SCREATE_SESSION_FAIL;
    }
    LOG(LOG_LEVEL_INFO, "Wayland session in progress on %s. Waiting until "
        "the compositor (pid %d) exits to end the session",
        baseobj->display, compositor_pid);
    return E_SCP_SCREATE_OK;

fail:
    /* Before the session is up, undo what was started, newest first. From
       here on the session's own teardown (session_send_term) does it. */
    g_file_delete(name_file);
    wayland_stop_child(backend_pid);
    wayland_stop_child(compositor_pid);
    return status;
}

/******************************************************************************/
static void
cleanup(struct session_data *baseobj)
{
    char file[XRDP_SOCKETS_MAXPATH];

    // Cleanup sockets used by the base display class
    session_base_cleanup_sockets(baseobj);

    /* wlxrdp's socket, which it should have deleted itself */
    g_snprintf(file, sizeof(file), XRDP_X11RDP_STR,
               g_login_info->uid, baseobj->display);
    if (g_file_exist(file))
    {
        LOG(LOG_LEVEL_DEBUG, "cleanup_sockets: deleting %s", file);
        if (g_file_delete(file) == 0)
        {
            LOG(LOG_LEVEL_WARNING,
                "cleanup_sockets: failed to delete %s (%s)",
                file, g_get_strerror());
        }
    }
}

/******************************************************************************/
static unsigned int
active_processes(const struct session_data *baseobj)
{
    // Downcast the base object pointer to a pointer to the Wayland object
    const struct session_data_wayland *self =
        (const struct session_data_wayland *)baseobj;

    return (self->compositor_pid > 0) +
           (self->backend_pid > 0) + (baseobj->chansrv_pid > 0);
}

/******************************************************************************/
static void
process_child_exit(struct session_data *baseobj,
                   int pid,
                   const struct proc_exit_status *e)
{
    // Downcast the base object pointer to a pointer to the Wayland object
    struct session_data_wayland *self = (struct session_data_wayland *)baseobj;

    if (pid == self->backend_pid)
    {
        LOG(LOG_LEVEL_INFO, "Wayland backend pid %d on display %s finished",
            self->backend_pid, baseobj->display);
        self->backend_pid = -1;
        if (self->compositor_pid > 0 && !self->terminating &&
                !wayland_compositor_ending(self->compositor_pid))
        {
            // The compositor (and the desktop) outlived the backend:
            // without one, the session can't be reached. Start another.
            wayland_restart_backend(self);
        }
    }
    else if (pid == self->compositor_pid)
    {
        int wait_time = time(NULL) - baseobj->start_time;

        if (e->reason == E_PXR_STATUS_CODE && e->val == 0)
        {
            LOG(LOG_LEVEL_INFO,
                "Wayland compositor (pid %d, display %s) "
                "finished normally in %d secs",
                self->compositor_pid, baseobj->display, wait_time);
        }
        else
        {
            char reason[128];
            session_base_exit_status_to_str(e, reason, sizeof(reason));

            LOG(LOG_LEVEL_WARNING, "Wayland compositor (pid %d, display %s) "
                "exited with %s",
                self->compositor_pid, baseobj->display, reason);
        }
        if (wait_time < 10)
        {
            /* This could be a config issue. Log a significant error */
            LOG(LOG_LEVEL_WARNING, "Wayland compositor (pid %d, display %s) "
                "exited quickly (%d secs). This could indicate a session "
                "config problem (see the session's startwayland.sh output)",
                self->compositor_pid, baseobj->display, wait_time);
        }

        utmp_logout(self->compositor_pid, baseobj->display, e);
        self->compositor_pid = -1;

        if (self->backend_pid > 0)
        {
            LOG(LOG_LEVEL_INFO, "Terminating the Wayland backend (pid %d) on "
                "display %s", self->backend_pid, baseobj->display);
            g_sigterm(self->backend_pid);
        }

        if (baseobj->chansrv_pid > 0)
        {
            LOG(LOG_LEVEL_INFO, "Terminating the xrdp channel server (pid %d) "
                "on display %s", baseobj->chansrv_pid, baseobj->display);
            g_sigterm(baseobj->chansrv_pid);
        }
    }
}

/******************************************************************************/
static int
main_sess_proc_active(const struct session_data *baseobj)
{
    // Downcast the base object pointer to a pointer to the Wayland object
    const struct session_data_wayland *self =
        (const struct session_data_wayland *)baseobj;

    return (self->compositor_pid > 0);
}

/******************************************************************************/
/* The compositor leads the session's process group */
static pid_t
wayland_getpgid(const struct session_data *baseobj)
{
    // Downcast the base object pointer to a pointer to the Wayland object
    const struct session_data_wayland *self =
        (const struct session_data_wayland *)baseobj;

    return self->compositor_pid;
}

/******************************************************************************/
static void
send_term(struct session_data *baseobj)
{
    // Downcast the base object pointer to a pointer to the Wayland object
    struct session_data_wayland *self = (struct session_data_wayland *)baseobj;

    // Ending: a Wayland backend that exits now is not restarted
    self->terminating = 1;
    if (self->compositor_pid > 0)
    {
        // As for X11: the compositor's exit ends the rest of the session
        g_sigterm(self->compositor_pid);
    }
}

/******************************************************************************/
static int
get_display_server_fd(const struct session_data *baseobj,
                      const struct login_info *login_info)
{
    char portname[XRDP_SOCKETS_MAXPATH];
    int rv = -1;

    // Downcast the base object pointer to a pointer to the Wayland object
    const struct session_data_wayland *self =
        (const struct session_data_wayland *)baseobj;

    if (self->backend_pid <= 0)
    {
        LOG(LOG_LEVEL_ERROR,
            "Request to connect to display server %s"
            " which has exited", baseobj->display);
        return -1;
    }

    /* wlxrdp listens where xorgxrdp would */
    g_snprintf(portname, sizeof(portname), XRDP_X11RDP_STR,
               login_info->uid, baseobj->display);

    // Use the transport library to get the fd
    struct trans *t = trans_create(TRANS_MODE_UNIX, 8 * 8192, 8192);
    if (t == NULL)
    {
        LOG(LOG_LEVEL_ERROR, "Out of memory creating transport");
    }
    else if (trans_connect(t, "localhost", portname, 3000) != 0)
    {
        LOG(LOG_LEVEL_ERROR, "Can't connect to display server %s [%s]",
            baseobj->display, g_get_strerror());
    }
    else
    {
        rv = t->sck;
        t->sck = -1;
    }
    trans_delete(t);

    return rv;
}

/******************************************************************************/
static const struct session_data_vtable
    wayland_vtable =
{
    .start = start,
    .cleanup = cleanup,
    .process_child_exit = process_child_exit,
    .active_processes = active_processes,
    .main_sess_proc_active = main_sess_proc_active,
    .getpgid = wayland_getpgid,
    .send_term = send_term,
    .get_display_server_fd = get_display_server_fd
};

/******************************************************************************/
struct session_data_wayland *
session_wayland_new(void)
{
    struct session_data_wayland *self;
    self = (struct session_data_wayland *)g_malloc(sizeof(*self), 0);

    if (self == NULL)
    {
        LOG(LOG_LEVEL_ERROR, "Out of memory allocating session data struct");
    }
    else
    {
        self->base.vtable = &wayland_vtable;
        self->compositor_pid = -1;
        self->backend_pid = -1;
        self->backend_restart_window = 0;
        self->backend_restarts = 0;
        self->terminating = 0;
    }

    return self;
}
