#!/usr/bin/env python3
"""OVN memory regression testing tool.

Creates a broad OVN topology to detect memory regressions between commits.
Designed to be run via ovn-benchmark.sh. (Run ./ovn-benchmark.sh --help
to see usage).

Topology (simulates the default ovn-kubernetes topology):

         lsp-0-*            lsp-1-*          (Workload ports)
            |                  |
         ls-0               ls-1             (Logical Switches)
            |                  |
         s2c-0/c2s-0        s2c-1/c2s-1
              \\               /
         +--- cluster (LR) ---+              (Cluster Router)
                    |
                sjc/rcj
                    |
         +----  join (LS)  ----+              (Join Switch)
              /               \\
         j2lr-0/lr2j-0     j2lr-1/lr2j-1
            |                  |
         lr-0               lr-1             (Gateway Routers)

  Chassis binding (with batch size B, default n/10):
    chassis-0: c2s-[0..B), lr-[0..B)
    chassis-1: c2s-[B..2B), lr-[B..2B)
    ...

Each node creates a dual-stack gateway router + logical switch pair
with:
  - Dual-stack addressing (IPv4 10.x.x.x/24, IPv6 fd00:x::/64)
  - NAT (SNAT/DNAT/DNAT_AND_SNAT), static routes, routing policies
  - Configurable ports per switch with port security
  - Security: Address sets, port groups, ACLs
  - Services: DHCP, DHCPv6, DNS, load balancers
  - QoS: Bandwidth limiting, DSCP marking

Note: Uses explicit (non-templated) load balancers to maximize memory
usage for regression testing. For templated LB testing, see
ovn-lb-benchmark.py.
"""

import argparse
import sys

import ovs.db.idl
import ovs.jsonrpc
import ovs.poller
import ovs.stream
import ovs.vlog
from ovs.db import error

vlog = ovs.vlog.Vlog('ovn-benchmark')
vlog.set_levels_from_string('console:warn')
vlog.init(None)

SCHEMA = '../ovn-nb.ovsschema'


def die(msg):
    sys.stderr.write(f'\nError: {msg}\n')
    sys.exit(1)


def ip_node(i):
    """Convert node index to two IP octets, supporting up to 65535 nodes."""
    return f'{i >> 8}.{i & 0xff}'


def ip6_node(i):
    """Convert node index to an IPv6 hextet for ULA addresses."""
    return f'{i:x}'


def create_address_sets(idl, n):
    """Create address sets for security groups."""
    vlog.info('Creating address sets')
    txn = ovs.db.idl.Transaction(idl)

    web_as = txn.insert(idl.tables['Address_Set'])
    web_as.name = 'web_servers'
    web_as.addresses = (
        [f'10.{ip_node(i)}.10' for i in range(n)]
        + [f'fd00:{ip6_node(i)}::a' for i in range(n)])

    db_as = txn.insert(idl.tables['Address_Set'])
    db_as.name = 'db_servers'
    db_as.addresses = (
        [f'10.{ip_node(i)}.20' for i in range(n)]
        + [f'fd00:{ip6_node(i)}::14' for i in range(n)])

    app_as = txn.insert(idl.tables['Address_Set'])
    app_as.name = 'app_servers'
    app_as.addresses = (
        [f'10.{ip_node(i)}.30' for i in range(n)]
        + [f'fd00:{ip6_node(i)}::1e' for i in range(n)])

    trusted_as = txn.insert(idl.tables['Address_Set'])
    trusted_as.name = 'trusted_networks'
    trusted_as.addresses = ['192.168.0.0/16', '172.16.0.0/12',
                            'fc00::/7']

    if txn.commit_block() != ovs.db.idl.Transaction.SUCCESS:
        die(f'Failed to create address sets ({txn.get_error()})')


def create_port_groups(idl):
    """Create port groups for security group implementation."""
    vlog.info('Creating port groups')
    txn = ovs.db.idl.Transaction(idl)

    web_pg = txn.insert(idl.tables['Port_Group'])
    web_pg.name = 'web_tier'

    db_pg = txn.insert(idl.tables['Port_Group'])
    db_pg.name = 'db_tier'

    app_pg = txn.insert(idl.tables['Port_Group'])
    app_pg.name = 'app_tier'

    if txn.commit_block() != ovs.db.idl.Transaction.SUCCESS:
        die(f'Failed to create port groups ({txn.get_error()})')


def create_dhcp_options(idl, n):
    """Create DHCP options for each subnet."""
    for i in range(n):
        vlog.info(f'Creating DHCP options for node {i}')
        txn = ovs.db.idl.Transaction(idl)
        dhcp_opts = txn.insert(idl.tables['DHCP_Options'])
        dhcp_opts.cidr = f'10.{ip_node(i)}.0/24'
        dhcp_opts.setkey('options', 'server_id', f'10.{ip_node(i)}.1')
        dhcp_opts.setkey('options', 'server_mac', '00:00:00:00:00:01')
        dhcp_opts.setkey('options', 'lease_time', '3600')
        dhcp_opts.setkey('options', 'router', f'10.{ip_node(i)}.1')
        dhcp_opts.setkey('options', 'dns_server', f'10.{ip_node(i)}.2')
        dhcp_opts.setkey('options', 'domain_name', '"example.com"')
        dhcp_opts.setkey('options', 'mtu', '1500')
        dhcp_opts.setkey('external_ids', 'subnet', f'ls-{i}')

        dhcpv6_opts = txn.insert(idl.tables['DHCP_Options'])
        dhcpv6_opts.cidr = f'fd00:{ip6_node(i)}::/64'
        dhcpv6_opts.setkey('options', 'server_id',
                           '00:00:00:00:00:01')
        dhcpv6_opts.setkey('options', 'dns_server',
                           f'fd00:{ip6_node(i)}::2')
        dhcpv6_opts.setkey('options', 'domain_search',
                           '"example.com"')
        dhcpv6_opts.setkey('external_ids', 'subnet',
                           f'ls-{i}-v6')

        if txn.commit_block() != ovs.db.idl.Transaction.SUCCESS:
            die(f'Failed to create DHCP options for node {i} '
                f'({txn.get_error()})')


def create_qos_rules(idl, n, switches):
    """Create QoS rules for bandwidth limiting and DSCP marking."""
    for i in range(n):
        vlog.info(f'Creating QoS rules for node {i}')
        txn = ovs.db.idl.Transaction(idl)

        ls = switches.get(f'ls-{i}')
        if ls:
            qos_bw = txn.insert(idl.tables['QoS'])
            qos_bw.priority = 100
            qos_bw.direction = 'to-lport'
            qos_bw.match = f'inport == "lsp-{i}-0"'
            qos_bw.setkey('bandwidth', 'rate', 1000)
            qos_bw.setkey('bandwidth', 'burst', 100)
            qos_bw.setkey('external_ids', 'type', 'rate-limit')
            ls.addvalue('qos_rules', qos_bw.uuid)

            qos_dscp = txn.insert(idl.tables['QoS'])
            qos_dscp.priority = 200
            qos_dscp.direction = 'from-lport'
            qos_dscp.match = 'ip4 && tcp.dst == 22'
            qos_dscp.setkey('action', 'dscp', 46)
            qos_dscp.setkey('external_ids', 'type', 'dscp-marking')
            ls.addvalue('qos_rules', qos_dscp.uuid)

            qos_mark = txn.insert(idl.tables['QoS'])
            qos_mark.priority = 150
            qos_mark.direction = 'from-lport'
            qos_mark.match = 'ip4 && udp'
            qos_mark.setkey('action', 'mark', 1)
            qos_mark.setkey('external_ids', 'type', 'packet-marking')
            ls.addvalue('qos_rules', qos_mark.uuid)

            qos_dscp_v6 = txn.insert(idl.tables['QoS'])
            qos_dscp_v6.priority = 200
            qos_dscp_v6.direction = 'from-lport'
            qos_dscp_v6.match = 'ip6 && tcp.dst == 22'
            qos_dscp_v6.setkey('action', 'dscp', 46)
            qos_dscp_v6.setkey('external_ids', 'type',
                               'dscp-marking-v6')
            ls.addvalue('qos_rules', qos_dscp_v6.uuid)

            qos_mark_v6 = txn.insert(idl.tables['QoS'])
            qos_mark_v6.priority = 150
            qos_mark_v6.direction = 'from-lport'
            qos_mark_v6.match = 'ip6 && udp'
            qos_mark_v6.setkey('action', 'mark', 1)
            qos_mark_v6.setkey('external_ids', 'type',
                               'packet-marking-v6')
            ls.addvalue('qos_rules', qos_mark_v6.uuid)

        if txn.commit_block() != ovs.db.idl.Transaction.SUCCESS:
            die(f'Failed to create QoS rules for node {i} ({txn.get_error()})')


def create_acls_for_port_group(idl, pg_name, allowed_ports, priority_base):
    """Create ACLs for a specific port group."""
    txn = ovs.db.idl.Transaction(idl)

    for row in idl.tables['Port_Group'].rows.values():
        if row.name == pg_name:
            acl_allow_est = txn.insert(idl.tables['ACL'])
            acl_allow_est.priority = priority_base + 100
            acl_allow_est.direction = 'to-lport'
            acl_allow_est.match = 'ct.est && !ct.rel && !ct.new && !ct.inv'
            acl_allow_est.action = 'allow-related'
            row.addvalue('acls', acl_allow_est.uuid)

            acl_allow_rel = txn.insert(idl.tables['ACL'])
            acl_allow_rel.priority = priority_base + 100
            acl_allow_rel.direction = 'to-lport'
            acl_allow_rel.match = 'ct.rel && !ct.est && !ct.new && !ct.inv'
            acl_allow_rel.action = 'allow-related'
            row.addvalue('acls', acl_allow_rel.uuid)

            for port in allowed_ports:
                acl_new = txn.insert(idl.tables['ACL'])
                acl_new.priority = priority_base + 50
                acl_new.direction = 'to-lport'
                acl_new.match = f'ct.new && tcp.dst == {port}'
                acl_new.action = 'allow-related'
                row.addvalue('acls', acl_new.uuid)

            acl_drop = txn.insert(idl.tables['ACL'])
            acl_drop.priority = priority_base
            acl_drop.direction = 'to-lport'
            acl_drop.match = 'inport == @' + pg_name
            acl_drop.action = 'drop'
            row.addvalue('acls', acl_drop.uuid)

            acl_arp = txn.insert(idl.tables['ACL'])
            acl_arp.priority = priority_base + 10
            acl_arp.direction = 'to-lport'
            acl_arp.match = 'arp || nd'
            acl_arp.action = 'allow'
            row.addvalue('acls', acl_arp.uuid)
            break

    if txn.commit_block() != ovs.db.idl.Transaction.SUCCESS:
        die(f'Failed to create ACLs for {pg_name} ({txn.get_error()})')


def create_acls(idl):
    """Create comprehensive ACLs for port groups.

    ACL Priority Allocation:
      2500-2999: Security enforcement (anti-spoofing, etc.)
      2000-2499: Management access (SSH, ICMP, DHCP)
      1000-1499: Port group ACLs (security groups)
         1100: Connection tracking (established, related)
         1050: New connection per-port rules
         1010: ARP/ND allow
         1000: Default drop

    Switch ACLs (higher priority) can override port-group ACLs, with
    security enforcement (anti-spoofing) taking highest priority.
    """
    vlog.info('Creating ACLs for port groups')
    create_acls_for_port_group(idl, 'web_tier', [80, 443], 1000)
    create_acls_for_port_group(idl, 'app_tier', [8080, 9000], 1000)
    create_acls_for_port_group(idl, 'db_tier', [5432, 3306], 1000)


def _add_acl(txn, idl, ls, priority, direction, match, action,
             description=None):
    """Create an ACL and attach it to a logical switch."""
    acl = txn.insert(idl.tables['ACL'])
    acl.priority = priority
    acl.direction = direction
    acl.match = match
    acl.action = action
    if description:
        acl.setkey('external_ids', 'description', description)
    ls.addvalue('acls', acl.uuid)


def add_acls_to_switch(idl, switch_name, node_id, switches):
    """Add ACLs directly to a logical switch.

    Adds switch-level ACLs for SSH, ICMP, anti-spoofing, and DHCP.
    Anti-spoofing (priority 2500) prevents VMs from using IPs outside
    their assigned subnet, blocking IP address spoofing attacks.
    """
    txn = ovs.db.idl.Transaction(idl)

    ls = switches.get(switch_name)
    if ls:
        _add_acl(txn, idl, ls, 2000, 'from-lport',
                 'tcp.dst == 22 && ip4.src == 10.0.0.0/8',
                 'allow', 'Allow SSH from internal')
        _add_acl(txn, idl, ls, 1500, 'from-lport',
                 'icmp4 || icmp6', 'allow')
        _add_acl(txn, idl, ls, 2500, 'from-lport',
                 f'ip4.src != 10.{ip_node(node_id)}.0/24',
                 'drop', 'Anti-spoofing')
        _add_acl(txn, idl, ls, 2000, 'from-lport',
                 'udp.src == 68 && udp.dst == 67', 'allow')

        _add_acl(txn, idl, ls, 2000, 'from-lport',
                 'tcp.dst == 22 && ip6.src == fd00::/16',
                 'allow', 'Allow SSH from internal (IPv6)')
        _add_acl(txn, idl, ls, 2500, 'from-lport',
                 f'ip6.src != fd00:{ip6_node(node_id)}::/64',
                 'drop', 'Anti-spoofing (IPv6)')
        _add_acl(txn, idl, ls, 2000, 'from-lport',
                 'udp.src == 546 && udp.dst == 547', 'allow')

    if txn.commit_block() != ovs.db.idl.Transaction.SUCCESS:
        die(f'Failed to add ACLs to switch {switch_name} '
            f'({txn.get_error()})')


def create_dns_records(idl, n, switches):
    """Create DNS records in the NB database."""
    for i in range(n):
        vlog.info(f'Creating DNS records for node {i}')
        txn = ovs.db.idl.Transaction(idl)
        dns = txn.insert(idl.tables['DNS'])
        dns.setkey('records', f'web-{i}.example.com',
                   f'10.{ip_node(i)}.10 '
                   f'fd00:{ip6_node(i)}::a')
        dns.setkey('records', f'app-{i}.example.com',
                   f'10.{ip_node(i)}.30 '
                   f'fd00:{ip6_node(i)}::1e')
        dns.setkey('records', f'db-{i}.example.com',
                   f'10.{ip_node(i)}.20 '
                   f'fd00:{ip6_node(i)}::14')
        dns.setkey('external_ids', 'zone', f'zone-{i}')

        ls = switches.get(f'ls-{i}')
        if ls:
            ls.addvalue('dns_records', dns.uuid)

        if txn.commit_block() != ovs.db.idl.Transaction.SUCCESS:
            die(f'Failed to create DNS records for node {i} '
                f'({txn.get_error()})')


def add_nat_rules(idl, n, routers):
    """Add NAT rules to routers.

    Creates SNAT, DNAT, and DNAT_AND_SNAT rules to exercise all NAT code paths.
    """
    for i in range(n):
        vlog.info(f'Adding NAT rules to router {i}')
        txn = ovs.db.idl.Transaction(idl)

        lr = routers.get(f'lr-{i}')
        if lr:
            nat_snat = txn.insert(idl.tables['NAT'])
            nat_snat.type = 'snat'
            nat_snat.logical_ip = f'10.{ip_node(i)}.0/24'
            nat_snat.external_ip = f'192.{ip_node(i)}.1'
            lr.addvalue('nat', nat_snat.uuid)

            nat_dnat = txn.insert(idl.tables['NAT'])
            nat_dnat.type = 'dnat'
            nat_dnat.logical_ip = f'10.{ip_node(i)}.10'
            nat_dnat.external_ip = f'192.{ip_node(i)}.10'
            nat_dnat.setkey('external_ids', 'service', 'web')
            lr.addvalue('nat', nat_dnat.uuid)

            nat_dnat_and_snat = txn.insert(idl.tables['NAT'])
            nat_dnat_and_snat.type = 'dnat_and_snat'
            nat_dnat_and_snat.logical_ip = f'10.{ip_node(i)}.20'
            nat_dnat_and_snat.external_ip = f'192.{ip_node(i)}.20'
            nat_dnat_and_snat.setkey('external_ids', 'service', 'db')
            lr.addvalue('nat', nat_dnat_and_snat.uuid)

            nat_snat_v6 = txn.insert(idl.tables['NAT'])
            nat_snat_v6.type = 'snat'
            nat_snat_v6.logical_ip = f'fd00:{ip6_node(i)}::/64'
            nat_snat_v6.external_ip = f'fd01:{ip6_node(i)}::1'
            lr.addvalue('nat', nat_snat_v6.uuid)

            nat_dnat_v6 = txn.insert(idl.tables['NAT'])
            nat_dnat_v6.type = 'dnat'
            nat_dnat_v6.logical_ip = f'fd00:{ip6_node(i)}::a'
            nat_dnat_v6.external_ip = f'fd01:{ip6_node(i)}::a'
            nat_dnat_v6.setkey('external_ids',
                               'service', 'web-v6')
            lr.addvalue('nat', nat_dnat_v6.uuid)

            nat_ds_v6 = txn.insert(idl.tables['NAT'])
            nat_ds_v6.type = 'dnat_and_snat'
            nat_ds_v6.logical_ip = f'fd00:{ip6_node(i)}::14'
            nat_ds_v6.external_ip = f'fd01:{ip6_node(i)}::14'
            nat_ds_v6.setkey('external_ids',
                             'service', 'db-v6')
            lr.addvalue('nat', nat_ds_v6.uuid)

        if txn.commit_block() != ovs.db.idl.Transaction.SUCCESS:
            die(f'Failed to add NAT rules for node {i} '
                f'({txn.get_error()})')


def add_static_routes(idl, n, routers):
    """Add static routes to routers."""
    for i in range(n):
        vlog.info(f'Adding static routes to router {i}')
        txn = ovs.db.idl.Transaction(idl)

        lr = routers.get(f'lr-{i}')
        if lr:
            route_default = txn.insert(
                idl.tables['Logical_Router_Static_Route'])
            route_default.ip_prefix = '0.0.0.0/0'
            route_default.nexthop = '10.0.0.1'
            route_default.setkey('external_ids', 'type', 'default')
            lr.addvalue('static_routes', route_default.uuid)

            route_specific = txn.insert(
                idl.tables['Logical_Router_Static_Route'])
            route_specific.ip_prefix = f'172.{ip_node(i)}.0/24'
            route_specific.nexthop = f'10.{ip_node(i)}.254'
            route_specific.setkey('external_ids', 'type', 'specific')
            lr.addvalue('static_routes', route_specific.uuid)

            route_discard = txn.insert(
                idl.tables['Logical_Router_Static_Route'])
            route_discard.ip_prefix = '192.0.2.0/24'
            route_discard.nexthop = 'discard'
            route_discard.setkey('external_ids', 'type', 'blackhole')
            lr.addvalue('static_routes', route_discard.uuid)

            route_default_v6 = txn.insert(
                idl.tables['Logical_Router_Static_Route'])
            route_default_v6.ip_prefix = '::/0'
            route_default_v6.nexthop = 'fd00::1'
            route_default_v6.setkey('external_ids', 'type',
                                    'default-v6')
            lr.addvalue('static_routes', route_default_v6.uuid)

            route_specific_v6 = txn.insert(
                idl.tables['Logical_Router_Static_Route'])
            route_specific_v6.ip_prefix = (
                f'fd02:{ip6_node(i)}::/64')
            route_specific_v6.nexthop = (
                f'fd00:{ip6_node(i)}::fe')
            route_specific_v6.setkey('external_ids', 'type',
                                     'specific-v6')
            lr.addvalue('static_routes',
                        route_specific_v6.uuid)

            route_discard_v6 = txn.insert(
                idl.tables['Logical_Router_Static_Route'])
            route_discard_v6.ip_prefix = '2001:db8::/32'
            route_discard_v6.nexthop = 'discard'
            route_discard_v6.setkey('external_ids', 'type',
                                    'blackhole-v6')
            lr.addvalue('static_routes',
                        route_discard_v6.uuid)

        if txn.commit_block() != ovs.db.idl.Transaction.SUCCESS:
            die(f'Failed to add static routes for node {i} '
                f'({txn.get_error()})')


def add_routing_policies(idl, n, routers):
    """Add routing policies to routers."""
    for i in range(n):
        vlog.info(f'Adding routing policies to router {i}')
        txn = ovs.db.idl.Transaction(idl)

        lr = routers.get(f'lr-{i}')
        if lr:
            policy_reroute = txn.insert(idl.tables['Logical_Router_Policy'])
            policy_reroute.priority = 100
            policy_reroute.match = f'ip4.src == 10.{ip_node(i)}.0/24'
            policy_reroute.action = 'reroute'
            policy_reroute.nexthops = [f'10.{ip_node((i + 1) % n)}.1']
            policy_reroute.setkey('external_ids', 'policy',
                                  'traffic-engineering')
            lr.addvalue('policies', policy_reroute.uuid)

            policy_allow = txn.insert(idl.tables['Logical_Router_Policy'])
            policy_allow.priority = 50
            policy_allow.match = 'ip4.dst == $trusted_networks'
            policy_allow.action = 'allow'
            lr.addvalue('policies', policy_allow.uuid)

            policy_reroute_v6 = txn.insert(
                idl.tables['Logical_Router_Policy'])
            policy_reroute_v6.priority = 100
            policy_reroute_v6.match = (
                f'ip6.src == fd00:{ip6_node(i)}::/64')
            policy_reroute_v6.action = 'reroute'
            policy_reroute_v6.nexthops = [
                f'fd00:{ip6_node((i + 1) % n)}::1']
            policy_reroute_v6.setkey(
                'external_ids', 'policy',
                'traffic-engineering-v6')
            lr.addvalue('policies',
                        policy_reroute_v6.uuid)

            policy_allow_v6 = txn.insert(
                idl.tables['Logical_Router_Policy'])
            policy_allow_v6.priority = 50
            policy_allow_v6.match = (
                'ip6.dst == $trusted_networks')
            policy_allow_v6.action = 'allow'
            lr.addvalue('policies', policy_allow_v6.uuid)

        if txn.commit_block() != ovs.db.idl.Transaction.SUCCESS:
            die(f'Failed to add routing policies for node {i} '
                f'({txn.get_error()})')


def _add_ports(txn, idl, s, i, ports_per_switch):
    for p in range(ports_per_switch):
        lsp = txn.insert(idl.tables['Logical_Switch_Port'])
        lsp.name = f'lsp-{i}-{p}'
        mac_byte = (p + 10) % 256
        mac = (f'00:00:{i >> 8:02x}:{i & 0xff:02x}'
               f':{p:02x}:{mac_byte:02x}')
        ip = f'10.{ip_node(i)}.{10 + p}'
        ip6 = f'fd00:{ip6_node(i)}::{10 + p:x}'
        lsp.addresses = [f'{mac} {ip} {ip6}']
        lsp.port_security = [f'{mac} {ip} {ip6}']
        lsp.setkey('external_ids', 'vm-id', f'vm-{i}-{p}')

        if p % 3 == 0:
            lsp.setkey('external_ids', 'tier', 'web')
        elif p % 3 == 1:
            lsp.setkey('external_ids', 'tier', 'app')
        else:
            lsp.setkey('external_ids', 'tier', 'db')

        s.addvalue('ports', lsp.uuid)


def _add_node(txn, idl, i, chassis, cluster_rtr, join_sw, lbg,
              ports_per_switch):
    gwr = txn.insert(idl.tables['Logical_Router'])
    gwr.name = f'lr-{i}'
    gwr.addvalue('load_balancer_group', lbg.uuid)
    gwr.setkey('options', 'chassis', chassis)

    gwr2join = txn.insert(idl.tables['Logical_Router_Port'])
    gwr2join.name = f'lr2j-{i}'
    gwr2join.mac = '00:00:00:00:00:01'
    gwr2join.networks = ['10.0.0.1/8', 'fd00::1/48']
    gwr.addvalue('ports', gwr2join.uuid)

    join2gwr = txn.insert(idl.tables['Logical_Switch_Port'])
    join2gwr.name = f'j2lr-{i}'
    join2gwr.type = 'router'
    join2gwr.addresses = ['router']
    join2gwr.setkey('options', 'router-port', gwr2join.name)
    join_sw.addvalue('ports', join2gwr.uuid)

    s = txn.insert(idl.tables['Logical_Switch'])
    s.name = f'ls-{i}'
    s.addvalue('load_balancer_group', lbg.uuid)
    s.setkey('other_config', 'subnet', f'10.{ip_node(i)}.0/24')
    s.setkey('other_config', 'mcast_snoop', 'true')

    cluster2s = txn.insert(idl.tables['Logical_Router_Port'])
    cluster2s.name = f'c2s-{i}'
    cluster2s.mac = '00:00:00:00:00:01'
    cluster2s.networks = [f'10.{ip_node(i)}.1/24',
                          f'fd00:{ip6_node(i)}::1/64']
    cluster_rtr.addvalue('ports', cluster2s.uuid)

    gw_chassis = txn.insert(idl.tables['Gateway_Chassis'])
    gw_chassis.name = f'{cluster2s.name}-{chassis}'
    gw_chassis.chassis_name = chassis
    gw_chassis.priority = 1
    cluster2s.addvalue('gateway_chassis', gw_chassis.uuid)

    s2cluster = txn.insert(idl.tables['Logical_Switch_Port'])
    s2cluster.name = f's2c-{i}'
    s2cluster.type = 'router'
    s2cluster.addresses = ['router']
    s2cluster.setkey('options', 'router-port', cluster2s.name)
    s.addvalue('ports', s2cluster.uuid)

    _add_ports(txn, idl, s, i, ports_per_switch)


def create_topology(idl, n, ports_per_switch, batch_size):
    """Create the basic topology with routers, switches, and ports."""
    vlog.info('Creating topology')
    txn = ovs.db.idl.Transaction(idl)
    lbg = txn.insert(idl.tables['Load_Balancer_Group'])
    lbg.name = 'lbg'

    vlog.info('Adding join switch')
    join_sw = txn.insert(idl.tables['Logical_Switch'])
    join_sw.name = 'join'

    cluster_rtr = txn.insert(idl.tables['Logical_Router'])
    cluster_rtr.name = 'cluster'

    rcj = txn.insert(idl.tables['Logical_Router_Port'])
    rcj.name = 'rcj'
    rcj.mac = '00:00:00:00:00:01'
    rcj.networks = ['10.0.0.1/8', 'fd00::1/48']
    cluster_rtr.addvalue('ports', rcj.uuid)

    sjc = txn.insert(idl.tables['Logical_Switch_Port'])
    sjc.name = 'sjc'
    sjc.type = 'router'
    sjc.addresses = ['router']
    sjc.setkey('options', 'router-port', 'rcj')
    join_sw.addvalue('ports', sjc.uuid)

    for i in range(n):
        vlog.info(f'Provisioning node {i}')
        chassis = f'chassis-{i // batch_size}'
        _add_node(txn, idl, i, chassis, cluster_rtr, join_sw, lbg,
                  ports_per_switch)

    if txn.commit_block() != ovs.db.idl.Transaction.SUCCESS:
        die(f'Failed to create topology ({txn.get_error()})')


def assign_ports_to_groups(idl):
    """Assign logical switch ports to port groups based on tier."""
    vlog.info('Assigning ports to port groups')

    tier_ports = {'web': [], 'app': [], 'db': []}

    for row in idl.tables['Logical_Switch_Port'].rows.values():
        tier = row.external_ids.get('tier')
        if tier in tier_ports:
            tier_ports[tier].append(row.uuid)

    txn = ovs.db.idl.Transaction(idl)
    tier_to_group = {'web': 'web_tier', 'app': 'app_tier', 'db': 'db_tier'}
    for row in idl.tables['Port_Group'].rows.values():
        for tier, group_name in tier_to_group.items():
            if row.name == group_name:
                for port_uuid in tier_ports[tier]:
                    row.addvalue('ports', port_uuid)

    if txn.commit_block() != ovs.db.idl.Transaction.SUCCESS:
        die(f'Failed to assign ports to groups ({txn.get_error()})')


def add_explicit_lbs(idl, n, n_vips, n_backends, routers, switches):
    """Add explicit (non-templated) load balancers.

    Uses explicit (non-templated) LBs to maximize memory usage for
    regression testing.
    """
    for i in range(n):
        lr = routers.get(f'lr-{i}')
        ls = switches.get(f'ls-{i}')
        for j in range(n_vips):
            vlog.info(f'Adding LB {j} for node {i}')
            txn = ovs.db.idl.Transaction(idl)
            port = j + 1
            j1 = (j + 1) // 250
            j2 = (j + 1) % 250

            lb = txn.insert(idl.tables['Load_Balancer'])
            lb.name = f'lb-{j}-{i}'
            lb.setkey('vips', f'42.42.{ip_node(i)}:{port}',
                      ','.join(f'42.{k}.{j1}.{j2}:{port}'
                               for k in range(n_backends)))
            lb.setkey(
                'vips',
                f'[fd42::{ip6_node(i)}:{j:x}]:{port}',
                ','.join(f'[fd42:{k:x}::{j1:x}:{j2:x}]:{port}'
                         for k in range(n_backends)))
            lb.protocol = 'tcp'
            lr.addvalue('load_balancer', lb.uuid)
            ls.addvalue('load_balancer', lb.uuid)
            if txn.commit_block() != ovs.db.idl.Transaction.SUCCESS:
                die(f'Failed to add LB ({txn.get_error()})')


def run(remote, n, n_vips, n_backends, ports_per_switch, batch_size):
    """Main execution function."""
    schema_helper = ovs.db.idl.SchemaHelper(SCHEMA)
    schema_helper.register_all()
    idl = ovs.db.idl.Idl(remote, schema_helper, leader_only=False)

    err, stream = ovs.stream.Stream.open_block(
        ovs.stream.Stream.open(remote), 2000
    )
    if err:
        sys.stderr.write(f'failed to connect to "{remote}"')
        sys.exit(1)

    if not stream:
        sys.stderr.write(f'failed to connect to "{remote}"')
        sys.exit(1)
    rpc = ovs.jsonrpc.Connection(stream)

    while idl.change_seqno == 0 and not idl.run():
        rpc.run()

        poller = ovs.poller.Poller()
        idl.wait(poller)
        rpc.wait(poller)
        poller.block()

    # Check if database is clean before proceeding
    if (len(idl.tables['Load_Balancer_Group'].rows) > 0 or
        len(idl.tables['Logical_Switch'].rows) > 0 or
        len(idl.tables['Logical_Router'].rows) > 0):
        die('Database is not empty. Please restart the sandbox or clear the '
            'database before running this script.')

    create_topology(idl, n, ports_per_switch, batch_size)

    # Build lookup dictionaries for O(1) access to switches and routers
    switches = {row.name: row
                for row in idl.tables['Logical_Switch'].rows.values()}
    routers = {row.name: row
               for row in idl.tables['Logical_Router'].rows.values()}

    create_address_sets(idl, n)
    create_port_groups(idl)
    assign_ports_to_groups(idl)
    create_dhcp_options(idl, n)
    create_dns_records(idl, n, switches)
    add_nat_rules(idl, n, routers)
    add_static_routes(idl, n, routers)
    create_acls(idl)
    for i in range(n):
        add_acls_to_switch(idl, f'ls-{i}', i, switches)
    create_qos_rules(idl, n, switches)
    add_routing_policies(idl, n, routers)
    add_explicit_lbs(idl, n, n_vips, n_backends, routers, switches)


def main():
    """Parse arguments and run the benchmark."""
    parser = argparse.ArgumentParser(
        description='Create a complex OVN topology with various features'
    )
    parser.add_argument(
        '-r', '--remote', required=True, help='NB connection string'
    )
    parser.add_argument(
        '-n', '--nodes', type=int, required=True, help='Number of nodes'
    )
    parser.add_argument(
        '-p',
        '--ports-per-switch',
        type=int,
        default=9,
        help='Number of logical switch ports per switch (default: 9, '
             'provides 3 ports per tier)',
    )
    parser.add_argument(
        '-v', '--vips', type=int, default=5,
        help='Number of LB VIPs per node (default: 5)'
    )
    parser.add_argument(
        '-B',
        '--backends',
        type=int,
        default=5,
        help='Number backends per VIP (default: 5)',
    )
    parser.add_argument(
        '-b',
        '--batch-size',
        type=int,
        default=0,
        help='Nodes per chassis (default: n/10)',
    )
    parser.add_argument(
        '-d', '--debug',
        action='store_true',
        help='Enable debug output (show info messages)',
    )
    args = parser.parse_args()

    if args.batch_size <= 0:
        args.batch_size = max(1, args.nodes // 10)

    if args.nodes > 65535:
        sys.stderr.write('Error: maximum supported node count is 65535\n')
        sys.exit(1)

    if args.ports_per_switch > 245:
        sys.stderr.write(
            'Error: maximum supported ports per switch is 245\n')
        sys.exit(1)

    if args.debug:
        vlog.set_levels_from_string('console:info')

    # Print configuration summary
    sys.stderr.write('\n=== OVN Benchmark Configuration ===\n')
    sys.stderr.write(f'Nodes:                        {args.nodes}'
                     f' ({args.nodes} routers + {args.nodes} switches)\n')
    sys.stderr.write(f'Ports per switch:             {args.ports_per_switch} '
                     f'({args.ports_per_switch * args.nodes} total ports)\n')
    sys.stderr.write(f'Load balancer VIPs per node:  {args.vips} '
                     f'({args.vips * args.nodes} total VIPs)\n')
    sys.stderr.write(f'Backends per VIP:             {args.backends}\n')
    sys.stderr.write(f'Total load balancers:         '
                     f'{args.nodes * args.vips}\n')
    n_chassis = ((args.nodes + args.batch_size - 1)
                 // args.batch_size)
    sys.stderr.write(f'Nodes per chassis (batch):    '
                     f'{args.batch_size} '
                     f'({n_chassis} chassis)\n')
    sys.stderr.write(f'Debug logging:                '
                     f'{"enabled" if args.debug else "disabled"}\n')
    sys.stderr.write('===================================\n\n')

    run(args.remote, args.nodes, args.vips, args.backends,
        args.ports_per_switch, args.batch_size)


if __name__ == '__main__':
    try:
        main()
    except error.Error as e:
        sys.stderr.write(f'{e}\n')
        sys.exit(1)
