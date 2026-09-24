/**
 * xrdp: A Remote Desktop Protocol server.
 *
 * Copyright (C) 2026 Tim Pletcher, all xrdp contributors
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

#ifndef _RAIL_WL_H_
#define _RAIL_WL_H_

#include "arch.h"
#include "parse.h"

/* a Wayland RemoteApp session: sway's IPC socket is in SWAYSOCK */
int
rail_wl_enabled(void);
int
rail_wl_init(void);
int
rail_wl_deinit(void);
int
rail_wl_data_in(struct stream *s, int chan_id, int chan_flags,
                int length, int total_length);
int
rail_wl_get_wait_objs(tbus *objs, int *count, int *timeout);
int
rail_wl_check_wait_objs(void);

#endif
