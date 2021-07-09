// SPDX-License-Identifier: GPL-2.0
/* Copyright 2019 NXP Semiconductors
 */
#include <uapi/linux/if_bridge.h>
#include <soc/mscc/ocelot_vcap.h>
#include <soc/mscc/ocelot_qsys.h>
#include <soc/mscc/ocelot_sys.h>
#include <soc/mscc/ocelot_dev.h>
#include <soc/mscc/ocelot_ana.h>
#include <soc/mscc/ocelot_ptp.h>
#include <soc/mscc/ocelot.h>
#include <linux/packing.h>
#include <linux/module.h>
#include <linux/of_net.h>
#include <linux/pci.h>
#include <linux/of.h>
#include <net/pkt_sched.h>
#include <net/dsa.h>
#include "felix_tsn.h"
#include "felix.h"

static enum dsa_tag_protocol felix_get_tag_protocol(struct dsa_switch *ds,
						    int port,
						    enum dsa_tag_protocol mp)
{
	return DSA_TAG_PROTO_OCELOT;
}

static int felix_set_ageing_time(struct dsa_switch *ds,
				 unsigned int ageing_time)
{
	struct ocelot *ocelot = ds->priv;

	ocelot_set_ageing_time(ocelot, ageing_time);

	return 0;
}

static int felix_fdb_dump(struct dsa_switch *ds, int port,
			  dsa_fdb_dump_cb_t *cb, void *data)
{
	struct ocelot *ocelot = ds->priv;

	return ocelot_fdb_dump(ocelot, port, cb, data);
}

static int felix_fdb_add(struct dsa_switch *ds, int port,
			 const unsigned char *addr, u16 vid)
{
	struct ocelot *ocelot = ds->priv;

	return ocelot_fdb_add(ocelot, port, addr, vid);
}

static int felix_fdb_del(struct dsa_switch *ds, int port,
			 const unsigned char *addr, u16 vid)
{
	struct ocelot *ocelot = ds->priv;

	return ocelot_fdb_del(ocelot, port, addr, vid);
}

static void felix_bridge_stp_state_set(struct dsa_switch *ds, int port,
				       u8 state)
{
	struct ocelot *ocelot = ds->priv;

	return ocelot_bridge_stp_state_set(ocelot, port, state);
}

static int felix_bridge_join(struct dsa_switch *ds, int port,
			     struct net_device *br)
{
	struct ocelot *ocelot = ds->priv;

	return ocelot_port_bridge_join(ocelot, port, br);
}

static void felix_bridge_leave(struct dsa_switch *ds, int port,
			       struct net_device *br)
{
	struct ocelot *ocelot = ds->priv;

	ocelot_port_bridge_leave(ocelot, port, br);
}

/* This callback needs to be present */
static int felix_vlan_prepare(struct dsa_switch *ds, int port,
			      const struct switchdev_obj_port_vlan *vlan)
{
	return 0;
}

static int felix_vlan_filtering(struct dsa_switch *ds, int port, bool enabled)
{
	struct ocelot *ocelot = ds->priv;

	ocelot_port_vlan_filtering(ocelot, port, enabled);

	return 0;
}

static void felix_vlan_add(struct dsa_switch *ds, int port,
			   const struct switchdev_obj_port_vlan *vlan)
{
	struct ocelot *ocelot = ds->priv;
	u16 vid;
	int err;

	for (vid = vlan->vid_begin; vid <= vlan->vid_end; vid++) {
		err = ocelot_vlan_add(ocelot, port, vid,
				      vlan->flags & BRIDGE_VLAN_INFO_PVID,
				      vlan->flags & BRIDGE_VLAN_INFO_UNTAGGED);
		if (err) {
			dev_err(ds->dev, "Failed to add VLAN %d to port %d: %d\n",
				vid, port, err);
			return;
		}

		if (vlan->proto == ETH_P_8021AD) {
			if (!ocelot->qinq_enable) {
				ocelot->qinq_enable = true;
				kref_init(&ocelot->qinq_refcount);
			} else {
				kref_get(&ocelot->qinq_refcount);
			}
		}
	}
}

static void felix_vlan_qinq_release(struct kref *ref)
{
	struct ocelot *ocelot;

	ocelot = container_of(ref, struct ocelot, qinq_refcount);
	ocelot->qinq_enable = false;
}

static int felix_vlan_del(struct dsa_switch *ds, int port,
			  const struct switchdev_obj_port_vlan *vlan)
{
	struct ocelot *ocelot = ds->priv;
	u16 vid;
	int err;

	for (vid = vlan->vid_begin; vid <= vlan->vid_end; vid++) {
		err = ocelot_vlan_del(ocelot, port, vid);
		if (err) {
			dev_err(ds->dev, "Failed to remove VLAN %d from port %d: %d\n",
				vid, port, err);
			return err;
		}

		if (ocelot->qinq_enable && vlan->proto == ETH_P_8021AD)
			kref_put(&ocelot->qinq_refcount,
				 felix_vlan_qinq_release);
	}
	return 0;
}

static int felix_port_enable(struct dsa_switch *ds, int port,
			     struct phy_device *phy)
{
	struct ocelot *ocelot = ds->priv;
	struct net_device *slave;

	ocelot_port_enable(ocelot, port, phy);

	slave = dsa_to_port(ds, port)->slave;
	slave->features |= NETIF_F_HW_VLAN_STAG_FILTER;

	return 0;
}

static void felix_port_disable(struct dsa_switch *ds, int port)
{
	struct ocelot *ocelot = ds->priv;

	return ocelot_port_disable(ocelot, port);
}

static void felix_phylink_validate(struct dsa_switch *ds, int port,
				   unsigned long *supported,
				   struct phylink_link_state *state)
{
	struct ocelot *ocelot = ds->priv;
	struct ocelot_port *ocelot_port = ocelot->ports[port];
	__ETHTOOL_DECLARE_LINK_MODE_MASK(mask) = { 0, };

	if (state->interface != PHY_INTERFACE_MODE_NA &&
	    state->interface != ocelot_port->phy_mode) {
		bitmap_zero(supported, __ETHTOOL_LINK_MODE_MASK_NBITS);
		return;
	}

	phylink_set_port_modes(mask);
	phylink_set(mask, Autoneg);
	phylink_set(mask, Pause);
	phylink_set(mask, Asym_Pause);
	phylink_set(mask, 10baseT_Half);
	phylink_set(mask, 10baseT_Full);
	phylink_set(mask, 100baseT_Half);
	phylink_set(mask, 100baseT_Full);
	phylink_set(mask, 1000baseT_Half);
	phylink_set(mask, 1000baseT_Full);

	if (state->interface == PHY_INTERFACE_MODE_INTERNAL ||
	    state->interface == PHY_INTERFACE_MODE_2500BASEX ||
	    state->interface == PHY_INTERFACE_MODE_USXGMII) {
		phylink_set(mask, 2500baseT_Full);
		phylink_set(mask, 2500baseX_Full);
	}

	bitmap_and(supported, supported, mask,
		   __ETHTOOL_LINK_MODE_MASK_NBITS);
	bitmap_and(state->advertising, state->advertising, mask,
		   __ETHTOOL_LINK_MODE_MASK_NBITS);
}

static int felix_phylink_mac_pcs_get_state(struct dsa_switch *ds, int port,
					   struct phylink_link_state *state)
{
	struct ocelot *ocelot = ds->priv;
	struct felix *felix = ocelot_to_felix(ocelot);

	if (felix->info->pcs_link_state)
		felix->info->pcs_link_state(ocelot, port, state);

	return 0;
}

static void felix_phylink_mac_config(struct dsa_switch *ds, int port,
				     unsigned int link_an_mode,
				     const struct phylink_link_state *state)
{
	struct ocelot *ocelot = ds->priv;
	struct felix *felix = ocelot_to_felix(ocelot);
	u32 mac_fc_cfg;

	switch (state->speed) {
	case SPEED_10:
		mac_fc_cfg = SYS_MAC_FC_CFG_FC_LINK_SPEED(3);
		break;
	case SPEED_100:
		mac_fc_cfg = SYS_MAC_FC_CFG_FC_LINK_SPEED(2);
		break;
	case SPEED_1000:
	case SPEED_2500:
		mac_fc_cfg = SYS_MAC_FC_CFG_FC_LINK_SPEED(1);
		break;
	case SPEED_UNKNOWN:
		mac_fc_cfg = SYS_MAC_FC_CFG_FC_LINK_SPEED(0);
		break;
	default:
		dev_err(ocelot->dev, "Unsupported speed on port %d: %d\n",
			port, state->speed);
		return;
	}

	/* handle Rx pause in all cases, with 2500base-X this is used for rate
	 * adaptation.
	 */
	mac_fc_cfg |= SYS_MAC_FC_CFG_RX_FC_ENA;

	if (state->pause & MLO_PAUSE_TX)
		mac_fc_cfg |= SYS_MAC_FC_CFG_TX_FC_ENA |
			      SYS_MAC_FC_CFG_PAUSE_VAL_CFG(0xffff) |
			      SYS_MAC_FC_CFG_FC_LATENCY_CFG(0x7) |
			      SYS_MAC_FC_CFG_ZERO_PAUSE_ENA;

	/* Flow control. Link speed is only used here to evaluate the time
	 * specification in incoming pause frames.
	 */
	ocelot_write_rix(ocelot, mac_fc_cfg, SYS_MAC_FC_CFG, port);

	ocelot_write_rix(ocelot, 0, ANA_POL_FLOWC, port);

	if (felix->info->pcs_init)
		felix->info->pcs_init(ocelot, port, link_an_mode, state);

	if (felix->info->port_sched_speed_set)
		felix->info->port_sched_speed_set(ocelot, port,
						  state->speed);
}

static void felix_phylink_mac_link_down(struct dsa_switch *ds, int port,
					unsigned int link_an_mode,
					phy_interface_t interface)
{
	struct ocelot *ocelot = ds->priv;
	struct ocelot_port *ocelot_port = ocelot->ports[port];
	int err;

	ocelot_port_rmwl(ocelot_port, 0, DEV_MAC_ENA_CFG_RX_ENA,
			 DEV_MAC_ENA_CFG);

	ocelot_rmw_rix(ocelot, 0, QSYS_SWITCH_PORT_MODE_PORT_ENA,
		       QSYS_SWITCH_PORT_MODE, port);

	err = ocelot_port_flush(ocelot, port);
	if (err)
		dev_err(ocelot->dev, "failed to flush port %d: %d\n",
			port, err);

	/* Put the port in reset. */
	ocelot_port_writel(ocelot_port,
			   DEV_CLOCK_CFG_MAC_TX_RST |
			   DEV_CLOCK_CFG_MAC_RX_RST |
			   DEV_CLOCK_CFG_LINK_SPEED(OCELOT_SPEED_1000),
			   DEV_CLOCK_CFG);
}

static void felix_phylink_mac_link_up(struct dsa_switch *ds, int port,
				      unsigned int link_an_mode,
				      phy_interface_t interface,
				      struct phy_device *phydev)
{
	struct ocelot *ocelot = ds->priv;
	struct ocelot_port *ocelot_port = ocelot->ports[port];

	/* Enable MAC module */
	ocelot_port_writel(ocelot_port, DEV_MAC_ENA_CFG_RX_ENA |
			   DEV_MAC_ENA_CFG_TX_ENA, DEV_MAC_ENA_CFG);

	/* Enable receiving frames on the port, and activate auto-learning of
	 * MAC addresses.
	 */
	ocelot_write_gix(ocelot, ANA_PORT_PORT_CFG_LEARNAUTO |
			 ANA_PORT_PORT_CFG_RECV_ENA |
			 ANA_PORT_PORT_CFG_PORTID_VAL(port),
			 ANA_PORT_PORT_CFG, port);

	/* Core: Enable port for frame transfer */
	ocelot_write_rix(ocelot,
			 QSYS_SWITCH_PORT_MODE_SCH_NEXT_CFG(1) |
			 QSYS_SWITCH_PORT_MODE_PORT_ENA,
			 QSYS_SWITCH_PORT_MODE, port);

	/* Take port out of reset by clearing the MAC_TX_RST, MAC_RX_RST and
	 * PORT_RST bits in DEV_CLOCK_CFG. Note that the way this system is
	 * integrated is that the MAC speed is fixed and it's the PCS who is
	 * performing the rate adaptation, so we have to write "1000Mbps" into
	 * the LINK_SPEED field of DEV_CLOCK_CFG (which is also its default
	 * value).
	 */
	ocelot_port_writel(ocelot_port,
			   DEV_CLOCK_CFG_LINK_SPEED(OCELOT_SPEED_1000),
			   DEV_CLOCK_CFG);
}

static void felix_port_qos_map_init(struct ocelot *ocelot, int port, int num_tc)
{
	int i;

	ocelot_rmw_gix(ocelot,
		       ANA_PORT_QOS_CFG_QOS_PCP_ENA,
		       ANA_PORT_QOS_CFG_QOS_PCP_ENA,
		       ANA_PORT_QOS_CFG,
		       port);

	for (i = 0; i < num_tc * 2; i++) {
		ocelot_rmw_ix(ocelot,
			      (ANA_PORT_PCP_DEI_MAP_DP_PCP_DEI_VAL & i) |
			      ANA_PORT_PCP_DEI_MAP_QOS_PCP_DEI_VAL(i),
			      ANA_PORT_PCP_DEI_MAP_DP_PCP_DEI_VAL |
			      ANA_PORT_PCP_DEI_MAP_QOS_PCP_DEI_VAL_M,
			      ANA_PORT_PCP_DEI_MAP,
			      port, i);
	}
}

static void felix_get_strings(struct dsa_switch *ds, int port,
			      u32 stringset, u8 *data)
{
	struct ocelot *ocelot = ds->priv;

	return ocelot_get_strings(ocelot, port, stringset, data);
}

static void felix_get_ethtool_stats(struct dsa_switch *ds, int port, u64 *data)
{
	struct ocelot *ocelot = ds->priv;

	ocelot_get_ethtool_stats(ocelot, port, data);
}

static int felix_get_sset_count(struct dsa_switch *ds, int port, int sset)
{
	struct ocelot *ocelot = ds->priv;

	return ocelot_get_sset_count(ocelot, port, sset);
}

static int felix_get_ts_info(struct dsa_switch *ds, int port,
			     struct ethtool_ts_info *info)
{
	struct ocelot *ocelot = ds->priv;

	return ocelot_get_ts_info(ocelot, port, info);
}

static int felix_set_preempt(struct dsa_switch *ds, int port,
			     struct ethtool_fp *fpcmd)
{
	struct ocelot *ocelot = ds->priv;
	struct felix *felix = ocelot_to_felix(ocelot);

	if (felix->info->port_set_preempt)
		return felix->info->port_set_preempt(ocelot, port, fpcmd);

	return -EOPNOTSUPP;
}

static int felix_get_preempt(struct dsa_switch *ds, int port,
			     struct ethtool_fp *fpcmd)
{
	struct ocelot *ocelot = ds->priv;
	struct felix *felix = ocelot_to_felix(ocelot);

	if (felix->info->port_get_preempt)
		return felix->info->port_get_preempt(ocelot, port, fpcmd);

	return -EOPNOTSUPP;
}

static int felix_parse_ports_node(struct felix *felix,
				  struct device_node *ports_node,
				  phy_interface_t *port_phy_modes)
{
	struct ocelot *ocelot = &felix->ocelot;
	struct device *dev = felix->ocelot.dev;
	struct device_node *child;

	for_each_available_child_of_node(ports_node, child) {
		phy_interface_t phy_mode;
		u32 port;
		int err;

		/* Get switch port number from DT */
		if (of_property_read_u32(child, "reg", &port) < 0) {
			dev_err(dev, "Port number not defined in device tree "
				"(property \"reg\")\n");
			of_node_put(child);
			return -ENODEV;
		}

		/* Get PHY mode from DT */
		err = of_get_phy_mode(child);
		if (err < 0) {
			dev_err(dev, "Failed to read phy-mode or "
				"phy-interface-type property for port %d\n",
				port);
			of_node_put(child);
			return -ENODEV;
		}

		phy_mode = err;

		err = felix->info->prevalidate_phy_mode(ocelot, port, phy_mode);
		if (err < 0) {
			dev_err(dev, "Unsupported PHY mode %s on port %d\n",
				phy_modes(phy_mode), port);
			return err;
		}

		port_phy_modes[port] = phy_mode;
	}

	return 0;
}

static int felix_parse_dt(struct felix *felix, phy_interface_t *port_phy_modes)
{
	struct device *dev = felix->ocelot.dev;
	struct device_node *switch_node;
	struct device_node *ports_node;
	int err;

	switch_node = dev->of_node;

	ports_node = of_get_child_by_name(switch_node, "ports");
	if (!ports_node) {
		dev_err(dev, "Incorrect bindings: absent \"ports\" node\n");
		return -ENODEV;
	}

	err = felix_parse_ports_node(felix, ports_node, port_phy_modes);
	of_node_put(ports_node);

	return err;
}

static int felix_init_structs(struct felix *felix, int num_phys_ports)
{
	struct ocelot *ocelot = &felix->ocelot;
	phy_interface_t *port_phy_modes;
	resource_size_t switch_base;
	int port, i, err;

	ocelot->num_phys_ports = num_phys_ports;
	ocelot->ports = devm_kcalloc(ocelot->dev, num_phys_ports,
				     sizeof(struct ocelot_port *), GFP_KERNEL);
	if (!ocelot->ports)
		return -ENOMEM;

	ocelot->map		= felix->info->map;
	ocelot->stats_layout	= felix->info->stats_layout;
	ocelot->num_stats	= felix->info->num_stats;
	ocelot->shared_queue_sz	= felix->info->shared_queue_sz;
	ocelot->num_mact_rows	= felix->info->num_mact_rows;
	ocelot->vcap		= felix->info->vcap;
	ocelot->ops		= felix->info->ops;
	ocelot->policer_base	= felix->info->policer_base;

	port_phy_modes = kcalloc(num_phys_ports, sizeof(phy_interface_t),
				 GFP_KERNEL);
	if (!port_phy_modes)
		return -ENOMEM;

	err = felix_parse_dt(felix, port_phy_modes);
	if (err) {
		kfree(port_phy_modes);
		return err;
	}

	switch_base = pci_resource_start(felix->pdev,
					 felix->info->switch_pci_bar);

	for (i = 0; i < TARGET_MAX; i++) {
		struct regmap *target;
		struct resource *res;

		if (!felix->info->target_io_res[i].name)
			continue;

		res = &felix->info->target_io_res[i];
		res->flags = IORESOURCE_MEM;
		res->start += switch_base;
		res->end += switch_base;

		target = ocelot_regmap_init(ocelot, res);
		if (IS_ERR(target)) {
			dev_err(ocelot->dev,
				"Failed to map device memory space\n");
			kfree(port_phy_modes);
			return PTR_ERR(target);
		}

		ocelot->targets[i] = target;
	}

	err = ocelot_regfields_init(ocelot, felix->info->regfields);
	if (err) {
		dev_err(ocelot->dev, "failed to init reg fields map\n");
		kfree(port_phy_modes);
		return err;
	}

	for (port = 0; port < num_phys_ports; port++) {
		struct ocelot_port *ocelot_port;
		void __iomem *port_regs;
		struct resource *res;

		ocelot_port = devm_kzalloc(ocelot->dev,
					   sizeof(struct ocelot_port),
					   GFP_KERNEL);
		if (!ocelot_port) {
			dev_err(ocelot->dev,
				"failed to allocate port memory\n");
			kfree(port_phy_modes);
			return -ENOMEM;
		}

		res = &felix->info->port_io_res[port];
		res->flags = IORESOURCE_MEM;
		res->start += switch_base;
		res->end += switch_base;

		port_regs = devm_ioremap_resource(ocelot->dev, res);
		if (IS_ERR(port_regs)) {
			dev_err(ocelot->dev,
				"failed to map registers for port %d\n", port);
			kfree(port_phy_modes);
			return PTR_ERR(port_regs);
		}

		ocelot_port->phy_mode = port_phy_modes[port];
		ocelot_port->ocelot = ocelot;
		ocelot_port->regs = port_regs;
		ocelot->ports[port] = ocelot_port;
	}

	kfree(port_phy_modes);

	if (felix->info->mdio_bus_alloc) {
		err = felix->info->mdio_bus_alloc(ocelot);
		if (err < 0)
			return err;
	}

	if (felix->info->psfp_init)
		felix->info->psfp_init(ocelot);

	return 0;
}

static int felix_qinq_port_bitmap_get(struct dsa_switch *ds, u32 *bitmap)
{
	struct ocelot *ocelot = ds->priv;
	struct ocelot_port *ocelot_port;
	int port;

	*bitmap = 0;
	for (port = 0; port < ds->num_ports; port++) {
		ocelot_port = ocelot->ports[port];
		if (ocelot_port->qinq_mode)
			*bitmap |= 0x01 << port;
	}

	return 0;
}

static int felix_qinq_port_bitmap_set(struct dsa_switch *ds, u32 bitmap)
{
	struct ocelot *ocelot = ds->priv;
	struct ocelot_port *ocelot_port;
	int port;

	for (port = 0; port < ds->num_ports; port++) {
		ocelot_port = ocelot->ports[port];
		if (bitmap & (0x01 << port))
			ocelot_port->qinq_mode = true;
		else
			ocelot_port->qinq_mode = false;
	}

	return 0;
}

enum felix_devlink_param_id {
	FELIX_DEVLINK_PARAM_ID_BASE = DEVLINK_PARAM_GENERIC_ID_MAX,
	FELIX_DEVLINK_PARAM_ID_QINQ_PORT_BITMAP,
};

static int felix_devlink_param_get(struct dsa_switch *ds, u32 id,
				   struct devlink_param_gset_ctx *ctx)
{
	int err;

	switch (id) {
	case FELIX_DEVLINK_PARAM_ID_QINQ_PORT_BITMAP:
		err = felix_qinq_port_bitmap_get(ds, &ctx->val.vu32);
		break;
	default:
		err = -EOPNOTSUPP;
		break;
	}

	return err;
}

static int felix_devlink_param_set(struct dsa_switch *ds, u32 id,
				   struct devlink_param_gset_ctx *ctx)
{
	int err;

	switch (id) {
	case FELIX_DEVLINK_PARAM_ID_QINQ_PORT_BITMAP:
		err = felix_qinq_port_bitmap_set(ds, ctx->val.vu32);
		break;
	default:
		err = -EOPNOTSUPP;
		break;
	}

	return err;
}

static const struct devlink_param felix_devlink_params[] = {
	DSA_DEVLINK_PARAM_DRIVER(FELIX_DEVLINK_PARAM_ID_QINQ_PORT_BITMAP,
				 "qinq_port_bitmap",
				 DEVLINK_PARAM_TYPE_U32,
				 BIT(DEVLINK_PARAM_CMODE_RUNTIME)),
};

static int felix_setup_devlink_params(struct dsa_switch *ds)
{
	return dsa_devlink_params_register(ds, felix_devlink_params,
					   ARRAY_SIZE(felix_devlink_params));
}

static void felix_teardown_devlink_params(struct dsa_switch *ds)
{
	dsa_devlink_params_unregister(ds, felix_devlink_params,
				      ARRAY_SIZE(felix_devlink_params));
}

/* Hardware initialization done here so that we can allocate structures with
 * devm without fear of dsa_register_switch returning -EPROBE_DEFER and causing
 * us to allocate structures twice (leak memory) and map PCI memory twice
 * (which will not work).
 */
static int felix_setup(struct dsa_switch *ds)
{
	struct ocelot *ocelot = ds->priv;
	struct felix *felix = ocelot_to_felix(ocelot);
	int port, err;

	err = felix_init_structs(felix, ds->num_ports);
	if (err)
		return err;

	ocelot_init(ocelot);

	for (port = 0; port < ds->num_ports; port++) {
		ocelot_init_port(ocelot, port);

		/* Bring up the CPU port module and configure the NPI port */
		if (dsa_is_cpu_port(ds, port))
			ocelot_configure_cpu(ocelot, port,
					     OCELOT_TAG_PREFIX_NONE,
					     OCELOT_TAG_PREFIX_LONG);

		/* Set the default QoS Classification based on PCP and DEI
		 * bits of vlan tag.
		 */
		felix_port_qos_map_init(ocelot, port, ds->num_tx_queues);
	}

	ocelot_setup_ptp_traps(ocelot);

	/* Include the CPU port module in the forwarding mask for unknown
	 * unicast - the hardware default value for ANA_FLOODING_FLD_UNICAST
	 * excludes BIT(ocelot->num_phys_ports), and so does ocelot_init, since
	 * Ocelot relies on whitelisting MAC addresses towards PGID_CPU.
	 */
	ocelot_write_rix(ocelot,
			 ANA_PGID_PGID_PGID(GENMASK(ocelot->num_phys_ports, 0)),
			 ANA_PGID_PGID, PGID_UC);

	ds->mtu_enforcement_ingress = true;
	/* It looks like the MAC/PCS interrupt register - PM0_IEVENT (0x8040)
	 * isn't instantiated for the Felix PF.
	 * In-band AN may take a few ms to complete, so we need to poll.
	 */
	ds->pcs_poll = true;
	/* Accept VLAN configuration from bridge core irrespective of VLAN
	 * filtering state.
	 */
	ds->vlan_bridge_vtu = true;

	err = felix_setup_devlink_params(ds);
	if (err < 0)
		return err;

	return 0;
}

static void felix_teardown(struct dsa_switch *ds)
{
	struct ocelot *ocelot = ds->priv;
	struct felix *felix = ocelot_to_felix(ocelot);

	if (felix->info->mdio_bus_free)
		felix->info->mdio_bus_free(ocelot);

	felix_teardown_devlink_params(ds);

	/* stop workqueue thread */
	ocelot_deinit(ocelot);
}

static int felix_hwtstamp_get(struct dsa_switch *ds, int port,
			      struct ifreq *ifr)
{
	struct ocelot *ocelot = ds->priv;

	return ocelot_hwstamp_get(ocelot, port, ifr);
}

static int felix_hwtstamp_set(struct dsa_switch *ds, int port,
			      struct ifreq *ifr)
{
	struct ocelot *ocelot = ds->priv;

	return ocelot_hwstamp_set(ocelot, port, ifr);
}

static bool felix_rxtstamp(struct dsa_switch *ds, int port,
			   struct sk_buff *skb, unsigned int type)
{
	struct skb_shared_hwtstamps *shhwtstamps;
	struct ocelot *ocelot = ds->priv;
	u8 *extraction = skb->data - ETH_HLEN - OCELOT_TAG_LEN;
	u32 tstamp_lo, tstamp_hi;
	struct timespec64 ts;
	u64 tstamp, val;

	ocelot_ptp_gettime64(&ocelot->ptp_info, &ts);
	tstamp = ktime_set(ts.tv_sec, ts.tv_nsec);

	packing(extraction, &val,  116, 85, OCELOT_TAG_LEN, UNPACK, 0);
	tstamp_lo = (u32)val;

	tstamp_hi = tstamp >> 32;
	if ((tstamp & 0xffffffff) < tstamp_lo)
		tstamp_hi--;

	tstamp = ((u64)tstamp_hi << 32) | tstamp_lo;

	shhwtstamps = skb_hwtstamps(skb);
	memset(shhwtstamps, 0, sizeof(struct skb_shared_hwtstamps));
	shhwtstamps->hwtstamp = tstamp;
	return false;
}

bool felix_txtstamp(struct dsa_switch *ds, int port,
		    struct sk_buff *clone, unsigned int type)
{
	struct ocelot *ocelot = ds->priv;
	struct ocelot_port *ocelot_port = ocelot->ports[port];

	if (!ocelot_port_add_txtstamp_skb(ocelot_port, clone))
		return true;

	return false;
}

static int felix_change_mtu(struct dsa_switch *ds, int port, int new_mtu)
{
	struct ocelot *ocelot = ds->priv;

	ocelot_port_set_maxlen(ocelot, port, new_mtu);

	return 0;
}

static int felix_get_max_mtu(struct dsa_switch *ds, int port)
{
	struct ocelot *ocelot = ds->priv;

	return ocelot_get_max_mtu(ocelot, port);
}

static int felix_cls_flower_add(struct dsa_switch *ds, int port,
				struct flow_cls_offload *cls, bool ingress)
{
	struct ocelot *ocelot = ds->priv;
	struct felix *felix = ocelot_to_felix(ocelot);

	if (felix->info->flower_replace) {
		if (cls->common.chain_index == OCELOT_PSFP_CHAIN)
			return felix->info->flower_replace(ocelot, port, cls,
							   ingress);
	}

	return ocelot_cls_flower_replace(ocelot, port, cls, ingress);
}

static int felix_cls_flower_del(struct dsa_switch *ds, int port,
				struct flow_cls_offload *cls, bool ingress)
{
	struct ocelot *ocelot = ds->priv;
	struct felix *felix = ocelot_to_felix(ocelot);
	int ret;

	if (felix->info->flower_destroy) {
		ret = felix->info->flower_destroy(ocelot, port, cls, ingress);
		if (!ret)
			return 0;
	}

	return ocelot_cls_flower_destroy(ocelot, port, cls, ingress);
}

static int felix_cls_flower_stats(struct dsa_switch *ds, int port,
				  struct flow_cls_offload *cls, bool ingress)
{
	struct ocelot *ocelot = ds->priv;
	struct felix *felix = ocelot_to_felix(ocelot);
	int ret;

	if (felix->info->flower_stats) {
		ret = felix->info->flower_stats(ocelot, port, cls, ingress);
		if (!ret)
			return 0;
	}

	return ocelot_cls_flower_stats(ocelot, port, cls, ingress);
}

static int felix_port_policer_add(struct dsa_switch *ds, int port,
				  struct dsa_mall_policer_tc_entry *policer)
{
	struct ocelot *ocelot = ds->priv;
	struct ocelot_policer pol = {
		.rate = div_u64(policer->rate_bytes_per_sec, 1000) * 8,
		.burst = div_u64(policer->rate_bytes_per_sec *
				 PSCHED_NS2TICKS(policer->burst),
				 PSCHED_TICKS_PER_SEC),
	};

	return ocelot_port_policer_add(ocelot, port, &pol);
}

static void felix_port_policer_del(struct dsa_switch *ds, int port)
{
	struct ocelot *ocelot = ds->priv;

	ocelot_port_policer_del(ocelot, port);
}

struct net_device *felix_port_to_netdev(struct ocelot *ocelot, int port)
{
	struct felix *felix = ocelot_to_felix(ocelot);
	struct dsa_switch *ds = felix->ds;

	if (!dsa_is_user_port(ds, port))
		return NULL;

	return dsa_to_port(ds, port)->slave;
}

int felix_netdev_to_port(struct net_device *dev)
{
	struct dsa_port *dp;

	dp = dsa_port_from_netdev(dev);
	if (IS_ERR(dp))
		return -EINVAL;

	return dp->index;
}

static struct dsa_switch_ops felix_switch_ops = {
	.get_tag_protocol	= felix_get_tag_protocol,
	.setup			= felix_setup,
	.teardown		= felix_teardown,
	.set_ageing_time	= felix_set_ageing_time,
	.get_strings		= felix_get_strings,
	.get_ethtool_stats	= felix_get_ethtool_stats,
	.get_sset_count		= felix_get_sset_count,
	.get_ts_info		= felix_get_ts_info,
	.set_preempt		= felix_set_preempt,
	.get_preempt		= felix_get_preempt,
	.phylink_validate	= felix_phylink_validate,
	.phylink_mac_link_state	= felix_phylink_mac_pcs_get_state,
	.phylink_mac_config	= felix_phylink_mac_config,
	.phylink_mac_link_down	= felix_phylink_mac_link_down,
	.phylink_mac_link_up	= felix_phylink_mac_link_up,
	.port_enable		= felix_port_enable,
	.port_disable		= felix_port_disable,
	.port_fdb_dump		= felix_fdb_dump,
	.port_fdb_add		= felix_fdb_add,
	.port_fdb_del		= felix_fdb_del,
	.port_bridge_join	= felix_bridge_join,
	.port_bridge_leave	= felix_bridge_leave,
	.port_stp_state_set	= felix_bridge_stp_state_set,
	.port_vlan_prepare	= felix_vlan_prepare,
	.port_vlan_filtering	= felix_vlan_filtering,
	.port_vlan_add		= felix_vlan_add,
	.port_vlan_del		= felix_vlan_del,
	.port_hwtstamp_get	= felix_hwtstamp_get,
	.port_hwtstamp_set	= felix_hwtstamp_set,
	.port_rxtstamp		= felix_rxtstamp,
	.port_txtstamp		= felix_txtstamp,
	.port_change_mtu	= felix_change_mtu,
	.port_max_mtu		= felix_get_max_mtu,
	.port_policer_add	= felix_port_policer_add,
	.port_policer_del	= felix_port_policer_del,
	.cls_flower_add		= felix_cls_flower_add,
	.cls_flower_del		= felix_cls_flower_del,
	.cls_flower_stats	= felix_cls_flower_stats,
	.devlink_param_get	= felix_devlink_param_get,
	.devlink_param_set	= felix_devlink_param_set,
};

static struct felix_info *felix_instance_tbl[] = {
	[FELIX_INSTANCE_VSC9959] = &felix_info_vsc9959,
};

static irqreturn_t felix_irq_handler(int irq, void *data)
{
	struct ocelot *ocelot = (struct ocelot *)data;

	/* The INTB interrupt is used for both PTP TX timestamp interrupt
	 * and preemption status change interrupt on each port.
	 */

#ifdef CONFIG_MSCC_FELIX_SWITCH_TSN
	felix_preempt_irq_clean(ocelot);
#endif
	ocelot_get_txtstamp(ocelot);

	return IRQ_HANDLED;
}

static int felix_pci_probe(struct pci_dev *pdev,
			   const struct pci_device_id *id)
{
	enum felix_instance instance = id->driver_data;
	struct dsa_switch *ds;
	struct ocelot *ocelot;
	struct felix *felix;
	int err;

	err = pci_enable_device(pdev);
	if (err) {
		dev_err(&pdev->dev, "device enable failed\n");
		goto err_pci_enable;
	}

	/* set up for high or low dma */
	err = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(64));
	if (err) {
		err = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32));
		if (err) {
			dev_err(&pdev->dev,
				"DMA configuration failed: 0x%x\n", err);
			goto err_dma;
		}
	}

	felix = devm_kzalloc(&pdev->dev, sizeof(*felix), GFP_KERNEL);
	if (!felix) {
		err = -ENOMEM;
		dev_err(&pdev->dev, "Failed to allocate driver memory\n");
		goto err_alloc_felix;
	}

	pci_set_drvdata(pdev, felix);
	ocelot = &felix->ocelot;
	ocelot->dev = &pdev->dev;
	felix->pdev = pdev;
	felix->info = felix_instance_tbl[instance];

	pci_set_master(pdev);

	err = devm_request_threaded_irq(&pdev->dev, pdev->irq, NULL,
					&felix_irq_handler, IRQF_ONESHOT,
					"felix-intb", ocelot);
	if (err) {
		dev_err(&pdev->dev, "Failed to request irq\n");
		goto err_alloc_irq;
	}

	ocelot->ptp = 1;

	if (felix->info->port_setup_tc)
		felix_switch_ops.port_setup_tc = felix->info->port_setup_tc;

	ds = kzalloc(sizeof(struct dsa_switch), GFP_KERNEL);
	if (!ds) {
		err = -ENOMEM;
		dev_err(&pdev->dev, "Failed to allocate DSA switch\n");
		goto err_alloc_ds;
	}

	ds->dev = &pdev->dev;
	ds->num_ports = felix->info->num_ports;
	ds->num_tx_queues = felix->info->num_tx_queues;
	ds->ops = &felix_switch_ops;
	ds->priv = ocelot;
	felix->ds = ds;

	err = dsa_register_switch(ds);
	if (err) {
		dev_err(&pdev->dev, "Failed to register DSA switch: %d\n", err);
		goto err_register_ds;
	}

#ifdef CONFIG_MSCC_FELIX_SWITCH_TSN
	felix_tsn_enable(ds);
#endif

	return 0;

err_register_ds:
err_alloc_ds:
err_alloc_irq:
err_alloc_felix:
err_dma:
	pci_disable_device(pdev);
err_pci_enable:
	return err;
}

static void felix_pci_remove(struct pci_dev *pdev)
{
	struct felix *felix;

	felix = pci_get_drvdata(pdev);

	dsa_unregister_switch(felix->ds);

	pci_disable_device(pdev);
}

static struct pci_device_id felix_ids[] = {
	{
		/* NXP LS1028A */
		PCI_DEVICE(PCI_VENDOR_ID_FREESCALE, 0xEEF0),
		.driver_data = FELIX_INSTANCE_VSC9959,
	},
	{ 0, }
};
MODULE_DEVICE_TABLE(pci, felix_ids);

static struct pci_driver felix_pci_driver = {
	.name		= KBUILD_MODNAME,
	.id_table	= felix_ids,
	.probe		= felix_pci_probe,
	.remove		= felix_pci_remove,
};

module_pci_driver(felix_pci_driver);

MODULE_DESCRIPTION("Felix Switch driver");
MODULE_LICENSE("GPL v2");
