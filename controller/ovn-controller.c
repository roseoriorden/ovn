/* Copyright (c) 2015, 2016, 2017 Nicira, Inc.
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

#include <config.h>

#include "ovn-controller.h"

#include <errno.h>
#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bfd.h"
#include "binding.h"
#include "chassis.h"
#include "command-line.h"
#include "compiler.h"
#include "daemon.h"
#include "dirs.h"
#include "openvswitch/dynamic-string.h"
#include "encaps.h"
#include "fatal-signal.h"
#include "lib/id-pool.h"
#include "if-status.h"
#include "ip-mcast.h"
#include "openvswitch/hmap.h"
#include "lb.h"
#include "lflow.h"
#include "lflow-cache.h"
#include "lflow-conj-ids.h"
#include "lib/vswitch-idl.h"
#include "lib/ovsdb-types.h"
#include "local_data.h"
#include "lport.h"
#include "memory.h"
#include "ofctrl.h"
#include "ofctrl-seqno.h"
#include "openvswitch/vconn.h"
#include "openvswitch/vlog.h"
#include "ovn/actions.h"
#include "ovn/features.h"
#include "lib/chassis-index.h"
#include "lib/extend-table.h"
#include "lib/ip-mcast-index.h"
#include "lib/mac-binding-index.h"
#include "lib/mcast-group-index.h"
#include "lib/ovn-dirs.h"
#include "lib/ovn-sb-idl.h"
#include "lib/ovn-util.h"
#include "ovsport.h"
#include "patch.h"
#include "vif-plug.h"
#include "vif-plug-provider.h"
#include "physical.h"
#include "pinctrl.h"
#include "openvswitch/poll-loop.h"
#include "lib/bitmap.h"
#include "lib/hash.h"
#include "simap.h"
#include "smap.h"
#include "sset.h"
#include "stream-ssl.h"
#include "stream.h"
#include "unixctl.h"
#include "util.h"
#include "timeval.h"
#include "timer.h"
#include "stopwatch.h"
#include "lib/inc-proc-eng.h"
#include "lib/ovn-l7.h"
#include "hmapx.h"
#include "mirror.h"
#include "mac-cache.h"
#include "statctrl.h"
#include "lib/dns-resolve.h"
#include "ct-zone.h"
#include "ovn-dns.h"
#include "acl-ids.h"
#include "route.h"
#include "route-exchange.h"
#include "route-table-notify.h"
#include "garp_rarp.h"
#include "host-if-monitor.h"
#include "neighbor.h"
#include "neighbor-exchange.h"
#include "neighbor-table-notify.h"
#include "evpn-arp.h"
#include "evpn-binding.h"
#include "evpn-fdb.h"

VLOG_DEFINE_THIS_MODULE(main);

static unixctl_cb_func ct_zone_list;
static unixctl_cb_func extend_table_list;
static unixctl_cb_func inject_pkt;
static unixctl_cb_func engine_recompute_cmd;
static unixctl_cb_func cluster_state_reset_cmd;
static unixctl_cb_func debug_pause_execution;
static unixctl_cb_func debug_resume_execution;
static unixctl_cb_func debug_status_execution;
static unixctl_cb_func debug_dump_local_bindings;
static unixctl_cb_func debug_dump_local_datapaths;
static unixctl_cb_func debug_dump_related_lports;
static unixctl_cb_func debug_dump_local_template_vars;
static unixctl_cb_func debug_dump_local_mac_bindings;
static unixctl_cb_func debug_dump_peer_ports;
static unixctl_cb_func debug_dump_lflow_conj_ids;
static unixctl_cb_func lflow_cache_flush_cmd;
static unixctl_cb_func lflow_cache_show_stats_cmd;
static unixctl_cb_func debug_delay_nb_cfg_report;

#define DEFAULT_BRIDGE_NAME "br-int"
#define DEFAULT_DATAPATH "system"

#define CONTROLLER_LOOP_STOPWATCH_NAME "flow-generation"
#define OFCTRL_PUT_STOPWATCH_NAME "flow-installation"
#define PINCTRL_RUN_STOPWATCH_NAME "pinctrl-run"
#define PATCH_RUN_STOPWATCH_NAME "patch-run"
#define CT_ZONE_COMMIT_STOPWATCH_NAME "ct-zone-commit"
#define IF_STATUS_MGR_RUN_STOPWATCH_NAME "if-status-mgr-run"
#define IF_STATUS_MGR_UPDATE_STOPWATCH_NAME "if-status-mgr-update"
#define OFCTRL_SEQNO_RUN_STOPWATCH_NAME "ofctrl-seqno-run"
#define BFD_RUN_STOPWATCH_NAME "bfd-run"
#define VIF_PLUG_RUN_STOPWATCH_NAME "vif-plug-run"

#define OVS_NB_CFG_NAME "ovn-nb-cfg"
#define OVS_NB_CFG_TS_NAME "ovn-nb-cfg-ts"
#define OVS_STARTUP_TS_NAME "ovn-startup-ts"

struct br_int_remote {
    char *target;
    int probe_interval;
};

static char *parse_options(int argc, char *argv[]);
OVS_NO_RETURN static void usage(void);

/* SSL/TLS options. */
static const char *ssl_private_key_file;
static const char *ssl_certificate_file;
static const char *ssl_ca_cert_file;

/* --unixctl-path: Path to use for unixctl server socket. */
static char *unixctl_path;

/* By default don't set an upper bound for the lflow cache and enable auto
 * trimming above 10K logical flows when reducing cache size by 50%.
 */
#define DEFAULT_LFLOW_CACHE_MAX_ENTRIES UINT32_MAX
#define DEFAULT_LFLOW_CACHE_MAX_MEM_KB (UINT64_MAX / 1024)
#define DEFAULT_LFLOW_CACHE_TRIM_LIMIT 10000
#define DEFAULT_LFLOW_CACHE_WMARK_PERC 50
#define DEFAULT_LFLOW_CACHE_TRIM_TO_MS 30000

struct controller_engine_ctx {
    struct lflow_cache *lflow_cache;
    struct if_status_mgr *if_mgr;
};

/* Pending packet to be injected into connected OVS. */
struct pending_pkt {
    /* Setting 'conn' indicates that a request is pending. */
    struct unixctl_conn *conn;
    char *flow_s;
};

/* Registered ofctrl seqno type for nb_cfg propagation. */
static size_t ofctrl_seq_type_nb_cfg;

static void
remove_newline(char *s)
{
    char *last = &s[strlen(s) - 1];
    switch (*last) {
    case '\n':
    case '\r':
        *last = '\0';
    default:
        return;
    }
}

static char *get_file_system_id(void)
{
    char *ret = NULL;
    char *filename = xasprintf("%s/system-id-override", ovn_sysconfdir());
    errno = 0;
    FILE *f = fopen(filename, "r");
    if (f) {
        char system_id[64];
        if (fgets(system_id, sizeof system_id, f)) {
            remove_newline(system_id);
            ret = xstrdup(system_id);
        }
        fclose(f);
    }
    free(filename);
    return ret;
}
/* Only set monitor conditions on tables that are available in the
 * server schema.
 */
#define sb_table_set_opt_mon_condition(idl, table, cond) \
    (sbrec_server_has_##table##_table(idl)               \
     ? sbrec_##table##_set_condition(idl, cond)          \
     : 0)

/* Assume the table exists in the server schema and set its condition. */
#define sb_table_set_req_mon_condition(idl, table, cond) \
    sbrec_##table##_set_condition(idl, cond)

static unsigned int
update_sb_monitors(struct ovsdb_idl *ovnsb_idl,
                   const struct sbrec_chassis *chassis,
                   const struct simap *local_ifaces,
                   const struct shash *local_bindings,
                   struct hmap *local_datapaths,
                   bool monitor_all)
{
    /* Monitor Port_Bindings rows for local interfaces and local datapaths.
     *
     * Monitor Logical_Flow, MAC_Binding, FDB, Multicast_Group, and DNS tables
     * for local datapaths.
     *
     * Monitor Controller_Event rows for local chassis.
     *
     * Monitor IP_Multicast for local datapaths.
     *
     * Monitor IGMP_Groups for local chassis.
     *
     * Monitor Template_Var for local chassis.
     *
     * Monitor ECMP_Nexthop for local datapaths.
     *
     * Monitor Advertised/Learned_Route for local datapaths.
     *
     * We always monitor patch ports because they allow us to see the linkages
     * between related logical datapaths.  That way, when we know that we have
     * a VIF on a particular logical switch, we immediately know to monitor all
     * the connected logical routers and logical switches. */
    struct ovsdb_idl_condition pb = OVSDB_IDL_CONDITION_INIT(&pb);
    struct ovsdb_idl_condition lf = OVSDB_IDL_CONDITION_INIT(&lf);
    struct ovsdb_idl_condition ldpg = OVSDB_IDL_CONDITION_INIT(&ldpg);
    struct ovsdb_idl_condition mb = OVSDB_IDL_CONDITION_INIT(&mb);
    struct ovsdb_idl_condition fdb = OVSDB_IDL_CONDITION_INIT(&fdb);
    struct ovsdb_idl_condition mg = OVSDB_IDL_CONDITION_INIT(&mg);
    struct ovsdb_idl_condition dns = OVSDB_IDL_CONDITION_INIT(&dns);
    struct ovsdb_idl_condition ce =  OVSDB_IDL_CONDITION_INIT(&ce);
    struct ovsdb_idl_condition ip_mcast = OVSDB_IDL_CONDITION_INIT(&ip_mcast);
    struct ovsdb_idl_condition igmp = OVSDB_IDL_CONDITION_INIT(&igmp);
    struct ovsdb_idl_condition chprv = OVSDB_IDL_CONDITION_INIT(&chprv);
    struct ovsdb_idl_condition tv = OVSDB_IDL_CONDITION_INIT(&tv);
    struct ovsdb_idl_condition nh = OVSDB_IDL_CONDITION_INIT(&nh);
    struct ovsdb_idl_condition ar = OVSDB_IDL_CONDITION_INIT(&ar);
    struct ovsdb_idl_condition lr = OVSDB_IDL_CONDITION_INIT(&lr);
    struct ovsdb_idl_condition amb = OVSDB_IDL_CONDITION_INIT(&amb);

    /* Always monitor all logical datapath groups. Otherwise, DPG updates may
     * be received *after* the lflows using it are seen by ovn-controller.
     * Since the number of DPGs are relatively small, we monitor all DPGs to
     * avoid the unnecessarily extra wake-ups of ovn-controller. */
    ovsdb_idl_condition_add_clause_true(&ldpg);

    /* Always monitor all learned routes. Otherwise, when we have a new local
     * datapath we directly try to learn routes from the vrf (if it exists).
     * If we then do not know all learned routes of this datapath we can get
     * duplicates.
     * XXX: This should be optimized, e.g. if we find a way to defer to learn
     * routes until db conditions are updated. */
    ovsdb_idl_condition_add_clause_true(&lr);

    if (monitor_all) {
        /* Monitor all Southbound tables unconditionally.  Do that even for
         * tables that could be easily filtered by chassis name (like
         * Chassis_Private).  That's because the current ovsdb-server
         * implementation uses a cache whose efficiency significantly
         * decreases when monitor conditions are present. */
        ovsdb_idl_condition_add_clause_true(&pb);
        ovsdb_idl_condition_add_clause_true(&lf);
        ovsdb_idl_condition_add_clause_true(&mb);
        ovsdb_idl_condition_add_clause_true(&fdb);
        ovsdb_idl_condition_add_clause_true(&mg);
        ovsdb_idl_condition_add_clause_true(&dns);
        ovsdb_idl_condition_add_clause_true(&ce);
        ovsdb_idl_condition_add_clause_true(&ip_mcast);
        ovsdb_idl_condition_add_clause_true(&igmp);
        ovsdb_idl_condition_add_clause_true(&chprv);
        ovsdb_idl_condition_add_clause_true(&tv);
        ovsdb_idl_condition_add_clause_true(&nh);
        ovsdb_idl_condition_add_clause_true(&ar);
        ovsdb_idl_condition_add_clause_true(&amb);
        goto out;
    }

    sbrec_port_binding_add_clause_type(&pb, OVSDB_F_EQ, "patch");
    /* XXX: We can optimize this, if we find a way to only monitor
     * ports that have a Gateway_Chassis that point's to our own
     * chassis */
    sbrec_port_binding_add_clause_type(&pb, OVSDB_F_EQ, "chassisredirect");
    sbrec_port_binding_add_clause_type(&pb, OVSDB_F_EQ, "external");
    if (chassis) {
        /* This should be mostly redundant with the other clauses for port
         * bindings, but it allows us to catch any ports that are assigned to
         * us but should not be.  That way, we can clear their chassis
         * assignments. */
        sbrec_port_binding_add_clause_chassis(&pb, OVSDB_F_EQ,
                                              &chassis->header_.uuid);

        sbrec_port_binding_add_clause_requested_chassis(
            &pb, OVSDB_F_EQ, &chassis->header_.uuid);

        /* Ensure that we find out about l2gateway and l3gateway ports that
         * should be present on this chassis.  Otherwise, we might never find
         * out about those ports, if their datapaths don't otherwise have a VIF
         * in this chassis. */
        const char *id = chassis->name;
        const struct smap l2 = SMAP_CONST1(&l2, "l2gateway-chassis", id);
        sbrec_port_binding_add_clause_options(&pb, OVSDB_F_INCLUDES, &l2);
        const struct smap l3 = SMAP_CONST1(&l3, "l3gateway-chassis", id);
        sbrec_port_binding_add_clause_options(&pb, OVSDB_F_INCLUDES, &l3);

        sbrec_controller_event_add_clause_chassis(&ce, OVSDB_F_EQ,
                                                  &chassis->header_.uuid);
        sbrec_igmp_group_add_clause_chassis(&igmp, OVSDB_F_EQ,
                                            &chassis->header_.uuid);

        /* Monitors Chassis_Private record for current chassis only. */
        sbrec_chassis_private_add_clause_name(&chprv, OVSDB_F_EQ,
                                              chassis->name);

        sbrec_chassis_template_var_add_clause_chassis(&tv, OVSDB_F_EQ,
                                                      chassis->name);
    } else {
        /* During initialization, we monitor all records in Chassis_Private so
         * that we don't try to recreate existing ones. */
        ovsdb_idl_condition_add_clause_true(&chprv);
        /* Also, to avoid traffic disruption (e.g., conntrack flushing for
         * zones that are used by OVN but not yet known due to the SB initial
         * contents not being available), monitor all port bindings
         * connected to gateways; they might be claimed as soon as the
         * chassis is available.
         */
        sbrec_port_binding_add_clause_type(&pb, OVSDB_F_EQ, "l2gateway");
        sbrec_port_binding_add_clause_type(&pb, OVSDB_F_EQ, "l3gateway");

        /* Monitor all advertised routes during startup.
         * Otherwise, once we claim a port on startup we do not yet know the
         * routes to advertise and might wrongly delete already installed
         * ones. */
        ovsdb_idl_condition_add_clause_true(&ar);
    }

    if (local_ifaces) {
        const char *name;

        ovs_assert(local_bindings);
        const struct simap_node *n;
        SIMAP_FOR_EACH (n, local_ifaces) {
            name = n->name;
            /* Skip the VIFs we bound already, we should have a local datapath
             * for those. */
            const struct sbrec_port_binding *local_pb
                = local_binding_get_primary_pb(local_bindings, name);
            if (local_pb && get_lport_type(local_pb) == LP_VIF &&
                local_pb->chassis == chassis) {
                continue;
            }
            sbrec_port_binding_add_clause_logical_port(&pb, OVSDB_F_EQ, name);
        }
        /* Monitor all sub-ports unconditionally; we don't expect a lot of
         * them in the SB database. */
        sbrec_port_binding_add_clause_parent_port(&pb, OVSDB_F_NE, NULL);
    }
    if (local_datapaths) {
        const struct local_datapath *ld;
        HMAP_FOR_EACH (ld, hmap_node, local_datapaths) {
            struct uuid *uuid = CONST_CAST(struct uuid *,
                                           &ld->datapath->header_.uuid);
            sbrec_port_binding_add_clause_datapath(&pb, OVSDB_F_EQ, uuid);
            sbrec_logical_flow_add_clause_logical_datapath(&lf, OVSDB_F_EQ,
                                                           uuid);
            sbrec_mac_binding_add_clause_datapath(&mb, OVSDB_F_EQ, uuid);
            sbrec_fdb_add_clause_dp_key(&fdb, OVSDB_F_EQ,
                                        ld->datapath->tunnel_key);
            sbrec_multicast_group_add_clause_datapath(&mg, OVSDB_F_EQ, uuid);
            sbrec_dns_add_clause_datapaths(&dns, OVSDB_F_INCLUDES, &uuid, 1);
            sbrec_ip_multicast_add_clause_datapath(&ip_mcast, OVSDB_F_EQ,
                                                   uuid);
            sbrec_ecmp_nexthop_add_clause_datapath(&nh, OVSDB_F_EQ, uuid);
            sbrec_advertised_route_add_clause_datapath(&ar, OVSDB_F_EQ, uuid);
            sbrec_advertised_mac_binding_add_clause_datapath(&amb, OVSDB_F_EQ,
                                                             uuid);
        }

        /* Datapath groups are immutable, which means a new group record is
         * created when a datapath is added to a group.  The logical flows
         * referencing a datapath group are also updated in such cases but the
         * new group UUID is not known by ovn-controller until the SB update
         * is received.  To avoid unnecessarily removing and adding lflows
         * that reference datapath groups, set the monitor condition to always
         * request all of them.
         */
        sbrec_logical_flow_add_clause_logical_dp_group(&lf, OVSDB_F_NE, NULL);
    }

out:;
    unsigned int cond_seqnos[] = {
        sb_table_set_req_mon_condition(ovnsb_idl, port_binding, &pb),
        sb_table_set_req_mon_condition(ovnsb_idl, logical_flow, &lf),
        sb_table_set_req_mon_condition(ovnsb_idl, logical_dp_group, &ldpg),
        sb_table_set_req_mon_condition(ovnsb_idl, mac_binding, &mb),
        sb_table_set_req_mon_condition(ovnsb_idl, fdb, &fdb),
        sb_table_set_req_mon_condition(ovnsb_idl, multicast_group, &mg),
        sb_table_set_req_mon_condition(ovnsb_idl, dns, &dns),
        sb_table_set_req_mon_condition(ovnsb_idl, controller_event, &ce),
        sb_table_set_req_mon_condition(ovnsb_idl, ip_multicast, &ip_mcast),
        sb_table_set_req_mon_condition(ovnsb_idl, igmp_group, &igmp),
        sb_table_set_req_mon_condition(ovnsb_idl, chassis_private, &chprv),
        sb_table_set_opt_mon_condition(ovnsb_idl, chassis_template_var, &tv),
        sb_table_set_opt_mon_condition(ovnsb_idl, ecmp_nexthop, &nh),
        sb_table_set_opt_mon_condition(ovnsb_idl, advertised_route, &ar),
        sb_table_set_opt_mon_condition(ovnsb_idl, learned_route, &lr),
        sb_table_set_opt_mon_condition(ovnsb_idl, advertised_mac_binding,
                                       &amb),
    };

    unsigned int expected_cond_seqno = 0;
    for (size_t i = 0; i < ARRAY_SIZE(cond_seqnos); i++) {
        expected_cond_seqno = MAX(expected_cond_seqno, cond_seqnos[i]);
    }

    ovsdb_idl_condition_destroy(&pb);
    ovsdb_idl_condition_destroy(&lf);
    ovsdb_idl_condition_destroy(&ldpg);
    ovsdb_idl_condition_destroy(&mb);
    ovsdb_idl_condition_destroy(&fdb);
    ovsdb_idl_condition_destroy(&mg);
    ovsdb_idl_condition_destroy(&dns);
    ovsdb_idl_condition_destroy(&ce);
    ovsdb_idl_condition_destroy(&ip_mcast);
    ovsdb_idl_condition_destroy(&igmp);
    ovsdb_idl_condition_destroy(&chprv);
    ovsdb_idl_condition_destroy(&tv);
    ovsdb_idl_condition_destroy(&nh);
    ovsdb_idl_condition_destroy(&ar);
    ovsdb_idl_condition_destroy(&lr);
    ovsdb_idl_condition_destroy(&amb);
    return expected_cond_seqno;
}

static const char *
br_int_name(const struct ovsrec_open_vswitch_table *ovs_table)
{
    const struct ovsrec_open_vswitch *cfg =
        ovsrec_open_vswitch_table_first(ovs_table);
    const char *chassis_id = get_ovs_chassis_id(ovs_table);
    return get_chassis_external_id_value(&cfg->external_ids, chassis_id,
                                         "ovn-bridge", DEFAULT_BRIDGE_NAME);
}

static const struct ovsrec_bridge *
create_br_int(struct ovsdb_idl_txn *ovs_idl_txn,
              const struct ovsrec_open_vswitch_table *ovs_table)
{
    const struct ovsrec_open_vswitch *cfg;
    cfg = ovsrec_open_vswitch_table_first(ovs_table);
    if (!cfg) {
        return NULL;
    }
    const char *bridge_name = br_int_name(ovs_table);

    ovsdb_idl_txn_add_comment(ovs_idl_txn,
            "ovn-controller: creating integration bridge '%s'", bridge_name);

    struct ovsrec_interface *iface;
    iface = ovsrec_interface_insert(ovs_idl_txn);
    ovsrec_interface_set_name(iface, bridge_name);
    ovsrec_interface_set_type(iface, "internal");

    struct ovsrec_port *port;
    port = ovsrec_port_insert(ovs_idl_txn);
    ovsrec_port_set_name(port, bridge_name);
    ovsrec_port_set_interfaces(port, &iface, 1);

    struct ovsrec_bridge *bridge;
    bridge = ovsrec_bridge_insert(ovs_idl_txn);
    ovsrec_bridge_set_name(bridge, bridge_name);
    ovsrec_bridge_set_fail_mode(bridge, "secure");
    ovsrec_bridge_set_ports(bridge, &port, 1);

    struct smap oc = SMAP_INITIALIZER(&oc);
    smap_add(&oc, "disable-in-band", "true");

    /* When a first non-local port is added to the integration bridge, it
     * results in the recalculation of datapath-id by ovs-vswitchd forcing all
     * active connections to the controllers to reconnect.
     *
     * We can avoid the disconnection by setting the 'other_config:hwaddr' for
     * the integration bridge. ovs-vswitchd uses this hwaddr to calculate the
     * datapath-id and it doesn't recalculate the datapath-id later when the
     * first non-local port is added.
     *
     * So generate a random mac and set the 'hwaddr' option in the
     * other_config.
     * */
    struct eth_addr br_hwaddr;
    eth_addr_random(&br_hwaddr);
    char ea_s[ETH_ADDR_STRLEN + 1];
    snprintf(ea_s, sizeof ea_s, ETH_ADDR_FMT,
             ETH_ADDR_ARGS(br_hwaddr));
    smap_add(&oc, "hwaddr", ea_s);

    ovsrec_bridge_set_other_config(bridge, &oc);
    smap_destroy(&oc);

    struct ovsrec_bridge **bridges;
    size_t bytes = sizeof *bridges * cfg->n_bridges;
    bridges = xmalloc(bytes + sizeof *bridges);
    if (cfg->n_bridges) {
        memcpy(bridges, cfg->bridges, bytes);
    }
    bridges[cfg->n_bridges] = bridge;
    ovsrec_open_vswitch_verify_bridges(cfg);
    ovsrec_open_vswitch_set_bridges(cfg, bridges, cfg->n_bridges + 1);
    free(bridges);

    return bridge;
}

static const struct ovsrec_datapath *
create_br_datapath(struct ovsdb_idl_txn *ovs_idl_txn,
                   const struct ovsrec_open_vswitch *cfg,
                   const char *datapath_type)
{
    ovsdb_idl_txn_add_comment(ovs_idl_txn,
                              "ovn-controller: creating bridge datapath '%s'",
                              datapath_type);

    struct ovsrec_datapath *dp = ovsrec_datapath_insert(ovs_idl_txn);
    ovsrec_open_vswitch_verify_datapaths(cfg);
    ovsrec_open_vswitch_update_datapaths_setkey(cfg, datapath_type, dp);
    return dp;
}

#define N_FLOW_TABLES 255

static void
update_flow_table_prefixes(struct ovsdb_idl_txn *ovs_idl_txn,
                           const struct ovsrec_bridge *br_int)
{
    const struct ovsdb_type *server_type;
    struct ds ds = DS_EMPTY_INITIALIZER;
    const char *prefixes[] = {
        "ip_src", "ip_dst", "ipv6_src", "ipv6_dst",
    };
    struct ovsrec_flow_table *ft;
    size_t max_prefixes;
    size_t i;

    /* We must not attempt setting more prefixes than our IDL supports.
     * Note: This should be a build time assertion, but IDL structures
     * are not defined as constants. */
    ovs_assert(
        ARRAY_SIZE(prefixes) <=
        ovsrec_flow_table_columns[OVSREC_FLOW_TABLE_COL_PREFIXES].type.n_max);

    server_type = ovsrec_flow_table_prefixes_server_type(
                                ovsdb_idl_txn_get_idl(ovs_idl_txn));
    if (!server_type) {
        /* Not connected or not in the server's schema somehow. */
        return;
    }

    max_prefixes = MIN(server_type->n_max, ARRAY_SIZE(prefixes));
    if (br_int->n_flow_tables == N_FLOW_TABLES &&
        br_int->value_flow_tables[0]->n_prefixes == max_prefixes) {
        /* Already up to date.  Ideally, we would check every table,
         * but it seems excessive. */
        return;
    }

    for (i = 1; i < br_int->n_flow_tables; i++) {
        if (br_int->value_flow_tables[i] != br_int->value_flow_tables[0]) {
            break;
        }
    }
    if (i == N_FLOW_TABLES) {
        /* Correct number of flow tables and all pointing to the same row. */
        ft = br_int->value_flow_tables[0];
    } else {
        /* Unexpected configuration.  Let's create a new flow table row.
         * Old ones will be garbage collected by the database. */
        struct ovsrec_flow_table *values[N_FLOW_TABLES];
        int64_t keys[N_FLOW_TABLES];

        ft = ovsrec_flow_table_insert(ovs_idl_txn);
        for (i = 0; i < ARRAY_SIZE(values); i++) {
            keys[i] = i;
            values[i] = ft;
        }
        ovsrec_bridge_set_flow_tables(br_int, keys, values,
                                      ARRAY_SIZE(values));
    }

    ds_put_cstr(&ds, "Setting flow table prefixes:");
    for (i = 0 ; i < max_prefixes; i++) {
        ds_put_char(&ds, ' ');
        ds_put_cstr(&ds, prefixes[i]);
        ds_put_char(&ds, ',');
    }
    ds_chomp(&ds, ',');
    VLOG_INFO("%s.", ds_cstr_ro(&ds));
    ds_destroy(&ds);

    ovsrec_flow_table_set_prefixes(ft, prefixes, max_prefixes);
}

static const struct ovsrec_bridge *
get_br_int(const struct ovsrec_bridge_table *bridge_table,
           const struct ovsrec_open_vswitch_table *ovs_table)
{
    const struct ovsrec_open_vswitch *cfg;
    cfg = ovsrec_open_vswitch_table_first(ovs_table);
    if (!cfg) {
        return NULL;
    }

    return get_bridge(bridge_table, br_int_name(ovs_table));
}

static const struct ovsrec_datapath *
get_br_datapath(const struct ovsrec_open_vswitch *cfg,
                const char *datapath_type)
{
    for (size_t i = 0; i < cfg->n_datapaths; i++) {
        if (!strcmp(cfg->key_datapaths[i], datapath_type)) {
            return cfg->value_datapaths[i];
        }
    }
    return NULL;
}

static void
process_br_int(struct ovsdb_idl_txn *ovs_idl_txn,
               const struct ovsrec_bridge_table *bridge_table,
               const struct ovsrec_open_vswitch_table *ovs_table,
               const struct ovsrec_bridge **br_int_,
               const struct ovsrec_datapath **br_int_dp)
{
    const struct ovsrec_bridge *br_int = get_br_int(bridge_table, ovs_table);

    ovs_assert(br_int_);
    if (ovs_idl_txn) {
        if (!br_int) {
            br_int = create_br_int(ovs_idl_txn, ovs_table);
        }

        if (br_int) {
            const struct ovsrec_open_vswitch *cfg =
                ovsrec_open_vswitch_table_first(ovs_table);
            ovs_assert(cfg);

            /* Propagate "ovn-bridge-datapath-type" from OVS table, if any.
             * Otherwise use the datapath-type set in br-int, if any.
             * Finally, assume "system" datapath if none configured.
             */
            const char *chassis_id = get_ovs_chassis_id(ovs_table);
            const char *datapath_type =
                get_chassis_external_id_value(
                    &cfg->external_ids, chassis_id,
                    "ovn-bridge-datapath-type", NULL);

            if (!datapath_type) {
                if (br_int->datapath_type[0]) {
                    datapath_type = br_int->datapath_type;
                } else {
                    datapath_type = DEFAULT_DATAPATH;
                }
            }
            if (strcmp(br_int->datapath_type, datapath_type)) {
                ovsrec_bridge_set_datapath_type(br_int, datapath_type);
            }
            if (!br_int->fail_mode || strcmp(br_int->fail_mode, "secure")) {
                ovsrec_bridge_set_fail_mode(br_int, "secure");
                VLOG_WARN("Integration bridge fail-mode changed to 'secure'.");
            }
            if (br_int_dp) {
                *br_int_dp = get_br_datapath(cfg, datapath_type);
                if (!(*br_int_dp)) {
                    *br_int_dp = create_br_datapath(ovs_idl_txn, cfg,
                                                    datapath_type);
                }
            }

            update_flow_table_prefixes(ovs_idl_txn, br_int);
        }
    }
    *br_int_ = br_int;
}

static void
update_ssl_config(const struct ovsrec_ssl_table *ssl_table)
{
    const struct ovsrec_ssl *ssl = ovsrec_ssl_table_first(ssl_table);

    if (ssl) {
        stream_ssl_set_key_and_cert(ssl->private_key, ssl->certificate);
        stream_ssl_set_ca_cert_file(ssl->ca_cert, ssl->bootstrap_ca_cert);
    } else {
        if (ssl_private_key_file && ssl_certificate_file) {
            stream_ssl_set_key_and_cert(ssl_private_key_file,
                                        ssl_certificate_file);
        }
        if (ssl_ca_cert_file) {
            stream_ssl_set_ca_cert_file(ssl_ca_cert_file, false);
        }
    }
}

/* Retrieves the pointer to the OVN Southbound database from 'ovs_idl' and
 * updates 'sbdb_idl' with that pointer. */
static void
update_sb_db(struct ovsdb_idl *ovs_idl, struct ovsdb_idl *ovnsb_idl,
             bool *monitor_all_p, bool *reset_ovnsb_idl_min_index,
             struct controller_engine_ctx *ctx,
             unsigned int *sb_cond_seqno)
{
    const struct ovsrec_open_vswitch *cfg = ovsrec_open_vswitch_first(ovs_idl);
    if (!cfg) {
        return;
    }

    /* Set remote based on user configuration. */
    const struct ovsrec_open_vswitch_table *ovs_table =
        ovsrec_open_vswitch_table_get(ovs_idl);
    const char *chassis_id = get_ovs_chassis_id(ovs_table);
    const char *remote =
        get_chassis_external_id_value(
            &cfg->external_ids, chassis_id, "ovn-remote", NULL);
    ovsdb_idl_set_remote(ovnsb_idl, remote, true);

    /* Set probe interval, based on user configuration and the remote. */
    int interval =
        get_chassis_external_id_value_int(
            &cfg->external_ids, chassis_id, "ovn-remote-probe-interval", -1);
    set_idl_probe_interval(ovnsb_idl, remote, interval);

    bool monitor_all =
        get_chassis_external_id_value_bool(
            &cfg->external_ids, chassis_id, "ovn-monitor-all", false);
    if (monitor_all) {
        /* Always call update_sb_monitors when monitor_all is true.
         * Otherwise, don't call it here, because there would be unnecessary
         * extra cost. Instead, it is called after the engine execution only
         * when it is necessary. */
        unsigned int next_cond_seqno =
            update_sb_monitors(ovnsb_idl, NULL, NULL, NULL, NULL, true);
        if (sb_cond_seqno) {
            *sb_cond_seqno = next_cond_seqno;
        }
    }
    if (monitor_all_p) {
        *monitor_all_p = monitor_all;
    }
    if (reset_ovnsb_idl_min_index && *reset_ovnsb_idl_min_index) {
        VLOG_INFO("Resetting southbound database cluster state");
        engine_set_force_recompute();
        ovsdb_idl_reset_min_index(ovnsb_idl);
        *reset_ovnsb_idl_min_index = false;
    }

    if (ctx) {
        lflow_cache_enable(
            ctx->lflow_cache,
            get_chassis_external_id_value_bool(
                &cfg->external_ids, chassis_id,
                "ovn-enable-lflow-cache", true),
            get_chassis_external_id_value_uint(
                &cfg->external_ids, chassis_id,
                "ovn-limit-lflow-cache",
                DEFAULT_LFLOW_CACHE_MAX_ENTRIES),
            get_chassis_external_id_value_ullong(
                &cfg->external_ids, chassis_id,
                "ovn-memlimit-lflow-cache-kb",
                DEFAULT_LFLOW_CACHE_MAX_MEM_KB),
            get_chassis_external_id_value_uint(
                &cfg->external_ids, chassis_id,
                "ovn-trim-limit-lflow-cache",
                DEFAULT_LFLOW_CACHE_TRIM_LIMIT),
            get_chassis_external_id_value_uint(
                &cfg->external_ids, chassis_id,
                "ovn-trim-wmark-perc-lflow-cache",
                DEFAULT_LFLOW_CACHE_WMARK_PERC),
            get_chassis_external_id_value_uint(
                &cfg->external_ids, chassis_id,
                "ovn-trim-timeout-ms",
                DEFAULT_LFLOW_CACHE_TRIM_TO_MS));
    }
}

/* Connection tracking zones. */
struct ed_type_ct_zones {
    struct ct_zone_ctx ctx;

    /* Tracked data. */
    bool recomputed;
};


static uint64_t
get_nb_cfg(const struct sbrec_sb_global_table *sb_global_table,
           unsigned int cond_seqno, unsigned int expected_cond_seqno)
{
    static uint64_t nb_cfg = 0;

    /* Delay getting nb_cfg if there are monitor condition changes
     * in flight.  It might be that those changes would instruct the
     * server to send updates that happened before SB_Global.nb_cfg.
     */
    if (cond_seqno != expected_cond_seqno) {
        return nb_cfg;
    }

    const struct sbrec_sb_global *sb
        = sbrec_sb_global_table_first(sb_global_table);
    nb_cfg = sb ? sb->nb_cfg : 0;
    return nb_cfg;
}

/* Propagates the local cfg seqno, 'cur_cfg', to the chassis_private record
 * and to the local OVS DB.
 */
static void
store_nb_cfg(struct ovsdb_idl_txn *sb_txn, struct ovsdb_idl_txn *ovs_txn,
             const struct sbrec_chassis_private *chassis,
             const struct ovsrec_bridge *br_int,
             unsigned int delay_nb_cfg_report,
             bool enable_ch_nb_cfg_update)
{
    struct ofctrl_acked_seqnos *acked_nb_cfg_seqnos =
        ofctrl_acked_seqnos_get(ofctrl_seq_type_nb_cfg);
    uint64_t cur_cfg = acked_nb_cfg_seqnos->last_acked;
    int64_t startup_ts = daemon_startup_ts();

    if (ovs_txn && br_int
            && startup_ts != smap_get_ullong(&br_int->external_ids,
                                             OVS_STARTUP_TS_NAME, 0)) {
        char *startup_ts_str = xasprintf("%"PRId64, startup_ts);
        ovsrec_bridge_update_external_ids_setkey(br_int, OVS_STARTUP_TS_NAME,
                                                 startup_ts_str);
        free(startup_ts_str);
    }

    if (!cur_cfg) {
        goto done;
    }

    long long ts_now = time_wall_msec();

    if (sb_txn && chassis && cur_cfg != chassis->nb_cfg
        && enable_ch_nb_cfg_update) {
        sbrec_chassis_private_set_nb_cfg(chassis, cur_cfg);
        sbrec_chassis_private_set_nb_cfg_timestamp(chassis, ts_now);

        if (delay_nb_cfg_report) {
            VLOG_INFO("Sleep for %u sec", delay_nb_cfg_report);
            xsleep(delay_nb_cfg_report);
        }
    }

    if (ovs_txn && br_int && cur_cfg != smap_get_ullong(&br_int->external_ids,
                                                        OVS_NB_CFG_NAME, 0)) {
        char *cur_cfg_ts_str = xasprintf("%lld", ts_now);
        char *cur_cfg_str = xasprintf("%"PRId64, cur_cfg);
        ovsrec_bridge_update_external_ids_setkey(br_int, OVS_NB_CFG_NAME,
                                                 cur_cfg_str);
        ovsrec_bridge_update_external_ids_setkey(br_int, OVS_NB_CFG_TS_NAME,
                                                 cur_cfg_ts_str);
        free(cur_cfg_ts_str);
        free(cur_cfg_str);
    }

done:
    ofctrl_acked_seqnos_destroy(acked_nb_cfg_seqnos);
}

static const char *
get_transport_zones(const struct ovsrec_open_vswitch_table *ovs_table)
{
    const struct ovsrec_open_vswitch *cfg
        = ovsrec_open_vswitch_table_first(ovs_table);
    const char *chassis_id = get_ovs_chassis_id(ovs_table);
    return get_chassis_external_id_value(&cfg->external_ids, chassis_id,
                                         "ovn-transport-zones", "");
}

static bool
get_ovn_cleanup_on_exit(const struct ovsrec_open_vswitch_table *ovs_table)
{
    const struct ovsrec_open_vswitch *cfg =
        ovsrec_open_vswitch_table_first(ovs_table);
    const char *chassis_id = get_ovs_chassis_id(ovs_table);
    if (!cfg || !chassis_id) {
        return false;
    }

    return get_chassis_external_id_value_bool(&cfg->external_ids, chassis_id,
                                              "ovn-cleanup-on-exit", true);
}

static void
ctrl_register_ovs_idl(struct ovsdb_idl *ovs_idl)
{
    /* We do not monitor all tables by default, so modules must register
     * their interest explicitly.
     * XXX: when the same column is monitored in different modes by different
     * modules, there is a chance that "track" flag added by
     * ovsdb_idl_track_add_column by one module being overwritten by a
     * following ovsdb_idl_add_column by another module. Before this is fixed
     * in OVSDB IDL, we need to be careful about the order so that the "track"
     * calls are after the "non-track" calls. */
    ovsdb_idl_add_table(ovs_idl, &ovsrec_table_open_vswitch);
    ovsdb_idl_add_column(ovs_idl, &ovsrec_open_vswitch_col_other_config);
    ovsdb_idl_add_column(ovs_idl, &ovsrec_open_vswitch_col_bridges);
    ovsdb_idl_add_column(ovs_idl, &ovsrec_open_vswitch_col_datapaths);
    ovsdb_idl_add_table(ovs_idl, &ovsrec_table_interface);
    ovsdb_idl_add_table(ovs_idl, &ovsrec_table_port);
    ovsdb_idl_add_table(ovs_idl, &ovsrec_table_bridge);
    ovsdb_idl_add_column(ovs_idl, &ovsrec_bridge_col_ports);
    ovsdb_idl_add_column(ovs_idl, &ovsrec_bridge_col_name);
    ovsdb_idl_add_column(ovs_idl, &ovsrec_bridge_col_fail_mode);
    ovsdb_idl_add_column(ovs_idl, &ovsrec_bridge_col_flow_tables);
    ovsdb_idl_add_column(ovs_idl, &ovsrec_bridge_col_other_config);
    ovsdb_idl_add_column(ovs_idl, &ovsrec_bridge_col_external_ids);
    ovsdb_idl_add_table(ovs_idl, &ovsrec_table_flow_table);
    ovsdb_idl_add_column(ovs_idl, &ovsrec_flow_table_col_prefixes);
    ovsdb_idl_add_table(ovs_idl, &ovsrec_table_ssl);
    ovsdb_idl_add_column(ovs_idl, &ovsrec_ssl_col_bootstrap_ca_cert);
    ovsdb_idl_add_column(ovs_idl, &ovsrec_ssl_col_ca_cert);
    ovsdb_idl_add_column(ovs_idl, &ovsrec_ssl_col_certificate);
    ovsdb_idl_add_column(ovs_idl, &ovsrec_ssl_col_private_key);
    ovsdb_idl_add_table(ovs_idl, &ovsrec_table_datapath);
    ovsdb_idl_add_column(ovs_idl, &ovsrec_datapath_col_capabilities);
    ovsdb_idl_add_column(ovs_idl, &ovsrec_datapath_col_ct_zones);
    ovsdb_idl_add_table(ovs_idl, &ovsrec_table_flow_sample_collector_set);
    ovsdb_idl_add_table(ovs_idl, &ovsrec_table_qos);
    ovsdb_idl_add_column(ovs_idl, &ovsrec_qos_col_other_config);
    ovsdb_idl_add_column(ovs_idl, &ovsrec_qos_col_external_ids);
    ovsdb_idl_add_column(ovs_idl, &ovsrec_qos_col_queues);
    ovsdb_idl_add_table(ovs_idl, &ovsrec_table_queue);
    ovsdb_idl_add_column(ovs_idl, &ovsrec_queue_col_other_config);
    ovsdb_idl_add_column(ovs_idl, &ovsrec_queue_col_external_ids);
    ovsdb_idl_add_column(ovs_idl, &ovsrec_interface_col_link_state);
    ovsdb_idl_add_table(ovs_idl, &ovsrec_table_ct_zone);
    ovsdb_idl_add_column(ovs_idl, &ovsrec_ct_zone_col_limit);

    chassis_register_ovs_idl(ovs_idl);
    encaps_register_ovs_idl(ovs_idl);
    binding_register_ovs_idl(ovs_idl);
    bfd_register_ovs_idl(ovs_idl);
    physical_register_ovs_idl(ovs_idl);
    vif_plug_register_ovs_idl(ovs_idl);
    ovsdb_idl_track_add_column(ovs_idl, &ovsrec_open_vswitch_col_external_ids);
    ovsdb_idl_track_add_column(ovs_idl, &ovsrec_interface_col_name);
    ovsdb_idl_track_add_column(ovs_idl, &ovsrec_interface_col_bfd);
    ovsdb_idl_track_add_column(ovs_idl, &ovsrec_interface_col_bfd_status);
    ovsdb_idl_track_add_column(ovs_idl, &ovsrec_interface_col_mtu);
    ovsdb_idl_track_add_column(ovs_idl, &ovsrec_interface_col_type);
    ovsdb_idl_track_add_column(ovs_idl, &ovsrec_interface_col_options);
    ovsdb_idl_track_add_column(ovs_idl, &ovsrec_interface_col_ofport);
    ovsdb_idl_track_add_column(ovs_idl, &ovsrec_interface_col_external_ids);
    ovsdb_idl_track_add_column(ovs_idl, &ovsrec_interface_col_link_speed);
    ovsdb_idl_track_add_column(ovs_idl, &ovsrec_port_col_name);
    ovsdb_idl_track_add_column(ovs_idl, &ovsrec_port_col_interfaces);
    ovsdb_idl_track_add_column(ovs_idl, &ovsrec_port_col_external_ids);
    ovsdb_idl_track_add_column(ovs_idl,
                               &ovsrec_flow_sample_collector_set_col_bridge);
    ovsdb_idl_track_add_column(ovs_idl,
                               &ovsrec_flow_sample_collector_set_col_id);
    mirror_register_ovs_idl(ovs_idl);
    /* XXX: There is a potential bug in CT zone I-P node,
     * the fact that we have to call recompute for the change of
     * OVS.bridge.external_ids be reflected. Currently, we don't
     * track that column which should be addressed in the future. */
}

#define SB_NODES \
    SB_NODE(sb_global) \
    SB_NODE(chassis) \
    SB_NODE(ha_chassis_group) \
    SB_NODE(encap) \
    SB_NODE(address_set) \
    SB_NODE(port_group) \
    SB_NODE(multicast_group) \
    SB_NODE(datapath_binding) \
    SB_NODE(logical_dp_group) \
    SB_NODE(port_binding) \
    SB_NODE(mac_binding) \
    SB_NODE(logical_flow) \
    SB_NODE(dhcp_options) \
    SB_NODE(dhcpv6_options) \
    SB_NODE(dns) \
    SB_NODE(load_balancer) \
    SB_NODE(fdb) \
    SB_NODE(meter) \
    SB_NODE(static_mac_binding) \
    SB_NODE(chassis_template_var) \
    SB_NODE(acl_id) \
    SB_NODE(advertised_route) \
    SB_NODE(learned_route) \
    SB_NODE(advertised_mac_binding)

enum sb_engine_node {
#define SB_NODE(NAME) SB_##NAME,
    SB_NODES
#undef SB_NODE
};

#define SB_NODE(NAME) ENGINE_FUNC_SB(NAME);
    SB_NODES
#undef SB_NODE

#define OVS_NODES \
    OVS_NODE(open_vswitch) \
    OVS_NODE(bridge) \
    OVS_NODE(port) \
    OVS_NODE(interface) \
    OVS_NODE(qos) \
    OVS_NODE(queue) \
    OVS_NODE(flow_sample_collector_set)

enum ovs_engine_node {
#define OVS_NODE(NAME) OVS_##NAME,
    OVS_NODES
#undef OVS_NODE
};

#define OVS_NODE(NAME) ENGINE_FUNC_OVS(NAME);
    OVS_NODES
#undef OVS_NODE

struct ed_type_ofctrl_is_connected {
    bool connected;
};

static void *
en_ofctrl_is_connected_init(struct engine_node *node OVS_UNUSED,
                            struct engine_arg *arg OVS_UNUSED)
{
    struct ed_type_ofctrl_is_connected *data = xzalloc(sizeof *data);
    return data;
}

static void
en_ofctrl_is_connected_cleanup(void *data OVS_UNUSED)
{
}

static enum engine_node_state
en_ofctrl_is_connected_run(struct engine_node *node OVS_UNUSED, void *data)
{
    struct controller_engine_ctx *ctrl_ctx = engine_get_context()->client_ctx;
    struct ed_type_ofctrl_is_connected *of_data = data;
    if (of_data->connected != ofctrl_is_connected()) {
        of_data->connected = !of_data->connected;

        /* Flush ofctrl seqno requests when the ofctrl connection goes down. */
        if (!of_data->connected) {
            ofctrl_seqno_flush();
            if_status_mgr_clear(ctrl_ctx->if_mgr);
        }
        return EN_UPDATED;
    }
    return EN_UNCHANGED;
}

struct ed_type_if_status_mgr {
    const struct if_status_mgr *manager;
    const struct ovsrec_interface_table *iface_table;
};

static void *
en_if_status_mgr_init(struct engine_node *node OVS_UNUSED,
                      struct engine_arg *arg OVS_UNUSED)
{
    struct ed_type_if_status_mgr *data = xzalloc(sizeof *data);
    return data;
}

static void
en_if_status_mgr_cleanup(void *data OVS_UNUSED)
{
}

static enum engine_node_state
en_if_status_mgr_run(struct engine_node *node, void *data_)
{
    enum engine_node_state state = EN_UNCHANGED;
    struct ed_type_if_status_mgr *data = data_;
    struct controller_engine_ctx *ctrl_ctx = engine_get_context()->client_ctx;
    data->manager = ctrl_ctx->if_mgr;
    data->iface_table = EN_OVSDB_GET(engine_get_input("OVS_interface", node));

    const struct ovsrec_interface *iface;
    OVSREC_INTERFACE_TABLE_FOR_EACH (iface, data->iface_table) {
        if (if_status_mgr_iface_update(data->manager, iface)) {
            state = EN_UPDATED;
        }
    }
    return state;
}

static enum engine_input_handler_result
if_status_mgr_ovs_interface_handler(struct engine_node *node OVS_UNUSED,
                                    void *data)
{
    struct ed_type_if_status_mgr *data_ = data;
    enum engine_input_handler_result result = EN_HANDLED_UNCHANGED;

    const struct ovsrec_interface *iface;
    OVSREC_INTERFACE_TABLE_FOR_EACH_TRACKED (iface, data_->iface_table) {
        if (if_status_mgr_iface_update(data_->manager, iface)) {
            result = EN_HANDLED_UPDATED;
        }
    }
    return result;
}

/* This engine node is to wrap the OVS_interface input and maintain a copy of
 * the old version of data for the column external_ids.
 *
 * There are some special considerations of this engine node:
 * 1. It has a single input OVS_interface, and it transparently passes the
 *    input changes as its own output data to its dependants. So there is no
 *    processing to OVS_interface changes but simply mark the node status as
 *    UPDATED (and so the run() and the change handler is the same).
 * 2. The iface_table_external_ids_old is computed/updated in the member
 *    clear_tracked_data(), because that is when the last round of processing
 *    has completed but the new IDL data is yet to refresh, so we replace the
 *    old data with the current data. */
struct ed_type_ovs_interface_shadow {
    const struct ovsrec_interface_table *iface_table;
    struct shash iface_table_external_ids_old;
};

static void *
en_ovs_interface_shadow_init(struct engine_node *node OVS_UNUSED,
                             struct engine_arg *arg OVS_UNUSED)
{
    struct ed_type_ovs_interface_shadow *data = xzalloc(sizeof *data);
    data->iface_table = NULL;
    shash_init(&data->iface_table_external_ids_old);

    return data;
}

static void
iface_table_external_ids_old_destroy(struct shash *table_ext_ids)
{
    struct shash_node *node;
    SHASH_FOR_EACH (node, table_ext_ids) {
        struct smap *ext_ids = node->data;
        smap_destroy(ext_ids);
    }
    shash_destroy_free_data(table_ext_ids);
}

static void
en_ovs_interface_shadow_cleanup(void *data_)
{
    struct ed_type_ovs_interface_shadow *data = data_;
    iface_table_external_ids_old_destroy(&data->iface_table_external_ids_old);
}

static void
en_ovs_interface_shadow_clear_tracked_data(void *data_)
{
    struct ed_type_ovs_interface_shadow *data = data_;
    iface_table_external_ids_old_destroy(&data->iface_table_external_ids_old);
    shash_init(&data->iface_table_external_ids_old);

    if (!data->iface_table) {
        return;
    }

    const struct ovsrec_interface *iface_rec;
    OVSREC_INTERFACE_TABLE_FOR_EACH (iface_rec, data->iface_table) {
        struct smap *external_ids = xmalloc(sizeof *external_ids);
        smap_clone(external_ids, &iface_rec->external_ids);
        shash_add(&data->iface_table_external_ids_old, iface_rec->name,
                  external_ids);
    }
}

static enum engine_node_state
en_ovs_interface_shadow_run(struct engine_node *node, void *data_)
{
    struct ed_type_ovs_interface_shadow *data = data_;
    const struct ovsrec_interface_table *iface_table =
        EN_OVSDB_GET(engine_get_input("OVS_interface", node));
    data->iface_table = iface_table;
    return EN_UPDATED;
}

static enum engine_input_handler_result
ovs_interface_shadow_ovs_interface_handler(struct engine_node *node,
                                           void *data_)
{
    enum engine_node_state state = en_ovs_interface_shadow_run(node, data_);
    if (state == EN_UPDATED) {
        return EN_HANDLED_UPDATED;
    } else {
        return EN_HANDLED_UNCHANGED;
    }
}

struct ed_type_activated_ports {
    struct ovs_list *activated_ports;
};

static void *
en_activated_ports_init(struct engine_node *node OVS_UNUSED,
                        struct engine_arg *arg OVS_UNUSED)
{
    struct ed_type_activated_ports *data = xzalloc(sizeof *data);
    data->activated_ports = NULL;
    return data;
}

static void
en_activated_ports_cleanup(void *data_)
{
    struct ed_type_activated_ports *data = data_;
    if (!data->activated_ports) {
        return;
    }

    struct activated_port *pp;
    LIST_FOR_EACH_POP (pp, list, data->activated_ports) {
        free(pp);
    }
    free(data->activated_ports);
    data->activated_ports = NULL;
}

static void
en_activated_ports_clear_tracked_data(void *data)
{
    en_activated_ports_cleanup(data);
}

static enum engine_node_state
en_activated_ports_run(struct engine_node *node OVS_UNUSED, void *data_)
{
    struct ed_type_activated_ports *data = data_;
    enum engine_node_state state = EN_UNCHANGED;
    data->activated_ports = get_ports_to_activate_in_engine();
    if (data->activated_ports) {
        state = EN_UPDATED;
    }
    return state;
}

struct ed_type_postponed_ports {
    struct sset *postponed_ports;
};

static void *
en_postponed_ports_init(struct engine_node *node OVS_UNUSED,
                        struct engine_arg *arg OVS_UNUSED)
{
    struct ed_type_postponed_ports *data = xzalloc(sizeof *data);
    data->postponed_ports = get_postponed_ports();
    return data;
}

static void
en_postponed_ports_cleanup(void *data_)
{
    struct ed_type_postponed_ports *data = data_;
    if (!data->postponed_ports) {
        return;
    }
    data->postponed_ports = NULL;
}

static enum engine_node_state
en_postponed_ports_run(struct engine_node *node OVS_UNUSED, void *data_)
{
    struct ed_type_postponed_ports *data = data_;
    enum engine_node_state state = EN_UNCHANGED;
    data->postponed_ports = get_postponed_ports();
    if (!sset_is_empty(data->postponed_ports)) {
        state = EN_UPDATED;
    }
    return state;
}

struct ed_type_runtime_data {
    /* Contains "struct local_datapath" nodes. */
    struct hmap local_datapaths;

    struct local_binding_data lbinding_data;

    /* Contains the name of each logical port resident on the local
     * hypervisor.  These logical ports include the VIFs (and their child
     * logical ports, if any) that belong to VMs running on the hypervisor,
     * l2gateway ports for which options:l2gateway-chassis designates the
     * local hypervisor, and localnet ports.
     * The value is mapped to enum binding_local_lport_status. */
    struct simap local_lports;

    /* Port bindings that are relevant to the local chassis (VIFs bound
     * localy, patch ports).
     */
    struct related_lports related_lports;
    struct sset active_tunnels;

    /* runtime data engine private data. */
    struct hmap qos_map;
    struct smap local_iface_ids;

    /* Tracked data. See below for more details and comments. */
    bool tracked;
    bool local_lports_changed;
    bool localnet_learn_fdb;
    bool localnet_learn_fdb_changed;
    struct hmap tracked_dp_bindings;

    struct shash local_active_ports_ipv6_pd;
    struct shash local_active_ports_ras;

    struct sset *postponed_ports;
};

/* struct ed_type_runtime_data has the below members for tracking the
 * changes done to the runtime_data engine by the runtime_data engine
 * handlers. Since this engine is an input to the flow_output engine,
 * the flow output runtime data handler will make use of this tracked data.
 *
 *  ------------------------------------------------------------------------
 * |                      | This is a hmap of                               |
 * |                      | 'struct tracked_datapath' defined in            |
 * |                      | ldata.h. Runtime data handlers for OVS          |
 * |                      | Interface and Port Binding changes store the    |
 * | @tracked_dp_bindings | changed datapaths (datapaths added/removed from |
 * |                      | local_datapaths) and changed port bindings      |
 * |                      | (added/updated/deleted in 'lbinding_data').     |
 * |                      | So any changes to the runtime data -            |
 * |                      | local_datapaths and lbinding_data is captured   |
 * |                      | here.                                           |
 *  ------------------------------------------------------------------------
 * |                      | This is a bool which represents if the runtime  |
 * |                      | data 'local_lports' changed by the runtime data |
 * |                      | handlers for OVS Interface and Port Binding     |
 * |                      | changes. If 'local_lports' is updated and also  |
 * |                      | results in any port binding updates, it is      |
 * |@local_lports_changed | captured in the @tracked_dp_bindings. So there  |
 * |                      | is no need to capture the changes in the        |
 * |                      | local_lports. If @local_lports_changed is true  |
 * |                      | but without anydata in the @tracked_dp_bindings,|
 * |                      | it means we needto only update the SB monitor   |
 * |                      | clauses and there isno need for any flow        |
 * |                      | (re)computations.                               |
 *  ------------------------------------------------------------------------
 * |                      | This represents if the data was tracked or not  |
 * |                      | by the runtime data handlers during the engine  |
 * |   @tracked           | run. If the runtime data recompute is           |
 * |                      | triggered, it means there is no tracked data.   |
 *  ------------------------------------------------------------------------
 *
 *
 * The changes to the following runtime_data variables are not tracked.
 *
 *  ---------------------------------------------------------------------
 * | local_datapaths  | The changes to these runtime data is captured in |
 * | lbinding_data   | the @tracked_dp_bindings indirectly and hence it  |
 * | local_lport_ids  | is not tracked explicitly.                       |
 *  ---------------------------------------------------------------------
 * | local_iface_ids  | This is used internally within the runtime data  |
 * | qos_map          | engine (used only in binding.c) and hence there  |
 * |                  | there is no need to track.                       |
 *  ---------------------------------------------------------------------
 * |                  | Active tunnels is built in the                   |
 * |                  | bfd_calculate_active_tunnels() for the tunnel    |
 * |                  | OVS interfaces. Any changes to non VIF OVS       |
 * |                  | interfaces results in triggering the full        |
 * | active_tunnels   | recompute of runtime data engine and hence there |
 * |                  | the tracked data doesn't track it. When we       |
 * |                  | support handling changes to non VIF OVS          |
 * |                  | interfaces we need to track the changes to the   |
 * |                  | active tunnels.                                  |
 *  ---------------------------------------------------------------------
 *
 */

static void
en_runtime_data_clear_tracked_data(void *data_)
{
    struct ed_type_runtime_data *data = data_;

    tracked_datapaths_destroy(&data->tracked_dp_bindings);
    hmap_init(&data->tracked_dp_bindings);
    data->local_lports_changed = false;
    data->tracked = false;
}

static void *
en_runtime_data_init(struct engine_node *node OVS_UNUSED,
                     struct engine_arg *arg OVS_UNUSED)
{
    struct ed_type_runtime_data *data = xzalloc(sizeof *data);

    hmap_init(&data->local_datapaths);
    simap_init(&data->local_lports);
    related_lports_init(&data->related_lports);
    sset_init(&data->active_tunnels);
    hmap_init(&data->qos_map);
    smap_init(&data->local_iface_ids);
    local_binding_data_init(&data->lbinding_data);
    shash_init(&data->local_active_ports_ipv6_pd);
    shash_init(&data->local_active_ports_ras);

    /* Init the tracked data. */
    hmap_init(&data->tracked_dp_bindings);

    return data;
}

static void
en_runtime_data_cleanup(void *data)
{
    struct ed_type_runtime_data *rt_data = data;

    simap_destroy(&rt_data->local_lports);
    related_lports_destroy(&rt_data->related_lports);
    sset_destroy(&rt_data->active_tunnels);
    destroy_qos_map(&rt_data->qos_map);
    smap_destroy(&rt_data->local_iface_ids);
    local_datapaths_destroy(&rt_data->local_datapaths);
    shash_destroy(&rt_data->local_active_ports_ipv6_pd);
    shash_destroy(&rt_data->local_active_ports_ras);
    local_binding_data_destroy(&rt_data->lbinding_data);
}

static void
init_binding_ctx(struct engine_node *node,
                 struct ed_type_runtime_data *rt_data,
                 struct binding_ctx_in *b_ctx_in,
                 struct binding_ctx_out *b_ctx_out)
{
    const struct ovsrec_open_vswitch_table *ovs_table =
        EN_OVSDB_GET(engine_get_input("OVS_open_vswitch", node));
    const struct ovsrec_bridge_table *bridge_table =
        EN_OVSDB_GET(engine_get_input("OVS_bridge", node));
    const char *chassis_id = get_ovs_chassis_id(ovs_table);
    const struct ovsrec_bridge *br_int = get_br_int(bridge_table, ovs_table);

    ovs_assert(br_int && chassis_id);

    struct ovsdb_idl_index *sbrec_chassis_by_name =
        engine_ovsdb_node_get_index(
                engine_get_input("SB_chassis", node),
                "name");

    const struct sbrec_chassis *chassis
        = chassis_lookup_by_name(sbrec_chassis_by_name, chassis_id);
    ovs_assert(chassis);

    struct ed_type_ovs_interface_shadow *iface_shadow =
        engine_get_input_data("ovs_interface_shadow", node);

    const struct ovsrec_qos_table *qos_table =
        EN_OVSDB_GET(engine_get_input("OVS_qos", node));

    const struct sbrec_port_binding_table *pb_table =
        EN_OVSDB_GET(engine_get_input("SB_port_binding", node));

    struct ovsdb_idl_index *sbrec_datapath_binding_by_key =
        engine_ovsdb_node_get_index(
                engine_get_input("SB_datapath_binding", node),
                "key");

    struct ovsdb_idl_index *sbrec_port_binding_by_name =
        engine_ovsdb_node_get_index(
                engine_get_input("SB_port_binding", node),
                "name");

    struct ovsdb_idl_index *sbrec_port_binding_by_datapath =
        engine_ovsdb_node_get_index(
                engine_get_input("SB_port_binding", node),
                "datapath");

    struct ovsdb_idl_index *ovsrec_port_by_qos =
        engine_ovsdb_node_get_index(
                engine_get_input("OVS_port", node), "qos");

    struct ovsdb_idl_index *ovsrec_queue_by_external_ids =
        engine_ovsdb_node_get_index(
                engine_get_input("OVS_queue", node), "external_ids");

    struct controller_engine_ctx *ctrl_ctx = engine_get_context()->client_ctx;

    b_ctx_in->ovnsb_idl_txn = engine_get_context()->ovnsb_idl_txn;
    b_ctx_in->ovs_idl_txn = engine_get_context()->ovs_idl_txn;
    b_ctx_in->sbrec_datapath_binding_by_key = sbrec_datapath_binding_by_key;
    b_ctx_in->sbrec_port_binding_by_datapath = sbrec_port_binding_by_datapath;
    b_ctx_in->sbrec_port_binding_by_name = sbrec_port_binding_by_name;
    b_ctx_in->ovsrec_port_by_qos = ovsrec_port_by_qos;
    b_ctx_in->ovsrec_queue_by_external_ids = ovsrec_queue_by_external_ids;
    b_ctx_in->iface_table = iface_shadow->iface_table;
    b_ctx_in->iface_table_external_ids_old =
        &iface_shadow->iface_table_external_ids_old;
    b_ctx_in->qos_table = qos_table;
    b_ctx_in->port_binding_table = pb_table;
    b_ctx_in->br_int = br_int;
    b_ctx_in->chassis_rec = chassis;
    b_ctx_in->active_tunnels = &rt_data->active_tunnels;
    b_ctx_in->bridge_table = bridge_table;
    b_ctx_in->ovs_table = ovs_table;

    b_ctx_out->local_datapaths = &rt_data->local_datapaths;
    b_ctx_out->local_active_ports_ipv6_pd =
        &rt_data->local_active_ports_ipv6_pd;
    b_ctx_out->local_active_ports_ras =
        &rt_data->local_active_ports_ras;
    b_ctx_out->local_lports = &rt_data->local_lports;
    b_ctx_out->local_lports_changed = false;
    b_ctx_out->related_lports = &rt_data->related_lports;
    b_ctx_out->related_lports_changed = false;
    b_ctx_out->non_vif_ports_changed = false;
    b_ctx_out->qos_map = &rt_data->qos_map;
    b_ctx_out->lbinding_data = &rt_data->lbinding_data;
    b_ctx_out->local_iface_ids = &rt_data->local_iface_ids;
    b_ctx_out->postponed_ports = rt_data->postponed_ports;
    b_ctx_out->tracked_dp_bindings = NULL;
    b_ctx_out->if_mgr = ctrl_ctx->if_mgr;
    b_ctx_out->localnet_learn_fdb = rt_data->localnet_learn_fdb;
    b_ctx_out->localnet_learn_fdb_changed = false;
}

static enum engine_node_state
en_runtime_data_run(struct engine_node *node, void *data)
{
    struct ed_type_runtime_data *rt_data = data;
    struct hmap *local_datapaths = &rt_data->local_datapaths;
    struct shash *local_active_ipv6_pd = &rt_data->local_active_ports_ipv6_pd;
    struct shash *local_active_ras = &rt_data->local_active_ports_ras;
    struct simap *local_lports = &rt_data->local_lports;
    struct sset *active_tunnels = &rt_data->active_tunnels;

    static bool first_run = true;
    if (first_run) {
        /* don't cleanup since there is no data yet */
        first_run = false;
    } else {
        local_datapaths_destroy(local_datapaths);
        shash_clear(local_active_ipv6_pd);
        shash_clear(local_active_ras);
        local_binding_data_destroy(&rt_data->lbinding_data);
        simap_destroy(local_lports);
        related_lports_destroy(&rt_data->related_lports);
        sset_destroy(active_tunnels);
        destroy_qos_map(&rt_data->qos_map);
        smap_destroy(&rt_data->local_iface_ids);
        hmap_init(local_datapaths);
        simap_init(local_lports);
        related_lports_init(&rt_data->related_lports);
        sset_init(active_tunnels);
        hmap_init(&rt_data->qos_map);
        smap_init(&rt_data->local_iface_ids);
        local_binding_data_init(&rt_data->lbinding_data);
    }

    struct ed_type_postponed_ports *pp_data =
        engine_get_input_data("postponed_ports", node);
    rt_data->postponed_ports = pp_data->postponed_ports;

    struct binding_ctx_in b_ctx_in;
    struct binding_ctx_out b_ctx_out;
    init_binding_ctx(node, rt_data, &b_ctx_in, &b_ctx_out);

    struct ed_type_ofctrl_is_connected *ed_ofctrl_is_connected =
        engine_get_input_data("ofctrl_is_connected", node);
    if (ed_ofctrl_is_connected->connected) {
        /* Calculate the active tunnels only if have an an active
         * OpenFlow connection to br-int.
         * If we don't have a connection to br-int, it could mean
         * ovs-vswitchd is down for some reason and the BFD status
         * in the Interface rows could be stale. So its better to
         * consider 'active_tunnels' set to be empty if it's not
         * connected. */
        bfd_calculate_active_tunnels(b_ctx_in.br_int, active_tunnels);
    }

    binding_run(&b_ctx_in, &b_ctx_out);
    rt_data->localnet_learn_fdb = b_ctx_out.localnet_learn_fdb;

    return EN_UPDATED;
}

struct ed_type_sb_ro {
    bool sb_readonly;
};

static void *
en_sb_ro_init(struct engine_node *node OVS_UNUSED,
              struct engine_arg *arg OVS_UNUSED)
{
    struct ed_type_sb_ro *data = xzalloc(sizeof *data);
    return data;
}

static enum engine_node_state
en_sb_ro_run(struct engine_node *node OVS_UNUSED, void *data)
{
    struct ed_type_sb_ro *sb_ro_data = data;
    bool sb_readonly = !engine_get_context()->ovnsb_idl_txn;
    if (sb_ro_data->sb_readonly != sb_readonly) {
        sb_ro_data->sb_readonly = sb_readonly;
        if (!sb_ro_data->sb_readonly) {
            return EN_UPDATED;
        }
    }
    return EN_UNCHANGED;
}

static void
en_sb_ro_cleanup(void *data OVS_UNUSED)
{
}

static enum engine_input_handler_result
runtime_data_sb_ro_handler(struct engine_node *node, void *data)
{
    const struct sbrec_chassis *chassis = NULL;
    enum engine_input_handler_result result = EN_HANDLED_UNCHANGED;

    struct ovsrec_open_vswitch_table *ovs_table =
        (struct ovsrec_open_vswitch_table *)EN_OVSDB_GET(
            engine_get_input("OVS_open_vswitch", node));

    const char *chassis_id = get_ovs_chassis_id(ovs_table);

    struct ovsdb_idl_index *sbrec_chassis_by_name =
        engine_ovsdb_node_get_index(
                engine_get_input("SB_chassis", node),
                "name");
    const struct sbrec_port_binding_table *pb_table =
        EN_OVSDB_GET(engine_get_input("SB_port_binding", node));

    if (chassis_id) {
        chassis = chassis_lookup_by_name(sbrec_chassis_by_name, chassis_id);
    }
    if (chassis) {
        struct ed_type_runtime_data *rt_data = data;
        struct controller_engine_ctx *ctrl_ctx =
            engine_get_context()->client_ctx;

        if (if_status_handle_claims(ctrl_ctx->if_mgr, &rt_data->lbinding_data,
                                    chassis, &rt_data->tracked_dp_bindings,
                                    pb_table)) {
            result = EN_HANDLED_UPDATED;
            rt_data->tracked = true;
        }
    }
    return result;
}

static enum engine_input_handler_result
runtime_data_ovs_interface_shadow_handler(struct engine_node *node, void *data)
{
    struct ed_type_runtime_data *rt_data = data;
    struct binding_ctx_in b_ctx_in;
    struct binding_ctx_out b_ctx_out;
    enum engine_input_handler_result result = EN_HANDLED_UNCHANGED;
    init_binding_ctx(node, rt_data, &b_ctx_in, &b_ctx_out);
    rt_data->tracked = true;
    b_ctx_out.tracked_dp_bindings = &rt_data->tracked_dp_bindings;

    if (!binding_handle_ovs_interface_changes(&b_ctx_in, &b_ctx_out)) {
        return EN_UNHANDLED;
    }

    if (b_ctx_out.local_lports_changed) {
        result = EN_HANDLED_UPDATED;
        rt_data->local_lports_changed = b_ctx_out.local_lports_changed;
    }

    return result;
}

static enum engine_input_handler_result
runtime_data_sb_port_binding_handler(struct engine_node *node, void *data)
{
    struct ed_type_runtime_data *rt_data = data;
    struct binding_ctx_in b_ctx_in;
    struct binding_ctx_out b_ctx_out;
    enum engine_input_handler_result result = EN_HANDLED_UNCHANGED;
    init_binding_ctx(node, rt_data, &b_ctx_in, &b_ctx_out);
    if (!b_ctx_in.chassis_rec) {
        return EN_UNHANDLED;
    }

    rt_data->tracked = true;
    b_ctx_out.tracked_dp_bindings = &rt_data->tracked_dp_bindings;

    if (!binding_handle_port_binding_changes(&b_ctx_in, &b_ctx_out)) {
        return EN_UNHANDLED;
    }

    rt_data->local_lports_changed = b_ctx_out.local_lports_changed;
    rt_data->localnet_learn_fdb = b_ctx_out.localnet_learn_fdb;
    rt_data->localnet_learn_fdb_changed = b_ctx_out.localnet_learn_fdb_changed;
    if (b_ctx_out.related_lports_changed ||
            b_ctx_out.non_vif_ports_changed ||
            b_ctx_out.local_lports_changed ||
            b_ctx_out.localnet_learn_fdb_changed ||
            !hmap_is_empty(b_ctx_out.tracked_dp_bindings)) {
        result = EN_HANDLED_UPDATED;
    }

    return result;
}

static enum engine_input_handler_result
runtime_data_sb_datapath_binding_handler(struct engine_node *node OVS_UNUSED,
                                         void *data OVS_UNUSED)
{
    struct sbrec_datapath_binding_table *dp_table =
        (struct sbrec_datapath_binding_table *)EN_OVSDB_GET(
            engine_get_input("SB_datapath_binding", node));
    const struct sbrec_datapath_binding *dp;
    struct ed_type_runtime_data *rt_data = data;

    SBREC_DATAPATH_BINDING_TABLE_FOR_EACH_TRACKED (dp, dp_table) {
        if (sbrec_datapath_binding_is_deleted(dp)) {
            if (get_local_datapath(&rt_data->local_datapaths,
                                   dp->tunnel_key)) {
                return EN_UNHANDLED;
            }

        }

        if (sbrec_datapath_binding_is_updated(
                dp, SBREC_DATAPATH_BINDING_COL_TUNNEL_KEY) &&
            !sbrec_datapath_binding_is_new(dp)) {
            /* If the tunnel key got updated, get_local_datapath will not find
             * the ld. Use get_local_datapath_no_hash which does not
             * rely on the hash.
             */
            if (get_local_datapath_no_hash(&rt_data->local_datapaths,
                                           dp->tunnel_key)) {
                return EN_UNHANDLED;
            }
        }
    }

    return EN_HANDLED_UNCHANGED;
}

static void
local_templates_to_string(const struct smap *local_templates,
                          struct ds *out_data)
{
    const struct smap_node **nodes = smap_sort(local_templates);

    for (size_t i = 0; i < smap_count(local_templates); i++) {
        const struct smap_node *node = nodes[i];

        ds_put_format(out_data, "name: '%s' value: '%s'\n",
                      node->key, node->value);
    }
    free(nodes);
}

struct ed_type_template_vars {
    struct smap local_templates;

    bool change_tracked;
    struct sset new;
    struct sset deleted;
    struct sset updated;
};

static void
template_vars_init(struct ovsdb_idl_index *tv_index_by_chassis,
                   const struct sbrec_chassis *chassis,
                   struct smap *local_templates)
{
    const struct sbrec_chassis_template_var *tv;
    struct sbrec_chassis_template_var *tv_key =
        sbrec_chassis_template_var_index_init_row(tv_index_by_chassis);
    sbrec_chassis_template_var_index_set_chassis(tv_key, chassis->name);

    tv = sbrec_chassis_template_var_index_find(tv_index_by_chassis, tv_key);
    if (tv) {
        smap_destroy(local_templates);
        smap_clone(local_templates, &tv->variables);
    }

    sbrec_chassis_template_var_index_destroy_row(tv_key);
}

static void
template_vars_update(const struct sbrec_chassis_template_var_table *tv_table,
                     const struct sbrec_chassis *chassis,
                     struct smap *local_templates, struct sset *new,
                     struct sset *deleted, struct sset *updated)
{
    const struct sbrec_chassis_template_var *tv;
    struct smap_node *node;
    SBREC_CHASSIS_TEMPLATE_VAR_TABLE_FOR_EACH_TRACKED (tv, tv_table) {
        if (strcmp(tv->chassis, chassis->name)) {
            continue;
        }

        if (sbrec_chassis_template_var_is_deleted(tv)) {
            SMAP_FOR_EACH (node, local_templates) {
                sset_add(deleted, node->key);
            }
        } else if (sbrec_chassis_template_var_is_new(tv)) {
            ovs_assert(smap_count(local_templates) == 0);
            SMAP_FOR_EACH (node, &tv->variables) {
                sset_add(new, node->key);
            }
        } else {
            SMAP_FOR_EACH (node, &tv->variables) {
                struct smap_node *old = smap_get_node(local_templates,
                                                      node->key);
                if (old) {
                    if (strcmp(old->value, node->value)) {
                        sset_add(updated, node->key);
                    }
                } else {
                    sset_add(new, node->key);
                }
            }
            SMAP_FOR_EACH (node, local_templates) {
                sset_add (deleted, node->key);
            }
        }

        smap_destroy(local_templates);
        smap_clone(local_templates, &tv->variables);
        return;
    }
}

static void *
en_template_vars_init(struct engine_node *node OVS_UNUSED,
                      struct engine_arg *arg OVS_UNUSED)
{
    struct ed_type_template_vars *tv_data = xzalloc(sizeof *tv_data);
    smap_init(&tv_data->local_templates);
    tv_data->change_tracked = false;
    sset_init(&tv_data->new);
    sset_init(&tv_data->deleted);
    sset_init(&tv_data->updated);
    return tv_data;
}

static enum engine_node_state
en_template_vars_run(struct engine_node *node, void *data)
{
    struct ed_type_template_vars *tv_data = data;

    const struct ovsrec_open_vswitch_table *ovs_table =
        EN_OVSDB_GET(engine_get_input("OVS_open_vswitch", node));
    struct ovsdb_idl_index *sbrec_chassis_by_name =
        engine_ovsdb_node_get_index(engine_get_input("SB_chassis", node),
                                    "name");
    const struct sbrec_chassis *chassis =
        chassis_lookup_by_name(sbrec_chassis_by_name,
                               get_ovs_chassis_id(ovs_table));
    struct ovsdb_idl_index *sbrec_chassis_template_var_index_by_chassis =
        engine_ovsdb_node_get_index(
            engine_get_input("SB_chassis_template_var", node),
            "chassis");

    smap_clear(&tv_data->local_templates);
    template_vars_init(sbrec_chassis_template_var_index_by_chassis,
                       chassis, &tv_data->local_templates);
    return EN_UPDATED;
}

static enum engine_input_handler_result
template_vars_sb_chassis_template_var_handler(struct engine_node *node,
                                              void *data)
{
    struct ed_type_template_vars *tv_data = data;
    enum engine_input_handler_result result;

    const struct sbrec_chassis_template_var_table *tv_table =
        EN_OVSDB_GET(engine_get_input("SB_chassis_template_var", node));
    const struct ovsrec_open_vswitch_table *ovs_table =
        EN_OVSDB_GET(engine_get_input("OVS_open_vswitch", node));
    struct ovsdb_idl_index *sbrec_chassis_by_name =
        engine_ovsdb_node_get_index(engine_get_input("SB_chassis", node),
                                    "name");
    const struct sbrec_chassis *chassis =
        chassis_lookup_by_name(sbrec_chassis_by_name,
                               get_ovs_chassis_id(ovs_table));

    template_vars_update(tv_table, chassis, &tv_data->local_templates,
                         &tv_data->new, &tv_data->deleted, &tv_data->updated);

    if (!sset_is_empty(&tv_data->new) || !sset_is_empty(&tv_data->deleted) ||
            !sset_is_empty(&tv_data->updated)) {
        result = EN_HANDLED_UPDATED;
    } else {
        result = EN_HANDLED_UNCHANGED;
    }

    tv_data->change_tracked = true;
    return result;
}

static void
en_template_vars_clear_tracked_data(void *data)
{
    struct ed_type_template_vars *tv_data = data;

    sset_clear(&tv_data->new);
    sset_clear(&tv_data->deleted);
    sset_clear(&tv_data->updated);
    tv_data->change_tracked = false;
}

static void
en_template_vars_cleanup(void *data)
{
    struct ed_type_template_vars *tv_data = data;

    smap_destroy(&tv_data->local_templates);
    sset_destroy(&tv_data->new);
    sset_destroy(&tv_data->deleted);
    sset_destroy(&tv_data->updated);
}

struct ed_type_addr_sets {
    struct shash addr_sets;
    bool change_tracked;
    struct sset new;
    struct sset deleted;
    struct shash updated;
};

static void *
en_addr_sets_init(struct engine_node *node OVS_UNUSED,
                  struct engine_arg *arg OVS_UNUSED)
{
    struct ed_type_addr_sets *as = xzalloc(sizeof *as);

    shash_init(&as->addr_sets);
    as->change_tracked = false;
    sset_init(&as->new);
    sset_init(&as->deleted);
    shash_init(&as->updated);
    return as;
}

static void
en_addr_sets_clear_tracked_data(void *data)
{
    struct ed_type_addr_sets *as = data;
    sset_clear(&as->new);
    sset_clear(&as->deleted);
    struct shash_node *node;
    SHASH_FOR_EACH_SAFE (node, &as->updated) {
        struct addr_set_diff *asd = node->data;
        expr_constant_set_destroy(asd->added);
        free(asd->added);
        expr_constant_set_destroy(asd->deleted);
        free(asd->deleted);
    }
    shash_clear_free_data(&as->updated);
    as->change_tracked = false;
}

static void
en_addr_sets_cleanup(void *data)
{
    en_addr_sets_clear_tracked_data(data);

    struct ed_type_addr_sets *as = data;
    expr_const_sets_destroy(&as->addr_sets);
    shash_destroy(&as->addr_sets);
    sset_destroy(&as->new);
    sset_destroy(&as->deleted);
    shash_destroy(&as->updated);
}

/* Iterate address sets in the southbound database.  Create and update the
 * corresponding symtab entries as necessary. */
static void
addr_sets_init(const struct sbrec_address_set_table *address_set_table,
               struct shash *addr_sets)
{
    const struct sbrec_address_set *as;
    SBREC_ADDRESS_SET_TABLE_FOR_EACH (as, address_set_table) {
        expr_const_sets_add_integers(addr_sets, as->name,
                                     (const char *const *) as->addresses,
                                     as->n_addresses);
    }
}

static void
addr_sets_update(const struct sbrec_address_set_table *address_set_table,
                 struct shash *addr_sets, struct sset *added,
                 struct sset *deleted, struct shash *updated)
{
    const struct sbrec_address_set *as;
    SBREC_ADDRESS_SET_TABLE_FOR_EACH_TRACKED (as, address_set_table) {
        if (sbrec_address_set_is_deleted(as)) {
            expr_const_sets_remove(addr_sets, as->name);
            sset_add(deleted, as->name);
        }
    }

    SBREC_ADDRESS_SET_TABLE_FOR_EACH_TRACKED (as, address_set_table) {
        if (!sbrec_address_set_is_deleted(as)) {
            struct expr_constant_set *cs_old = shash_find_data(addr_sets,
                                                               as->name);
            if (!cs_old) {
                sset_add(added, as->name);
                expr_const_sets_add_integers(addr_sets, as->name,
                    (const char *const *) as->addresses, as->n_addresses);
            } else {
                /* Find out the diff for the updated address set. */
                struct expr_constant_set *cs_new =
                    expr_constant_set_create_integers(
                        (const char *const *) as->addresses, as->n_addresses);
                struct addr_set_diff *as_diff = xmalloc(sizeof *as_diff);
                expr_constant_set_integers_diff(cs_old, cs_new,
                                                &as_diff->added,
                                                &as_diff->deleted);
                if (!as_diff->added && !as_diff->deleted) {
                    /* The address set may have been updated, but the change
                     * doesn't has any impact to the generated constant-set.
                     * For example, ff::01 is changed to ff::00:01. */
                    free(as_diff);
                    expr_constant_set_destroy(cs_new);
                    free(cs_new);
                    continue;
                }
                shash_add(updated, as->name, as_diff);
                expr_const_sets_add(addr_sets, as->name, cs_new);
            }
        }
    }
}

static enum engine_node_state
en_addr_sets_run(struct engine_node *node, void *data)
{
    struct ed_type_addr_sets *as = data;

    expr_const_sets_destroy(&as->addr_sets);

    struct sbrec_address_set_table *as_table =
        (struct sbrec_address_set_table *)EN_OVSDB_GET(
            engine_get_input("SB_address_set", node));

    addr_sets_init(as_table, &as->addr_sets);

    as->change_tracked = false;
    return EN_UPDATED;
}

static enum engine_input_handler_result
addr_sets_sb_address_set_handler(struct engine_node *node, void *data)
{
    struct ed_type_addr_sets *as = data;
    enum engine_input_handler_result result;

    struct sbrec_address_set_table *as_table =
        (struct sbrec_address_set_table *)EN_OVSDB_GET(
            engine_get_input("SB_address_set", node));

    addr_sets_update(as_table, &as->addr_sets, &as->new,
                     &as->deleted, &as->updated);

    if (!sset_is_empty(&as->new) || !sset_is_empty(&as->deleted) ||
            !shash_is_empty(&as->updated)) {
        result = EN_HANDLED_UPDATED;
    } else {
        result = EN_HANDLED_UNCHANGED;
    }

    as->change_tracked = true;
    return result;
}

struct ed_type_port_groups{
    /* A copy of SB port_groups, each converted as a sset for efficient lport
     * lookup. */
    struct shash port_group_ssets;

    /* Const sets containing local lports, used for expr parsing. */
    struct shash port_groups_cs_local;

    bool change_tracked;
    struct sset new;
    struct sset deleted;
    struct sset updated;
};

static void
port_group_ssets_add_or_update(struct shash *port_group_ssets,
                               const struct sbrec_port_group *pg)
{
    struct sset *lports = shash_find_data(port_group_ssets, pg->name);
    if (lports) {
        sset_clear(lports);
    } else {
        lports = xzalloc(sizeof *lports);
        sset_init(lports);
        shash_add(port_group_ssets, pg->name, lports);
    }

    for (size_t i = 0; i < pg->n_ports; i++) {
        sset_add(lports, pg->ports[i]);
    }
}

static void
port_group_ssets_delete(struct shash *port_group_ssets,
                        const char *pg_name)
{
    struct shash_node *node = shash_find(port_group_ssets, pg_name);
    if (node) {
        struct sset *lports = node->data;
        shash_delete(port_group_ssets, node);
        sset_destroy(lports);
        free(lports);
    }
}

/* Delete and free all ssets in port_group_ssets, but not
 * destroying the shash itself. */
static void
port_group_ssets_clear(struct shash *port_group_ssets)
{
    struct shash_node *node;
    SHASH_FOR_EACH_SAFE (node, port_group_ssets) {
        struct sset *lports = node->data;
        shash_delete(port_group_ssets, node);
        sset_destroy(lports);
        free(lports);
    }
}

static void *
en_port_groups_init(struct engine_node *node OVS_UNUSED,
                    struct engine_arg *arg OVS_UNUSED)
{
    struct ed_type_port_groups *pg = xzalloc(sizeof *pg);

    shash_init(&pg->port_group_ssets);
    shash_init(&pg->port_groups_cs_local);
    pg->change_tracked = false;
    sset_init(&pg->new);
    sset_init(&pg->deleted);
    sset_init(&pg->updated);
    return pg;
}

static void
en_port_groups_cleanup(void *data)
{
    struct ed_type_port_groups *pg = data;

    expr_const_sets_destroy(&pg->port_groups_cs_local);
    shash_destroy(&pg->port_groups_cs_local);

    port_group_ssets_clear(&pg->port_group_ssets);
    shash_destroy(&pg->port_group_ssets);

    sset_destroy(&pg->new);
    sset_destroy(&pg->deleted);
    sset_destroy(&pg->updated);
}

static void
port_groups_init(const struct sbrec_port_group_table *port_group_table,
                 const struct sset *local_lports,
                 struct shash *port_group_ssets,
                 struct shash *port_groups_cs_local)
{
    const struct sbrec_port_group *pg;
    SBREC_PORT_GROUP_TABLE_FOR_EACH (pg, port_group_table) {
        port_group_ssets_add_or_update(port_group_ssets, pg);
        expr_const_sets_add_strings(port_groups_cs_local, pg->name,
                                    (const char *const *) pg->ports,
                                    pg->n_ports, local_lports);
    }
}

static void
port_groups_update(const struct sbrec_port_group_table *port_group_table,
                   const struct sset *local_lports,
                   struct shash *port_group_ssets,
                   struct shash *port_groups_cs_local,
                   struct sset *new, struct sset *deleted,
                   struct sset *updated)
{
    const struct sbrec_port_group *pg;
    SBREC_PORT_GROUP_TABLE_FOR_EACH_TRACKED (pg, port_group_table) {
        if (sbrec_port_group_is_deleted(pg)) {
            expr_const_sets_remove(port_groups_cs_local, pg->name);
            port_group_ssets_delete(port_group_ssets, pg->name);
            sset_add(deleted, pg->name);
        }
    }

    SBREC_PORT_GROUP_TABLE_FOR_EACH_TRACKED (pg, port_group_table) {
        if (!sbrec_port_group_is_deleted(pg)) {
            port_group_ssets_add_or_update(port_group_ssets, pg);
            expr_const_sets_add_strings(port_groups_cs_local, pg->name,
                                        (const char *const *) pg->ports,
                                        pg->n_ports, local_lports);
            if (sbrec_port_group_is_new(pg)) {
                sset_add(new, pg->name);
            } else {
                sset_add(updated, pg->name);
            }
        }
    }
}

static void
en_port_groups_clear_tracked_data(void *data_)
{
    struct ed_type_port_groups *pg = data_;
    sset_clear(&pg->new);
    sset_clear(&pg->deleted);
    sset_clear(&pg->updated);
    pg->change_tracked = false;
}

static enum engine_node_state
en_port_groups_run(struct engine_node *node, void *data)
{
    struct ed_type_port_groups *pg = data;

    expr_const_sets_destroy(&pg->port_groups_cs_local);
    port_group_ssets_clear(&pg->port_group_ssets);

    const struct sbrec_port_group_table *pg_table =
        EN_OVSDB_GET(engine_get_input("SB_port_group", node));

    struct ed_type_runtime_data *rt_data =
        engine_get_input_data("runtime_data", node);

    port_groups_init(pg_table, &rt_data->related_lports.lport_names,
                     &pg->port_group_ssets, &pg->port_groups_cs_local);

    return EN_UPDATED;
}

static enum engine_input_handler_result
port_groups_sb_port_group_handler(struct engine_node *node, void *data)
{
    struct ed_type_port_groups *pg = data;
    enum engine_input_handler_result result;

    const struct sbrec_port_group_table *pg_table =
        EN_OVSDB_GET(engine_get_input("SB_port_group", node));

    struct ed_type_runtime_data *rt_data =
        engine_get_input_data("runtime_data", node);

    port_groups_update(pg_table, &rt_data->related_lports.lport_names,
                       &pg->port_group_ssets, &pg->port_groups_cs_local,
                       &pg->new, &pg->deleted, &pg->updated);

    if (!sset_is_empty(&pg->new) || !sset_is_empty(&pg->deleted) ||
            !sset_is_empty(&pg->updated)) {
        result = EN_HANDLED_UPDATED;
    } else {
        result = EN_HANDLED_UNCHANGED;
    }

    pg->change_tracked = true;
    return result;
}

static enum engine_input_handler_result
port_groups_runtime_data_handler(struct engine_node *node, void *data)
{
    const struct sbrec_port_group_table *pg_table =
        EN_OVSDB_GET(engine_get_input("SB_port_group", node));
    struct ed_type_port_groups *pg = data;

    struct ed_type_runtime_data *rt_data =
        engine_get_input_data("runtime_data", node);

    enum engine_input_handler_result result;

    if (!rt_data->tracked) {
        return EN_UNHANDLED;
    }

    if (hmap_is_empty(&rt_data->tracked_dp_bindings)) {
        goto out;
    }

    const struct sbrec_port_group *pg_sb;
    SBREC_PORT_GROUP_TABLE_FOR_EACH (pg_sb, pg_table) {
        struct sset *pg_lports = shash_find_data(&pg->port_group_ssets,
                                                 pg_sb->name);
        ovs_assert(pg_lports);

        struct tracked_datapath *tdp;
        bool need_update = false;
        HMAP_FOR_EACH (tdp, node, &rt_data->tracked_dp_bindings) {
            struct shash_node *shash_node;
            SHASH_FOR_EACH (shash_node, &tdp->lports) {
                struct tracked_lport *lport = shash_node->data;
                if (sset_contains(pg_lports, lport->pb->logical_port)) {
                    /* At least one local port-binding change is related to the
                     * port_group, so the port_group_cs_local needs update. */
                    need_update = true;
                    break;
                }
            }
            if (need_update) {
                break;
            }
        }
        if (need_update) {
            expr_const_sets_add_strings(&pg->port_groups_cs_local, pg_sb->name,
                                        (const char *const *) pg_sb->ports,
                                        pg_sb->n_ports,
                                        &rt_data->related_lports.lport_names);
            sset_add(&pg->updated, pg_sb->name);
        }
    }

out:
    if (!sset_is_empty(&pg->new) || !sset_is_empty(&pg->deleted) ||
            !sset_is_empty(&pg->updated)) {
        result = EN_HANDLED_UPDATED;
    } else {
        result = EN_HANDLED_UNCHANGED;
    }
    pg->change_tracked = true;
    return result;
}

static void *
en_ct_zones_init(struct engine_node *node OVS_UNUSED,
                 struct engine_arg *arg OVS_UNUSED)
{
    struct ed_type_ct_zones *data = xzalloc(sizeof *data);

    ct_zone_ctx_init(&data->ctx);

    return data;
}

static void
en_ct_zones_clear_tracked_data(void *data_)
{
    struct ed_type_ct_zones *data = data_;
    data->recomputed = false;
}

static void
en_ct_zones_cleanup(void *data)
{
    struct ed_type_ct_zones *ct_zones_data = data;

    ct_zone_ctx_destroy(&ct_zones_data->ctx);
}

static enum engine_node_state
en_ct_zones_run(struct engine_node *node, void *data)
{
    struct ed_type_ct_zones *ct_zones_data = data;
    struct ed_type_runtime_data *rt_data =
        engine_get_input_data("runtime_data", node);

    const struct ovsrec_open_vswitch_table *ovs_table =
        EN_OVSDB_GET(engine_get_input("OVS_open_vswitch", node));
    const struct ovsrec_bridge_table *bridge_table =
        EN_OVSDB_GET(engine_get_input("OVS_bridge", node));
    const struct sbrec_datapath_binding_table *dp_table =
            EN_OVSDB_GET(engine_get_input("SB_datapath_binding", node));

    const struct ovsrec_bridge *br_int = get_br_int(bridge_table, ovs_table);

    ct_zones_restore(&ct_zones_data->ctx, ovs_table, dp_table, br_int);
    ct_zones_update(&rt_data->local_lports, ovs_table,
                    &rt_data->local_datapaths, &ct_zones_data->ctx);
    ct_zones_limits_sync(&ct_zones_data->ctx, &rt_data->local_datapaths,
                         &rt_data->lbinding_data.lports);

    ct_zones_data->recomputed = true;
    return EN_UPDATED;
}

/* Handles datapath binding changes for the ct_zones engine.
 * Returns false if the datapath is deleted or if the requested snat
 * ct zone doesn't match with the ct_zones data. */
static enum engine_input_handler_result
ct_zones_datapath_binding_handler(struct engine_node *node, void *data)
{
    struct ed_type_ct_zones *ct_zones_data = data;
    const struct sbrec_datapath_binding *dp;
    struct ed_type_runtime_data *rt_data =
        engine_get_input_data("runtime_data", node);
    const struct sbrec_datapath_binding_table *dp_table =
        EN_OVSDB_GET(engine_get_input("SB_datapath_binding", node));

    SBREC_DATAPATH_BINDING_TABLE_FOR_EACH_TRACKED (dp, dp_table) {
        const struct local_datapath *local_dp=
                get_local_datapath(&rt_data->local_datapaths, dp->tunnel_key);
        if (!local_dp) {
            continue;
        }

        if (sbrec_datapath_binding_is_deleted(dp) ||
            sbrec_datapath_binding_is_new(dp)) {
            /* Fall back to full recompute of ct_zones engine. */
            return EN_UNHANDLED;
        }

        if (!ct_zone_handle_dp_update(&ct_zones_data->ctx, local_dp,
                                      &rt_data->lbinding_data.lports)) {
            return EN_UNHANDLED;
        }
    }

    return EN_HANDLED_UNCHANGED;
}

static enum engine_input_handler_result
ct_zones_runtime_data_handler(struct engine_node *node, void *data)
{
    struct ed_type_runtime_data *rt_data =
        engine_get_input_data("runtime_data", node);
    const struct ovsrec_open_vswitch_table *ovs_table =
        EN_OVSDB_GET(engine_get_input("OVS_open_vswitch", node));

    /* There is no tracked data. Fall back to full recompute of ct_zones. */
    if (!rt_data->tracked) {
        return EN_UNHANDLED;
    }

    struct ed_type_ct_zones *ct_zones_data = data;

    struct hmap *tracked_dp_bindings = &rt_data->tracked_dp_bindings;
    int scan_start, min_ct_zone, max_ct_zone;
    struct tracked_datapath *tdp;

    bool updated = false;

    ct_zones_parse_range(ovs_table, &min_ct_zone, &max_ct_zone);
    scan_start = min_ct_zone;

    HMAP_FOR_EACH (tdp, node, tracked_dp_bindings) {
        if (tdp->tracked_type == TRACKED_RESOURCE_NEW) {
            /* A new datapath has been added. Fall back to full recompute. */
            return EN_UNHANDLED;
        }

        struct shash_node *shash_node;
        SHASH_FOR_EACH (shash_node, &tdp->lports) {
            struct tracked_lport *t_lport = shash_node->data;
            if (strcmp(t_lport->pb->type, "")
                && strcmp(t_lport->pb->type, "localport")
                && strcmp(t_lport->pb->type, "l3gateway")
                && strcmp(t_lport->pb->type, "localnet")) {
                /* We allocate zone-id's only to VIF, localport, l3gateway,
                 * and localnet lports. */
                if (sbrec_port_binding_is_updated(t_lport->pb,
                                              SBREC_PORT_BINDING_COL_TYPE)) {
                    updated |= ct_zone_handle_port_update(&ct_zones_data->ctx,
                                               t_lport->pb,
                                               false, &scan_start,
                                               min_ct_zone, max_ct_zone);
                }

                continue;
            }

            bool port_updated =
                    t_lport->tracked_type == TRACKED_RESOURCE_NEW ||
                    t_lport->tracked_type == TRACKED_RESOURCE_UPDATED;
            updated |= ct_zone_handle_port_update(&ct_zones_data->ctx,
                                                  t_lport->pb,
                                                  port_updated, &scan_start,
                                                  min_ct_zone, max_ct_zone);
        }
    }

    return updated ? EN_HANDLED_UPDATED : EN_HANDLED_UNCHANGED;
}

/* The data in the ct_zones node is always valid (i.e., no stale pointers). */
static bool
en_ct_zones_is_valid(struct engine_node *node OVS_UNUSED)
{
    return true;
}

struct ed_type_mff_ovn_geneve {
    enum mf_field_id mff_ovn_geneve;
};

static void *
en_mff_ovn_geneve_init(struct engine_node *node OVS_UNUSED,
                       struct engine_arg *arg OVS_UNUSED)
{
    struct ed_type_mff_ovn_geneve *data = xzalloc(sizeof *data);
    return data;
}

static void
en_mff_ovn_geneve_cleanup(void *data OVS_UNUSED)
{
}

static enum engine_node_state
en_mff_ovn_geneve_run(struct engine_node *node OVS_UNUSED, void *data)
{
    struct ed_type_mff_ovn_geneve *ed_mff_ovn_geneve = data;
    enum mf_field_id mff_ovn_geneve = ofctrl_get_mf_field_id();
    if (ed_mff_ovn_geneve->mff_ovn_geneve != mff_ovn_geneve) {
        ed_mff_ovn_geneve->mff_ovn_geneve = mff_ovn_geneve;
        return EN_UPDATED;
    }
    return EN_UNCHANGED;
}

/* Stores the load balancers that are applied to the datapath 'dp'. */
struct load_balancers_by_dp {
    struct hmap_node node;
    const struct sbrec_datapath_binding *dp;
    struct vector dp_lbs; /* Vector of const struct sbrec_load_balancer *. */
};

static struct load_balancers_by_dp *
load_balancers_by_dp_create(struct hmap *lbs,
                            const struct sbrec_datapath_binding *dp)
{
    struct load_balancers_by_dp *lbs_by_dp = xzalloc(sizeof *lbs_by_dp);

    lbs_by_dp->dp = dp;
    lbs_by_dp->dp_lbs =
        VECTOR_EMPTY_INITIALIZER(const struct sbrec_load_balancer *);
    hmap_insert(lbs, &lbs_by_dp->node, hash_uint64(dp->tunnel_key));
    return lbs_by_dp;
}

static void
load_balancers_by_dp_destroy(struct load_balancers_by_dp *lbs_by_dp)
{
    if (!lbs_by_dp) {
        return;
    }

    vector_destroy(&lbs_by_dp->dp_lbs);
    free(lbs_by_dp);
}

static struct load_balancers_by_dp *
load_balancers_by_dp_find(struct hmap *lbs,
                          const struct sbrec_datapath_binding *dp)
{
    uint32_t hash = hash_uint64(dp->tunnel_key);
    struct load_balancers_by_dp *lbs_by_dp;

    HMAP_FOR_EACH_WITH_HASH (lbs_by_dp, node, hash, lbs) {
        if (lbs_by_dp->dp == dp) {
            return lbs_by_dp;
        }
    }
    return NULL;
}

static void
load_balancers_by_dp_add_one(const struct hmap *local_datapaths,
                             const struct sbrec_datapath_binding *datapath,
                             const struct sbrec_load_balancer *lb,
                             struct hmap *lbs)
{
    struct local_datapath *ldp =
        get_local_datapath(local_datapaths, datapath->tunnel_key);

    if (!ldp) {
        return;
    }

    struct load_balancers_by_dp *lbs_by_dp =
        load_balancers_by_dp_find(lbs, ldp->datapath);
    if (!lbs_by_dp) {
        lbs_by_dp = load_balancers_by_dp_create(lbs, ldp->datapath);
    }

    vector_push(&lbs_by_dp->dp_lbs, &lb);
}

/* Builds and returns a hmap of 'load_balancers_by_dp', one record for each
 * local datapath.
 */
static struct hmap *
load_balancers_by_dp_init(const struct hmap *local_datapaths,
                          const struct sbrec_load_balancer_table *lb_table)
{
    struct hmap *lbs = xmalloc(sizeof *lbs);
    hmap_init(lbs);

    const struct sbrec_load_balancer *lb;
    SBREC_LOAD_BALANCER_TABLE_FOR_EACH (lb, lb_table) {
        for (size_t i = 0; i < lb->n_datapaths; i++) {
            load_balancers_by_dp_add_one(local_datapaths,
                                         lb->datapaths[i], lb, lbs);
        }
        /* datapath_group column is deprecated. */
        for (size_t i = 0; lb->datapath_group
                           && i < lb->datapath_group->n_datapaths; i++) {
            load_balancers_by_dp_add_one(local_datapaths,
                                         lb->datapath_group->datapaths[i],
                                         lb, lbs);
        }
        for (size_t i = 0; lb->ls_datapath_group
                           && i < lb->ls_datapath_group->n_datapaths; i++) {
            load_balancers_by_dp_add_one(local_datapaths,
                                         lb->ls_datapath_group->datapaths[i],
                                         lb, lbs);
        }
        for (size_t i = 0; lb->lr_datapath_group
                           && i < lb->lr_datapath_group->n_datapaths; i++) {
            load_balancers_by_dp_add_one(local_datapaths,
                                         lb->lr_datapath_group->datapaths[i],
                                         lb, lbs);
        }
    }
    return lbs;
}

static void
load_balancers_by_dp_cleanup(struct hmap *lbs)
{
    if (!lbs) {
        return;
    }

    struct load_balancers_by_dp *lbs_by_dp;

    HMAP_FOR_EACH_POP (lbs_by_dp, node, lbs) {
        load_balancers_by_dp_destroy(lbs_by_dp);
    }
    hmap_destroy(lbs);
    free(lbs);
}

/* Engine node which is used to handle runtime related data to
 * load balancers. */
struct ed_type_lb_data {
    /* Locally installed 'struct ovn_controller_lb' by UUID. */
    struct hmap local_lbs;
    /* 'struct ovn_lb_five_tuple' removed during last run. */
    struct hmap removed_tuples;
    /* Load balancer <-> resource cross reference */
    struct objdep_mgr deps_mgr;
    /* Objects processed in the current engine execution.
     * Cleared by 'en_lb_data_clear_tracked_data' before each engine
     * execution. */
    struct uuidset objs_processed;

    bool change_tracked;
    /* Load balancers removed/updated during last run. */
    struct hmap old_lbs;
    /* uuids of load balancers removed during last run. */
    struct uuidset deleted;
    /* uuids of load balancers updated during last run. */
    struct uuidset updated;
    /* uuids of load balancers added during last run. */
    struct uuidset new;
};

struct lb_data_ctx_in {
    const struct sbrec_load_balancer_table *lb_table;
    const struct hmap *local_datapaths;
    const struct smap *template_vars;
};

static void
lb_data_removed_five_tuples_add(struct ed_type_lb_data *lb_data,
                                const struct ovn_controller_lb *lb)
{
    if (!ovs_feature_is_supported(OVS_CT_TUPLE_FLUSH_SUPPORT) ||
        !lb->ct_flush) {
        return;
    }

    for (size_t i = 0; i < lb->n_vips; i++) {
        struct ovn_lb_vip *vip = &lb->vips[i];

        const struct ovn_lb_backend *backend;
        VECTOR_FOR_EACH_PTR (&vip->backends, backend) {
            ovn_lb_5tuple_add(&lb_data->removed_tuples, vip, backend,
                              lb->proto);
        }
    }
}

static void
lb_data_removed_five_tuples_remove(struct ed_type_lb_data *lb_data,
                                   const struct ovn_controller_lb *lb)
{
    if (!ovs_feature_is_supported(OVS_CT_TUPLE_FLUSH_SUPPORT) ||
        !lb->ct_flush) {
        return;
    }

    for (size_t i = 0; i < lb->n_vips; i++) {
        struct ovn_lb_vip *vip = &lb->vips[i];

        const struct ovn_lb_backend *backend;
        VECTOR_FOR_EACH_PTR (&vip->backends, backend) {
            struct ovn_lb_5tuple tuple;
            ovn_lb_5tuple_init(&tuple, vip, backend, lb->proto);
            ovn_lb_5tuple_find_and_delete(&lb_data->removed_tuples, &tuple);
        }
    }
}

static void
lb_data_local_lb_add(struct ed_type_lb_data *lb_data,
                     const struct sbrec_load_balancer *sbrec_lb,
                     const struct smap *template_vars, bool tracked)
{
    struct sset template_vars_ref = SSET_INITIALIZER(&template_vars_ref);
    const struct uuid *uuid = &sbrec_lb->header_.uuid;

    struct ovn_controller_lb *lb =
        ovn_controller_lb_create(sbrec_lb, template_vars, &template_vars_ref);
    hmap_insert(&lb_data->local_lbs, &lb->hmap_node, uuid_hash(uuid));

    const char *tv_name;
    SSET_FOR_EACH (tv_name, &template_vars_ref) {
        objdep_mgr_add(&lb_data->deps_mgr, OBJDEP_TYPE_TEMPLATE, tv_name,
                       uuid);
    }

    sset_destroy(&template_vars_ref);

    lb_data_removed_five_tuples_remove(lb_data, lb);

    if (!tracked) {
        return;
    }

    if (ovn_controller_lb_find(&lb_data->old_lbs, uuid)) {
        uuidset_insert(&lb_data->updated, uuid);
        uuidset_find_and_delete(&lb_data->deleted, uuid);
    } else {
        uuidset_insert(&lb_data->new, uuid);
    }
}

static void
lb_data_local_lb_remove(struct ed_type_lb_data *lb_data,
                        struct ovn_controller_lb *lb)
{
    const struct uuid *uuid = &lb->slb->header_.uuid;

    objdep_mgr_remove_obj(&lb_data->deps_mgr, uuid);
    hmap_remove(&lb_data->local_lbs, &lb->hmap_node);

    lb_data_removed_five_tuples_add(lb_data, lb);

    hmap_insert(&lb_data->old_lbs, &lb->hmap_node, uuid_hash(uuid));
    uuidset_insert(&lb_data->deleted, uuid);
}

static bool
lb_data_handle_changed_ref(enum objdep_type type, const char *res_name,
                           struct uuidset *objs_todo, const void *in_arg,
                           void *out_arg)
{
    const struct lb_data_ctx_in *ctx_in = in_arg;
    struct ed_type_lb_data *lb_data = out_arg;

    struct uuidset_node *ofrn;
    UUIDSET_FOR_EACH (ofrn, objs_todo) {
        struct uuid *uuid = &ofrn->uuid;

        VLOG_DBG("Reprocess LB "UUID_FMT" for resource type: %s, name: %s",
                 UUID_ARGS(uuid), objdep_type_name(type), res_name);

        struct ovn_controller_lb *lb =
            ovn_controller_lb_find(&lb_data->local_lbs, uuid);
        if (!lb) {
            continue;
        }

        lb_data_local_lb_remove(lb_data, lb);

        const struct sbrec_load_balancer *sbrec_lb =
            sbrec_load_balancer_table_get_for_uuid(ctx_in->lb_table, uuid);
        if (!lb_is_local(sbrec_lb, ctx_in->local_datapaths)) {
            continue;
        }

        lb_data_local_lb_add(lb_data, sbrec_lb, ctx_in->template_vars, true);
    }

    uuidset_destroy(objs_todo);
    return true;
}

static void *
en_lb_data_init(struct engine_node *node OVS_UNUSED,
                struct engine_arg *arg OVS_UNUSED)
{
    struct ed_type_lb_data *lb_data = xzalloc(sizeof *lb_data);

    hmap_init(&lb_data->local_lbs);
    hmap_init(&lb_data->removed_tuples);
    objdep_mgr_init(&lb_data->deps_mgr);
    uuidset_init(&lb_data->objs_processed);
    lb_data->change_tracked = false;
    hmap_init(&lb_data->old_lbs);
    uuidset_init(&lb_data->deleted);
    uuidset_init(&lb_data->updated);
    uuidset_init(&lb_data->new);

    return lb_data;
}

static enum engine_node_state
en_lb_data_run(struct engine_node *node, void *data)
{
    struct ed_type_lb_data *lb_data = data;
    struct ed_type_runtime_data *rt_data =
        engine_get_input_data("runtime_data", node);
    struct ed_type_template_vars *tv_data =
        engine_get_input_data("template_vars", node);
    const struct sbrec_load_balancer_table *lb_table =
        EN_OVSDB_GET(engine_get_input("SB_load_balancer", node));

    objdep_mgr_clear(&lb_data->deps_mgr);

    struct ovn_controller_lb *lb;
    HMAP_FOR_EACH_SAFE (lb, hmap_node, &lb_data->local_lbs) {
        hmap_remove(&lb_data->local_lbs, &lb->hmap_node);
        lb_data_removed_five_tuples_add(lb_data, lb);
        ovn_controller_lb_destroy(lb);
    }

    const struct sbrec_load_balancer *sbrec_lb;
    SBREC_LOAD_BALANCER_TABLE_FOR_EACH (sbrec_lb, lb_table) {
        if (!lb_is_local(sbrec_lb, &rt_data->local_datapaths)) {
            continue;
        }

        lb_data_local_lb_add(lb_data, sbrec_lb,
                             &tv_data->local_templates, false);
    }

    return EN_UPDATED;
}

static enum engine_input_handler_result
lb_data_sb_load_balancer_handler(struct engine_node *node, void *data)
{
    struct ed_type_lb_data *lb_data = data;
    struct ed_type_runtime_data *rt_data =
        engine_get_input_data("runtime_data", node);
    struct ed_type_template_vars *tv_data =
        engine_get_input_data("template_vars", node);
    const struct sbrec_load_balancer_table *lb_table =
        EN_OVSDB_GET(engine_get_input("SB_load_balancer", node));

    const struct sbrec_load_balancer *sbrec_lb;
    SBREC_LOAD_BALANCER_TABLE_FOR_EACH_TRACKED (sbrec_lb, lb_table) {
        struct ovn_controller_lb *lb;

        if (!sbrec_load_balancer_is_new(sbrec_lb)) {
            lb = ovn_controller_lb_find(&lb_data->local_lbs,
                                        &sbrec_lb->header_.uuid);
            if (lb) {
                lb_data_local_lb_remove(lb_data, lb);
            }
        }

        if (sbrec_load_balancer_is_deleted(sbrec_lb) ||
            !lb_is_local(sbrec_lb, &rt_data->local_datapaths)) {
            continue;
        }

        lb_data_local_lb_add(lb_data, sbrec_lb,
                             &tv_data->local_templates, true);
    }

    lb_data->change_tracked = true;
    if (!uuidset_is_empty(&lb_data->deleted) ||
        !uuidset_is_empty(&lb_data->updated) ||
        !uuidset_is_empty(&lb_data->new)) {
        return EN_HANDLED_UPDATED;
    }

    return EN_HANDLED_UNCHANGED;
}

static enum engine_input_handler_result
lb_data_template_var_handler(struct engine_node *node, void *data)
{
    struct ed_type_lb_data *lb_data = data;
    struct ed_type_runtime_data *rt_data =
        engine_get_input_data("runtime_data", node);
    struct ed_type_template_vars *tv_data =
        engine_get_input_data("template_vars", node);
    const struct sbrec_load_balancer_table *lb_table =
        EN_OVSDB_GET(engine_get_input("SB_load_balancer", node));
    enum engine_input_handler_result result = EN_HANDLED_UNCHANGED;

    if (!tv_data->change_tracked) {
        return EN_UNHANDLED;
    }

    const struct lb_data_ctx_in ctx_in = {
        .lb_table = lb_table,
        .local_datapaths = &rt_data->local_datapaths,
        .template_vars = &tv_data->local_templates
    };

    const char *res_name;
    bool changed;

    SSET_FOR_EACH (res_name, &tv_data->deleted) {
        if (!objdep_mgr_handle_change(&lb_data->deps_mgr,
                                      OBJDEP_TYPE_TEMPLATE,
                                      res_name, lb_data_handle_changed_ref,
                                      &lb_data->objs_processed,
                                      &ctx_in, lb_data, &changed)) {
            return EN_UNHANDLED;
        }
        if (changed) {
            result = EN_HANDLED_UPDATED;
        }
    }
    SSET_FOR_EACH (res_name, &tv_data->updated) {
        if (!objdep_mgr_handle_change(&lb_data->deps_mgr,
                                      OBJDEP_TYPE_TEMPLATE,
                                      res_name, lb_data_handle_changed_ref,
                                      &lb_data->objs_processed,
                                      &ctx_in, lb_data, &changed)) {
            return EN_UNHANDLED;
        }
        if (changed) {
            result = EN_HANDLED_UPDATED;
        }
    }
    SSET_FOR_EACH (res_name, &tv_data->new) {
        if (!objdep_mgr_handle_change(&lb_data->deps_mgr,
                                      OBJDEP_TYPE_TEMPLATE,
                                      res_name, lb_data_handle_changed_ref,
                                      &lb_data->objs_processed,
                                      &ctx_in, lb_data, &changed)) {
            return EN_UNHANDLED;
        }
        if (changed) {
            result = EN_HANDLED_UPDATED;
        }
    }

    lb_data->change_tracked = true;

    return result;
}

static enum engine_input_handler_result
lb_data_runtime_data_handler(struct engine_node *node, void *data OVS_UNUSED)
{
    struct ed_type_lb_data *lb_data = data;
    struct ed_type_runtime_data *rt_data =
        engine_get_input_data("runtime_data", node);
    struct ed_type_template_vars *tv_data =
        engine_get_input_data("template_vars", node);
    const struct sbrec_load_balancer_table *lb_table =
        EN_OVSDB_GET(engine_get_input("SB_load_balancer", node));

    /* There are no tracked data. Fall back to full recompute of
     * lb_ct_tuple. */
    if (!rt_data->tracked) {
        return EN_UNHANDLED;
    }

    struct hmap *tracked_dp_bindings = &rt_data->tracked_dp_bindings;
    if (hmap_is_empty(tracked_dp_bindings)) {
        return EN_HANDLED_UNCHANGED;
    }

    struct hmap *lbs = NULL;

    struct tracked_datapath *tdp;
    HMAP_FOR_EACH (tdp, node, tracked_dp_bindings) {
        if (tdp->tracked_type != TRACKED_RESOURCE_NEW) {
            continue;
        }

        if (!lbs) {
            lbs = load_balancers_by_dp_init(&rt_data->local_datapaths,
                                            lb_table);
        }

        struct load_balancers_by_dp *lbs_by_dp =
            load_balancers_by_dp_find(lbs, tdp->dp);
        if (!lbs_by_dp) {
            continue;
        }

        const struct sbrec_load_balancer *sbrec_lb;
        VECTOR_FOR_EACH (&lbs_by_dp->dp_lbs, sbrec_lb) {
            struct ovn_controller_lb *lb =
                ovn_controller_lb_find(&lb_data->local_lbs,
                                       &sbrec_lb->header_.uuid);

            if (!lb && lb_is_local(sbrec_lb, &rt_data->local_datapaths)) {
                lb_data_local_lb_add(lb_data, sbrec_lb,
                                     &tv_data->local_templates, true);
            }
        }
    }

    load_balancers_by_dp_cleanup(lbs);

    lb_data->change_tracked = true;
    if (!uuidset_is_empty(&lb_data->deleted) ||
        !uuidset_is_empty(&lb_data->updated) ||
        !uuidset_is_empty(&lb_data->new)) {
        return EN_HANDLED_UPDATED;
    }

    return EN_HANDLED_UNCHANGED;
}

static void
en_lb_data_clear_tracked_data(void *data)
{
    struct ed_type_lb_data *lb_data = data;

    struct ovn_controller_lb *lb;
    HMAP_FOR_EACH_POP (lb, hmap_node, &lb_data->old_lbs) {
        ovn_controller_lb_destroy(lb);
    }

    hmap_clear(&lb_data->old_lbs);
    uuidset_clear(&lb_data->objs_processed);
    uuidset_clear(&lb_data->deleted);
    uuidset_clear(&lb_data->updated);
    uuidset_clear(&lb_data->new);
    lb_data->change_tracked = false;
}

static void
en_lb_data_cleanup(void *data)
{
    struct ed_type_lb_data *lb_data = data;

    ovn_controller_lbs_destroy(&lb_data->local_lbs);
    ovn_lb_5tuples_destroy(&lb_data->removed_tuples);
    objdep_mgr_destroy(&lb_data->deps_mgr);
    uuidset_destroy(&lb_data->objs_processed);
    ovn_controller_lbs_destroy(&lb_data->old_lbs);
    uuidset_destroy(&lb_data->deleted);
    uuidset_destroy(&lb_data->updated);
    uuidset_destroy(&lb_data->new);
}

static void
mac_binding_add_sb(struct mac_cache_data *data,
                   const struct sbrec_mac_binding *smb)
{
    struct mac_binding_data mb_data;
    if (!mac_binding_data_from_sbrec(&mb_data, smb)) {
        return;
    }

    mac_binding_add(&data->mac_bindings, mb_data, smb, 0);
}

static void
mac_binding_remove_sb(struct mac_cache_data *data,
                      const struct sbrec_mac_binding *smb)
{
    struct mac_binding_data mb_data;
    if (!mac_binding_data_from_sbrec(&mb_data, smb)) {
        return;
    }

    struct mac_binding *mb = mac_binding_find(&data->mac_bindings, &mb_data);
    if (!mb) {
        return;
    }

    mac_binding_remove(&data->mac_bindings, mb);
}

static void
fdb_add_sb(struct mac_cache_data *data, const struct sbrec_fdb *sfdb)
{
    struct fdb_data fdb_data;
    if (!fdb_data_from_sbrec(&fdb_data, sfdb)) {
        return;
    }

    struct fdb *fdb = fdb_add(&data->fdbs, fdb_data, 0);

    fdb->sbrec_fdb = sfdb;
}

static void
fdb_remove_sb(struct mac_cache_data *data, const struct sbrec_fdb *sfdb)
{
    struct fdb_data fdb_data;
    if (!fdb_data_from_sbrec(&fdb_data, sfdb)) {
        return;
    }

    struct fdb *fdb = fdb_find(&data->fdbs, &fdb_data);
    if (!fdb) {
        return;
    }

    fdb_remove(&data->fdbs, fdb);
}

static void
mac_cache_mb_handle_for_datapath(struct mac_cache_data *data,
                                 const struct sbrec_datapath_binding *dp,
                                 struct ovsdb_idl_index *sbrec_mb_by_dp)
{
    bool has_threshold = !!mac_cache_threshold_find(data, dp->tunnel_key);

    struct sbrec_mac_binding *mb_index_row =
            sbrec_mac_binding_index_init_row(sbrec_mb_by_dp);
    sbrec_mac_binding_index_set_datapath(mb_index_row, dp);

    const struct sbrec_mac_binding *mb;
    SBREC_MAC_BINDING_FOR_EACH_EQUAL (mb, mb_index_row, sbrec_mb_by_dp) {
        if (has_threshold) {
            mac_binding_add_sb(data, mb);
        } else {
            mac_binding_remove_sb(data, mb);
        }
    }

    sbrec_mac_binding_index_destroy_row(mb_index_row);
}

static void
mac_cache_fdb_handle_for_datapath(struct mac_cache_data *data,
                                  const struct sbrec_datapath_binding *dp,
                                  struct ovsdb_idl_index *sbrec_fdb_by_dp_key)
{
    bool has_threshold = !!mac_cache_threshold_find(data, dp->tunnel_key);

    struct sbrec_fdb *fdb_index_row =
            sbrec_fdb_index_init_row(sbrec_fdb_by_dp_key);
    sbrec_fdb_index_set_dp_key(fdb_index_row, dp->tunnel_key);

    const struct sbrec_fdb *fdb;
    SBREC_FDB_FOR_EACH_EQUAL (fdb, fdb_index_row, sbrec_fdb_by_dp_key) {
        if (has_threshold) {
            fdb_add_sb(data, fdb);
        } else {
            fdb_remove_sb(data, fdb);
        }
    }

    sbrec_fdb_index_destroy_row(fdb_index_row);
}

static void *
en_mac_cache_init(struct engine_node *node OVS_UNUSED,
                   struct engine_arg *arg OVS_UNUSED)
{
    struct mac_cache_data *cache_data = xzalloc(sizeof *cache_data);

    hmap_init(&cache_data->thresholds);
    hmap_init(&cache_data->mac_bindings);
    hmap_init(&cache_data->fdbs);

    return cache_data;
}

static enum engine_node_state
en_mac_cache_run(struct engine_node *node, void *data)
{
    struct mac_cache_data *cache_data = data;
    struct ed_type_runtime_data *rt_data =
            engine_get_input_data("runtime_data", node);
    const struct sbrec_datapath_binding_table *dp_table =
            EN_OVSDB_GET(engine_get_input("SB_datapath_binding", node));
    struct ovsdb_idl_index *sbrec_mb_by_dp =
            engine_ovsdb_node_get_index(
                    engine_get_input("SB_mac_binding", node),
                    "datapath");
    struct ovsdb_idl_index *sbrec_fdb_by_dp_key =
            engine_ovsdb_node_get_index(
                    engine_get_input("SB_fdb", node),
                    "dp_key");

    mac_cache_thresholds_clear(cache_data);
    mac_bindings_clear(&cache_data->mac_bindings);
    fdbs_clear(&cache_data->fdbs);

    const struct sbrec_datapath_binding *sbrec_dp;
    SBREC_DATAPATH_BINDING_TABLE_FOR_EACH (sbrec_dp, dp_table) {
        if (!get_local_datapath(&rt_data->local_datapaths,
                                sbrec_dp->tunnel_key)) {
            continue;
        }

        mac_cache_threshold_add(cache_data, sbrec_dp);
        mac_cache_mb_handle_for_datapath(cache_data, sbrec_dp,
                                         sbrec_mb_by_dp);
        mac_cache_fdb_handle_for_datapath(cache_data, sbrec_dp,
                                          sbrec_fdb_by_dp_key);
    }

    return EN_UPDATED;
}

static enum engine_input_handler_result
mac_cache_sb_mac_binding_handler(struct engine_node *node, void *data)
{
    struct mac_cache_data *cache_data = data;
    struct ed_type_runtime_data *rt_data =
            engine_get_input_data("runtime_data", node);
    const struct sbrec_mac_binding_table *mb_table =
            EN_OVSDB_GET(engine_get_input("SB_mac_binding", node));
    size_t previous_size = hmap_count(&cache_data->mac_bindings);

    const struct sbrec_mac_binding *sbrec_mb;
    SBREC_MAC_BINDING_TABLE_FOR_EACH_TRACKED (sbrec_mb, mb_table) {
        if (!sbrec_mac_binding_is_new(sbrec_mb)) {
            mac_binding_remove_sb(cache_data, sbrec_mb);
        }

        if (sbrec_mac_binding_is_deleted(sbrec_mb) ||
            !get_local_datapath(&rt_data->local_datapaths,
                                sbrec_mb->datapath->tunnel_key)) {
            continue;
        }

        if (mac_cache_threshold_find(cache_data,
                                     sbrec_mb->datapath->tunnel_key)) {
            mac_binding_add_sb(cache_data, sbrec_mb);
        }
    }

    if (hmap_count(&cache_data->mac_bindings) != previous_size) {
        return EN_HANDLED_UPDATED;
    }

    return EN_HANDLED_UNCHANGED;
}

static enum engine_input_handler_result
mac_cache_sb_fdb_handler(struct engine_node *node, void *data)
{
    struct mac_cache_data *cache_data = data;
    struct ed_type_runtime_data *rt_data =
            engine_get_input_data("runtime_data", node);
    const struct sbrec_fdb_table *fdb_table =
            EN_OVSDB_GET(engine_get_input("SB_fdb", node));

    size_t previous_size = hmap_count(&cache_data->fdbs);

    struct local_datapath *local_dp;
    const struct sbrec_fdb *sbrec_fdb;
    SBREC_FDB_TABLE_FOR_EACH_TRACKED (sbrec_fdb, fdb_table) {
        if (!sbrec_fdb_is_new(sbrec_fdb)) {
            fdb_remove_sb(cache_data, sbrec_fdb);
        }

        local_dp = get_local_datapath(&rt_data->local_datapaths,
                                      sbrec_fdb->dp_key);
        if (sbrec_fdb_is_deleted(sbrec_fdb) || !local_dp) {
            continue;
        }

        if (mac_cache_threshold_find(cache_data, sbrec_fdb->dp_key)) {
            fdb_add_sb(cache_data, sbrec_fdb);
        }
    }

    if (hmap_count(&cache_data->fdbs) != previous_size) {
        return EN_HANDLED_UPDATED;
    }

    return EN_HANDLED_UNCHANGED;
}

static enum engine_input_handler_result
mac_cache_runtime_data_handler(struct engine_node *node, void *data OVS_UNUSED)
{
    struct mac_cache_data *cache_data = data;
    struct ed_type_runtime_data *rt_data =
            engine_get_input_data("runtime_data", node);
    struct ovsdb_idl_index *sbrec_mb_by_dp =
            engine_ovsdb_node_get_index(
                    engine_get_input("SB_mac_binding", node),
                    "datapath");
    struct ovsdb_idl_index *sbrec_fdb_by_dp_key =
            engine_ovsdb_node_get_index(
                    engine_get_input("SB_fdb", node),
                    "dp_key");

    /* There are no tracked data. Fall back to full recompute. */
    if (!rt_data->tracked) {
        return EN_UNHANDLED;
    }

    size_t previous_mb_size = hmap_count(&cache_data->mac_bindings);
    size_t previous_fdb_size = hmap_count(&cache_data->fdbs);

    struct tracked_datapath *tdp;
    HMAP_FOR_EACH (tdp, node, &rt_data->tracked_dp_bindings) {
        if (tdp->tracked_type == TRACKED_RESOURCE_UPDATED) {
            continue;
        }
        mac_cache_threshold_replace(cache_data, tdp->dp,
                                    &rt_data->local_datapaths);
    }

    HMAP_FOR_EACH (tdp, node, &rt_data->tracked_dp_bindings) {
        mac_cache_mb_handle_for_datapath(cache_data, tdp->dp,
                                         sbrec_mb_by_dp);

        mac_cache_fdb_handle_for_datapath(cache_data, tdp->dp,
                                          sbrec_fdb_by_dp_key);
    }

    if (hmap_count(&cache_data->mac_bindings) != previous_mb_size ||
        hmap_count(&cache_data->fdbs) != previous_fdb_size) {
        return EN_HANDLED_UPDATED;
    }

    return EN_HANDLED_UNCHANGED;
}

static enum engine_input_handler_result
mac_cache_sb_datapath_binding_handler(struct engine_node *node, void *data)
{
    struct mac_cache_data *cache_data = data;
    struct ed_type_runtime_data *rt_data =
            engine_get_input_data("runtime_data", node);
    const struct sbrec_datapath_binding_table *dp_table =
            EN_OVSDB_GET(engine_get_input("SB_datapath_binding", node));
    struct ovsdb_idl_index *sbrec_mb_by_dp =
            engine_ovsdb_node_get_index(
                    engine_get_input("SB_mac_binding", node),
                    "datapath");
    struct ovsdb_idl_index *sbrec_fdb_by_dp_key =
            engine_ovsdb_node_get_index(
                    engine_get_input("SB_fdb", node),
                    "dp_key");

    size_t previous_mb_size = hmap_count(&cache_data->mac_bindings);
    size_t previous_fdb_size = hmap_count(&cache_data->fdbs);
    bool sync_needed = false;

    const struct sbrec_datapath_binding *sbrec_dp;
    SBREC_DATAPATH_BINDING_TABLE_FOR_EACH_TRACKED (sbrec_dp, dp_table) {
        if (!sbrec_datapath_binding_is_new(sbrec_dp) &&
            sbrec_datapath_binding_is_updated(
                sbrec_dp, SBREC_DATAPATH_BINDING_COL_TUNNEL_KEY)) {
            sync_needed = true;
        }

        mac_cache_threshold_replace(cache_data, sbrec_dp,
                                    &rt_data->local_datapaths);
    }

    if (sync_needed) {
        mac_cache_thresholds_sync(cache_data, &rt_data->local_datapaths);
    }

    SBREC_DATAPATH_BINDING_TABLE_FOR_EACH_TRACKED (sbrec_dp, dp_table) {
        mac_cache_mb_handle_for_datapath(cache_data, sbrec_dp,
                                         sbrec_mb_by_dp);

        mac_cache_fdb_handle_for_datapath(cache_data, sbrec_dp,
                                          sbrec_fdb_by_dp_key);
    }

    if (hmap_count(&cache_data->mac_bindings) != previous_mb_size ||
        hmap_count(&cache_data->fdbs) != previous_fdb_size) {
        return EN_HANDLED_UPDATED;
    }

    return EN_HANDLED_UNCHANGED;
}


static void
en_mac_cache_cleanup(void *data)
{
    struct mac_cache_data *cache_data = data;

    mac_cache_thresholds_clear(cache_data);
    hmap_destroy(&cache_data->thresholds);
    mac_bindings_clear(&cache_data->mac_bindings);
    hmap_destroy(&cache_data->mac_bindings);
    fdbs_clear(&cache_data->fdbs);
    hmap_destroy(&cache_data->fdbs);
}

struct ed_type_bfd_chassis {
    struct sset bfd_chassis;
};

static void *
en_bfd_chassis_init(struct engine_node *node OVS_UNUSED,
                   struct engine_arg *arg OVS_UNUSED)
{
    struct ed_type_bfd_chassis *data = xzalloc(sizeof *data);
    sset_init(&data->bfd_chassis);
    return data;
}

static enum engine_node_state
en_bfd_chassis_run(struct engine_node *node, void *data OVS_UNUSED)
{
    struct ed_type_bfd_chassis *bfd_chassis = data;
    const struct ovsrec_open_vswitch_table *ovs_table =
        EN_OVSDB_GET(engine_get_input("OVS_open_vswitch", node));
    const char *chassis_id = get_ovs_chassis_id(ovs_table);
    const struct sbrec_ha_chassis_group_table *ha_chassis_grp_table =
        EN_OVSDB_GET(engine_get_input("SB_ha_chassis_group", node));
    struct ovsdb_idl_index *sbrec_chassis_by_name =
        engine_ovsdb_node_get_index(
            engine_get_input("SB_chassis", node),
            "name");
    const struct sbrec_chassis *chassis
        = chassis_lookup_by_name(sbrec_chassis_by_name, chassis_id);

    sset_clear(&bfd_chassis->bfd_chassis);
    bfd_calculate_chassis(chassis, ha_chassis_grp_table,
                          &bfd_chassis->bfd_chassis);
    return EN_UPDATED;
}

static void
en_bfd_chassis_cleanup(void *data OVS_UNUSED){
    struct ed_type_bfd_chassis *bfd_chassis = data;
    sset_destroy(&bfd_chassis->bfd_chassis);
}

static void *
en_dns_cache_init(struct engine_node *node OVS_UNUSED,
                  struct engine_arg *arg OVS_UNUSED)
{
    ovn_dns_cache_init();
    return NULL;
}

static enum engine_node_state
en_dns_cache_run(struct engine_node *node, void *data OVS_UNUSED)
{
    const struct sbrec_dns_table *dns_table =
        EN_OVSDB_GET(engine_get_input("SB_dns", node));

    ovn_dns_sync_cache(dns_table);

    return EN_UPDATED;
}

static enum engine_input_handler_result
dns_cache_sb_dns_handler(struct engine_node *node, void *data OVS_UNUSED)
{
    const struct sbrec_dns_table *dns_table =
        EN_OVSDB_GET(engine_get_input("SB_dns", node));

    ovn_dns_update_cache(dns_table);

    return EN_HANDLED_UPDATED;
}

static void
en_dns_cache_cleanup(void *data OVS_UNUSED)
{
    ovn_dns_cache_destroy();
}


/* Engine node which is used to handle the Non VIF data like
 *   - OVS patch ports
 *   - Tunnel ports and the related chassis information.
 */
struct ed_type_non_vif_data {
    struct simap patch_ofports; /* simap of patch ovs ports. */
    struct hmap chassis_tunnels; /* hmap of 'struct chassis_tunnel' from the
                                  * tunnel OVS ports. */
};

static void *
en_non_vif_data_init(struct engine_node *node OVS_UNUSED,
                     struct engine_arg *arg OVS_UNUSED)
{
    struct ed_type_non_vif_data *data = xzalloc(sizeof *data);
    simap_init(&data->patch_ofports);
    hmap_init(&data->chassis_tunnels);
    return data;
}

static void
en_non_vif_data_cleanup(void *data OVS_UNUSED)
{
    struct ed_type_non_vif_data *ed_non_vif_data = data;
    simap_destroy(&ed_non_vif_data->patch_ofports);
    chassis_tunnels_destroy(&ed_non_vif_data->chassis_tunnels);
}

static enum engine_node_state
en_non_vif_data_run(struct engine_node *node, void *data)
{
    struct ed_type_non_vif_data *ed_non_vif_data = data;
    simap_destroy(&ed_non_vif_data->patch_ofports);
    chassis_tunnels_destroy(&ed_non_vif_data->chassis_tunnels);
    simap_init(&ed_non_vif_data->patch_ofports);
    hmap_init(&ed_non_vif_data->chassis_tunnels);

    const struct ovsrec_open_vswitch_table *ovs_table =
        EN_OVSDB_GET(engine_get_input("OVS_open_vswitch", node));
    const struct ovsrec_bridge_table *bridge_table =
        EN_OVSDB_GET(engine_get_input("OVS_bridge", node));

    const struct ovsrec_bridge *br_int = get_br_int(bridge_table, ovs_table);
    const char *chassis_id = get_ovs_chassis_id(ovs_table);
    ovs_assert(br_int && chassis_id);

    struct ovsdb_idl_index *sbrec_chassis_by_name =
        engine_ovsdb_node_get_index(
            engine_get_input("SB_chassis", node),
            "name");

    const struct sbrec_chassis *chassis
        = chassis_lookup_by_name(sbrec_chassis_by_name, chassis_id);
    ovs_assert(chassis);

    local_nonvif_data_run(br_int, chassis, &ed_non_vif_data->patch_ofports,
                          &ed_non_vif_data->chassis_tunnels);
    return EN_UPDATED;
}

static enum engine_input_handler_result
non_vif_data_ovs_iface_handler(struct engine_node *node, void *data OVS_UNUSED)
{
    const struct ovsrec_interface_table *iface_table =
        EN_OVSDB_GET(engine_get_input("OVS_interface", node));

    if (local_nonvif_data_handle_ovs_iface_changes(iface_table)) {
        return EN_HANDLED_UNCHANGED;
    } else {
        return EN_UNHANDLED;
    }
}

struct ed_type_northd_options {
    bool explicit_arp_ns_output;
    bool always_tunnel; /* Indicates if the traffic to the
                         * logical port of a bridged logical
                         * switch (i.e with localnet port) should
                         * be tunnelled or sent via the localnet
                         * port.  Default value is 'false'. */
    bool register_consolidation;
    bool enable_ch_nb_cfg_update;
};


static void *
en_northd_options_init(struct engine_node *node OVS_UNUSED,
                       struct engine_arg *arg OVS_UNUSED)
{
    struct ed_type_northd_options *n_opts = xzalloc(sizeof *n_opts);
    return n_opts;
}

static void
en_northd_options_cleanup(void *data OVS_UNUSED)
{
}

static enum engine_node_state
en_northd_options_run(struct engine_node *node, void *data)
{
    struct ed_type_northd_options *n_opts = data;
    const struct sbrec_sb_global_table *sb_global_table =
        EN_OVSDB_GET(engine_get_input("SB_sb_global", node));
    const struct sbrec_sb_global *sb_global =
        sbrec_sb_global_table_first(sb_global_table);

    n_opts->explicit_arp_ns_output =
            sb_global
            ? smap_get_bool(&sb_global->options, "arp_ns_explicit_output",
                            false)
            : false;

    n_opts->always_tunnel =
            sb_global
            ? smap_get_bool(&sb_global->options, "always_tunnel",
                            false)
            : false;

    n_opts->register_consolidation =
        sb_global
        ? smap_get_bool(&sb_global->options, "register_consolidation",
                        false)
        : false;

    n_opts->enable_ch_nb_cfg_update =
        sb_global
        ? smap_get_bool(&sb_global->options, "enable_chassis_nb_cfg_update",
                        true)
        : true;

    return EN_UPDATED;
}

static enum engine_input_handler_result
en_northd_options_sb_sb_global_handler(struct engine_node *node, void *data)
{
    struct ed_type_northd_options *n_opts = data;
    const struct sbrec_sb_global_table *sb_global_table =
        EN_OVSDB_GET(engine_get_input("SB_sb_global", node));
    const struct sbrec_sb_global *sb_global =
        sbrec_sb_global_table_first(sb_global_table);
    enum engine_input_handler_result result = EN_HANDLED_UNCHANGED;

    bool explicit_arp_ns_output =
            sb_global
            ? smap_get_bool(&sb_global->options, "arp_ns_explicit_output",
                            false)
            : false;

    if (explicit_arp_ns_output != n_opts->explicit_arp_ns_output) {
        n_opts->explicit_arp_ns_output = explicit_arp_ns_output;
        result =  EN_HANDLED_UPDATED;
    }

    bool always_tunnel =
            sb_global
            ? smap_get_bool(&sb_global->options, "always_tunnel",
                            false)
            : false;

    if (always_tunnel != n_opts->always_tunnel) {
        n_opts->always_tunnel = always_tunnel;
        result = EN_HANDLED_UPDATED;
    }

    bool register_consolidation =
        sb_global
        ? smap_get_bool(&sb_global->options, "register_consolidation",
                        false)
        : false;

    if (register_consolidation != n_opts->register_consolidation) {
        n_opts->register_consolidation = register_consolidation;
        result = EN_HANDLED_UPDATED;
    }

    bool enable_ch_nb_cfg_update =
        sb_global
        ? smap_get_bool(&sb_global->options, "enable_chassis_nb_cfg_update",
                        true)
        : true;

    if (enable_ch_nb_cfg_update != n_opts->enable_ch_nb_cfg_update) {
        n_opts->enable_ch_nb_cfg_update = enable_ch_nb_cfg_update;
        result = EN_HANDLED_UPDATED;
    }

    return result;
}

struct ed_type_dhcp_options {
    struct hmap v4_opts;
    struct hmap v6_opts;
};

static void *
en_dhcp_options_init(struct engine_node *node OVS_UNUSED,
                     struct engine_arg *arg OVS_UNUSED)
{
    struct ed_type_dhcp_options *dhcp_opts = xzalloc(sizeof *dhcp_opts);

    hmap_init(&dhcp_opts->v4_opts);
    hmap_init(&dhcp_opts->v6_opts);
    return dhcp_opts;
}

static void
en_dhcp_options_cleanup(void *data)
{
    struct ed_type_dhcp_options *dhcp_opts = data;

    dhcp_opts_destroy(&dhcp_opts->v4_opts);
    dhcp_opts_destroy(&dhcp_opts->v6_opts);
}

static enum engine_node_state
en_dhcp_options_run(struct engine_node *node, void *data)
{
    struct ed_type_dhcp_options *dhcp_opts = data;

    const struct sbrec_dhcp_options_table *dhcp_table =
        EN_OVSDB_GET(engine_get_input("SB_dhcp_options", node));

    const struct sbrec_dhcpv6_options_table *dhcpv6_table =
        EN_OVSDB_GET(engine_get_input("SB_dhcpv6_options", node));

    dhcp_opts_clear(&dhcp_opts->v4_opts);
    dhcp_opts_clear(&dhcp_opts->v6_opts);

    const struct sbrec_dhcp_options *dhcp_opt_row;
    SBREC_DHCP_OPTIONS_TABLE_FOR_EACH (dhcp_opt_row, dhcp_table) {
        dhcp_opt_add(&dhcp_opts->v4_opts, dhcp_opt_row->name,
                     dhcp_opt_row->code, dhcp_opt_row->type);
    }

    const struct sbrec_dhcpv6_options *dhcpv6_opt_row;
    SBREC_DHCPV6_OPTIONS_TABLE_FOR_EACH (dhcpv6_opt_row, dhcpv6_table) {
       dhcp_opt_add(&dhcp_opts->v6_opts, dhcpv6_opt_row->name,
                    dhcpv6_opt_row->code, dhcpv6_opt_row->type);
    }
    return EN_UPDATED;
}

struct lflow_output_persistent_data {
    struct lflow_cache *lflow_cache;
};

struct ed_type_lflow_output {
    /* Logical flow table */
    struct ovn_desired_flow_table flow_table;
    /* group ids for load balancing */
    struct ovn_extend_table group_table;
    /* meter ids for QoS */
    struct ovn_extend_table meter_table;
    /* lflow <-> resource cross reference */
    struct objdep_mgr lflow_deps_mgr;;
    /* conjunciton ID usage information of lflows */
    struct conj_ids conj_ids;

    /* objects (lflows) processed in the current engine execution.
     * Cleared by en_lflow_output_clear_tracked_data before each engine
     * execution. */
    struct uuidset objs_processed;

    /* Data which is persistent and not cleared during
     * full recompute. */
    struct lflow_output_persistent_data pd;

    /* Fixed neighbor discovery supported options. */
    struct hmap nd_ra_opts;

    /* Fixed controller_event supported options. */
    struct controller_event_options controller_event_opts;

    /* Configured Flow Sample Collector Sets. */
    struct flow_collector_ids collector_ids;
};

static void
init_lflow_ctx(struct engine_node *node,
               struct ed_type_lflow_output *fo,
               struct lflow_ctx_in *l_ctx_in,
               struct lflow_ctx_out *l_ctx_out)
{
    struct ovsdb_idl_index *sbrec_port_binding_by_name =
        engine_ovsdb_node_get_index(
                engine_get_input("SB_port_binding", node),
                "name");

    struct ovsdb_idl_index *sbrec_port_binding_by_key =
        engine_ovsdb_node_get_index(
                engine_get_input("SB_port_binding", node),
                "key");

    struct ovsdb_idl_index *sbrec_logical_flow_by_dp =
        engine_ovsdb_node_get_index(
                engine_get_input("SB_logical_flow", node),
                "logical_datapath");

    struct ovsdb_idl_index *sbrec_logical_flow_by_dp_group =
        engine_ovsdb_node_get_index(
                engine_get_input("SB_logical_flow", node),
                "logical_dp_group");

    struct ovsdb_idl_index *sbrec_mc_group_by_name_dp =
        engine_ovsdb_node_get_index(
                engine_get_input("SB_multicast_group", node),
                "name_datapath");

    struct ovsdb_idl_index *sbrec_fdb_by_dp_key =
        engine_ovsdb_node_get_index(
                engine_get_input("SB_fdb", node),
                "dp_key");

    struct ovsdb_idl_index *sbrec_mac_binding_by_datapath =
        engine_ovsdb_node_get_index(
                engine_get_input("SB_mac_binding", node),
                "datapath");

    struct ovsdb_idl_index *sbrec_static_mac_binding_by_datapath =
        engine_ovsdb_node_get_index(
                engine_get_input("SB_static_mac_binding", node),
                "datapath");

    const struct sbrec_port_binding_table *port_binding_table =
        EN_OVSDB_GET(engine_get_input("SB_port_binding", node));

    const struct sbrec_mac_binding_table *mac_binding_table =
        EN_OVSDB_GET(engine_get_input("SB_mac_binding", node));

    const struct sbrec_logical_flow_table *logical_flow_table =
        EN_OVSDB_GET(engine_get_input("SB_logical_flow", node));

    const struct sbrec_logical_dp_group_table *logical_dp_group_table =
        EN_OVSDB_GET(engine_get_input("SB_logical_dp_group", node));

    const struct sbrec_multicast_group_table *multicast_group_table =
        EN_OVSDB_GET(engine_get_input("SB_multicast_group", node));

    const struct sbrec_fdb_table *fdb_table =
        EN_OVSDB_GET(engine_get_input("SB_fdb", node));

    const struct sbrec_static_mac_binding_table *smb_table =
        EN_OVSDB_GET(engine_get_input("SB_static_mac_binding", node));

    const struct ovsrec_open_vswitch_table *ovs_table =
        EN_OVSDB_GET(engine_get_input("OVS_open_vswitch", node));

    const char *chassis_id = get_ovs_chassis_id(ovs_table);
    const struct sbrec_chassis *chassis = NULL;
    struct ovsdb_idl_index *sbrec_chassis_by_name =
        engine_ovsdb_node_get_index(
                engine_get_input("SB_chassis", node),
                "name");
    if (chassis_id) {
        chassis = chassis_lookup_by_name(sbrec_chassis_by_name, chassis_id);
    }

    ovs_assert(chassis);

    struct ed_type_runtime_data *rt_data =
        engine_get_input_data("runtime_data", node);

    struct ed_type_non_vif_data *non_vif_data =
        engine_get_input_data("non_vif_data", node);

    struct ed_type_addr_sets *as_data =
        engine_get_input_data("addr_sets", node);
    struct shash *addr_sets = &as_data->addr_sets;

    struct ed_type_port_groups *pg_data =
        engine_get_input_data("port_groups", node);
    struct shash *port_groups = &pg_data->port_groups_cs_local;

    struct ed_type_northd_options *n_opts =
        engine_get_input_data("northd_options", node);

    struct ed_type_dhcp_options *dhcp_opts =
        engine_get_input_data("dhcp_options", node);

    struct ed_type_template_vars *template_vars =
        engine_get_input_data("template_vars", node);

    struct ed_type_lb_data *lb_data =
        engine_get_input_data("lb_data", node);

    l_ctx_in->sbrec_multicast_group_by_name_datapath =
        sbrec_mc_group_by_name_dp;
    l_ctx_in->sbrec_logical_flow_by_logical_datapath =
        sbrec_logical_flow_by_dp;
    l_ctx_in->sbrec_logical_flow_by_logical_dp_group =
        sbrec_logical_flow_by_dp_group;
    l_ctx_in->sbrec_port_binding_by_name = sbrec_port_binding_by_name;
    l_ctx_in->sbrec_port_binding_by_key = sbrec_port_binding_by_key;
    l_ctx_in->sbrec_fdb_by_dp_key = sbrec_fdb_by_dp_key;
    l_ctx_in->sbrec_mac_binding_by_datapath = sbrec_mac_binding_by_datapath;
    l_ctx_in->sbrec_static_mac_binding_by_datapath =
        sbrec_static_mac_binding_by_datapath;
    l_ctx_in->port_binding_table = port_binding_table;
    l_ctx_in->mac_binding_table = mac_binding_table;
    l_ctx_in->logical_flow_table = logical_flow_table;
    l_ctx_in->logical_dp_group_table = logical_dp_group_table;
    l_ctx_in->mc_group_table = multicast_group_table;
    l_ctx_in->fdb_table = fdb_table,
    l_ctx_in->chassis = chassis;
    l_ctx_in->static_mac_binding_table = smb_table;
    l_ctx_in->local_datapaths = &rt_data->local_datapaths;
    l_ctx_in->addr_sets = addr_sets;
    l_ctx_in->port_groups = port_groups;
    l_ctx_in->active_tunnels = &rt_data->active_tunnels;
    l_ctx_in->related_lport_ids = &rt_data->related_lports.lport_ids;
    l_ctx_in->binding_lports = &rt_data->lbinding_data.lports;
    l_ctx_in->localnet_learn_fdb = rt_data->localnet_learn_fdb;
    l_ctx_in->localnet_learn_fdb_changed = rt_data->localnet_learn_fdb_changed;
    l_ctx_in->chassis_tunnels = &non_vif_data->chassis_tunnels;
    l_ctx_in->explicit_arp_ns_output = n_opts->explicit_arp_ns_output;
    l_ctx_in->register_consolidation = n_opts->register_consolidation;
    l_ctx_in->nd_ra_opts = &fo->nd_ra_opts;
    l_ctx_in->dhcp_opts = &dhcp_opts->v4_opts;
    l_ctx_in->dhcpv6_opts = &dhcp_opts->v6_opts;
    l_ctx_in->controller_event_opts = &fo->controller_event_opts;
    l_ctx_in->template_vars = &template_vars->local_templates;
    l_ctx_in->collector_ids = &fo->collector_ids;
    l_ctx_in->local_lbs = &lb_data->local_lbs;

    l_ctx_out->flow_table = &fo->flow_table;
    l_ctx_out->group_table = &fo->group_table;
    l_ctx_out->meter_table = &fo->meter_table;
    l_ctx_out->lflow_deps_mgr = &fo->lflow_deps_mgr;
    l_ctx_out->conj_ids = &fo->conj_ids;
    l_ctx_out->objs_processed = &fo->objs_processed;
    l_ctx_out->lflow_cache = fo->pd.lflow_cache;
}

static void *
en_lflow_output_init(struct engine_node *node OVS_UNUSED,
                     struct engine_arg *arg OVS_UNUSED)
{
    struct ed_type_lflow_output *data = xzalloc(sizeof *data);
    ovn_desired_flow_table_init(&data->flow_table);
    ovn_extend_table_init(&data->group_table, "group-table", 0);
    ovn_extend_table_init(&data->meter_table, "meter-table", 0);
    objdep_mgr_init(&data->lflow_deps_mgr);
    lflow_conj_ids_init(&data->conj_ids);
    uuidset_init(&data->objs_processed);
    nd_ra_opts_init(&data->nd_ra_opts);
    controller_event_opts_init(&data->controller_event_opts);
    flow_collector_ids_init(&data->collector_ids);
    return data;
}

static void
en_lflow_output_clear_tracked_data(void *data)
{
    struct ed_type_lflow_output *flow_output_data = data;
    uuidset_clear(&flow_output_data->objs_processed);
}

static void
en_lflow_output_cleanup(void *data)
{
    struct ed_type_lflow_output *flow_output_data = data;
    ovn_desired_flow_table_destroy(&flow_output_data->flow_table);
    ovn_extend_table_destroy(&flow_output_data->group_table);
    ovn_extend_table_destroy(&flow_output_data->meter_table);
    objdep_mgr_destroy(&flow_output_data->lflow_deps_mgr);
    lflow_conj_ids_destroy(&flow_output_data->conj_ids);
    uuidset_destroy(&flow_output_data->objs_processed);
    lflow_cache_destroy(flow_output_data->pd.lflow_cache);
    nd_ra_opts_destroy(&flow_output_data->nd_ra_opts);
    controller_event_opts_destroy(&flow_output_data->controller_event_opts);
    flow_collector_ids_destroy(&flow_output_data->collector_ids);
}

static enum engine_node_state
en_lflow_output_run(struct engine_node *node, void *data)
{
    const struct ovsrec_open_vswitch_table *ovs_table =
        EN_OVSDB_GET(engine_get_input("OVS_open_vswitch", node));
    const struct ovsrec_bridge_table *bridge_table =
        EN_OVSDB_GET(engine_get_input("OVS_bridge", node));
    const struct ovsrec_bridge *br_int = get_br_int(bridge_table, ovs_table);
    const char *chassis_id = get_ovs_chassis_id(ovs_table);
    const struct ovsrec_flow_sample_collector_set_table *flow_collector_table =
        EN_OVSDB_GET(engine_get_input("OVS_flow_sample_collector_set", node));

    struct ovsdb_idl_index *sbrec_chassis_by_name =
        engine_ovsdb_node_get_index(
                engine_get_input("SB_chassis", node),
                "name");

    const struct sbrec_chassis *chassis = NULL;
    if (chassis_id) {
        chassis = chassis_lookup_by_name(sbrec_chassis_by_name, chassis_id);
    }

    ovs_assert(br_int && chassis);

    const struct ovsrec_flow_sample_collector_set *set;
    OVSREC_FLOW_SAMPLE_COLLECTOR_SET_TABLE_FOR_EACH (set,
                                                    flow_collector_table) {
        if (set->bridge == br_int) {
            struct ed_type_lflow_output *lfo = data;
            flow_collector_ids_clear(&lfo->collector_ids);
            flow_collector_ids_init_from_table(&lfo->collector_ids,
                                               flow_collector_table);
        }
    }

    struct ed_type_lflow_output *fo = data;
    struct ovn_desired_flow_table *lflow_table = &fo->flow_table;
    struct ovn_extend_table *group_table = &fo->group_table;
    struct ovn_extend_table *meter_table = &fo->meter_table;
    struct objdep_mgr *lflow_deps_mgr = &fo->lflow_deps_mgr;

    static bool first_run = true;
    if (first_run) {
        first_run = false;
    } else {
        ovn_desired_flow_table_clear(lflow_table);
        ovn_extend_table_clear(group_table, false /* desired */);
        ovn_extend_table_clear(meter_table, false /* desired */);
        objdep_mgr_clear(lflow_deps_mgr);
        lflow_conj_ids_clear(&fo->conj_ids);
    }

    struct controller_engine_ctx *ctrl_ctx = engine_get_context()->client_ctx;

    fo->pd.lflow_cache = ctrl_ctx->lflow_cache;

    struct lflow_ctx_in l_ctx_in;
    struct lflow_ctx_out l_ctx_out;
    init_lflow_ctx(node, fo, &l_ctx_in, &l_ctx_out);
    lflow_run(&l_ctx_in, &l_ctx_out);

    return EN_UPDATED;
}

static enum engine_input_handler_result
lflow_output_sb_logical_flow_handler(struct engine_node *node, void *data)
{
    struct ed_type_lflow_output *fo = data;
    struct lflow_ctx_in l_ctx_in;
    struct lflow_ctx_out l_ctx_out;
    init_lflow_ctx(node, fo, &l_ctx_in, &l_ctx_out);

    if (lflow_handle_changed_flows(&l_ctx_in, &l_ctx_out)) {
        return EN_HANDLED_UPDATED;
    }

    return EN_UNHANDLED;
}

static enum engine_input_handler_result
lflow_output_flow_sample_collector_set_handler(struct engine_node *node,
                                               void *data OVS_UNUSED)
{
    const struct ovsrec_flow_sample_collector_set_table *flow_collector_table =
        EN_OVSDB_GET(engine_get_input("OVS_flow_sample_collector_set", node));
    const struct ovsrec_open_vswitch_table *ovs_table =
        EN_OVSDB_GET(engine_get_input("OVS_open_vswitch", node));
    const struct ovsrec_bridge_table *bridge_table =
        EN_OVSDB_GET(engine_get_input("OVS_bridge", node));

    const struct ovsrec_open_vswitch *cfg =
        ovsrec_open_vswitch_table_first(ovs_table);
    if (!cfg) {
        return EN_HANDLED_UNCHANGED;
    }

    const struct ovsrec_bridge *br_int;
    br_int = get_bridge(bridge_table, br_int_name(ovs_table));
    if (!br_int) {
        return EN_HANDLED_UNCHANGED;
    }

    const struct ovsrec_flow_sample_collector_set *set;
    OVSREC_FLOW_SAMPLE_COLLECTOR_SET_TABLE_FOR_EACH_TRACKED (set,
                                                        flow_collector_table) {
        if (set->bridge == br_int) {
            struct ed_type_lflow_output *lfo = data;
            flow_collector_ids_clear(&lfo->collector_ids);
            flow_collector_ids_init_from_table(&lfo->collector_ids,
                                               flow_collector_table);
            return EN_UNHANDLED;
        }
    }

    return EN_HANDLED_UPDATED;
}

static void
pflow_output_get_debug(struct engine_node *node, struct physical_debug *debug)
{
    const struct ovsrec_open_vswitch_table *ovs_table =
        EN_OVSDB_GET(engine_get_input("OVS_open_vswitch", node));
    const struct ovsrec_bridge_table *bridge_table =
        EN_OVSDB_GET(engine_get_input("OVS_bridge", node));
    const struct sbrec_sb_global_table *sb_global_table =
        EN_OVSDB_GET(engine_get_input("SB_sb_global", node));
    const struct sbrec_sb_global *sb_global =
        sbrec_sb_global_table_first(sb_global_table);

    if (!debug) {
        return;
    }
    debug->collector_set_id = 0;
    debug->obs_domain_id = 0;

    const struct ovsrec_open_vswitch *ovs_cfg =
        ovsrec_open_vswitch_table_first(ovs_table);
    if (!ovs_cfg) {
        return;
    }

    const struct ovsrec_bridge *br_int;
    br_int = get_bridge(bridge_table, br_int_name(ovs_table));
    if (!br_int) {
        return;
    }

    const uint32_t debug_collector_set =
        smap_get_uint(&sb_global->options, "debug_drop_collector_set", 0);
    if (!debug_collector_set) {
        return;
    }

    struct ovsdb_idl_index *ovsrec_flow_sample_collector_set_by_id =
        engine_ovsdb_node_get_index(
                engine_get_input("OVS_flow_sample_collector_set", node), "id");

    struct ovsrec_flow_sample_collector_set *s =
        ovsrec_flow_sample_collector_set_index_init_row(
        ovsrec_flow_sample_collector_set_by_id);

    ovsrec_flow_sample_collector_set_index_set_id(s, debug_collector_set);
    ovsrec_flow_sample_collector_set_index_set_bridge(s, br_int);
    if (!ovsrec_flow_sample_collector_set_index_find(
        ovsrec_flow_sample_collector_set_by_id, s)) {
        ovsrec_flow_sample_collector_set_index_destroy_row(s);
        return;
    }
    ovsrec_flow_sample_collector_set_index_destroy_row(s);

    debug->collector_set_id = debug_collector_set;
    debug->obs_domain_id = smap_get_uint(&sb_global->options,
                                         "debug_drop_domain_id", 0);
}

static enum engine_input_handler_result
lflow_output_sb_mac_binding_handler(struct engine_node *node, void *data)
{
    struct ovsdb_idl_index *sbrec_port_binding_by_name =
        engine_ovsdb_node_get_index(
                engine_get_input("SB_port_binding", node),
                "name");

    const struct sbrec_mac_binding_table *mac_binding_table =
        EN_OVSDB_GET(engine_get_input("SB_mac_binding", node));

    struct ed_type_runtime_data *rt_data =
        engine_get_input_data("runtime_data", node);
    const struct hmap *local_datapaths = &rt_data->local_datapaths;

    struct ed_type_lflow_output *lfo = data;

    lflow_handle_changed_mac_bindings(sbrec_port_binding_by_name,
            mac_binding_table, local_datapaths, &lfo->flow_table);

    return EN_HANDLED_UPDATED;
}

static enum engine_input_handler_result
lflow_output_sb_static_mac_binding_handler(struct engine_node *node,
                                           void *data)
{
    struct ovsdb_idl_index *sbrec_port_binding_by_name =
        engine_ovsdb_node_get_index(
                engine_get_input("SB_port_binding", node),
                "name");

    const struct sbrec_static_mac_binding_table *smb_table =
        EN_OVSDB_GET(engine_get_input("SB_static_mac_binding", node));

    struct ed_type_runtime_data *rt_data =
        engine_get_input_data("runtime_data", node);
    const struct hmap *local_datapaths = &rt_data->local_datapaths;

    struct ed_type_lflow_output *lfo = data;

    lflow_handle_changed_static_mac_bindings(sbrec_port_binding_by_name,
        smb_table, local_datapaths, &lfo->flow_table);

    return EN_HANDLED_UPDATED;
}

static enum engine_input_handler_result
lflow_output_sb_multicast_group_handler(struct engine_node *node, void *data)
{
    struct ed_type_lflow_output *lfo = data;

    struct lflow_ctx_in l_ctx_in;
    struct lflow_ctx_out l_ctx_out;
    init_lflow_ctx(node, lfo, &l_ctx_in, &l_ctx_out);
    if (!lflow_handle_changed_mc_groups(&l_ctx_in, &l_ctx_out)) {
        return EN_UNHANDLED;
    }

    return EN_HANDLED_UPDATED;
}

static enum engine_input_handler_result
lflow_output_sb_port_binding_handler(struct engine_node *node, void *data)
{
    struct ed_type_lflow_output *lfo = data;

    struct lflow_ctx_in l_ctx_in;
    struct lflow_ctx_out l_ctx_out;
    init_lflow_ctx(node, lfo, &l_ctx_in, &l_ctx_out);
    if (!lflow_handle_changed_port_bindings(&l_ctx_in, &l_ctx_out)) {
        return EN_UNHANDLED;
    }

    return EN_HANDLED_UPDATED;
}

static enum engine_input_handler_result
lflow_output_addr_sets_handler(struct engine_node *node, void *data)
{
    struct ed_type_addr_sets *as_data =
        engine_get_input_data("addr_sets", node);

    struct ed_type_lflow_output *fo = data;

    struct lflow_ctx_in l_ctx_in;
    struct lflow_ctx_out l_ctx_out;
    init_lflow_ctx(node, fo, &l_ctx_in, &l_ctx_out);

    bool changed;
    const char *ref_name;
    enum engine_input_handler_result result = EN_HANDLED_UNCHANGED;

    if (!as_data->change_tracked) {
        return EN_UNHANDLED;
    }

    SSET_FOR_EACH (ref_name, &as_data->deleted) {
        if (!objdep_mgr_handle_change(l_ctx_out.lflow_deps_mgr,
                                      OBJDEP_TYPE_ADDRSET, ref_name,
                                      lflow_handle_changed_ref,
                                      l_ctx_out.objs_processed,
                                      &l_ctx_in, &l_ctx_out, &changed)) {
            return EN_UNHANDLED;
        }
        if (changed) {
            result = EN_HANDLED_UPDATED;
        }
    }
    struct shash_node *shash_node;
    SHASH_FOR_EACH (shash_node, &as_data->updated) {
        struct addr_set_diff *as_diff = shash_node->data;
        if (!lflow_handle_addr_set_update(shash_node->name, as_diff, &l_ctx_in,
                                          &l_ctx_out, &changed)) {
            VLOG_DBG("Can't incrementally handle the change of address set %s."
                     " Reprocess related lflows.", shash_node->name);
            if (!objdep_mgr_handle_change(l_ctx_out.lflow_deps_mgr,
                                          OBJDEP_TYPE_ADDRSET,
                                          shash_node->name,
                                          lflow_handle_changed_ref,
                                          l_ctx_out.objs_processed,
                                          &l_ctx_in, &l_ctx_out, &changed)) {
                return EN_UNHANDLED;
            }
        }
        if (changed) {
            result = EN_HANDLED_UPDATED;
        }
    }
    SSET_FOR_EACH (ref_name, &as_data->new) {
        if (!objdep_mgr_handle_change(l_ctx_out.lflow_deps_mgr,
                                      OBJDEP_TYPE_ADDRSET, ref_name,
                                      lflow_handle_changed_ref,
                                      l_ctx_out.objs_processed,
                                      &l_ctx_in, &l_ctx_out, &changed)) {
            return EN_UNHANDLED;
        }
        if (changed) {
            result = EN_HANDLED_UPDATED;
        }
    }

    return result;
}

static enum engine_input_handler_result
lflow_output_port_groups_handler(struct engine_node *node, void *data)
{
    struct ed_type_port_groups *pg_data =
        engine_get_input_data("port_groups", node);

    struct ed_type_lflow_output *fo = data;

    struct lflow_ctx_in l_ctx_in;
    struct lflow_ctx_out l_ctx_out;
    init_lflow_ctx(node, fo, &l_ctx_in, &l_ctx_out);

    bool changed;
    const char *ref_name;
    enum engine_input_handler_result result = EN_HANDLED_UNCHANGED;

    if (!pg_data->change_tracked) {
        return EN_UNHANDLED;
    }

    SSET_FOR_EACH (ref_name, &pg_data->deleted) {
        if (!objdep_mgr_handle_change(l_ctx_out.lflow_deps_mgr,
                                      OBJDEP_TYPE_PORTGROUP, ref_name,
                                      lflow_handle_changed_ref,
                                      l_ctx_out.objs_processed,
                                      &l_ctx_in, &l_ctx_out, &changed)) {
            return EN_UNHANDLED;
        }
        if (changed) {
            result = EN_HANDLED_UPDATED;
        }
    }
    SSET_FOR_EACH (ref_name, &pg_data->updated) {
        if (!objdep_mgr_handle_change(l_ctx_out.lflow_deps_mgr,
                                      OBJDEP_TYPE_PORTGROUP, ref_name,
                                      lflow_handle_changed_ref,
                                      l_ctx_out.objs_processed,
                                      &l_ctx_in, &l_ctx_out, &changed)) {
            return EN_UNHANDLED;
        }
        if (changed) {
            result = EN_HANDLED_UPDATED;
        }
    }
    SSET_FOR_EACH (ref_name, &pg_data->new) {
        if (!objdep_mgr_handle_change(l_ctx_out.lflow_deps_mgr,
                                      OBJDEP_TYPE_PORTGROUP, ref_name,
                                      lflow_handle_changed_ref,
                                      l_ctx_out.objs_processed,
                                      &l_ctx_in, &l_ctx_out, &changed)) {
            return EN_UNHANDLED;
        }
        if (changed) {
            result = EN_HANDLED_UPDATED;
        }
    }

    return result;
}

static enum engine_input_handler_result
lflow_output_template_vars_handler(struct engine_node *node, void *data)
{
    struct ed_type_template_vars *tv_data =
        engine_get_input_data("template_vars", node);

    struct ed_type_lflow_output *fo = data;
    struct lflow_ctx_out l_ctx_out;
    struct lflow_ctx_in l_ctx_in;
    init_lflow_ctx(node, fo, &l_ctx_in, &l_ctx_out);

    const char *res_name;
    bool changed;
    enum engine_input_handler_result result = EN_HANDLED_UNCHANGED;

    if (!tv_data->change_tracked) {
        return EN_UNHANDLED;
    }

    SSET_FOR_EACH (res_name, &tv_data->deleted) {
        if (!objdep_mgr_handle_change(l_ctx_out.lflow_deps_mgr,
                                      OBJDEP_TYPE_TEMPLATE,
                                      res_name, lflow_handle_changed_ref,
                                      l_ctx_out.objs_processed,
                                      &l_ctx_in, &l_ctx_out, &changed)) {
            return EN_UNHANDLED;
        }
        if (changed) {
            result = EN_HANDLED_UPDATED;
        }
    }
    SSET_FOR_EACH (res_name, &tv_data->updated) {
        if (!objdep_mgr_handle_change(l_ctx_out.lflow_deps_mgr,
                                      OBJDEP_TYPE_TEMPLATE,
                                      res_name, lflow_handle_changed_ref,
                                      l_ctx_out.objs_processed,
                                      &l_ctx_in, &l_ctx_out, &changed)) {
            return EN_UNHANDLED;
        }
        if (changed) {
            result = EN_HANDLED_UPDATED;
        }
    }
    SSET_FOR_EACH (res_name, &tv_data->new) {
        if (!objdep_mgr_handle_change(l_ctx_out.lflow_deps_mgr,
                                      OBJDEP_TYPE_TEMPLATE,
                                      res_name, lflow_handle_changed_ref,
                                      l_ctx_out.objs_processed,
                                      &l_ctx_in, &l_ctx_out, &changed)) {
            return EN_UNHANDLED;
        }
        if (changed) {
            result = EN_HANDLED_UPDATED;
        }
    }

    return result;
}

static enum engine_input_handler_result
lflow_output_runtime_data_handler(struct engine_node *node,
                                  void *data OVS_UNUSED)
{
    struct ed_type_runtime_data *rt_data =
        engine_get_input_data("runtime_data", node);

    /* There is no tracked data. Fall back to full recompute of
     * flow_output. */
    if (!rt_data->tracked) {
        return EN_UNHANDLED;
    }

    struct hmap *tracked_dp_bindings = &rt_data->tracked_dp_bindings;
    if (hmap_is_empty(tracked_dp_bindings)) {
        return rt_data->local_lports_changed
               ? EN_HANDLED_UPDATED
               : EN_HANDLED_UNCHANGED;
    }

    struct lflow_ctx_in l_ctx_in;
    struct lflow_ctx_out l_ctx_out;
    struct ed_type_lflow_output *fo = data;
    init_lflow_ctx(node, fo, &l_ctx_in, &l_ctx_out);

    struct tracked_datapath *tdp;
    HMAP_FOR_EACH (tdp, node, tracked_dp_bindings) {
        if (tdp->tracked_type == TRACKED_RESOURCE_NEW) {
            if (!lflow_add_flows_for_datapath(tdp->dp, &l_ctx_in,
                                              &l_ctx_out)) {
                return EN_UNHANDLED;
            }
        }
        struct shash_node *shash_node;
        SHASH_FOR_EACH (shash_node, &tdp->lports) {
            struct tracked_lport *lport = shash_node->data;
            if (!lflow_handle_flows_for_lport(
                    lport->pb, &l_ctx_in, &l_ctx_out,
                    lport->tracked_type == TRACKED_RESOURCE_REMOVED)) {
                return EN_UNHANDLED;
            }
        }
    }

    return EN_HANDLED_UPDATED;
}

static enum engine_input_handler_result
lflow_output_lb_data_handler(struct engine_node *node, void *data)
{
    struct ed_type_lflow_output *fo = data;
    struct ed_type_lb_data *lb_data = engine_get_input_data("lb_data", node);

    if (!lb_data->change_tracked) {
        return EN_UNHANDLED;
    }

    struct lflow_ctx_in l_ctx_in;
    struct lflow_ctx_out l_ctx_out;
    init_lflow_ctx(node, fo, &l_ctx_in, &l_ctx_out);

    bool handled = lflow_handle_changed_lbs(&l_ctx_in, &l_ctx_out,
                                            &lb_data->deleted,
                                            &lb_data->updated,
                                            &lb_data->new);

    return handled ? EN_HANDLED_UPDATED : EN_UNHANDLED;
}

static enum engine_input_handler_result
lflow_output_sb_fdb_handler(struct engine_node *node, void *data)
{
    struct ed_type_lflow_output *fo = data;
    struct lflow_ctx_in l_ctx_in;
    struct lflow_ctx_out l_ctx_out;
    init_lflow_ctx(node, fo, &l_ctx_in, &l_ctx_out);

    bool handled = lflow_handle_changed_fdbs(&l_ctx_in, &l_ctx_out);

    return handled ? EN_HANDLED_UPDATED : EN_UNHANDLED;
}

static enum engine_input_handler_result
lflow_output_sb_meter_handler(struct engine_node *node, void *data)
{
    struct ed_type_lflow_output *fo = data;
    const struct sbrec_meter_table *meter_table =
        EN_OVSDB_GET(engine_get_input("SB_meter", node));

    const struct sbrec_meter *iter;
    SBREC_METER_TABLE_FOR_EACH_TRACKED (iter, meter_table) {
        if (ovn_extend_table_desired_lookup_by_name(&fo->meter_table,
                                                    iter->name)) {
            return EN_HANDLED_UPDATED;
        }
    }

    return EN_HANDLED_UNCHANGED;
}

struct ed_type_pflow_output {
    /* Desired physical flows. */
    struct ovn_desired_flow_table flow_table;
    /* Drop debugging options. */
    struct physical_debug debug;
};

static void
parse_encap_ips(const struct ovsrec_open_vswitch_table *ovs_table,
                size_t *n_encap_ips, const char * **encap_ips)
{
    const struct ovsrec_open_vswitch *cfg =
        ovsrec_open_vswitch_table_first(ovs_table);
    const char *chassis_id = get_ovs_chassis_id(ovs_table);
    const char *encap_ips_str =
        get_chassis_external_id_value(&cfg->external_ids, chassis_id,
                                      "ovn-encap-ip", NULL);
    struct sset encap_ip_set;
    sset_from_delimited_string(&encap_ip_set, encap_ips_str,  ",");

    /* Sort the ips so that their index is deterministic. */
    *encap_ips = sset_sort(&encap_ip_set);

    /* Copy the strings so that we can destroy the sset. */
    for (size_t i = 0; (*encap_ips)[i]; i++) {
        (*encap_ips)[i] = xstrdup((*encap_ips)[i]);
    }
    *n_encap_ips = sset_count(&encap_ip_set);
    sset_destroy(&encap_ip_set);

    /* Move the default encap IP, if configured, to the first so that it will
     * have index 0, because we use index as encap-id and we need 0 to be the
     * default encap-id. */
    const char *encap_ip_default =
        get_chassis_external_id_value(&cfg->external_ids, chassis_id,
                                      "ovn-encap-ip-default", NULL);
    if (encap_ip_default) {
        for (size_t i = 0; i < *n_encap_ips; i++) {
            if (!strcmp(encap_ip_default, (*encap_ips)[i])) {
                const char *tmp = (*encap_ips)[0];
                (*encap_ips)[0] = (*encap_ips)[i];
                (*encap_ips)[i] = tmp;
                break;
            }
        }
    }
}

struct ed_type_evpn_vtep_binding {
    /* Contains 'struct evpn_binding'. */
    struct hmap bindings;
    /* Contains pointers to 'struct evpn_binding'. */
    struct hmapx updated_bindings;
    /* Contains 'flow_uuid' from removed 'struct evpn_binding'. */
    struct uuidset removed_bindings;
    /* Contains 'struct evpn_datapath'. */
    struct hmap datapaths;
    /* Contains 'struct evpn_multicast_group'. */
    struct hmap multicast_groups;
    /* Contains pointers to 'struct evpn_multicast_group'. */
    struct hmapx updated_multicast_groups;
    /* Contains 'flow_uuid' from removed 'struct evpn_multicast_group'. */
    struct uuidset removed_multicast_groups;
    /* Contains 'struct tnlid_node". */
    struct hmap tunnel_keys;
};

struct ed_type_evpn_fdb {
    /* Contains 'struct evpn_fdb'. */
    struct hmap fdbs;
    /* Contains pointers to 'struct evpn_fdb'. */
    struct hmapx updated_fdbs;
    /* Contains 'flow_uuid' from removed 'struct evpn_fdb'. */
    struct uuidset removed_fdbs;
};

struct ed_type_evpn_arp {
    /* Contains 'struct evpn_arp'. */
    struct hmap arps;
    /* Contains pointers to 'struct evpn_arp'. */
    struct hmapx updated_arps;
    /* Contains 'flow_uuid' from removed 'struct evpn_arps'. */
    struct uuidset removed_arps;
};

static void init_physical_ctx(struct engine_node *node,
                              struct ed_type_runtime_data *rt_data,
                              struct ed_type_non_vif_data *non_vif_data,
                              struct physical_ctx *p_ctx)
{
    struct ovsdb_idl_index *sbrec_port_binding_by_name =
        engine_ovsdb_node_get_index(
                engine_get_input("SB_port_binding", node),
                "name");

    struct ovsdb_idl_index *sbrec_port_binding_by_datapath =
        engine_ovsdb_node_get_index(
                engine_get_input("SB_port_binding", node),
                "datapath");

    const struct sbrec_multicast_group_table *multicast_group_table =
        EN_OVSDB_GET(engine_get_input("SB_multicast_group", node));

    const struct sbrec_port_binding_table *port_binding_table =
        EN_OVSDB_GET(engine_get_input("SB_port_binding", node));

    const struct sbrec_chassis_table *chassis_table =
        EN_OVSDB_GET(engine_get_input("SB_chassis", node));

    const struct ed_type_mff_ovn_geneve *ed_mff_ovn_geneve =
        engine_get_input_data("mff_ovn_geneve", node);

    const struct ovsrec_interface_table *ovs_interface_table =
        EN_OVSDB_GET(engine_get_input("if_status_mgr", node));

    const struct ovsrec_open_vswitch_table *ovs_table =
        EN_OVSDB_GET(engine_get_input("OVS_open_vswitch", node));
    const struct ovsrec_bridge_table *bridge_table =
        EN_OVSDB_GET(engine_get_input("OVS_bridge", node));
    const struct ovsrec_bridge *br_int = get_br_int(bridge_table, ovs_table);
    const char *chassis_id = get_ovs_chassis_id(ovs_table);
    const struct sbrec_chassis *chassis = NULL;
    struct ovsdb_idl_index *sbrec_chassis_by_name =
        engine_ovsdb_node_get_index(
                engine_get_input("SB_chassis", node),
                "name");
    if (chassis_id) {
        chassis = chassis_lookup_by_name(sbrec_chassis_by_name, chassis_id);
    }

    ovs_assert(br_int && chassis);

    struct ed_type_ct_zones *ct_zones_data =
        engine_get_input_data("ct_zones", node);
    struct shash *ct_zones = &ct_zones_data->ctx.current;

    struct ed_type_northd_options *n_opts =
        engine_get_input_data("northd_options", node);

    struct ed_type_evpn_vtep_binding *eb_data =
        engine_get_input_data("evpn_vtep_binding", node);

    struct ed_type_evpn_fdb *efdb_data =
        engine_get_input_data("evpn_fdb", node);

    struct ed_type_evpn_arp *earp_data =
        engine_get_input_data("evpn_arp", node);

    parse_encap_ips(ovs_table, &p_ctx->n_encap_ips, &p_ctx->encap_ips);
    p_ctx->sbrec_port_binding_by_name = sbrec_port_binding_by_name;
    p_ctx->sbrec_port_binding_by_datapath = sbrec_port_binding_by_datapath;
    p_ctx->port_binding_table = port_binding_table;
    p_ctx->ovs_interface_table = ovs_interface_table;
    p_ctx->mc_group_table = multicast_group_table;
    p_ctx->br_int = br_int;
    p_ctx->chassis_table = chassis_table;
    p_ctx->chassis = chassis;
    p_ctx->active_tunnels = &rt_data->active_tunnels;
    p_ctx->local_datapaths = &rt_data->local_datapaths;
    p_ctx->ct_zones = ct_zones;
    p_ctx->mff_ovn_geneve = ed_mff_ovn_geneve->mff_ovn_geneve;
    p_ctx->local_bindings = &rt_data->lbinding_data.bindings;
    p_ctx->patch_ofports = &non_vif_data->patch_ofports;
    p_ctx->chassis_tunnels = &non_vif_data->chassis_tunnels;
    p_ctx->always_tunnel = n_opts->always_tunnel;
    p_ctx->evpn_bindings = &eb_data->bindings;
    p_ctx->evpn_multicast_groups = &eb_data->multicast_groups;
    p_ctx->evpn_fdbs = &efdb_data->fdbs;
    p_ctx->evpn_arps = &earp_data->arps;

    struct controller_engine_ctx *ctrl_ctx = engine_get_context()->client_ctx;
    p_ctx->if_mgr = ctrl_ctx->if_mgr;

    pflow_output_get_debug(node, &p_ctx->debug);
    sset_init(&p_ctx->reprocessed_pbs);
}

static void
destroy_physical_ctx(struct physical_ctx *p_ctx)
{
    for (size_t i = 0; i < p_ctx->n_encap_ips; i++) {
        free((char *)(p_ctx->encap_ips[i]));
    }
    free(p_ctx->encap_ips);
    sset_destroy(&p_ctx->reprocessed_pbs);
}

static void *
en_pflow_output_init(struct engine_node *node OVS_UNUSED,
                             struct engine_arg *arg OVS_UNUSED)
{
    struct ed_type_pflow_output *data = xzalloc(sizeof *data);
    ovn_desired_flow_table_init(&data->flow_table);
    return data;
}

static void
en_pflow_output_cleanup(void *data OVS_UNUSED)
{
    struct ed_type_pflow_output *pfo = data;
    ovn_desired_flow_table_destroy(&pfo->flow_table);
}

static enum engine_node_state
en_pflow_output_run(struct engine_node *node, void *data)
{
    struct ed_type_pflow_output *pfo = data;
    struct ovn_desired_flow_table *pflow_table = &pfo->flow_table;
    static bool first_run = true;
    if (first_run) {
        first_run = false;
    } else {
        ovn_desired_flow_table_clear(pflow_table);
    }

    struct ed_type_runtime_data *rt_data =
        engine_get_input_data("runtime_data", node);
    struct ed_type_non_vif_data *non_vif_data =
        engine_get_input_data("non_vif_data", node);

    struct physical_ctx p_ctx;
    init_physical_ctx(node, rt_data, non_vif_data, &p_ctx);
    physical_run(&p_ctx, pflow_table);
    destroy_physical_ctx(&p_ctx);

    return EN_UPDATED;
}

static enum engine_input_handler_result
pflow_output_if_status_mgr_handler(struct engine_node *node,
                                   void *data)
{
    struct ed_type_pflow_output *pfo = data;
    struct ed_type_runtime_data *rt_data =
        engine_get_input_data("runtime_data", node);
    struct ed_type_non_vif_data *non_vif_data =
        engine_get_input_data("non_vif_data", node);
    struct ed_type_if_status_mgr *if_mgr_data =
        engine_get_input_data("if_status_mgr", node);

    struct physical_ctx p_ctx;
    init_physical_ctx(node, rt_data, non_vif_data, &p_ctx);

    const struct ovsrec_interface *iface;
    enum engine_input_handler_result result = EN_HANDLED_UNCHANGED;
    OVSREC_INTERFACE_TABLE_FOR_EACH_TRACKED (iface, if_mgr_data->iface_table) {
        const char *iface_id = smap_get(&iface->external_ids, "iface-id");
        if (!iface_id) {
            continue;
        }

        const struct sbrec_port_binding *pb = lport_lookup_by_name(
            p_ctx.sbrec_port_binding_by_name, iface_id);
        if (!pb) {
            continue;
        }
        if (pb->n_additional_chassis) {
            /* Update flows for all ports in datapath. */
            physical_multichassis_reprocess(pb, &p_ctx, &pfo->flow_table);
        } else {
            /* If any multichassis ports, update flows for the port. */
            bool removed = sbrec_port_binding_is_deleted(pb);
            if (!physical_handle_flows_for_lport(pb, removed, &p_ctx,
                                                 &pfo->flow_table)) {
                destroy_physical_ctx(&p_ctx);
                return EN_UNHANDLED;
            }
        }
        result = EN_HANDLED_UPDATED;
    }
    destroy_physical_ctx(&p_ctx);
    return result;
}

static enum engine_input_handler_result
pflow_output_sb_port_binding_handler(struct engine_node *node,
                                     void *data)
{
    struct ed_type_runtime_data *rt_data =
        engine_get_input_data("runtime_data", node);
    struct ed_type_non_vif_data *non_vif_data =
        engine_get_input_data("non_vif_data", node);

    struct ed_type_pflow_output *pfo = data;

    struct physical_ctx p_ctx;
    init_physical_ctx(node, rt_data, non_vif_data, &p_ctx);

    /* We handle port-binding changes for physical flow processing
     * only. flow_output runtime data handler takes care of processing
     * logical flows for any port binding changes.
     */
    const struct sbrec_port_binding *pb;
    SBREC_PORT_BINDING_TABLE_FOR_EACH_TRACKED (pb, p_ctx.port_binding_table) {
        /* Trigger a full recompute if type column is updated. */
        if (sbrec_port_binding_is_updated(pb, SBREC_PORT_BINDING_COL_TYPE)) {
            destroy_physical_ctx(&p_ctx);
            return EN_UNHANDLED;
        }
        bool removed = sbrec_port_binding_is_deleted(pb);
        if (!physical_handle_flows_for_lport(pb, removed, &p_ctx,
                                             &pfo->flow_table)) {
            destroy_physical_ctx(&p_ctx);
            return EN_UNHANDLED;
        }
    }

    destroy_physical_ctx(&p_ctx);
    return EN_HANDLED_UPDATED;
}

static enum engine_input_handler_result
pflow_output_sb_multicast_group_handler(struct engine_node *node, void *data)
{
    struct ed_type_runtime_data *rt_data =
        engine_get_input_data("runtime_data", node);
    struct ed_type_non_vif_data *non_vif_data =
        engine_get_input_data("non_vif_data", node);

    struct ed_type_pflow_output *pfo = data;

    struct physical_ctx p_ctx;
    init_physical_ctx(node, rt_data, non_vif_data, &p_ctx);

    physical_handle_mc_group_changes(&p_ctx, &pfo->flow_table);

    destroy_physical_ctx(&p_ctx);
    return EN_HANDLED_UPDATED;
}

static enum engine_input_handler_result
pflow_output_runtime_data_handler(struct engine_node *node, void *data)
{
    struct ed_type_runtime_data *rt_data =
        engine_get_input_data("runtime_data", node);
    struct ed_type_non_vif_data *non_vif_data =
        engine_get_input_data("non_vif_data", node);

    /* There is no tracked data. Fall back to full recompute of
     * pflow_output. */
    if (!rt_data->tracked) {
        return EN_UNHANDLED;
    }

    struct hmap *tracked_dp_bindings = &rt_data->tracked_dp_bindings;
    if (hmap_is_empty(tracked_dp_bindings)) {
        return EN_HANDLED_UNCHANGED;
    }

    struct ed_type_pflow_output *pfo = data;

    struct physical_ctx p_ctx;
    init_physical_ctx(node, rt_data, non_vif_data, &p_ctx);

    struct tracked_datapath *tdp;
    HMAP_FOR_EACH (tdp, node, tracked_dp_bindings) {
        if (tdp->tracked_type != TRACKED_RESOURCE_UPDATED) {
            /* Fall back to full recompute when a local datapath
             * is added or deleted. */
            destroy_physical_ctx(&p_ctx);
            return EN_UNHANDLED;
        }

        struct shash_node *shash_node;
        SHASH_FOR_EACH (shash_node, &tdp->lports) {
            struct tracked_lport *lport = shash_node->data;
            bool removed =
                lport->tracked_type == TRACKED_RESOURCE_REMOVED ? true: false;
            if (!physical_handle_flows_for_lport(lport->pb, removed, &p_ctx,
                                                 &pfo->flow_table)) {
                destroy_physical_ctx(&p_ctx);
                return EN_UNHANDLED;
            }
        }
    }

    destroy_physical_ctx(&p_ctx);
    return EN_HANDLED_UPDATED;
}

static enum engine_input_handler_result
pflow_output_ct_zones_handler(struct engine_node *node OVS_UNUSED,
                                    void *data OVS_UNUSED)
{
    struct ed_type_ct_zones *ct_zones_data =
        engine_get_input_data("ct_zones", node);

    /* If ct_zones engine node was recomputed, then fall back to full
     * recompute of pflow_output.  Otherwise there is no need to do
     * anything for the following reasons:
     *   - When an lport is claimed, ct zone handler for the
     *     runtime_data allocates the zone id for the lport (and it is
     *     saved in the br-int external_ids).
     *   - pflow_output handler for the runtime_data adds the physical
     *     flows for the claimed lport.
     * */
    return ct_zones_data->recomputed ? EN_UNHANDLED : EN_HANDLED_UNCHANGED;
}

static enum engine_input_handler_result
pflow_output_activated_ports_handler(struct engine_node *node, void *data)
{
    struct ed_type_activated_ports *ap =
        engine_get_input_data("activated_ports", node);
    if (!ap->activated_ports) {
        return EN_HANDLED_UNCHANGED;
    }

    struct ed_type_pflow_output *pfo = data;
    struct ed_type_runtime_data *rt_data =
        engine_get_input_data("runtime_data", node);
    struct ed_type_non_vif_data *non_vif_data =
        engine_get_input_data("non_vif_data", node);

    struct physical_ctx p_ctx;
    init_physical_ctx(node, rt_data, non_vif_data, &p_ctx);

    struct activated_port *pp;
    LIST_FOR_EACH (pp, list, ap->activated_ports) {
        struct ovsdb_idl_index *sbrec_datapath_binding_by_key =
            engine_ovsdb_node_get_index(
                    engine_get_input("SB_datapath_binding", node),
                    "key");
        struct ovsdb_idl_index *sbrec_port_binding_by_key =
            engine_ovsdb_node_get_index(
                    engine_get_input("SB_port_binding", node),
                    "key");
        const struct sbrec_port_binding *pb = lport_lookup_by_key(
            sbrec_datapath_binding_by_key, sbrec_port_binding_by_key,
            pp->dp_key, pp->port_key);
        if (pb) {
            if (!physical_handle_flows_for_lport(pb, false, &p_ctx,
                                                 &pfo->flow_table)) {
                destroy_physical_ctx(&p_ctx);
                return EN_UNHANDLED;
            }
            tag_port_as_activated_in_engine(pp);
        }
    }
    destroy_physical_ctx(&p_ctx);
    return EN_HANDLED_UPDATED;
}

static enum engine_input_handler_result
pflow_output_debug_handler(struct engine_node *node, void *data)
{
    struct ed_type_pflow_output *pfo = data;
    struct physical_debug debug;

    pflow_output_get_debug(node, &debug);

    if (pfo->debug.collector_set_id != debug.collector_set_id ||
        pfo->debug.obs_domain_id != debug.obs_domain_id) {
        pfo->debug = debug;
        return EN_UNHANDLED;
    }
    return EN_HANDLED_UPDATED;
}
static enum engine_input_handler_result
pflow_output_evpn_binding_handler(struct engine_node *node, void *data)
{
    struct ed_type_pflow_output *pfo = data;
    struct ed_type_runtime_data *rt_data =
        engine_get_input_data("runtime_data", node);
    struct ed_type_non_vif_data *non_vif_data =
        engine_get_input_data("non_vif_data", node);
    struct ed_type_evpn_vtep_binding *eb_data =
        engine_get_input_data("evpn_vtep_binding", node);

    struct physical_ctx ctx;
    init_physical_ctx(node, rt_data, non_vif_data, &ctx);

    physical_handle_evpn_binding_changes(&ctx, &pfo->flow_table,
                                         &eb_data->updated_bindings,
                                         &eb_data->updated_multicast_groups,
                                         &eb_data->removed_bindings,
                                         &eb_data->removed_multicast_groups);
    destroy_physical_ctx(&ctx);
    return EN_HANDLED_UPDATED;
}

static enum engine_input_handler_result
pflow_output_fdb_handler(struct engine_node *node, void *data)
{
    struct ed_type_pflow_output *pfo = data;
    struct ed_type_evpn_fdb *ef_data =
        engine_get_input_data("evpn_fdb", node);

    physical_handle_evpn_fdb_changes(&pfo->flow_table, &ef_data->updated_fdbs,
                                     &ef_data->removed_fdbs);
    return EN_HANDLED_UPDATED;
}

static enum engine_input_handler_result
pflow_output_arp_handler(struct engine_node *node, void *data)
{
    struct ed_type_pflow_output *pfo = data;
    struct ed_type_runtime_data *rt_data =
        engine_get_input_data("runtime_data", node);
    struct ed_type_evpn_arp *ea_data =
        engine_get_input_data("evpn_arp", node);

    physical_handle_evpn_arp_changes(&rt_data->local_datapaths,
                                     &pfo->flow_table,
                                     &ea_data->updated_arps,
                                     &ea_data->removed_arps);
    return EN_HANDLED_UPDATED;
}

static void *
en_controller_output_init(struct engine_node *node OVS_UNUSED,
                          struct engine_arg *arg OVS_UNUSED)
{
    return NULL;
}

static void
en_controller_output_cleanup(void *data OVS_UNUSED)
{

}

static enum engine_node_state
en_controller_output_run(struct engine_node *node OVS_UNUSED,
                         void *data OVS_UNUSED)
{
    return EN_UPDATED;
}

static enum engine_input_handler_result
controller_output_pflow_output_handler(struct engine_node *node OVS_UNUSED,
                                       void *data OVS_UNUSED)
{
    return EN_HANDLED_UPDATED;
}

static enum engine_input_handler_result
controller_output_lflow_output_handler(struct engine_node *node OVS_UNUSED,
                                       void *data OVS_UNUSED)
{
    return EN_HANDLED_UPDATED;
}

static enum engine_input_handler_result
controller_output_mac_cache_handler(struct engine_node *node OVS_UNUSED,
                                    void *data OVS_UNUSED)
{
    return EN_HANDLED_UPDATED;
}

static enum engine_input_handler_result
controller_output_bfd_chassis_handler(struct engine_node *node OVS_UNUSED,
                                    void *data OVS_UNUSED)
{
    return EN_HANDLED_UPDATED;
}

static enum engine_input_handler_result
controller_output_acl_id_handler(struct engine_node *node OVS_UNUSED,
                                    void *data OVS_UNUSED)
{
    return EN_HANDLED_UPDATED;
}

static enum engine_input_handler_result
controller_output_route_exchange_handler(struct engine_node *node OVS_UNUSED,
                                         void *data OVS_UNUSED)
{
    return EN_HANDLED_UPDATED;
}

static enum engine_input_handler_result
controller_output_garp_rarp_handler(struct engine_node *node OVS_UNUSED,
                                    void *data OVS_UNUSED)
{
    return EN_HANDLED_UPDATED;
}

/* Handles sbrec_chassis changes.
 * If a new chassis is added or removed return false, so that
 * flows are recomputed.  For any updates, there is no need for
 * any flow computation.  Encap changes will also result in
 * sbrec_chassis changes, but we handle encap changes separately.
 */
static enum engine_input_handler_result
pflow_lflow_output_sb_chassis_handler(struct engine_node *node,
                                      void *data OVS_UNUSED)
{
    const struct sbrec_chassis_table *chassis_table =
        EN_OVSDB_GET(engine_get_input("SB_chassis", node));

    const struct sbrec_chassis *ch;
    SBREC_CHASSIS_TABLE_FOR_EACH_TRACKED (ch, chassis_table) {
        if (sbrec_chassis_is_deleted(ch) || sbrec_chassis_is_new(ch)) {
            return EN_UNHANDLED;
        }
    }

    return EN_HANDLED_UNCHANGED;
}

struct ed_type_route {
    /* Contains struct tracked_datapath entries for local datapaths subject to
     * route exchange. */
    struct hmap tracked_route_datapaths;

    /* Contains the tracked_ports that in the last run were bound locally. */
    struct sset tracked_ports_local;

    /* Contains the tracked_ports that in the last run were not bound
     * locally. */
    struct sset tracked_ports_remote;

    /* Contains all the currently configured dynamic-routing-port-name values
     * on all datapaths.
     */
    struct sset filtered_ports;

    /* Contains struct advertise_datapath_entry */
    struct hmap announce_routes;

    struct ovsdb_idl *ovnsb_idl;
};

static enum engine_node_state
en_route_run(struct engine_node *node, void *data)
{
    struct ed_type_route *re_data = data;

    const struct ovsrec_open_vswitch_table *ovs_table =
        EN_OVSDB_GET(engine_get_input("OVS_open_vswitch", node));
    const char *chassis_id = get_ovs_chassis_id(ovs_table);
    ovs_assert(chassis_id);

    struct ovsdb_idl_index *sbrec_chassis_by_name =
        engine_ovsdb_node_get_index(
                engine_get_input("SB_chassis", node),
                "name");
    const struct sbrec_chassis *chassis
        = chassis_lookup_by_name(sbrec_chassis_by_name, chassis_id);
    ovs_assert(chassis);

    struct ovsdb_idl_index *sbrec_port_binding_by_name =
        engine_ovsdb_node_get_index(
                engine_get_input("SB_port_binding", node),
                "name");
    struct ed_type_runtime_data *rt_data =
        engine_get_input_data("runtime_data", node);

    const struct sbrec_advertised_route_table *advertised_route_table =
        EN_OVSDB_GET(engine_get_input("SB_advertised_route", node));

    const struct ovsrec_open_vswitch *cfg
        = ovsrec_open_vswitch_table_first(ovs_table);
    const char *dynamic_routing_port_mapping =
        smap_get(&cfg->external_ids, "dynamic-routing-port-mapping");

    struct route_ctx_in r_ctx_in = {
        .advertised_route_table = advertised_route_table,
        .sbrec_port_binding_by_name = sbrec_port_binding_by_name,
        .chassis = chassis,
        .dynamic_routing_port_mapping = dynamic_routing_port_mapping,
        .local_datapaths = &rt_data->local_datapaths,
        .local_bindings = &rt_data->lbinding_data.bindings,
    };

    struct route_ctx_out r_ctx_out = {
        .tracked_re_datapaths = &re_data->tracked_route_datapaths,
        .tracked_ports_local = &re_data->tracked_ports_local,
        .filtered_ports = &re_data->filtered_ports,
        .tracked_ports_remote = &re_data->tracked_ports_remote,
        .announce_routes = &re_data->announce_routes,
    };

    route_cleanup(&re_data->announce_routes);
    tracked_datapaths_clear(r_ctx_out.tracked_re_datapaths);
    sset_clear(r_ctx_out.tracked_ports_local);
    sset_clear(r_ctx_out.tracked_ports_remote);
    sset_clear(r_ctx_out.filtered_ports);

    route_run(&r_ctx_in, &r_ctx_out);
    return EN_UPDATED;
}


static void *
en_route_init(struct engine_node *node OVS_UNUSED,
              struct engine_arg *arg)
{
    struct ed_type_route *data = xzalloc(sizeof *data);

    hmap_init(&data->tracked_route_datapaths);
    sset_init(&data->tracked_ports_local);
    sset_init(&data->tracked_ports_remote);
    sset_init(&data->filtered_ports);
    hmap_init(&data->announce_routes);
    data->ovnsb_idl = arg->sb_idl;

    return data;
}

static void
en_route_cleanup(void *data)
{
    struct ed_type_route *re_data = data;

    tracked_datapaths_destroy(&re_data->tracked_route_datapaths);
    sset_destroy(&re_data->tracked_ports_local);
    sset_destroy(&re_data->tracked_ports_remote);
    sset_destroy(&re_data->filtered_ports);
    route_cleanup(&re_data->announce_routes);
    hmap_destroy(&re_data->announce_routes);
}

static enum engine_input_handler_result
route_runtime_data_handler(struct engine_node *node, void *data)
{
    struct ed_type_route *re_data = data;
    struct ed_type_runtime_data *rt_data =
        engine_get_input_data("runtime_data", node);

    const struct ovsrec_open_vswitch_table *ovs_table =
        EN_OVSDB_GET(engine_get_input("OVS_open_vswitch", node));
    const char *chassis_id = get_ovs_chassis_id(ovs_table);
    ovs_assert(chassis_id);

    struct ovsdb_idl_index *sbrec_chassis_by_name =
        engine_ovsdb_node_get_index(
                engine_get_input("SB_chassis", node),
                "name");
    const struct sbrec_chassis *chassis
        = chassis_lookup_by_name(sbrec_chassis_by_name, chassis_id);
    ovs_assert(chassis);

    struct ovsdb_idl_index *sbrec_port_binding_by_name =
        engine_ovsdb_node_get_index(
                engine_get_input("SB_port_binding", node),
                "name");

    if (!rt_data->tracked) {
        return EN_UNHANDLED;
    }

    /* There are the following cases where we need to handle updates to
     * runtime_data:
     * 1. A datapath binding has changed that is already taking part in route
     *    exchange.
     * 2. A route-exchange relevant port went form local to remote or the
     *    other way round.
     * 3. A tracked_port went from local to remote or the other way round.
     * */
    struct tracked_datapath *t_dp;
    HMAP_FOR_EACH (t_dp, node, &rt_data->tracked_dp_bindings) {
        struct tracked_datapath *re_t_dp =
            tracked_datapath_find(&re_data->tracked_route_datapaths, t_dp->dp);

        if (re_t_dp) {
            /* XXX: Until we get I-P support for route exchange we need to
             * request recompute. */
            return EN_UNHANDLED;
        }

        struct shash_node *shash_node;
        SHASH_FOR_EACH (shash_node, &t_dp->lports) {
            struct tracked_lport *lport = shash_node->data;

            if (route_exchange_find_port(sbrec_port_binding_by_name, chassis,
                                         lport->pb, NULL)) {
                /* XXX: Until we get I-P support for route exchange we need to
                 * request recompute. */
                return EN_UNHANDLED;
            }

            /* When the port is removed we went from local to remote,
             * otherwise it's from remote to local. */
            struct sset *tracked_ports =
                lport->tracked_type == TRACKED_RESOURCE_REMOVED
                ? &re_data->tracked_ports_local
                : &re_data->tracked_ports_remote;

            const char *name = lport->pb->logical_port;
            if (sset_contains(tracked_ports, name)) {
                /* XXX: Until we get I-P support for route exchange we need to
                 * request recompute. */
                return EN_UNHANDLED;
            }

            /* If this logical port name is used to filter on which router
             * ports learning should happen then process the changes. */
            if (sset_find(&re_data->filtered_ports, name)) {
                /* XXX: Until we get I-P support for route exchange we need to
                * request recompute. */
                return EN_UNHANDLED;
            }

            const char *dp_name = smap_get(&lport->pb->options,
                                           "distributed-port");
            if (dp_name && sset_contains(tracked_ports, dp_name)) {
                /* XXX: Until we get I-P support for route exchange we need to
                 * request recompute. */
                return EN_UNHANDLED;
            }
        }
    }

    return EN_HANDLED_UNCHANGED;
}

static enum engine_input_handler_result
route_sb_port_binding_data_handler(struct engine_node *node, void *data)
{
    struct ed_type_route *re_data = data;
    const struct sbrec_port_binding_table *pb_table =
        EN_OVSDB_GET(engine_get_input("SB_port_binding", node));

    const struct ovsrec_open_vswitch_table *ovs_table =
        EN_OVSDB_GET(engine_get_input("OVS_open_vswitch", node));
    const char *chassis_id = get_ovs_chassis_id(ovs_table);
    ovs_assert(chassis_id);

    struct ovsdb_idl_index *sbrec_chassis_by_name =
        engine_ovsdb_node_get_index(
                engine_get_input("SB_chassis", node),
                "name");
    const struct sbrec_chassis *chassis
        = chassis_lookup_by_name(sbrec_chassis_by_name, chassis_id);
    ovs_assert(chassis);

    struct ovsdb_idl_index *sbrec_port_binding_by_name =
        engine_ovsdb_node_get_index(
                engine_get_input("SB_port_binding", node),
                "name");


    /* There are the following cases where we need to handle updates to the
     * port_binding table:
     * 1. The port_binding is part of a router datapath that already takes
     *    part in route exchange.
     * 2. The port_binding is now becoming part of route exchange.
     *
     * We do not need to handle port_bindings that are tracked_ports and switch
     * between being local and remote. This is handled as part of the
     * runtime_data handler. */
    const struct sbrec_port_binding *sbrec_pb;
    SBREC_PORT_BINDING_TABLE_FOR_EACH_TRACKED (sbrec_pb, pb_table) {
        struct tracked_datapath *re_t_dp =
            tracked_datapath_find(&re_data->tracked_route_datapaths,
                                  sbrec_pb->datapath);
        if (re_t_dp) {
            /* XXX: Until we get I-P support for route exchange we need to
             * request recompute. */
            return EN_UNHANDLED;
        }

        if (route_exchange_find_port(sbrec_port_binding_by_name,
                                     chassis, sbrec_pb, NULL)) {
            /* XXX: Until we get I-P support for route exchange we need to
             * request recompute. */
            return EN_UNHANDLED;
        }

        /* If this logical port name is used to filter on which router
         * ports learning should happen then process the changes. */
        if (sset_find(&re_data->filtered_ports, sbrec_pb->logical_port)) {
            /* XXX: Until we get I-P support for route exchange we need to
             * request recompute. */
            return EN_UNHANDLED;
        }
    }

    return EN_HANDLED_UNCHANGED;
}

static enum engine_input_handler_result
route_sb_advertised_route_data_handler(struct engine_node *node, void *data)
{
    struct ed_type_route *re_data = data;
    const struct sbrec_advertised_route_table *advertised_route_table =
        EN_OVSDB_GET(engine_get_input("SB_advertised_route", node));

    /* There are the following cases where we need to handle updates to the
     * advertised_route table:
     * 1. The advertised_route is created or deleted and we know about its
     *    datapath locally.
     *
     * Updates to advertised_route can generally be ignored as northd will not
     * update these entries. For exceptions see below.
     * We also get update notifications if a referenced port_binding is
     * updated, but these are handled in the runtime_data handler. */
    const struct sbrec_advertised_route *sbrec_route;
    SBREC_ADVERTISED_ROUTE_TABLE_FOR_EACH_TRACKED (sbrec_route,
                                                   advertised_route_table) {
        struct tracked_datapath *re_t_dp =
            tracked_datapath_find(&re_data->tracked_route_datapaths,
                                  sbrec_route->datapath);
        if (!re_t_dp) {
            continue;
        }

        if (sbrec_advertised_route_is_new(sbrec_route) ||
                sbrec_advertised_route_is_deleted(sbrec_route)) {
            /* XXX: Until we get I-P support for route exchange we need to
             * request recompute. */
            return EN_UNHANDLED;
        }

        if (sbrec_route->tracked_port) {
            const char *name = sbrec_route->tracked_port->logical_port;
            if (!(sset_contains(&re_data->tracked_ports_local, name) ||
                 sset_contains(&re_data->tracked_ports_remote, name))) {
                /* Advertised_Routes are generally not changed by northd.
                 * However if we did not monitor for the Port_Binding
                 * referenced by tracked_port previously then it would have
                 * been NULL. If we notice that we have now loaded the
                 * Port_Binding we need to recompute to correctly update
                 * the route priority. */
                return EN_UNHANDLED;
            }
        }
    }
    return EN_HANDLED_UNCHANGED;
}

struct ed_type_route_exchange {
    /* We need the idl to check if the Learned_Route table exists. */
    struct ovsdb_idl *sb_idl;
    /* Set to true when SB is readonly and we have routes that need
     * to be inserted into SB. */
    bool sb_changes_pending;
};

static enum engine_node_state
en_route_exchange_run(struct engine_node *node, void *data)
{
    struct ed_type_route_exchange *re = data;

    struct ovsdb_idl_index *sbrec_learned_route_by_datapath =
        engine_ovsdb_node_get_index(
            engine_get_input("SB_learned_route", node),
            "datapath");

    struct ovsdb_idl_index *sbrec_port_binding_by_name =
        engine_ovsdb_node_get_index(
                engine_get_input("SB_port_binding", node),
                "name");

    struct ed_type_route *route_data =
        engine_get_input_data("route", node);

    /* There can not actually be any routes to advertise unless we also have
     * the Learned_Route table, since they where introduced in the same
     * release. */
    if (!sbrec_server_has_learned_route_table(re->sb_idl)) {
        return EN_STALE;
    }

    struct route_exchange_ctx_in r_ctx_in = {
        .ovnsb_idl_txn = engine_get_context()->ovnsb_idl_txn,
        .sbrec_learned_route_by_datapath = sbrec_learned_route_by_datapath,
        .sbrec_port_binding_by_name = sbrec_port_binding_by_name,
        .announce_routes = &route_data->announce_routes,
    };
    struct route_exchange_ctx_out r_ctx_out = {
        .sb_changes_pending = false,
    };

    hmap_init(&r_ctx_out.route_table_watches);

    route_exchange_run(&r_ctx_in, &r_ctx_out);
    route_table_notify_update_watches(&r_ctx_out.route_table_watches);

    route_table_watch_request_cleanup(&r_ctx_out.route_table_watches);
    hmap_destroy(&r_ctx_out.route_table_watches);

    re->sb_changes_pending = r_ctx_out.sb_changes_pending;

    return EN_UPDATED;
}

static enum engine_input_handler_result
route_exchange_sb_ro_handler(struct engine_node *node OVS_UNUSED, void *data)
{
    struct ed_type_route_exchange *re = data;
    if (re->sb_changes_pending) {
        return EN_UNHANDLED;
    }

    return EN_HANDLED_UNCHANGED;
}


static void *
en_route_exchange_init(struct engine_node *node OVS_UNUSED,
                       struct engine_arg *arg)
{
    struct ed_type_route_exchange *re = xzalloc(sizeof *re);

    re->sb_idl = arg->sb_idl;
    return re;
}

static void
en_route_exchange_cleanup(void *data OVS_UNUSED)
{
}

struct ed_type_route_table_notify {
    /* For incremental processing this could be tracked per datapath in
     * the future. */
    bool changed;
};

static enum engine_node_state
en_route_table_notify_run(struct engine_node *node OVS_UNUSED, void *data)
{
    struct ed_type_route_table_notify *rtn = data;
    enum engine_node_state state;
    if (rtn->changed) {
        state = EN_UPDATED;
    } else {
        state = EN_UNCHANGED;
    }
    rtn->changed = false;
    return state;
}


static void *
en_route_table_notify_init(struct engine_node *node OVS_UNUSED,
                           struct engine_arg *arg OVS_UNUSED)
{
    struct ed_type_route_table_notify *rtn = xzalloc(sizeof *rtn);
    rtn->changed = true;
    return rtn;
}

static void
en_route_table_notify_cleanup(void *data OVS_UNUSED)
{
}

struct ed_type_route_exchange_status {
    bool netlink_trigger_run;
};

static void *
en_route_exchange_status_init(struct engine_node *node OVS_UNUSED,
                              struct engine_arg *arg OVS_UNUSED)
{
    return xzalloc(sizeof(struct ed_type_route_exchange_status));
}

static enum engine_node_state
en_route_exchange_status_run(struct engine_node *node OVS_UNUSED, void *data)
{
    struct ed_type_route_exchange_status *res = data;
    enum engine_node_state state;

    if (res->netlink_trigger_run) {
        state = EN_UPDATED;
    } else {
        state = EN_UNCHANGED;
    }
    res->netlink_trigger_run = false;

    return state;
}

static void
en_route_exchange_status_cleanup(void *data OVS_UNUSED)
{
}

static enum engine_node_state
en_garp_rarp_run(struct engine_node *node, void *data_)
{
    struct ed_type_garp_rarp *data = data_;
    struct controller_engine_ctx *ctrl_ctx =
        engine_get_context()->client_ctx;

    const struct ovsrec_open_vswitch_table *ovs_table =
        EN_OVSDB_GET(engine_get_input("OVS_open_vswitch", node));
    const char *chassis_id = get_ovs_chassis_id(ovs_table);
    ovs_assert(chassis_id);

    struct ovsdb_idl_index *sbrec_chassis_by_name =
        engine_ovsdb_node_get_index(
                engine_get_input("SB_chassis", node),
                "name");
    const struct sbrec_chassis *chassis
        = chassis_lookup_by_name(sbrec_chassis_by_name, chassis_id);
    ovs_assert(chassis);

    const struct ovsrec_open_vswitch *cfg
        = ovsrec_open_vswitch_table_first(ovs_table);

    struct ovsdb_idl_index *sbrec_port_binding_by_datapath =
        engine_ovsdb_node_get_index(
                engine_get_input("SB_port_binding", node),
                "datapath");
    struct ovsdb_idl_index *sbrec_port_binding_by_name =
        engine_ovsdb_node_get_index(
                engine_get_input("SB_port_binding", node),
                "name");
    struct ovsdb_idl_index *sbrec_mac_binding_by_lport_ip =
        engine_ovsdb_node_get_index(
                engine_get_input("SB_mac_binding", node),
                "lport_ip");

    struct ed_type_runtime_data *rt_data =
        engine_get_input_data("runtime_data", node);

    struct garp_rarp_ctx_in r_ctx_in = {
        .ovnsb_idl_txn = engine_get_context()->ovnsb_idl_txn,
        .cfg = cfg,
        .sbrec_port_binding_by_datapath = sbrec_port_binding_by_datapath,
        .sbrec_port_binding_by_name = sbrec_port_binding_by_name,
        .sbrec_mac_binding_by_lport_ip = sbrec_mac_binding_by_lport_ip,
        .chassis = chassis,
        .active_tunnels = &rt_data->active_tunnels,
        .local_datapaths = &rt_data->local_datapaths,
        .data = data,
        .mgr = ctrl_ctx->if_mgr,
    };

    garp_rarp_run(&r_ctx_in);
    return EN_UPDATED;
}


static void *
en_garp_rarp_init(struct engine_node *node OVS_UNUSED,
                  struct engine_arg *arg OVS_UNUSED)
{
    return garp_rarp_init();
}

static void
en_garp_rarp_cleanup(void *data)
{
    garp_rarp_cleanup(data);
}

static enum engine_input_handler_result
garp_rarp_sb_port_binding_handler(struct engine_node *node,
                                  void *data_)
{
    /* We need to handle a change if there was change on a datapath with
     * a localnet port.
     * Also the ha_chassis status of a port binding might change. */
    struct ed_type_garp_rarp *data = data_;

    const struct ovsrec_open_vswitch_table *ovs_table =
        EN_OVSDB_GET(engine_get_input("OVS_open_vswitch", node));
    const char *chassis_id = get_ovs_chassis_id(ovs_table);
    ovs_assert(chassis_id);

    struct ovsdb_idl_index *sbrec_chassis_by_name =
        engine_ovsdb_node_get_index(
                engine_get_input("SB_chassis", node),
                "name");
    const struct sbrec_chassis *chassis
        = chassis_lookup_by_name(sbrec_chassis_by_name, chassis_id);
    ovs_assert(chassis);

    struct ed_type_runtime_data *rt_data =
            engine_get_input_data("runtime_data", node);
    const struct sbrec_port_binding_table *port_binding_table =
        EN_OVSDB_GET(engine_get_input("SB_port_binding", node));
    struct ovsdb_idl_index *sbrec_port_binding_by_name =
        engine_ovsdb_node_get_index(
                engine_get_input("SB_port_binding", node),
                "name");
    struct controller_engine_ctx *ctrl_ctx = engine_get_context()->client_ctx;

    const struct sbrec_port_binding *pb;
    SBREC_PORT_BINDING_TABLE_FOR_EACH_TRACKED (pb, port_binding_table) {
        struct local_datapath *ld = get_local_datapath(
            &rt_data->local_datapaths, pb->datapath->tunnel_key);

        if (!ld) {
            continue;
        }

        if (ld->localnet_port) {
            /* XXX: actually handle this incrementally. */
            return EN_UNHANDLED;
        }

        if (sset_contains(&data->non_local_lports, pb->logical_port) &&
            lport_is_chassis_resident(sbrec_port_binding_by_name, chassis,
                                      pb->logical_port)) {
            /* XXX: actually handle this incrementally. */
            return EN_UNHANDLED;
        }

        if (sset_contains(&data->local_lports, pb->logical_port) &&
            !lport_is_chassis_resident(sbrec_port_binding_by_name, chassis,
                                       pb->logical_port)) {
            /* XXX: actually handle this incrementally. */
            return EN_UNHANDLED;
        }

        /* If the cr_port was updated, bound to a different chassis in idl
         * and (re)bound to our chassis in runtime data, make sure to reset
         * garp timers*/
        if (sbrec_port_binding_is_updated(pb,
                                          SBREC_PORT_BINDING_COL_CHASSIS) &&
            if_status_reclaimed(ctrl_ctx->if_mgr, pb->logical_port)) {
            garp_rarp_node_reset_timers(pb->logical_port);
        }
    }

    return EN_HANDLED_UNCHANGED;
}

static enum engine_input_handler_result
garp_rarp_sb_datapath_binding_handler(struct engine_node *node,
                                      void *data_ OVS_UNUSED)
{
    struct ed_type_runtime_data *rt_data =
            engine_get_input_data("runtime_data", node);

    const struct sbrec_datapath_binding_table *dp_binding_table =
        EN_OVSDB_GET(engine_get_input("SB_datapath_binding", node));
    const struct sbrec_datapath_binding *dp;
    SBREC_DATAPATH_BINDING_TABLE_FOR_EACH_TRACKED (dp, dp_binding_table) {
        struct local_datapath *ld = get_local_datapath(
            &rt_data->local_datapaths, dp->tunnel_key);
        if (!ld || ld->is_switch) {
            continue;
        }

        if (sbrec_datapath_binding_is_updated(
                    dp, SBREC_DATAPATH_BINDING_COL_EXTERNAL_IDS)) {
            return EN_UNHANDLED;
        }
    }

    return EN_HANDLED_UNCHANGED;
}

static enum engine_input_handler_result
garp_rarp_runtime_data_handler(struct engine_node *node, void *data OVS_UNUSED)
{
    /* We use two elements from rt_data:
     * 1. active_tunnels: There is currently not incremental processing for
     *    this in runtime_data. So we just fall back to a recompute.
     * 2. local_datapaths: This has incremental processing on the runtime_data
     *    side. We are only interested in datapaths with a localnet port so
     *    we just recompute if there is one in there. Otherwise the change is
     *    irrelevant for us. */

    struct ed_type_runtime_data *rt_data =
            engine_get_input_data("runtime_data", node);

    /* There are no tracked data. Fall back to full recompute. */
    if (!rt_data->tracked) {
        return EN_UNHANDLED;
    }

    struct tracked_datapath *tdp;
    HMAP_FOR_EACH (tdp, node, &rt_data->tracked_dp_bindings) {
        if (tdp->tracked_type == TRACKED_RESOURCE_REMOVED) {
            /* This is currently not handled incrementally in runtime_data
             * so it should never happen. Recompute just in case. */
            return EN_UNHANDLED;
        }

        struct local_datapath *ld = get_local_datapath(
            &rt_data->local_datapaths, tdp->dp->tunnel_key);

        if (!ld) {
            continue;
        }

        if (ld->localnet_port) {
            /* XXX: actually handle this incrementally. */
            return EN_UNHANDLED;
        }

        /* The localnet port might also have been removed. */
        struct tracked_lport *tlp;
        struct shash_node *sn;
        SHASH_FOR_EACH (sn, &tdp->lports) {
            tlp = sn->data;
            if (!strcmp(tlp->pb->type, "localnet")) {
                return EN_UNHANDLED;
            }
        }
    }

    return EN_HANDLED_UNCHANGED;
}

struct ed_type_host_if_monitor {
    /* For incremental processing this could be tracked per interface in
     * the future. */
    bool changed;
};

static void *
en_host_if_monitor_init(struct engine_node *node OVS_UNUSED,
                       struct engine_arg *arg OVS_UNUSED)
{
    struct ed_type_host_if_monitor *hifm = xmalloc(sizeof *hifm);

    *hifm = (struct ed_type_host_if_monitor) {
        .changed = true,
    };
    return hifm;
}

static void
en_host_if_monitor_cleanup(void *data OVS_UNUSED)
{
}

static enum engine_node_state
en_host_if_monitor_run(struct engine_node *node OVS_UNUSED, void *data)
{
    struct ed_type_host_if_monitor *hifm = data;
    enum engine_node_state state;
    if (hifm->changed) {
        state = EN_UPDATED;
    } else {
        state = EN_UNCHANGED;
    }
    hifm->changed = false;
    return state;
}

struct ed_type_neighbor {
    /* Contains struct neighbor_interface_monitor pointers. */
    struct vector monitored_interfaces;
    /* Contains set of PB names that are currently advertised. */
    struct sset advertised_pbs;
};

static void *
en_neighbor_init(struct engine_node *node OVS_UNUSED,
                 struct engine_arg *arg OVS_UNUSED)
{
    struct ed_type_neighbor *data = xzalloc(sizeof *data);

    *data = (struct ed_type_neighbor) {
        .monitored_interfaces =
            VECTOR_EMPTY_INITIALIZER(struct neighbor_interface_monitor *),
        .advertised_pbs = SSET_INITIALIZER(&data->advertised_pbs),
    };
    return data;
}

static void
en_neighbor_cleanup(void *data)
{
    struct ed_type_neighbor *ne_data = data;

    neighbor_cleanup(&ne_data->monitored_interfaces);
    vector_destroy(&ne_data->monitored_interfaces);
    sset_destroy(&ne_data->advertised_pbs);
}

static enum engine_node_state
en_neighbor_run(struct engine_node *node OVS_UNUSED, void *data)
{
    struct ed_type_neighbor *ne_data = data;
    struct ed_type_runtime_data *rt_data =
        engine_get_input_data("runtime_data", node);
    struct ovsdb_idl_index *sbrec_port_binding_by_datapath =
        engine_ovsdb_node_get_index(
            engine_get_input("SB_port_binding", node),
            "datapath");
    struct ovsdb_idl_index *sbrec_port_binding_by_name =
        engine_ovsdb_node_get_index(
            engine_get_input("SB_port_binding", node),
            "name");
    const struct ovsrec_open_vswitch_table *ovs_table =
        EN_OVSDB_GET(engine_get_input("OVS_open_vswitch", node));
    struct ovsdb_idl_index *sbrec_chassis_by_name =
        engine_ovsdb_node_get_index(
            engine_get_input("SB_chassis", node),
            "name");
    struct ovsdb_idl_index *sbrec_advertised_mac_binding_by_datapath =
        engine_ovsdb_node_get_index(
            engine_get_input("SB_advertised_mac_binding", node),
            "datapath");

    const char *chassis_id = get_ovs_chassis_id(ovs_table);
    ovs_assert(chassis_id);
    const struct sbrec_chassis *chassis =
        chassis_lookup_by_name(sbrec_chassis_by_name, chassis_id);
    ovs_assert(chassis);

    struct neighbor_ctx_in n_ctx_in = {
        .local_datapaths = &rt_data->local_datapaths,
        .sbrec_pb_by_dp = sbrec_port_binding_by_datapath,
        .sbrec_amb_by_dp = sbrec_advertised_mac_binding_by_datapath,
        .sbrec_pb_by_name = sbrec_port_binding_by_name,
        .chassis = chassis,
    };

    struct neighbor_ctx_out n_ctx_out = {
        .monitored_interfaces = &ne_data->monitored_interfaces,
        .advertised_pbs = &ne_data->advertised_pbs,
    };

    neighbor_cleanup(&ne_data->monitored_interfaces);
    sset_clear(&ne_data->advertised_pbs);
    neighbor_run(&n_ctx_in, &n_ctx_out);

    return EN_UPDATED;
}

static enum engine_input_handler_result
neighbor_runtime_data_handler(struct engine_node *node, void *data)
{
    struct ed_type_neighbor *ne_data = data;
    struct ed_type_runtime_data *rt_data =
        engine_get_input_data("runtime_data", node);
    struct ovsdb_idl_index *sbrec_port_binding_by_name =
        engine_ovsdb_node_get_index(
            engine_get_input("SB_port_binding", node),
            "name");
    const struct ovsrec_open_vswitch_table *ovs_table =
        EN_OVSDB_GET(engine_get_input("OVS_open_vswitch", node));
    struct ovsdb_idl_index *sbrec_chassis_by_name =
        engine_ovsdb_node_get_index(
            engine_get_input("SB_chassis", node),
            "name");

    const char *chassis_id = get_ovs_chassis_id(ovs_table);
    ovs_assert(chassis_id);
    const struct sbrec_chassis *chassis =
        chassis_lookup_by_name(sbrec_chassis_by_name, chassis_id);
    ovs_assert(chassis);

    /* There are no tracked data. Fall back to full recompute. */
    if (!rt_data->tracked) {
        return EN_UNHANDLED;
    }

    struct tracked_datapath *tdp;
    HMAP_FOR_EACH (tdp, node, &rt_data->tracked_dp_bindings) {
        struct local_datapath *ld =
            get_local_datapath(&rt_data->local_datapaths, tdp->dp->tunnel_key);
        if (!ld || !ld->is_switch) {
            continue;
        }

        int64_t vni = ovn_smap_get_llong(&tdp->dp->external_ids,
                                         "dynamic-routing-vni", -1);
        if (!ovn_is_valid_vni(vni)) {
            continue;
        }

        if (tdp->tracked_type == TRACKED_RESOURCE_NEW ||
            tdp->tracked_type == TRACKED_RESOURCE_REMOVED) {
            return EN_UNHANDLED;
        }

        const char *redistribute = smap_get(&ld->datapath->external_ids,
                                            "dynamic-routing-redistribute");
        if (!redistribute) {
            continue;
        }
        if (strcmp(redistribute, "fdb") && strcmp(redistribute, "ip")) {
            continue;
        }

        const struct shash_node *shash_node;
        SHASH_FOR_EACH (shash_node, &tdp->lports) {
            if (neighbor_is_relevant_port_updated(sbrec_port_binding_by_name,
                                                  chassis,
                                                  &ne_data->advertised_pbs,
                                                  shash_node->data)) {
                return EN_UNHANDLED;
            }
        }
    }

    return EN_HANDLED_UNCHANGED;
}

static enum engine_input_handler_result
neighbor_sb_datapath_binding_handler(struct engine_node *node,
                                     void *data OVS_UNUSED)
{
    const struct sbrec_datapath_binding_table *dp_table =
        EN_OVSDB_GET(engine_get_input("SB_datapath_binding", node));
    struct ed_type_runtime_data *rt_data =
        engine_get_input_data("runtime_data", node);

    const struct sbrec_datapath_binding *dp;
    SBREC_DATAPATH_BINDING_TABLE_FOR_EACH_TRACKED (dp, dp_table) {
        if (sbrec_datapath_binding_is_new(dp) ||
            sbrec_datapath_binding_is_deleted(dp)) {
            /* We are reflecting only datapaths that are becoming or are
             * removed from being local, that is taken care of by runtime_data
             * handler. */
           return EN_HANDLED_UNCHANGED;
        }

        struct local_datapath *ld =
            get_local_datapath(&rt_data->local_datapaths, dp->tunnel_key);
        if (!ld || !ld->is_switch) {
            continue;
        }

        if (sbrec_datapath_binding_is_updated(
                dp, SBREC_DATAPATH_BINDING_COL_EXTERNAL_IDS)) {
            return EN_UNHANDLED;
        }
    }

    return EN_HANDLED_UNCHANGED;
}

static enum engine_input_handler_result
neighbor_sb_port_binding_handler(struct engine_node *node, void *data)
{
    struct ed_type_neighbor *ne_data = data;
    const struct sbrec_port_binding_table *pb_table =
        EN_OVSDB_GET(engine_get_input("SB_port_binding", node));

    const struct sbrec_port_binding *pb;
    SBREC_PORT_BINDING_TABLE_FOR_EACH_TRACKED (pb, pb_table) {
        if (sbrec_port_binding_is_new(pb) ||
            sbrec_port_binding_is_deleted(pb)) {
            /* The removal and addition is handled via runtime_data. */
            return EN_HANDLED_UNCHANGED;
        }

        if (sbrec_port_binding_is_updated(pb, SBREC_PORT_BINDING_COL_MAC) &&
            sset_contains(&ne_data->advertised_pbs, pb->logical_port)) {
            return EN_UNHANDLED;
        }
    }

    return EN_HANDLED_UNCHANGED;
}

struct ed_type_neighbor_table_notify {
    /* For incremental processing this could be tracked per interface in
     * the future. */
    bool changed;
};

static void *
en_neighbor_table_notify_init(struct engine_node *node OVS_UNUSED,
                              struct engine_arg *arg OVS_UNUSED)
{
    struct ed_type_neighbor_table_notify *ntn = xmalloc(sizeof *ntn);

    *ntn = (struct ed_type_neighbor_table_notify) {
        .changed = true,
    };
    return ntn;
}

static void
en_neighbor_table_notify_cleanup(void *data OVS_UNUSED)
{
}

static enum engine_node_state
en_neighbor_table_notify_run(struct engine_node *node OVS_UNUSED,
                             void *data)
{
    struct ed_type_neighbor_table_notify *ntn = data;
    enum engine_node_state state;
    if (ntn->changed) {
        state = EN_UPDATED;
    } else {
        state = EN_UNCHANGED;
    }
    ntn->changed = false;
    return state;
}

struct ed_type_neighbor_exchange {
    /* Contains 'struct evpn_remote_vtep'. */
    struct hmap remote_vteps;
    /* Contains 'struct evpn_static_entry', remote FDB entries learned through
     * EVPN. */
    struct hmap static_fdbs;
    /* Contains 'struct evpn_static_entry', remote ARP entries learned through
     * EVPN. */
    struct hmap static_arps;
};

static void *
en_neighbor_exchange_init(struct engine_node *node OVS_UNUSED,
                          struct engine_arg *arg OVS_UNUSED)
{
    struct ed_type_neighbor_exchange *data = xmalloc(sizeof *data);
    *data = (struct ed_type_neighbor_exchange) {
        .remote_vteps = HMAP_INITIALIZER(&data->remote_vteps),
        .static_fdbs = HMAP_INITIALIZER(&data->static_fdbs),
        .static_arps = HMAP_INITIALIZER(&data->static_arps),
    };

    return data;
}

static void
en_neighbor_exchange_cleanup(void *data_)
{
    struct ed_type_neighbor_exchange *data = data_;
    evpn_remote_vteps_clear(&data->remote_vteps);
    evpn_static_entries_clear(&data->static_fdbs);
    evpn_static_entries_clear(&data->static_arps);
    hmap_destroy(&data->remote_vteps);
    hmap_destroy(&data->static_fdbs);
    hmap_destroy(&data->static_arps);
}

static enum engine_node_state
en_neighbor_exchange_run(struct engine_node *node, void *data_)
{
    struct ed_type_neighbor_exchange *data = data_;
    const struct ed_type_neighbor *neighbor_data =
        engine_get_input_data("neighbor", node);

    evpn_remote_vteps_clear(&data->remote_vteps);
    evpn_static_entries_clear(&data->static_fdbs);
    evpn_static_entries_clear(&data->static_arps);

    struct neighbor_exchange_ctx_in n_ctx_in = {
        .monitored_interfaces = &neighbor_data->monitored_interfaces,
    };
    struct neighbor_exchange_ctx_out n_ctx_out = {
        .neighbor_table_watches =
            HMAP_INITIALIZER(&n_ctx_out.neighbor_table_watches),
        .remote_vteps = &data->remote_vteps,
        .static_fdbs = &data->static_fdbs,
        .static_arps = &data->static_arps,
    };

    neighbor_exchange_run(&n_ctx_in, &n_ctx_out);
    neighbor_table_notify_update_watches(&n_ctx_out.neighbor_table_watches);

    neighbor_table_watch_request_cleanup(&n_ctx_out.neighbor_table_watches);
    hmap_destroy(&n_ctx_out.neighbor_table_watches);

    return EN_UPDATED;
}

struct ed_type_neighbor_exchange_status {
    bool netlink_trigger_run;
};

static void *
en_neighbor_exchange_status_init(struct engine_node *node OVS_UNUSED,
                                 struct engine_arg *arg OVS_UNUSED)
{
    return xzalloc(sizeof(struct ed_type_neighbor_exchange_status));
}

static void
en_neighbor_exchange_status_cleanup(void *data OVS_UNUSED)
{
}

static enum engine_node_state
en_neighbor_exchange_status_run(struct engine_node *node OVS_UNUSED,
                                void *data OVS_UNUSED)
{
    struct ed_type_neighbor_exchange_status *res = data;
    enum engine_node_state state;

    if (res->netlink_trigger_run) {
        state = EN_UPDATED;
    } else {
        state = EN_UNCHANGED;
    }
    res->netlink_trigger_run = false;

    return state;
}

static void *
en_evpn_vtep_binding_init(struct engine_node *node OVS_UNUSED,
                          struct engine_arg *arg OVS_UNUSED)
{
    struct ed_type_evpn_vtep_binding *data = xmalloc(sizeof *data);
    *data = (struct ed_type_evpn_vtep_binding) {
        .bindings = HMAP_INITIALIZER(&data->bindings),
        .updated_bindings = HMAPX_INITIALIZER(&data->updated_bindings),
        .removed_bindings = UUIDSET_INITIALIZER(&data->removed_bindings),
        .datapaths = HMAP_INITIALIZER(&data->datapaths),
        .multicast_groups = HMAP_INITIALIZER(&data->multicast_groups),
        .updated_multicast_groups =
            HMAPX_INITIALIZER(&data->updated_multicast_groups),
        .removed_multicast_groups =
            UUIDSET_INITIALIZER(&data->removed_multicast_groups),
        .tunnel_keys = HMAP_INITIALIZER(&data->tunnel_keys),
    };

    return data;
}

static void
en_evpn_vtep_binding_clear_tracked_data(void *data_)
{
    struct ed_type_evpn_vtep_binding *data = data_;
    hmapx_clear(&data->updated_bindings);
    uuidset_clear(&data->removed_bindings);
    evpn_datapaths_clear(&data->datapaths);
    hmapx_clear(&data->updated_multicast_groups);
    uuidset_clear(&data->removed_multicast_groups);
}

static void
en_evpn_vtep_binding_cleanup(void *data_)
{
    struct ed_type_evpn_vtep_binding *data = data_;
    evpn_bindings_destroy(&data->bindings);
    hmapx_destroy(&data->updated_bindings);
    uuidset_destroy(&data->removed_bindings);
    evpn_datapaths_destroy(&data->datapaths);
    evpn_multicast_groups_destroy(&data->multicast_groups);
    hmapx_clear(&data->updated_multicast_groups);
    uuidset_clear(&data->removed_multicast_groups);
    ovn_destroy_tnlids(&data->tunnel_keys);
}

static enum engine_node_state
en_evpn_vtep_binding_run(struct engine_node *node, void *data_)
{
    struct ed_type_evpn_vtep_binding *data = data_;
    const struct ed_type_neighbor_exchange *ne_data =
        engine_get_input_data("neighbor_exchange", node);
    const struct ed_type_runtime_data *rt_data =
        engine_get_input_data("runtime_data", node);
    const struct ovsrec_open_vswitch_table *ovs_table =
        EN_OVSDB_GET(engine_get_input("OVS_open_vswitch", node));
    const struct ovsrec_bridge_table *bridge_table =
        EN_OVSDB_GET(engine_get_input("OVS_bridge", node));
    const struct ovsrec_bridge *br_int = get_br_int(bridge_table, ovs_table);

    struct evpn_binding_ctx_in b_ctx_in = {
        .br_int = br_int,
        .local_datapaths = &rt_data->local_datapaths,
        .remote_vteps = &ne_data->remote_vteps,
    };

    struct evpn_binding_ctx_out b_ctx_out = {
        .bindings = &data->bindings,
        .updated_bindings = &data->updated_bindings,
        .removed_bindings = &data->removed_bindings,
        .datapaths = &data->datapaths,
        .multicast_groups = &data->multicast_groups,
        .updated_multicast_groups = &data->updated_multicast_groups,
        .removed_multicast_groups = &data->removed_multicast_groups,
        .tunnel_keys = &data->tunnel_keys,
    };

    evpn_binding_run(&b_ctx_in, &b_ctx_out);

    if (hmapx_count(&data->updated_bindings) ||
        uuidset_count(&data->removed_bindings) ||
        hmapx_count(&data->updated_multicast_groups) ||
        uuidset_count(&data->removed_multicast_groups)) {
        return EN_UPDATED;
    }

    return EN_UNCHANGED;
}

static enum engine_input_handler_result
evpn_vtep_binding_ovs_interface_handler(struct engine_node *node,
                                        void *data OVS_UNUSED)
{
    const struct ovsrec_interface_table *iface_table =
        EN_OVSDB_GET(engine_get_input("OVS_interface", node));

    const struct ovsrec_interface *iface;
    OVSREC_INTERFACE_TABLE_FOR_EACH_TRACKED (iface, iface_table) {
        if (!smap_get_bool(&iface->external_ids, "ovn-evpn-tunnel", false)) {
            continue;
        }

        if (ovsrec_interface_is_new(iface) ||
            ovsrec_interface_is_deleted(iface) ||
            ovsrec_interface_is_updated(iface, OVSREC_INTERFACE_COL_OFPORT)) {
            return EN_UNHANDLED;
        }
    }

    return EN_HANDLED_UNCHANGED;
}

static enum engine_input_handler_result
evpn_vtep_binding_datapath_binding_handler(struct engine_node *node,
                                           void *data OVS_UNUSED)
{
    const struct sbrec_datapath_binding_table *dp_table =
        EN_OVSDB_GET(engine_get_input("SB_datapath_binding", node));
    const struct ed_type_runtime_data *rt_data =
        engine_get_input_data("runtime_data", node);

    const struct sbrec_datapath_binding *dp;
    SBREC_DATAPATH_BINDING_TABLE_FOR_EACH_TRACKED (dp, dp_table) {
        if (sbrec_datapath_binding_is_new(dp) ||
            sbrec_datapath_binding_is_deleted(dp)) {
            /* The removal and addition is handled via the
             * en_neighbor_exchange I-P node. */
            return EN_HANDLED_UNCHANGED;
        }

        struct local_datapath *ld =
            get_local_datapath(&rt_data->local_datapaths, dp->tunnel_key);
        if (!ld || !ld->is_switch) {
            continue;
        }

        int64_t vni = ovn_smap_get_llong(&dp->external_ids,
                                         "dynamic-routing-vni", -1);
        if (!ovn_is_valid_vni(vni)) {
            continue;
        }

        if (sbrec_datapath_binding_is_updated(
                dp, SBREC_DATAPATH_BINDING_COL_TUNNEL_KEY)) {
            return EN_UNHANDLED;
        }
    }

    return EN_HANDLED_UNCHANGED;
}

static void *
en_evpn_fdb_init(struct engine_node *node OVS_UNUSED,
                 struct engine_arg *arg OVS_UNUSED)
{
    struct ed_type_evpn_fdb *data = xmalloc(sizeof *data);
    *data = (struct ed_type_evpn_fdb) {
        .fdbs = HMAP_INITIALIZER(&data->fdbs),
        .updated_fdbs = HMAPX_INITIALIZER(&data->updated_fdbs),
        .removed_fdbs = UUIDSET_INITIALIZER(&data->removed_fdbs),
    };

    return data;
}

static void
en_evpn_fdb_clear_tracked_data(void *data_)
{
    struct ed_type_evpn_fdb *data = data_;
    hmapx_clear(&data->updated_fdbs);
    uuidset_clear(&data->removed_fdbs);
}

static void
en_evpn_fdb_cleanup(void *data_)
{
    struct ed_type_evpn_fdb *data = data_;
    evpn_fdbs_destroy(&data->fdbs);
    hmapx_destroy(&data->updated_fdbs);
    uuidset_destroy(&data->removed_fdbs);
}

static enum engine_node_state
en_evpn_fdb_run(struct engine_node *node, void *data_)
{
    struct ed_type_evpn_fdb *data = data_;
    const struct ed_type_neighbor_exchange *ne_data =
        engine_get_input_data("neighbor_exchange", node);
    const struct ed_type_evpn_vtep_binding *eb_data =
        engine_get_input_data("evpn_vtep_binding", node);

    struct evpn_fdb_ctx_in f_ctx_in = {
        .static_fdbs = &ne_data->static_fdbs,
        .bindings = &eb_data->bindings,
    };

    struct evpn_fdb_ctx_out f_ctx_out = {
        .fdbs = &data->fdbs,
        .updated_fdbs = &data->updated_fdbs,
        .removed_fdbs = &data->removed_fdbs,
    };

    evpn_fdb_run(&f_ctx_in, &f_ctx_out);

    if (hmapx_count(&data->updated_fdbs) ||
        uuidset_count(&data->removed_fdbs)) {
        return EN_UPDATED;
    }

    return EN_UNCHANGED;
}

static enum engine_input_handler_result
evpn_fdb_vtep_binding_handler(struct engine_node *node, void *data OVS_UNUSED)
{
    const struct ed_type_evpn_vtep_binding *eb_data =
        engine_get_input_data("evpn_vtep_binding", node);

    if (hmapx_is_empty(&eb_data->updated_bindings) &&
        uuidset_is_empty(&eb_data->removed_bindings)) {
        return EN_HANDLED_UNCHANGED;
    }

    return EN_UNHANDLED;
}

static void *
en_evpn_arp_init(struct engine_node *node OVS_UNUSED,
                 struct engine_arg *arg OVS_UNUSED)
{
    struct ed_type_evpn_arp *data = xmalloc(sizeof *data);
    *data = (struct ed_type_evpn_arp) {
        .arps = HMAP_INITIALIZER(&data->arps),
        .updated_arps = HMAPX_INITIALIZER(&data->updated_arps),
        .removed_arps = UUIDSET_INITIALIZER(&data->removed_arps),
    };

    return data;
}

static void
en_evpn_arp_clear_tracked_data(void *data_)
{
    struct ed_type_evpn_arp *data = data_;
    hmapx_clear(&data->updated_arps);
    uuidset_clear(&data->removed_arps);
}

static void
en_evpn_arp_cleanup(void *data_)
{
    struct ed_type_evpn_arp *data = data_;
    evpn_arps_destroy(&data->arps);
    hmapx_destroy(&data->updated_arps);
    uuidset_destroy(&data->removed_arps);
}

static enum engine_node_state
en_evpn_arp_run(struct engine_node *node, void *data_)
{
    struct ed_type_evpn_arp *data = data_;
    const struct ed_type_neighbor_exchange *ne_data =
        engine_get_input_data("neighbor_exchange", node);
    const struct ed_type_evpn_vtep_binding *eb_data =
        engine_get_input_data("evpn_vtep_binding", node);

    struct evpn_arp_ctx_in f_ctx_in = {
        .datapaths = &eb_data->datapaths,
        .static_arps = &ne_data->static_arps,
    };

    struct evpn_arp_ctx_out f_ctx_out = {
        .arps = &data->arps,
        .updated_arps = &data->updated_arps,
        .removed_arps = &data->removed_arps,
    };

    evpn_arp_run(&f_ctx_in, &f_ctx_out);

    if (hmapx_count(&data->updated_arps) ||
        uuidset_count(&data->removed_arps)) {
        return EN_UPDATED;
    }

    return EN_UNCHANGED;
}

static enum engine_input_handler_result
evpn_arp_vtep_binding_handler(struct engine_node *node, void *data OVS_UNUSED)
{
    const struct ed_type_evpn_vtep_binding *eb_data =
        engine_get_input_data("evpn_vtep_binding", node);

    if (hmapx_is_empty(&eb_data->updated_bindings) &&
        uuidset_is_empty(&eb_data->removed_bindings)) {
        return EN_HANDLED_UNCHANGED;
    }

    return EN_UNHANDLED;
}

/* Returns false if the northd internal version stored in SB_Global
 * and ovn-controller internal version don't match.
 */
static bool
check_northd_version(struct ovsdb_idl *ovs_idl, struct ovsdb_idl *ovnsb_idl,
                     const char *version)
{
    static bool version_mismatch;

    const struct ovsrec_open_vswitch *cfg = ovsrec_open_vswitch_first(ovs_idl);
    const struct ovsrec_open_vswitch_table *ovs_table =
        ovsrec_open_vswitch_table_get(ovs_idl);
    const char *chassis_id = get_ovs_chassis_id(ovs_table);
    if (!cfg || !get_chassis_external_id_value_bool(
                     &cfg->external_ids, chassis_id,
                     "ovn-match-northd-version", false)) {
        version_mismatch = false;
        return true;
    }

    const struct sbrec_sb_global *sb = sbrec_sb_global_first(ovnsb_idl);
    if (!sb) {
        version_mismatch = true;
        return false;
    }

    const char *northd_version =
        smap_get_def(&sb->options, "northd_internal_version", "");

    if (strcmp(northd_version, version)) {
        static struct vlog_rate_limit rl = VLOG_RATE_LIMIT_INIT(1, 1);
        VLOG_WARN_RL(&rl, "controller version - %s mismatch with northd "
                     "version - %s", version, northd_version);
        version_mismatch = true;
        return false;
    }

    /* If there used to be a mismatch and ovn-northd got updated, force a
     * full recompute.
     */
    if (version_mismatch) {
        engine_set_force_recompute();
    }
    version_mismatch = false;
    return true;
}

static void
br_int_remote_update(struct br_int_remote *remote,
                     const struct ovsrec_bridge *br_int,
                     const struct ovsrec_open_vswitch_table *ovs_table)
{
    if (!br_int) {
        return;
    }

    const struct ovsrec_open_vswitch *cfg =
            ovsrec_open_vswitch_table_first(ovs_table);

    const char *ext_target =
            smap_get(&cfg->external_ids, "ovn-bridge-remote");
    char *target = ext_target
            ? xstrdup(ext_target)
            : xasprintf("unix:%s/%s.mgmt", ovs_rundir(), br_int->name);

    if (!remote->target || strcmp(remote->target, target)) {
        free(remote->target);
        remote->target = target;
    } else {
        free(target);
    }

    unsigned long long probe_interval =
            smap_get_ullong(&cfg->external_ids,
                            "ovn-bridge-remote-probe-interval", 0);
    remote->probe_interval = MIN(probe_interval / 1000, INT_MAX);
}

static void
ovsdb_idl_loop_next_cfg_inc(struct ovsdb_idl_loop *idl_loop)
{
    if (idl_loop->next_cfg == INT64_MAX) {
        idl_loop->next_cfg = 0;
    } else {
        idl_loop->next_cfg++;
    }
}

int
main(int argc, char *argv[])
{
    struct unixctl_server *unixctl;
    struct ovn_exit_args exit_args = {0};
    struct br_int_remote br_int_remote = {0};
    int retval;

    /* Read from system-id-override file once on startup. */
    file_system_id = get_file_system_id();

    ovs_cmdl_proctitle_init(argc, argv);
    ovn_set_program_name(argv[0]);
    service_start(&argc, &argv);
    char *ovs_remote = parse_options(argc, argv);
    fatal_ignore_sigpipe();

    daemonize_start(true, false);

    char *abs_unixctl_path = get_abs_unix_ctl_path(unixctl_path);
    retval = unixctl_server_create(abs_unixctl_path, &unixctl);
    free(abs_unixctl_path);
    if (retval) {
        exit(EXIT_FAILURE);
    }
    unixctl_command_register("exit", "", 0, 1, ovn_exit_command_callback,
                             &exit_args);

    daemonize_complete();

    /* Register ofctrl seqno types. */
    ofctrl_seq_type_nb_cfg = ofctrl_seqno_add_type();

    patch_init();
    pinctrl_init();
    lflow_init();
    mirror_init();
    vif_plug_provider_initialize();
    statctrl_init();
    dns_resolve_init(true);

    /* Connect to OVS OVSDB instance. */
    struct ovsdb_idl_loop ovs_idl_loop = OVSDB_IDL_LOOP_INITIALIZER(
        ovsdb_idl_create(ovs_remote, &ovsrec_idl_class, false, true));
    ctrl_register_ovs_idl(ovs_idl_loop.idl);

    struct ovsdb_idl_index *ovsrec_port_by_interfaces
        = ovsdb_idl_index_create1(ovs_idl_loop.idl,
                                  &ovsrec_port_col_interfaces);
    struct ovsdb_idl_index *ovsrec_port_by_name
        = ovsdb_idl_index_create1(ovs_idl_loop.idl,
                                  &ovsrec_port_col_name);
    struct ovsdb_idl_index *ovsrec_port_by_qos
        = ovsdb_idl_index_create1(ovs_idl_loop.idl,
                                  &ovsrec_port_col_qos);
    struct ovsdb_idl_index *ovsrec_interface_by_name
        = ovsdb_idl_index_create1(ovs_idl_loop.idl,
                                  &ovsrec_interface_col_name);
    struct ovsdb_idl_index *ovsrec_queue_by_external_ids
        = ovsdb_idl_index_create1(ovs_idl_loop.idl,
                                  &ovsrec_queue_col_external_ids);
    struct ovsdb_idl_index *ovsrec_flow_sample_collector_set_by_id
        = ovsdb_idl_index_create2(ovs_idl_loop.idl,
                                  &ovsrec_flow_sample_collector_set_col_bridge,
                                  &ovsrec_flow_sample_collector_set_col_id);

    ovsdb_idl_get_initial_snapshot(ovs_idl_loop.idl);

    /* Configure OVN SB database. */
    struct ovsdb_idl_loop ovnsb_idl_loop = OVSDB_IDL_LOOP_INITIALIZER(
        ovsdb_idl_create_unconnected(&sbrec_idl_class, true));
    ovsdb_idl_set_leader_only(ovnsb_idl_loop.idl, false);

    unixctl_command_register("connection-status", "", 0, 0,
                             ovn_conn_show, ovnsb_idl_loop.idl);

    struct ovsdb_idl_index *sbrec_chassis_by_name
        = chassis_index_create(ovnsb_idl_loop.idl);
    struct ovsdb_idl_index *sbrec_chassis_private_by_name
        = chassis_private_index_create(ovnsb_idl_loop.idl);
    struct ovsdb_idl_index *sbrec_multicast_group_by_name_datapath
        = mcast_group_index_create(ovnsb_idl_loop.idl);
    struct ovsdb_idl_index *sbrec_meter_by_name
        = ovsdb_idl_index_create1(ovnsb_idl_loop.idl, &sbrec_meter_col_name);
    struct ovsdb_idl_index *sbrec_logical_flow_by_logical_datapath
        = ovsdb_idl_index_create1(ovnsb_idl_loop.idl,
                                  &sbrec_logical_flow_col_logical_datapath);
    struct ovsdb_idl_index *sbrec_logical_flow_by_logical_dp_group
        = ovsdb_idl_index_create1(ovnsb_idl_loop.idl,
                                  &sbrec_logical_flow_col_logical_dp_group);
    struct ovsdb_idl_index *sbrec_port_binding_by_name
        = ovsdb_idl_index_create1(ovnsb_idl_loop.idl,
                                  &sbrec_port_binding_col_logical_port);
    struct ovsdb_idl_index *sbrec_port_binding_by_key
        = ovsdb_idl_index_create2(ovnsb_idl_loop.idl,
                                  &sbrec_port_binding_col_tunnel_key,
                                  &sbrec_port_binding_col_datapath);
    struct ovsdb_idl_index *sbrec_port_binding_by_datapath
        = ovsdb_idl_index_create1(ovnsb_idl_loop.idl,
                                  &sbrec_port_binding_col_datapath);
    struct ovsdb_idl_index *sbrec_port_binding_by_type
        = ovsdb_idl_index_create1(ovnsb_idl_loop.idl,
                                  &sbrec_port_binding_col_type);
    struct ovsdb_idl_index *sbrec_port_binding_by_requested_chassis
        = ovsdb_idl_index_create1(ovnsb_idl_loop.idl,
                                  &sbrec_port_binding_col_requested_chassis);
    struct ovsdb_idl_index *sbrec_datapath_binding_by_key
        = ovsdb_idl_index_create1(ovnsb_idl_loop.idl,
                                  &sbrec_datapath_binding_col_tunnel_key);
    struct ovsdb_idl_index *sbrec_mac_binding_by_lport_ip
        = mac_binding_by_lport_ip_index_create(ovnsb_idl_loop.idl);
    struct ovsdb_idl_index *sbrec_ip_multicast
        = ip_mcast_index_create(ovnsb_idl_loop.idl);
    struct ovsdb_idl_index *sbrec_igmp_group
        = igmp_group_index_create(ovnsb_idl_loop.idl);
    struct ovsdb_idl_index *sbrec_fdb_by_dp_key
        = ovsdb_idl_index_create1(ovnsb_idl_loop.idl,
                                  &sbrec_fdb_col_dp_key);
    struct ovsdb_idl_index *sbrec_fdb_by_dp_key_mac
        = ovsdb_idl_index_create2(ovnsb_idl_loop.idl,
                                  &sbrec_fdb_col_mac,
                                  &sbrec_fdb_col_dp_key);
    struct ovsdb_idl_index *sbrec_mac_binding_by_datapath
        = mac_binding_by_datapath_index_create(ovnsb_idl_loop.idl);
    struct ovsdb_idl_index *sbrec_static_mac_binding_by_datapath
        = ovsdb_idl_index_create1(ovnsb_idl_loop.idl,
                                  &sbrec_static_mac_binding_col_datapath);
    struct ovsdb_idl_index *sbrec_chassis_template_var_index_by_chassis
        = ovsdb_idl_index_create1(ovnsb_idl_loop.idl,
                                  &sbrec_chassis_template_var_col_chassis);
    struct ovsdb_idl_index *sbrec_learned_route_index_by_datapath
        = ovsdb_idl_index_create1(ovnsb_idl_loop.idl,
                                  &sbrec_learned_route_col_datapath);
    struct ovsdb_idl_index *sbrec_advertised_mac_binding_index_by_dp
        = ovsdb_idl_index_create1(ovnsb_idl_loop.idl,
                                  &sbrec_advertised_mac_binding_col_datapath);
    struct ovsdb_idl_index *sbrec_encaps
        = ovsdb_idl_index_create2(ovnsb_idl_loop.idl,
                                  &sbrec_encap_col_type, &sbrec_encap_col_ip);

    ovsdb_idl_track_add_all(ovnsb_idl_loop.idl);
    ovsdb_idl_omit_alert(ovnsb_idl_loop.idl,
                         &sbrec_chassis_private_col_nb_cfg);
    ovsdb_idl_omit_alert(ovnsb_idl_loop.idl,
                         &sbrec_chassis_private_col_nb_cfg_timestamp);
    /* Omit the timestamp columns of the MAC_Binding and FDB tables.
     * ovn-controller doesn't need to react to changes in timestamp
     * values (it does read them to implement aging).  Therefore we
     * can disable change tracking and alerting for these columns. */
    ovsdb_idl_omit_alert(ovnsb_idl_loop.idl, &sbrec_mac_binding_col_timestamp);
    ovsdb_idl_omit_alert(ovnsb_idl_loop.idl, &sbrec_fdb_col_timestamp);

    /* Omit the external_ids column of all the tables except for -
     *  - DNS. pinctrl.c uses the external_ids column of DNS,
     *    which it shouldn't. This should be removed.
     *
     *  - Datapath_binding - lflow.c is using this to check if the datapath
     *                       is switch or not. This should be removed.
     * */

    ovsdb_idl_omit(ovnsb_idl_loop.idl, &sbrec_sb_global_col_external_ids);
    ovsdb_idl_omit(ovnsb_idl_loop.idl, &sbrec_logical_flow_col_external_ids);
    ovsdb_idl_omit(ovnsb_idl_loop.idl, &sbrec_port_binding_col_external_ids);
    ovsdb_idl_omit(ovnsb_idl_loop.idl, &sbrec_ssl_col_external_ids);
    ovsdb_idl_omit(ovnsb_idl_loop.idl,
                   &sbrec_gateway_chassis_col_external_ids);
    ovsdb_idl_omit(ovnsb_idl_loop.idl, &sbrec_ha_chassis_col_external_ids);
    ovsdb_idl_omit(ovnsb_idl_loop.idl,
                   &sbrec_ha_chassis_group_col_external_ids);
    ovsdb_idl_omit(ovnsb_idl_loop.idl,
                   &sbrec_advertised_route_col_external_ids);
    ovsdb_idl_omit(ovnsb_idl_loop.idl,
                   &sbrec_learned_route_col_external_ids);
    ovsdb_idl_omit(ovnsb_idl_loop.idl,
                   &sbrec_advertised_mac_binding_col_external_ids);

    /* We don't want to monitor Connection table at all. So omit all the
     * columns. */
    ovsdb_idl_omit(ovnsb_idl_loop.idl, &sbrec_connection_col_external_ids);
    ovsdb_idl_omit(ovnsb_idl_loop.idl, &sbrec_connection_col_inactivity_probe);
    ovsdb_idl_omit(ovnsb_idl_loop.idl, &sbrec_connection_col_is_connected);
    ovsdb_idl_omit(ovnsb_idl_loop.idl, &sbrec_connection_col_max_backoff);
    ovsdb_idl_omit(ovnsb_idl_loop.idl, &sbrec_connection_col_other_config);
    ovsdb_idl_omit(ovnsb_idl_loop.idl, &sbrec_connection_col_read_only);
    ovsdb_idl_omit(ovnsb_idl_loop.idl, &sbrec_connection_col_role);
    ovsdb_idl_omit(ovnsb_idl_loop.idl, &sbrec_connection_col_status);
    ovsdb_idl_omit(ovnsb_idl_loop.idl, &sbrec_connection_col_target);

    /* Omit alerts to the Chassis external_ids column, the configuration
     * from the local open_vswitch table has now being moved to the
     * other_config column so we no longer need to monitor it */
    ovsdb_idl_omit_alert(ovnsb_idl_loop.idl, &sbrec_chassis_col_external_ids);

    /* Do not monitor Chassis_Private external_ids */
    ovsdb_idl_omit(ovnsb_idl_loop.idl,
                   &sbrec_chassis_private_col_external_ids);

    update_sb_monitors(ovnsb_idl_loop.idl, NULL, NULL, NULL, NULL, false);

    stopwatch_create(CONTROLLER_LOOP_STOPWATCH_NAME, SW_MS);
    stopwatch_create(OFCTRL_PUT_STOPWATCH_NAME, SW_MS);
    stopwatch_create(PINCTRL_RUN_STOPWATCH_NAME, SW_MS);
    stopwatch_create(PATCH_RUN_STOPWATCH_NAME, SW_MS);
    stopwatch_create(CT_ZONE_COMMIT_STOPWATCH_NAME, SW_MS);
    stopwatch_create(IF_STATUS_MGR_RUN_STOPWATCH_NAME, SW_MS);
    stopwatch_create(IF_STATUS_MGR_UPDATE_STOPWATCH_NAME, SW_MS);
    stopwatch_create(OFCTRL_SEQNO_RUN_STOPWATCH_NAME, SW_MS);
    stopwatch_create(BFD_RUN_STOPWATCH_NAME, SW_MS);
    stopwatch_create(VIF_PLUG_RUN_STOPWATCH_NAME, SW_MS);

    /* Define inc-proc-engine nodes. */
    ENGINE_NODE(sb_ro);
    ENGINE_NODE(template_vars, CLEAR_TRACKED_DATA);
    ENGINE_NODE(ct_zones, CLEAR_TRACKED_DATA, IS_VALID);
    ENGINE_NODE(ovs_interface_shadow, CLEAR_TRACKED_DATA);
    ENGINE_NODE(runtime_data, CLEAR_TRACKED_DATA, SB_WRITE);
    ENGINE_NODE(non_vif_data);
    ENGINE_NODE(mff_ovn_geneve);
    ENGINE_NODE(ofctrl_is_connected);
    ENGINE_NODE(activated_ports, CLEAR_TRACKED_DATA);
    ENGINE_NODE(postponed_ports);
    ENGINE_NODE(pflow_output);
    ENGINE_NODE(lflow_output, CLEAR_TRACKED_DATA);
    ENGINE_NODE(controller_output);
    ENGINE_NODE(addr_sets, CLEAR_TRACKED_DATA);
    ENGINE_NODE(port_groups, CLEAR_TRACKED_DATA);
    ENGINE_NODE(northd_options);
    ENGINE_NODE(dhcp_options);
    ENGINE_NODE(if_status_mgr);
    ENGINE_NODE(lb_data, CLEAR_TRACKED_DATA);
    ENGINE_NODE(mac_cache);
    ENGINE_NODE(bfd_chassis);
    ENGINE_NODE(dns_cache);
    ENGINE_NODE(acl_id, IS_VALID);
    ENGINE_NODE(route);
    ENGINE_NODE(route_table_notify);
    ENGINE_NODE(route_exchange, SB_WRITE);
    ENGINE_NODE(route_exchange_status);
    ENGINE_NODE(garp_rarp, SB_WRITE);
    ENGINE_NODE(host_if_monitor);
    ENGINE_NODE(neighbor);
    ENGINE_NODE(neighbor_table_notify);
    ENGINE_NODE(neighbor_exchange);
    ENGINE_NODE(neighbor_exchange_status);
    ENGINE_NODE(evpn_vtep_binding, CLEAR_TRACKED_DATA);
    ENGINE_NODE(evpn_fdb, CLEAR_TRACKED_DATA);
    ENGINE_NODE(evpn_arp, CLEAR_TRACKED_DATA);

#define SB_NODE(NAME) ENGINE_NODE_SB(NAME);
    SB_NODES
#undef SB_NODE

#define OVS_NODE(NAME) ENGINE_NODE_OVS(NAME);
    OVS_NODES
#undef OVS_NODE

    /* Add dependencies between inc-proc-engine nodes. */
    engine_add_input(&en_template_vars, &en_ovs_open_vswitch, NULL);
    engine_add_input(&en_template_vars, &en_sb_chassis, NULL);
    engine_add_input(&en_template_vars, &en_sb_chassis_template_var,
                     template_vars_sb_chassis_template_var_handler);

    engine_add_input(&en_lb_data, &en_sb_load_balancer,
                     lb_data_sb_load_balancer_handler);
    engine_add_input(&en_lb_data, &en_template_vars,
                     lb_data_template_var_handler);
    engine_add_input(&en_lb_data, &en_runtime_data,
                     lb_data_runtime_data_handler);

    engine_add_input(&en_route, &en_ovs_open_vswitch, NULL);
    engine_add_input(&en_route, &en_sb_chassis, NULL);
    engine_add_input(&en_route, &en_sb_port_binding,
                     route_sb_port_binding_data_handler);
    engine_add_input(&en_route, &en_runtime_data,
                     route_runtime_data_handler);
    engine_add_input(&en_route, &en_sb_advertised_route,
                     route_sb_advertised_route_data_handler);

    engine_add_input(&en_route_exchange, &en_route, NULL);
    engine_add_input(&en_route_exchange, &en_sb_learned_route,
                     engine_noop_handler);
    engine_add_input(&en_route_exchange, &en_sb_port_binding,
                     engine_noop_handler);
    engine_add_input(&en_route_exchange, &en_route_table_notify, NULL);
    engine_add_input(&en_route_exchange, &en_route_exchange_status, NULL);
    engine_add_input(&en_route_exchange, &en_sb_ro,
                     route_exchange_sb_ro_handler);

    engine_add_input(&en_addr_sets, &en_sb_address_set,
                     addr_sets_sb_address_set_handler);
    engine_add_input(&en_port_groups, &en_sb_port_group,
                     port_groups_sb_port_group_handler);
    /* port_groups computation requires runtime_data's lbinding_data for the
     * locally bound ports. */
    engine_add_input(&en_port_groups, &en_runtime_data,
                     port_groups_runtime_data_handler);

    engine_add_input(&en_non_vif_data, &en_ovs_open_vswitch, NULL);
    engine_add_input(&en_non_vif_data, &en_ovs_bridge, NULL);
    engine_add_input(&en_non_vif_data, &en_sb_chassis, NULL);
    engine_add_input(&en_non_vif_data, &en_ovs_interface,
                     non_vif_data_ovs_iface_handler);

    engine_add_input(&en_if_status_mgr, &en_ovs_interface,
                     if_status_mgr_ovs_interface_handler);
    engine_add_input(&en_bfd_chassis, &en_ovs_open_vswitch, NULL);
    engine_add_input(&en_bfd_chassis, &en_sb_chassis, NULL);
    engine_add_input(&en_bfd_chassis, &en_sb_ha_chassis_group, NULL);

    /* Note: The order of inputs is important, all OVS interface changes must
     * be handled before any ct_zone changes.
     */
    engine_add_input(&en_pflow_output, &en_non_vif_data,
                     NULL);
    engine_add_input(&en_pflow_output, &en_northd_options, NULL);
    engine_add_input(&en_pflow_output, &en_ct_zones,
                     pflow_output_ct_zones_handler);
    engine_add_input(&en_pflow_output, &en_sb_chassis,
                     pflow_lflow_output_sb_chassis_handler);

    engine_add_input(&en_pflow_output, &en_if_status_mgr,
                     pflow_output_if_status_mgr_handler);
    engine_add_input(&en_pflow_output, &en_sb_port_binding,
                     pflow_output_sb_port_binding_handler);
    engine_add_input(&en_pflow_output, &en_sb_multicast_group,
                     pflow_output_sb_multicast_group_handler);

    /* pflow_output needs to access the SB datapath binding and hence a noop
     * handler.
     */
    engine_add_input(&en_pflow_output, &en_sb_datapath_binding,
                     engine_noop_handler);
    engine_add_input(&en_pflow_output, &en_activated_ports,
                     pflow_output_activated_ports_handler);

    engine_add_input(&en_pflow_output, &en_runtime_data,
                     pflow_output_runtime_data_handler);
    engine_add_input(&en_pflow_output, &en_sb_encap, NULL);
    engine_add_input(&en_pflow_output, &en_mff_ovn_geneve, NULL);
    engine_add_input(&en_pflow_output, &en_ovs_open_vswitch, NULL);
    engine_add_input(&en_pflow_output, &en_ovs_bridge, NULL);
    engine_add_input(&en_pflow_output, &en_ovs_flow_sample_collector_set,
                     pflow_output_debug_handler);
    engine_add_input(&en_pflow_output, &en_sb_sb_global,
                     pflow_output_debug_handler);

    engine_add_input(&en_northd_options, &en_sb_sb_global,
                     en_northd_options_sb_sb_global_handler);

    engine_add_input(&en_dhcp_options, &en_sb_dhcp_options, NULL);
    engine_add_input(&en_dhcp_options, &en_sb_dhcpv6_options, NULL);

    engine_add_input(&en_lflow_output, &en_northd_options, NULL);
    engine_add_input(&en_lflow_output, &en_dhcp_options, NULL);

    /* Keep en_addr_sets before en_runtime_data because
     * lflow_output_runtime_data_handler may *partially* reprocess a lflow when
     * the lflow is attached to a DP group and a new DP in that DP group is
     * added locally, i.e. reprocessing the lflow for the new DP only but not
     * for the other DPs in the group. If we handle en_addr_sets after this,
     * incrementally processing an updated address set for the added IPs may
     * end up adding redundant flows/conjunctions for the lflow agaist the new
     * DP because it has been processed on the DP already. */
    engine_add_input(&en_lflow_output, &en_addr_sets,
                     lflow_output_addr_sets_handler);
    engine_add_input(&en_lflow_output, &en_port_groups,
                     lflow_output_port_groups_handler);
    engine_add_input(&en_lflow_output, &en_template_vars,
                     lflow_output_template_vars_handler);
    engine_add_input(&en_lflow_output, &en_runtime_data,
                     lflow_output_runtime_data_handler);
    engine_add_input(&en_lflow_output, &en_non_vif_data,
                     NULL);

    engine_add_input(&en_lflow_output, &en_sb_multicast_group,
                     lflow_output_sb_multicast_group_handler);

    engine_add_input(&en_lflow_output, &en_sb_chassis,
                     pflow_lflow_output_sb_chassis_handler);

    engine_add_input(&en_lflow_output, &en_sb_port_binding,
                     lflow_output_sb_port_binding_handler);

    engine_add_input(&en_lflow_output, &en_ovs_open_vswitch, NULL);
    engine_add_input(&en_lflow_output, &en_ovs_bridge, NULL);
    engine_add_input(&en_lflow_output, &en_ovs_flow_sample_collector_set,
                     lflow_output_flow_sample_collector_set_handler);

    engine_add_input(&en_lflow_output, &en_sb_mac_binding,
                     lflow_output_sb_mac_binding_handler);
    engine_add_input(&en_lflow_output, &en_sb_static_mac_binding,
                     lflow_output_sb_static_mac_binding_handler);
    engine_add_input(&en_lflow_output, &en_sb_logical_flow,
                     lflow_output_sb_logical_flow_handler);
    /* Using a noop handler since we don't really need any data from datapath
     * groups or a full recompute.  Update of a datapath group will put
     * logical flow into the tracked list, so the logical flow handler will
     * process all changes. */
    engine_add_input(&en_lflow_output, &en_sb_logical_dp_group,
                     engine_noop_handler);

    engine_add_input(&en_lflow_output, &en_lb_data,
                     lflow_output_lb_data_handler);
    engine_add_input(&en_lflow_output, &en_sb_fdb,
                     lflow_output_sb_fdb_handler);
    engine_add_input(&en_lflow_output, &en_sb_meter,
                     lflow_output_sb_meter_handler);

    engine_add_input(&en_ct_zones, &en_ovs_open_vswitch, NULL);
    engine_add_input(&en_ct_zones, &en_ovs_bridge, NULL);
    engine_add_input(&en_ct_zones, &en_sb_datapath_binding,
                     ct_zones_datapath_binding_handler);
    engine_add_input(&en_ct_zones, &en_runtime_data,
                     ct_zones_runtime_data_handler);

    engine_add_input(&en_ovs_interface_shadow, &en_ovs_interface,
                     ovs_interface_shadow_ovs_interface_handler);

    engine_add_input(&en_runtime_data, &en_ofctrl_is_connected, NULL);

    engine_add_input(&en_runtime_data, &en_ovs_open_vswitch, NULL);
    engine_add_input(&en_runtime_data, &en_ovs_bridge, NULL);
    engine_add_input(&en_runtime_data, &en_ovs_qos, NULL);
    engine_add_input(&en_runtime_data, &en_ovs_queue, NULL);

    engine_add_input(&en_runtime_data, &en_sb_chassis, NULL);
    engine_add_input(&en_runtime_data, &en_sb_datapath_binding,
                     runtime_data_sb_datapath_binding_handler);
    engine_add_input(&en_runtime_data, &en_sb_port_binding,
                     runtime_data_sb_port_binding_handler);
    /* Reuse the same handler for any previously postponed ports. */
    engine_add_input(&en_runtime_data, &en_postponed_ports,
                     runtime_data_sb_port_binding_handler);
    /* Run sb_ro_handler after port_binding_handler in case port get deleted */
    engine_add_input(&en_runtime_data, &en_sb_ro, runtime_data_sb_ro_handler);

    /* The OVS interface handler for runtime_data changes MUST be executed
     * after the sb_port_binding_handler as port_binding deletes must be
     * processed first.
     *
     * runtime_data needs to access the OVS Port data and hence a noop
     * handler.
     */
    engine_add_input(&en_runtime_data, &en_ovs_port,
                     engine_noop_handler);
    engine_add_input(&en_runtime_data, &en_ovs_interface_shadow,
                     runtime_data_ovs_interface_shadow_handler);

    engine_add_input(&en_mac_cache, &en_runtime_data,
                     mac_cache_runtime_data_handler);
    engine_add_input(&en_mac_cache, &en_sb_mac_binding,
                     mac_cache_sb_mac_binding_handler);
    engine_add_input(&en_mac_cache, &en_sb_fdb,
                     mac_cache_sb_fdb_handler);
    engine_add_input(&en_mac_cache, &en_sb_datapath_binding,
                     mac_cache_sb_datapath_binding_handler);
    engine_add_input(&en_mac_cache, &en_sb_port_binding,
                     engine_noop_handler);

    engine_add_input(&en_dns_cache, &en_sb_dns,
                     dns_cache_sb_dns_handler);

    engine_add_input(&en_garp_rarp, &en_ovs_open_vswitch, NULL);
    engine_add_input(&en_garp_rarp, &en_sb_chassis, NULL);
    engine_add_input(&en_garp_rarp, &en_sb_port_binding,
                     garp_rarp_sb_port_binding_handler);
    engine_add_input(&en_garp_rarp, &en_sb_datapath_binding,
                     garp_rarp_sb_datapath_binding_handler);
    /* The mac_binding data is just used in an index to filter duplicates when
     * inserting data to the southbound. */
    engine_add_input(&en_garp_rarp, &en_sb_mac_binding, engine_noop_handler);
    engine_add_input(&en_garp_rarp, &en_runtime_data,
                     garp_rarp_runtime_data_handler);

    engine_add_input(&en_neighbor, &en_ovs_open_vswitch, NULL);
    engine_add_input(&en_neighbor, &en_sb_chassis, NULL);
    engine_add_input(&en_neighbor, &en_sb_advertised_mac_binding, NULL);
    engine_add_input(&en_neighbor, &en_runtime_data,
                     neighbor_runtime_data_handler);
    engine_add_input(&en_neighbor, &en_sb_datapath_binding,
                     neighbor_sb_datapath_binding_handler);
    engine_add_input(&en_neighbor, &en_sb_port_binding,
                     neighbor_sb_port_binding_handler);
    engine_add_input(&en_neighbor_exchange, &en_neighbor, NULL);
    engine_add_input(&en_neighbor_exchange, &en_host_if_monitor, NULL);
    engine_add_input(&en_neighbor_exchange, &en_neighbor_table_notify, NULL);
    engine_add_input(&en_neighbor_exchange, &en_neighbor_exchange_status,
                     NULL);

    engine_add_input(&en_evpn_vtep_binding, &en_ovs_open_vswitch, NULL);
    engine_add_input(&en_evpn_vtep_binding, &en_ovs_bridge, NULL);
    engine_add_input(&en_evpn_vtep_binding, &en_neighbor_exchange, NULL);
    /* The runtime_data are needed only for local datapaths, any update of
     * local datapath will be reflected via en_neighbor_exchange. */
    engine_add_input(&en_evpn_vtep_binding, &en_runtime_data,
                     engine_noop_handler);
    engine_add_input(&en_evpn_vtep_binding, &en_ovs_interface,
                     evpn_vtep_binding_ovs_interface_handler);
    engine_add_input(&en_evpn_vtep_binding, &en_sb_datapath_binding,
                     evpn_vtep_binding_datapath_binding_handler);

    engine_add_input(&en_evpn_fdb, &en_neighbor_exchange, NULL);
    engine_add_input(&en_evpn_fdb, &en_evpn_vtep_binding,
                     evpn_fdb_vtep_binding_handler);

    engine_add_input(&en_evpn_arp, &en_neighbor_exchange, NULL);
    engine_add_input(&en_evpn_arp, &en_evpn_vtep_binding,
                     evpn_arp_vtep_binding_handler);

    engine_add_input(&en_pflow_output, &en_evpn_vtep_binding,
                     pflow_output_evpn_binding_handler);
    engine_add_input(&en_pflow_output, &en_evpn_fdb,
                     pflow_output_fdb_handler);
    engine_add_input(&en_pflow_output, &en_evpn_arp,
                     pflow_output_arp_handler);

    engine_add_input(&en_controller_output, &en_dns_cache,
                     NULL);
    engine_add_input(&en_controller_output, &en_lflow_output,
                     controller_output_lflow_output_handler);
    engine_add_input(&en_controller_output, &en_pflow_output,
                     controller_output_pflow_output_handler);
    engine_add_input(&en_controller_output, &en_mac_cache,
                     controller_output_mac_cache_handler);
    engine_add_input(&en_controller_output, &en_bfd_chassis,
                     controller_output_bfd_chassis_handler);
    engine_add_input(&en_controller_output, &en_route_exchange,
                     controller_output_route_exchange_handler);
    engine_add_input(&en_controller_output, &en_garp_rarp,
                     controller_output_garp_rarp_handler);

    engine_add_input(&en_acl_id, &en_sb_acl_id, NULL);
    engine_add_input(&en_controller_output, &en_acl_id,
                     controller_output_acl_id_handler);

    struct engine_arg engine_arg = {
        .sb_idl = ovnsb_idl_loop.idl,
        .ovs_idl = ovs_idl_loop.idl,
    };
    engine_init(&en_controller_output, &engine_arg);

    engine_ovsdb_node_add_index(&en_sb_chassis, "name", sbrec_chassis_by_name);
    engine_ovsdb_node_add_index(&en_sb_multicast_group, "name_datapath",
                                sbrec_multicast_group_by_name_datapath);
    engine_ovsdb_node_add_index(&en_sb_logical_flow, "logical_datapath",
                                sbrec_logical_flow_by_logical_datapath);
    engine_ovsdb_node_add_index(&en_sb_logical_flow, "logical_dp_group",
                                sbrec_logical_flow_by_logical_dp_group);
    engine_ovsdb_node_add_index(&en_sb_port_binding, "name",
                                sbrec_port_binding_by_name);
    engine_ovsdb_node_add_index(&en_sb_port_binding, "key",
                                sbrec_port_binding_by_key);
    engine_ovsdb_node_add_index(&en_sb_port_binding, "datapath",
                                sbrec_port_binding_by_datapath);
    engine_ovsdb_node_add_index(&en_sb_datapath_binding, "key",
                                sbrec_datapath_binding_by_key);
    engine_ovsdb_node_add_index(&en_sb_fdb, "dp_key",
                                sbrec_fdb_by_dp_key);
    engine_ovsdb_node_add_index(&en_sb_mac_binding, "datapath",
                                sbrec_mac_binding_by_datapath);
    engine_ovsdb_node_add_index(&en_sb_static_mac_binding, "datapath",
                                sbrec_static_mac_binding_by_datapath);
    engine_ovsdb_node_add_index(&en_sb_chassis_template_var, "chassis",
                                sbrec_chassis_template_var_index_by_chassis);
    engine_ovsdb_node_add_index(&en_sb_learned_route, "datapath",
                                sbrec_learned_route_index_by_datapath);
    engine_ovsdb_node_add_index(&en_sb_advertised_mac_binding, "datapath",
                                sbrec_advertised_mac_binding_index_by_dp);
    engine_ovsdb_node_add_index(&en_sb_mac_binding, "lport_ip",
                                sbrec_mac_binding_by_lport_ip);
    engine_ovsdb_node_add_index(&en_ovs_flow_sample_collector_set, "id",
                                ovsrec_flow_sample_collector_set_by_id);
    engine_ovsdb_node_add_index(&en_ovs_port, "qos", ovsrec_port_by_qos);
    engine_ovsdb_node_add_index(&en_ovs_interface, "name",
                                ovsrec_interface_by_name);
    engine_ovsdb_node_add_index(&en_ovs_queue, "external_ids",
                                ovsrec_queue_by_external_ids);

    struct ed_type_lflow_output *lflow_output_data =
        engine_get_internal_data(&en_lflow_output);
    struct ed_type_pflow_output *pflow_output_data =
        engine_get_internal_data(&en_pflow_output);
    struct ed_type_ct_zones *ct_zones_data =
        engine_get_internal_data(&en_ct_zones);
    struct ed_type_bfd_chassis *bfd_chassis_data =
        engine_get_internal_data(&en_bfd_chassis);
    struct ed_type_runtime_data *runtime_data =
        engine_get_internal_data(&en_runtime_data);
    struct ed_type_template_vars *template_vars_data =
        engine_get_internal_data(&en_template_vars);
    struct ed_type_lb_data *lb_data =
        engine_get_internal_data(&en_lb_data);
    struct mac_cache_data *mac_cache_data =
            engine_get_internal_data(&en_mac_cache);
    struct ed_type_neighbor_exchange *ne_data =
        engine_get_internal_data(&en_neighbor_exchange);
    struct ed_type_evpn_vtep_binding *eb_data =
        engine_get_internal_data(&en_evpn_vtep_binding);
    struct ed_type_evpn_fdb *efdb_data =
        engine_get_internal_data(&en_evpn_fdb);
    struct ed_type_evpn_arp *earp_data =
        engine_get_internal_data(&en_evpn_arp);

    ofctrl_init(&lflow_output_data->group_table,
                &lflow_output_data->meter_table);

    unixctl_command_register("group-table-list", "", 0, 0,
                             extend_table_list,
                             &lflow_output_data->group_table);

    unixctl_command_register("meter-table-list", "", 0, 0,
                             extend_table_list,
                             &lflow_output_data->meter_table);

    unixctl_command_register("ct-zone-list", "", 0, 0,
                             ct_zone_list,
                             &ct_zones_data->ctx.current);

    unixctl_command_register("evpn/remote-vtep-list", "", 0, 0,
                             evpn_remote_vtep_list,
                             &ne_data->remote_vteps);
    unixctl_command_register("evpn/vtep-binding-list", "", 0, 0,
                             evpn_vtep_binding_list,
                             &eb_data->bindings);
    unixctl_command_register("evpn/vtep-multicast-group-list", "", 0, 0,
                             evpn_multicast_group_list,
                             &eb_data->multicast_groups);
    unixctl_command_register("evpn/vtep-fdb-list", "", 0, 0,
                             evpn_fdb_list,
                             &efdb_data->fdbs);
    unixctl_command_register("evpn/vtep-arp-list", "", 0, 0,
                             evpn_arp_list,
                             &earp_data->arps);

    struct pending_pkt pending_pkt = { .conn = NULL };
    unixctl_command_register("inject-pkt", "MICROFLOW", 1, 1, inject_pkt,
                             &pending_pkt);

    unixctl_command_register("recompute", "[deprecated]", 0, 0,
                             engine_recompute_cmd,
                             NULL);
    unixctl_command_register("lflow-cache/flush", "", 0, 0,
                             lflow_cache_flush_cmd,
                             &lflow_output_data->pd);
    /* Keep deprecated 'flush-lflow-cache' command for now. */
    unixctl_command_register("flush-lflow-cache", "[deprecated]", 0, 0,
                             lflow_cache_flush_cmd,
                             &lflow_output_data->pd);
    unixctl_command_register("lflow-cache/show-stats", "", 0, 0,
                             lflow_cache_show_stats_cmd,
                             &lflow_output_data->pd);

    bool reset_ovnsb_idl_min_index = false;
    unixctl_command_register("sb-cluster-state-reset", "", 0, 0,
                             cluster_state_reset_cmd,
                             &reset_ovnsb_idl_min_index);

    bool paused = false;
    unixctl_command_register("debug/pause", "", 0, 0, debug_pause_execution,
                             &paused);
    unixctl_command_register("debug/resume", "", 0, 0, debug_resume_execution,
                             &paused);
    unixctl_command_register("debug/status", "", 0, 0, debug_status_execution,
                             &paused);

    unsigned int delay_nb_cfg_report = 0;
    unixctl_command_register("debug/delay-nb-cfg-report", "SECONDS", 1, 1,
                             debug_delay_nb_cfg_report, &delay_nb_cfg_report);

    unixctl_command_register("debug/dump-local-bindings", "", 0, 0,
                             debug_dump_local_bindings,
                             &runtime_data->lbinding_data);

    unixctl_command_register("debug/dump-local-datapaths", "", 0, 0,
                             debug_dump_local_datapaths,
                             &runtime_data->local_datapaths);

    unixctl_command_register("debug/dump-related-ports", "", 0, 0,
                             debug_dump_related_lports,
                             &runtime_data->related_lports);

    unixctl_command_register("debug/dump-lflow-conj-ids", "", 0, 0,
                             debug_dump_lflow_conj_ids,
                             &lflow_output_data->conj_ids);

    unixctl_command_register("debug/dump-peer-ports", "", 0, 0,
                             debug_dump_peer_ports,
                             &runtime_data->local_datapaths);

    unixctl_command_register("debug/dump-local-template-vars", "", 0, 0,
                             debug_dump_local_template_vars,
                             &template_vars_data->local_templates);

    unixctl_command_register("debug/dump-mac-bindings", "", 0, 0,
                             debug_dump_local_mac_bindings,
                             &mac_cache_data->mac_bindings);
    ovn_debug_commands_register();

    unsigned int ovs_cond_seqno = UINT_MAX;
    unsigned int ovnsb_cond_seqno = UINT_MAX;
    unsigned int ovnsb_expected_cond_seqno = UINT_MAX;

    struct controller_engine_ctx ctrl_engine_ctx = {
        .lflow_cache = lflow_cache_create(),
        .if_mgr = if_status_mgr_create(),
    };
    struct if_status_mgr *if_mgr = ctrl_engine_ctx.if_mgr;

    struct shash vif_plug_deleted_iface_ids =
        SHASH_INITIALIZER(&vif_plug_deleted_iface_ids);
    struct shash vif_plug_changed_iface_ids =
        SHASH_INITIALIZER(&vif_plug_changed_iface_ids);

    char *ovn_version = ovn_get_internal_version();
    VLOG_INFO("OVN internal version is : [%s]", ovn_version);

    /* Main loop. */
    int ovnsb_txn_status = 1;
    bool sb_monitor_all = false;
    struct tracked_acl_ids *tracked_acl_ids = NULL;
    while (!exit_args.exiting) {
        ovsrcu_quiesce_end();

        memory_run();
        if (memory_should_report()) {
            struct simap usage = SIMAP_INITIALIZER(&usage);

            lflow_cache_get_memory_usage(ctrl_engine_ctx.lflow_cache, &usage);
            ofctrl_get_memory_usage(&usage);
            if_status_mgr_get_memory_usage(if_mgr, &usage);
            local_datapath_memory_usage(&usage);
            ovsdb_idl_get_memory_usage(ovnsb_idl_loop.idl, &usage);
            ovsdb_idl_get_memory_usage(ovs_idl_loop.idl, &usage);
            memory_report(&usage);
            simap_destroy(&usage);
        }

        /* If we're paused just run the unixctl server and skip most of the
         * processing loop.
         */
        if (paused) {
            unixctl_server_run(unixctl);
            unixctl_server_wait(unixctl);
            goto loop_done;
        }

        engine_init_run();

        struct ovsdb_idl_txn *ovs_idl_txn = ovsdb_idl_loop_run(&ovs_idl_loop);
        unsigned int new_ovs_cond_seqno
            = ovsdb_idl_get_condition_seqno(ovs_idl_loop.idl);
        if (new_ovs_cond_seqno != ovs_cond_seqno) {
            if (!new_ovs_cond_seqno) {
                VLOG_INFO("OVS IDL reconnected, force recompute.");
                engine_set_force_recompute();
            }
            ovs_cond_seqno = new_ovs_cond_seqno;
        }

        update_sb_db(ovs_idl_loop.idl, ovnsb_idl_loop.idl, &sb_monitor_all,
                     &reset_ovnsb_idl_min_index,
                     &ctrl_engine_ctx, &ovnsb_expected_cond_seqno);
        update_ssl_config(ovsrec_ssl_table_get(ovs_idl_loop.idl));

        struct ovsdb_idl_txn *ovnsb_idl_txn
            = ovsdb_idl_loop_run(&ovnsb_idl_loop);
        unsigned int new_ovnsb_cond_seqno
            = ovsdb_idl_get_condition_seqno(ovnsb_idl_loop.idl);
        if (ovnsb_idl_txn && ovnsb_txn_status == -1) {
            ovsdb_idl_loop_next_cfg_inc(&ovnsb_idl_loop);
        }
        if (new_ovnsb_cond_seqno != ovnsb_cond_seqno) {
            if (!new_ovnsb_cond_seqno) {
                VLOG_INFO("OVNSB IDL reconnected, force recompute.");
                engine_set_force_recompute();
            }
            ovnsb_cond_seqno = new_ovnsb_cond_seqno;
        }

        /* Check if we have received all initial dumps of the southbound
         * based on the monitor condtions we set.
         * If we have sb_monitor_all that means we have all data that we would
         * ever need.
         * In other cases we depend on engine runs. This is handled below. */
        if (ovnsb_cond_seqno == ovnsb_expected_cond_seqno
            && ovnsb_expected_cond_seqno != UINT_MAX && sb_monitor_all) {
            daemon_started_recently_ignore();
        }

        struct engine_context eng_ctx = {
            .ovs_idl_txn = ovs_idl_txn,
            .ovnsb_idl_txn = ovnsb_idl_txn,
            .client_ctx = &ctrl_engine_ctx
        };

        engine_set_context(&eng_ctx);

        bool northd_version_match =
            check_northd_version(ovs_idl_loop.idl, ovnsb_idl_loop.idl,
                                 ovn_version);

        const struct ovsrec_bridge_table *bridge_table =
            ovsrec_bridge_table_get(ovs_idl_loop.idl);
        const struct ovsrec_open_vswitch_table *ovs_table =
            ovsrec_open_vswitch_table_get(ovs_idl_loop.idl);
        const struct ovsrec_bridge *br_int = NULL;
        const struct ovsrec_datapath *br_int_dp = NULL;
        const struct ovsrec_open_vswitch *cfg =
            ovsrec_open_vswitch_table_first(ovs_table);
        process_br_int(ovs_idl_txn, bridge_table, ovs_table, &br_int,
                       ovsrec_server_has_datapath_table(ovs_idl_loop.idl)
                       ? &br_int_dp
                       : NULL);
        br_int_remote_update(&br_int_remote, br_int, ovs_table);
        statctrl_update_swconn(br_int_remote.target,
                               br_int_remote.probe_interval);
        pinctrl_update_swconn(br_int_remote.target,
                              br_int_remote.probe_interval);

        /* Enable ACL matching for double tagged traffic. */
        if (ovs_idl_txn && cfg) {
            int vlan_limit = smap_get_int(
                &cfg->other_config, "vlan-limit", -1);
            if (vlan_limit != 0) {
                ovsrec_open_vswitch_update_other_config_setkey(
                    cfg, "vlan-limit", "0");
            }
        }

        static bool chassis_idx_stored = false;
        if (ovs_idl_txn && !chassis_idx_stored) {
            store_chassis_index_if_needed(ovs_table);
            chassis_idx_stored = true;
        }

        if (ovsdb_idl_has_ever_connected(ovnsb_idl_loop.idl) &&
            northd_version_match && cfg) {

            /* Unconditionally remove all deleted lflows from the lflow
             * cache.
             */
            if (lflow_cache_is_enabled(ctrl_engine_ctx.lflow_cache)) {
                lflow_handle_cached_flows(
                    ctrl_engine_ctx.lflow_cache,
                    sbrec_logical_flow_table_get(ovnsb_idl_loop.idl));
            }

            /* Contains the transport zones that this Chassis belongs to */
            struct sset transport_zones = SSET_INITIALIZER(&transport_zones);
            sset_from_delimited_string(&transport_zones,
                get_transport_zones(ovsrec_open_vswitch_table_get(
                                    ovs_idl_loop.idl)), ",");

            const char *chassis_id = get_ovs_chassis_id(ovs_table);
            const struct sbrec_chassis *chassis = NULL;
            const struct sbrec_chassis_private *chassis_private = NULL;
            if (chassis_id) {
                chassis = chassis_run(ovnsb_idl_txn, sbrec_chassis_by_name,
                                      sbrec_chassis_private_by_name,
                                      ovs_table, chassis_id,
                                      br_int, &transport_zones,
                                      &chassis_private, sbrec_encaps);
            }

            /* If any OVS feature support changed, force a full recompute.
             * 'br_int_dp' is valid only if an OVS transaction is possible.
             */
            if (ovs_idl_txn
                && ovs_feature_support_run(br_int_dp ?
                                           &br_int_dp->capabilities : NULL,
                                           br_int_remote.target,
                                           br_int_remote.probe_interval)) {
                VLOG_INFO("OVS feature set changed, force recompute.");
                engine_set_force_recompute();

                struct ed_type_lflow_output *lflow_out_data =
                    engine_get_internal_data(&en_lflow_output);

                ovn_extend_table_reinit(&lflow_out_data->group_table,
                                        ovs_feature_max_select_groups_get());
                ovn_extend_table_reinit(&lflow_out_data->meter_table,
                                        ovs_feature_max_meters_get());
            }

            if (br_int) {
                ct_zones_data = engine_get_data(&en_ct_zones);
                if (ofctrl_run(br_int_remote.target,
                               br_int_remote.probe_interval, ovs_table,
                               ct_zones_data ? &ct_zones_data->ctx.pending
                                             : NULL,
                               tracked_acl_ids)) {
                    static struct vlog_rate_limit rl
                            = VLOG_RATE_LIMIT_INIT(1, 1);

                    VLOG_INFO_RL(&rl, "OVS OpenFlow connection reconnected,"
                                      "force recompute.");
                    engine_set_force_recompute();
                }

                if (chassis && ovs_feature_set_discovered()) {
                    encaps_run(ovs_idl_txn, ovnsb_idl_txn, br_int,
                               sbrec_chassis_table_get(ovnsb_idl_loop.idl),
                               chassis,
                               sbrec_sb_global_first(ovnsb_idl_loop.idl),
                               ovs_table,
                               &transport_zones,
                               bridge_table);

                    struct ed_type_route_table_notify *rtn =
                        engine_get_internal_data(&en_route_table_notify);
                    rtn->changed = route_table_notify_run();

                    struct ed_type_host_if_monitor *hifm =
                        engine_get_internal_data(&en_host_if_monitor);
                    hifm->changed = host_if_monitor_run();

                    struct ed_type_neighbor_table_notify *ntn =
                        engine_get_internal_data(&en_neighbor_table_notify);
                    ntn->changed = neighbor_table_notify_run();

                    struct ed_type_route_exchange_status *rt_res =
                        engine_get_internal_data(&en_route_exchange_status);
                    rt_res->netlink_trigger_run =
                        !!route_exchange_status_run();

                    struct ed_type_neighbor_exchange_status *neigh_res =
                        engine_get_internal_data(&en_neighbor_exchange_status);
                    neigh_res->netlink_trigger_run =
                        !!neighbor_exchange_status_run();

                    stopwatch_start(CONTROLLER_LOOP_STOPWATCH_NAME,
                                    time_msec());

                    /* Recompute is not allowed in following cases: */
                    /* 1. No ovnsb_idl_txn  */
                    /* Even if there's no SB DB transaction available,
                    * try to run the engine so that we can handle any
                    * incremental changes that don't require a recompute.
                    * If a recompute is required, the engine will cancel,
                    * triggerring a full run in the next iteration.
                    */
                    /* 2. ofctrl_has_backlog */
                    /* When there are in-flight messages pending to
                     * ovs-vswitchd, we should hold on recomputing so
                     * that the previous flow installations won't be
                     * delayed.  However, we still want to try if
                     * recompute is not needed and we can quickly
                     * incrementally process the new changes, to avoid
                     * unnecessarily forced recomputes later on.  This
                     * is because the OVSDB change tracker cannot
                     * preserve tracked changes across iterations.  If
                     * change tracking is improved, we can simply skip
                     * this round of engine_run and continue processing
                     * acculated changes incrementally later when
                     * ofctrl_has_backlog() returns false. */

                    bool recompute_allowed = (ovnsb_idl_txn &&
                                              !ofctrl_has_backlog());
                    engine_run(recompute_allowed);
                    tracked_acl_ids = engine_get_data(&en_acl_id);

                    stopwatch_stop(CONTROLLER_LOOP_STOPWATCH_NAME,
                                   time_msec());

                    ct_zones_data = engine_get_data(&en_ct_zones);
                    bfd_chassis_data = engine_get_data(&en_bfd_chassis);
                    if (ovs_idl_txn) {
                        if (ct_zones_data) {
                            stopwatch_start(CT_ZONE_COMMIT_STOPWATCH_NAME,
                                            time_msec());
                            ct_zones_commit(br_int, br_int_dp, ovs_idl_txn,
                                            &ct_zones_data->ctx);
                            stopwatch_stop(CT_ZONE_COMMIT_STOPWATCH_NAME,
                                           time_msec());
                        }
                        if (bfd_chassis_data) {
                            stopwatch_start(
                                BFD_RUN_STOPWATCH_NAME, time_msec());
                            bfd_run(
                                ovsrec_interface_table_get(ovs_idl_loop.idl),
                                br_int, &bfd_chassis_data->bfd_chassis,
                                chassis, sbrec_sb_global_table_get(
                                    ovnsb_idl_loop.idl)
                            );
                            stopwatch_stop(
                                BFD_RUN_STOPWATCH_NAME, time_msec());
                        }
                    }

                    runtime_data = engine_get_data(&en_runtime_data);
                    if (runtime_data) {
                        stopwatch_start(PATCH_RUN_STOPWATCH_NAME, time_msec());
                        patch_run(ovs_idl_txn,
                            sbrec_port_binding_by_type,
                            ovsrec_bridge_table_get(ovs_idl_loop.idl),
                            ovsrec_open_vswitch_table_get(ovs_idl_loop.idl),
                            ovsrec_port_by_name,
                            br_int, chassis, &runtime_data->local_datapaths);
                        stopwatch_stop(PATCH_RUN_STOPWATCH_NAME, time_msec());
                        if (vif_plug_provider_has_providers() && ovs_idl_txn) {
                            struct vif_plug_ctx_in vif_plug_ctx_in = {
                                .ovs_idl_txn = ovs_idl_txn,
                                .sbrec_port_binding_by_name =
                                    sbrec_port_binding_by_name,
                                .sbrec_port_binding_by_requested_chassis =
                                    sbrec_port_binding_by_requested_chassis,
                                .ovsrec_port_by_interfaces =
                                    ovsrec_port_by_interfaces,
                                .ovsrec_interface_by_name =
                                    ovsrec_interface_by_name,
                                .ovs_table = ovs_table,
                                .br_int = br_int,
                                .iface_table =
                                    ovsrec_interface_table_get(
                                                    ovs_idl_loop.idl),
                                .chassis_rec = chassis,
                                .local_bindings =
                                        &runtime_data->lbinding_data.bindings,
                            };
                            struct vif_plug_ctx_out vif_plug_ctx_out = {
                                .deleted_iface_ids =
                                    &vif_plug_deleted_iface_ids,
                                .changed_iface_ids =
                                    &vif_plug_changed_iface_ids,
                            };
                            stopwatch_start(VIF_PLUG_RUN_STOPWATCH_NAME,
                                            time_msec());
                            vif_plug_run(&vif_plug_ctx_in,
                                         &vif_plug_ctx_out);
                            stopwatch_stop(VIF_PLUG_RUN_STOPWATCH_NAME,
                                           time_msec());
                        }
                        stopwatch_start(PINCTRL_RUN_STOPWATCH_NAME,
                                        time_msec());
                        pinctrl_update(ovnsb_idl_loop.idl);
                        pinctrl_run(ovnsb_idl_txn,
                                    sbrec_datapath_binding_by_key,
                                    sbrec_port_binding_by_key,
                                    sbrec_port_binding_by_name,
                                    sbrec_mac_binding_by_lport_ip,
                                    sbrec_igmp_group,
                                    sbrec_ip_multicast,
                                    sbrec_fdb_by_dp_key_mac,
                                    sbrec_controller_event_table_get(
                                        ovnsb_idl_loop.idl),
                                    sbrec_service_monitor_table_get(
                                        ovnsb_idl_loop.idl),
                                    sbrec_mac_binding_table_get(
                                        ovnsb_idl_loop.idl),
                                    sbrec_bfd_table_get(ovnsb_idl_loop.idl),
                                    sbrec_ecmp_nexthop_table_get(
                                        ovnsb_idl_loop.idl),
                                    chassis,
                                    &runtime_data->local_datapaths,
                                    &runtime_data->local_active_ports_ipv6_pd,
                                    &runtime_data->local_active_ports_ras,
                                    ovsrec_open_vswitch_table_get(
                                            ovs_idl_loop.idl),
                                    ovnsb_idl_loop.cur_cfg);
                        stopwatch_stop(PINCTRL_RUN_STOPWATCH_NAME,
                                       time_msec());
                        mirror_run(ovs_idl_txn,
                                   ovsrec_mirror_table_get(ovs_idl_loop.idl),
                                   sbrec_mirror_table_get(ovnsb_idl_loop.idl),
                                   br_int,
                                   &runtime_data->lbinding_data.bindings);
                        /* Updating monitor conditions if runtime data or
                         * logical datapath goups changed. */
                        if (engine_node_changed(&en_runtime_data)
                            || engine_node_changed(&en_sb_logical_dp_group)) {
                            bool had_all_data = ovnsb_cond_seqno ==
                                                ovnsb_expected_cond_seqno;
                            ovnsb_expected_cond_seqno =
                                update_sb_monitors(
                                    ovnsb_idl_loop.idl, chassis,
                                    &runtime_data->local_lports,
                                    &runtime_data->lbinding_data.bindings,
                                    &runtime_data->local_datapaths,
                                    sb_monitor_all);
                            bool condition_changed = ovnsb_cond_seqno !=
                                                     ovnsb_expected_cond_seqno;
                            if (had_all_data && condition_changed) {
                                /* We limit the amount of condition updates
                                 * that we treat as daemon_started_recently.
                                 * This allows us to proceed even if there is
                                 * a continuous reason for monitor updates. */
                                daemon_started_recently_countdown();
                            }
                        }
                        /* If there is no new expected seqno we have finished
                         * loading all needed data from southbound. We then
                         * need to run one more time since we might behave
                         * differently. */
                        if (daemon_started_recently()) {
                            bool condition_changed = ovnsb_cond_seqno !=
                                                     ovnsb_expected_cond_seqno;
                            if (!condition_changed) {
                                daemon_started_recently_ignore();
                                poll_immediate_wake();
                            }
                        }
                        if (ovs_idl_txn) {
                            update_qos(sbrec_port_binding_by_name, ovs_idl_txn,
                                       ovsrec_port_by_qos,
                                       ovsrec_qos_table_get(ovs_idl_loop.idl),
                                       &runtime_data->qos_map,
                                       ovs_table, bridge_table);
                        }
                    }

                    mac_cache_data = engine_get_data(&en_mac_cache);
                    if (mac_cache_data) {
                        statctrl_run(ovnsb_idl_txn, sbrec_port_binding_by_name,
                                     mac_cache_data);
                    }

                    ofctrl_seqno_update_create(
                        ofctrl_seq_type_nb_cfg,
                        get_nb_cfg(sbrec_sb_global_table_get(
                                                       ovnsb_idl_loop.idl),
                                              ovnsb_cond_seqno,
                                              ovnsb_expected_cond_seqno));

                    struct local_binding_data *binding_data =
                        runtime_data ? &runtime_data->lbinding_data : NULL;
                    stopwatch_start(IF_STATUS_MGR_UPDATE_STOPWATCH_NAME,
                                    time_msec());
                    if_status_mgr_update(if_mgr, binding_data, chassis,
                                         ovsrec_interface_table_get(
                                                    ovs_idl_loop.idl),
                                         sbrec_port_binding_table_get(
                                                    ovnsb_idl_loop.idl),
                                         !ovs_idl_txn,
                                         !ovnsb_idl_txn);
                    stopwatch_stop(IF_STATUS_MGR_UPDATE_STOPWATCH_NAME,
                                   time_msec());

                    lflow_output_data = engine_get_data(&en_lflow_output);
                    pflow_output_data = engine_get_data(&en_pflow_output);
                    lb_data = engine_get_data(&en_lb_data);
                    if (lflow_output_data && pflow_output_data &&
                        ct_zones_data && lb_data) {
                        stopwatch_start(OFCTRL_PUT_STOPWATCH_NAME,
                                        time_msec());
                        ofctrl_put(&lflow_output_data->flow_table,
                                   &pflow_output_data->flow_table,
                                   &ct_zones_data->ctx.pending,
                                   &ct_zones_data->ctx.current,
                                   &lb_data->removed_tuples,
                                   runtime_data ?
                                        &runtime_data->local_datapaths : NULL,
                                   sbrec_meter_by_name,
                                   sbrec_ecmp_nexthop_table_get(
                                        ovnsb_idl_loop.idl),
                                   ofctrl_seqno_get_req_cfg(),
                                   engine_node_changed(&en_lflow_output),
                                   engine_node_changed(&en_pflow_output),
                                   tracked_acl_ids,
                                   !daemon_started_recently());
                        stopwatch_stop(OFCTRL_PUT_STOPWATCH_NAME, time_msec());
                    }
                    stopwatch_start(OFCTRL_SEQNO_RUN_STOPWATCH_NAME,
                                    time_msec());
                    ofctrl_seqno_run(ofctrl_get_cur_cfg());
                    stopwatch_stop(OFCTRL_SEQNO_RUN_STOPWATCH_NAME,
                                   time_msec());
                    stopwatch_start(IF_STATUS_MGR_RUN_STOPWATCH_NAME,
                                    time_msec());
                    if_status_mgr_run(if_mgr, binding_data, chassis,
                                      ovsrec_interface_table_get(
                                                  ovs_idl_loop.idl),
                                      sbrec_port_binding_table_get(
                                                 ovnsb_idl_loop.idl),
                                      !ovnsb_idl_txn, !ovs_idl_txn);
                    stopwatch_stop(IF_STATUS_MGR_RUN_STOPWATCH_NAME,
                                   time_msec());
                }
            }

            if (!engine_has_run()) {
                if (engine_need_run()) {
                    VLOG_DBG("engine did not run, force recompute next time: "
                             "br_int %p, chassis %p", br_int, chassis);
                    engine_set_force_recompute_immediate();
                } else {
                    VLOG_DBG("engine did not run, and it was not needed"
                             " either: br_int %p, chassis %p",
                             br_int, chassis);
                }
            } else if (engine_canceled()) {
                VLOG_DBG("engine was canceled, force recompute next time: "
                         "br_int %p, chassis %p", br_int, chassis);
                engine_set_force_recompute_immediate();
            } else {
                engine_clear_force_recompute();
            }

            struct ed_type_northd_options *n_opts =
                engine_get_data(&en_northd_options);
            store_nb_cfg(ovnsb_idl_txn, ovs_idl_txn, chassis_private,
                         br_int, delay_nb_cfg_report,
                         n_opts ? n_opts->enable_ch_nb_cfg_update : true);

            if (pending_pkt.conn) {
                struct ed_type_addr_sets *as_data =
                    engine_get_data(&en_addr_sets);
                struct ed_type_port_groups *pg_data =
                    engine_get_data(&en_port_groups);
                struct ed_type_template_vars *tv_data =
                    engine_get_data(&en_template_vars);
                if (br_int && chassis && as_data && pg_data && tv_data) {
                    char *error = ofctrl_inject_pkt(br_int, pending_pkt.flow_s,
                        &as_data->addr_sets, &pg_data->port_groups_cs_local,
                        &tv_data->local_templates);
                    if (error) {
                        unixctl_command_reply_error(pending_pkt.conn, error);
                        free(error);
                    } else {
                        unixctl_command_reply(pending_pkt.conn, NULL);
                    }
                } else {
                    VLOG_DBG("Pending_pkt conn but br_int %p or chassis "
                             "%p not ready.", br_int, chassis);
                    unixctl_command_reply_error(pending_pkt.conn,
                        "ovn-controller not ready.");
                }
                pending_pkt.conn = NULL;
                free(pending_pkt.flow_s);
            }

            sset_destroy(&transport_zones);

            if (br_int) {
                ofctrl_wait();
                pinctrl_wait(ovnsb_idl_txn);
                statctrl_wait(ovnsb_idl_txn);
            }

            binding_wait();
            route_table_notify_wait();
            host_if_monitor_wait();
            neighbor_table_notify_wait();
        }

        unixctl_server_run(unixctl);

        unixctl_server_wait(unixctl);
        if (exit_args.exiting || pending_pkt.conn) {
            poll_immediate_wake();
        }

        ovnsb_txn_status = ovsdb_idl_loop_commit_and_wait(&ovnsb_idl_loop);
        if (!ovnsb_txn_status) {
            VLOG_INFO("OVNSB commit failed, force recompute next time.");
            engine_set_force_recompute_immediate();
        } else if (ovnsb_txn_status == 1) {
            ovsdb_idl_loop_next_cfg_inc(&ovnsb_idl_loop);
        } else if (ovnsb_txn_status == -1) {
            /* The commit is still in progress */
        } else {
            OVS_NOT_REACHED();
        }

        int ovs_txn_status = ovsdb_idl_loop_commit_and_wait(&ovs_idl_loop);
        if (!ovs_txn_status) {
            /* The transaction failed. */
            vif_plug_clear_deleted(
                    &vif_plug_deleted_iface_ids);
            vif_plug_clear_changed(
                    &vif_plug_changed_iface_ids);
        } else if (ovs_txn_status == 1) {
            /* The transaction committed successfully
             * (or it did not change anything in the database). */
            ct_zones_data = engine_get_data(&en_ct_zones);
            if (ct_zones_data) {
                ct_zones_pending_clear_commited(&ct_zones_data->ctx.pending);
            }

            vif_plug_finish_deleted(
                    &vif_plug_deleted_iface_ids);
            vif_plug_finish_changed(
                    &vif_plug_changed_iface_ids);
        } else if (ovs_txn_status == -1) {
            /* The commit is still in progress */
        } else {
            OVS_NOT_REACHED();
        }

        ovsdb_idl_track_clear(ovnsb_idl_loop.idl);
        ovsdb_idl_track_clear(ovs_idl_loop.idl);

        lflow_cache_run(ctrl_engine_ctx.lflow_cache);
        lflow_cache_wait(ctrl_engine_ctx.lflow_cache);

loop_done:
        memory_wait();
        ovsrcu_quiesce_start();
        poll_block();
        if (should_service_stop()) {
            exit_args.exiting = true;
        }
    }

    const struct ovsrec_open_vswitch_table *ovs_table =
        ovsrec_open_vswitch_table_get(ovs_idl_loop.idl);
    bool restart = exit_args.restart || !get_ovn_cleanup_on_exit(ovs_table);
    VLOG_INFO("Exiting ovn-controller, resource cleanup: %s",
              restart ? "False (--restart)" : "True");

    /* It's time to exit.  Clean up the databases if we are not restarting */
    if (!restart) {
        bool done = !ovsdb_idl_has_ever_connected(ovnsb_idl_loop.idl);
        while (!done) {
            update_sb_db(ovs_idl_loop.idl, ovnsb_idl_loop.idl,
                         NULL, NULL, NULL, NULL);
            update_ssl_config(ovsrec_ssl_table_get(ovs_idl_loop.idl));

            struct ovsdb_idl_txn *ovs_idl_txn
                = ovsdb_idl_loop_run(&ovs_idl_loop);
            struct ovsdb_idl_txn *ovnsb_idl_txn
                = ovsdb_idl_loop_run(&ovnsb_idl_loop);

            const struct ovsrec_bridge_table *bridge_table
                = ovsrec_bridge_table_get(ovs_idl_loop.idl);

            const struct sbrec_port_binding_table *port_binding_table
                = sbrec_port_binding_table_get(ovnsb_idl_loop.idl);

            const struct ovsrec_bridge *br_int = get_br_int(bridge_table,
                                                            ovs_table);
            const char *chassis_id = get_ovs_chassis_id(ovs_table);
            const struct sbrec_chassis *chassis
                = (chassis_id
                   ? chassis_lookup_by_name(sbrec_chassis_by_name, chassis_id)
                   : NULL);

            const struct sbrec_chassis_private *chassis_private
                = (chassis_id
                   ? chassis_private_lookup_by_name(
                         sbrec_chassis_private_by_name, chassis_id)
                   : NULL);

            /* Run all of the cleanup functions, even if one of them returns
             * false. We're done if all of them return true. */
            done = binding_cleanup(ovnsb_idl_txn, port_binding_table, chassis);
            done = chassis_cleanup(ovs_idl_txn, ovnsb_idl_txn, ovs_table,
                                   chassis, chassis_private) && done;
            done = encaps_cleanup(ovs_idl_txn, br_int) && done;
            done = igmp_group_cleanup(ovnsb_idl_txn, sbrec_igmp_group, chassis)
                   && done;
            if (done) {
                poll_immediate_wake();
            }

            ovsdb_idl_loop_commit_and_wait(&ovnsb_idl_loop);
            int ovs_txn_status = ovsdb_idl_loop_commit_and_wait(&ovs_idl_loop);
            if (!ovs_txn_status) {
                /* The transaction failed. */
                vif_plug_clear_deleted(
                        &vif_plug_deleted_iface_ids);
                vif_plug_clear_changed(
                        &vif_plug_changed_iface_ids);
            } else if (ovs_txn_status == 1) {
                /* The transaction committed successfully
                 * (or it did not change anything in the database). */
                vif_plug_finish_deleted(
                        &vif_plug_deleted_iface_ids);
                vif_plug_finish_changed(
                        &vif_plug_changed_iface_ids);
            }

            poll_block();
        }
        route_exchange_cleanup_vrfs();
    }

    /* The engine cleanup should happen only after threads have been
     * destroyed and joined in case they are accessing engine data. */
    pinctrl_destroy();
    statctrl_destroy();

    engine_set_context(NULL);
    engine_cleanup();

    free(ovn_version);
    lflow_destroy();
    ofctrl_destroy();
    ofctrl_seqno_destroy();
    binding_destroy();
    patch_destroy();
    mirror_destroy();
    encaps_destroy();
    if_status_mgr_destroy(if_mgr);
    shash_destroy(&vif_plug_deleted_iface_ids);
    shash_destroy(&vif_plug_changed_iface_ids);
    vif_plug_provider_destroy_all();

    ovsdb_idl_loop_destroy(&ovs_idl_loop);
    ovsdb_idl_loop_destroy(&ovnsb_idl_loop);

    ovs_feature_support_destroy();
    free(br_int_remote.target);
    free(ovs_remote);
    free(file_system_id);
    free(cli_system_id);
    ovn_exit_args_finish(&exit_args);
    unixctl_server_destroy(unixctl);
    service_stop();
    ovsrcu_exit();
    dns_resolve_destroy();
    route_exchange_destroy();
    route_table_notify_destroy();

    exit(retval);
}

static char *
parse_options(int argc, char *argv[])
{
    enum {
        OPT_PEER_CA_CERT = UCHAR_MAX + 1,
        OPT_BOOTSTRAP_CA_CERT,
        VLOG_OPTION_ENUMS,
        OVN_DAEMON_OPTION_ENUMS,
        SSL_OPTION_ENUMS,
        OPT_ENABLE_DUMMY_VIF_PLUG,
    };

    static struct option long_options[] = {
        {"help", no_argument, NULL, 'h'},
        {"version", no_argument, NULL, 'V'},
        {"unixctl", required_argument, NULL, 'u'},
        VLOG_LONG_OPTIONS,
        OVN_DAEMON_LONG_OPTIONS,
        STREAM_SSL_LONG_OPTIONS,
        {"peer-ca-cert", required_argument, NULL, OPT_PEER_CA_CERT},
        {"bootstrap-ca-cert", required_argument, NULL, OPT_BOOTSTRAP_CA_CERT},
        {"chassis", required_argument, NULL, 'n'},
        {"enable-dummy-vif-plug", no_argument, NULL,
         OPT_ENABLE_DUMMY_VIF_PLUG},
        {NULL, 0, NULL, 0}
    };
    char *short_options = ovs_cmdl_long_options_to_short_options(long_options);

    for (;;) {
        int c;

        c = getopt_long(argc, argv, short_options, long_options, NULL);
        if (c == -1) {
            break;
        }

        switch (c) {
        case 'h':
            usage();

        case 'V':
            ovs_print_version(OFP15_VERSION, OFP15_VERSION);
            printf("SB DB Schema %s\n", sbrec_get_db_version());
            exit(EXIT_SUCCESS);

        case 'u':
            unixctl_path = optarg;
            break;

        VLOG_OPTION_HANDLERS
        OVN_DAEMON_OPTION_HANDLERS

        case 'p':
            ssl_private_key_file = optarg;
            break;

        case 'c':
            ssl_certificate_file = optarg;
            break;

        case 'C':
            ssl_ca_cert_file = optarg;
            break;

        case OPT_SSL_PROTOCOLS:
            stream_ssl_set_protocols(optarg);
            break;

        case OPT_SSL_CIPHERS:
            stream_ssl_set_ciphers(optarg);
            break;

        case OPT_SSL_CIPHERSUITES:
            stream_ssl_set_ciphersuites(optarg);
            break;

        case OPT_PEER_CA_CERT:
            stream_ssl_set_peer_ca_cert_file(optarg);
            break;

        case OPT_BOOTSTRAP_CA_CERT:
            stream_ssl_set_ca_cert_file(optarg, true);
            break;

        case OPT_ENABLE_DUMMY_VIF_PLUG:
            vif_plug_dummy_enable();
            break;

        case 'n':
            free(cli_system_id);
            cli_system_id = xstrdup(optarg);
            break;

        case '?':
            exit(EXIT_FAILURE);

        default:
            abort();
        }
    }
    free(short_options);

    argc -= optind;
    argv += optind;

    char *ovs_remote;
    if (argc == 0) {
        ovs_remote = xasprintf("unix:%s/db.sock", ovs_rundir());
    } else if (argc == 1) {
        ovs_remote = xstrdup(argv[0]);
    } else {
        VLOG_FATAL("exactly zero or one non-option argument required; "
                   "use --help for usage");
    }
    return ovs_remote;
}

static void
usage(void)
{
    printf("%s: OVN controller\n"
           "usage %s [OPTIONS] [OVS-DATABASE]\n"
           "where OVS-DATABASE is a socket on which the OVS OVSDB server is listening.\n",
               program_name, program_name);
    stream_usage("OVS-DATABASE", true, false, true);
    daemon_usage();
    vlog_usage();
    printf("\nOther options:\n"
           "  -u, --unixctl=SOCKET    set control socket name\n"
           "  -n                      custom chassis name\n"
           "  -h, --help              display this help message\n"
           "  -V, --version           display version information\n");
    exit(EXIT_SUCCESS);
}

static void
ct_zone_list(struct unixctl_conn *conn, int argc OVS_UNUSED,
             const char *argv[] OVS_UNUSED, void *ct_zones_)
{
    struct shash *ct_zones = ct_zones_;
    struct ds ds = DS_EMPTY_INITIALIZER;
    struct shash_node *node;

    SHASH_FOR_EACH (node, ct_zones) {
        struct ct_zone *ct_zone = node->data;
        ds_put_format(&ds, "%s %d\n", node->name, ct_zone->zone);
    }

    unixctl_command_reply(conn, ds_cstr(&ds));
    ds_destroy(&ds);
}

static void
extend_table_list(struct unixctl_conn *conn, int argc OVS_UNUSED,
                 const char *argv[] OVS_UNUSED, void *extend_table_)
{
    struct ovn_extend_table *extend_table = extend_table_;
    struct ds ds = DS_EMPTY_INITIALIZER;
    struct simap items = SIMAP_INITIALIZER(&items);

    struct ovn_extend_table_info *item;
    HMAP_FOR_EACH (item, hmap_node, &extend_table->existing) {
        simap_put(&items, item->name, item->table_id);
    }

    const struct simap_node **nodes = simap_sort(&items);
    size_t n_nodes = simap_count(&items);
    for (size_t i = 0; i < n_nodes; i++) {
        const struct simap_node *node = nodes[i];
        ds_put_format(&ds, "%s: %d\n", node->name, node->data);
    }

    free(nodes);
    simap_destroy(&items);

    unixctl_command_reply(conn, ds_cstr(&ds));
    ds_destroy(&ds);
}

static void
inject_pkt(struct unixctl_conn *conn, int argc OVS_UNUSED,
           const char *argv[], void *pending_pkt_)
{
    struct pending_pkt *pending_pkt = pending_pkt_;

    if (pending_pkt->conn) {
        unixctl_command_reply_error(conn, "already pending packet injection");
        return;
    }
    pending_pkt->conn = conn;
    pending_pkt->flow_s = xstrdup(argv[1]);
}

static void
engine_recompute_cmd(struct unixctl_conn *conn OVS_UNUSED, int argc OVS_UNUSED,
                     const char *argv[] OVS_UNUSED, void *arg OVS_UNUSED)
{
    engine_trigger_recompute();
    unixctl_command_reply(conn, NULL);
}

static void
lflow_cache_flush_cmd(struct unixctl_conn *conn OVS_UNUSED,
                      int argc OVS_UNUSED, const char *argv[] OVS_UNUSED,
                      void *arg_)
{
    VLOG_INFO("User triggered lflow cache flush.");
    struct lflow_output_persistent_data *fo_pd = arg_;
    lflow_cache_flush(fo_pd->lflow_cache);
    engine_set_force_recompute_immediate();
    unixctl_command_reply(conn, NULL);
}

static void
lflow_cache_show_stats_cmd(struct unixctl_conn *conn, int argc OVS_UNUSED,
                           const char *argv[] OVS_UNUSED, void *arg_)
{
    struct lflow_output_persistent_data *fo_pd = arg_;
    struct lflow_cache *lc = fo_pd->lflow_cache;
    struct ds ds = DS_EMPTY_INITIALIZER;

    lflow_cache_get_stats(lc, &ds);
    unixctl_command_reply(conn, ds_cstr(&ds));
    ds_destroy(&ds);
}

static void
cluster_state_reset_cmd(struct unixctl_conn *conn, int argc OVS_UNUSED,
               const char *argv[] OVS_UNUSED, void *idl_reset_)
{
    bool *idl_reset = idl_reset_;

    *idl_reset = true;
    poll_immediate_wake();
    unixctl_command_reply(conn, NULL);
}

static void
debug_pause_execution(struct unixctl_conn *conn, int argc OVS_UNUSED,
                      const char *argv[] OVS_UNUSED, void *paused_)
{
    bool *paused = paused_;

    VLOG_INFO("User triggered execution pause.");
    *paused = true;
    unixctl_command_reply(conn, NULL);
}

static void
debug_resume_execution(struct unixctl_conn *conn, int argc OVS_UNUSED,
                       const char *argv[] OVS_UNUSED, void *paused_)
{
    bool *paused = paused_;

    VLOG_INFO("User triggered execution resume.");
    *paused = false;
    poll_immediate_wake();
    unixctl_command_reply(conn, NULL);
}

static void
debug_status_execution(struct unixctl_conn *conn, int argc OVS_UNUSED,
                       const char *argv[] OVS_UNUSED, void *paused_)
{
    bool *paused = paused_;

    if (*paused) {
        unixctl_command_reply(conn, "paused");
    } else {
        unixctl_command_reply(conn, "running");
    }
}

static void
debug_delay_nb_cfg_report(struct unixctl_conn *conn, int argc OVS_UNUSED,
                          const char *argv[], void *delay_)
{
    unsigned int *delay = delay_;

    if (!str_to_uint(argv[1], 10, delay)) {
        unixctl_command_reply_error(conn, "unsigned integer required");
        return;
    }

    char *msg;
    if (*delay) {
        msg = xasprintf("delay nb_cfg report for %u seconds.", *delay);
        unixctl_command_reply(conn, msg);
        free(msg);
    } else {
        unixctl_command_reply(conn, "no delay for nb_cfg report.");
    }
}

static void
debug_dump_local_bindings(struct unixctl_conn *conn, int argc OVS_UNUSED,
                          const char *argv[] OVS_UNUSED, void *local_bindings)
{
    struct ds binding_data = DS_EMPTY_INITIALIZER;
    binding_dump_local_bindings(local_bindings, &binding_data);
    unixctl_command_reply(conn, ds_cstr(&binding_data));
    ds_destroy(&binding_data);
}

static void
debug_dump_local_datapaths(struct unixctl_conn *conn, int argc OVS_UNUSED,
                           const char *argv[] OVS_UNUSED,
                           void *local_datapaths)
{
    struct ds local_dps_data = DS_EMPTY_INITIALIZER;
    binding_dump_local_datapaths(local_datapaths, &local_dps_data);
    unixctl_command_reply(conn, ds_cstr(&local_dps_data));
    ds_destroy(&local_dps_data);
}

static void
debug_dump_related_lports(struct unixctl_conn *conn, int argc OVS_UNUSED,
                          const char *argv[] OVS_UNUSED, void *related_lports)
{
    struct ds data = DS_EMPTY_INITIALIZER;
    binding_dump_related_lports(related_lports, &data);
    unixctl_command_reply(conn, ds_cstr(&data));
    ds_destroy(&data);
}

static void
debug_dump_peer_ports(struct unixctl_conn *conn, int argc OVS_UNUSED,
                          const char *argv[] OVS_UNUSED, void *local_datapaths)
{
    struct ds peer_ports = DS_EMPTY_INITIALIZER;
    local_data_dump_peer_ports(local_datapaths, &peer_ports);
    unixctl_command_reply(conn, ds_cstr(&peer_ports));
    ds_destroy(&peer_ports);
}

static void
debug_dump_lflow_conj_ids(struct unixctl_conn *conn, int argc OVS_UNUSED,
                          const char *argv[] OVS_UNUSED, void *conj_ids)
{
    struct ds conj_ids_dump = DS_EMPTY_INITIALIZER;
    lflow_conj_ids_dump(conj_ids, &conj_ids_dump);
    unixctl_command_reply(conn, ds_cstr(&conj_ids_dump));
    ds_destroy(&conj_ids_dump);
}

static void
debug_dump_local_template_vars(struct unixctl_conn *conn, int argc OVS_UNUSED,
                               const char *argv[] OVS_UNUSED, void *local_vars)
{
    struct ds tv_str = DS_EMPTY_INITIALIZER;
    ds_put_cstr(&tv_str, "Local template vars:\n");
    local_templates_to_string(local_vars, &tv_str);
    unixctl_command_reply(conn, ds_cstr(&tv_str));
    ds_destroy(&tv_str);
}

static void
debug_dump_local_mac_bindings(struct unixctl_conn *conn, int argc OVS_UNUSED,
                               const char *argv[] OVS_UNUSED,
                               void *mac_bindings)
{
    struct ds mb_str = DS_EMPTY_INITIALIZER;

    ds_put_cstr(&mb_str, "Local MAC bindings:\n");
    mac_bindings_to_string(mac_bindings, &mb_str);
    unixctl_command_reply(conn, ds_cstr(&mb_str));
    ds_destroy(&mb_str);
}
