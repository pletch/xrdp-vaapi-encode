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
 * @file session_wayland.h
 * @brief Wayland session objects
 *
 */


#ifndef SESSION_WAYLAND_H
#define SESSION_WAYLAND_H

#include <time.h>

#include "arch.h"
#include "session_base.h"

/**
 * Data involved in running a Wayland session (derived class)
 */
struct session_data_wayland
{
    struct session_data base;
    pid_t compositor_pid; ///< PID of the compositor (leads the session)
    pid_t backend_pid; ///< PID of wlxrdp
    time_t backend_restart_window; ///< start of the backend restart count
    unsigned int backend_restarts; ///< backend restarts in it
    int terminating; ///< send_term() has been called
};

/**
 * Create a new Wayland session object
 *
 * @return semi-initialised object; session_base_new() completes it
 */
struct session_data_wayland *
session_wayland_new(void);

#endif // SESSION_WAYLAND_H
