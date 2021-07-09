// SPDX-License-Identifier: (GPL-2.0 OR MIT)
/* Microsemi Ocelot Switch driver
 * Copyright (c) 2019 Microsemi Corporation
 */

#include <linux/iopoll.h>
#include <linux/proc_fs.h>

#include <soc/mscc/ocelot_vcap.h>
#include "ocelot_police.h"
#include "ocelot_vcap.h"

#define ENTRY_WIDTH 32

enum vcap_sel {
	VCAP_SEL_ENTRY = 0x1,
	VCAP_SEL_ACTION = 0x2,
	VCAP_SEL_COUNTER = 0x4,
	VCAP_SEL_ALL = 0x7,
};

enum vcap_cmd {
	VCAP_CMD_WRITE = 0, /* Copy from Cache to TCAM */
	VCAP_CMD_READ = 1, /* Copy from TCAM to Cache */
	VCAP_CMD_MOVE_UP = 2, /* Move <count> up */
	VCAP_CMD_MOVE_DOWN = 3, /* Move <count> down */
	VCAP_CMD_INITIALIZE = 4, /* Write all (from cache) */
};

#define VCAP_ENTRY_WIDTH 12 /* Max entry width (32bit words) */
#define VCAP_COUNTER_WIDTH 4 /* Max counter width (32bit words) */

struct vcap_data {
	u32 entry[VCAP_ENTRY_WIDTH]; /* ENTRY_DAT */
	u32 mask[VCAP_ENTRY_WIDTH]; /* MASK_DAT */
	u32 action[VCAP_ENTRY_WIDTH]; /* ACTION_DAT */
	u32 counter[VCAP_COUNTER_WIDTH]; /* CNT_DAT */
	u32 tg; /* TG_DAT */
	u32 type; /* Action type */
	u32 tg_sw; /* Current type-group */
	u32 cnt; /* Current counter */
	u32 key_offset; /* Current entry offset */
	u32 action_offset; /* Current action offset */
	u32 counter_offset; /* Current counter offset */
	u32 tg_value; /* Current type-group value */
	u32 tg_mask; /* Current type-group mask */
};

static u32 vcap_read_update_ctrl(struct ocelot *ocelot,
				 const struct vcap_props *vcap)
{
	return ocelot_target_read(ocelot, vcap->target, VCAP_CORE_UPDATE_CTRL);
}

static void vcap_cmd(struct ocelot *ocelot, const struct vcap_props *vcap,
		     u16 ix, int cmd, int sel)
{
	u32 value = (VCAP_CORE_UPDATE_CTRL_UPDATE_CMD(cmd) |
		     VCAP_CORE_UPDATE_CTRL_UPDATE_ADDR(ix) |
		     VCAP_CORE_UPDATE_CTRL_UPDATE_SHOT);

	if ((sel & VCAP_SEL_ENTRY) && ix >= vcap->entry_count)
		return;

	if (!(sel & VCAP_SEL_ENTRY))
		value |= VCAP_CORE_UPDATE_CTRL_UPDATE_ENTRY_DIS;

	if (!(sel & VCAP_SEL_ACTION))
		value |= VCAP_CORE_UPDATE_CTRL_UPDATE_ACTION_DIS;

	if (!(sel & VCAP_SEL_COUNTER))
		value |= VCAP_CORE_UPDATE_CTRL_UPDATE_CNT_DIS;

	ocelot_target_write(ocelot, vcap->target, value, VCAP_CORE_UPDATE_CTRL);

	read_poll_timeout(vcap_read_update_ctrl, value,
			  (value & VCAP_CORE_UPDATE_CTRL_UPDATE_SHOT) == 0,
			  10, 100000, false, ocelot, vcap);
}

/* Convert from 0-based row to VCAP entry row and run command */
static void vcap_row_cmd(struct ocelot *ocelot, const struct vcap_props *vcap,
			 u32 row, int cmd, int sel)
{
	vcap_cmd(ocelot, vcap, vcap->entry_count - row - 1, cmd, sel);
}

static void vcap_entry2cache(struct ocelot *ocelot,
			     const struct vcap_props *vcap,
			     struct vcap_data *data)
{
	u32 entry_words, i;

	entry_words = DIV_ROUND_UP(vcap->entry_width, ENTRY_WIDTH);

	for (i = 0; i < entry_words; i++) {
		ocelot_target_write_rix(ocelot, vcap->target, data->entry[i],
					VCAP_CACHE_ENTRY_DAT, i);
		ocelot_target_write_rix(ocelot, vcap->target, ~data->mask[i],
					VCAP_CACHE_MASK_DAT, i);
	}
	ocelot_target_write(ocelot, vcap->target, data->tg, VCAP_CACHE_TG_DAT);
}

static void vcap_cache2entry(struct ocelot *ocelot,
			     const struct vcap_props *vcap,
			     struct vcap_data *data)
{
	u32 entry_words, i;

	entry_words = DIV_ROUND_UP(vcap->entry_width, ENTRY_WIDTH);

	for (i = 0; i < entry_words; i++) {
		data->entry[i] = ocelot_target_read_rix(ocelot, vcap->target,
							VCAP_CACHE_ENTRY_DAT, i);
		// Invert mask
		data->mask[i] = ~ocelot_target_read_rix(ocelot, vcap->target,
							VCAP_CACHE_MASK_DAT, i);
	}
	data->tg = ocelot_target_read(ocelot, vcap->target, VCAP_CACHE_TG_DAT);
}

static void vcap_action2cache(struct ocelot *ocelot,
			      const struct vcap_props *vcap,
			      struct vcap_data *data)
{
	u32 action_words, i, width, mask;

	/* Encode action type */
	width = vcap->action_type_width;
	if (width) {
		mask = GENMASK(width, 0);
		data->action[0] = ((data->action[0] & ~mask) | data->type);
	}

	action_words = DIV_ROUND_UP(vcap->action_width, ENTRY_WIDTH);

	for (i = 0; i < action_words; i++)
		ocelot_target_write_rix(ocelot, vcap->target, data->action[i],
					VCAP_CACHE_ACTION_DAT, i);

	for (i = 0; i < vcap->counter_words; i++)
		ocelot_target_write_rix(ocelot, vcap->target, data->counter[i],
					VCAP_CACHE_CNT_DAT, i);
}

static void vcap_cache2action(struct ocelot *ocelot,
			      const struct vcap_props *vcap,
			      struct vcap_data *data)
{
	u32 action_words, i, width;

	action_words = DIV_ROUND_UP(vcap->action_width, ENTRY_WIDTH);

	for (i = 0; i < action_words; i++)
		data->action[i] = ocelot_target_read_rix(ocelot, vcap->target,
							 VCAP_CACHE_ACTION_DAT,
							 i);

	for (i = 0; i < vcap->counter_words; i++)
		data->counter[i] = ocelot_target_read_rix(ocelot, vcap->target,
							  VCAP_CACHE_CNT_DAT,
							  i);

	/* Extract action type */
	width = vcap->action_type_width;
	data->type = (width ? (data->action[0] & GENMASK(width, 0)) : 0);
}

/* Calculate offsets for entry */
static void vcap_data_offset_get(const struct vcap_props *vcap,
				 struct vcap_data *data, int ix)
{
	int num_subwords_per_entry, num_subwords_per_action;
	int i, col, offset, num_entries_per_row, base;
	u32 width = vcap->tg_width;

	num_entries_per_row = (1 << (data->tg_sw - 1));
	col = (ix % num_entries_per_row);
	num_subwords_per_entry = (vcap->sw_count / num_entries_per_row);
	base = (vcap->sw_count - col * num_subwords_per_entry -
		num_subwords_per_entry);
	data->tg_value = 0;
	data->tg_mask = 0;
	for (i = 0; i < num_subwords_per_entry; i++) {
		offset = ((base + i) * width);
		data->tg_value |= (data->tg_sw << offset);
		data->tg_mask |= GENMASK(offset + width - 1, offset);
	}

	/* Calculate key/action/counter offsets */
	col = (num_entries_per_row - col - 1);
	data->key_offset = (base * vcap->entry_width) / vcap->sw_count;
	data->counter_offset = (num_subwords_per_entry * col *
				vcap->counter_width);
	i = data->type;
	width = vcap->action_table[i].width;
	num_subwords_per_action = vcap->action_table[i].count;
	data->action_offset = ((num_subwords_per_action * col * width) /
				num_entries_per_row);
	data->action_offset += vcap->action_type_width;
}

static void vcap_data_set(u32 *data, u32 offset, u32 len, u32 value)
{
	u32 i, v, m;

	for (i = 0; i < len; i++, offset++) {
		v = data[offset / ENTRY_WIDTH];
		m = (1 << (offset % ENTRY_WIDTH));
		if (value & (1 << i))
			v |= m;
		else
			v &= ~m;
		data[offset / ENTRY_WIDTH] = v;
	}
}

static u32 vcap_data_get(u32 *data, u32 offset, u32 len)
{
	u32 i, v, m, value = 0;

	for (i = 0; i < len; i++, offset++) {
		v = data[offset / ENTRY_WIDTH];
		m = (1 << (offset % ENTRY_WIDTH));
		if (v & m)
			value |= (1 << i);
	}
	return value;
}

static void vcap_key_field_set(struct vcap_data *data, u32 offset, u32 width,
			       u32 value, u32 mask)
{
	vcap_data_set(data->entry, offset + data->key_offset, width, value);
	vcap_data_set(data->mask, offset + data->key_offset, width, mask);
}

static void vcap_key_set(const struct vcap_props *vcap, struct vcap_data *data,
			 int field, u32 value, u32 mask)
{
	u32 offset = vcap->keys[field].offset;
	u32 length = vcap->keys[field].length;

	vcap_key_field_set(data, offset, length, value, mask);
}

static void vcap_key_bytes_set(const struct vcap_props *vcap,
			       struct vcap_data *data, int field,
			       u8 *val, u8 *msk)
{
	u32 offset = vcap->keys[field].offset;
	u32 count  = vcap->keys[field].length;
	u32 i, j, n = 0, value = 0, mask = 0;

	WARN_ON(count % 8);

	/* Data wider than 32 bits are split up in chunks of maximum 32 bits.
	 * The 32 LSB of the data are written to the 32 MSB of the TCAM.
	 */
	offset += count;
	count /= 8;

	for (i = 0; i < count; i++) {
		j = (count - i - 1);
		value += (val[j] << n);
		mask += (msk[j] << n);
		n += 8;
		if (n == ENTRY_WIDTH || (i + 1) == count) {
			offset -= n;
			vcap_key_field_set(data, offset, n, value, mask);
			n = 0;
			value = 0;
			mask = 0;
		}
	}
}

static void vcap_key_l4_port_set(const struct vcap_props *vcap,
				 struct vcap_data *data, int field,
				 struct ocelot_vcap_udp_tcp *port)
{
	u32 offset = vcap->keys[field].offset;
	u32 length = vcap->keys[field].length;

	WARN_ON(length != 16);

	vcap_key_field_set(data, offset, length, port->value, port->mask);
}

static void vcap_key_bit_set(const struct vcap_props *vcap,
			     struct vcap_data *data, int field,
			     enum ocelot_vcap_bit val)
{
	u32 value = (val == OCELOT_VCAP_BIT_1 ? 1 : 0);
	u32 msk = (val == OCELOT_VCAP_BIT_ANY ? 0 : 1);
	u32 offset = vcap->keys[field].offset;
	u32 length = vcap->keys[field].length;

	WARN_ON(length != 1);

	vcap_key_field_set(data, offset, length, value, msk);
}

static void vcap_action_set(const struct vcap_props *vcap,
			    struct vcap_data *data, int field, u32 value)
{
	int offset = vcap->actions[field].offset;
	int length = vcap->actions[field].length;

	vcap_data_set(data->action, offset + data->action_offset, length,
		       value);
}

static void is2_action_set(struct ocelot *ocelot, struct vcap_data *data,
			   struct ocelot_vcap_filter *filter)
{
	const struct vcap_props *vcap = &ocelot->vcap[VCAP_IS2];
	struct ocelot_vcap_action *a = &filter->action;

	vcap_action_set(vcap, data, VCAP_IS2_ACT_MASK_MODE, a->mask_mode);
	vcap_action_set(vcap, data, VCAP_IS2_ACT_PORT_MASK, a->port_mask);
	vcap_action_set(vcap, data, VCAP_IS2_ACT_POLICE_ENA, a->police_ena);
	vcap_action_set(vcap, data, VCAP_IS2_ACT_POLICE_IDX,
			a->pol_ix);
	vcap_action_set(vcap, data, VCAP_IS2_ACT_CPU_QU_NUM, a->cpu_qu_num);
	vcap_action_set(vcap, data, VCAP_IS2_ACT_CPU_COPY_ENA, a->cpu_copy_ena);
}

static void is2_entry_set(struct ocelot *ocelot, int ix,
			  struct ocelot_vcap_filter *filter)
{
	const struct vcap_props *vcap = &ocelot->vcap[VCAP_IS2];
	u32 val, msk, type, type_mask = 0xf, i, count;
	struct ocelot_vcap_key_vlan *tag = &filter->vlan;
	struct ocelot_vcap_u64 payload;
	struct vcap_data data;
	int row = (ix / 2);

	memset(&payload, 0, sizeof(payload));
	memset(&data, 0, sizeof(data));

	/* Read row */
	vcap_row_cmd(ocelot, vcap, row, VCAP_CMD_READ, VCAP_SEL_ALL);
	vcap_cache2entry(ocelot, vcap, &data);
	vcap_cache2action(ocelot, vcap, &data);

	data.tg_sw = VCAP_TG_HALF;
	vcap_data_offset_get(vcap, &data, ix);
	data.tg = (data.tg & ~data.tg_mask);
	if (filter->prio != 0)
		data.tg |= data.tg_value;

	data.type = IS2_ACTION_TYPE_NORMAL;

	vcap_key_set(vcap, &data, VCAP_IS2_HK_PAG, filter->pag, 0xff);
	vcap_key_set(vcap, &data, VCAP_IS2_HK_IGR_PORT_MASK, 0,
		     ~filter->ingress_port_mask);
	vcap_key_bit_set(vcap, &data, VCAP_IS2_HK_FIRST,
			 (filter->lookup == 0) ? OCELOT_VCAP_BIT_1 :
			 OCELOT_VCAP_BIT_0);
	vcap_key_bit_set(vcap, &data, VCAP_IS2_HK_HOST_MATCH,
			 OCELOT_VCAP_BIT_ANY);
	vcap_key_bit_set(vcap, &data, VCAP_IS2_HK_L2_MC, filter->dmac_mc);
	vcap_key_bit_set(vcap, &data, VCAP_IS2_HK_L2_BC, filter->dmac_bc);
	vcap_key_bit_set(vcap, &data, VCAP_IS2_HK_VLAN_TAGGED, tag->tagged);
	vcap_key_set(vcap, &data, VCAP_IS2_HK_VID,
		     tag->vid.value, tag->vid.mask);
	vcap_key_set(vcap, &data, VCAP_IS2_HK_PCP,
		     tag->pcp.value[0], tag->pcp.mask[0]);
	vcap_key_bit_set(vcap, &data, VCAP_IS2_HK_DEI, tag->dei);

	switch (filter->key_type) {
	case OCELOT_VCAP_KEY_ETYPE: {
		struct ocelot_vcap_key_etype *etype = &filter->key.etype;

		type = IS2_TYPE_ETYPE;
		vcap_key_bytes_set(vcap, &data, VCAP_IS2_HK_L2_DMAC,
				   etype->dmac.value, etype->dmac.mask);
		vcap_key_bytes_set(vcap, &data, VCAP_IS2_HK_L2_SMAC,
				   etype->smac.value, etype->smac.mask);
		vcap_key_bytes_set(vcap, &data, VCAP_IS2_HK_MAC_ETYPE_ETYPE,
				   etype->etype.value, etype->etype.mask);
		/* Clear unused bits */
		vcap_key_set(vcap, &data, VCAP_IS2_HK_MAC_ETYPE_L2_PAYLOAD0,
			     0, 0);
		vcap_key_set(vcap, &data, VCAP_IS2_HK_MAC_ETYPE_L2_PAYLOAD1,
			     0, 0);
		vcap_key_set(vcap, &data, VCAP_IS2_HK_MAC_ETYPE_L2_PAYLOAD2,
			     0, 0);
		vcap_key_bytes_set(vcap, &data,
				   VCAP_IS2_HK_MAC_ETYPE_L2_PAYLOAD0,
				   etype->data.value, etype->data.mask);
		break;
	}
	case OCELOT_VCAP_KEY_LLC: {
		struct ocelot_vcap_key_llc *llc = &filter->key.llc;

		type = IS2_TYPE_LLC;
		vcap_key_bytes_set(vcap, &data, VCAP_IS2_HK_L2_DMAC,
				   llc->dmac.value, llc->dmac.mask);
		vcap_key_bytes_set(vcap, &data, VCAP_IS2_HK_L2_SMAC,
				   llc->smac.value, llc->smac.mask);
		for (i = 0; i < 4; i++) {
			payload.value[i] = llc->llc.value[i];
			payload.mask[i] = llc->llc.mask[i];
		}
		vcap_key_bytes_set(vcap, &data, VCAP_IS2_HK_MAC_LLC_L2_LLC,
				   payload.value, payload.mask);
		break;
	}
	case OCELOT_VCAP_KEY_SNAP: {
		struct ocelot_vcap_key_snap *snap = &filter->key.snap;

		type = IS2_TYPE_SNAP;
		vcap_key_bytes_set(vcap, &data, VCAP_IS2_HK_L2_DMAC,
				   snap->dmac.value, snap->dmac.mask);
		vcap_key_bytes_set(vcap, &data, VCAP_IS2_HK_L2_SMAC,
				   snap->smac.value, snap->smac.mask);
		vcap_key_bytes_set(vcap, &data, VCAP_IS2_HK_MAC_SNAP_L2_SNAP,
				   filter->key.snap.snap.value,
				   filter->key.snap.snap.mask);
		break;
	}
	case OCELOT_VCAP_KEY_ARP: {
		struct ocelot_vcap_key_arp *arp = &filter->key.arp;

		type = IS2_TYPE_ARP;
		vcap_key_bytes_set(vcap, &data, VCAP_IS2_HK_MAC_ARP_SMAC,
				   arp->smac.value, arp->smac.mask);
		vcap_key_bit_set(vcap, &data,
				 VCAP_IS2_HK_MAC_ARP_ADDR_SPACE_OK,
				 arp->ethernet);
		vcap_key_bit_set(vcap, &data,
				 VCAP_IS2_HK_MAC_ARP_PROTO_SPACE_OK,
				 arp->ip);
		vcap_key_bit_set(vcap, &data,
				 VCAP_IS2_HK_MAC_ARP_LEN_OK,
				 arp->length);
		vcap_key_bit_set(vcap, &data,
				 VCAP_IS2_HK_MAC_ARP_TARGET_MATCH,
				 arp->dmac_match);
		vcap_key_bit_set(vcap, &data,
				 VCAP_IS2_HK_MAC_ARP_SENDER_MATCH,
				 arp->smac_match);
		vcap_key_bit_set(vcap, &data,
				 VCAP_IS2_HK_MAC_ARP_OPCODE_UNKNOWN,
				 arp->unknown);

		/* OPCODE is inverse, bit 0 is reply flag, bit 1 is RARP flag */
		val = ((arp->req == OCELOT_VCAP_BIT_0 ? 1 : 0) |
		       (arp->arp == OCELOT_VCAP_BIT_0 ? 2 : 0));
		msk = ((arp->req == OCELOT_VCAP_BIT_ANY ? 0 : 1) |
		       (arp->arp == OCELOT_VCAP_BIT_ANY ? 0 : 2));
		vcap_key_set(vcap, &data, VCAP_IS2_HK_MAC_ARP_OPCODE,
			     val, msk);
		vcap_key_bytes_set(vcap, &data,
				   VCAP_IS2_HK_MAC_ARP_L3_IP4_DIP,
				   arp->dip.value.addr, arp->dip.mask.addr);
		vcap_key_bytes_set(vcap, &data,
				   VCAP_IS2_HK_MAC_ARP_L3_IP4_SIP,
				   arp->sip.value.addr, arp->sip.mask.addr);
		vcap_key_set(vcap, &data, VCAP_IS2_HK_MAC_ARP_DIP_EQ_SIP,
			     0, 0);
		break;
	}
	case OCELOT_VCAP_KEY_IPV4:
	case OCELOT_VCAP_KEY_IPV6: {
		enum ocelot_vcap_bit sip_eq_dip, sport_eq_dport, seq_zero, tcp;
		enum ocelot_vcap_bit ttl, fragment, options, tcp_ack, tcp_urg;
		enum ocelot_vcap_bit tcp_fin, tcp_syn, tcp_rst, tcp_psh;
		struct ocelot_vcap_key_ipv4 *ipv4 = NULL;
		struct ocelot_vcap_key_ipv6 *ipv6 = NULL;
		struct ocelot_vcap_udp_tcp *sport, *dport;
		struct ocelot_vcap_ipv4 sip, dip;
		struct ocelot_vcap_u8 proto, ds;
		struct ocelot_vcap_u48 *ip_data;

		if (filter->key_type == OCELOT_VCAP_KEY_IPV4) {
			ipv4 = &filter->key.ipv4;
			ttl = ipv4->ttl;
			fragment = ipv4->fragment;
			options = ipv4->options;
			proto = ipv4->proto;
			ds = ipv4->ds;
			ip_data = &ipv4->data;
			sip = ipv4->sip;
			dip = ipv4->dip;
			sport = &ipv4->sport;
			dport = &ipv4->dport;
			tcp_fin = ipv4->tcp_fin;
			tcp_syn = ipv4->tcp_syn;
			tcp_rst = ipv4->tcp_rst;
			tcp_psh = ipv4->tcp_psh;
			tcp_ack = ipv4->tcp_ack;
			tcp_urg = ipv4->tcp_urg;
			sip_eq_dip = ipv4->sip_eq_dip;
			sport_eq_dport = ipv4->sport_eq_dport;
			seq_zero = ipv4->seq_zero;
		} else {
			ipv6 = &filter->key.ipv6;
			ttl = ipv6->ttl;
			fragment = OCELOT_VCAP_BIT_ANY;
			options = OCELOT_VCAP_BIT_ANY;
			proto = ipv6->proto;
			ds = ipv6->ds;
			ip_data = &ipv6->data;
			for (i = 0; i < 8; i++) {
				val = ipv6->sip.value[i + 8];
				msk = ipv6->sip.mask[i + 8];
				if (i < 4) {
					dip.value.addr[i] = val;
					dip.mask.addr[i] = msk;
				} else {
					sip.value.addr[i - 4] = val;
					sip.mask.addr[i - 4] = msk;
				}
			}
			sport = &ipv6->sport;
			dport = &ipv6->dport;
			tcp_fin = ipv6->tcp_fin;
			tcp_syn = ipv6->tcp_syn;
			tcp_rst = ipv6->tcp_rst;
			tcp_psh = ipv6->tcp_psh;
			tcp_ack = ipv6->tcp_ack;
			tcp_urg = ipv6->tcp_urg;
			sip_eq_dip = ipv6->sip_eq_dip;
			sport_eq_dport = ipv6->sport_eq_dport;
			seq_zero = ipv6->seq_zero;
		}

		vcap_key_bit_set(vcap, &data, VCAP_IS2_HK_IP4,
				 ipv4 ? OCELOT_VCAP_BIT_1 : OCELOT_VCAP_BIT_0);
		vcap_key_bit_set(vcap, &data, VCAP_IS2_HK_L3_FRAGMENT,
				 fragment);
		vcap_key_set(vcap, &data, VCAP_IS2_HK_L3_FRAG_OFS_GT0, 0, 0);
		vcap_key_bit_set(vcap, &data, VCAP_IS2_HK_L3_OPTIONS,
				 options);
		vcap_key_bit_set(vcap, &data, VCAP_IS2_HK_IP4_L3_TTL_GT0,
				 ttl);
		vcap_key_bytes_set(vcap, &data, VCAP_IS2_HK_L3_TOS,
				   ds.value, ds.mask);
		vcap_key_bytes_set(vcap, &data, VCAP_IS2_HK_L3_IP4_DIP,
				   dip.value.addr, dip.mask.addr);
		vcap_key_bytes_set(vcap, &data, VCAP_IS2_HK_L3_IP4_SIP,
				   sip.value.addr, sip.mask.addr);
		vcap_key_bit_set(vcap, &data, VCAP_IS2_HK_DIP_EQ_SIP,
				 sip_eq_dip);
		val = proto.value[0];
		msk = proto.mask[0];
		type = IS2_TYPE_IP_UDP_TCP;
		if (msk == 0xff && (val == 6 || val == 17)) {
			/* UDP/TCP protocol match */
			tcp = (val == 6 ?
			       OCELOT_VCAP_BIT_1 : OCELOT_VCAP_BIT_0);
			vcap_key_bit_set(vcap, &data, VCAP_IS2_HK_TCP, tcp);
			vcap_key_l4_port_set(vcap, &data,
					     VCAP_IS2_HK_L4_DPORT, dport);
			vcap_key_l4_port_set(vcap, &data,
					     VCAP_IS2_HK_L4_SPORT, sport);
			vcap_key_set(vcap, &data, VCAP_IS2_HK_L4_RNG, 0, 0);
			vcap_key_bit_set(vcap, &data,
					 VCAP_IS2_HK_L4_SPORT_EQ_DPORT,
					 sport_eq_dport);
			vcap_key_bit_set(vcap, &data,
					 VCAP_IS2_HK_L4_SEQUENCE_EQ0,
					 seq_zero);
			vcap_key_bit_set(vcap, &data, VCAP_IS2_HK_L4_FIN,
					 tcp_fin);
			vcap_key_bit_set(vcap, &data, VCAP_IS2_HK_L4_SYN,
					 tcp_syn);
			vcap_key_bit_set(vcap, &data, VCAP_IS2_HK_L4_RST,
					 tcp_rst);
			vcap_key_bit_set(vcap, &data, VCAP_IS2_HK_L4_PSH,
					 tcp_psh);
			vcap_key_bit_set(vcap, &data, VCAP_IS2_HK_L4_ACK,
					 tcp_ack);
			vcap_key_bit_set(vcap, &data, VCAP_IS2_HK_L4_URG,
					 tcp_urg);
			vcap_key_set(vcap, &data, VCAP_IS2_HK_L4_1588_DOM,
				     0, 0);
			vcap_key_set(vcap, &data, VCAP_IS2_HK_L4_1588_VER,
				     0, 0);
		} else {
			if (msk == 0) {
				/* Any IP protocol match */
				type_mask = IS2_TYPE_MASK_IP_ANY;
			} else {
				/* Non-UDP/TCP protocol match */
				type = IS2_TYPE_IP_OTHER;
				for (i = 0; i < 6; i++) {
					payload.value[i] = ip_data->value[i];
					payload.mask[i] = ip_data->mask[i];
				}
			}
			vcap_key_bytes_set(vcap, &data,
					   VCAP_IS2_HK_IP4_L3_PROTO,
					   proto.value, proto.mask);
			vcap_key_bytes_set(vcap, &data,
					   VCAP_IS2_HK_L3_PAYLOAD,
					   payload.value, payload.mask);
		}
		break;
	}
	case OCELOT_VCAP_KEY_ANY:
	default:
		type = 0;
		type_mask = 0;
		count = vcap->entry_width / 2;
		/* Iterate over the non-common part of the key and
		 * clear entry data
		 */
		for (i = vcap->keys[VCAP_IS2_HK_L2_DMAC].offset;
		     i < count; i += ENTRY_WIDTH) {
			vcap_key_field_set(&data, i, min(32u, count - i), 0, 0);
		}
		break;
	}

	vcap_key_set(vcap, &data, VCAP_IS2_TYPE, type, type_mask);
	is2_action_set(ocelot, &data, filter);
	vcap_data_set(data.counter, data.counter_offset,
		       vcap->counter_width, filter->stats.pkts);

	/* Write row */
	vcap_entry2cache(ocelot, vcap, &data);
	vcap_action2cache(ocelot, vcap, &data);
	vcap_row_cmd(ocelot, vcap, row, VCAP_CMD_WRITE, VCAP_SEL_ALL);
}

static void is1_action_set(struct ocelot *ocelot, struct vcap_data *data,
			   struct ocelot_vcap_filter *filter)
{
	const struct vcap_props *vcap = &ocelot->vcap[VCAP_IS1];
	const struct ocelot_vcap_action *a = &filter->action;

	vcap_action_set(vcap, data, VCAP_IS1_ACT_VID_REPLACE_ENA,
			a->vid_replace_ena);
	vcap_action_set(vcap, data, VCAP_IS1_ACT_VID_ADD_VAL, a->vid);
	vcap_action_set(vcap, data, VCAP_IS1_ACT_VLAN_POP_CNT_ENA,
			a->vlan_pop_cnt_ena);
	vcap_action_set(vcap, data, VCAP_IS1_ACT_VLAN_POP_CNT,
			a->vlan_pop_cnt);
	vcap_action_set(vcap, data, VCAP_IS1_ACT_PCP_DEI_ENA, a->pcp_dei_ena);
	vcap_action_set(vcap, data, VCAP_IS1_ACT_PCP_VAL, a->pcp);
	vcap_action_set(vcap, data, VCAP_IS1_ACT_DEI_VAL, a->dei);
	vcap_action_set(vcap, data, VCAP_IS1_ACT_QOS_ENA, a->qos_ena);
	vcap_action_set(vcap, data, VCAP_IS1_ACT_QOS_VAL, a->qos_val);
	vcap_action_set(vcap, data, VCAP_IS1_ACT_PAG_OVERRIDE_MASK,
			a->pag_override_mask);
	vcap_action_set(vcap, data, VCAP_IS1_ACT_PAG_VAL, a->pag_val);
}

static int is1_entry_set(struct ocelot *ocelot, int ix,
			  struct ocelot_vcap_filter *filter)
{
	const struct vcap_props *vcap = &ocelot->vcap[VCAP_IS1];
	u32 val, msk, type, i;
	struct ocelot_vcap_key_vlan *tag = &filter->vlan;
	struct ocelot_vcap_key_vlan *ctag = &filter->cvlan;
	struct ocelot_vcap_u64 payload;
	struct vcap_data data;
	int row = ix / 2;

	memset(&payload, 0, sizeof(payload));
	memset(&data, 0, sizeof(data));

	/* Read row */
	vcap_row_cmd(ocelot, vcap, row, VCAP_CMD_READ, VCAP_SEL_ALL);
	vcap_cache2entry(ocelot, vcap, &data);
	vcap_cache2action(ocelot, vcap, &data);

	data.tg_sw = VCAP_TG_HALF;
	data.type = IS1_ACTION_TYPE_NORMAL;
	vcap_data_offset_get(vcap, &data, ix);
	data.tg = (data.tg & ~data.tg_mask);
	if (filter->prio != 0)
		data.tg |= data.tg_value;

	if (filter->lookup == 1)
		type = IS1_TYPE_S1_5TUPLE_IP4;
	else
		type = IS1_TYPE_S1_NORMAL;

	vcap_key_set(vcap, &data, VCAP_IS1_HK_LOOKUP, filter->lookup, 0x3);
	vcap_key_set(vcap, &data, VCAP_IS1_HK_IGR_PORT_MASK, 0,
		     ~filter->ingress_port_mask);
	vcap_key_bit_set(vcap, &data, VCAP_IS1_HK_L2_MC, filter->dmac_mc);
	vcap_key_bit_set(vcap, &data, VCAP_IS1_HK_L2_BC, filter->dmac_bc);
	vcap_key_bit_set(vcap, &data, VCAP_IS1_HK_VLAN_TAGGED, tag->tagged);
	vcap_key_set(vcap, &data, VCAP_IS1_HK_VID,
		     tag->vid.value, tag->vid.mask);
	vcap_key_set(vcap, &data, VCAP_IS1_HK_PCP,
		     tag->pcp.value[0], tag->pcp.mask[0]);
	if (ctag->vid.value != 0) {
		if (type != IS1_TYPE_S1_5TUPLE_IP4)
			return -EOPNOTSUPP;

		vcap_key_bit_set(vcap, &data, VCAP_IS1_HK_TPID,
				 OCELOT_VCAP_BIT_1);
		vcap_key_bit_set(vcap, &data, VCAP_IS1_HK_VLAN_DBL_TAGGED,
				 OCELOT_VCAP_BIT_1);
		vcap_key_set(vcap, &data, VCAP_IS1_HK_IP4_INNER_VID,
			     ctag->vid.value, ctag->vid.mask);
		vcap_key_set(vcap, &data, VCAP_IS1_HK_IP4_INNER_PCP,
			     ctag->pcp.value[0], ctag->pcp.mask[0]);
	}

	switch (filter->key_type) {
	case OCELOT_VCAP_KEY_ETYPE: {
		struct ocelot_vcap_key_etype *etype = &filter->key.etype;

		if (type != IS1_TYPE_S1_NORMAL)
                        return -EOPNOTSUPP;

		if (etype->dmac.mask[0])
			return -EOPNOTSUPP;

		vcap_key_bytes_set(vcap, &data, VCAP_IS1_HK_L2_SMAC,
				   etype->smac.value, etype->smac.mask);
		vcap_key_bytes_set(vcap, &data, VCAP_IS1_HK_ETYPE,
				   etype->etype.value, etype->etype.mask);
		break;
	}
	case OCELOT_VCAP_KEY_IPV4:
	case OCELOT_VCAP_KEY_IPV6: {
		enum ocelot_vcap_bit tcp;
		enum ocelot_vcap_bit fragment, options;
		struct ocelot_vcap_key_ipv4 *ipv4 = NULL;
		struct ocelot_vcap_key_ipv6 *ipv6 = NULL;
		struct ocelot_vcap_udp_tcp *sport, *dport;
		struct ocelot_vcap_ipv4 sip, dip;
		struct ocelot_vcap_u8 proto, ds;
		struct ocelot_vcap_u48 *ip_data;
		struct ocelot_vcap_u32 port;

		if (filter->key_type == OCELOT_VCAP_KEY_IPV4) {
			ipv4 = &filter->key.ipv4;
			fragment = ipv4->fragment;
			options = ipv4->options;
			proto = ipv4->proto;
			ds = ipv4->ds;
			ip_data = &ipv4->data;
			sip = ipv4->sip;
			dip = ipv4->dip;
			sport = &ipv4->sport;
			dport = &ipv4->dport;
		} else {
			ipv6 = &filter->key.ipv6;
			fragment = OCELOT_VCAP_BIT_ANY;
			options = OCELOT_VCAP_BIT_ANY;
			proto = ipv6->proto;
			ds = ipv6->ds;
			ip_data = &ipv6->data;
			for (i = 0; i < 4; i++) {
				dip.value.addr[i] = ipv6->dip.value[i];
				dip.mask.addr[i] = ipv6->dip.mask[i];
				sip.value.addr[i] = ipv6->sip.value[i];
				sip.mask.addr[i] = ipv6->sip.mask[i];
			}
			sport = &ipv6->sport;
			dport = &ipv6->dport;
		}

		if (type == IS1_TYPE_S1_5TUPLE_IP4) {
			vcap_key_bit_set(vcap, &data, VCAP_IS1_HK_IP4_IP4,
					 ipv4 ? OCELOT_VCAP_BIT_1 : OCELOT_VCAP_BIT_0);
			vcap_key_bit_set(vcap, &data,
					 VCAP_IS1_HK_IP4_L3_FRAGMENT,
					 fragment);
			vcap_key_bit_set(vcap, &data,
					 VCAP_IS1_HK_IP4_L3_OPTIONS,
					 options);
			vcap_key_bytes_set(vcap, &data,
					   VCAP_IS1_HK_IP4_L3_IP4_DIP,
					   dip.value.addr, dip.mask.addr);
			vcap_key_bytes_set(vcap, &data,
					   VCAP_IS1_HK_IP4_L3_IP4_SIP,
					   sip.value.addr, sip.mask.addr);
			val = proto.value[0];
			msk = proto.mask[0];
			if (msk == 0xff && (val == 6 || val == 17)) {
				/* UDP/TCP protocol match */
				tcp = (val == 6 ?
				       OCELOT_VCAP_BIT_1 : OCELOT_VCAP_BIT_0);
				vcap_key_bit_set(vcap, &data,
						 VCAP_IS1_HK_IP4_TCP,
						 tcp);
				vcap_key_l4_port_set(vcap, &data,
						     VCAP_IS1_HK_L4_SPORT,
						     sport);
				vcap_key_set(vcap, &data,
					     VCAP_IS1_HK_IP4_L4_RNG,
					     0, 0);
				port.value[0] = sport->value & 0xFF;
				port.value[1] = sport->value >> 8;
				port.value[2] = dport->value & 0xFF;
				port.value[3] = dport->value >> 8;
				port.mask[0] = sport->mask & 0xFF;
				port.mask[1] = sport->mask >> 8;
				port.mask[2] = dport->mask & 0xFF;
				port.mask[3] = dport->mask >> 8;
				vcap_key_bytes_set(vcap, &data,
						   VCAP_IS1_HK_IP4_IP_PAYLOAD_S1_5TUPLE,
						   port.value, port.mask);
			}
		} else if (type == IS1_TYPE_S1_NORMAL) {
			vcap_key_bit_set(vcap, &data, VCAP_IS1_HK_IP4,
					 ipv4 ? OCELOT_VCAP_BIT_1 : OCELOT_VCAP_BIT_0);
			vcap_key_bit_set(vcap, &data,
					 VCAP_IS1_HK_L3_FRAGMENT,
					 fragment);
			vcap_key_bit_set(vcap, &data,
					 VCAP_IS1_HK_L3_OPTIONS,
					 options);
			vcap_key_bit_set(vcap, &data,
					 VCAP_IS1_HK_ETYPE_LEN,
					 OCELOT_VCAP_BIT_1);
			vcap_key_bytes_set(vcap, &data,
					   VCAP_IS1_HK_L3_IP4_SIP,
					   sip.value.addr, sip.mask.addr);
			if (dip.mask.addr[0])
				return -EOPNOTSUPP;

			val = proto.value[0];
			msk = proto.mask[0];
			if (msk == 0xff && (val == 6 || val == 17)) {
				/* UDP/TCP protocol match */
				tcp = (val == 6 ?
				       OCELOT_VCAP_BIT_1 : OCELOT_VCAP_BIT_0);
				vcap_key_bit_set(vcap, &data,
						 VCAP_IS1_HK_TCP_UDP,
						 OCELOT_VCAP_BIT_1);
				vcap_key_bit_set(vcap, &data,
						 VCAP_IS1_HK_TCP,
						 tcp);
				vcap_key_l4_port_set(vcap, &data,
						     VCAP_IS1_HK_L4_SPORT,
						     sport);
			}
		}
		break;
	}
	default:
		break;
	}
	vcap_key_bit_set(vcap, &data, VCAP_IS1_HK_TYPE,
			 type ? OCELOT_VCAP_BIT_1 : OCELOT_VCAP_BIT_0);

	is1_action_set(ocelot, &data, filter);
	vcap_data_set(data.counter, data.counter_offset,
		      vcap->counter_width, filter->stats.pkts);

	/* Write row */
	vcap_entry2cache(ocelot, vcap, &data);
	vcap_action2cache(ocelot, vcap, &data);
	vcap_row_cmd(ocelot, vcap, row, VCAP_CMD_WRITE, VCAP_SEL_ALL);

	return 0;
}

static void es0_action_set(struct ocelot *ocelot, struct vcap_data *data,
			   struct ocelot_vcap_filter *filter)
{
	const struct vcap_props *vcap = &ocelot->vcap[VCAP_ES0];
	const struct ocelot_vcap_action *a = &filter->action;

	vcap_action_set(vcap, data, VCAP_ES0_ACT_PUSH_OUTER_TAG,
			a->push_outer_tag);
	vcap_action_set(vcap, data, VCAP_ES0_ACT_PUSH_INNER_TAG,
			a->push_inner_tag);
	vcap_action_set(vcap, data, VCAP_ES0_ACT_TAG_A_TPID_SEL,
			a->tag_a_tpid_sel);
	vcap_action_set(vcap, data, VCAP_ES0_ACT_TAG_A_VID_SEL,
			a->tag_a_vid_sel);
	vcap_action_set(vcap, data, VCAP_ES0_ACT_TAG_A_PCP_SEL,
			a->tag_a_pcp_sel);
	vcap_action_set(vcap, data, VCAP_ES0_ACT_VID_A_VAL, a->vid_a_val);
	vcap_action_set(vcap, data, VCAP_ES0_ACT_PCP_A_VAL, a->pcp_a_val);
	vcap_action_set(vcap, data, VCAP_ES0_ACT_TAG_B_TPID_SEL,
			a->tag_b_tpid_sel);
	vcap_action_set(vcap, data, VCAP_ES0_ACT_TAG_B_VID_SEL,
			a->tag_b_vid_sel);
	vcap_action_set(vcap, data, VCAP_ES0_ACT_TAG_B_PCP_SEL,
			a->tag_b_pcp_sel);
	vcap_action_set(vcap, data, VCAP_ES0_ACT_VID_B_VAL, a->vid_b_val);
	vcap_action_set(vcap, data, VCAP_ES0_ACT_PCP_B_VAL, a->pcp_b_val);
}

static void es0_entry_set(struct ocelot *ocelot, int ix,
			  struct ocelot_vcap_filter *filter)
{
	const struct vcap_props *vcap = &ocelot->vcap[VCAP_ES0];
	struct ocelot_vcap_key_vlan *tag = &filter->vlan;
	struct ocelot_vcap_u64 payload;
	struct vcap_data data;
	int row = ix;
	u32 msk = 0x7;

	memset(&payload, 0, sizeof(payload));
	memset(&data, 0, sizeof(data));

	/* Read row */
	vcap_row_cmd(ocelot, vcap, row, VCAP_CMD_READ, VCAP_SEL_ALL);
	vcap_cache2entry(ocelot, vcap, &data);
	vcap_cache2action(ocelot, vcap, &data);

	data.tg_sw = VCAP_TG_FULL;
	data.type = ES0_ACTION_TYPE_NORMAL;
	vcap_data_offset_get(vcap, &data, ix);
	data.tg = (data.tg & ~data.tg_mask);
	if (filter->prio != 0)
		data.tg |= data.tg_value;

	vcap_key_set(vcap, &data, VCAP_ES0_IGR_PORT, filter->ingress_port, msk);
	vcap_key_set(vcap, &data, VCAP_ES0_EGR_PORT, filter->egress_port, msk);
	vcap_key_bit_set(vcap, &data, VCAP_ES0_L2_MC, filter->dmac_mc);
	vcap_key_bit_set(vcap, &data, VCAP_ES0_L2_BC, filter->dmac_bc);
	vcap_key_set(vcap, &data, VCAP_ES0_VID,
		     tag->vid.value, tag->vid.mask);
	vcap_key_set(vcap, &data, VCAP_ES0_PCP,
		     tag->pcp.value[0], tag->pcp.mask[0]);

	es0_action_set(ocelot, &data, filter);
	vcap_data_set(data.counter, data.counter_offset,
		      vcap->counter_width, filter->stats.pkts);

	/* Write row */
	vcap_entry2cache(ocelot, vcap, &data);
	vcap_action2cache(ocelot, vcap, &data);
	vcap_row_cmd(ocelot, vcap, row, VCAP_CMD_WRITE, VCAP_SEL_ALL);
}

static void vcap_entry_get(struct ocelot *ocelot,
			   struct ocelot_vcap_filter *rule, int ix)
{
	const struct vcap_props *vcap = &ocelot->vcap[rule->vcap_id];
	struct vcap_data data;
	int row, count;
	u32 cnt;

	if (rule->vcap_id == VCAP_ES0)
		data.tg_sw = VCAP_TG_FULL;
	else
		data.tg_sw = VCAP_TG_HALF;

	count = (1 << (data.tg_sw - 1));
	row = (ix / count);
	vcap_row_cmd(ocelot, vcap, row, VCAP_CMD_READ, VCAP_SEL_COUNTER);
	vcap_cache2action(ocelot, vcap, &data);
	vcap_data_offset_get(vcap, &data, ix);
	cnt = vcap_data_get(data.counter, data.counter_offset,
			       vcap->counter_width);

	rule->stats.pkts = cnt;
}

static int vcap_entry_set(struct ocelot *ocelot, int ix,
			   struct ocelot_vcap_filter *filter)
{
	int ret;

	switch (filter->vcap_id) {
	case VCAP_ES0:
		es0_entry_set(ocelot, ix, filter);
		break;
	case VCAP_IS1:
		ret = is1_entry_set(ocelot, ix, filter);
		if (ret)
			return ret;
		break;
	case VCAP_IS2:
		is2_entry_set(ocelot, ix, filter);
		break;
	default:
		break;
	}

	return 0;
}

static int ocelot_vcap_filter_add_to_block(struct ocelot *ocelot,
					   struct ocelot_vcap_block *block,
					   struct ocelot_vcap_filter *filter)
{
	struct ocelot_vcap_filter *tmp;
	struct list_head *pos, *n;
	int index;

	if (filter->vcap_id == VCAP_IS2 && filter->action.police_ena) {
		index = filter->action.pol_ix;
		if (index > OCELOT_POLICER_DISCARD)
			return -EINVAL;

		ocelot_vcap_policer_add(ocelot, index, &filter->action.pol);
	}

	block->count++;

	if (list_empty(&block->rules)) {
		list_add(&filter->list, &block->rules);
		return 0;
	}

	list_for_each_safe(pos, n, &block->rules) {
		tmp = list_entry(pos, struct ocelot_vcap_filter, list);
		if (filter->prio < tmp->prio)
			break;
	}
	list_add(&filter->list, pos->prev);

	return 0;
}

static int ocelot_vcap_block_get_filter_index(struct ocelot_vcap_block *block,
					      struct ocelot_vcap_filter *filter)
{
	struct ocelot_vcap_filter *tmp;
	int index = -1;

	list_for_each_entry(tmp, &block->rules, list) {
		++index;
		if (filter->id == tmp->id)
			return index;
	}
	return -ENOENT;
}

static struct ocelot_vcap_filter*
ocelot_vcap_block_find_filter_by_index(struct ocelot_vcap_block *block,
				       int index)
{
	struct ocelot_vcap_filter *tmp;
	int i = 0;

	list_for_each_entry(tmp, &block->rules, list) {
		if (i == index)
			return tmp;
		++i;
	}

	return NULL;
}

int ocelot_vcap_get_vcap_id(struct ocelot_vcap_block *block,
			    struct ocelot_vcap_filter *filter)
{
	struct ocelot_vcap_filter *tmp;
	int i;

	for (i = filter->vcap_id; i > VCAP_ES0; i--)
		list_for_each_entry(tmp, &block[i].rules, list)
			if (filter->id == tmp->id) {
				filter->vcap_id = i;
				return 0;
			}

	return -EINVAL;
}

struct ocelot_vcap_filter *
ocelot_vcap_block_find_filter_by_id(struct ocelot_vcap_block *block, int id)
{
	struct ocelot_vcap_filter *filter;

	list_for_each_entry(filter, &block->rules, list)
		if (filter->id == id)
			return filter;

	return NULL;
}

/* If @on=false, then SNAP, ARP, IP and OAM frames will not match on keys based
 * on destination and source MAC addresses, but only on higher-level protocol
 * information. The only frame types to match on keys containing MAC addresses
 * in this case are non-SNAP, non-ARP, non-IP and non-OAM frames.
 *
 * If @on=true, then the above frame types (SNAP, ARP, IP and OAM) will match
 * on MAC_ETYPE keys such as destination and source MAC on this ingress port.
 * However the setting has the side effect of making these frames not matching
 * on any _other_ keys than MAC_ETYPE ones.
 */
static void ocelot_match_all_as_mac_etype(struct ocelot *ocelot, int port,
					  int lookup, bool on)
{
	u32 val = 0;

	if (on)
		val = ANA_PORT_VCAP_S2_CFG_S2_SNAP_DIS(BIT(lookup)) |
		      ANA_PORT_VCAP_S2_CFG_S2_ARP_DIS(BIT(lookup)) |
		      ANA_PORT_VCAP_S2_CFG_S2_IP_TCPUDP_DIS(BIT(lookup)) |
		      ANA_PORT_VCAP_S2_CFG_S2_IP_OTHER_DIS(BIT(lookup)) |
		      ANA_PORT_VCAP_S2_CFG_S2_OAM_DIS(BIT(lookup));

	ocelot_rmw_gix(ocelot, val,
		       ANA_PORT_VCAP_S2_CFG_S2_SNAP_DIS(BIT(lookup)) |
		       ANA_PORT_VCAP_S2_CFG_S2_ARP_DIS(BIT(lookup)) |
		       ANA_PORT_VCAP_S2_CFG_S2_IP_TCPUDP_DIS(BIT(lookup)) |
		       ANA_PORT_VCAP_S2_CFG_S2_IP_OTHER_DIS(BIT(lookup)) |
		       ANA_PORT_VCAP_S2_CFG_S2_OAM_DIS(BIT(lookup)),
		       ANA_PORT_VCAP_S2_CFG, port);
}

static bool ocelot_vcap_is_problematic_mac_etype(struct ocelot_vcap_filter *filter)
{
	u16 proto, mask;

	if (filter->key_type != OCELOT_VCAP_KEY_ETYPE)
		return false;

	proto = ntohs(*(u16 *)filter->key.etype.etype.value);
	mask = ntohs(*(u16 *)filter->key.etype.etype.mask);

	/* ETH_P_ALL match, so all protocols below are included */
	if (mask == 0)
		return true;
	if (proto == ETH_P_ARP)
		return true;
	if (proto == ETH_P_IP)
		return true;
	if (proto == ETH_P_IPV6)
		return true;

	return false;
}

static bool ocelot_vcap_is_problematic_non_mac_etype(struct ocelot_vcap_filter *filter)
{
	if (filter->key_type == OCELOT_VCAP_KEY_SNAP)
		return true;
	if (filter->key_type == OCELOT_VCAP_KEY_ARP)
		return true;
	if (filter->key_type == OCELOT_VCAP_KEY_IPV4)
		return true;
	if (filter->key_type == OCELOT_VCAP_KEY_IPV6)
		return true;
	return false;
}

static bool
ocelot_exclusive_mac_etype_filter_rules(struct ocelot *ocelot,
					struct ocelot_vcap_filter *filter)
{
	struct ocelot_vcap_block *block = &ocelot->block[VCAP_IS2];
	struct ocelot_vcap_filter *tmp;
	unsigned long port;
	int i;

	if (filter->vcap_id != VCAP_IS2)
		return true;

	/* We only have the S2_IP_TCPUDP_DIS set of knobs for VCAP IS2 */
	if (filter->vcap_id != VCAP_IS2)
		return true;

	if (ocelot_vcap_is_problematic_mac_etype(filter)) {
		/* Search for any non-MAC_ETYPE rules on the port */
		for (i = 0; i < block->count; i++) {
			tmp = ocelot_vcap_block_find_filter_by_index(block, i);
			if (tmp->ingress_port_mask & filter->ingress_port_mask &&
			    tmp->lookup == filter->lookup &&
			    ocelot_vcap_is_problematic_non_mac_etype(tmp))
				return false;
		}

		for_each_set_bit(port, &filter->ingress_port_mask,
				 ocelot->num_phys_ports)
			ocelot_match_all_as_mac_etype(ocelot, port,
						      filter->lookup, true);
	} else if (ocelot_vcap_is_problematic_non_mac_etype(filter)) {
		/* Search for any MAC_ETYPE rules on the port */
		for (i = 0; i < block->count; i++) {
			tmp = ocelot_vcap_block_find_filter_by_index(block, i);
			if (tmp->ingress_port_mask & filter->ingress_port_mask &&
			    tmp->lookup == filter->lookup &&
			    ocelot_vcap_is_problematic_mac_etype(tmp))
				return false;
		}

		for_each_set_bit(port, &filter->ingress_port_mask,
				 ocelot->num_phys_ports)
			ocelot_match_all_as_mac_etype(ocelot, port,
						      filter->lookup, false);
	}

	return true;
}

int ocelot_vcap_filter_add(struct ocelot *ocelot,
			   struct ocelot_vcap_filter *filter,
			   struct netlink_ext_ack *extack)
{
	struct ocelot_vcap_block *block = &ocelot->block[filter->vcap_id];
	int i, index, ret;

	if (!ocelot_exclusive_mac_etype_filter_rules(ocelot, filter)) {
		NL_SET_ERR_MSG_MOD(extack,
				   "Cannot mix MAC_ETYPE with non-MAC_ETYPE rules, use the other IS2 lookup");
		return -EBUSY;
	}

	/* Add rule to the linked list */
	ret = ocelot_vcap_filter_add_to_block(ocelot, block, filter);
	if (ret)
		return ret;

	/* Get the index of the inserted rule */
	index = ocelot_vcap_block_get_filter_index(block, filter);

	/* Move down the rules to make place for the new filter */
	for (i = block->count - 1; i > index; i--) {
		struct ocelot_vcap_filter *tmp;

		tmp = ocelot_vcap_block_find_filter_by_index(block, i);
		vcap_entry_set(ocelot, i, tmp);
	}

	/* Now insert the new rule */
	return vcap_entry_set(ocelot, index, filter);
}

static void ocelot_vcap_block_remove_filter(struct ocelot *ocelot,
					    struct ocelot_vcap_block *block,
					    struct ocelot_vcap_filter *filter)
{
	struct ocelot_vcap_filter *tmp;
	struct list_head *pos, *q;
	int index;

	list_for_each_safe(pos, q, &block->rules) {
		tmp = list_entry(pos, struct ocelot_vcap_filter, list);
		if (tmp->id == filter->id) {
			if (tmp->action.police_ena) {
				index = tmp->action.pol_ix;
				ocelot_vcap_policer_del(ocelot, index);
			}
			list_del(pos);
			kfree(tmp);
		}
	}

	block->count--;
}

int ocelot_vcap_filter_del(struct ocelot *ocelot,
			   struct ocelot_vcap_filter *filter)
{
	struct ocelot_vcap_block *block = &ocelot->block[filter->vcap_id];
	struct ocelot_vcap_filter del_filter;
	int i, index;

	memset(&del_filter, 0, sizeof(del_filter));

	/* Gets index of the rule */
	index = ocelot_vcap_block_get_filter_index(block, filter);
	if (index < 0)
		return -ENOENT;

	/* Delete rule */
	ocelot_vcap_block_remove_filter(ocelot, block, filter);

	/* Move up all the blocks over the deleted rule */
	for (i = index; i < block->count; i++) {
		struct ocelot_vcap_filter *tmp;

		tmp = ocelot_vcap_block_find_filter_by_index(block, i);
		vcap_entry_set(ocelot, i, tmp);
	}

	/* Now delete the last rule, because it is duplicated */
	del_filter.vcap_id = filter->vcap_id;
	vcap_entry_set(ocelot, block->count, &del_filter);

	return 0;
}

int ocelot_vcap_filter_stats_update(struct ocelot *ocelot,
				    struct ocelot_vcap_filter *filter)
{
	struct ocelot_vcap_block *block = &ocelot->block[filter->vcap_id];
	struct ocelot_vcap_filter *tmp;
	int index;

	index = ocelot_vcap_block_get_filter_index(block, filter);
	if (index < 0)
		return -ENOENT;

	vcap_entry_get(ocelot, filter, index);

	/* After we get the result we need to clear the counters */
	tmp = ocelot_vcap_block_find_filter_by_index(block, index);
	tmp->stats.pkts = 0;
	vcap_entry_set(ocelot, index, tmp);

	return 0;
}

static void vcap_init(struct ocelot *ocelot, const struct vcap_props *vcap)
{
	struct vcap_data data;

	memset(&data, 0, sizeof(data));

	vcap_entry2cache(ocelot, vcap, &data);
	ocelot_target_write(ocelot, vcap->target, vcap->entry_count,
			    VCAP_CORE_MV_CFG);
	vcap_cmd(ocelot, vcap, 0, VCAP_CMD_INITIALIZE, VCAP_SEL_ENTRY);

	vcap_action2cache(ocelot, vcap, &data);
	ocelot_target_write(ocelot, vcap->target, vcap->action_count,
			    VCAP_CORE_MV_CFG);
	vcap_cmd(ocelot, vcap, 0, VCAP_CMD_INITIALIZE,
		 VCAP_SEL_ACTION | VCAP_SEL_COUNTER);
}

int ocelot_vcap_init(struct ocelot *ocelot)
{
	struct ocelot_vcap_block *block;

	vcap_init(ocelot, &ocelot->vcap[VCAP_ES0]);
	vcap_init(ocelot, &ocelot->vcap[VCAP_IS1]);
	vcap_init(ocelot, &ocelot->vcap[VCAP_IS2]);

	/* Create a policer that will drop the frames for the cpu.
	 * This policer will be used as action in the acl rules to drop
	 * frames.
	 */
	ocelot_write_gix(ocelot, 0x299, ANA_POL_MODE_CFG,
			 OCELOT_POLICER_DISCARD);
	ocelot_write_gix(ocelot, 0x1, ANA_POL_PIR_CFG,
			 OCELOT_POLICER_DISCARD);
	ocelot_write_gix(ocelot, 0x3fffff, ANA_POL_PIR_STATE,
			 OCELOT_POLICER_DISCARD);
	ocelot_write_gix(ocelot, 0x0, ANA_POL_CIR_CFG,
			 OCELOT_POLICER_DISCARD);
	ocelot_write_gix(ocelot, 0x3fffff, ANA_POL_CIR_STATE,
			 OCELOT_POLICER_DISCARD);

	block = &ocelot->block[VCAP_IS2];
	INIT_LIST_HEAD(&ocelot->block[VCAP_ES0].rules);
	INIT_LIST_HEAD(&ocelot->block[VCAP_IS1].rules);
	INIT_LIST_HEAD(&ocelot->block[VCAP_IS2].rules);
	INIT_LIST_HEAD(&ocelot->dummy_rules);

	return 0;
}