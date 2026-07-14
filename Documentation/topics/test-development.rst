..
      Licensed under the Apache License, Version 2.0 (the "License"); you may
      not use this file except in compliance with the License. You may obtain
      a copy of the License at

          http://www.apache.org/licenses/LICENSE-2.0

      Unless required by applicable law or agreed to in writing, software
      distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
      WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
      License for the specific language governing permissions and limitations
      under the License.

      Convention for heading levels in OVN documentation:

      =======  Heading 0 (reserved for the title in a document)
      -------  Heading 1
      ~~~~~~~  Heading 2
      +++++++  Heading 3
      '''''''  Heading 4

      Avoid deeper levels because they do not render well.

================
Test Development
================

This document provides information relevant to writing tests for OVN. The
documentation for executing tests exists in :doc:`/topics/testing`.

OVN uses `Autotest <https://www.gnu.org/software/autoconf/manual/autoconf-2.68/html_node/Using-Autotest.html#Using-Autotest>`_
for generating its tests. The top-level of the test code is located in the
file ``tests/testsuite.at``. This file is expanded into a shell script that
runs all of the OVN tests. Please refer to the Autotest documentation linked
above for more information regarding Autotest, as the rest of this document
assumes a general understanding of it.

OVN Test Suite Overview
-----------------------

All test code is located in the ``tests/`` directory at the root of the OVN
repository. Generally, files suffixed with ``-macros.at`` contain macros and
shell functions to aid in writing tests. Files suffixed with just ``.at``
contain the code for running actual tests.

By convention macros are denoted with all uppercase letters, while functions
use lowercase letters.

The most used and important macros/functions are documented here. All of
these macros/functions are implicitly available from all test files when
running the testsuite via ``make check``.

Macros and Functions
--------------------

Verification
~~~~~~~~~~~~

check COMMAND...
++++++++++++++++

Function to run COMMAND and check that it succeeds (returns exit code 0). Also
logs the COMMAND to standard output. Note that most ``ovn-nbctl`` and ``ovn-sbctl``
commands must be run within ``check`` so that the return status is checked.

check_uuid COMMAND
++++++++++++++++++

Function to run COMMAND and check that it does not print anything else than
uuid as output. It also fails if the output is empty.

Packet Verification
~~~~~~~~~~~~~~~~~~~

OVN_CHECK_PACKETS([PCAP], [EXPECTED])
+++++++++++++++++++++++++++++++++++++

Macro to compare packets read from PCAP, in pcap format, to those read from
EXPECTED, which is a text file containing packets as hex strings, one per line.
If PCAP contains fewer packets than EXPECTED, it waits up to 30 seconds by
default for more packets to appear. The wait time can be customized via the
``OVS_CTL_TIMEOUT`` environment variable.

The reception of any extra or duplicate packets causes the test to fail.

OVN_CHECK_PACKETS_CONTAIN([PCAP], [EXPECTED])
+++++++++++++++++++++++++++++++++++++++++++++

Macro to check packets read from PCAP contain data from EXPECTED. Waits until
all expected packets are received, and ignores extra packets.

OVN_CHECK_PACKETS_UNIQ([PCAP], [EXPECTED])
++++++++++++++++++++++++++++++++++++++++++

Macro to check that all EXPECTED packets are received. Duplicate packets are
ignored, however extra packets cause the test to fail.

OVN_CHECK_PACKETS_REMOVE_BROADCAST([PCAP], [EXPECTED])
++++++++++++++++++++++++++++++++++++++++++++++++++++++

Macro that waits until the expected number of packets (excluding broadcasts)
are received. Then it compares EXPECTED and received packets. Any (non
broadcast), extra or duplicate packets cause the test to fail.

Database Verification
~~~~~~~~~~~~~~~~~~~~~

count_rows [DATABASE:]TABLE [CONDITION...]
++++++++++++++++++++++++++++++++++++++++++

Function that prints the number of rows in TABLE matching optional CONDITIONs.
Uses the Southbound database by default. Prefix the table name with ``nb:`` to
query the Northbound database instead, e.g.::

    count_rows nb:Logical_Switch_Port name=lsp0

check_row_count [DATABASE:]TABLE COUNT [CONDITION...]
+++++++++++++++++++++++++++++++++++++++++++++++++++++

Function that asserts TABLE contains exactly COUNT rows matching optional
CONDITIONs. Fails the test immediately if the count does not match, printing
the full table listing for debugging.

wait_row_count [DATABASE:]TABLE COUNT [CONDITION...]
++++++++++++++++++++++++++++++++++++++++++++++++++++

Function that polls until TABLE contains exactly COUNT rows matching optional
CONDITIONs. On timeout, prints the actual row count and full table contents.

fetch_column [DATABASE:]TABLE [COLUMN [CONDITION...]]
+++++++++++++++++++++++++++++++++++++++++++++++++++++

Function that prints all values of COLUMN from rows in TABLE matching optional
CONDITIONs, sorted and whitespace-collapsed onto one line. COLUMN defaults to
``_uuid``.

check_column EXPECTED [DATABASE:]TABLE COLUMN [CONDITION...]
++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++

Function that fetches all values of COLUMN from matching rows in TABLE and
compares against EXPECTED, ignoring order. Fails the test if they differ.

wait_column EXPECTED [DATABASE:]TABLE [COLUMN [CONDITION...]]
+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++

Function that polls until all values of COLUMN in matching rows of TABLE equal
EXPECTED, ignoring order. COLUMN defaults to ``_uuid``.

Daemon/Sandbox Management
~~~~~~~~~~~~~~~~~~~~~~~~~

ovn_start [--backup-northd[=paused]] [--use-tcp-to-sb] [AZ]
+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++

Creates and initializes ovn-sb and ovn-nb databases and starts their
ovsdb-server instance, sets appropriate environment variables so that ovn-sbctl
and ovn-nbctl use them by default, and starts ovn-northd running against them.

Normally this starts only an active northd and no backup northd. The following
options are accepted:

``--backup-northd``         Start a backup northd.
``--backup-northd=paused``  Start the backup northd in the paused state.
``--use-tcp-to-sb``         Use TCP instead of SSL to connect to sb database.

The AZ parameter may name an arbitrary availability zone. If AZ is specified
then the ovn-ic daemon is also started.

net_add NETWORK
+++++++++++++++

Function to create a new interconnection network named NETWORK.

sim_add SANDBOX
+++++++++++++++

Function to start a new simulated Open vSwitch instance named SANDBOX. Files
related to the instance, such as logs, databases, sockets, and pidfiles, are
created in a subdirectory of the main test directory also named
SANDBOX. Afterward, the ``as`` command (see below) can be used to run Open
vSwitch commands in the context of the new sandbox.

The new sandbox starts out without any bridges. Use ovs-vsctl in the context of
the new sandbox to create a bridge, e.g.::

    sim_add hv0           # Create sandbox hv0.
    as hv0                # Set hv0 as default sandbox.
    ovs-vsctl add-br br0  # Add bridge br0 inside hv0.

or::

     sim_add hv0
     as hv0 ovs-vsctl add-br br0

ovn_attach NETWORK BRIDGE IP [MASKLEN] [ENCAP]
++++++++++++++++++++++++++++++++++++++++++++++

First, this function attaches BRIDGE to interconnection network NETWORK.
Second, it configures (simulated) address IP (with network mask length MASKLEN,
which defaults to 24) on BRIDGE. Finally, it configures the Open vSwitch
database to work with OVN and starts ovn-controller.

as SANDBOX [COMMAND...]
+++++++++++++++++++++++

``as SANDBOX`` sets the ``OVS_*DIR`` and ``OVN_*DIR`` environment variables to
point to $ovs_base/SANDBOX.

``as SANDBOX COMMAND...`` sets those variables in a subshell and invokes
COMMAND there.

ovn_as AZ [COMMAND...]
++++++++++++++++++++++

Switches the environment to availability zone AZ. If COMMAND is given, it runs
in a subshell with AZ's environment and then returns to the original
environment. Without COMMAND, sets ``OVN_NB_DB``, ``OVN_SB_DB``, and related
environment variables to point to AZ's databases persistently.

wait_for_ports_up [PORT...]
+++++++++++++++++++++++++++

With arguments, this function waits for specified Logical_Switch_Ports to come
up. Without arguments, waits for all "plain" and router Logical_Switch_Ports to
come up.

PARSE_LISTENING_PORT(LOGFILE, VARIABLE)
+++++++++++++++++++++++++++++++++++++++

Macro that parses the TCP or SSL/TLS port on which a server is listening from
LOGFILE, given that the server was told to listen on a kernel-chosen port, and
assigns the port number to shell VARIABLE. You should specify the listening
remote as ptcp:0:127.0.0.1 or pssl:0:127.0.0.1, or the equivalent with [::1]
instead of 127.0.0.1. Here's an example of how to use this with ovsdb-server::

    ovsdb-server --log-file --remote=ptcp:0:127.0.0.1 ...
    PARSE_LISTENING_PORT([ovsdb-server.log], [TCP_PORT])

Now $TCP_PORT holds the listening port.

OVN_POPULATE_ARP()
++++++++++++++++++

Macro to pre-populate the ARP tables of all of the OVN instances that have been
started with ``ovn_attach()``. That means that packets sent from one
hypervisor to another never get dropped or delayed by ARP resolution, which
makes testing easier.

OVS_TRAFFIC_VSWITCHD_START([vsctl-args], [vsctl-output], [=override])
+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++

Macro to create a database and start ovsdb-server, start ovs-vswitchd
connected to that database, call ovs-vsctl to create a bridge named br0 with
predictable settings, passing 'vsctl-args' as additional commands to
ovs-vsctl. If 'vsctl-args' causes ovs-vsctl to provide output (e.g. because it
includes "create" commands) then 'vsctl-output' specifies the expected output
after filtering through uuidfilt.

If a test needs to use "system" devices (as dummies), then specify
``=override`` (literally) as the third argument. Otherwise, system devices
won't work at all (which makes sense because tests should not access a system's
real Ethernet devices).

OVS_TRAFFIC_VSWITCHD_STOP([WHITELIST], [extra_cmds])
++++++++++++++++++++++++++++++++++++++++++++++++++++

Macro to gracefully stop ovs-vswitchd and ovsdb-server, checking their log
files for messages with severity WARN or higher and signaling an error if any
is present. The optional WHITELIST may contain shell-quoted "sed" commands to
delete any warnings that are actually expected, e.g.::

    OVS_TRAFFIC_VSWITCHD_STOP(["/expected error/d"])

'extra_cmds' are shell commands to be executed after OVS_VSWITCHD_STOP() is
invoked. They can be used to perform additional cleanups such as name space
removal.

OVS_APP_EXIT_AND_WAIT(DAEMON)
+++++++++++++++++++++++++++++

Ask the daemon named DAEMON to exit, via ``ovs-appctl``, and then wait for it
to exit.

OVN_CONTROLLER_EXIT(HV [, OPTION])
+++++++++++++++++++++++++++++++++++

Macro to gracefully exit ovn-controller running on hypervisor HV by sending
``exit`` via ``ovn-appctl``. The optional OPTION (e.g. ``--restart``) is passed
to the exit command. Waits until the process has fully terminated.

Daemon Pause/Resume
~~~~~~~~~~~~~~~~~~~

These functions are used to temporarily pause and resume OVN daemons during
tests.  Pausing a daemon is useful for testing behavior when components are
temporarily unavailable or for avoiding races during setup.

``sleep_northd`` / ``wake_up_northd``
  Pause and resume ovn-northd via ``SIGSTOP``/``SIGCONT``.

``sleep_controller HV`` / ``wake_up_controller HV``
  Pause and resume ovn-controller on hypervisor HV using the
  ``debug/pause`` and ``debug/resume`` appctl commands. Waits until the
  controller reports the expected status before returning.

``sleep_ovs HV`` / ``wake_up_ovs HV``
  Pause and resume ovs-vswitchd on hypervisor HV via
  ``SIGSTOP``/``SIGCONT``.

``sleep_sb`` / ``wake_up_sb``
  Pause and resume the Southbound ovsdb-server via
  ``SIGSTOP``/``SIGCONT``.

``sleep_nb`` / ``wake_up_nb``
  Pause and resume the Northbound ovsdb-server via
  ``SIGSTOP``/``SIGCONT``.

``sleep_ovsdb NAME`` / ``wake_up_ovsdb NAME``
  Pause and resume an ovsdb-server identified by directory NAME via
  ``SIGSTOP``/``SIGCONT``.

``stop_ovsdb_controller_updates TCP_PORT``
  Block ovn-controller from communicating with ovsdb-server by installing an
  nftables rule that drops incoming TCP packets on TCP_PORT. Skips the test
  if ``nft`` is not available.

``restart_ovsdb_controller_updates TCP_PORT``
  Remove the nftables rule installed by ``stop_ovsdb_controller_updates``,
  restoring communication.

Cleanup
~~~~~~~

OVN_CLEANUP(sim [, sim ...])
++++++++++++++++++++++++++++

Macro to gracefully terminate all OVN daemons, including those in specified
sandbox instances. Also checks the log file for messages with severity WARN or
higher and signals an error if any is present. Optional arguments may contain
"acceptable" error messages.

Before terminating the daemons, it also issues recomputes on ovn-controllers in
listed sandboxes, and checks whether the related ports and the openflows before
and after recompute are the same. Optional arguments may also contain
acceptable ``related_ports`` differences, datapaths and tables on which flow
differences are considered as acceptable.

OVN_CLEANUP_SBOX(sbox)
++++++++++++++++++++++

Macro to gracefully terminate OVN daemons in the specified sandbox instance.
The sandbox name ``vtep`` is treated as a special case, and is assumed to have
ovn-controller-vtep and ovs-vtep daemons running instead of ovn-controller.

Also checks the log file for messages with severity WARN or
higher and signals an error if any is present. Optional arguments may contain
"acceptable" error messages.

Before terminating the daemons, it also issues recomputes on ovn-controllers in
listed sandboxes, and checks whether the related ports and the openflows before
and after recompute are the same. Optional arguments may also contain
acceptable ``related_ports`` differences, datapaths and tables on which flow
differences are considered as acceptable.

OVN_CLEANUP_CONTROLLER(sbox)
++++++++++++++++++++++++++++

Macro to gracefully terminate ovn-controller in the specified sandbox
instance. The sandbox name ``vtep`` is treated as a special case, and is
assumed to have ovn-controller-vtep and ovs-vtep daemons running instead of
ovn-controller.

Issues recomputes on ovn-controllers in listed sandbox, and checks whether the
related ports and the openflows before and after recompute are the same.
Optional arguments may also contain acceptable ``related_ports`` differences,
datapaths and tables on which flow differences are considered as acceptable.

OVN_CLEANUP_IC([az ...])
++++++++++++++++++++++++

Macro to gracefully terminate all interconnection DBs and daemons in the
specified AZs, if any.

on_exit COMMAND
+++++++++++++++

Function to add the shell COMMAND to a collection that is executed when the
current test completes, as a cleanup action. The most common use is to kill a
daemon started by the test. This is important to prevent tests that start
daemons from hanging at exit.

Cleanup commands are executed in the reverse order of calls to this function.

Packet Construction and Injection
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

CHECK_SCAPY
+++++++++++

Macro that skips the current test if scapy is not available, then starts a
scapy-server daemon for packet construction. Must be placed at the top of any
test that uses scapy-based packet functions such as ``fmt_pkt``, ``send_garp``,
``send_na``, or ``send_udp``.

start_scapy_server
++++++++++++++++++

Function that starts a detached ``scapy-server.py`` daemon with a Unix control
socket. Uses ``flock`` for mutual exclusion. Called automatically by
``CHECK_SCAPY``; typically not called directly.

fmt_pkt SCAPY_EXPRESSION
+++++++++++++++++++++++++

Function that converts a scapy packet expression string into a raw hex byte
string. Communicates with the running scapy-server via its Unix control socket.
Requires ``start_scapy_server`` (or ``CHECK_SCAPY``) to have been called
first. Example::

    pkt=$(fmt_pkt "Ether(src='00:00:00:00:00:01', dst='ff:ff:ff:ff:ff:ff')/
                   IP(src='192.168.0.1', dst='192.168.0.2')/
                   UDP(sport=1234, dport=5678)")

netdev_dummy_receive INTERFACE PACKET [HV [PCAP_FILE]]
++++++++++++++++++++++++++++++++++++++++++++++++++++++

Function that injects a hex-encoded PACKET into a dummy netdev INTERFACE. If HV
is specified, runs in that hypervisor's context via ``as``. If PCAP_FILE is
specified, waits until its modification timestamp changes to confirm the packet
was recorded.

reset_pcap_file IFACE PCAP_FILE
+++++++++++++++++++++++++++++++

Function that resets the pcap capture file for an OVS dummy-datapath interface.
Removes old pcap files and reconfigures the interface's ``tx_pcap`` and
``rxq_pcap`` options to point to new files named ``PCAP_FILE-tx.pcap`` and
``PCAP_FILE-rx.pcap``.

send_garp HV INPORT OP ETH_SRC ETH_DST SPA TPA
++++++++++++++++++++++++++++++++++++++++++++++++

Function that constructs and injects a Gratuitous ARP packet on INPORT of HV.
OP is the ARP operation (1=request, 2=reply). Uses ``fmt_pkt`` internally.

send_na HV DEV HSRC HDST ISRC IDST
+++++++++++++++++++++++++++++++++++

Function that constructs and injects an ICMPv6 Neighbor Advertisement on DEV of
HV. Uses scapy to build the packet with target=ISRC and a Destination
Link-Layer Address option.

send_udp HV DEV HDST HSRC IDST ISRC
++++++++++++++++++++++++++++++++++++

Function that constructs and injects a basic IPv4/UDP packet on DEV of HV using
scapy. Note the argument order: destination MAC/IP come before source MAC/IP.

send_ip_multicast_pkt INPORT HV ETH_SRC ETH_DST IP_SRC IP_DST IP_LEN TTL IP_CHKSUM IP_PROTO DATA
+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++

Function that constructs and injects an IPv4 multicast packet on INPORT of HV.
Builds raw hex from the given Ethernet/IP header fields and DATA payload.

store_ip_multicast_pkt ETH_SRC ETH_DST IP_SRC IP_DST IP_LEN TTL IP_CHKSUM IP_PROTO DATA OUTFILE
++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++

Function that builds an IPv4 multicast packet from the given header fields and
appends the hex string to OUTFILE. Does not inject the packet; store-only
counterpart of ``send_ip_multicast_pkt``.

Tracing
~~~~~~~

ovn_trace ARGS...
+++++++++++++++++

Function that wraps ``ovn-trace``, passing all arguments through. Saves full
output to a file called ``trace`` (captured on failure) and strips comment lines
starting with ``#`` from the displayed output.

ovn_trace_client TARGET ARGS...
+++++++++++++++++++++++++++++++

Function that traces via an already-running ``ovn-trace`` daemon by connecting
to it with ``ovn-appctl -t TARGET trace``. Otherwise behaves the same as
``ovn_trace``.

Output Normalization
~~~~~~~~~~~~~~~~~~~~

trim_zeros
++++++++++

A sed filter (reads from stdin) that strips trailing ``00`` hex byte pairs from
each line. Used to normalize packet hex dumps for comparison.

ovn_strip_lflows
++++++++++++++++

A sed+sort filter (reads from stdin) that replaces ``table=NN`` with
``table=??`` in logical flow dumps, then sorts the output. Normalizes table
numbers so tests do not depend on specific table assignments.

ovn_strip_collector_set
+++++++++++++++++++++++

A sed filter (reads from stdin) that replaces ``collector_set=NN`` with
``collector_set=??`` in output. Normalizes collector set IDs so tests do not
depend on specific ID assignments.

Test Management
~~~~~~~~~~~~~~~

OVN_FOR_EACH_NORTHD(TEST)
+++++++++++++++++++++++++

Macro to wrap an arbitrary TEST. Defines versions of the TEST with all
combinations of northd, parallelization enabled and conditional monitoring
on/off. Normally the first statement in TEST is a call to ``AT_SETUP``.

OVN_FOR_EACH_NORTHD_NO_HV(TEST)
+++++++++++++++++++++++++++++++

Macro to wrap an arbitrary TEST. Defines versions of the TEST with all
combinations of northd and parallelization enabled. To be used when the
ovn-controller configuration is not relevant. Normally the first statement in
TEST is a call to ``AT_SETUP``.

OVN_FOR_EACH_NORTHD_NO_HV_PARALLELIZATION(TEST)
++++++++++++++++++++++++++++++++++++++++++++++++

Macro to wrap an arbitrary TEST. Defines two versions of the TEST, one with
parallelization enabled and one without. To be used when we want to test
parallelization permutations and ovn-controller configuration is not relevant.

OVN_FOR_EACH_NORTHD_FLOW_TUNNEL(TEST)
++++++++++++++++++++++++++++++++++++++

Macro to wrap an arbitrary TEST. Defines versions of the TEST with all
combinations of northd, parallelization enabled, conditional monitoring on/off,
and flow-based tunnels on/off. Produces 4 test variants. Use this for tests
that need to verify behavior with both port-based and flow-based tunnel
implementations.

TAG_UNSTABLE
++++++++++++

Macro that marks a test as unstable. The test can be skipped by setting the
``SKIP_UNSTABLE=yes`` environment variable. Adds the ``unstable`` keyword so
these tests can be selected or excluded with ``-k unstable``.

TAG_TEST_NOT_UPGRADABLE
+++++++++++++++++++++++

Macro that marks a test to be skipped when running upgrade tests. The test is
skipped when the ``UPGRADE_TEST`` environment variable is set to ``yes``.

OVN_SKIP_MEM_LEAK([LEAK ...])
+++++++++++++++++++++++++++++

Macro that suppresses specified memory leak reports from LeakSanitizer (LSAN).
Each argument (function name, file path, etc.) is added to a suppression file
and the ``LSAN_OPTIONS`` environment variable is updated accordingly.

Batched Command Execution
~~~~~~~~~~~~~~~~~~~~~~~~~

OVN_NBCTL(NBCTL_COMMAND)
++++++++++++++++++++++++

Macro to add NBCTL_COMMAND to list of commands to be run by the
``RUN_OVN_NBCTL`` macro.

RUN_OVN_NBCTL()
+++++++++++++++

Macro to execute a list of commands built by the ``OVN_NBCTL`` macro. The list
of commands is executed in a single invocation of ``ovn-nbctl``.

OVS_VSCTL(VSCTL_COMMAND)
++++++++++++++++++++++++

Macro to add VSCTL_COMMAND to list of commands to be run by ``RUN_OVS_VSCTL``.

RUN_OVS_VSCTL()
+++++++++++++++

Macro to execute the list of commands built by the ``OVS_VSCTL`` macro. The
list of commands is executed in a single invocation of ``ovs-vsctl``.

Polling and Waiting
~~~~~~~~~~~~~~~~~~~

OVS_WAIT_UNTIL(COMMAND[, IF-FAILED])
++++++++++++++++++++++++++++++++++++

Macro that executes shell COMMAND in a loop until it returns zero return code.
If COMMAND does not return zero code within reasonable time limit, then the
test fails. In that case, runs IF-FAILED before exiting.

OVS_WAIT_WHILE(COMMAND[, IF-FAILED])
++++++++++++++++++++++++++++++++++++

Macro that executes shell COMMAND in a loop until it returns non-zero return
code. If COMMAND does not return non-zero code within reasonable time limit,
then the test fails. In that case, runs IF-FAILED before exiting.

OVS_WAIT_UNTIL_EQUAL(COMMAND, OUTPUT)
+++++++++++++++++++++++++++++++++++++

Macro that executes shell COMMAND in a loop until it returns zero and the
output equals OUTPUT. If COMMAND does not return zero or a desired output
within a reasonable time limit, fails the test.

OVS_WAIT_FOR_OUTPUT(COMMAND, EXIT-STATUS, STDOUT, STDERR)
+++++++++++++++++++++++++++++++++++++++++++++++++++++++++

Executes shell COMMAND in a loop until it exits with status EXIT-STATUS, prints
STDOUT on stdout, and prints STDERR on stderr. If this doesn't happen within a
reasonable time limit, then the test fails.

There is an ``OVS_WAIT_FOR_OUTPUT_UNQUOTED`` version of this macro that expands
shell ``$variables``, ``$(command)``, and so on in the STDOUT and STDERR
arguments.  The plain version treats STDOUT and STDERR as literal text.

OpenFlow Table Constants
~~~~~~~~~~~~~~~~~~~~~~~~

The ``OFTABLE_*`` m4 macros map symbolic OpenFlow table names to their numeric
IDs for use in tests that inspect or match on flow tables. Using these macros
instead of raw numbers ensures tests stay correct if table assignments change.
The constants are defined in ``tests/ovn-macros.at`` and mirror the table
definitions in ``controller/lflow.h``.

Examples::

    OFTABLE_PHY_TO_LOG            = 0
    OFTABLE_LOG_INGRESS_PIPELINE  = 8
    OFTABLE_LOG_EGRESS_PIPELINE   = 62
    OFTABLE_LOG_TO_PHY            = 89
    OFTABLE_MAC_BINDING           = 90
    OFTABLE_CHK_LB_HAIRPIN        = 92
    OFTABLE_CHK_IN_PORT_SEC       = 97
    OFTABLE_CHK_OUT_PORT_SEC      = 99

There is also an ``OFTABLE_SAVE_INPORT_HEX`` macro that provides the hex
representation of ``OFTABLE_SAVE_INPORT``.
