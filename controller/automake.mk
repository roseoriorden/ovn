bin_PROGRAMS += controller/ovn-controller
controller_ovn_controller_SOURCES = \
	controller/acl-ids.c \
	controller/acl-ids.h \
	controller/bfd.c \
	controller/bfd.h \
	controller/binding.c \
	controller/binding.h \
	controller/chassis.c \
	controller/chassis.h \
	controller/encaps.c \
	controller/encaps.h \
	controller/ha-chassis.c \
	controller/ha-chassis.h \
	controller/if-status.c \
	controller/if-status.h \
	controller/ip-mcast.c \
	controller/ip-mcast.h \
	controller/lb.c \
	controller/lb.h \
	controller/lflow.c \
	controller/lflow.h \
	controller/lflow-cache.c \
	controller/lflow-cache.h \
	controller/lflow-conj-ids.c \
	controller/lflow-conj-ids.h \
	controller/lport.c \
	controller/lport.h \
	controller/ofctrl.c \
	controller/ofctrl.h \
	controller/ofctrl-seqno.c \
	controller/ofctrl-seqno.h \
	controller/pinctrl.c \
	controller/pinctrl.h \
	controller/patch.c \
	controller/patch.h \
	controller/ovn-controller.c \
	controller/ovn-controller.h \
	controller/physical.c \
	controller/physical.h \
	controller/local_data.c \
	controller/local_data.h \
	controller/ovsport.h \
	controller/ovsport.c \
	controller/vif-plug.h \
	controller/vif-plug.c \
	controller/mirror.h \
	controller/mirror.c \
	controller/mac-cache.h \
	controller/mac-cache.c \
	controller/statctrl.h \
	controller/statctrl.c \
	controller/ct-zone.h \
	controller/ct-zone.c \
	controller/ovn-dns.c \
	controller/ovn-dns.h \
	controller/ecmp-next-hop-monitor.h \
	controller/ecmp-next-hop-monitor.c \
	controller/route-exchange.h \
	controller/route-table-notify.h \
	controller/route.h \
	controller/route.c \
	controller/garp_rarp.h \
	controller/garp_rarp.c

if HAVE_NETLINK
controller_ovn_controller_SOURCES += \
	controller/route-exchange-netlink.h \
	controller/route-exchange-netlink.c \
	controller/route-exchange.c \
	controller/route-table-notify.c
else
controller_ovn_controller_SOURCES += \
	controller/route-exchange-stub.c \
	controller/route-table-notify-stub.c
endif

controller_ovn_controller_LDADD = lib/libovn.la $(OVS_LIBDIR)/libopenvswitch.la
man_MANS += controller/ovn-controller.8
EXTRA_DIST += controller/ovn-controller.8.xml
CLEANFILES += controller/ovn-controller.8
