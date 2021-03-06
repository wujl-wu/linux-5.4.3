/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright 2019 NXP Semiconductors
 */
#ifndef _MSCC_FELIX_H
#define _MSCC_FELIX_H

#define ocelot_to_felix(o)		container_of((o), struct felix, ocelot)

/* Platform-specific information */
struct felix_info {
	struct resource			*target_io_res;
	struct resource			*port_io_res;
	struct resource			*imdio_res;
	const struct reg_field		*regfields;
	const u32 *const		*map;
	const struct ocelot_ops		*ops;
	int				shared_queue_sz;
	int				num_mact_rows;
	const struct ocelot_stat_layout	*stats_layout;
	unsigned int			num_stats;
	int				num_ports;
	int				num_tx_queues;
	const struct vcap_props		*vcap;
	int				switch_pci_bar;
	int				imdio_pci_bar;
	int				policer_base;
	int	(*mdio_bus_alloc)(struct ocelot *ocelot);
	void	(*mdio_bus_free)(struct ocelot *ocelot);
	void	(*pcs_init)(struct ocelot *ocelot, int port,
			    unsigned int link_an_mode,
			    const struct phylink_link_state *state);
	void	(*pcs_link_state)(struct ocelot *ocelot, int port,
				  struct phylink_link_state *state);
	int	(*prevalidate_phy_mode)(struct ocelot *ocelot, int port,
					phy_interface_t phy_mode);
	int	(*port_setup_tc)(struct dsa_switch *ds, int port,
				 enum tc_setup_type type, void *type_data);
	void	(*port_sched_speed_set)(struct ocelot *ocelot, int port,
					u32 speed);
	int	(*port_set_preempt)(struct ocelot *ocelot, int port,
				    struct ethtool_fp *fpcmd);
	int	(*port_get_preempt)(struct ocelot *ocelot, int port,
				    struct ethtool_fp *fpcmd);
	int	(*flower_replace)(struct ocelot *ocelot, int port,
				  struct flow_cls_offload *f, bool ingress);
	int	(*flower_destroy)(struct ocelot *ocelot, int port,
				  struct flow_cls_offload *f, bool ingress);
	int	(*flower_stats)(struct ocelot *ocelot, int port,
				struct flow_cls_offload *f, bool ingress);
	void	(*psfp_init)(struct ocelot *ocelot);
};

extern struct felix_info		felix_info_vsc9959;

enum felix_instance {
	FELIX_INSTANCE_VSC9959		= 0,
};

/* DSA glue / front-end for struct ocelot */
struct felix {
	struct dsa_switch		*ds;
	struct pci_dev			*pdev;
	struct felix_info		*info;
	struct ocelot			ocelot;
	struct mii_bus			*imdio;
	struct phy_device		**pcs;
};

void vsc9959_new_base_time(struct ocelot *ocelot, ktime_t base_time,
			   u64 cycle_time, struct timespec64 *new_base_ts);
int felix_flower_stream_replace(struct ocelot *ocelot, int port,
				struct flow_cls_offload *f, bool ingress);
int felix_flower_stream_destroy(struct ocelot *ocelot, int port,
				struct flow_cls_offload *f, bool ingress);
int felix_flower_stream_stats(struct ocelot *ocelot, int port,
			      struct flow_cls_offload *f, bool ingress);
void felix_psfp_init(struct ocelot *ocelot);
struct net_device *felix_port_to_netdev(struct ocelot *ocelot, int port);
int felix_netdev_to_port(struct net_device *dev);

#endif
