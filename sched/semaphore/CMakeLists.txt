# ##############################################################################
# sched/semaphore/CMakeLists.txt
#
# SPDX-License-Identifier: Apache-2.0
#
# Licensed to the Apache Software Foundation (ASF) under one or more contributor
# license agreements.  See the NOTICE file distributed with this work for
# additional information regarding copyright ownership.  The ASF licenses this
# file to you under the Apache License, Version 2.0 (the "License"); you may not
# use this file except in compliance with the License.  You may obtain a copy of
# the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
# WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
# License for the specific language governing permissions and limitations under
# the License.
#
# ##############################################################################

# Add semaphore-related files to the build
set(CSRCS
    sem_destroy.c
    sem_wait.c
    sem_trywait.c
    sem_tickwait.c
    sem_timedwait.c
    sem_clockwait.c
    sem_timeout.c
    sem_post.c
    sem_recover.c
    sem_reset.c
    sem_waitirq.c
    sem_rw.c)

if(CONFIG_PRIORITY_INHERITANCE)
  list(APPEND CSRCS sem_initialize.c sem_holder.c sem_setprotocol.c)
endif()

if(CONFIG_PRIORITY_PROTECT)
  list(APPEND CSRCS sem_protect.c)
endif()

target_sources(sched PRIVATE ${CSRCS})
