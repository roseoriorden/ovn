/* Copyright (c) 2021 Red Hat, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at:
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#ifndef STOPWATCH_NAMES_H
#define STOPWATCH_NAMES_H 1

#define NORTHD_LOOP_STOPWATCH_NAME "ovn-northd-loop"
#define OVNNB_DB_RUN_STOPWATCH_NAME "ovnnb_db_run"
#define OVNSB_DB_RUN_STOPWATCH_NAME "ovnsb_db_run"
#define BUILD_LFLOWS_CTX_STOPWATCH_NAME "build_flows_ctx"
#define CLEAR_LFLOWS_CTX_STOPWATCH_NAME "clear_lflows_ctx"
#define BUILD_LFLOWS_STOPWATCH_NAME "build_lflows"
#define LFLOWS_DATAPATHS_STOPWATCH_NAME "lflows_datapaths"
#define LFLOWS_PORTS_STOPWATCH_NAME "lflows_ports"
#define LFLOWS_LBS_STOPWATCH_NAME "lflows_lbs"
#define LFLOWS_LR_STATEFUL_STOPWATCH_NAME "lflows_lr_stateful"
#define LFLOWS_LS_STATEFUL_STOPWATCH_NAME "lflows_ls_stateful"
#define LFLOWS_IGMP_STOPWATCH_NAME "lflows_igmp"
#define LFLOWS_DP_GROUPS_STOPWATCH_NAME "lflows_dp_groups"
#define LFLOWS_TO_SB_STOPWATCH_NAME "lflows_to_sb"
#define PORT_GROUP_RUN_STOPWATCH_NAME "port_group_run"
#define SYNC_METERS_RUN_STOPWATCH_NAME "sync_meters_run"
#define LR_NAT_RUN_STOPWATCH_NAME "lr_nat_run"
#define LR_STATEFUL_RUN_STOPWATCH_NAME "lr_stateful"
#define LS_STATEFUL_RUN_STOPWATCH_NAME "ls_stateful"
#define ADVERTISED_ROUTE_SYNC_RUN_STOPWATCH_NAME "advertised_route_sync"
#define LEARNED_ROUTE_SYNC_RUN_STOPWATCH_NAME "learned_route_sync"
#define DYNAMIC_ROUTES_RUN_STOPWATCH_NAME "dynamic_routes"
#define GROUP_ECMP_ROUTE_RUN_STOPWATCH_NAME "group_ecmp_route"

#endif
