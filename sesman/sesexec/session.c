/**
 * xrdp: A Remote Desktop Protocol server.
 *
 * Copyright (C) Jay Sorg 2004-2015
 *
 * BSD process grouping by:
 * Copyright (c) 1995 Tatu Ylonen <ylo@cs.hut.fi>, Espoo, Finland.
 * Copyright (c) 2000-2001 Markus Friedl.
 * Copyright (c) 2011-2015 Koichiro Iwao, Kyushu Institute of Technology.
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
 * @file session.c
 * @brief Session management code
 * @author Jay Sorg, Simone Fedele
 *
 */

#if defined(HAVE_CONFIG_H)
#include "config_ac.h"
#endif

#include <stdio.h>
#include <errno.h>

#include "arch.h"
#include "session.h"
#include "session_base.h"
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
/**
 * Steps common to all session types, run before the session's own start()
 */
static enum scp_screate_status
session_start_preamble(struct login_info *login_info,
                       struct session_data *sd)
{
    /* Set the secondary groups before starting the session to prevent
     * problems on PAM-based systems (see Linux pam_setcred(3)).
     * If we have *BSD setusercontext() this is not done here */
#ifndef HAVE_SETUSERCONTEXT
    if (g_initgroups(login_info->username) != 0)
    {
        LOG(LOG_LEVEL_ERROR,
            "Failed to initialise secondary groups for %s: %s",
            login_info->username, g_get_strerror());
        return E_SCP_SCREATE_GENERAL_ERROR;
    }
#endif

    if (auth_start_session(login_info->auth_info, sd->display) != 0)
    {
        // Errors are logged by the auth module, as they are
        // specific to that module
        return E_SCP_SCREATE_GENERAL_ERROR;
    }
#ifdef USE_BSD_SETLOGIN
    /**
     * Create a new session and process group since the 4.4BSD
     * setlogin() affects the entire process group
     */
    if (g_setsid() < 0)
    {
        LOG(LOG_LEVEL_WARNING,
            "[session start] (display %s): setsid failed - pid %d",
            sd->display, g_getpid());
    }

    if (g_setlogin(login_info->username) < 0)
    {
        LOG(LOG_LEVEL_WARNING,
            "[session start] (display %s): setlogin failed for user %s - pid %d",
            sd->display, login_info->username, g_getpid());
    }
#endif

    return E_SCP_SCREATE_OK;
}

/******************************************************************************/
enum scp_screate_status
session_start(struct login_info *login_info,
              const struct session_parameters *sp,
              struct session_data **session_data)
{
    enum scp_screate_status status = E_SCP_SCREATE_OK;

    /* Create the session object (of the class for the session type) */
    struct session_data *sd = session_base_new(sp);
    if (sd == NULL)
    {
        status = E_SCP_SCREATE_NO_MEMORY;
    }
    else
    {
        if (sp->x11_display >= 0)
        {
            /* Initialise the display name for logging purposes */
            g_get_display_string_from_x11_display(sp->x11_display,
                                                  sd->display,
                                                  MAX_DISPLAY_NAME_SIZE);
            /* Add the DISPLAY to the list of environment variables we
             * set for all the sub-processes */
            char displayname[32];
            snprintf(displayname, sizeof(displayname), ":%d",
                     sp->x11_display);

            if (!list_add_strdup(g_cfg->env_names, "DISPLAY") ||
                    !list_add_strdup(g_cfg->env_values, displayname))
            {
                status = E_SCP_SCREATE_NO_MEMORY;
            }
        }

        if (status == E_SCP_SCREATE_OK)
        {
            status = session_start_preamble(login_info, sd);
        }

        if (status == E_SCP_SCREATE_OK)
        {
            status = sd->vtable->start(sd, login_info, sd->params);
        }

        if (status == E_SCP_SCREATE_OK)
        {
            *session_data = sd;
        }
        else
        {
            *session_data = NULL;
            session_base_destroy(sd);
        }
    }

    return status;
}

/******************************************************************************/
void
session_data_free(struct session_data *session_data)
{
    session_base_destroy(session_data);
}

/******************************************************************************/
void
session_process_sigchld_event(struct session_data *sd)
{
    session_base_process_sigchld_event(sd);
}

/******************************************************************************/
unsigned int
session_active(const struct session_data *sd)
{
    return (sd == NULL) ? 0 : sd->vtable->active_processes(sd);
}

/******************************************************************************/
time_t
session_get_start_time(const struct session_data *sd)
{
    return (sd == NULL) ? 0 : sd->start_time;
}

/******************************************************************************/
unsigned int
session_get_connect_count(const struct session_data *sd)
{
    return (sd == NULL) ? 0 : sd->connect_count;
}

/******************************************************************************/
const char *
session_get_display(const struct session_data *sd)
{
    return (sd == NULL) ? "" : sd->display;
}

/******************************************************************************/
unsigned int
session_increment_connect_count(struct session_data *sd)
{
    return (sd == NULL) ? 0 : sd->connect_count++;
}

/******************************************************************************/
const struct session_parameters *
session_get_parameters(const struct session_data *sd)
{
    return (sd == NULL) ? NULL : sd->params;
}

/******************************************************************************/
void
session_send_term(struct session_data *sd, int wait_for_all)
{
    if (sd != NULL)
    {
        session_base_send_term(sd, wait_for_all);
    }
}

/******************************************************************************/
static void
start_reconnect_script(struct session_data *sd,
                       const struct login_info *login_info,
                       void *closure)
{
    env_set_user(login_info->uid,
                 g_cfg->env_names,
                 g_cfg->env_values);

    auth_set_env(login_info->auth_info);

    if (g_file_exist(g_cfg->reconnect_sh))
    {
        /* The 'closure' parameter points to a list of strings
         * which need to be set in the environment for the reconnect script */
        if (closure != NULL)
        {
            const char **p = (const char **)closure;
            while (*p != NULL && *(p + 1) != NULL)
            {
                (void)g_setenv(*p, *(p + 1), 1);
                p += 2;
            }
        }
        LOG_DEVEL_LEAKING_FDS("reconnect script", 3, -1);

        LOG(LOG_LEVEL_INFO,
            "Starting session reconnection script on display %s: %s",
            sd->display, g_cfg->reconnect_sh);
        g_execlp3(g_cfg->reconnect_sh, g_cfg->reconnect_sh, 0);

        /* should not get here */
        LOG(LOG_LEVEL_ERROR,
            "Error starting session reconnection script on display %s: %s",
            sd->display, g_cfg->reconnect_sh);
    }
    else
    {
        LOG(LOG_LEVEL_WARNING,
            "Session reconnection script file does not exist: %s",
            g_cfg->reconnect_sh);
    }
}

/******************************************************************************/
void
session_run_reconnect_script(const struct login_info *login_info,
                             const struct session_data *sd,
                             const char *vars[])
{
    /* The fork helper passes the object on to the child, which does not
     * change it */
    struct session_data *self = (struct session_data *)sd;

    if (session_base_fork_child(self, login_info,
                                sd->vtable->getpgid(sd),
                                start_reconnect_script, (void *)vars) < 0)
    {
        LOG(LOG_LEVEL_ERROR, "Failed to fork for session reconnection script");
    }
}

/******************************************************************************/
int
session_get_display_server_fd(const struct login_info *login_info,
                              const struct session_data *sd)
{
    return sd->vtable->get_display_server_fd(sd, login_info);
}

/******************************************************************************/
int
session_get_chansrv_fd(const struct login_info *login_info,
                       const struct session_data *sd)
{
    char portname[XRDP_SOCKETS_MAXPATH];

    int rv = -1;

    if (sd->chansrv_pid <= 0)
    {
        LOG(LOG_LEVEL_ERROR,
            "Request to connect to chansrv %s"
            " which has exited", sd->display);
    }
    else
    {
        snprintf(portname, sizeof(portname),
                 XRDP_CHANSRV_STR, login_info->uid, sd->display);

        // Use the transport library to get the fd
        struct trans *t = trans_create(TRANS_MODE_UNIX, 8192, 8192);
        if (t == NULL)
        {
            LOG(LOG_LEVEL_ERROR, "Out of memory creating transport");
        }
        else if (trans_connect(t, NULL, portname, 10 * 1000) != 0)
        {
            LOG(LOG_LEVEL_ERROR, "Can't connect to chansrv %s [%s]",
                sd->display,
                g_get_strerror());
        }
        else
        {
            rv = t->sck;
            t->sck = -1;
        }
        trans_delete(t);
    }

    return rv;
}
