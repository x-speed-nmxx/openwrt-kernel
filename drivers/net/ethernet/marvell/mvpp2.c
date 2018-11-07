/*
 * Driver for Marvell PPv2 network controller for Armada 375 SoC.
 *
 * Copyright (C) 2014 Marvell
 *
 * Marcin Wojtas <mw@semihalf.com>
 *
 * This file is licensed under the terms of the GNU General Public
 * License version 2. This program is licensed "as is" without any
 * warranty of any kind, whether express or implied.
 */

#include <linux/acpi.h>
#include <linux/kernel.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/platform_device.h>
#include <linux/skbuff.h>
#include <linux/inetdevice.h>
#include <linux/mbus.h>
#include <linux/module.h>
#include <linux/mfd/syscon.h>
#include <linux/interrupt.h>
#include <linux/cpumask.h>
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/of_mdio.h>
#include <linux/of_net.h>
#include <linux/of_address.h>
#include <linux/of_device.h>
#include <linux/of_reserved_mem.h>
#include <linux/phy.h>
#include <linux/phylink.h>
#include <linux/phy/phy.h>
#include <linux/clk.h>
#include <linux/hrtimer.h>
#include <linux/ktime.h>
#include <linux/regmap.h>
#include <uapi/linux/ppp_defs.h>
#include <net/ip.h>
#include <net/ipv6.h>
#include <net/tso.h>
#include <net/busy_poll.h>

#ifndef CACHE_LINE_MASK
#define CACHE_LINE_MASK            (~(L1_CACHE_BYTES - 1))
#endif

/* Fifo Registers */
#define MVPP2_RX_DATA_FIFO_SIZE_REG(port)	(0x00 + 4 * (port))
#define MVPP2_RX_ATTR_FIFO_SIZE_REG(port)	(0x20 + 4 * (port))
#define MVPP2_RX_MIN_PKT_SIZE_REG		0x60
#define MVPP2_RX_FIFO_INIT_REG			0x64
#define MVPP22_TX_FIFO_THRESH_REG(port)		(0x8840 + 4 * (port))
#define MVPP22_TX_FIFO_SIZE_REG(port)		(0x8860 + 4 * (port))

/* RX DMA Top Registers */
#define MVPP2_RX_CTRL_REG(port)			(0x140 + 4 * (port))
#define     MVPP2_RX_LOW_LATENCY_PKT_SIZE(s)	(((s) & 0xfff) << 16)
#define     MVPP2_RX_USE_PSEUDO_FOR_CSUM_MASK	BIT(31)
#define MVPP2_POOL_BUF_SIZE_REG(pool)		(0x180 + 4 * (pool))
#define     MVPP2_POOL_BUF_SIZE_OFFSET		5
#define MVPP2_RXQ_CONFIG_REG(rxq)		(0x800 + 4 * (rxq))
#define     MVPP2_SNOOP_PKT_SIZE_MASK		0x1ff
#define     MVPP2_SNOOP_BUF_HDR_MASK		BIT(9)
#define     MVPP2_RXQ_POOL_SHORT_OFFS		20
#define     MVPP21_RXQ_POOL_SHORT_MASK		0x700000
#define     MVPP22_RXQ_POOL_SHORT_MASK		0xf00000
#define     MVPP2_RXQ_POOL_LONG_OFFS		24
#define     MVPP21_RXQ_POOL_LONG_MASK		0x7000000
#define     MVPP22_RXQ_POOL_LONG_MASK		0xf000000
#define     MVPP2_RXQ_PACKET_OFFSET_OFFS	28
#define     MVPP2_RXQ_PACKET_OFFSET_MASK	0x70000000
#define     MVPP2_RXQ_DISABLE_MASK		BIT(31)
#define MVPP2_RXQ_MAX_NUM			128

/* Top Registers */
#define MVPP2_MH_REG(port)			(0x5040 + 4 * (port))
#define MVPP2_DSA_EXTENDED			BIT(5)

/* Parser Registers */
#define MVPP2_PRS_INIT_LOOKUP_REG		0x1000
#define     MVPP2_PRS_PORT_LU_MAX		0xf
#define     MVPP2_PRS_PORT_LU_MASK(port)	(0xff << ((port) * 4))
#define     MVPP2_PRS_PORT_LU_VAL(port, val)	((val) << ((port) * 4))
#define MVPP2_PRS_INIT_OFFS_REG(port)		(0x1004 + ((port) & 4))
#define     MVPP2_PRS_INIT_OFF_MASK(port)	(0x3f << (((port) % 4) * 8))
#define     MVPP2_PRS_INIT_OFF_VAL(port, val)	((val) << (((port) % 4) * 8))
#define MVPP2_PRS_MAX_LOOP_REG(port)		(0x100c + ((port) & 4))
#define     MVPP2_PRS_MAX_LOOP_MASK(port)	(0xff << (((port) % 4) * 8))
#define     MVPP2_PRS_MAX_LOOP_VAL(port, val)	((val) << (((port) % 4) * 8))
#define MVPP2_PRS_TCAM_IDX_REG			0x1100
#define MVPP2_PRS_TCAM_DATA_REG(idx)		(0x1104 + (idx) * 4)
#define     MVPP2_PRS_TCAM_INV_MASK		BIT(31)
#define MVPP2_PRS_SRAM_IDX_REG			0x1200
#define MVPP2_PRS_SRAM_DATA_REG(idx)		(0x1204 + (idx) * 4)
#define MVPP2_PRS_TCAM_CTRL_REG			0x1230
#define     MVPP2_PRS_TCAM_EN_MASK		BIT(0)

/* RSS Registers */
#define MVPP22_RSS_IDX_REG			0x1500
#define    MVPP22_RSS_IDX_ENTRY_NUM_OFF		0
#define    MVPP22_RSS_IDX_TBL_NUM_OFF		8
#define    MVPP22_RSS_IDX_RXQ_NUM_OFF		16
#define MVPP22_RSS_RXQ2RSS_TBL_REG		0x1504
#define    MVPP22_RSS_RXQ2RSS_TBL_POINT_OFF	0
#define    MVPP22_RSS_RXQ2RSS_TBL_POINT_MASK	0x7
#define MVPP22_RSS_TBL_ENTRY_REG		0x1508
#define    MVPP22_RSS_TBL_ENTRY_OFF		0
#define    MVPP22_RSS_TBL_ENTRY_MASK		0xff
#define MVPP22_RSS_WIDTH_REG			0x150c
#define    MVPP22_RSS_WIDTH_OFF			0
#define    MVPP22_RSS_WIDTH_MASK		0xf

/* Classifier Registers */
#define MVPP2_CLS_MODE_REG			0x1800
#define     MVPP2_CLS_MODE_ACTIVE_MASK		BIT(0)
#define MVPP2_CLS_PORT_WAY_REG			0x1810
#define     MVPP2_CLS_PORT_WAY_MASK(port)	(1 << (port))
#define MVPP2_CLS_LKP_INDEX_REG			0x1814
#define     MVPP2_CLS_LKP_INDEX_WAY_OFFS	6
#define MVPP2_CLS_LKP_TBL_REG			0x1818
#define     MVPP2_CLS_LKP_TBL_RXQ_MASK		0xff
#define     MVPP2_CLS_LKP_TBL_LOOKUP_EN_MASK	BIT(25)
#define MVPP2_CLS_FLOW_INDEX_REG		0x1820
#define MVPP2_CLS_FLOW_TBL0_REG			0x1824
#define MVPP2_CLS_FLOW_TBL1_REG			0x1828
#define MVPP2_CLS_FLOW_TBL2_REG			0x182c
#define MVPP2_CLS_OVERSIZE_RXQ_LOW_REG(port)	(0x1980 + ((port) * 4))
#define     MVPP2_CLS_OVERSIZE_RXQ_LOW_BITS	3
#define     MVPP2_CLS_OVERSIZE_RXQ_LOW_MASK	0x7
#define MVPP2_CLS_SWFWD_P2HQ_REG(port)		(0x19b0 + ((port) * 4))
#define MVPP2_CLS_SWFWD_PCTRL_REG		0x19d0
#define     MVPP2_CLS_SWFWD_PCTRL_MASK(port)	(1 << (port))

/* Classifier C2 Engine Registers */
#define MVPP2_CLS2_TCAM_IDX_REG			0x1B00
#define MVPP2_CLS2_TCAM_DATA_REG(idx)		(0x1B10 + (idx) * 4)
#define MVPP2_CLS2_TCAM_INV_REG			0x1B24
#define MVPP2_CLS2_TCAM_INV_INVALID_OFF		31
#define MVPP2_CLS2_TCAM_INV_INVALID_MASK	BIT(31)
#define MVPP2_CLS2_ACT_DATA_REG			0x1B30
#define MVPP2_CLS2_ACT_DATA_TBL_ID_OFF		0
#define MVPP2_CLS2_ACT_DATA_TBL_ID_MASK		0x3F
#define MVPP2_CLS2_ACT_DATA_TBL_SEL_OFF		6
#define MVPP2_CLS2_ACT_DATA_TBL_SEL_MASK	BIT(6)
#define MVPP2_CLS2_ACT_DATA_TBL_PRI_DSCP_OFF	7
#define MVPP2_CLS2_ACT_DATA_TBL_PRI_DSCP_MASK	BIT(7)
#define MVPP2_CLS2_ACT_DATA_TBL_GEM_ID_OFF	8
#define MVPP2_CLS2_ACT_DATA_TBL_GEM_ID_MASK	BIT(8)
#define MVPP2_CLS2_ACT_DATA_TBL_LOW_Q_OFF	9
#define MVPP2_CLS2_ACT_DATA_TBL_LOW_Q_MASK	BIT(9)
#define MVPP2_CLS2_ACT_DATA_TBL_HIGH_Q_OFF	10
#define MVPP2_CLS2_ACT_DATA_TBL_HIGH_Q_MASK	BIT(10)
#define MVPP2_CLS2_ACT_DATA_TBL_COLOR_OFF	11
#define MVPP2_CLS2_ACT_DATA_TBL_COLOR_MASK	BIT(11)
#define MVPP2_CLS2_DSCP_PRI_INDEX_REG		0x1B40
#define MVPP2_CLS2_DSCP_PRI_INDEX_LINE_OFF	0
#define MVPP2_CLS2_DSCP_PRI_INDEX_LINE_BITS	6
#define MVPP2_CLS2_DSCP_PRI_INDEX_LINE_MASK	0x0000003f
#define MVPP2_CLS2_DSCP_PRI_INDEX_SEL_OFF	6
#define MVPP2_CLS2_DSCP_PRI_INDEX_SEL_MASK	BIT(6)
#define MVPP2_CLS2_DSCP_PRI_INDEX_TBL_ID_OFF	8
#define MVPP2_CLS2_DSCP_PRI_INDEX_TBL_ID_BITS	6
#define MVPP2_CLS2_DSCP_PRI_INDEX_TBL_ID_MASK	0x00003f00
#define MVPP2_CLS2_QOS_TBL_REG			0x1B44
#define MVPP2_CLS2_QOS_TBL_PRI_OFF		0
#define MVPP2_CLS2_QOS_TBL_PRI_BITS		3
#define MVPP2_CLS2_QOS_TBL_PRI_MASK		0x00000007
#define MVPP2_CLS2_QOS_TBL_DSCP_OFF		3
#define MVPP2_CLS2_QOS_TBL_DSCP_BITS		6
#define MVPP2_CLS2_QOS_TBL_DSCP_MASK		0x000001f8
#define MVPP2_CLS2_QOS_TBL_COLOR_OFF		9
#define MVPP2_CLS2_QOS_TBL_COLOR_BITS		3
#define MVPP2_CLS2_QOS_TBL_COLOR_MASK		0x00000e00
#define MVPP2_CLS2_QOS_TBL_GEMPORT_OFF		12
#define MVPP2_CLS2_QOS_TBL_GEMPORT_BITS		12
#define MVPP2_CLS2_QOS_TBL_GEMPORT_MASK		0x00fff000
#define MVPP2_CLS2_QOS_TBL_QUEUENUM_OFF		24
#define MVPP2_CLS2_QOS_TBL_QUEUENUM_BITS	8
#define MVPP2_CLS2_QOS_TBL_QUEUENUM_MASK	0xff000000
#define MVPP2_CLS2_HIT_CTR_REG			0x1B50
#define MVPP2_CLS2_HIT_CTR_OFF			0
#define MVPP2_CLS2_HIT_CTR_BITS			32
#define MVPP2_CLS2_HIT_CTR_MASK			0xffffffff
#define MVPP2_CLS2_HIT_CTR_CLR_REG		0x1B54
#define MVPP2_CLS2_HIT_CTR_CLR_CLR_OFF		0
#define MVPP2_CLS2_HIT_CTR_CLR_CLR_MASK		BIT(0)
#define MVPP2_CLS2_HIT_CTR_CLR_DONE_OFF		1
#define MVPP2_CLS2_HIT_CTR_CLR_DONE_MASK	BIT(1)
#define MVPP2_CLS2_ACT_REG			0x1B60
#define MVPP2_CLS2_ACT_COLOR_OFF		0
#define MVPP2_CLS2_ACT_COLOR_BITS		3
#define MVPP2_CLS2_ACT_COLOR_MASK		0x00000007
#define MVPP2_CLS2_ACT_PRI_OFF			3
#define MVPP2_CLS2_ACT_PRI_BITS			2
#define MVPP2_CLS2_ACT_PRI_MASK			0x00000018
#define MVPP2_CLS2_ACT_DSCP_OFF			5
#define MVPP2_CLS2_ACT_DSCP_BITS		2
#define MVPP2_CLS2_ACT_DSCP_MASK		0x00000060
#define MVPP2_CLS2_ACT_GEM_OFF			7
#define MVPP2_CLS2_ACT_GEM_BITS			2
#define MVPP2_CLS2_ACT_GEM_MASK			0x00000180
#define MVPP2_CLS2_ACT_QL_OFF			9
#define MVPP2_CLS2_ACT_QL_BITS			2
#define MVPP2_CLS2_ACT_QL_MASK			0x00000600
#define MVPP2_CLS2_ACT_QH_OFF			11
#define MVPP2_CLS2_ACT_QH_BITS			2
#define MVPP2_CLS2_ACT_QH_MASK			0x00001800
#define MVPP2_CLS2_ACT_FRWD_OFF			13
#define MVPP2_CLS2_ACT_FRWD_BITS		3
#define MVPP2_CLS2_ACT_FRWD_MASK		0x0000e000
#define MVPP2_CLS2_ACT_PLCR_OFF			16
#define MVPP2_CLS2_ACT_PLCR_BITS		2
#define MVPP2_CLS2_ACT_PLCR_MASK		0x00030000
#define MVPP2_CLS2_ACT_FLD_EN_OFF		18
#define MVPP2_CLS2_ACT_FLD_EN_BITS		1
#define MVPP2_CLS2_ACT_FLD_EN_MASK		0x00040000
#define MVPP2_CLS2_ACT_RSS_OFF			19
#define MVPP2_CLS2_ACT_RSS_BITS			2
#define MVPP2_CLS2_ACT_RSS_MASK			0x00180000
#define MVPP2_CLS2_ACT_QOS_ATTR_REG		0x1B64
#define MVPP2_CLS2_ACT_QOS_ATTR_PRI_OFF		0
#define MVPP2_CLS2_ACT_QOS_ATTR_PRI_BITS	3
#define MVPP2_CLS2_ACT_QOS_ATTR_PRI_MASK	0x00000007
#define MVPP2_CLS2_ACT_QOS_ATTR_PRI_MAX		\
				((1 << MVPP2_CLS2_ACT_QOS_ATTR_PRI_BITS) - 1)
#define MVPP2_CLS2_ACT_QOS_ATTR_DSCP_OFF	3
#define MVPP2_CLS2_ACT_QOS_ATTR_DSCP_BITS	6
#define MVPP2_CLS2_ACT_QOS_ATTR_DSCP_MASK	0x000001f8
#define MVPP2_CLS2_ACT_QOS_ATTR_DSCP_MAX	\
				((1 << MVPP2_CLS2_ACT_QOS_ATTR_DSCP_BITS) - 1)
#define MVPP2_CLS2_ACT_QOS_ATTR_GEM_OFF		9
#define MVPP2_CLS2_ACT_QOS_ATTR_GEM_BITS	12
#define MVPP2_CLS2_ACT_QOS_ATTR_GEM_MASK	0x001ffe00
#define MVPP2_CLS2_ACT_QOS_ATTR_GEM_MAX		\
				((1 << MVPP2_CLS2_ACT_QOS_ATTR_GEM_BITS) - 1)
#define MVPP2_CLS2_ACT_QOS_ATTR_QL_OFF		21
#define MVPP2_CLS2_ACT_QOS_ATTR_QL_BITS		3
#define MVPP2_CLS2_ACT_QOS_ATTR_QL_MASK		0x00e00000
#define MVPP2_CLS2_ACT_QOS_ATTR_QH_OFF		24
#define MVPP2_CLS2_ACT_QOS_ATTR_QH_BITS		5
#define MVPP2_CLS2_ACT_QOS_ATTR_QH_MASK		0x1f000000
#define MVPP2_CLS2_ACT_HWF_ATTR_REG		0x1B68
#define MVPP2_CLS2_ACT_HWF_ATTR_DPTR_OFF	1
#define MVPP2_CLS2_ACT_HWF_ATTR_DPTR_BITS	15
#define MVPP2_CLS2_ACT_HWF_ATTR_DPTR_MASK	0x0000fffe
#define MVPP2_CLS2_ACT_HWF_ATTR_DPTR_MAX	\
				((1 << MVPP2_CLS2_ACT_HWF_ATTR_DPTR_BITS) - 1)
#define MVPP2_CLS2_ACT_HWF_ATTR_IPTR_OFF	16
#define MVPP2_CLS2_ACT_HWF_ATTR_IPTR_BITS	8
#define MVPP2_CLS2_ACT_HWF_ATTR_IPTR_MASK	0x00ff0000
#define MVPP2_CLS2_ACT_HWF_ATTR_IPTR_MAX	\
				((1 << MVPP2_CLS2_ACT_HWF_ATTR_IPTR_BITS) - 1)
#define MVPP2_CLS2_ACT_HWF_ATTR_L4CHK_OFF	24
#define MVPP2_CLS2_ACT_HWF_ATTR_L4CHK_BITS	1
#define MVPP2_CLS2_ACT_HWF_ATTR_L4CHK_MASK	0x01000000
#define MVPP2_CLS2_ACT_HWF_ATTR_MTUIDX_OFF	25
#define MVPP2_CLS2_ACT_HWF_ATTR_MTUIDX_BITS	4
#define MVPP2_CLS2_ACT_HWF_ATTR_MTUIDX_MASK	0x1e000000
#define MVPP2_CLS2_ACT_DUP_ATTR_REG		0x1B6C
#define MVPP2_CLS2_ACT_DUP_ATTR_DUPID_OFF	0
#define MVPP2_CLS2_ACT_DUP_ATTR_DUPID_BITS	8
#define MVPP2_CLS2_ACT_DUP_ATTR_DUPID_MASK	0x000000ff
#define MVPP2_CLS2_ACT_DUP_ATTR_DUPCNT_OFF	8
#define MVPP2_CLS2_ACT_DUP_ATTR_DUPCNT_BITS	4
#define MVPP2_CLS2_ACT_DUP_ATTR_DUPCNT_MASK	0x00000f00
#define MVPP2_CLS2_ACT_DUP_ATTR_PLCRID_OFF	24
#define MVPP2_CLS2_ACT_DUP_ATTR_PLCRID_BITS	5
#define MVPP2_CLS2_ACT_DUP_ATTR_PLCRID_MASK	0x1f000000
#define MVPP2_CLS2_ACT_DUP_ATTR_PLCRBK_OFF	29
#define MVPP2_CLS2_ACT_DUP_ATTR_PLCRBK_BITS	1
#define MVPP2_CLS2_ACT_DUP_ATTR_PLCRBK_MASK	0x20000000
#define MVPP2_CLS2_ACT_DUP_ATTR_RSSEN_OFF	30
#define MVPP2_CLS2_ACT_DUP_ATTR_RSSEN_BITS	1
#define MVPP2_CLS2_ACT_DUP_ATTR_RSSEN_MASK	0x40000000
#define MVPP2_CLS2_TCAM_CFG0_REG		0x1b80
#define MVPP2_CLS2_TCAM_CFG0_EN_OFF		0
#define MVPP2_CLS2_TCAM_CFG0_EN_MASK		0x00000001
#define MVPP2_CLS2_TCAM_CFG0_SIZE_OFF		1
#define MVPP2_CLS2_TCAM_CFG0_SIZE_MASK		0x0000001e
#define MVPP2_CLS2_TCAM_CTRL_REG		0x1B90
#define MVPP2_CLS2_TCAM_CTRL_EN_OFF		0
#define MVPP2_CLS2_TCAM_CTRL_EN_MASK		0x0000001

/* Classifier C2 QOS Table (DSCP/PRI Table) */
#define MVPP2_QOS_TBL_LINE_NUM_PRI		8
#define MVPP2_QOS_TBL_NUM_PRI			64
#define MVPP2_QOS_TBL_LINE_NUM_DSCP		64
#define MVPP2_QOS_TBL_NUM_DSCP			8

/* Descriptor Manager Top Registers */
#define MVPP2_RXQ_NUM_REG			0x2040
#define MVPP2_RXQ_DESC_ADDR_REG			0x2044
#define     MVPP22_DESC_ADDR_OFFS		8
#define MVPP2_RXQ_DESC_SIZE_REG			0x2048
#define     MVPP2_RXQ_DESC_SIZE_MASK		0x3ff0
#define MVPP2_RXQ_STATUS_UPDATE_REG(rxq)	(0x3000 + 4 * (rxq))
#define     MVPP2_RXQ_NUM_PROCESSED_OFFSET	0
#define     MVPP2_RXQ_NUM_NEW_OFFSET		16
#define MVPP2_RXQ_STATUS_REG(rxq)		(0x3400 + 4 * (rxq))
#define     MVPP2_RXQ_OCCUPIED_MASK		0x3fff
#define     MVPP2_RXQ_NON_OCCUPIED_OFFSET	16
#define     MVPP2_RXQ_NON_OCCUPIED_MASK		0x3fff0000
#define MVPP2_RXQ_THRESH_REG			0x204c
#define     MVPP2_OCCUPIED_THRESH_OFFSET	0
#define     MVPP2_OCCUPIED_THRESH_MASK		0x3fff
#define MVPP2_RXQ_INDEX_REG			0x2050
#define MVPP2_TXQ_NUM_REG			0x2080
#define MVPP2_TXQ_DESC_ADDR_REG			0x2084
#define MVPP2_TXQ_DESC_SIZE_REG			0x2088
#define     MVPP2_TXQ_DESC_SIZE_MASK		0x3ff0
#define MVPP2_TXQ_THRESH_REG			0x2094
#define	    MVPP2_TXQ_THRESH_OFFSET		16
#define	    MVPP2_TXQ_THRESH_MASK		0x3fff
#define MVPP2_AGGR_TXQ_UPDATE_REG		0x2090
#define MVPP2_TXQ_INDEX_REG			0x2098
#define MVPP2_TXQ_PREF_BUF_REG			0x209c
#define     MVPP2_PREF_BUF_PTR(desc)		((desc) & 0xfff)
#define     MVPP2_PREF_BUF_SIZE_4		(BIT(12) | BIT(13))
#define     MVPP2_PREF_BUF_SIZE_16		(BIT(12) | BIT(14))
#define     MVPP2_PREF_BUF_THRESH(val)		((val) << 17)
#define     MVPP2_TXQ_DRAIN_EN_MASK		BIT(31)
#define MVPP2_TXQ_PENDING_REG			0x20a0
#define     MVPP2_TXQ_PENDING_MASK		0x3fff
#define MVPP2_TXQ_INT_STATUS_REG		0x20a4
#define MVPP2_TXQ_SENT_REG(txq)			(0x3c00 + 4 * (txq))
#define     MVPP2_TRANSMITTED_COUNT_OFFSET	16
#define     MVPP2_TRANSMITTED_COUNT_MASK	0x3fff0000
#define MVPP2_TXQ_RSVD_REQ_REG			0x20b0
#define     MVPP2_TXQ_RSVD_REQ_Q_OFFSET		16
#define MVPP2_TXQ_RSVD_RSLT_REG			0x20b4
#define     MVPP2_TXQ_RSVD_RSLT_MASK		0x3fff
#define MVPP2_TXQ_RSVD_CLR_REG			0x20b8
#define     MVPP2_TXQ_RSVD_CLR_OFFSET		16
#define MVPP2_AGGR_TXQ_DESC_ADDR_REG(cpu)	(0x2100 + 4 * (cpu))
#define     MVPP22_AGGR_TXQ_DESC_ADDR_OFFS	8
#define MVPP2_AGGR_TXQ_DESC_SIZE_REG(cpu)	(0x2140 + 4 * (cpu))
#define     MVPP2_AGGR_TXQ_DESC_SIZE_MASK	0x3ff0
#define MVPP2_AGGR_TXQ_STATUS_REG(cpu)		(0x2180 + 4 * (cpu))
#define     MVPP2_AGGR_TXQ_PENDING_MASK		0x3fff
#define MVPP2_AGGR_TXQ_INDEX_REG(cpu)		(0x21c0 + 4 * (cpu))

/* MBUS bridge registers */
#define MVPP2_WIN_BASE(w)			(0x4000 + ((w) << 2))
#define MVPP2_WIN_SIZE(w)			(0x4020 + ((w) << 2))
#define MVPP2_WIN_REMAP(w)			(0x4040 + ((w) << 2))
#define MVPP2_BASE_ADDR_ENABLE			0x4060

/* AXI Bridge Registers */
#define MVPP22_AXI_BM_WR_ATTR_REG		0x4100
#define MVPP22_AXI_BM_RD_ATTR_REG		0x4104
#define MVPP22_AXI_AGGRQ_DESCR_RD_ATTR_REG	0x4110
#define MVPP22_AXI_TXQ_DESCR_WR_ATTR_REG	0x4114
#define MVPP22_AXI_TXQ_DESCR_RD_ATTR_REG	0x4118
#define MVPP22_AXI_RXQ_DESCR_WR_ATTR_REG	0x411c
#define MVPP22_AXI_RX_DATA_WR_ATTR_REG		0x4120
#define MVPP22_AXI_TX_DATA_RD_ATTR_REG		0x4130
#define MVPP22_AXI_RD_NORMAL_CODE_REG		0x4150
#define MVPP22_AXI_RD_SNOOP_CODE_REG		0x4154
#define MVPP22_AXI_WR_NORMAL_CODE_REG		0x4160
#define MVPP22_AXI_WR_SNOOP_CODE_REG		0x4164

/* Values for AXI Bridge registers */
#define MVPP22_AXI_ATTR_CACHE_OFFS		0
#define MVPP22_AXI_ATTR_DOMAIN_OFFS		12

#define MVPP22_AXI_CODE_CACHE_OFFS		0
#define MVPP22_AXI_CODE_DOMAIN_OFFS		4

#define MVPP22_AXI_CODE_CACHE_NON_CACHE		0x3
#define MVPP22_AXI_CODE_CACHE_WR_CACHE		0x7
#define MVPP22_AXI_CODE_CACHE_RD_CACHE		0xb

#define MVPP22_AXI_CODE_DOMAIN_OUTER_DOM	2
#define MVPP22_AXI_CODE_DOMAIN_SYSTEM		3

/* Interrupt Cause and Mask registers */
#define MVPP2_ISR_TX_THRESHOLD_REG(port)	(0x5140 + 4 * (port))
#define     MVPP2_MAX_ISR_TX_THRESHOLD		0xfffff0

#define MVPP2_ISR_RX_THRESHOLD_REG(rxq)		(0x5200 + 4 * (rxq))
#define     MVPP2_MAX_ISR_RX_THRESHOLD		0xfffff0
#define MVPP21_ISR_RXQ_GROUP_REG(port)		(0x5400 + 4 * (port))

#define MVPP22_ISR_RXQ_GROUP_INDEX_REG		0x5400
#define MVPP22_ISR_RXQ_GROUP_INDEX_SUBGROUP_MASK 0xf
#define MVPP22_ISR_RXQ_GROUP_INDEX_GROUP_MASK	0x380
#define MVPP22_ISR_RXQ_GROUP_INDEX_GROUP_OFFSET	7

#define MVPP22_ISR_RXQ_GROUP_INDEX_SUBGROUP_MASK 0xf
#define MVPP22_ISR_RXQ_GROUP_INDEX_GROUP_MASK	0x380

#define MVPP22_ISR_RXQ_SUB_GROUP_CONFIG_REG	0x5404
#define MVPP22_ISR_RXQ_SUB_GROUP_STARTQ_MASK	0x1f
#define MVPP22_ISR_RXQ_SUB_GROUP_SIZE_MASK	0xf00
#define MVPP22_ISR_RXQ_SUB_GROUP_SIZE_OFFSET	8

#define MVPP2_ISR_ENABLE_REG(port)		(0x5420 + 4 * (port))
#define     MVPP2_ISR_ENABLE_INTERRUPT(mask)	((mask) & 0xffff)
#define     MVPP2_ISR_DISABLE_INTERRUPT(mask)	(((mask) << 16) & 0xffff0000)
#define MVPP2_ISR_RX_TX_CAUSE_REG(port)		(0x5480 + 4 * (port))
#define     MVPP21_CAUSE_RXQ_OCCUP_DESC_ALL_MASK	0xffff
#define     MVPP22_CAUSE_RXQ_OCCUP_DESC_ALL_MASK	0xff
#define     MVPP2_CAUSE_TXQ_OCCUP_DESC_ALL_MASK	0xff0000
#define     MVPP2_CAUSE_TXQ_OCCUP_DESC_ALL_OFFSET	16
#define     MVPP2_CAUSE_RX_FIFO_OVERRUN_MASK	BIT(24)
#define     MVPP2_CAUSE_FCS_ERR_MASK		BIT(25)
#define     MVPP2_CAUSE_TX_FIFO_UNDERRUN_MASK	BIT(26)
#define     MVPP2_CAUSE_TX_EXCEPTION_SUM_MASK	BIT(29)
#define     MVPP2_CAUSE_RX_EXCEPTION_SUM_MASK	BIT(30)
#define     MVPP2_CAUSE_MISC_SUM_MASK		BIT(31)
#define MVPP2_ISR_RX_TX_MASK_REG(port)		(0x54a0 + 4 * (port))
#define MVPP2_ISR_PON_RX_TX_MASK_REG		0x54bc
#define     MVPP2_PON_CAUSE_RXQ_OCCUP_DESC_ALL_MASK	0xffff
#define     MVPP2_PON_CAUSE_TXP_OCCUP_DESC_ALL_MASK	0x3fc00000
#define     MVPP2_PON_CAUSE_MISC_SUM_MASK		BIT(31)
#define MVPP2_ISR_MISC_CAUSE_REG		0x55b0

/* Buffer Manager registers */
#define MVPP2_BM_POOL_BASE_REG(pool)		(0x6000 + ((pool) * 4))
#define     MVPP2_BM_POOL_BASE_ADDR_MASK	0xfffff80
#define MVPP2_BM_POOL_SIZE_REG(pool)		(0x6040 + ((pool) * 4))
#define     MVPP2_BM_POOL_SIZE_MASK		0xfff0
#define MVPP2_BM_POOL_READ_PTR_REG(pool)	(0x6080 + ((pool) * 4))
#define     MVPP2_BM_POOL_GET_READ_PTR_MASK	0xfff0
#define MVPP2_BM_POOL_PTRS_NUM_REG(pool)	(0x60c0 + ((pool) * 4))
#define     MVPP2_BM_POOL_PTRS_NUM_MASK		0xfff0
#define MVPP2_BM_BPPI_READ_PTR_REG(pool)	(0x6100 + ((pool) * 4))
#define MVPP2_BM_BPPI_PTRS_NUM_REG(pool)	(0x6140 + ((pool) * 4))
#define     MVPP2_BM_BPPI_PTR_NUM_MASK		0x7ff
#define MVPP22_BM_POOL_PTRS_NUM_MASK		0xfff8
#define     MVPP2_BM_BPPI_PREFETCH_FULL_MASK	BIT(16)
#define MVPP2_BM_POOL_CTRL_REG(pool)		(0x6200 + ((pool) * 4))
#define     MVPP2_BM_START_MASK			BIT(0)
#define     MVPP2_BM_STOP_MASK			BIT(1)
#define     MVPP2_BM_STATE_MASK			BIT(4)
#define     MVPP2_BM_LOW_THRESH_OFFS		8
#define     MVPP2_BM_LOW_THRESH_MASK		0x7f00
#define     MVPP2_BM_LOW_THRESH_VALUE(val)	((val) << \
						MVPP2_BM_LOW_THRESH_OFFS)
#define     MVPP2_BM_HIGH_THRESH_OFFS		16
#define     MVPP2_BM_HIGH_THRESH_MASK		0x7f0000
#define     MVPP2_BM_HIGH_THRESH_VALUE(val)	((val) << \
						MVPP2_BM_HIGH_THRESH_OFFS)
#define MVPP2_BM_INTR_CAUSE_REG(pool)		(0x6240 + ((pool) * 4))
#define     MVPP2_BM_RELEASED_DELAY_MASK	BIT(0)
#define     MVPP2_BM_ALLOC_FAILED_MASK		BIT(1)
#define     MVPP2_BM_BPPE_EMPTY_MASK		BIT(2)
#define     MVPP2_BM_BPPE_FULL_MASK		BIT(3)
#define     MVPP2_BM_AVAILABLE_BP_LOW_MASK	BIT(4)
#define MVPP2_BM_INTR_MASK_REG(pool)		(0x6280 + ((pool) * 4))
#define MVPP2_BM_PHY_ALLOC_REG(pool)		(0x6400 + ((pool) * 4))
#define     MVPP2_BM_PHY_ALLOC_GRNTD_MASK	BIT(0)
#define MVPP2_BM_VIRT_ALLOC_REG			0x6440
#define MVPP22_BM_ADDR_HIGH_ALLOC		0x6444
#define     MVPP22_BM_ADDR_HIGH_PHYS_MASK	0xff
#define     MVPP22_BM_ADDR_HIGH_VIRT_MASK	0xff00
#define     MVPP22_BM_ADDR_HIGH_VIRT_SHIFT	8
#define MVPP2_BM_PHY_RLS_REG(pool)		(0x6480 + ((pool) * 4))
#define     MVPP2_BM_PHY_RLS_MC_BUFF_MASK	BIT(0)
#define     MVPP2_BM_PHY_RLS_PRIO_EN_MASK	BIT(1)
#define     MVPP2_BM_PHY_RLS_GRNTD_MASK		BIT(2)
#define MVPP2_BM_VIRT_RLS_REG			0x64c0
#define MVPP22_BM_ADDR_HIGH_RLS_REG		0x64c4
#define     MVPP22_BM_ADDR_HIGH_PHYS_RLS_MASK	0xff
#define     MVPP22_BM_ADDR_HIGH_VIRT_RLS_MASK	0xff00
#define     MVPP22_BM_ADDR_HIGH_VIRT_RLS_SHIFT	8

/* TX Scheduler registers */
#define MVPP2_TXP_SCHED_PORT_INDEX_REG		0x8000
#define MVPP2_TXP_SCHED_Q_CMD_REG		0x8004
#define     MVPP2_TXP_SCHED_ENQ_MASK		0xff
#define     MVPP2_TXP_SCHED_DISQ_OFFSET		8
#define MVPP2_TXP_SCHED_CMD_1_REG		0x8010
#define MVPP2_TXP_SCHED_FIXED_PRIO_REG		0x8014
#define MVPP2_TXP_SCHED_PERIOD_REG		0x8018
#define MVPP2_TXP_SCHED_MTU_REG			0x801c
#define     MVPP2_TXP_MTU_MAX			0x7FFFF
#define MVPP2_TXP_SCHED_REFILL_REG		0x8020
#define     MVPP2_TXP_REFILL_TOKENS_ALL_MASK	0x7ffff
#define     MVPP2_TXP_REFILL_PERIOD_ALL_MASK	0x3ff00000
#define     MVPP2_TXP_REFILL_PERIOD_MASK(v)	((v) << 20)
#define MVPP2_TXP_SCHED_TOKEN_SIZE_REG		0x8024
#define     MVPP2_TXP_TOKEN_SIZE_MAX		0xffffffff
#define MVPP2_TXQ_SCHED_REFILL_REG(q)		(0x8040 + ((q) << 2))
#define     MVPP2_TXQ_REFILL_TOKENS_ALL_MASK	0x7ffff
#define     MVPP2_TXQ_REFILL_PERIOD_ALL_MASK	0x3ff00000
#define     MVPP2_TXQ_REFILL_PERIOD_MASK(v)	((v) << 20)
#define MVPP2_TXQ_SCHED_TOKEN_SIZE_REG(q)	(0x8060 + ((q) << 2))
#define     MVPP2_TXQ_TOKEN_SIZE_MAX		0x7fffffff
#define MVPP2_TXQ_SCHED_TOKEN_CNTR_REG(q)	(0x8080 + ((q) << 2))
#define     MVPP2_TXQ_TOKEN_CNTR_MAX		0xffffffff

/* TX general registers */
#define MVPP2_TX_SNOOP_REG			0x8800
#define MVPP2_TX_PORT_FLUSH_REG			0x8810
#define     MVPP2_TX_PORT_FLUSH_MASK(port)	(1 << (port))

/* LMS registers */
#define MVPP2_SRC_ADDR_MIDDLE			0x24
#define MVPP2_SRC_ADDR_HIGH			0x28
#define MVPP2_PHY_AN_CFG0_REG			0x34
#define     MVPP2_PHY_AN_STOP_SMI0_MASK		BIT(7)
#define MVPP2_MNG_EXTENDED_GLOBAL_CTRL_REG	0x305c
#define     MVPP2_EXT_GLOBAL_CTRL_DEFAULT	0x27

/* Per-port registers */
#define MVPP2_GMAC_CTRL_0_REG			0x0
#define     MVPP2_GMAC_PORT_EN_MASK		BIT(0)
#define     MVPP2_GMAC_PORT_TYPE_MASK		BIT(1)
#define     MVPP2_GMAC_MAX_RX_SIZE_OFFS		2
#define     MVPP2_GMAC_MAX_RX_SIZE_MASK		0x7ffc
#define     MVPP2_GMAC_MIB_CNTR_EN_MASK		BIT(15)
#define MVPP2_GMAC_CTRL_1_REG			0x4
#define     MVPP2_GMAC_PERIODIC_XON_EN_MASK	BIT(1)
#define     MVPP2_GMAC_GMII_LB_EN_MASK		BIT(5)
#define     MVPP2_GMAC_PCS_LB_EN_BIT		6
#define     MVPP2_GMAC_PCS_LB_EN_MASK		BIT(6)
#define     MVPP2_GMAC_SA_LOW_OFFS		7
#define MVPP2_GMAC_CTRL_2_REG			0x8
#define     MVPP2_GMAC_INBAND_AN_MASK		BIT(0)
#define     MVPP2_GMAC_FLOW_CTRL_MASK		GENMASK(2, 1)
#define     MVPP2_GMAC_PCS_ENABLE_MASK		BIT(3)
#define     MVPP2_GMAC_INTERNAL_CLK_MASK	BIT(4)
#define     MVPP2_GMAC_DISABLE_PADDING		BIT(5)
#define     MVPP2_GMAC_PORT_RESET_MASK		BIT(6)
#define MVPP2_GMAC_AUTONEG_CONFIG		0xc
#define     MVPP2_GMAC_FORCE_LINK_DOWN		BIT(0)
#define     MVPP2_GMAC_FORCE_LINK_PASS		BIT(1)
#define     MVPP2_GMAC_IN_BAND_AUTONEG		BIT(2)
#define     MVPP2_GMAC_IN_BAND_AUTONEG_BYPASS	BIT(3)
#define     MVPP2_GMAC_IN_BAND_RESTART_AN	BIT(4)
#define     MVPP2_GMAC_CONFIG_MII_SPEED	BIT(5)
#define     MVPP2_GMAC_CONFIG_GMII_SPEED	BIT(6)
#define     MVPP2_GMAC_AN_SPEED_EN		BIT(7)
#define     MVPP2_GMAC_FC_ADV_EN		BIT(9)
#define     MVPP2_GMAC_FC_ADV_ASM_EN		BIT(10)
#define     MVPP2_GMAC_FLOW_CTRL_AUTONEG	BIT(11)
#define     MVPP2_GMAC_CONFIG_FULL_DUPLEX	BIT(12)
#define     MVPP2_GMAC_AN_DUPLEX_EN		BIT(13)
#define MVPP2_GMAC_STATUS0			0x10
#define     MVPP2_GMAC_STATUS0_LINK_UP		BIT(0)
#define     MVPP2_GMAC_STATUS0_GMII_SPEED	BIT(1)
#define     MVPP2_GMAC_STATUS0_MII_SPEED	BIT(2)
#define     MVPP2_GMAC_STATUS0_FULL_DUPLEX	BIT(3)
#define     MVPP2_GMAC_STATUS0_RX_PAUSE		BIT(6)
#define     MVPP2_GMAC_STATUS0_TX_PAUSE		BIT(7)
#define     MVPP2_GMAC_STATUS0_AN_COMPLETE	BIT(11)
#define MVPP2_GMAC_PORT_FIFO_CFG_1_REG		0x1c
#define     MVPP2_GMAC_TX_FIFO_MIN_TH_OFFS	6
#define     MVPP2_GMAC_TX_FIFO_MIN_TH_ALL_MASK	0x1fc0
#define     MVPP2_GMAC_TX_FIFO_MIN_TH_MASK(v)	(((v) << 6) & \
					MVPP2_GMAC_TX_FIFO_MIN_TH_ALL_MASK)
#define MVPP22_GMAC_INT_STAT			0x20
#define     MVPP22_GMAC_INT_STAT_LINK		BIT(1)
#define MVPP22_GMAC_INT_MASK			0x24
#define     MVPP22_GMAC_INT_MASK_LINK_STAT	BIT(1)
#define MVPP22_GMAC_CTRL_4_REG			0x90
#define     MVPP22_CTRL4_EXT_PIN_GMII_SEL	BIT(0)
#define     MVPP22_CTRL4_RX_FC_EN		BIT(3)
#define     MVPP22_CTRL4_TX_FC_EN		BIT(4)
#define     MVPP22_CTRL4_DP_CLK_SEL		BIT(5)
#define     MVPP22_CTRL4_SYNC_BYPASS_DIS	BIT(6)
#define     MVPP22_CTRL4_QSGMII_BYPASS_ACTIVE	BIT(7)
#define MVPP22_GMAC_INT_SUM_MASK		0xa4
#define     MVPP22_GMAC_INT_SUM_MASK_LINK_STAT	BIT(1)

/* Per-port XGMAC registers. PPv2.2 only, only for GOP port 0,
 * relative to port->base.
 */
#define MVPP22_XLG_CTRL0_REG			0x100
#define     MVPP22_XLG_CTRL0_PORT_EN		BIT(0)
#define     MVPP22_XLG_CTRL0_MAC_RESET_DIS	BIT(1)
#define     MVPP22_XLG_CTRL0_RX_FLOW_CTRL_EN	BIT(7)
#define     MVPP22_XLG_CTRL0_TX_FLOW_CTRL_EN	BIT(8)
#define     MVPP22_XLG_CTRL0_MIB_CNT_DIS	BIT(14)
#define MVPP22_XLG_CTRL1_REG			0x104
#define     MVPP22_XLG_CTRL1_FRAMESIZELIMIT_OFFS	0
#define     MVPP22_XLG_CTRL1_FRAMESIZELIMIT_MASK	0x1fff
#define MVPP22_XLG_STATUS			0x10c
#define     MVPP22_XLG_STATUS_LINK_UP		BIT(0)
#define MVPP22_XLG_INT_STAT			0x114
#define     MVPP22_XLG_INT_STAT_LINK		BIT(1)
#define MVPP22_XLG_INT_MASK			0x118
#define     MVPP22_XLG_INT_MASK_LINK		BIT(1)
#define MVPP22_XLG_CTRL3_REG			0x11c
#define     MVPP22_XLG_CTRL3_MACMODESELECT_MASK	(7 << 13)
#define     MVPP22_XLG_CTRL3_MACMODESELECT_GMAC	(0 << 13)
#define     MVPP22_XLG_CTRL3_MACMODESELECT_10G	BIT(13)
#define MVPP22_XLG_EXT_INT_MASK			0x15c
#define     MVPP22_XLG_EXT_INT_MASK_XLG		BIT(1)
#define     MVPP22_XLG_EXT_INT_MASK_GIG		BIT(2)
#define MVPP22_XLG_CTRL4_REG			0x184
#define     MVPP22_XLG_CTRL4_FWD_FC		BIT(5)
#define     MVPP22_XLG_CTRL4_FWD_PFC		BIT(6)
#define     MVPP22_XLG_CTRL4_MACMODSELECT_GMAC	BIT(12)
/* Check of 3 consecutive IDLEs for XLG link up indication */
#define     MVPP22_XLG_CTRL4_EN_IDLE_CHECK	BIT(14)

/* SMI registers. PPv2.2 only, relative to priv->iface_base. */
#define MVPP22_SMI_MISC_CFG_REG			0x1204
#define     MVPP22_SMI_POLLING_EN		BIT(10)

#define MVPP22_GMAC_BASE(port)		(0x7000 + (port) * 0x1000 + 0xe00)

#define MVPP2_CAUSE_TXQ_SENT_DESC_ALL_MASK	0xff

/* Descriptor ring Macros */
#define MVPP2_QUEUE_NEXT_DESC(q, index) \
	(((index) < (q)->last_desc) ? ((index) + 1) : 0)

/* XPCS registers. PPv2.2 only */
#define MVPP22_MPCS_BASE(port)			(0x7000 + (port) * 0x1000)
#define MVPP22_MPCS_CTRL			0x14
#define     MVPP22_MPCS_CTRL_FWD_ERR_CONN	BIT(10)
#define MVPP22_MPCS_CLK_RESET			0x14c
#define     MAC_CLK_RESET_SD_TX			BIT(0)
#define     MAC_CLK_RESET_SD_RX			BIT(1)
#define     MAC_CLK_RESET_MAC			BIT(2)
#define     MVPP22_MPCS_CLK_RESET_DIV_RATIO(n)	((n) << 4)
#define     MVPP22_MPCS_CLK_RESET_DIV_SET	BIT(11)

/* XPCS registers. PPv2.2 only */
#define MVPP22_XPCS_BASE(port)			(0x7400 + (port) * 0x1000)
#define MVPP22_XPCS_CFG0			0x0
#define     MVPP22_XPCS_CFG0_PCS_MODE(n)	((n) << 3)
#define     MVPP22_XPCS_CFG0_ACTIVE_LANE(n)	((n) << 5)

/* System controller registers. Accessed through a regmap. */
#define GENCONF_SOFT_RESET1				0x1108
#define     GENCONF_SOFT_RESET1_GOP			BIT(6)
#define GENCONF_PORT_CTRL0				0x1110
#define     GENCONF_PORT_CTRL0_BUS_WIDTH_SELECT		BIT(1)
#define     GENCONF_PORT_CTRL0_RX_DATA_SAMPLE		BIT(29)
#define     GENCONF_PORT_CTRL0_CLK_DIV_PHASE_CLR	BIT(31)
#define GENCONF_PORT_CTRL1				0x1114
#define     GENCONF_PORT_CTRL1_EN(p)			BIT(p)
#define     GENCONF_PORT_CTRL1_RESET(p)			(BIT(p) << 28)
#define GENCONF_CTRL0					0x1120
#define     GENCONF_CTRL0_PORT0_RGMII			BIT(0)
#define     GENCONF_CTRL0_PORT1_RGMII_MII		BIT(1)
#define     GENCONF_CTRL0_PORT1_RGMII			BIT(2)

/* Various constants */

/* Coalescing */
#define MVPP2_TXDONE_COAL_PKTS_THRESH	32
#define MVPP2_TXDONE_HRTIMER_PERIOD_NS	1000000UL
#define MVPP2_TXDONE_COAL_USEC		1000
#define MVPP2_RX_COAL_PKTS		32
#define MVPP2_RX_COAL_USEC		64
#define MVPP2_TX_BULK_TIME		(50 * NSEC_PER_USEC)

/* The two bytes Marvell header. Either contains a special value used
 * by Marvell switches when a specific hardware mode is enabled (not
 * supported by this driver) or is filled automatically by zeroes on
 * the RX side. Those two bytes being at the front of the Ethernet
 * header, they allow to have the IP header aligned on a 4 bytes
 * boundary automatically: the hardware skips those two bytes on its
 * own.
 */
#define MVPP2_MH_SIZE			2
#define MVPP2_ETH_TYPE_LEN		2
#define MVPP2_PPPOE_HDR_SIZE		8
#define MVPP2_VLAN_TAG_LEN		4
#define MVPP2_VLAN_TAG_EDSA_LEN		8

/* Lbtd 802.3 type */
#define MVPP2_IP_LBDT_TYPE		0xfffa

#define MVPP2_TX_CSUM_MAX_SIZE		9800

/* Timeout constants */
#define MVPP2_TX_DISABLE_TIMEOUT_MSEC	1000
#define MVPP2_TX_PENDING_TIMEOUT_MSEC	1000

#define MVPP2_TX_MTU_MAX		0x7ffff

/* Maximum number of T-CONTs of PON port */
#define MVPP2_MAX_TCONT			16

/* Maximum number of supported ports */
#define MVPP2_MAX_PORTS			4

/* Maximum number of TXQs used by single port */
#define MVPP2_MAX_TXQ			8

/* SKB/TSO/TX-ring-size/pause-wakeup constatnts depend upon the
 *  MAX_TSO_SEGS - the max number of fragments to allow in the GSO skb.
 *  Min-Min requirement for it = maxPacket(64kB)/stdMTU(1500)=44 fragments
 *  and MVPP2_MAX_TSO_SEGS=max(MVPP2_MAX_TSO_SEGS, MAX_SKB_FRAGS).
 * MAX_SKB_DESCS: we need 2 descriptors per TSO fragment (1 header, 1 data)
 *  + per-cpu-reservation MVPP2_CPU_DESC_CHUNK*CPUs for optimization.
 * TX stop activation threshold (e.g. Queue is full) is MAX_SKB_DESCS
 * TX stop-to-wake hysteresis is MAX_TSO_SEGS
 * The Tx ring size cannot be smaller than TSO_SEGS + HYSTERESIS + SKBs
 */
#define MVPP2_MAX_TSO_SEGS		44
#define MVPP2_MAX_SKB_DESCS(hifs)	(MVPP2_MAX_TSO_SEGS * 2 + \
					MVPP2_CPU_DESC_CHUNK * hifs)
#define MVPP2_TX_PAUSE_HYSTERESIS	MVPP2_MAX_TSO_SEGS

/* Dfault number of RXQs in use */
#define MVPP2_DEFAULT_RXQ		4

/* Max number of Rx descriptors */
#define MVPP2_MAX_RXD_MAX		1280
#define MVPP2_MAX_RXD_DFLT		MVPP2_MAX_RXD_MAX

/* Max number of Tx descriptors */
#define MVPP2_MAX_TXD_MAX		2048
#define MVPP2_MAX_TXD_DFLT		1024
#define MVPP2_MIN_TXD(hifs)	ALIGN(MVPP2_MAX_TSO_SEGS + \
				      MVPP2_MAX_SKB_DESCS(hifs) + \
				      MVPP2_TX_PAUSE_HYSTERESIS, 32)

/* Amount of Tx descriptors that can be reserved at once by CPU */
#define MVPP2_CPU_DESC_CHUNK		64

/* Max number of Tx descriptors in each aggregated queue */
#define MVPP2_AGGR_TXQ_SIZE		256

/* Descriptor aligned size */
#define MVPP2_DESC_ALIGNED_SIZE		32

/* Descriptor alignment mask */
#define MVPP2_TX_DESC_ALIGN		(MVPP2_DESC_ALIGNED_SIZE - 1)

/* RX FIFO constants */
#define MVPP2_RX_FIFO_PORT_DATA_SIZE_32KB	0x8000
#define MVPP2_RX_FIFO_PORT_DATA_SIZE_8KB	0x2000
#define MVPP2_RX_FIFO_PORT_DATA_SIZE_4KB	0x1000
#define MVPP2_RX_FIFO_PORT_ATTR_SIZE_32KB	0x200
#define MVPP2_RX_FIFO_PORT_ATTR_SIZE_8KB	0x80
#define MVPP2_RX_FIFO_PORT_ATTR_SIZE_4KB	0x40
#define MVPP2_RX_FIFO_PORT_MIN_PKT		0x80

/* TX FIFO constants */
#define MVPP22_TX_FIFO_DATA_SIZE_10KB		0xa
#define MVPP22_TX_FIFO_DATA_SIZE_3KB		0x3
#define MVPP2_TX_FIFO_THRESHOLD_MIN		256
#define MVPP2_TX_FIFO_THRESHOLD_10KB	\
	(MVPP22_TX_FIFO_DATA_SIZE_10KB * 1024 - MVPP2_TX_FIFO_THRESHOLD_MIN)
#define MVPP2_TX_FIFO_THRESHOLD_3KB	\
	(MVPP22_TX_FIFO_DATA_SIZE_3KB * 1024 - MVPP2_TX_FIFO_THRESHOLD_MIN)

/* RX buffer constants */
#define MVPP2_SKB_SHINFO_SIZE \
	SKB_DATA_ALIGN(sizeof(struct skb_shared_info))

#define MVPP2_RX_PKT_SIZE(mtu) \
	ALIGN((mtu) + MVPP2_MH_SIZE + MVPP2_VLAN_TAG_LEN + \
	      ETH_HLEN + ETH_FCS_LEN, cache_line_size())

#define MVPP2_RX_BUF_SIZE(pkt_size)	((pkt_size) + NET_SKB_PAD)
#define MVPP2_RX_TOTAL_SIZE(buf_size)	((buf_size) + MVPP2_SKB_SHINFO_SIZE)
#define MVPP2_RX_MAX_PKT_SIZE(total_size) \
	((total_size) - NET_SKB_PAD - MVPP2_SKB_SHINFO_SIZE)

#define MVPP2_BIT_TO_BYTE(bit)		((bit) / 8)

/* IPv6 max L3 address size */
#define MVPP2_MAX_L3_ADDR_SIZE		16

/* Port flags */
#define MVPP2_F_LOOPBACK		BIT(0)
#define MVPP22_F_IF_MUSDK		BIT(2) /* musdk port */
#define MVPP2_F_IF_TX_ON		BIT(3)

/* Marvell tag types */
enum mvpp2_tag_type {
	MVPP2_TAG_TYPE_NONE = 0,
	MVPP2_TAG_TYPE_MH   = 1,
	MVPP2_TAG_TYPE_DSA  = 2,
	MVPP2_TAG_TYPE_EDSA = 3,
	MVPP2_TAG_TYPE_VLAN = 4,
	MVPP2_TAG_TYPE_LAST = 5
};

/* Parser constants */
#define MVPP2_PRS_TCAM_SRAM_SIZE	256
#define MVPP2_PRS_TCAM_WORDS		6
#define MVPP2_PRS_SRAM_WORDS		4
#define MVPP2_PRS_FLOW_ID_SIZE		64
#define MVPP2_PRS_FLOW_ID_MASK		0x3f
#define MVPP2_PRS_TCAM_ENTRY_INVALID	1
#define MVPP2_PRS_TCAM_DSA_TAGGED_BIT	BIT(5)
#define MVPP2_PRS_IPV4_HEAD		0x40
#define MVPP2_PRS_IPV4_HEAD_MASK	0xf0
#define MVPP2_PRS_IPV4_MC		0xe0
#define MVPP2_PRS_IPV4_MC_MASK		0xf0
#define MVPP2_PRS_IPV4_BC_MASK		0xff
#define MVPP2_PRS_IPV4_IHL		0x5
#define MVPP2_PRS_IPV4_IHL_MASK		0xf
#define MVPP2_PRS_IPV6_MC		0xff
#define MVPP2_PRS_IPV6_MC_MASK		0xff
#define MVPP2_PRS_IPV6_HOP_MASK		0xff
#define MVPP2_PRS_TCAM_PROTO_MASK	0xff
#define MVPP2_PRS_TCAM_PROTO_MASK_L	0x3f
#define MVPP2_PRS_DBL_VLANS_MAX		100
#define MVPP2_PRS_CAST_MASK		BIT(0)
#define MVPP2_PRS_MCAST_VAL		BIT(0)
#define MVPP2_PRS_UCAST_VAL		0x0

/* Tcam structure:
 * - lookup ID - 4 bits
 * - port ID - 1 byte
 * - additional information - 1 byte
 * - header data - 8 bytes
 * The fields are represented by MVPP2_PRS_TCAM_DATA_REG(5)->(0).
 */
#define MVPP2_PRS_AI_BITS			8
#define MVPP2_PRS_PORT_MASK			0xff
#define MVPP2_PRS_LU_MASK			0xf
#define MVPP2_PRS_TCAM_AI_BYTE			16
#define MVPP2_PRS_TCAM_PORT_BYTE		17
#define MVPP2_PRS_TCAM_LU_BYTE			20
#define MVPP2_PRS_TCAM_EN_OFFS(offs)		((offs) + 2)
#define MVPP2_PRS_TCAM_INV_WORD			5
#define MVPP2_PRS_VID_TCAM_BYTE			2

/* PRS TCAM and SRAM have HW-entry 160 bit
 * TCAM configurator uses 4byte-word with 2 bytes of data and 2 bytes of mask.
 * Little/Big Endian (LE/BE) word operations need converter for data-byte-array
 * into offset for 4 bytes register having LE "native" ordering.
 * MACROs have ambiguty so use inline procedures
 */
static inline int mvpp2_offs_endian(const int offs)
{
#if defined(__BIG_ENDIAN)
	/* Byte-offset to access u8/u32 Register space:
	 *    Input  native/LE: 0  1  2  3
	 *    Output BE offset: 3  2  1  0
	 */
	return ((offs & ~0x3) + (3 - (offs % 4)));
#else
	return offs;
#endif
}

static inline int tcam_data_byte_offs_le(int offs)
			{ return ((offs - (offs % 2)) * 2 + (offs % 2)); }
static inline int tcam_data_mask_offs_le(int offs)
			{ return ((offs * 2) - (offs % 2) + 2); }

#define MVPP2_PRS_TCAM_DATA_BYTE(offs)		\
			(mvpp2_offs_endian(tcam_data_byte_offs_le(offs)))
#define MVPP2_PRS_TCAM_DATA_BYTE_EN(offs)	\
			(mvpp2_offs_endian(tcam_data_mask_offs_le(offs)))

#define MVPP2_SRAM_BIT_TO_BYTE(_bit_)	mvpp2_offs_endian((_bit_) / 8)


/* TCAM range for unicast and multicast filtering. We have 25 entries per port,
 * with 4 dedicated to UC filtering and the rest to multicast filtering.
 * Additionnally we reserve one entry for the broadcast address, and one for
 * each port's own address.
 */
#define MVPP2_PRS_MAC_UC_MC_FILT_MAX	25
#define MVPP2_PRS_MAC_RANGE_SIZE	80

/* Number of entries per port dedicated to UC and MC filtering */
#define MVPP2_PRS_MAC_UC_FILT_MAX	4
#define MVPP2_PRS_MAC_MC_FILT_MAX	(MVPP2_PRS_MAC_UC_MC_FILT_MAX - \
					 MVPP2_PRS_MAC_UC_FILT_MAX)

/* There is a TCAM range reserved for VLAN filtering entries, range size is 33
 * 10 VLAN ID filter entries per port
 * 1 default VLAN filter entry per port
 * It is assumed that there are 3 ports for filter, not including loopback port
 */
#define MVPP2_PRS_VLAN_FILT_MAX		11
#define MVPP2_PRS_VLAN_FILT_RANGE_SIZE	33

#define MVPP2_PRS_VLAN_FILT_MAX_ENTRY   (MVPP2_PRS_VLAN_FILT_MAX - 2)
#define MVPP2_PRS_VLAN_FILT_DFLT_ENTRY  (MVPP2_PRS_VLAN_FILT_MAX - 1)

/* Tcam entries ID */
#define MVPP2_PE_DROP_ALL		0
#define MVPP2_PE_FIRST_FREE_TID		1

/* MAC filtering range */
#define MVPP2_PE_MAC_RANGE_END		(MVPP2_PE_VID_FILT_RANGE_START - 1)
#define MVPP2_PE_MAC_RANGE_START	(MVPP2_PE_MAC_RANGE_END - \
						MVPP2_PRS_MAC_RANGE_SIZE + 1)
/* VLAN filtering range */
#define MVPP2_PE_VID_FILT_RANGE_END     (MVPP2_PRS_TCAM_SRAM_SIZE - 31)
#define MVPP2_PE_VID_FILT_RANGE_START   (MVPP2_PE_VID_FILT_RANGE_END - \
					 MVPP2_PRS_VLAN_FILT_RANGE_SIZE + 1)
#define MVPP2_PE_LAST_FREE_TID          (MVPP2_PE_MAC_RANGE_START - 1)
#define MVPP2_PE_IP6_EXT_PROTO_UN	(MVPP2_PRS_TCAM_SRAM_SIZE - 30)
#define MVPP2_PE_IP6_ADDR_UN		(MVPP2_PRS_TCAM_SRAM_SIZE - 29)
#define MVPP2_PE_IP4_ADDR_UN		(MVPP2_PRS_TCAM_SRAM_SIZE - 28)
#define MVPP2_PE_LAST_DEFAULT_FLOW	(MVPP2_PRS_TCAM_SRAM_SIZE - 27)
#define MVPP2_PE_FIRST_DEFAULT_FLOW	(MVPP2_PRS_TCAM_SRAM_SIZE - 22)
#define MVPP2_PE_EDSA_TAGGED		(MVPP2_PRS_TCAM_SRAM_SIZE - 21)
#define MVPP2_PE_EDSA_UNTAGGED		(MVPP2_PRS_TCAM_SRAM_SIZE - 20)
#define MVPP2_PE_DSA_TAGGED		(MVPP2_PRS_TCAM_SRAM_SIZE - 19)
#define MVPP2_PE_DSA_UNTAGGED		(MVPP2_PRS_TCAM_SRAM_SIZE - 18)
#define MVPP2_PE_ETYPE_EDSA_TAGGED	(MVPP2_PRS_TCAM_SRAM_SIZE - 17)
#define MVPP2_PE_ETYPE_EDSA_UNTAGGED	(MVPP2_PRS_TCAM_SRAM_SIZE - 16)
#define MVPP2_PE_ETYPE_DSA_TAGGED	(MVPP2_PRS_TCAM_SRAM_SIZE - 15)
#define MVPP2_PE_ETYPE_DSA_UNTAGGED	(MVPP2_PRS_TCAM_SRAM_SIZE - 14)
#define MVPP2_PE_MH_DEFAULT		(MVPP2_PRS_TCAM_SRAM_SIZE - 13)
#define MVPP2_PE_DSA_DEFAULT		(MVPP2_PRS_TCAM_SRAM_SIZE - 12)
#define MVPP2_PE_IP6_PROTO_UN		(MVPP2_PRS_TCAM_SRAM_SIZE - 11)
#define MVPP2_PE_IP4_PROTO_UN		(MVPP2_PRS_TCAM_SRAM_SIZE - 10)
#define MVPP2_PE_ETH_TYPE_UN		(MVPP2_PRS_TCAM_SRAM_SIZE - 9)
#define MVPP2_PE_VID_FLTR_DEFAULT	(MVPP2_PRS_TCAM_SRAM_SIZE - 8)
#define MVPP2_PE_VID_EDSA_FLTR_DEFAULT	(MVPP2_PRS_TCAM_SRAM_SIZE - 7)
#define MVPP2_PE_VLAN_DBL		(MVPP2_PRS_TCAM_SRAM_SIZE - 6)
#define MVPP2_PE_VLAN_NONE		(MVPP2_PRS_TCAM_SRAM_SIZE - 5)
/* reserved */
#define MVPP2_PE_MAC_MC_PROMISCUOUS	(MVPP2_PRS_TCAM_SRAM_SIZE - 3)
#define MVPP2_PE_MAC_UC_PROMISCUOUS	(MVPP2_PRS_TCAM_SRAM_SIZE - 2)
#define MVPP2_PE_MAC_NON_PROMISCUOUS	(MVPP2_PRS_TCAM_SRAM_SIZE - 1)

#define MVPP2_PRS_VID_PORT_FIRST(port)	(MVPP2_PE_VID_FILT_RANGE_START + \
					 ((port) * MVPP2_PRS_VLAN_FILT_MAX))
#define MVPP2_PRS_VID_PORT_LAST(port)	(MVPP2_PRS_VID_PORT_FIRST(port) \
					 + MVPP2_PRS_VLAN_FILT_MAX_ENTRY)
/* Index of default vid filter for given port */
#define MVPP2_PRS_VID_PORT_DFLT(port)	(MVPP2_PRS_VID_PORT_FIRST(port) \
					 + MVPP2_PRS_VLAN_FILT_DFLT_ENTRY)

/* Sram structure
 * The fields are represented by MVPP2_PRS_TCAM_DATA_REG(3)->(0).
 */
#define MVPP2_PRS_SRAM_RI_OFFS			0
#define MVPP2_PRS_SRAM_RI_WORD			0
#define MVPP2_PRS_SRAM_RI_CTRL_OFFS		32
#define MVPP2_PRS_SRAM_RI_CTRL_WORD		1
#define MVPP2_PRS_SRAM_RI_CTRL_BITS		32
#define MVPP2_PRS_SRAM_SHIFT_OFFS		64
#define MVPP2_PRS_SRAM_SHIFT_SIGN_BIT		72
#define MVPP2_PRS_SRAM_UDF_OFFS			73
#define MVPP2_PRS_SRAM_UDF_BITS			8
#define MVPP2_PRS_SRAM_UDF_MASK			0xff
#define MVPP2_PRS_SRAM_UDF_SIGN_BIT		81
#define MVPP2_PRS_SRAM_UDF_TYPE_OFFS		82
#define MVPP2_PRS_SRAM_UDF_TYPE_MASK		0x7
#define MVPP2_PRS_SRAM_UDF_TYPE_L3		1
#define MVPP2_PRS_SRAM_UDF_TYPE_L4		4
#define MVPP2_PRS_SRAM_OP_SEL_SHIFT_OFFS	85
#define MVPP2_PRS_SRAM_OP_SEL_SHIFT_MASK	0x3
#define MVPP2_PRS_SRAM_OP_SEL_SHIFT_ADD		1
#define MVPP2_PRS_SRAM_OP_SEL_SHIFT_IP4_ADD	2
#define MVPP2_PRS_SRAM_OP_SEL_SHIFT_IP6_ADD	3
#define MVPP2_PRS_SRAM_OP_SEL_UDF_OFFS		87
#define MVPP2_PRS_SRAM_OP_SEL_UDF_BITS		2
#define MVPP2_PRS_SRAM_OP_SEL_UDF_MASK		0x3
#define MVPP2_PRS_SRAM_OP_SEL_UDF_ADD		0
#define MVPP2_PRS_SRAM_OP_SEL_UDF_IP4_ADD	2
#define MVPP2_PRS_SRAM_OP_SEL_UDF_IP6_ADD	3
#define MVPP2_PRS_SRAM_OP_SEL_BASE_OFFS		89
#define MVPP2_PRS_SRAM_AI_OFFS			90
#define MVPP2_PRS_SRAM_AI_CTRL_OFFS		98
#define MVPP2_PRS_SRAM_AI_CTRL_BITS		8
#define MVPP2_PRS_SRAM_AI_MASK			0xff
#define MVPP2_PRS_SRAM_NEXT_LU_OFFS		106
#define MVPP2_PRS_SRAM_NEXT_LU_MASK		0xf
#define MVPP2_PRS_SRAM_LU_DONE_BIT		110
#define MVPP2_PRS_SRAM_LU_GEN_BIT		111

/* Sram result info bits assignment */
#define MVPP2_PRS_RI_MAC_ME_MASK		0x1
#define MVPP2_PRS_RI_DSA_MASK			0x2
#define MVPP2_PRS_RI_VLAN_MASK			(BIT(2) | BIT(3))
#define MVPP2_PRS_RI_VLAN_NONE			0x0
#define MVPP2_PRS_RI_VLAN_SINGLE		BIT(2)
#define MVPP2_PRS_RI_VLAN_DOUBLE		BIT(3)
#define MVPP2_PRS_RI_VLAN_TRIPLE		(BIT(2) | BIT(3))
#define MVPP2_PRS_RI_CPU_CODE_MASK		0x70
#define MVPP2_PRS_RI_CPU_CODE_RX_SPEC		BIT(4)
#define MVPP2_PRS_RI_L2_CAST_MASK		(BIT(9) | BIT(10))
#define MVPP2_PRS_RI_L2_UCAST			0x0
#define MVPP2_PRS_RI_L2_MCAST			BIT(9)
#define MVPP2_PRS_RI_L2_BCAST			BIT(10)
#define MVPP2_PRS_RI_PPPOE_MASK			0x800
#define MVPP2_PRS_RI_L3_PROTO_MASK		(BIT(12) | BIT(13) | BIT(14))
#define MVPP2_PRS_RI_L3_UN			0x0
#define MVPP2_PRS_RI_L3_IP4			BIT(12)
#define MVPP2_PRS_RI_L3_IP4_OPT			BIT(13)
#define MVPP2_PRS_RI_L3_IP4_OTHER		(BIT(12) | BIT(13))
#define MVPP2_PRS_RI_L3_IP6			BIT(14)
#define MVPP2_PRS_RI_L3_IP6_EXT			(BIT(12) | BIT(14))
#define MVPP2_PRS_RI_L3_ARP			(BIT(13) | BIT(14))
#define MVPP2_PRS_RI_L3_ADDR_MASK		(BIT(15) | BIT(16))
#define MVPP2_PRS_RI_L3_UCAST			0x0
#define MVPP2_PRS_RI_L3_MCAST			BIT(15)
#define MVPP2_PRS_RI_L3_BCAST			(BIT(15) | BIT(16))
#define MVPP2_PRS_RI_IP_FRAG_MASK		0x20000
#define MVPP2_PRS_RI_IP_FRAG_TRUE		BIT(17)
#define MVPP2_PRS_RI_IP_FRAG_FALSE		0
#define MVPP2_PRS_RI_UDF3_MASK			0x300000
#define MVPP2_PRS_RI_UDF3_RX_SPECIAL		BIT(21)
#define MVPP2_PRS_RI_L4_PROTO_MASK		0x1c00000
#define MVPP2_PRS_RI_L4_TCP			BIT(22)
#define MVPP2_PRS_RI_L4_UDP			BIT(23)
#define MVPP2_PRS_RI_L4_OTHER			(BIT(22) | BIT(23))
#define MVPP2_PRS_RI_UDF7_MASK			0x60000000
#define MVPP2_PRS_RI_UDF7_IP6_LITE		BIT(29)
#define MVPP2_PRS_RI_DROP_MASK			0x80000000

/* Sram additional info bits assignment */
#define MVPP2_PRS_IPV4_DIP_AI_BIT		BIT(0)
#define MVPP2_PRS_IPV6_NO_EXT_AI_BIT		BIT(0)
#define MVPP2_PRS_IPV6_EXT_AI_BIT		BIT(1)
#define MVPP2_PRS_IPV6_EXT_AH_AI_BIT		BIT(2)
#define MVPP2_PRS_IPV6_EXT_AH_LEN_AI_BIT	BIT(3)
#define MVPP2_PRS_IPV6_EXT_AH_L4_AI_BIT		BIT(4)
#define MVPP2_PRS_SINGLE_VLAN_AI		0
#define MVPP2_PRS_DBL_VLAN_AI_BIT		BIT(7)
#define MVPP2_PRS_EDSA_VID_AI_BIT		BIT(0)

/* DSA/EDSA type */
#define MVPP2_PRS_TAGGED		true
#define MVPP2_PRS_UNTAGGED		false
#define MVPP2_PRS_EDSA			true
#define MVPP2_PRS_DSA			false

/* lkpid table structure	*/
#define MVPP2_FLOWID_RXQ_OFFS		0
#define MVPP2_FLOWID_RXQ_MASK		0xff

#define MVPP2_FLOWID_FLOW		16
#define MVPP2_FLOWID_FLOW_BITS		9
#define MVPP2_FLOWID_FLOW_MASK		(((1 << \
			MVPP2_FLOWID_FLOW_BITS) - 1) << MVPP2_FLOWID_FLOW)

#define MVPP2_FLOWID_EN_OFFS		25 /*one bit */
#define MVPP2_FLOWID_EN_MASK		BIT(MVPP2_FLOWID_EN_OFFS)

/* flow table structure */
#define MVPP2_FLOW_TBL_SIZE		512
/*-------------------------  DWORD 0  --------------------------------- */
#define MVPP2_FLOW_LAST			0
#define MVPP2_FLOW_LAST_MASK		1 /*one bit*/

#define MVPP2_FLOW_ENGINE		1
#define MVPP2_FLOW_ENGINE_BITS		3
#define MVPP2_FLOW_ENGINE_MASK		(((1 << \
			MVPP2_FLOW_ENGINE_BITS) - 1) << MVPP2_FLOW_ENGINE)
#define MVPP2_FLOW_ENGINE_MAX		7 /* valid value 1 - 7 */

#define MVPP2_FLOW_PORT_ID		4
#define MVPP2_FLOW_PORT_ID_BITS		8
#define MVPP2_FLOW_PORT_ID_MASK		(((1 << \
			MVPP2_FLOW_PORT_ID_BITS) - 1) << MVPP2_FLOW_PORT_ID)
#define MVPP2_FLOW_PORT_ID_MAX		((1 << MVPP2_FLOW_PORT_ID_BITS) - 1)

#define MVPP2_FLOW_PORT_TYPE		12
#define MVPP2_FLOW_PORT_TYPE_BITS	2
#define MVPP2_FLOW_PORT_TYPE_MASK	(((1 << \
		MVPP2_FLOW_PORT_TYPE_BITS) - 1) << MVPP2_FLOW_PORT_TYPE)
#define MVPP2_FLOW_PORT_TYPE_MAX	2 /* valid value 0 - 2 */

#define MVPP2_FLOW_PPPOE		14
#define MVPP2_FLOW_PPPOE_BITS		2
#define MVPP2_FLOW_PPPOE_MASK		(((1 << \
			MVPP2_FLOW_PPPOE_BITS) - 1) << MVPP2_FLOW_PPPOE)
#define MVPP2_FLOW_PPPOE_MAX		2 /* valid value 0 - 2 */

#define MVPP2_FLOW_VLAN			16
#define MVPP2_FLOW_VLAN_BITS		3
#define MVPP2_FLOW_VLAN_MASK		(((1 << \
			MVPP2_FLOW_VLAN_BITS) - 1) << MVPP2_FLOW_VLAN)
#define MVPP2_FLOW_VLAN_MAX		((1 << MVPP2_FLOW_VLAN_BITS) - 1)

#define MVPP2_FLOW_MACME		19
#define MVPP2_FLOW_MACME_BITS		2
#define MVPP2_FLOW_MACME_MASK		(((1 << \
			MVPP2_FLOW_MACME_BITS) - 1) << MVPP2_FLOW_MACME)
#define MVPP2_FLOW_MACME_MAX		2 /* valid value 0 - 2 */

#define MVPP2_FLOW_UDF7			21
#define MVPP2_FLOW_UDF7_BITS		2
#define MVPP2_FLOW_UDF7_MASK		(((1 << \
			MVPP2_FLOW_UDF7_BITS) - 1) << MVPP2_FLOW_UDF7)
#define MVPP2_FLOW_UDF7_MAX		((1 << MVPP2_FLOW_UDF7_BITS) - 1)

#define MVPP2_FLOW_PORT_ID_SEL		23
#define MVPP2_FLOW_PORT_ID_SEL_MASK	BIT(MVPP2_FLOW_PORT_ID_SEL)

/*-----------------------  DWORD 1  ------------------------------------ */

#define MVPP2_FLOW_FIELDS_NUM		0
#define MVPP2_FLOW_FIELDS_NUM_BITS	3
#define MVPP2_FLOW_FIELDS_NUM_MASK	(((1 << \
		MVPP2_FLOW_FIELDS_NUM_BITS) - 1) << MVPP2_FLOW_FIELDS_NUM)
#define MVPP2_FLOW_FIELDS_NUM_MAX	4 /*valid vaue 0 - 4 */

#define MVPP2_FLOW_LKP_TYPE		3
#define MVPP2_FLOW_LKP_TYPE_BITS	6
#define MVPP2_FLOW_LKP_TYPE_MASK	(((1 << \
		MVPP2_FLOW_LKP_TYPE_BITS) - 1) << MVPP2_FLOW_LKP_TYPE)
#define MVPP2_FLOW_LKP_TYPE_MAX		((1 << MVPP2_FLOW_LKP_TYPE_BITS) - 1)

#define MVPP2_FLOW_FIELD_PRIO		9
#define MVPP2_FLOW_FIELD_PRIO_BITS	6
#define MVPP2_FLOW_FIELD_PRIO_MASK	(((1 << \
		MVPP2_FLOW_FIELD_PRIO_BITS) - 1) << MVPP2_FLOW_FIELD_PRIO)
#define MVPP2_FLOW_FIELD_PRIO_MAX	((1 << MVPP2_FLOW_FIELD_PRIO_BITS) - 1)

#define MVPP2_FLOW_SEQ_CTRL		15
#define MVPP2_FLOW_SEQ_CTRL_BITS	3
#define MVPP2_FLOW_SEQ_CTRL_MASK	(((1 << \
		MVPP2_FLOW_SEQ_CTRL_BITS) - 1) << MVPP2_FLOW_SEQ_CTRL)
#define MVPP2_FLOW_SEQ_CTRL_MAX		4

/*-------------------------  DWORD 2  ---------------------------------- */
#define MVPP2_FLOW_FIELD0_ID		0
#define MVPP2_FLOW_FIELD1_ID		6
#define MVPP2_FLOW_FIELD2_ID		12
#define MVPP2_FLOW_FIELD3_ID		18

#define MVPP2_FLOW_FIELD_ID_BITS	6
#define MVPP2_FLOW_FIELD_ID(num)	(MVPP2_FLOW_FIELD0_ID + \
					(MVPP2_FLOW_FIELD_ID_BITS * (num)))
#define MVPP2_FLOW_FIELD_MASK(num)	(((1 << \
	MVPP2_FLOW_FIELD_ID_BITS) - 1) << (MVPP2_FLOW_FIELD_ID_BITS * (num)))
#define MVPP2_FLOW_FIELD_MAX		((1 << MVPP2_FLOW_FIELD_ID_BITS) - 1)

/* lookup id attribute define */
#define MVPP2_PRS_FL_ATTR_VLAN_BIT	BIT(0)
#define MVPP2_PRS_FL_ATTR_IP4_BIT	BIT(1)
#define MVPP2_PRS_FL_ATTR_IP6_BIT	BIT(2)
#define MVPP2_PRS_FL_ATTR_ARP_BIT	BIT(3)
#define MVPP2_PRS_FL_ATTR_FRAG_BIT	BIT(4)
#define MVPP2_PRS_FL_ATTR_TCP_BIT	BIT(5)
#define MVPP2_PRS_FL_ATTR_UDP_BIT	BIT(6)

/* MAC entries, shadow udf */
enum mvpp2_prs_udf {
	MVPP2_PRS_UDF_MAC_DEF,
	MVPP2_PRS_UDF_MAC_RANGE,
	MVPP2_PRS_UDF_L2_DEF,
	MVPP2_PRS_UDF_L2_DEF_COPY,
	MVPP2_PRS_UDF_L2_USER,
};

/* Lookup ID */
enum mvpp2_prs_lookup {
	MVPP2_PRS_LU_MH,
	MVPP2_PRS_LU_MAC,
	MVPP2_PRS_LU_DSA,
	MVPP2_PRS_LU_VLAN,
	MVPP2_PRS_LU_VID,
	MVPP2_PRS_LU_L2,
	MVPP2_PRS_LU_PPPOE,
	MVPP2_PRS_LU_IP4,
	MVPP2_PRS_LU_IP6,
	MVPP2_PRS_LU_FLOWS,
	MVPP2_PRS_LU_LAST,
};

/* L2 cast enum */
enum mvpp2_prs_l2_cast {
	MVPP2_PRS_L2_UNI_CAST,
	MVPP2_PRS_L2_MULTI_CAST,
};

/* L3 cast enum */
enum mvpp2_prs_l3_cast {
	MVPP2_PRS_L3_UNI_CAST,
	MVPP2_PRS_L3_MULTI_CAST,
	MVPP2_PRS_L3_BROAD_CAST
};

/* Packet flow ID */
enum mvpp2_prs_flow {
	MVPP2_PRS_FL_START = 8,
	MVPP2_PRS_FL_IP4_TCP_NF_UNTAG = MVPP2_PRS_FL_START,
	MVPP2_PRS_FL_IP4_UDP_NF_UNTAG,
	MVPP2_PRS_FL_IP4_TCP_NF_TAG,
	MVPP2_PRS_FL_IP4_UDP_NF_TAG,
	MVPP2_PRS_FL_IP6_TCP_NF_UNTAG,
	MVPP2_PRS_FL_IP6_UDP_NF_UNTAG,
	MVPP2_PRS_FL_IP6_TCP_NF_TAG,
	MVPP2_PRS_FL_IP6_UDP_NF_TAG,
	MVPP2_PRS_FL_IP4_TCP_FRAG_UNTAG,
	MVPP2_PRS_FL_IP4_UDP_FRAG_UNTAG,
	MVPP2_PRS_FL_IP4_TCP_FRAG_TAG,
	MVPP2_PRS_FL_IP4_UDP_FRAG_TAG,
	MVPP2_PRS_FL_IP6_TCP_FRAG_UNTAG,
	MVPP2_PRS_FL_IP6_UDP_FRAG_UNTAG,
	MVPP2_PRS_FL_IP6_TCP_FRAG_TAG,
	MVPP2_PRS_FL_IP6_UDP_FRAG_TAG,
	MVPP2_PRS_FL_IP4_UNTAG, /* non-TCP, non-UDP, same for below */
	MVPP2_PRS_FL_IP4_TAG,
	MVPP2_PRS_FL_IP6_UNTAG,
	MVPP2_PRS_FL_IP6_TAG,
	MVPP2_PRS_FL_NON_IP_UNTAG,
	MVPP2_PRS_FL_NON_IP_TAG,
	MVPP2_PRS_FL_LAST,
	MVPP2_PRS_FL_TCAM_NUM = 52,
};

enum mvpp2_cls_engine_num {
	MVPP2_CLS_ENGINE_C2 = 1,
	MVPP2_CLS_ENGINE_C3A,
	MVPP2_CLS_ENGINE_C3B,
	MVPP2_CLS_ENGINE_C4,
	MVPP2_CLS_ENGINE_C3HA = 6,
	MVPP2_CLS_ENGINE_C3HB,
};

enum mvpp2_cls_lkp_type {
	MVPP2_CLS_LKP_HASH = 0,
	MVPP2_CLS_LKP_VLAN_PRI,
	MVPP2_CLS_LKP_DSCP_PRI,
	MVPP2_CLS_LKP_DEFAULT,
	MVPP2_CLS_LKP_MAX,
};

enum mvpp2_cls_fl_pri {
	MVPP2_CLS_FL_COS_PRI = 0,
	MVPP2_CLS_FL_RSS_PRI,
};

enum mvpp2_cls_filed_id {
	MVPP2_CLS_FIELD_IP4SA = 0x10,
	MVPP2_CLS_FIELD_IP4DA = 0x11,
	MVPP2_CLS_FIELD_IP6SA = 0x17,
	MVPP2_CLS_FIELD_IP6DA = 0x1A,
	MVPP2_CLS_FIELD_L4SIP = 0x1D,
	MVPP2_CLS_FIELD_L4DIP = 0x1E,
};

enum mvpp2_cos_type {
	MVPP2_COS_TYPE_DFLT,
	MVPP2_COS_TYPE_VLAN,
	MVPP2_COS_TYPE_DSCP,
	MVPP2_COS_TYPE_NUM
};

enum mvpp2_cos_classifier {
	MVPP2_COS_CLS_VLAN, /* CoS based on VLAN pri */
	MVPP2_COS_CLS_DSCP,
	MVPP2_COS_CLS_VLAN_DSCP, /* CoS based on VLAN pri, */
				/*if untagged and IP, then based on DSCP */
	MVPP2_COS_CLS_DSCP_VLAN,
	MVPP2_COS_CLS_INVALID
};

/* Structure dexcribe RXQ and corresponding rss table */
struct mvpp22_rss_tbl_ptr {
	u8 rxq_idx;
	u8 rss_tbl_ptr;
};

/* Normal RSS entry */
struct mvpp22_rss_tbl_entry {
	u8 tbl_id;
	u8 tbl_line;
	u8 width;
	u8 rxq;
};

enum mvpp2_mac_del_option {
	MVPP2_DEL_MAC_ALL = 0,
	MVPP2_DEL_MAC_NOT_IN_LIST,
};

struct mvpp2_prs_result_info {
	u32 ri;
	u32 ri_mask;
};

struct mvpp2_prs_flow_id {
	u32 flow_id;
	struct mvpp2_prs_result_info prs_result;
};

/* Classifier constants */
#define MVPP2_CLS_FLOWS_TBL_DATA_WORDS	3
#define MVPP2_CLS_FLOWS_TBL_SWAP_IDX	(MVPP2_FLOW_TBL_SIZE - 5)
#define MVPP2_CLS_LKP_TBL_SIZE		64

/* RSS constants */
#define MVPP22_RSS_TABLE_ENTRIES	32

/* BM constants */
#define MVPP2_BM_JUMBO_BUF_NUM		512
#define MVPP2_BM_LONG_BUF_NUM		1024
#define MVPP2_BM_SHORT_BUF_NUM		2048
#define MVPP2_BM_POOL_SIZE_MAX		(16 * 1024 -	\
					MVPP2_BM_POOL_PTR_ALIGN / 4)
#define MVPP2_BM_POOL_PTR_ALIGN		128

/* BM cookie (32 bits) definition */
#define MVPP2_BM_COOKIE_POOL_OFFS	8
#define MVPP2_BM_COOKIE_CPU_OFFS	24

#define MVPP2_BM_SHORT_FRAME_SIZE		1024
#define MVPP2_BM_LONG_FRAME_SIZE		2048
#define MVPP2_BM_JUMBO_FRAME_SIZE		10240
/* BM short pool packet size
 * These value assure that for SWF the total number
 * of bytes allocated for each buffer will be 512
 */
#define MVPP2_BM_SHORT_PKT_SIZE	MVPP2_RX_MAX_PKT_SIZE(MVPP2_BM_SHORT_FRAME_SIZE)
#define MVPP2_BM_LONG_PKT_SIZE	MVPP2_RX_MAX_PKT_SIZE(MVPP2_BM_LONG_FRAME_SIZE)
#define MVPP2_BM_JUMBO_PKT_SIZE	MVPP2_RX_MAX_PKT_SIZE(MVPP2_BM_JUMBO_FRAME_SIZE)

#define MVPP21_ADDR_SPACE_SZ		0
#define MVPP22_ADDR_SPACE_SZ		SZ_64K

/* pp22 HW has 9 hif's(Host Interfaces) and can support up to 9
 * SW threads.
 */
#define MVPP2_MAX_THREADS		9
#define MVPP2_MAX_QVECS			MVPP2_MAX_THREADS

enum mvpp2_bm_pool_log_num {
	MVPP2_BM_SHORT,
	MVPP2_BM_LONG,
	MVPP2_BM_JUMBO,
	MVPP2_BM_POOLS_NUM
};

static struct {
	int pkt_size;
	int buf_num;
} mvpp2_pools[MVPP2_BM_POOLS_NUM];

/* GMAC MIB Counters register definitions */
#define MVPP21_MIB_COUNTERS_OFFSET		0x1000
#define MVPP21_MIB_COUNTERS_PORT_SZ		0x400
#define MVPP22_MIB_COUNTERS_OFFSET		0x0
#define MVPP22_MIB_COUNTERS_PORT_SZ		0x100

#define MVPP2_MIB_GOOD_OCTETS_RCVD		0x0
#define MVPP2_MIB_BAD_OCTETS_RCVD		0x8
#define MVPP2_MIB_CRC_ERRORS_SENT		0xc
#define MVPP2_MIB_UNICAST_FRAMES_RCVD		0x10
#define MVPP2_MIB_BROADCAST_FRAMES_RCVD		0x18
#define MVPP2_MIB_MULTICAST_FRAMES_RCVD		0x1c
#define MVPP2_MIB_FRAMES_64_OCTETS		0x20
#define MVPP2_MIB_FRAMES_65_TO_127_OCTETS	0x24
#define MVPP2_MIB_FRAMES_128_TO_255_OCTETS	0x28
#define MVPP2_MIB_FRAMES_256_TO_511_OCTETS	0x2c
#define MVPP2_MIB_FRAMES_512_TO_1023_OCTETS	0x30
#define MVPP2_MIB_FRAMES_1024_TO_MAX_OCTETS	0x34
#define MVPP2_MIB_GOOD_OCTETS_SENT		0x38
#define MVPP2_MIB_UNICAST_FRAMES_SENT		0x40
#define MVPP2_MIB_MULTICAST_FRAMES_SENT		0x48
#define MVPP2_MIB_BROADCAST_FRAMES_SENT		0x4c
#define MVPP2_MIB_FC_SENT			0x54
#define MVPP2_MIB_FC_RCVD			0x58
#define MVPP2_MIB_RX_FIFO_OVERRUN		0x5c
#define MVPP2_MIB_UNDERSIZE_RCVD		0x60
#define MVPP2_MIB_FRAGMENTS_ERR_RCVD		0x64
#define MVPP2_MIB_OVERSIZE_RCVD			0x68
#define MVPP2_MIB_JABBER_RCVD			0x6c
#define MVPP2_MIB_MAC_RCV_ERROR			0x70
#define MVPP2_MIB_BAD_CRC_EVENT			0x74
#define MVPP2_MIB_COLLISION			0x78
#define MVPP2_MIB_LATE_COLLISION		0x7c

#define MVPP2_MIB_COUNTERS_STATS_DELAY		(1 * HZ)

/* Other counters */
#define MVPP2_OVERRUN_DROP_REG(port)		(0x7000 + 4 * (port))
#define MVPP2_CLS_DROP_REG(port)		(0x7020 + 4 * (port))
#define MVPP2_CNT_IDX_REG			0x7040
#define MVPP2_TX_PKT_FULLQ_DROP_REG		0x7200
#define MVPP2_TX_PKT_EARLY_DROP_REG		0x7204
#define MVPP2_TX_PKT_BM_DROP_REG		0x7208
#define MVPP2_TX_PKT_BM_MC_DROP_REG		0x720c
#define MVPP2_RX_PKT_FULLQ_DROP_REG		0x7220
#define MVPP2_RX_PKT_EARLY_DROP_REG		0x7224
#define MVPP2_RX_PKT_BM_DROP_REG		0x7228

/* Definitions */
struct mvpp2_cos {
	u8 cos_classifier;	/* CoS based on VLAN or DSCP */
	u8 num_cos_queues;	/* number of queue to do CoS */
	u8 default_cos;		/* Default CoS value for non-IP or non-VLAN */
	u8 reserved;
	u32 pri_map;	/* each nibble maps a cos_value(0~7) to a queue */
};

struct mvpp2_rss {
	/* MVPP22 2-tuple hash generated upon IPsrc+IPdst */
	enum { MVPP22_RSS_5T, MVPP22_RSS_2T } rss_mode;
	u8 dflt_cpu; /*non-IP packet */
	u8 rss_en;
};

/* Shared Packet Processor resources */
struct mvpp2 {
	/* Shared registers' base addresses */
	void __iomem *lms_base;
	void __iomem *iface_base;

	/* On PPv2.2, each "software thread" can access the base
	 * register through a separate address space, each 64 KB apart
	 * from each other. Typically, such address spaces will be
	 * used per CPU.
	 */
	void __iomem *swth_base[MVPP2_MAX_THREADS];

	/* On PPv2.2, some port control registers are located into the system
	 * controller space. These registers are accessible through a regmap.
	 */
	struct regmap *sysctrl_base;

	u8 spinlocks_bitmap; /* bitmap of required locks */
	/* Spinlocks per hif to protect BM refill */
	spinlock_t bm_spinlock[MVPP2_MAX_THREADS];

	/* Common clocks */
	struct clk *pp_clk;
	struct clk *gop_clk;
	struct clk *mg_clk;
	struct clk *axi_clk;

	/* List of pointers to port structures */
	int port_count;
	struct mvpp2_port *port_list[MVPP2_MAX_PORTS];

	/* Aggregated TXQs */
	struct mvpp2_tx_queue *aggr_txqs;

	/* BM pools */
	struct mvpp2_bm_pool *bm_pools;

	/* RSS indirection table */
	u32 indir[MVPP22_RSS_TABLE_ENTRIES];
	/* PRS shadow table */
	struct mvpp2_prs_shadow *prs_shadow;
	/* PRS auxiliary table for double vlan entries control */
	bool *prs_double_vlans;
	/* CLS shadow info for update in running time */
	struct mvpp2_cls_shadow *cls_shadow;
	/* C2 shadow info */
	struct mvpp2_c2_shadow *c2_shadow;

	/* Tclk value */
	u32 tclk;

	/* HW version */
	enum { MVPP21, MVPP22 } hw_version;

	/* Maximum number of RXQs per port */
	unsigned int max_port_rxqs;

	/* Workqueue to gather hardware statistics */
	char queue_name[30];
	struct workqueue_struct *stats_queue;

	bool custom_dma_mask;
};

struct mvpp2_pcpu_stats {
	struct	u64_stats_sync syncp;
	u64	rx_packets;
	u64	rx_bytes;
	u64	tx_packets;
	u64	tx_bytes;
};

/* Per-CPU port control */
struct mvpp2_port_pcpu {
	/* Timer & Tasklet for bulk-tx optimization */
	struct hrtimer bulk_timer;
	bool bulk_timer_scheduled;
	bool bulk_timer_restart_req;
	struct tasklet_struct bulk_tasklet;

	/* Timer & Tasklet for egress finalization */
	struct hrtimer tx_done_timer;
	bool tx_done_timer_scheduled;
	struct tasklet_struct tx_done_tasklet;
};

struct mvpp2_queue_vector {
	int irq;
	struct napi_struct napi;
	enum { MVPP2_QUEUE_VECTOR_SHARED, MVPP2_QUEUE_VECTOR_PRIVATE } type;
	int sw_thread_id;
	u16 sw_thread_mask;
	int first_rxq;
	int nrxqs;
	u32 pending_cause_rx;
	struct mvpp2_port *port;
};

struct mvpp2_port {
	u8 id;

	/* Index of the port from the "group of ports" complex point
	 * of view
	 */
	int gop_id;

	int link_irq;

	struct mvpp2 *priv;

	/* Firmware node associated to the port */
	struct fwnode_handle *fwnode;

	/* Is a PHY always connected to the port */
	bool has_phy;

	/* Per-port registers' base address */
	void __iomem *base;
	void __iomem *stats_base;

	struct mvpp2_rx_queue **rxqs;
	unsigned int nrxqs;
	struct mvpp2_tx_queue **txqs;
	unsigned int ntxqs;
	struct net_device *dev;

	int pkt_size;

	/* Per-CPU port control */
	struct mvpp2_port_pcpu __percpu *pcpu;

	/* Flags */
	unsigned long flags;

	u16 tx_ring_size;
	u16 rx_ring_size;
	struct mvpp2_pcpu_stats __percpu *stats;
	u64 *ethtool_stats;

	/* Per-port work and its lock to gather hardware statistics */
	struct mutex gather_stats_lock;
	struct delayed_work stats_work;

	struct device_node *of_node;

	phy_interface_t phy_interface;
	struct phylink *phylink;
	struct phy *comphy;

	struct mvpp2_bm_pool *pool_long;
	struct mvpp2_bm_pool *pool_short;

	/* Index of first port's physical RXQ */
	u8 first_rxq;

	struct mvpp2_queue_vector qvecs[MVPP2_MAX_QVECS];
	unsigned int nqvecs;
	bool has_tx_irqs;

	u32 tx_time_coal;

	struct mvpp2_rss	rss_cfg;
	struct mvpp2_cos	cos_cfg;

	/* us private storage, allocated/used by User/Kernel mode toggling */
	void *us_cfg;

	/* Coherency-update for TX-ON from link_status_irq (on 1 cpu only) */
	struct tasklet_struct txqs_on_tasklet;
};

/* The mvpp2_tx_desc and mvpp2_rx_desc structures describe the
 * layout of the transmit and reception DMA descriptors, and their
 * layout is therefore defined by the hardware design
 */

#define MVPP2_TXD_L3_OFF_SHIFT		0
#define MVPP2_TXD_IP_HLEN_SHIFT		8
#define MVPP2_TXD_L4_CSUM_FRAG		BIT(13)
#define MVPP2_TXD_L4_CSUM_NOT		BIT(14)
#define MVPP2_TXD_IP_CSUM_DISABLE	BIT(15)
#define MVPP2_TXD_PADDING_DISABLE	BIT(23)
#define MVPP2_TXD_L4_UDP		BIT(24)
#define MVPP2_TXD_L3_IP6		BIT(26)
#define MVPP2_TXD_L_DESC		BIT(28)
#define MVPP2_TXD_F_DESC		BIT(29)

#define MVPP2_RXD_ERR_SUMMARY		BIT(15)
#define MVPP2_RXD_ERR_CODE_MASK		(BIT(13) | BIT(14))
#define MVPP2_RXD_ERR_CRC		0x0
#define MVPP2_RXD_ERR_OVERRUN		BIT(13)
#define MVPP2_RXD_ERR_RESOURCE		(BIT(13) | BIT(14))
#define MVPP2_RXD_BM_POOL_ID_OFFS	16
#define MVPP2_RXD_BM_POOL_ID_MASK	(BIT(16) | BIT(17) | BIT(18))
#define MVPP2_RXD_HWF_SYNC		BIT(21)
#define MVPP2_RXD_L4_CSUM_OK		BIT(22)
#define MVPP2_RXD_IP4_HEADER_ERR	BIT(24)
#define MVPP2_RXD_L4_TCP		BIT(25)
#define MVPP2_RXD_L4_UDP		BIT(26)
#define MVPP2_RXD_L3_IP4		BIT(28)
#define MVPP2_RXD_L3_IP6		BIT(30)
#define MVPP2_RXD_BUF_HDR		BIT(31)

#define MV_BM_LOCK			BIT(0)
#define MV_AGGR_QUEUE_LOCK	BIT(1)

/* Sub fields of "parserInfo" field */
#define MVPP2_RXD_LKP_ID_OFFS		0
#define MVPP2_RXD_LKP_ID_BITS		6
#define MVPP2_RXD_LKP_ID_MASK		(((1 << \
		MVPP2_RXD_LKP_ID_BITS) - 1) << MVPP2_RXD_LKP_ID_OFFS)
#define MVPP2_RXD_CPU_CODE_OFFS		6
#define MVPP2_RXD_CPU_CODE_BITS		3
#define MVPP2_RXD_CPU_CODE_MASK		(((1 << \
		MVPP2_RXD_CPU_CODE_BITS) - 1) << MVPP2_RXD_CPU_CODE_OFFS)
#define MVPP2_RXD_PPPOE_BIT		9
#define MVPP2_RXD_PPPOE_MASK		BIT(MVPP2_RXD_PPPOE_BIT)
#define MVPP2_RXD_L3_CAST_OFFS		10
#define MVPP2_RXD_L3_CAST_BITS		2
#define MVPP2_RXD_L3_CAST_MASK		(((1 << \
		MVPP2_RXD_L3_CAST_BITS) - 1) << MVPP2_RXD_L3_CAST_OFFS)
#define MVPP2_RXD_L2_CAST_OFFS		12
#define MVPP2_RXD_L2_CAST_BITS		2
#define MVPP2_RXD_L2_CAST_MASK		(((1 << \
		MVPP2_RXD_L2_CAST_BITS) - 1) << MVPP2_RXD_L2_CAST_OFFS)
#define MVPP2_RXD_VLAN_INFO_OFFS	14
#define MVPP2_RXD_VLAN_INFO_BITS	2
#define MVPP2_RXD_VLAN_INFO_MASK	(((1 << \
		MVPP2_RXD_VLAN_INFO_BITS) - 1) << MVPP2_RXD_VLAN_INFO_OFFS)
/* Bits of "bmQset" field */
#define MVPP2_RXD_BUFF_QSET_NUM_OFFS	0
#define MVPP2_RXD_BUFF_QSET_NUM_MASK	(0x7f << MVPP2_RXD_BUFF_QSET_NUM_OFFS)
#define MVPP2_RXD_BUFF_TYPE_OFFS	7
#define MVPP2_RXD_BUFF_TYPE_MASK	(0x1 << MVPP2_RXD_BUFF_TYPE_OFFS)
/* Bits of "status" field */
#define MVPP2_RXD_L3_OFFSET_OFFS	0
#define MVPP2_RXD_L3_OFFSET_MASK	(0x7F << MVPP2_RXD_L3_OFFSET_OFFS)
#define MVPP2_RXD_IP_HLEN_OFFS		8
#define MVPP2_RXD_IP_HLEN_MASK		(0x1F << MVPP2_RXD_IP_HLEN_OFFS)
#define MVPP2_RXD_ES_BIT		15
#define MVPP2_RXD_ES_MASK		BIT(MVPP2_RXD_ES_BIT)
#define MVPP2_RXD_HWF_SYNC_BIT		21
#define MVPP2_RXD_HWF_SYNC_MASK		BIT(MVPP2_RXD_HWF_SYNC_BIT)
#define MVPP2_RXD_L4_CHK_OK_BIT		22
#define MVPP2_RXD_L4_CHK_OK_MASK	BIT(MVPP2_RXD_L4_CHK_OK_BIT)
#define MVPP2_RXD_IP_FRAG_BIT		23
#define MVPP2_RXD_IP_FRAG_MASK		BIT(MVPP2_RXD_IP_FRAG_BIT)
#define MVPP2_RXD_IP4_HEADER_ERR_BIT	24
#define MVPP2_RXD_IP4_HEADER_ERR_MASK	BIT(MVPP2_RXD_IP4_HEADER_ERR_BIT)
#define MVPP2_RXD_L4_OFFS		25
#define MVPP2_RXD_L4_MASK		(7 << MVPP2_RXD_L4_OFFS)
/* Value 0 - N/A, 3-7 - User Defined */
#define MVPP2_RXD_L3_OFFS		28
#define MVPP2_RXD_L3_MASK		(7 << MVPP2_RXD_L3_OFFS)
/* Value 0 - N/A, 6-7 - User Defined */
#define MVPP2_RXD_L3_IP4_OPT		(2 << MVPP2_RXD_L3_OFFS)
#define MVPP2_RXD_L3_IP4_OTHER		(3 << MVPP2_RXD_L3_OFFS)
#define MVPP2_RXD_L3_IP6_EXT		(5 << MVPP2_RXD_L3_OFFS)
#define MVPP2_RXD_BUF_HDR_BIT		31
#define MVPP2_RXD_BUF_HDR_MASK		BIT(MVPP2_RXD_BUF_HDR_BIT)
/* status field MACROs */
#define MVPP2_RXD_L3_IS_IP4(status)		(((status) & \
				MVPP2_RXD_L3_MASK) == MVPP2_RXD_L3_IP4)
#define MVPP2_RXD_L3_IS_IP4_OPT(status)		(((status) & \
				MVPP2_RXD_L3_MASK) == MVPP2_RXD_L3_IP4_OPT)
#define MVPP2_RXD_L3_IS_IP4_OTHER(status)	(((status) & \
				MVPP2_RXD_L3_MASK) == MVPP2_RXD_L3_IP4_OTHER)
#define MVPP2_RXD_L3_IS_IP6(status)		(((status) & \
				MVPP2_RXD_L3_MASK) == MVPP2_RXD_L3_IP6)
#define MVPP2_RXD_L3_IS_IP6_EXT(status)		(((status) & \
				MVPP2_RXD_L3_MASK) == MVPP2_RXD_L3_IP6_EXT)
#define MVPP2_RXD_L4_IS_UDP(status)		(((status) & \
				MVPP2_RXD_L4_MASK) == MVPP2_RXD_L4_UDP)
#define MVPP2_RXD_L4_IS_TCP(status)		(((status) & \
				MVPP2_RXD_L4_MASK) == MVPP2_RXD_L4_TCP)
#define MVPP2_RXD_IP4_HDR_ERR(status)		((status) & \
				MVPP2_RXD_IP4_HEADER_ERR_MASK)
#define MVPP2_RXD_IP4_FRG(status)		((status) & \
				MVPP2_RXD_IP_FRAG_MASK)
#define MVPP2_RXD_L4_CHK_OK(status)		((status) & \
				MVPP2_RXD_L4_CHK_OK_MASK)

/* HW TX descriptor for PPv2.1 */
struct mvpp21_tx_desc {
	u32 command;		/* Options used by HW for packet transmitting.*/
	u8  packet_offset;	/* the offset from the buffer beginning	*/
	u8  phys_txq;		/* destination queue ID			*/
	u16 data_size;		/* data size of transmitted packet in bytes */
	u32 buf_dma_addr;	/* physical addr of transmitted buffer	*/
	u32 buf_cookie;		/* cookie for access to TX buffer in tx path */
	u32 reserved1[3];	/* hw_cmd (for future use, BM, PON, PNC) */
	u32 reserved2;		/* reserved (for future use)		*/
};

/* HW RX descriptor for PPv2.1 */
struct mvpp21_rx_desc {
	u32 status;		/* info about received packet		*/
	u16 reserved1;		/* parser_info (for future use, PnC)	*/
	u16 data_size;		/* size of received packet in bytes	*/
	u32 buf_dma_addr;	/* physical address of the buffer	*/
	u32 buf_cookie;		/* cookie for access to RX buffer in rx path */
	u16 reserved2;		/* gem_port_id (for future use, PON)	*/
	u16 reserved3;		/* csum_l4 (for future use, PnC)	*/
	u8  reserved4;		/* bm_qset (for future use, BM)		*/
	u8  reserved5;
	u16 reserved6;		/* classify_info (for future use, PnC)	*/
	u32 reserved7;		/* flow_id (for future use, PnC) */
	u32 reserved8;
};

/* HW TX descriptor for PPv2.2 */
struct mvpp22_tx_desc {
	u32 command;
	u8  packet_offset;
	u8  phys_txq;
	u16 data_size;
	u64 desc_misc;		/* multi-purpose (ptp, ...) */;
	u64 buf_dma_addr_ptp;
	u64 buf_cookie_misc;
};

/* HW RX descriptor for PPv2.2 */
struct mvpp22_rx_desc {
	u32 status;
	u16 reserved1;
	u16 data_size;
	u16 reserved2;		/* gem_port_id (for future use, PON)	*/
	u16 reserved3;		/* csum_l4 (for future use, PnC)	*/
	u32 timestamp;		/* ptp */
	u64 buf_dma_addr_key_hash;
	u64 buf_cookie_misc;
};

/* Opaque type used by the driver to manipulate the HW TX and RX
 * descriptors
 */
struct mvpp2_tx_desc {
	union {
		struct mvpp21_tx_desc pp21;
		struct mvpp22_tx_desc pp22;
	};
};

struct mvpp2_rx_desc {
	union {
		struct mvpp21_rx_desc pp21;
		struct mvpp22_rx_desc pp22;
	};
};

struct mvpp2_txq_pcpu_buf {
	/* Transmitted SKB */
	struct sk_buff *skb;

	/* Physical address of transmitted buffer */
	dma_addr_t dma;

	/* Size transmitted */
	size_t size;
};

/* Per-CPU Tx queue control */
struct mvpp2_txq_pcpu {
	int cpu;

	/* Number of Tx DMA descriptors in the descriptor ring */
	int size;

	/* Number of currently used Tx DMA descriptor in the
	 * descriptor ring
	 */
	int count;

	int wake_threshold;
	int stop_threshold;

	/* Number of Tx DMA descriptors reserved for each CPU */
	int reserved_num;

	/* Infos about transmitted buffers */
	struct mvpp2_txq_pcpu_buf *buffs;

	/* Index of last TX DMA descriptor that was inserted */
	int txq_put_index;

	/* Index of the TX DMA descriptor to be cleaned up */
	int txq_get_index;

	/* DMA buffer for TSO headers */
	char *tso_headers;
	dma_addr_t tso_headers_dma;
};

struct mvpp2_tx_queue {
	/* Physical number of this Tx queue */
	u8 id;

	/* Logical number of this Tx queue */
	u8 log_id;

	/* Number of Tx DMA descriptors in the descriptor ring */
	int size;

	/* Number of currently used Tx DMA descriptor in the descriptor ring */
	int count;
	int pending;

	/* Per-CPU control of physical Tx queues */
	struct mvpp2_txq_pcpu __percpu *pcpu;

	u32 done_pkts_coal;

	/* AGGR TX queue lock */
	spinlock_t spinlock;

	/* Virtual address of thex Tx DMA descriptors array */
	struct mvpp2_tx_desc *descs;

	/* DMA address of the Tx DMA descriptors array */
	dma_addr_t descs_dma;

	/* Index of the last Tx DMA descriptor */
	int last_desc;

	/* Index of the next Tx DMA descriptor to process */
	int next_desc_to_proc;
} __aligned(L1_CACHE_BYTES);

struct mvpp2_rx_queue {
	/* RX queue number, in the range 0-31 for physical RXQs */
	u8 id;

	/* Num of rx descriptors in the rx descriptor ring */
	int size;

	u32 pkts_coal;
	u32 time_coal;

	/* Virtual address of the RX DMA descriptors array */
	struct mvpp2_rx_desc *descs;

	/* DMA address of the RX DMA descriptors array */
	dma_addr_t descs_dma;

	/* Index of the last RX DMA descriptor */
	int last_desc;

	/* Index of the next RX DMA descriptor to process */
	int next_desc_to_proc;

	/* ID of port to which physical RXQ is mapped */
	int port;

	/* Port's logic RXQ number to which physical RXQ is mapped */
	int logic_rxq;
};

union mvpp2_prs_tcam_entry {
	u32 word[MVPP2_PRS_TCAM_WORDS];
	u8  byte[MVPP2_PRS_TCAM_WORDS * 4];
};

union mvpp2_prs_sram_entry {
	u32 word[MVPP2_PRS_SRAM_WORDS];
	u8  byte[MVPP2_PRS_SRAM_WORDS * 4];
};

struct mvpp2_prs_entry {
	u32 index;
	union mvpp2_prs_tcam_entry tcam;
	union mvpp2_prs_sram_entry sram;
};

struct mvpp2_prs_shadow {
	bool valid;
	bool finish;

	/* Lookup ID */
	int lu;

	/* User defined offset */
	int udf;

	/* Result info */
	u32 ri;
	u32 ri_mask;
};

struct mvpp2_cls_flow_entry {
	u32 index;
	u32 data[MVPP2_CLS_FLOWS_TBL_DATA_WORDS];
};

struct mvpp2_cls_lookup_entry {
	u32 lkpid;
	u32 way;
	u32 data;
};

struct mvpp2_cls_flow_info {
	u32 lkpid;

	/* The flow table entry index of CoS Dflt/Vlan/Dscp rule */
	u32 flow_entry[MVPP2_COS_TYPE_NUM];

	/* The flow table entry index of RSS rule */
	u32 flow_entry_rss1;
	/* The flow table entry index of RSS rule for UDP packet to
	 * update hash mode
	 */
	u32 flow_entry_rss2;
};

struct mvpp2_cls_shadow {
	struct mvpp2_cls_flow_info *flow_info;
	u32 flow_free_start; /* The start of free entry index in flow table */
	u32 flow_swap_area;
};

/* Classifier engine2 and QoS structure */

/* C2  constants */
#define MVPP2_CLS_C2_TCAM_SIZE			256
#define MVPP2_CLS_C2_TCAM_WORDS			5
#define MVPP2_CLS_C2_TCAM_DATA_BYTES		10
#define MVPP2_CLS_C2_SRAM_WORDS			5
/*                   HEK: advanced Header Extracted Key capability */
#define MVPP2_CLS_C2_HEK_LKP_TYPE_OFFS		0
#define MVPP2_CLS_C2_HEK_LKP_TYPE_BITS		6
#define MVPP2_CLS_C2_HEK_LKP_TYPE_MASK		(0x3F << \
					MVPP2_CLS_C2_HEK_LKP_TYPE_OFFS)
#define MVPP2_CLS_C2_HEK_PORT_TYPE_OFFS		6
#define MVPP2_CLS_C2_HEK_PORT_TYPE_BITS		2
#define MVPP2_CLS_C2_HEK_PORT_TYPE_MASK		(0x3 << \
					MVPP2_CLS_C2_HEK_PORT_TYPE_OFFS)
#define MVPP2_CLS_C2_QOS_DSCP_TBL_SIZE		64
#define MVPP2_CLS_C2_QOS_PRIO_TBL_SIZE		8
#define MVPP2_CLS_C2_QOS_DSCP_TBL_NUM		8
#define MVPP2_CLS_C2_QOS_PRIO_TBL_NUM		64

struct mvpp2_cls_c2_entry {
	u32          index;
	bool         inv;
	union {
		u32	words[MVPP2_CLS_C2_TCAM_WORDS];
		u8	bytes[MVPP2_CLS_C2_TCAM_WORDS * 4];
	} tcam;
	union {
		u32	words[MVPP2_CLS_C2_SRAM_WORDS];
		struct {
			u32 action_tbl; /* 0x1B30 */
			u32 actions;    /* 0x1B60 */
			u32 qos_attr;   /* 0x1B64*/
			u32 hwf_attr;   /* 0x1B68 */
			u32 rss_attr;   /* 0x1B6C */
			u32 seq_attr;   /* 0x1B70 */
		} regs;
	} sram;
};

enum mvpp2_cls2_hek_offs {
	MVPP2_CLS_C2_HEK_OFF_BYTE0 = 0,
	MVPP2_CLS_C2_HEK_OFF_BYTE1,
	MVPP2_CLS_C2_HEK_OFF_BYTE2,
	MVPP2_CLS_C2_HEK_OFF_BYTE3,
	MVPP2_CLS_C2_HEK_OFF_BYTE4,
	MVPP2_CLS_C2_HEK_OFF_BYTE5,
	MVPP2_CLS_C2_HEK_OFF_BYTE6,
	MVPP2_CLS_C2_HEK_OFF_BYTE7,
	MVPP2_CLS_C2_HEK_OFF_LKP_PORT_TYPE,
	MVPP2_CLS_C2_HEK_OFF_PORT_ID,
	MVPP2_CLS_C2_HEK_OFF_MAX
};

struct mvpp2_cls_c2_qos_entry {
	u32 tbl_id;
	u32 tbl_sel;
	u32 tbl_line;
	u32 data;
};

enum mvpp2_src_port_type {
	MVPP2_SRC_PORT_TYPE_PHY,
	MVPP2_SRC_PORT_TYPE_UNI,
	MVPP2_SRC_PORT_TYPE_VIR
};

struct mvpp2_src_port {
	enum mvpp2_src_port_type	port_type;
	u32				port_value;
	u32				port_mask;
};

enum mvpp2_qos_tbl_sel {
	MVPP2_QOS_TBL_SEL_PRI = 0,
	MVPP2_QOS_TBL_SEL_DSCP,
};

enum mvpp2_qos_src_tbl {
	MVPP2_QOS_SRC_ACTION_TBL = 0,
	MVPP2_QOS_SRC_DSCP_PBIT_TBL,
};

struct mvpp2_engine_qos_info {
	/* dscp pri table or none */
	enum mvpp2_qos_tbl_sel	qos_tbl_type;
	/* dscp or pri table index */
	u32				qos_tbl_index;
	/* policer id, 0xffff do not assign policer */
	u16				policer_id;
	/* pri/dscp comes from qos or act tbl */
	enum mvpp2_qos_src_tbl	pri_dscp_src;
	/* gemport comes from qos or act tbl */
	enum mvpp2_qos_src_tbl	gemport_src;
	enum mvpp2_qos_src_tbl	q_low_src;
	enum mvpp2_qos_src_tbl	q_high_src;
	enum mvpp2_qos_src_tbl	color_src;
};

enum mvpp2_color_action_type {
	/* Do not update color */
	MVPP2_COLOR_ACTION_TYPE_NO_UPDT = 0,
	/* Do not update color and lock */
	MVPP2_COLOR_ACTION_TYPE_NO_UPDT_LOCK,
	/* Update to green */
	MVPP2_COLOR_ACTION_TYPE_GREEN,
	/* Update to green and lock */
	MVPP2_COLOR_ACTION_TYPE_GREEN_LOCK,
	/* Update to yellow */
	MVPP2_COLOR_ACTION_TYPE_YELLOW,
	/* Update to yellow */
	MVPP2_COLOR_ACTION_TYPE_YELLOW_LOCK,
	/* Update to red */
	MVPP2_COLOR_ACTION_TYPE_RED,
	/* Update to red and lock */
	MVPP2_COLOR_ACTION_TYPE_RED_LOCK,
};

enum mvpp2_general_action_type {
	/* The field will be not updated */
	MVPP2_ACTION_TYPE_NO_UPDT,
	/* The field will be not updated and lock */
	MVPP2_ACTION_TYPE_NO_UPDT_LOCK,
	/* The field will be updated */
	MVPP2_ACTION_TYPE_UPDT,
	/* The field will be updated and lock */
	MVPP2_ACTION_TYPE_UPDT_LOCK,
};

enum mvpp2_flowid_action_type {
	/* FlowID is disable */
	MVPP2_ACTION_FLOWID_DISABLE = 0,
	/* FlowID is enable */
	MVPP2_ACTION_FLOWID_ENABLE,
};

enum mvpp2_frwd_action_type {
	/* The decision will be not updated */
	MVPP2_FRWD_ACTION_TYPE_NO_UPDT,
	/* The decision is not updated, and following no change to it */
	MVPP2_FRWD_ACTION_TYPE_NO_UPDT_LOCK,
	/* The packet to CPU (Software Forwarding) */
	MVPP2_FRWD_ACTION_TYPE_SWF,
	 /* The packet to CPU, and following no change to it */
	MVPP2_FRWD_ACTION_TYPE_SWF_LOCK,
	/* The packet to one transmit port (Hardware Forwarding) */
	MVPP2_FRWD_ACTION_TYPE_HWF,
	/* The packet to one tx port, and following no change to it */
	MVPP2_FRWD_ACTION_TYPE_HWF_LOCK,
	/* The pkt to one tx port, and maybe internal packets is used */
	MVPP2_FRWD_ACTION_TYPE_HWF_LOW_LATENCY,
	/* Same to above, but following no change to it*/
	MVPP2_FRWD_ACTION_TYPE_HWF_LOW_LATENCY_LOCK,
};

struct mvpp2_engine_pkt_action {
	enum mvpp2_color_action_type		color_act;
	enum mvpp2_general_action_type	pri_act;
	enum mvpp2_general_action_type	dscp_act;
	enum mvpp2_general_action_type	gemp_act;
	enum mvpp2_general_action_type	q_low_act;
	enum mvpp2_general_action_type	q_high_act;
	enum mvpp2_general_action_type	rss_act;
	enum mvpp2_flowid_action_type		flowid_act;
	enum mvpp2_frwd_action_type		frwd_act;
};

struct mvpp2_qos_value {
	u16		pri;
	u16		dscp;
	u16		gemp;
	u16		q_low;
	u16		q_high;
};

struct mvpp2_engine_pkt_mod {
	u32		mod_cmd_idx;
	u32		mod_data_idx;
	u32		l4_chksum_update_flag;
};

struct mvpp2_duplicate_info {
	/* pkt duplication flow id */
	u32		flow_id;
	/* pkt duplication count */
	u32		flow_cnt;
};

/* The logic C2 entry, easy to understand and use */
struct mvpp2_c2_add_entry {
	struct mvpp2_src_port		port;
	u8				lkp_type;
	u8				lkp_type_mask;
	/* priority in this look_type */
	u32				priority;
	/* all the qos input */
	struct mvpp2_engine_qos_info	qos_info;
	/* update&lock info */
	struct mvpp2_engine_pkt_action	action;
	/* pri/dscp/gemport/qLow/qHigh */
	struct mvpp2_qos_value		qos_value;
	/* PMT cmd_idx and data_idx */
	struct mvpp2_engine_pkt_mod	pkt_mod;
	/* RSS enable or disable */
	int				rss_en;
	/* pkt duplication flow info */
	struct mvpp2_duplicate_info	flow_info;
};

struct mvpp2_c2_rule_idx {
	/* The TCAM rule index for VLAN pri check with QoS pbit table */
	u32 vlan_pri_idx;
	/* The TCAM rule index for DSCP check with QoS dscp table */
	u32 dscp_pri_idx;
	/* The default rule for flow untagged and non-IP */
	u32 default_rule_idx;
};

struct mvpp2_c2_shadow {
	int c2_tcam_free_start;
	/* Per src port */
	struct mvpp2_c2_rule_idx rule_idx_info[8];
};

struct mvpp2_bm_pool {
	/* Pool number in the range 0-7 */
	int id;

	/* Buffer Pointers Pool External (BPPE) size */
	int size;
	/* BPPE size in bytes */
	int size_bytes;
	/* Number of buffers for this pool */
	int buf_num;
	/* Pool buffer size */
	int buf_size;
	/* Packet size */
	int pkt_size;
	int frag_size;

	/* BPPE virtual base address */
	u32 *virt_addr;
	/* BPPE DMA base address */
	dma_addr_t dma_addr;

	/* Ports using BM pool */
	u32 port_map;
};

#define IS_TSO_HEADER(txq_pcpu, addr) \
	((addr) >= (txq_pcpu)->tso_headers_dma && \
	 (addr) < (txq_pcpu)->tso_headers_dma + \
	 (txq_pcpu)->size * TSO_HEADER_SIZE)

/* The prototype is added here to be used in start_dev when using ACPI. This
 * will be removed once phylink is used for all modes (dt+ACPI).
 */
static void mvpp2_mac_config(struct net_device *dev, unsigned int mode,
			     const struct phylink_link_state *state);

/* Queue modes */
enum mv_pp2_queue_distribution_mode {
	MVPP2_QDIST_SINGLE_MODE,
	MVPP2_QDIST_MULTI_MODE,
	MVPP2_SINGLE_RESOURCE_MODE
};

static int queue_mode = MVPP2_QDIST_MULTI_MODE;
static u8 used_hifs;

module_param(queue_mode, int, 0444);
MODULE_PARM_DESC(queue_mode, "Set queue_mode (single=0, multi=1)");

static short num_cos_queues = 4;
module_param(num_cos_queues, short, 0444);
MODULE_PARM_DESC(num_cos_queues, "num_cos_queues 1 or 4");

static bool rss_mode;
module_param(rss_mode, bool, 0444);
MODULE_PARM_DESC(rss_mode, "true or false - for 2 or 5 hash tuples");

static short default_cpu;
module_param(default_cpu, short, 0444);
MODULE_PARM_DESC(default_cpu, "run on cpu-N when RSS disabled");

static long pri_map = 0x3210;
module_param(pri_map, long, 0444);
MODULE_PARM_DESC(pri_map, "0x3210:: cos0:rxq0, cos1:rxq1, cos2:rxq2, cos3:rxq3");

static short default_cos = 3;
module_param(default_cos, short, 0444);
MODULE_PARM_DESC(default_cos, "CoS value for non-IP packet");

/* Bind/Map port-id into CPUno written in appropriated port-id-nibble */
static long rx_cpu_map;
module_param(rx_cpu_map, long, 0444);
MODULE_PARM_DESC(rx_cpu_map, "map Port(nibble-shift) into CPUno(nibble-value)");

static u8 cos_classifier;
/* END ---------- MODULE Extra-params ---------- */

/* High-level User-Interface COS config-parameters */
#define MVPP2_COS_PARAM_MAX_STRLEN	48
#define MVPP2_CFG_PARAM_BUF_SIZE	PAGE_SIZE

enum { MVPP2_COS_MODE, MVPP2_COS_DFLT, MVPP2_COS2RXQ };

static const char * const mvpp2_param[] = {"cos_mode", "cos_dflt", "cos2rxq"};

/* RX-TX fast-forwarding path optimization */
#define MVPP2_RXTX_HASH			0xbac0
#define MVPP2_RXTX_HASH_BMID_MASK	0xf
/* The recycle pool size should be "effectively big" but limited (to eliminate
 * memory-wasting on TX-pick). It should be >8 (Net-stack-forwarding-buffer)
 * and >pkt-coalescing. For "effective" >=NAPI_POLL_WEIGHT.
 * For 4 ports we need more buffers but not x4, statistically it is enough x3.
 * SKB-pool is shared for Small/Large/Jumbo buffers so we need more SKBs,
 * statistically it is enough x5.
 */
#define MVPP2_RECYCLE_FULL	(NAPI_POLL_WEIGHT * 3)
#define MVPP2_RECYCLE_FULL_SKB	(NAPI_POLL_WEIGHT * 5)

struct mvpp2_recycle_pool {
	void *pbuf[MVPP2_RECYCLE_FULL_SKB];
};

struct mvpp2_recycle_pcpu {
	/* All pool-indexes are in 1 cache-line */
	short int idx[MVPP2_BM_POOLS_NUM + 1];
	/* BM/SKB-buffer pools */
	struct mvpp2_recycle_pool pool[MVPP2_BM_POOLS_NUM + 1];
} __aligned(L1_CACHE_BYTES);

struct mvpp2_share {
	struct mvpp2_recycle_pcpu *recycle;
	void *recycle_base;

	/* Run-time Debug disable capability */
	bool recycle_dis;

	/* Counters set by Probe/Init/Open */
	int num_cp;
	int num_open_ports;
};

struct mvpp2_share mvpp2_share;

static void mvpp2_recycle_put(struct mvpp2_txq_pcpu *txq_pcpu);

#define MVPP2_DRIVER_NAME "mvpp2"
#define MVPP2_DRIVER_VERSION "1.0"

/* Utility/helper methods */

static void mvpp2_write(struct mvpp2 *priv, u32 offset, u32 data)
{
	writel(data, priv->swth_base[0] + offset);
}

static u32 mvpp2_read(struct mvpp2 *priv, u32 offset)
{
	return readl(priv->swth_base[0] + offset);
}

static u32 mvpp2_read_relaxed(struct mvpp2 *priv, u32 offset)
{
	return readl_relaxed(priv->swth_base[0] + offset);
}

static int mvpp2_check_sw_thread(int cpu)
{
	if (queue_mode == MVPP2_SINGLE_RESOURCE_MODE)
		return 0;
	else
		return cpu;
}

/* These accessors should be used to access:
 *
 * - per-CPU registers, where each CPU has its own copy of the
 *   register.
 *
 *   MVPP2_BM_VIRT_ALLOC_REG
 *   MVPP2_BM_ADDR_HIGH_ALLOC
 *   MVPP22_BM_ADDR_HIGH_RLS_REG
 *   MVPP2_BM_VIRT_RLS_REG
 *   MVPP2_ISR_RX_TX_CAUSE_REG
 *   MVPP2_ISR_RX_TX_MASK_REG
 *   MVPP2_TXQ_NUM_REG
 *   MVPP2_AGGR_TXQ_UPDATE_REG
 *   MVPP2_TXQ_RSVD_REQ_REG
 *   MVPP2_TXQ_RSVD_RSLT_REG
 *   MVPP2_TXQ_SENT_REG
 *   MVPP2_RXQ_NUM_REG
 *
 * - global registers that must be accessed through a specific CPU
 *   window, because they are related to an access to a per-CPU
 *   register
 *
 *   MVPP2_BM_PHY_ALLOC_REG    (related to MVPP2_BM_VIRT_ALLOC_REG)
 *   MVPP2_BM_PHY_RLS_REG      (related to MVPP2_BM_VIRT_RLS_REG)
 *   MVPP2_RXQ_THRESH_REG      (related to MVPP2_RXQ_NUM_REG)
 *   MVPP2_RXQ_DESC_ADDR_REG   (related to MVPP2_RXQ_NUM_REG)
 *   MVPP2_RXQ_DESC_SIZE_REG   (related to MVPP2_RXQ_NUM_REG)
 *   MVPP2_RXQ_INDEX_REG       (related to MVPP2_RXQ_NUM_REG)
 *   MVPP2_TXQ_PENDING_REG     (related to MVPP2_TXQ_NUM_REG)
 *   MVPP2_TXQ_DESC_ADDR_REG   (related to MVPP2_TXQ_NUM_REG)
 *   MVPP2_TXQ_DESC_SIZE_REG   (related to MVPP2_TXQ_NUM_REG)
 *   MVPP2_TXQ_INDEX_REG       (related to MVPP2_TXQ_NUM_REG)
 *   MVPP2_TXQ_PENDING_REG     (related to MVPP2_TXQ_NUM_REG)
 *   MVPP2_TXQ_PREF_BUF_REG    (related to MVPP2_TXQ_NUM_REG)
 *   MVPP2_TXQ_PREF_BUF_REG    (related to MVPP2_TXQ_NUM_REG)
 */
static void mvpp2_percpu_write(struct mvpp2 *priv, int cpu,
			       u32 offset, u32 data)
{
	cpu = mvpp2_check_sw_thread(cpu);
	writel(data, priv->swth_base[cpu] + offset);
}

static u32 mvpp2_percpu_read(struct mvpp2 *priv, int cpu,
			     u32 offset)
{
	cpu = mvpp2_check_sw_thread(cpu);
	return readl(priv->swth_base[cpu] + offset);
}

static void mvpp2_percpu_write_relaxed(struct mvpp2 *priv, int cpu,
				       u32 offset, u32 data)
{
	writel_relaxed(data, priv->swth_base[cpu] + offset);
}

static u32 mvpp2_percpu_read_relaxed(struct mvpp2 *priv, int cpu,
				     u32 offset)
{
	return readl_relaxed(priv->swth_base[cpu] + offset);
}

static dma_addr_t mvpp2_txdesc_dma_addr_get(struct mvpp2_port *port,
					    struct mvpp2_tx_desc *tx_desc)
{
	if (port->priv->hw_version == MVPP21)
		return tx_desc->pp21.buf_dma_addr;
	else
		return tx_desc->pp22.buf_dma_addr_ptp & GENMASK_ULL(39, 0);
}

static void mvpp2_txdesc_dma_addr_set(struct mvpp2_port *port,
				      struct mvpp2_tx_desc *tx_desc,
				      dma_addr_t dma_addr)
{
	dma_addr_t addr, offset;

	addr = dma_addr & ~MVPP2_TX_DESC_ALIGN;
	offset = dma_addr & MVPP2_TX_DESC_ALIGN;

	if (port->priv->hw_version == MVPP21) {
		tx_desc->pp21.buf_dma_addr = addr;
		tx_desc->pp21.packet_offset = offset;
	} else {
		u64 val = (u64)addr;

		tx_desc->pp22.buf_dma_addr_ptp &= ~GENMASK_ULL(39, 0);
		tx_desc->pp22.buf_dma_addr_ptp |= val;
		tx_desc->pp22.packet_offset = offset;
	}
}

static size_t mvpp2_txdesc_size_get(struct mvpp2_port *port,
				    struct mvpp2_tx_desc *tx_desc)
{
	if (port->priv->hw_version == MVPP21)
		return tx_desc->pp21.data_size;
	else
		return tx_desc->pp22.data_size;
}

static void mvpp2_txdesc_size_set(struct mvpp2_port *port,
				  struct mvpp2_tx_desc *tx_desc,
				  size_t size)
{
	if (port->priv->hw_version == MVPP21)
		tx_desc->pp21.data_size = size;
	else
		tx_desc->pp22.data_size = size;
}

static void mvpp2_txdesc_txq_set(struct mvpp2_port *port,
				 struct mvpp2_tx_desc *tx_desc,
				 unsigned int txq)
{
	if (port->priv->hw_version == MVPP21)
		tx_desc->pp21.phys_txq = txq;
	else
		tx_desc->pp22.phys_txq = txq;
}

static void mvpp2_txdesc_cmd_set(struct mvpp2_port *port,
				 struct mvpp2_tx_desc *tx_desc,
				 unsigned int command)
{
	if (port->priv->hw_version == MVPP21)
		tx_desc->pp21.command = command;
	else
		tx_desc->pp22.command = command;
}

static unsigned int mvpp2_txdesc_offset_get(struct mvpp2_port *port,
					    struct mvpp2_tx_desc *tx_desc)
{
	if (port->priv->hw_version == MVPP21)
		return tx_desc->pp21.packet_offset;
	else
		return tx_desc->pp22.packet_offset;
}

static dma_addr_t mvpp2_rxdesc_dma_addr_get(struct mvpp2_port *port,
					    struct mvpp2_rx_desc *rx_desc)
{
	if (port->priv->hw_version == MVPP21)
		return rx_desc->pp21.buf_dma_addr;
	else
		return rx_desc->pp22.buf_dma_addr_key_hash & GENMASK_ULL(39, 0);
}

static size_t mvpp2_rxdesc_size_get(struct mvpp2_port *port,
				    struct mvpp2_rx_desc *rx_desc)
{
	if (port->priv->hw_version == MVPP21)
		return rx_desc->pp21.data_size;
	else
		return rx_desc->pp22.data_size;
}

static u32 mvpp2_rxdesc_status_get(struct mvpp2_port *port,
				   struct mvpp2_rx_desc *rx_desc)
{
	if (port->priv->hw_version == MVPP21)
		return rx_desc->pp21.status;
	else
		return rx_desc->pp22.status;
}

static void mvpp2_txq_inc_get(struct mvpp2_txq_pcpu *txq_pcpu)
{
	txq_pcpu->txq_get_index++;
	if (txq_pcpu->txq_get_index == txq_pcpu->size)
		txq_pcpu->txq_get_index = 0;
}

static inline void mvpp2_rx_desc_endian(int hw_version,
					struct mvpp2_rx_desc *rx_desc)
{
#if defined(__BIG_ENDIAN)
	if (hw_version == MVPP21) {
		cpu_to_le32s(&rx_desc->pp21.status);
		cpu_to_le16s(&rx_desc->pp21.reserved1);
		cpu_to_le16s(&rx_desc->pp21.data_size);
		cpu_to_le32s(&rx_desc->pp21.buf_dma_addr);
		cpu_to_le32s(&rx_desc->pp21.buf_cookie);
		cpu_to_le16s(&rx_desc->pp21.reserved2);
		cpu_to_le16s(&rx_desc->pp21.reserved3);
		/* No swap needed for 2 BYTE-fields reserved4/5 */
		cpu_to_le16s(&rx_desc->pp21.reserved6);
		cpu_to_le32s(&rx_desc->pp21.reserved7);
		cpu_to_le32s(&rx_desc->pp21.reserved8);
	} else {
		cpu_to_le32s(&rx_desc->pp22.status);
		cpu_to_le16s(&rx_desc->pp22.reserved1);
		cpu_to_le16s(&rx_desc->pp22.data_size);
		cpu_to_le16s(&rx_desc->pp22.reserved2);
		cpu_to_le16s(&rx_desc->pp22.reserved3);
		cpu_to_le32s(&rx_desc->pp22.timestamp);
		cpu_to_le64s(&rx_desc->pp22.buf_dma_addr_key_hash);
		cpu_to_le64s(&rx_desc->pp22.buf_cookie_misc);
	}
#endif
}

static inline void mvpp2_tx_desc_endian(int hw_version,
					struct mvpp2_tx_desc *tx_desc)
{
#if defined(__BIG_ENDIAN)
	if (hw_version == MVPP21) {
		cpu_to_le32s(&tx_desc->pp21.command);
		/* No swap needed for 2 BYTE-fields packet_offset/phys_txq */
		cpu_to_le16s(&tx_desc->pp21.data_size);
		cpu_to_le32s(&tx_desc->pp21.buf_dma_addr);
		cpu_to_le32s(&tx_desc->pp21.buf_cookie);
		cpu_to_le32s(&tx_desc->pp21.reserved1[0]);
		cpu_to_le32s(&tx_desc->pp21.reserved1[1]);
		cpu_to_le32s(&tx_desc->pp21.reserved1[2]);
		cpu_to_le32s(&tx_desc->pp21.reserved2);
	} else {
		cpu_to_le32s(&tx_desc->pp22.command);
		/* No swap needed for 2 BYTE-fields packet_offset/phys_txq */
		cpu_to_le16s(&tx_desc->pp22.data_size);
		cpu_to_le64s(&tx_desc->pp22.desc_misc);
		cpu_to_le64s(&tx_desc->pp22.buf_dma_addr_ptp);
		cpu_to_le64s(&tx_desc->pp22.buf_cookie_misc);
	}
#endif
}

static void mvpp2_txq_inc_put(struct mvpp2_port *port,
			      struct mvpp2_txq_pcpu *txq_pcpu,
			      struct sk_buff *skb,
			      struct mvpp2_tx_desc *tx_desc)
{
	struct mvpp2_txq_pcpu_buf *tx_buf =
		txq_pcpu->buffs + txq_pcpu->txq_put_index;
	tx_buf->skb = skb;
	tx_buf->size = mvpp2_txdesc_size_get(port, tx_desc);
	tx_buf->dma = mvpp2_txdesc_dma_addr_get(port, tx_desc) +
		mvpp2_txdesc_offset_get(port, tx_desc);
	txq_pcpu->txq_put_index++;
	if (txq_pcpu->txq_put_index == txq_pcpu->size)
		txq_pcpu->txq_put_index = 0;

	mvpp2_tx_desc_endian(port->priv->hw_version, tx_desc);
}

/* Get number of physical egress port */
static inline int mvpp2_egress_port(struct mvpp2_port *port)
{
	return MVPP2_MAX_TCONT + port->id;
}

/* Get number of physical TXQ */
static inline int mvpp2_txq_phys(int port, int txq)
{
	return (MVPP2_MAX_TCONT + port) * MVPP2_MAX_TXQ + txq;
}

/* Flow ID definition array */
static struct mvpp2_prs_flow_id
	mvpp2_prs_flow_id_array[MVPP2_PRS_FL_TCAM_NUM] = {
	/***********#Flow ID#**************#Result Info#************/
	{MVPP2_PRS_FL_IP4_TCP_NF_UNTAG, {MVPP2_PRS_RI_VLAN_NONE |
					 MVPP2_PRS_RI_L3_IP4 |
					 MVPP2_PRS_RI_IP_FRAG_FALSE |
					 MVPP2_PRS_RI_L4_TCP,
					 MVPP2_PRS_RI_VLAN_MASK |
					 MVPP2_PRS_RI_L3_PROTO_MASK |
					 MVPP2_PRS_RI_IP_FRAG_MASK |
					 MVPP2_PRS_RI_L4_PROTO_MASK} },
	{MVPP2_PRS_FL_IP4_TCP_NF_UNTAG, {MVPP2_PRS_RI_VLAN_NONE |
					 MVPP2_PRS_RI_L3_IP4_OPT |
					 MVPP2_PRS_RI_IP_FRAG_FALSE |
					 MVPP2_PRS_RI_L4_TCP,
					 MVPP2_PRS_RI_VLAN_MASK |
					 MVPP2_PRS_RI_L3_PROTO_MASK |
					 MVPP2_PRS_RI_IP_FRAG_MASK |
					 MVPP2_PRS_RI_L4_PROTO_MASK} },
	{MVPP2_PRS_FL_IP4_TCP_NF_UNTAG, {MVPP2_PRS_RI_VLAN_NONE |
					 MVPP2_PRS_RI_L3_IP4_OTHER |
					 MVPP2_PRS_RI_IP_FRAG_FALSE |
					 MVPP2_PRS_RI_L4_TCP,
					 MVPP2_PRS_RI_VLAN_MASK |
					 MVPP2_PRS_RI_L3_PROTO_MASK |
					 MVPP2_PRS_RI_IP_FRAG_MASK |
					 MVPP2_PRS_RI_L4_PROTO_MASK} },

	{MVPP2_PRS_FL_IP4_UDP_NF_UNTAG, {MVPP2_PRS_RI_VLAN_NONE |
					 MVPP2_PRS_RI_L3_IP4 |
					 MVPP2_PRS_RI_IP_FRAG_FALSE |
					 MVPP2_PRS_RI_L4_UDP,
					 MVPP2_PRS_RI_VLAN_MASK |
					 MVPP2_PRS_RI_L3_PROTO_MASK |
					 MVPP2_PRS_RI_IP_FRAG_MASK |
					 MVPP2_PRS_RI_L4_PROTO_MASK} },
	{MVPP2_PRS_FL_IP4_UDP_NF_UNTAG, {MVPP2_PRS_RI_VLAN_NONE |
					 MVPP2_PRS_RI_L3_IP4_OPT |
					 MVPP2_PRS_RI_IP_FRAG_FALSE |
					 MVPP2_PRS_RI_L4_UDP,
					 MVPP2_PRS_RI_VLAN_MASK |
					 MVPP2_PRS_RI_L3_PROTO_MASK |
					 MVPP2_PRS_RI_IP_FRAG_MASK |
					 MVPP2_PRS_RI_L4_PROTO_MASK} },
	{MVPP2_PRS_FL_IP4_UDP_NF_UNTAG, {MVPP2_PRS_RI_VLAN_NONE |
					 MVPP2_PRS_RI_L3_IP4_OTHER |
					 MVPP2_PRS_RI_IP_FRAG_FALSE |
					 MVPP2_PRS_RI_L4_UDP,
					 MVPP2_PRS_RI_VLAN_MASK |
					 MVPP2_PRS_RI_L3_PROTO_MASK |
					 MVPP2_PRS_RI_IP_FRAG_MASK |
					 MVPP2_PRS_RI_L4_PROTO_MASK} },

	{MVPP2_PRS_FL_IP4_TCP_NF_TAG,	{MVPP2_PRS_RI_L3_IP4 |
					 MVPP2_PRS_RI_IP_FRAG_FALSE |
					 MVPP2_PRS_RI_L4_TCP,
					 MVPP2_PRS_RI_L3_PROTO_MASK |
					 MVPP2_PRS_RI_IP_FRAG_MASK |
					 MVPP2_PRS_RI_L4_PROTO_MASK} },
	{MVPP2_PRS_FL_IP4_TCP_NF_TAG,	{MVPP2_PRS_RI_L3_IP4_OPT |
					 MVPP2_PRS_RI_IP_FRAG_FALSE |
					 MVPP2_PRS_RI_L4_TCP,
					 MVPP2_PRS_RI_L3_PROTO_MASK |
					 MVPP2_PRS_RI_IP_FRAG_MASK |
					 MVPP2_PRS_RI_L4_PROTO_MASK} },
	{MVPP2_PRS_FL_IP4_TCP_NF_TAG,	{MVPP2_PRS_RI_L3_IP4_OTHER |
					 MVPP2_PRS_RI_IP_FRAG_FALSE |
					 MVPP2_PRS_RI_L4_TCP,
					 MVPP2_PRS_RI_L3_PROTO_MASK |
					 MVPP2_PRS_RI_IP_FRAG_MASK |
					 MVPP2_PRS_RI_L4_PROTO_MASK} },

	{MVPP2_PRS_FL_IP4_UDP_NF_TAG,	{MVPP2_PRS_RI_L3_IP4 |
					 MVPP2_PRS_RI_IP_FRAG_FALSE |
					 MVPP2_PRS_RI_L4_UDP,
					 MVPP2_PRS_RI_L3_PROTO_MASK |
					 MVPP2_PRS_RI_IP_FRAG_MASK |
					 MVPP2_PRS_RI_L4_PROTO_MASK} },
	{MVPP2_PRS_FL_IP4_UDP_NF_TAG,	{MVPP2_PRS_RI_L3_IP4_OPT |
					 MVPP2_PRS_RI_IP_FRAG_FALSE |
					 MVPP2_PRS_RI_L4_UDP,
					 MVPP2_PRS_RI_L3_PROTO_MASK |
					 MVPP2_PRS_RI_IP_FRAG_MASK |
					 MVPP2_PRS_RI_L4_PROTO_MASK} },
	{MVPP2_PRS_FL_IP4_UDP_NF_TAG,	{MVPP2_PRS_RI_L3_IP4_OTHER |
					 MVPP2_PRS_RI_IP_FRAG_FALSE |
					 MVPP2_PRS_RI_L4_UDP,
					 MVPP2_PRS_RI_L3_PROTO_MASK |
					 MVPP2_PRS_RI_IP_FRAG_MASK |
					 MVPP2_PRS_RI_L4_PROTO_MASK} },

	{MVPP2_PRS_FL_IP6_TCP_NF_UNTAG, {MVPP2_PRS_RI_VLAN_NONE |
					 MVPP2_PRS_RI_L3_IP6 |
					 MVPP2_PRS_RI_IP_FRAG_FALSE |
					 MVPP2_PRS_RI_L4_TCP,
					 MVPP2_PRS_RI_VLAN_MASK |
					 MVPP2_PRS_RI_L3_PROTO_MASK |
					 MVPP2_PRS_RI_IP_FRAG_MASK |
					 MVPP2_PRS_RI_L4_PROTO_MASK} },
	{MVPP2_PRS_FL_IP6_TCP_NF_UNTAG, {MVPP2_PRS_RI_VLAN_NONE |
					 MVPP2_PRS_RI_L3_IP6_EXT |
					 MVPP2_PRS_RI_IP_FRAG_FALSE |
					 MVPP2_PRS_RI_L4_TCP,
					 MVPP2_PRS_RI_VLAN_MASK |
					 MVPP2_PRS_RI_L3_PROTO_MASK |
					 MVPP2_PRS_RI_IP_FRAG_MASK |
					 MVPP2_PRS_RI_L4_PROTO_MASK} },

	{MVPP2_PRS_FL_IP6_UDP_NF_UNTAG, {MVPP2_PRS_RI_VLAN_NONE |
					 MVPP2_PRS_RI_L3_IP6 |
					 MVPP2_PRS_RI_IP_FRAG_FALSE |
					 MVPP2_PRS_RI_L4_UDP,
					 MVPP2_PRS_RI_VLAN_MASK |
					 MVPP2_PRS_RI_L3_PROTO_MASK |
					 MVPP2_PRS_RI_IP_FRAG_MASK |
					 MVPP2_PRS_RI_L4_PROTO_MASK} },
	{MVPP2_PRS_FL_IP6_UDP_NF_UNTAG, {MVPP2_PRS_RI_VLAN_NONE |
					 MVPP2_PRS_RI_L3_IP6_EXT |
					 MVPP2_PRS_RI_IP_FRAG_FALSE |
					 MVPP2_PRS_RI_L4_UDP,
					 MVPP2_PRS_RI_VLAN_MASK |
					 MVPP2_PRS_RI_L3_PROTO_MASK |
					 MVPP2_PRS_RI_IP_FRAG_MASK |
					 MVPP2_PRS_RI_L4_PROTO_MASK} },

	{MVPP2_PRS_FL_IP6_TCP_NF_TAG,	{MVPP2_PRS_RI_L3_IP6 |
					 MVPP2_PRS_RI_IP_FRAG_FALSE |
					 MVPP2_PRS_RI_L4_TCP,
					 MVPP2_PRS_RI_L3_PROTO_MASK |
					 MVPP2_PRS_RI_IP_FRAG_MASK |
					 MVPP2_PRS_RI_L4_PROTO_MASK} },
	{MVPP2_PRS_FL_IP6_TCP_NF_TAG,	{MVPP2_PRS_RI_L3_IP6_EXT |
					 MVPP2_PRS_RI_IP_FRAG_FALSE |
					 MVPP2_PRS_RI_L4_TCP,
					 MVPP2_PRS_RI_L3_PROTO_MASK |
					 MVPP2_PRS_RI_IP_FRAG_MASK |
					 MVPP2_PRS_RI_L4_PROTO_MASK} },

	{MVPP2_PRS_FL_IP6_UDP_NF_TAG,	{MVPP2_PRS_RI_L3_IP6 |
					 MVPP2_PRS_RI_IP_FRAG_FALSE |
					 MVPP2_PRS_RI_L4_UDP,
					 MVPP2_PRS_RI_L3_PROTO_MASK |
					 MVPP2_PRS_RI_IP_FRAG_MASK |
					 MVPP2_PRS_RI_L4_PROTO_MASK} },
	{MVPP2_PRS_FL_IP6_UDP_NF_TAG,	{MVPP2_PRS_RI_L3_IP6_EXT |
					 MVPP2_PRS_RI_IP_FRAG_FALSE |
					 MVPP2_PRS_RI_L4_UDP,
					 MVPP2_PRS_RI_L3_PROTO_MASK |
					 MVPP2_PRS_RI_IP_FRAG_MASK |
					 MVPP2_PRS_RI_L4_PROTO_MASK} },

	{MVPP2_PRS_FL_IP4_TCP_FRAG_UNTAG, {MVPP2_PRS_RI_VLAN_NONE |
					   MVPP2_PRS_RI_L3_IP4 |
					   MVPP2_PRS_RI_IP_FRAG_TRUE |
					   MVPP2_PRS_RI_L4_TCP,
					   MVPP2_PRS_RI_VLAN_MASK |
					   MVPP2_PRS_RI_L3_PROTO_MASK |
					   MVPP2_PRS_RI_IP_FRAG_MASK |
					   MVPP2_PRS_RI_L4_PROTO_MASK} },
	{MVPP2_PRS_FL_IP4_TCP_FRAG_UNTAG, {MVPP2_PRS_RI_VLAN_NONE |
					   MVPP2_PRS_RI_L3_IP4_OPT |
					   MVPP2_PRS_RI_IP_FRAG_TRUE |
					   MVPP2_PRS_RI_L4_TCP,
					   MVPP2_PRS_RI_VLAN_MASK |
					   MVPP2_PRS_RI_L3_PROTO_MASK |
					   MVPP2_PRS_RI_IP_FRAG_MASK |
					   MVPP2_PRS_RI_L4_PROTO_MASK} },
	{MVPP2_PRS_FL_IP4_TCP_FRAG_UNTAG, {MVPP2_PRS_RI_VLAN_NONE |
					   MVPP2_PRS_RI_L3_IP4_OTHER |
					   MVPP2_PRS_RI_IP_FRAG_TRUE |
					   MVPP2_PRS_RI_L4_TCP,
					   MVPP2_PRS_RI_VLAN_MASK |
					   MVPP2_PRS_RI_L3_PROTO_MASK |
					   MVPP2_PRS_RI_IP_FRAG_MASK |
					   MVPP2_PRS_RI_L4_PROTO_MASK} },

	{MVPP2_PRS_FL_IP4_UDP_FRAG_UNTAG, {MVPP2_PRS_RI_VLAN_NONE |
					   MVPP2_PRS_RI_L3_IP4 |
					   MVPP2_PRS_RI_IP_FRAG_TRUE |
					   MVPP2_PRS_RI_L4_UDP,
					   MVPP2_PRS_RI_VLAN_MASK |
					   MVPP2_PRS_RI_L3_PROTO_MASK |
					   MVPP2_PRS_RI_IP_FRAG_MASK |
					   MVPP2_PRS_RI_L4_PROTO_MASK} },
	{MVPP2_PRS_FL_IP4_UDP_FRAG_UNTAG, {MVPP2_PRS_RI_VLAN_NONE |
					   MVPP2_PRS_RI_L3_IP4_OPT |
					   MVPP2_PRS_RI_IP_FRAG_TRUE |
					   MVPP2_PRS_RI_L4_UDP,
					   MVPP2_PRS_RI_VLAN_MASK |
					   MVPP2_PRS_RI_L3_PROTO_MASK |
					   MVPP2_PRS_RI_IP_FRAG_MASK |
					   MVPP2_PRS_RI_L4_PROTO_MASK} },
	{MVPP2_PRS_FL_IP4_UDP_FRAG_UNTAG, {MVPP2_PRS_RI_VLAN_NONE |
					   MVPP2_PRS_RI_L3_IP4_OTHER |
					   MVPP2_PRS_RI_IP_FRAG_TRUE |
					   MVPP2_PRS_RI_L4_UDP,
					   MVPP2_PRS_RI_VLAN_MASK |
					   MVPP2_PRS_RI_L3_PROTO_MASK |
					   MVPP2_PRS_RI_IP_FRAG_MASK |
					   MVPP2_PRS_RI_L4_PROTO_MASK} },

	{MVPP2_PRS_FL_IP4_TCP_FRAG_TAG, {MVPP2_PRS_RI_L3_IP4 |
					 MVPP2_PRS_RI_IP_FRAG_TRUE |
					 MVPP2_PRS_RI_L4_TCP,
					 MVPP2_PRS_RI_L3_PROTO_MASK |
					 MVPP2_PRS_RI_IP_FRAG_MASK |
					 MVPP2_PRS_RI_L4_PROTO_MASK} },
	{MVPP2_PRS_FL_IP4_TCP_FRAG_TAG, {MVPP2_PRS_RI_L3_IP4_OPT |
					 MVPP2_PRS_RI_IP_FRAG_TRUE |
					 MVPP2_PRS_RI_L4_TCP,
					 MVPP2_PRS_RI_L3_PROTO_MASK |
					 MVPP2_PRS_RI_IP_FRAG_MASK |
					 MVPP2_PRS_RI_L4_PROTO_MASK} },
	{MVPP2_PRS_FL_IP4_TCP_FRAG_TAG, {MVPP2_PRS_RI_L3_IP4_OTHER |
					 MVPP2_PRS_RI_IP_FRAG_TRUE |
					 MVPP2_PRS_RI_L4_TCP,
					 MVPP2_PRS_RI_L3_PROTO_MASK |
					 MVPP2_PRS_RI_IP_FRAG_MASK |
					 MVPP2_PRS_RI_L4_PROTO_MASK} },

	{MVPP2_PRS_FL_IP4_UDP_FRAG_TAG, {MVPP2_PRS_RI_L3_IP4 |
					 MVPP2_PRS_RI_IP_FRAG_TRUE |
					 MVPP2_PRS_RI_L4_UDP,
					 MVPP2_PRS_RI_L3_PROTO_MASK |
					 MVPP2_PRS_RI_IP_FRAG_MASK |
					 MVPP2_PRS_RI_L4_PROTO_MASK} },
	{MVPP2_PRS_FL_IP4_UDP_FRAG_TAG,	{MVPP2_PRS_RI_L3_IP4_OPT |
					 MVPP2_PRS_RI_IP_FRAG_TRUE |
					 MVPP2_PRS_RI_L4_UDP,
					 MVPP2_PRS_RI_L3_PROTO_MASK |
					 MVPP2_PRS_RI_IP_FRAG_MASK |
					 MVPP2_PRS_RI_L4_PROTO_MASK} },
	{MVPP2_PRS_FL_IP4_UDP_FRAG_TAG,	{MVPP2_PRS_RI_L3_IP4_OTHER |
					 MVPP2_PRS_RI_IP_FRAG_TRUE |
					 MVPP2_PRS_RI_L4_UDP,
					 MVPP2_PRS_RI_L3_PROTO_MASK |
					 MVPP2_PRS_RI_IP_FRAG_MASK |
					 MVPP2_PRS_RI_L4_PROTO_MASK} },

	{MVPP2_PRS_FL_IP6_TCP_FRAG_UNTAG, {MVPP2_PRS_RI_VLAN_NONE |
					   MVPP2_PRS_RI_L3_IP6 |
					   MVPP2_PRS_RI_IP_FRAG_TRUE |
					   MVPP2_PRS_RI_L4_TCP,
					   MVPP2_PRS_RI_VLAN_MASK |
					   MVPP2_PRS_RI_L3_PROTO_MASK |
					   MVPP2_PRS_RI_IP_FRAG_MASK |
					   MVPP2_PRS_RI_L4_PROTO_MASK} },
	{MVPP2_PRS_FL_IP6_TCP_FRAG_UNTAG, {MVPP2_PRS_RI_VLAN_NONE |
					   MVPP2_PRS_RI_L3_IP6_EXT |
					   MVPP2_PRS_RI_IP_FRAG_TRUE |
					   MVPP2_PRS_RI_L4_TCP,
					   MVPP2_PRS_RI_VLAN_MASK |
					   MVPP2_PRS_RI_L3_PROTO_MASK |
					   MVPP2_PRS_RI_IP_FRAG_MASK |
					   MVPP2_PRS_RI_L4_PROTO_MASK} },

	{MVPP2_PRS_FL_IP6_UDP_FRAG_UNTAG, {MVPP2_PRS_RI_VLAN_NONE |
					   MVPP2_PRS_RI_L3_IP6 |
					   MVPP2_PRS_RI_IP_FRAG_TRUE |
					   MVPP2_PRS_RI_L4_UDP,
					   MVPP2_PRS_RI_VLAN_MASK |
					   MVPP2_PRS_RI_L3_PROTO_MASK |
					   MVPP2_PRS_RI_IP_FRAG_MASK |
					   MVPP2_PRS_RI_L4_PROTO_MASK} },
	{MVPP2_PRS_FL_IP6_UDP_FRAG_UNTAG, {MVPP2_PRS_RI_VLAN_NONE |
					   MVPP2_PRS_RI_L3_IP6_EXT |
					   MVPP2_PRS_RI_IP_FRAG_TRUE |
					   MVPP2_PRS_RI_L4_UDP,
					   MVPP2_PRS_RI_VLAN_MASK |
					   MVPP2_PRS_RI_L3_PROTO_MASK |
					   MVPP2_PRS_RI_IP_FRAG_MASK |
					   MVPP2_PRS_RI_L4_PROTO_MASK} },

	{MVPP2_PRS_FL_IP6_TCP_FRAG_TAG,	{MVPP2_PRS_RI_L3_IP6 |
					 MVPP2_PRS_RI_IP_FRAG_TRUE |
					 MVPP2_PRS_RI_L4_TCP,
					 MVPP2_PRS_RI_L3_PROTO_MASK |
					 MVPP2_PRS_RI_IP_FRAG_MASK |
					 MVPP2_PRS_RI_L4_PROTO_MASK} },
	{MVPP2_PRS_FL_IP6_TCP_FRAG_TAG,	{MVPP2_PRS_RI_L3_IP6_EXT |
					 MVPP2_PRS_RI_IP_FRAG_TRUE |
					 MVPP2_PRS_RI_L4_TCP,
					 MVPP2_PRS_RI_L3_PROTO_MASK |
					 MVPP2_PRS_RI_IP_FRAG_MASK |
					 MVPP2_PRS_RI_L4_PROTO_MASK} },

	{MVPP2_PRS_FL_IP6_UDP_FRAG_TAG,	{MVPP2_PRS_RI_L3_IP6 |
					 MVPP2_PRS_RI_IP_FRAG_TRUE |
					 MVPP2_PRS_RI_L4_UDP,
					 MVPP2_PRS_RI_L3_PROTO_MASK |
					 MVPP2_PRS_RI_IP_FRAG_MASK |
					 MVPP2_PRS_RI_L4_PROTO_MASK} },
	{MVPP2_PRS_FL_IP6_UDP_FRAG_TAG, {MVPP2_PRS_RI_L3_IP6_EXT |
					 MVPP2_PRS_RI_IP_FRAG_TRUE |
					 MVPP2_PRS_RI_L4_UDP,
					 MVPP2_PRS_RI_L3_PROTO_MASK |
					 MVPP2_PRS_RI_IP_FRAG_MASK |
					 MVPP2_PRS_RI_L4_PROTO_MASK} },

	{MVPP2_PRS_FL_IP4_UNTAG, {MVPP2_PRS_RI_VLAN_NONE |
				  MVPP2_PRS_RI_L3_IP4,
				  MVPP2_PRS_RI_VLAN_MASK |
				  MVPP2_PRS_RI_L3_PROTO_MASK} },
	{MVPP2_PRS_FL_IP4_UNTAG, {MVPP2_PRS_RI_VLAN_NONE |
				  MVPP2_PRS_RI_L3_IP4_OPT,
				  MVPP2_PRS_RI_VLAN_MASK |
				  MVPP2_PRS_RI_L3_PROTO_MASK} },
	{MVPP2_PRS_FL_IP4_UNTAG, {MVPP2_PRS_RI_VLAN_NONE |
				  MVPP2_PRS_RI_L3_IP4_OTHER,
				  MVPP2_PRS_RI_VLAN_MASK |
				  MVPP2_PRS_RI_L3_PROTO_MASK} },

	{MVPP2_PRS_FL_IP4_TAG, {MVPP2_PRS_RI_L3_IP4,
				MVPP2_PRS_RI_L3_PROTO_MASK} },
	{MVPP2_PRS_FL_IP4_TAG, {MVPP2_PRS_RI_L3_IP4_OPT,
				MVPP2_PRS_RI_L3_PROTO_MASK} },
	{MVPP2_PRS_FL_IP4_TAG, {MVPP2_PRS_RI_L3_IP4_OTHER,
				MVPP2_PRS_RI_L3_PROTO_MASK} },

	{MVPP2_PRS_FL_IP6_UNTAG, {MVPP2_PRS_RI_VLAN_NONE |
				  MVPP2_PRS_RI_L3_IP6,
				  MVPP2_PRS_RI_VLAN_MASK |
				  MVPP2_PRS_RI_L3_PROTO_MASK} },
	{MVPP2_PRS_FL_IP6_UNTAG, {MVPP2_PRS_RI_VLAN_NONE |
				  MVPP2_PRS_RI_L3_IP6_EXT,
				  MVPP2_PRS_RI_VLAN_MASK |
				  MVPP2_PRS_RI_L3_PROTO_MASK} },

	{MVPP2_PRS_FL_IP6_TAG, {MVPP2_PRS_RI_L3_IP6,
				MVPP2_PRS_RI_L3_PROTO_MASK} },
	{MVPP2_PRS_FL_IP6_TAG, {MVPP2_PRS_RI_L3_IP6_EXT,
				MVPP2_PRS_RI_L3_PROTO_MASK} },

	{MVPP2_PRS_FL_NON_IP_UNTAG, {MVPP2_PRS_RI_VLAN_NONE,
				     MVPP2_PRS_RI_VLAN_MASK} },

	{MVPP2_PRS_FL_NON_IP_TAG, {0, 0} },
};

/* Array of bitmask to indicate flow id attribute */
static int mvpp2_prs_flow_id_attr_tbl[MVPP2_PRS_FL_LAST];

/* Parser configuration routines */
/* Update parser tcam and sram hw entries */
static int mvpp2_prs_hw_write(struct mvpp2 *priv, struct mvpp2_prs_entry *pe)
{
	int i;

	if (pe->index > MVPP2_PRS_TCAM_SRAM_SIZE - 1)
		return -EINVAL;

	/* Clear entry invalidation bit */
	pe->tcam.word[MVPP2_PRS_TCAM_INV_WORD] &= ~MVPP2_PRS_TCAM_INV_MASK;

	/* Write tcam index - indirect access */
	mvpp2_write(priv, MVPP2_PRS_TCAM_IDX_REG, pe->index);
	for (i = 0; i < MVPP2_PRS_TCAM_WORDS; i++)
		mvpp2_write(priv, MVPP2_PRS_TCAM_DATA_REG(i), pe->tcam.word[i]);

	/* Write sram index - indirect access */
	mvpp2_write(priv, MVPP2_PRS_SRAM_IDX_REG, pe->index);
	for (i = 0; i < MVPP2_PRS_SRAM_WORDS; i++)
		mvpp2_write(priv, MVPP2_PRS_SRAM_DATA_REG(i), pe->sram.word[i]);

	return 0;
}

/* Initialize tcam entry from hw */
static int mvpp2_prs_init_from_hw(struct mvpp2 *priv,
				  struct mvpp2_prs_entry *pe, int tid)
{
	int i;

	if (tid > MVPP2_PRS_TCAM_SRAM_SIZE - 1)
		return -EINVAL;

	memset(pe, 0, sizeof(*pe));
	pe->index = tid;

	/* Write tcam index - indirect access */
	mvpp2_write(priv, MVPP2_PRS_TCAM_IDX_REG, pe->index);

	pe->tcam.word[MVPP2_PRS_TCAM_INV_WORD] =
		mvpp2_read(priv,
			   MVPP2_PRS_TCAM_DATA_REG(MVPP2_PRS_TCAM_INV_WORD));

	if (pe->tcam.word[MVPP2_PRS_TCAM_INV_WORD] & MVPP2_PRS_TCAM_INV_MASK)
		return MVPP2_PRS_TCAM_ENTRY_INVALID;

	for (i = 0; i < MVPP2_PRS_TCAM_WORDS; i++)
		pe->tcam.word[i] = mvpp2_read(priv, MVPP2_PRS_TCAM_DATA_REG(i));

	/* Write sram index - indirect access */
	mvpp2_write(priv, MVPP2_PRS_SRAM_IDX_REG, pe->index);
	for (i = 0; i < MVPP2_PRS_SRAM_WORDS; i++)
		pe->sram.word[i] = mvpp2_read(priv, MVPP2_PRS_SRAM_DATA_REG(i));

	return 0;
}

/* Invalidate tcam hw entry */
static void mvpp2_prs_hw_inv(struct mvpp2 *priv, int index)
{
	/* Write index - indirect access */
	mvpp2_write(priv, MVPP2_PRS_TCAM_IDX_REG, index);
	mvpp2_write(priv, MVPP2_PRS_TCAM_DATA_REG(MVPP2_PRS_TCAM_INV_WORD),
		    MVPP2_PRS_TCAM_INV_MASK);
}

/* Enable shadow table entry and set its lookup ID */
static void mvpp2_prs_shadow_set(struct mvpp2 *priv, int index, int lu)
{
	priv->prs_shadow[index].valid = true;
	priv->prs_shadow[index].lu = lu;
}

/* Update ri fields in shadow table entry */
static void mvpp2_prs_shadow_ri_set(struct mvpp2 *priv, int index,
				    unsigned int ri, unsigned int ri_mask)
{
	priv->prs_shadow[index].ri_mask = ri_mask;
	priv->prs_shadow[index].ri = ri;
}

/* Update lookup field in tcam sw entry */
static void mvpp2_prs_tcam_lu_set(struct mvpp2_prs_entry *pe, unsigned int lu)
{
	const int enable_off = MVPP2_PRS_TCAM_EN_OFFS(MVPP2_PRS_TCAM_LU_BYTE);

	pe->tcam.byte[mvpp2_offs_endian(MVPP2_PRS_TCAM_LU_BYTE)] = lu;
	pe->tcam.byte[mvpp2_offs_endian(enable_off)] = MVPP2_PRS_LU_MASK;
}

/* Update mask for single port in tcam sw entry */
static void mvpp2_prs_tcam_port_set(struct mvpp2_prs_entry *pe,
				    unsigned int port, bool add)
{
	const int enable_off = MVPP2_PRS_TCAM_EN_OFFS(MVPP2_PRS_TCAM_PORT_BYTE);

	if (add)
		pe->tcam.byte[mvpp2_offs_endian(enable_off)] &= ~(1 << port);
	else
		pe->tcam.byte[mvpp2_offs_endian(enable_off)] |= 1 << port;
}

/* Update port map in tcam sw entry */
static void mvpp2_prs_tcam_port_map_set(struct mvpp2_prs_entry *pe,
					unsigned int ports)
{
	const int enable_off = MVPP2_PRS_TCAM_EN_OFFS(MVPP2_PRS_TCAM_PORT_BYTE);

	pe->tcam.byte[mvpp2_offs_endian(MVPP2_PRS_TCAM_PORT_BYTE)] = 0;
	pe->tcam.byte[mvpp2_offs_endian(enable_off)] &=
						(u8)(~MVPP2_PRS_PORT_MASK);
	pe->tcam.byte[mvpp2_offs_endian(enable_off)] |=
						~ports & MVPP2_PRS_PORT_MASK;
}

/* Obtain port map from tcam sw entry */
static unsigned int mvpp2_prs_tcam_port_map_get(struct mvpp2_prs_entry *pe)
{
	const int enable_off =
			MVPP2_PRS_TCAM_EN_OFFS(MVPP2_PRS_TCAM_PORT_BYTE);
	int endian_enable_off = mvpp2_offs_endian(enable_off);

	return ~pe->tcam.byte[endian_enable_off] & MVPP2_PRS_PORT_MASK;
}

/* Set byte of data and its enable bits in tcam sw entry */
static void mvpp2_prs_tcam_data_byte_set(struct mvpp2_prs_entry *pe,
					 unsigned int offs, unsigned char byte,
					 unsigned char enable)
{
	pe->tcam.byte[MVPP2_PRS_TCAM_DATA_BYTE(offs)] = byte;
	pe->tcam.byte[MVPP2_PRS_TCAM_DATA_BYTE_EN(offs)] = enable;
}

/* Get byte of data and its enable bits from tcam sw entry */
static void mvpp2_prs_tcam_data_byte_get(struct mvpp2_prs_entry *pe,
					 unsigned int offs, unsigned char *byte,
					 unsigned char *enable)
{
	*byte = pe->tcam.byte[MVPP2_PRS_TCAM_DATA_BYTE(offs)];
	*enable = pe->tcam.byte[MVPP2_PRS_TCAM_DATA_BYTE_EN(offs)];
}

/* Compare tcam data bytes with a pattern */
static bool mvpp2_prs_tcam_data_cmp(struct mvpp2_prs_entry *pe, int offs,
				    u16 data)
{
	u16 tcam_data;

	tcam_data = pe->tcam.byte[MVPP2_PRS_TCAM_DATA_BYTE(offs)] << 8;
	tcam_data |= pe->tcam.byte[MVPP2_PRS_TCAM_DATA_BYTE(offs + 1)];
	return (tcam_data == data);
}

/* Update ai bits in tcam sw entry */
static void mvpp2_prs_tcam_ai_update(struct mvpp2_prs_entry *pe,
				     unsigned int bits, unsigned int enable)
{
	const int ai_idx = MVPP2_PRS_TCAM_AI_BYTE;
	int i, offs;

	for (i = 0; i < MVPP2_PRS_AI_BITS; i++) {
		if (!(enable & BIT(i)))
			continue;

		offs = mvpp2_offs_endian(ai_idx);
		if (bits & BIT(i))
			pe->tcam.byte[offs] |= 1 << i;
		else
			pe->tcam.byte[offs] &= ~(1 << i);
	}
	offs = mvpp2_offs_endian(MVPP2_PRS_TCAM_EN_OFFS(ai_idx));
	pe->tcam.byte[offs] |= enable;
}

/* Get ai bits from tcam sw entry */
static int mvpp2_prs_tcam_ai_get(struct mvpp2_prs_entry *pe)
{
	return pe->tcam.byte[mvpp2_offs_endian(MVPP2_PRS_TCAM_AI_BYTE)];
}

/* Set ethertype in tcam sw entry */
static void mvpp2_prs_match_etype(struct mvpp2_prs_entry *pe, int offset,
				  unsigned short ethertype)
{
	mvpp2_prs_tcam_data_byte_set(pe, offset + 0, ethertype >> 8, 0xff);
	mvpp2_prs_tcam_data_byte_set(pe, offset + 1, ethertype & 0xff, 0xff);
}

/* Set vid in tcam sw entry */
static void mvpp2_prs_match_vid(struct mvpp2_prs_entry *pe, int offset,
				unsigned short vid)
{
	mvpp2_prs_tcam_data_byte_set(pe, offset + 0, (vid & 0xf00) >> 8, 0xf);
	mvpp2_prs_tcam_data_byte_set(pe, offset + 1, vid & 0xff, 0xff);
}

/* Set bits in sram sw entry */
static void mvpp2_prs_sram_bits_set(struct mvpp2_prs_entry *pe, int bit_num,
				    int val)
{
	pe->sram.byte[MVPP2_BIT_TO_BYTE(bit_num)] |= (val << (bit_num % 8));
}

/* Clear bits in sram sw entry */
static void mvpp2_prs_sram_bits_clear(struct mvpp2_prs_entry *pe, int bit_num,
				      int val)
{
	pe->sram.byte[MVPP2_BIT_TO_BYTE(bit_num)] &= ~(val << (bit_num % 8));
}

/* Set dword of data and its enable bits in tcam sw entry */
static void mvpp2_prs_tcam_data_dword_set(struct mvpp2_prs_entry *pe,
					  unsigned int offs,
					  unsigned int word,
					  unsigned int enable)
{
	int index, offset;
	unsigned char byte, byte_mask;

	for (index = 0; index < 4; index++) {
		offset = (offs * 4) + index;
		byte = ((unsigned char *)&word)[index];
		byte_mask = ((unsigned char *)&enable)[index];
		mvpp2_prs_tcam_data_byte_set(pe, offset, byte, byte_mask);
	}
}

/* Get dword of data and its enable bits from tcam sw entry */
static void mvpp2_prs_tcam_data_dword_get(struct mvpp2_prs_entry *pe,
					  unsigned int offs,
					  unsigned int *word,
					  unsigned int *enable)
{
	int index, offset;
	unsigned char byte, mask;

	for (index = 0; index < 4; index++) {
		offset = (offs * 4) + index;
		mvpp2_prs_tcam_data_byte_get(pe, offset,  &byte, &mask);
		((unsigned char *)word)[index] = byte;
		((unsigned char *)enable)[index] = mask;
	}
}

/* Update ri bits in sram sw entry */
static void mvpp2_prs_sram_ri_update(struct mvpp2_prs_entry *pe,
				     unsigned int bits, unsigned int mask)
{
	unsigned int i;

	for (i = 0; i < MVPP2_PRS_SRAM_RI_CTRL_BITS; i++) {
		int ri_off = MVPP2_PRS_SRAM_RI_OFFS;

		if (!(mask & BIT(i)))
			continue;

		if (bits & BIT(i))
			mvpp2_prs_sram_bits_set(pe, ri_off + i, 1);
		else
			mvpp2_prs_sram_bits_clear(pe, ri_off + i, 1);

		mvpp2_prs_sram_bits_set(pe, MVPP2_PRS_SRAM_RI_CTRL_OFFS + i, 1);
	}
}

/* Obtain ri bits from sram sw entry */
static int mvpp2_prs_sram_ri_get(struct mvpp2_prs_entry *pe)
{
	return pe->sram.word[MVPP2_PRS_SRAM_RI_WORD];
}

/* Update ai bits in sram sw entry */
static void mvpp2_prs_sram_ai_update(struct mvpp2_prs_entry *pe,
				     unsigned int bits, unsigned int mask)
{
	unsigned int i;
	int ai_off = MVPP2_PRS_SRAM_AI_OFFS;

	for (i = 0; i < MVPP2_PRS_SRAM_AI_CTRL_BITS; i++) {
		if (!(mask & BIT(i)))
			continue;

		if (bits & BIT(i))
			mvpp2_prs_sram_bits_set(pe, ai_off + i, 1);
		else
			mvpp2_prs_sram_bits_clear(pe, ai_off + i, 1);

		mvpp2_prs_sram_bits_set(pe, MVPP2_PRS_SRAM_AI_CTRL_OFFS + i, 1);
	}
}

/* Read ai bits from sram sw entry */
static int mvpp2_prs_sram_ai_get(struct mvpp2_prs_entry *pe)
{
	u8 bits;
	int ai_off = MVPP2_BIT_TO_BYTE(MVPP2_PRS_SRAM_AI_OFFS);
	int ai_en_off = ai_off + 1;
	int ai_shift = MVPP2_PRS_SRAM_AI_OFFS % 8;

	bits = (pe->sram.byte[ai_off] >> ai_shift) |
	       (pe->sram.byte[ai_en_off] << (8 - ai_shift));

	return bits;
}

/* In sram sw entry set lookup ID field of the tcam key to be used in the next
 * lookup interation
 */
static void mvpp2_prs_sram_next_lu_set(struct mvpp2_prs_entry *pe,
				       unsigned int lu)
{
	int sram_next_off = MVPP2_PRS_SRAM_NEXT_LU_OFFS;

	mvpp2_prs_sram_bits_clear(pe, sram_next_off,
				  MVPP2_PRS_SRAM_NEXT_LU_MASK);
	mvpp2_prs_sram_bits_set(pe, sram_next_off, lu);
}

/* In the sram sw entry set sign and value of the next lookup offset
 * and the offset value generated to the classifier
 */
static void mvpp2_prs_sram_shift_set(struct mvpp2_prs_entry *pe, int shift,
				     unsigned int op)
{
	/* Set sign */
	if (shift < 0) {
		mvpp2_prs_sram_bits_set(pe, MVPP2_PRS_SRAM_SHIFT_SIGN_BIT, 1);
		shift = 0 - shift;
	} else {
		mvpp2_prs_sram_bits_clear(pe, MVPP2_PRS_SRAM_SHIFT_SIGN_BIT, 1);
	}

	/* Set value */
	pe->sram.byte[MVPP2_BIT_TO_BYTE(MVPP2_PRS_SRAM_SHIFT_OFFS)] =
							   (unsigned char)shift;

	/* Reset and set operation */
	mvpp2_prs_sram_bits_clear(pe, MVPP2_PRS_SRAM_OP_SEL_SHIFT_OFFS,
				  MVPP2_PRS_SRAM_OP_SEL_SHIFT_MASK);
	mvpp2_prs_sram_bits_set(pe, MVPP2_PRS_SRAM_OP_SEL_SHIFT_OFFS, op);

	/* Set base offset as current */
	mvpp2_prs_sram_bits_clear(pe, MVPP2_PRS_SRAM_OP_SEL_BASE_OFFS, 1);
}

/* In the sram sw entry set sign and value of the user defined offset
 * generated to the classifier
 */
static void mvpp2_prs_sram_offset_set(struct mvpp2_prs_entry *pe,
				      unsigned int type, int offset,
				      unsigned int op)
{
	/* Set sign */
	if (offset < 0) {
		mvpp2_prs_sram_bits_set(pe, MVPP2_PRS_SRAM_UDF_SIGN_BIT, 1);
		offset = 0 - offset;
	} else {
		mvpp2_prs_sram_bits_clear(pe, MVPP2_PRS_SRAM_UDF_SIGN_BIT, 1);
	}

	/* Set value */
	mvpp2_prs_sram_bits_clear(pe, MVPP2_PRS_SRAM_UDF_OFFS,
				  MVPP2_PRS_SRAM_UDF_MASK);
	mvpp2_prs_sram_bits_set(pe, MVPP2_PRS_SRAM_UDF_OFFS, offset);
	pe->sram.byte[MVPP2_BIT_TO_BYTE(MVPP2_PRS_SRAM_UDF_OFFS +
					MVPP2_PRS_SRAM_UDF_BITS)] &=
	      ~(MVPP2_PRS_SRAM_UDF_MASK >> (8 - (MVPP2_PRS_SRAM_UDF_OFFS % 8)));
	pe->sram.byte[MVPP2_BIT_TO_BYTE(MVPP2_PRS_SRAM_UDF_OFFS +
					MVPP2_PRS_SRAM_UDF_BITS)] |=
				(offset >> (8 - (MVPP2_PRS_SRAM_UDF_OFFS % 8)));

	/* Set offset type */
	mvpp2_prs_sram_bits_clear(pe, MVPP2_PRS_SRAM_UDF_TYPE_OFFS,
				  MVPP2_PRS_SRAM_UDF_TYPE_MASK);
	mvpp2_prs_sram_bits_set(pe, MVPP2_PRS_SRAM_UDF_TYPE_OFFS, type);

	/* Set offset operation */
	mvpp2_prs_sram_bits_clear(pe, MVPP2_PRS_SRAM_OP_SEL_UDF_OFFS,
				  MVPP2_PRS_SRAM_OP_SEL_UDF_MASK);
	mvpp2_prs_sram_bits_set(pe, MVPP2_PRS_SRAM_OP_SEL_UDF_OFFS, op);

	pe->sram.byte[MVPP2_BIT_TO_BYTE(MVPP2_PRS_SRAM_OP_SEL_UDF_OFFS +
					MVPP2_PRS_SRAM_OP_SEL_UDF_BITS)] &=
					     ~(MVPP2_PRS_SRAM_OP_SEL_UDF_MASK >>
				    (8 - (MVPP2_PRS_SRAM_OP_SEL_UDF_OFFS % 8)));

	pe->sram.byte[MVPP2_BIT_TO_BYTE(MVPP2_PRS_SRAM_OP_SEL_UDF_OFFS +
					MVPP2_PRS_SRAM_OP_SEL_UDF_BITS)] |=
			     (op >> (8 - (MVPP2_PRS_SRAM_OP_SEL_UDF_OFFS % 8)));

	/* Set base offset as current */
	mvpp2_prs_sram_bits_clear(pe, MVPP2_PRS_SRAM_OP_SEL_BASE_OFFS, 1);
}

/* Find parser flow entry */
static int mvpp2_prs_flow_find(struct mvpp2 *priv, int flow,
			       u32 ri, u32 ri_mask)
{
	struct mvpp2_prs_entry pe;
	int tid;
	unsigned int dword, enable;

	/* Go through the all entires with MVPP2_PRS_LU_FLOWS */
	for (tid = MVPP2_PRS_TCAM_SRAM_SIZE - 1; tid >= 0; tid--) {
		u8 bits;

		if (!priv->prs_shadow[tid].valid ||
		    priv->prs_shadow[tid].lu != MVPP2_PRS_LU_FLOWS)
			continue;

		mvpp2_prs_init_from_hw(priv, &pe, tid);

		/* Check result info, because there maybe several
		 * TCAM lines to generate the same flow
		 */
		mvpp2_prs_tcam_data_dword_get(&pe, 0, &dword, &enable);
		if (dword != ri || enable != ri_mask)
			continue;

		bits = mvpp2_prs_sram_ai_get(&pe);

		/* Sram store classification lookup ID in AI bits [5:0] */
		if ((bits & MVPP2_PRS_FLOW_ID_MASK) == flow)
			return tid;
	}

	return -ENOENT;
}

/* Return first free tcam index, seeking from start to end */
static int mvpp2_prs_tcam_first_free(struct mvpp2 *priv, unsigned char start,
				     unsigned char end)
{
	int tid;

	if (start > end)
		swap(start, end);

	if (end >= MVPP2_PRS_TCAM_SRAM_SIZE)
		end = MVPP2_PRS_TCAM_SRAM_SIZE - 1;

	for (tid = start; tid <= end; tid++) {
		if (!priv->prs_shadow[tid].valid)
			return tid;
	}

	return -EINVAL;
}

/* Enable/disable dropping all mac da's */
static void mvpp2_prs_mac_drop_all_set(struct mvpp2 *priv, int port, bool add)
{
	struct mvpp2_prs_entry pe;

	if (priv->prs_shadow[MVPP2_PE_DROP_ALL].valid) {
		/* Entry exist - update port only */
		mvpp2_prs_init_from_hw(priv, &pe, MVPP2_PE_DROP_ALL);
	} else {
		/* Entry doesn't exist - create new */
		memset(&pe, 0, sizeof(pe));
		mvpp2_prs_tcam_lu_set(&pe, MVPP2_PRS_LU_MAC);
		pe.index = MVPP2_PE_DROP_ALL;

		/* Non-promiscuous mode for all ports - DROP unknown packets */
		mvpp2_prs_sram_ri_update(&pe, MVPP2_PRS_RI_DROP_MASK,
					 MVPP2_PRS_RI_DROP_MASK);

		mvpp2_prs_sram_bits_set(&pe, MVPP2_PRS_SRAM_LU_GEN_BIT, 1);
		mvpp2_prs_sram_next_lu_set(&pe, MVPP2_PRS_LU_FLOWS);

		/* Update shadow table */
		mvpp2_prs_shadow_set(priv, pe.index, MVPP2_PRS_LU_MAC);

		/* Mask all ports */
		mvpp2_prs_tcam_port_map_set(&pe, 0);
	}

	/* Update port mask */
	mvpp2_prs_tcam_port_set(&pe, port, add);

	mvpp2_prs_hw_write(priv, &pe);
}

/* Set port to unicast or multicast promiscuous mode */
static void mvpp2_prs_mac_promisc_set(struct mvpp2 *priv, int port,
				      enum mvpp2_prs_l2_cast l2_cast, bool add)
{
	struct mvpp2_prs_entry pe;
	unsigned char cast_match;
	unsigned int ri;
	int tid;

	if (l2_cast == MVPP2_PRS_L2_UNI_CAST) {
		cast_match = MVPP2_PRS_UCAST_VAL;
		tid = MVPP2_PE_MAC_UC_PROMISCUOUS;
		ri = MVPP2_PRS_RI_L2_UCAST;
	} else {
		cast_match = MVPP2_PRS_MCAST_VAL;
		tid = MVPP2_PE_MAC_MC_PROMISCUOUS;
		ri = MVPP2_PRS_RI_L2_MCAST;
	}

	/* promiscuous mode - Accept unknown unicast or multicast packets */
	if (priv->prs_shadow[tid].valid) {
		mvpp2_prs_init_from_hw(priv, &pe, tid);
	} else {
		memset(&pe, 0, sizeof(pe));
		mvpp2_prs_tcam_lu_set(&pe, MVPP2_PRS_LU_MAC);
		pe.index = tid;

		/* Continue - set next lookup */
		mvpp2_prs_sram_next_lu_set(&pe, MVPP2_PRS_LU_DSA);

		/* Set result info bits */
		mvpp2_prs_sram_ri_update(&pe, ri, MVPP2_PRS_RI_L2_CAST_MASK);

		/* Match UC or MC addresses */
		mvpp2_prs_tcam_data_byte_set(&pe, 0, cast_match,
					     MVPP2_PRS_CAST_MASK);

		/* Shift to ethertype */
		mvpp2_prs_sram_shift_set(&pe, 2 * ETH_ALEN,
					 MVPP2_PRS_SRAM_OP_SEL_SHIFT_ADD);

		/* Mask all ports */
		mvpp2_prs_tcam_port_map_set(&pe, 0);

		/* Update shadow table */
		mvpp2_prs_shadow_set(priv, pe.index, MVPP2_PRS_LU_MAC);
	}

	/* Update port mask */
	mvpp2_prs_tcam_port_set(&pe, port, add);

	mvpp2_prs_hw_write(priv, &pe);
}

/* Set entry for dsa packets */
static void mvpp2_prs_dsa_tag_set(struct mvpp2 *priv, int port, bool add,
				  bool tagged, bool extend)
{
	struct mvpp2_prs_entry pe;
	int tid, shift;

	if (extend) {
		tid = tagged ? MVPP2_PE_EDSA_TAGGED : MVPP2_PE_EDSA_UNTAGGED;
		shift = 8;
	} else {
		tid = tagged ? MVPP2_PE_DSA_TAGGED : MVPP2_PE_DSA_UNTAGGED;
		shift = 4;
	}

	if (priv->prs_shadow[tid].valid) {
		/* Entry exist - update port only */
		mvpp2_prs_init_from_hw(priv, &pe, tid);
	} else {
		/* Entry doesn't exist - create new */
		memset(&pe, 0, sizeof(pe));
		mvpp2_prs_tcam_lu_set(&pe, MVPP2_PRS_LU_DSA);
		pe.index = tid;

		/* Update shadow table */
		mvpp2_prs_shadow_set(priv, pe.index, MVPP2_PRS_LU_DSA);

		if (tagged) {
			/* Set tagged bit in DSA tag */
			mvpp2_prs_tcam_data_byte_set(&pe, 0,
						     MVPP2_PRS_TCAM_DSA_TAGGED_BIT,
						     MVPP2_PRS_TCAM_DSA_TAGGED_BIT);

			/* Set ai bits for next iteration */
			mvpp2_prs_sram_ai_update(&pe, extend,
						 MVPP2_PRS_SRAM_AI_MASK);

			/* Set result info bits to 'single vlan' */
			mvpp2_prs_sram_ri_update(&pe, MVPP2_PRS_RI_VLAN_SINGLE,
						 MVPP2_PRS_RI_VLAN_MASK);

			/* If packet is tagged continue check vid filtering */
			mvpp2_prs_sram_next_lu_set(&pe, MVPP2_PRS_LU_VID);
		} else {
			/* Shift 4 bytes for DSA tag or 8 bytes for EDSA tag*/
			mvpp2_prs_sram_shift_set(&pe, shift,
						 MVPP2_PRS_SRAM_OP_SEL_SHIFT_ADD);

			/* Set result info bits to 'no vlans' */
			mvpp2_prs_sram_ri_update(&pe, MVPP2_PRS_RI_VLAN_NONE,
						 MVPP2_PRS_RI_VLAN_MASK);
			mvpp2_prs_sram_next_lu_set(&pe, MVPP2_PRS_LU_L2);
		}

		/* Mask all ports */
		mvpp2_prs_tcam_port_map_set(&pe, 0);
	}

	/* Update port mask */
	mvpp2_prs_tcam_port_set(&pe, port, add);

	mvpp2_prs_hw_write(priv, &pe);
}

/* Set entry for dsa ethertype */
static void mvpp2_prs_dsa_tag_ethertype_set(struct mvpp2 *priv, int port,
					    bool add, bool tagged, bool extend)
{
	struct mvpp2_prs_entry pe;
	int tid, shift, port_mask;

	if (extend) {
		tid = tagged ? MVPP2_PE_ETYPE_EDSA_TAGGED :
		      MVPP2_PE_ETYPE_EDSA_UNTAGGED;
		port_mask = 0;
		shift = 8;
	} else {
		tid = tagged ? MVPP2_PE_ETYPE_DSA_TAGGED :
		      MVPP2_PE_ETYPE_DSA_UNTAGGED;
		port_mask = MVPP2_PRS_PORT_MASK;
		shift = 4;
	}

	if (priv->prs_shadow[tid].valid) {
		/* Entry exist - update port only */
		mvpp2_prs_init_from_hw(priv, &pe, tid);
	} else {
		/* Entry doesn't exist - create new */
		memset(&pe, 0, sizeof(pe));
		mvpp2_prs_tcam_lu_set(&pe, MVPP2_PRS_LU_DSA);
		pe.index = tid;

		/* Set ethertype */
		mvpp2_prs_match_etype(&pe, 0, ETH_P_EDSA);
		mvpp2_prs_match_etype(&pe, 2, 0);

		mvpp2_prs_sram_ri_update(&pe, MVPP2_PRS_RI_DSA_MASK,
					 MVPP2_PRS_RI_DSA_MASK);
		/* Shift ethertype + 2 byte reserved + tag*/
		mvpp2_prs_sram_shift_set(&pe, 2 + MVPP2_ETH_TYPE_LEN + shift,
					 MVPP2_PRS_SRAM_OP_SEL_SHIFT_ADD);

		/* Update shadow table */
		mvpp2_prs_shadow_set(priv, pe.index, MVPP2_PRS_LU_DSA);

		if (tagged) {
			/* Set tagged bit in DSA tag */
			mvpp2_prs_tcam_data_byte_set(&pe,
						     MVPP2_ETH_TYPE_LEN + 2 + 3,
						 MVPP2_PRS_TCAM_DSA_TAGGED_BIT,
						 MVPP2_PRS_TCAM_DSA_TAGGED_BIT);
			/* Clear all ai bits for next iteration */
			mvpp2_prs_sram_ai_update(&pe, 0,
						 MVPP2_PRS_SRAM_AI_MASK);
			/* If packet is tagged continue check vlans */
			mvpp2_prs_sram_next_lu_set(&pe, MVPP2_PRS_LU_VLAN);
		} else {
			/* Set result info bits to 'no vlans' */
			mvpp2_prs_sram_ri_update(&pe, MVPP2_PRS_RI_VLAN_NONE,
						 MVPP2_PRS_RI_VLAN_MASK);
			mvpp2_prs_sram_next_lu_set(&pe, MVPP2_PRS_LU_L2);
		}
		/* Mask/unmask all ports, depending on dsa type */
		mvpp2_prs_tcam_port_map_set(&pe, port_mask);
	}

	/* Update port mask */
	mvpp2_prs_tcam_port_set(&pe, port, add);

	mvpp2_prs_hw_write(priv, &pe);
}

/* Search for existing single/triple vlan entry */
static int mvpp2_prs_vlan_find(struct mvpp2 *priv, unsigned short tpid, int ai)
{
	struct mvpp2_prs_entry pe;
	int tid;

	/* Go through the all entries with MVPP2_PRS_LU_VLAN */
	for (tid = MVPP2_PE_FIRST_FREE_TID;
	     tid <= MVPP2_PE_LAST_FREE_TID; tid++) {
		unsigned int ri_bits, ai_bits;
		bool match;

		if (!priv->prs_shadow[tid].valid ||
		    priv->prs_shadow[tid].lu != MVPP2_PRS_LU_VLAN)
			continue;

		mvpp2_prs_init_from_hw(priv, &pe, tid);
		match = mvpp2_prs_tcam_data_cmp(&pe, 0, tpid);
		if (!match)
			continue;

		/* Get vlan type */
		ri_bits = mvpp2_prs_sram_ri_get(&pe);
		ri_bits &= MVPP2_PRS_RI_VLAN_MASK;

		/* Get current ai value from tcam */
		ai_bits = mvpp2_prs_tcam_ai_get(&pe);
		/* Clear double vlan bit */
		ai_bits &= ~MVPP2_PRS_DBL_VLAN_AI_BIT;

		if (ai != ai_bits)
			continue;

		if (ri_bits == MVPP2_PRS_RI_VLAN_SINGLE ||
		    ri_bits == MVPP2_PRS_RI_VLAN_TRIPLE)
			return tid;
	}

	return -ENOENT;
}

/* Add/update single/triple vlan entry */
static int mvpp2_prs_vlan_add(struct mvpp2 *priv, unsigned short tpid, int ai,
			      unsigned int port_map)
{
	struct mvpp2_prs_entry pe;
	int tid_aux, tid;
	int ret = 0;

	memset(&pe, 0, sizeof(pe));

	tid = mvpp2_prs_vlan_find(priv, tpid, ai);

	if (tid < 0) {
		/* Create new tcam entry */
		tid = mvpp2_prs_tcam_first_free(priv, MVPP2_PE_LAST_FREE_TID,
						MVPP2_PE_FIRST_FREE_TID);
		if (tid < 0)
			return tid;

		/* Get last double vlan tid */
		for (tid_aux = MVPP2_PE_LAST_FREE_TID;
		     tid_aux >= MVPP2_PE_FIRST_FREE_TID; tid_aux--) {
			unsigned int ri_bits;

			if (!priv->prs_shadow[tid_aux].valid ||
			    priv->prs_shadow[tid_aux].lu != MVPP2_PRS_LU_VLAN)
				continue;

			mvpp2_prs_init_from_hw(priv, &pe, tid_aux);
			ri_bits = mvpp2_prs_sram_ri_get(&pe);
			if ((ri_bits & MVPP2_PRS_RI_VLAN_MASK) ==
			    MVPP2_PRS_RI_VLAN_DOUBLE)
				break;
		}

		if (tid <= tid_aux)
			return -EINVAL;

		memset(&pe, 0, sizeof(pe));
		pe.index = tid;
		mvpp2_prs_tcam_lu_set(&pe, MVPP2_PRS_LU_VLAN);

		mvpp2_prs_match_etype(&pe, 0, tpid);

		/* VLAN tag detected, proceed with VID filtering */
		mvpp2_prs_sram_next_lu_set(&pe, MVPP2_PRS_LU_VID);

		/* Clear all ai bits for next iteration */
		mvpp2_prs_sram_ai_update(&pe, 0, MVPP2_PRS_SRAM_AI_MASK);

		if (ai == MVPP2_PRS_SINGLE_VLAN_AI) {
			mvpp2_prs_sram_ri_update(&pe, MVPP2_PRS_RI_VLAN_SINGLE,
						 MVPP2_PRS_RI_VLAN_MASK);
		} else {
			ai |= MVPP2_PRS_DBL_VLAN_AI_BIT;
			mvpp2_prs_sram_ri_update(&pe, MVPP2_PRS_RI_VLAN_TRIPLE,
						 MVPP2_PRS_RI_VLAN_MASK);
		}
		mvpp2_prs_tcam_ai_update(&pe, ai, MVPP2_PRS_SRAM_AI_MASK);

		mvpp2_prs_shadow_set(priv, pe.index, MVPP2_PRS_LU_VLAN);
	} else {
		mvpp2_prs_init_from_hw(priv, &pe, tid);
	}
	/* Update ports' mask */
	mvpp2_prs_tcam_port_map_set(&pe, port_map);

	mvpp2_prs_hw_write(priv, &pe);

	return ret;
}

/* Get first free double vlan ai number */
static int mvpp2_prs_double_vlan_ai_free_get(struct mvpp2 *priv)
{
	int i;

	for (i = 1; i < MVPP2_PRS_DBL_VLANS_MAX; i++) {
		if (!priv->prs_double_vlans[i])
			return i;
	}

	return -EINVAL;
}

/* Search for existing double vlan entry */
static int mvpp2_prs_double_vlan_find(struct mvpp2 *priv, unsigned short tpid1,
				      unsigned short tpid2)
{
	struct mvpp2_prs_entry pe;
	int tid;

	/* Go through the all entries with MVPP2_PRS_LU_VLAN */
	for (tid = MVPP2_PE_FIRST_FREE_TID;
	     tid <= MVPP2_PE_LAST_FREE_TID; tid++) {
		unsigned int ri_mask;
		bool match;

		if (!priv->prs_shadow[tid].valid ||
		    priv->prs_shadow[tid].lu != MVPP2_PRS_LU_VLAN)
			continue;

		mvpp2_prs_init_from_hw(priv, &pe, tid);

		match = mvpp2_prs_tcam_data_cmp(&pe, 0, tpid1) &&
			mvpp2_prs_tcam_data_cmp(&pe, 4, tpid2);

		if (!match)
			continue;

		ri_mask = mvpp2_prs_sram_ri_get(&pe) & MVPP2_PRS_RI_VLAN_MASK;
		if (ri_mask == MVPP2_PRS_RI_VLAN_DOUBLE)
			return tid;
	}

	return -ENOENT;
}

/* Add or update double vlan entry */
static int mvpp2_prs_double_vlan_add(struct mvpp2 *priv, unsigned short tpid1,
				     unsigned short tpid2,
				     unsigned int port_map)
{
	int tid_aux, tid, ai, ret = 0;
	struct mvpp2_prs_entry pe;

	memset(&pe, 0, sizeof(pe));

	tid = mvpp2_prs_double_vlan_find(priv, tpid1, tpid2);

	if (tid < 0) {
		/* Create new tcam entry */
		tid = mvpp2_prs_tcam_first_free(priv, MVPP2_PE_FIRST_FREE_TID,
						MVPP2_PE_LAST_FREE_TID);
		if (tid < 0)
			return tid;

		/* Set ai value for new double vlan entry */
		ai = mvpp2_prs_double_vlan_ai_free_get(priv);
		if (ai < 0)
			return ai;

		/* Get first single/triple vlan tid */
		for (tid_aux = MVPP2_PE_FIRST_FREE_TID;
		     tid_aux <= MVPP2_PE_LAST_FREE_TID; tid_aux++) {
			unsigned int ri_bits;

			if (!priv->prs_shadow[tid_aux].valid ||
			    priv->prs_shadow[tid_aux].lu != MVPP2_PRS_LU_VLAN)
				continue;

			mvpp2_prs_init_from_hw(priv, &pe, tid_aux);
			ri_bits = mvpp2_prs_sram_ri_get(&pe);
			ri_bits &= MVPP2_PRS_RI_VLAN_MASK;
			if (ri_bits == MVPP2_PRS_RI_VLAN_SINGLE ||
			    ri_bits == MVPP2_PRS_RI_VLAN_TRIPLE)
				break;
		}

		if (tid >= tid_aux)
			return -ERANGE;

		memset(&pe, 0, sizeof(pe));
		mvpp2_prs_tcam_lu_set(&pe, MVPP2_PRS_LU_VLAN);
		pe.index = tid;

		priv->prs_double_vlans[ai] = true;

		mvpp2_prs_match_etype(&pe, 0, tpid1);
		mvpp2_prs_match_etype(&pe, 4, tpid2);

		mvpp2_prs_sram_next_lu_set(&pe, MVPP2_PRS_LU_VLAN);
		/* Shift 4 bytes - skip outer vlan tag */
		mvpp2_prs_sram_shift_set(&pe, MVPP2_VLAN_TAG_LEN,
					 MVPP2_PRS_SRAM_OP_SEL_SHIFT_ADD);
		mvpp2_prs_sram_ri_update(&pe, MVPP2_PRS_RI_VLAN_DOUBLE,
					 MVPP2_PRS_RI_VLAN_MASK);
		mvpp2_prs_sram_ai_update(&pe, ai | MVPP2_PRS_DBL_VLAN_AI_BIT,
					 MVPP2_PRS_SRAM_AI_MASK);

		mvpp2_prs_shadow_set(priv, pe.index, MVPP2_PRS_LU_VLAN);
	} else {
		mvpp2_prs_init_from_hw(priv, &pe, tid);
	}

	/* Update ports' mask */
	mvpp2_prs_tcam_port_map_set(&pe, port_map);
	mvpp2_prs_hw_write(priv, &pe);

	return ret;
}

/* IPv4 header parsing for fragmentation and L4 offset */
static int mvpp2_prs_ip4_proto(struct mvpp2 *priv, unsigned short proto,
			       unsigned int ri, unsigned int ri_mask)
{
	struct mvpp2_prs_entry pe;
	int tid;

	if (proto != IPPROTO_TCP && proto != IPPROTO_UDP &&
	    proto != IPPROTO_IGMP)
		return -EINVAL;

	/* Not fragmented packet */
	tid = mvpp2_prs_tcam_first_free(priv, MVPP2_PE_FIRST_FREE_TID,
					MVPP2_PE_LAST_FREE_TID);
	if (tid < 0)
		return tid;

	memset(&pe, 0, sizeof(pe));
	mvpp2_prs_tcam_lu_set(&pe, MVPP2_PRS_LU_IP4);
	pe.index = tid;

	/* Set next lu to IPv4 */
	mvpp2_prs_sram_next_lu_set(&pe, MVPP2_PRS_LU_IP4);
	mvpp2_prs_sram_shift_set(&pe, 12, MVPP2_PRS_SRAM_OP_SEL_SHIFT_ADD);
	/* Set L4 offset */
	mvpp2_prs_sram_offset_set(&pe, MVPP2_PRS_SRAM_UDF_TYPE_L4,
				  sizeof(struct iphdr) - 4,
				  MVPP2_PRS_SRAM_OP_SEL_UDF_ADD);
	mvpp2_prs_sram_ai_update(&pe, MVPP2_PRS_IPV4_DIP_AI_BIT,
				 MVPP2_PRS_IPV4_DIP_AI_BIT);
	mvpp2_prs_sram_ri_update(&pe, ri, ri_mask | MVPP2_PRS_RI_IP_FRAG_MASK);

	mvpp2_prs_tcam_data_byte_set(&pe, 2, 0x00,
				     MVPP2_PRS_TCAM_PROTO_MASK_L);
	mvpp2_prs_tcam_data_byte_set(&pe, 3, 0x00,
				     MVPP2_PRS_TCAM_PROTO_MASK);

	mvpp2_prs_tcam_data_byte_set(&pe, 5, proto, MVPP2_PRS_TCAM_PROTO_MASK);
	mvpp2_prs_tcam_ai_update(&pe, 0, MVPP2_PRS_IPV4_DIP_AI_BIT);
	/* Unmask all ports */
	mvpp2_prs_tcam_port_map_set(&pe, MVPP2_PRS_PORT_MASK);

	/* Update shadow table and hw entry */
	mvpp2_prs_shadow_set(priv, pe.index, MVPP2_PRS_LU_IP4);
	mvpp2_prs_hw_write(priv, &pe);

	/* Fragmented packet */
	tid = mvpp2_prs_tcam_first_free(priv, MVPP2_PE_FIRST_FREE_TID,
					MVPP2_PE_LAST_FREE_TID);
	if (tid < 0)
		return tid;

	pe.index = tid;
	/* Clear ri before updating */
	pe.sram.word[MVPP2_PRS_SRAM_RI_WORD] = 0x0;
	pe.sram.word[MVPP2_PRS_SRAM_RI_CTRL_WORD] = 0x0;
	mvpp2_prs_sram_ri_update(&pe, ri, ri_mask);

	mvpp2_prs_sram_ri_update(&pe, ri | MVPP2_PRS_RI_IP_FRAG_TRUE,
				 ri_mask | MVPP2_PRS_RI_IP_FRAG_MASK);

	mvpp2_prs_tcam_data_byte_set(&pe, 2, 0x00, 0x0);
	mvpp2_prs_tcam_data_byte_set(&pe, 3, 0x00, 0x0);

	/* Update shadow table and hw entry */
	mvpp2_prs_shadow_set(priv, pe.index, MVPP2_PRS_LU_IP4);
	mvpp2_prs_hw_write(priv, &pe);

	return 0;
}

/* IPv4 L3 multicast or broadcast */
static int mvpp2_prs_ip4_cast(struct mvpp2 *priv, unsigned short l3_cast)
{
	struct mvpp2_prs_entry pe;
	int mask, tid;

	tid = mvpp2_prs_tcam_first_free(priv, MVPP2_PE_FIRST_FREE_TID,
					MVPP2_PE_LAST_FREE_TID);
	if (tid < 0)
		return tid;

	memset(&pe, 0, sizeof(pe));
	mvpp2_prs_tcam_lu_set(&pe, MVPP2_PRS_LU_IP4);
	pe.index = tid;

	switch (l3_cast) {
	case MVPP2_PRS_L3_MULTI_CAST:
		mvpp2_prs_tcam_data_byte_set(&pe, 0, MVPP2_PRS_IPV4_MC,
					     MVPP2_PRS_IPV4_MC_MASK);
		mvpp2_prs_sram_ri_update(&pe, MVPP2_PRS_RI_L3_MCAST,
					 MVPP2_PRS_RI_L3_ADDR_MASK);
		break;
	case  MVPP2_PRS_L3_BROAD_CAST:
		mask = MVPP2_PRS_IPV4_BC_MASK;
		mvpp2_prs_tcam_data_byte_set(&pe, 0, mask, mask);
		mvpp2_prs_tcam_data_byte_set(&pe, 1, mask, mask);
		mvpp2_prs_tcam_data_byte_set(&pe, 2, mask, mask);
		mvpp2_prs_tcam_data_byte_set(&pe, 3, mask, mask);
		mvpp2_prs_sram_ri_update(&pe, MVPP2_PRS_RI_L3_BCAST,
					 MVPP2_PRS_RI_L3_ADDR_MASK);
		break;
	default:
		return -EINVAL;
	}

	/* Finished: go to flowid generation */
	mvpp2_prs_sram_next_lu_set(&pe, MVPP2_PRS_LU_FLOWS);
	mvpp2_prs_sram_bits_set(&pe, MVPP2_PRS_SRAM_LU_GEN_BIT, 1);

	mvpp2_prs_tcam_ai_update(&pe, MVPP2_PRS_IPV4_DIP_AI_BIT,
				 MVPP2_PRS_IPV4_DIP_AI_BIT);
	/* Unmask all ports */
	mvpp2_prs_tcam_port_map_set(&pe, MVPP2_PRS_PORT_MASK);

	/* Update shadow table and hw entry */
	mvpp2_prs_shadow_set(priv, pe.index, MVPP2_PRS_LU_IP4);
	mvpp2_prs_hw_write(priv, &pe);

	return 0;
}

/* Set entries for protocols over IPv6  */
static int mvpp2_prs_ip6_proto(struct mvpp2 *priv, unsigned short proto,
			       unsigned int ri, unsigned int ri_mask)
{
	struct mvpp2_prs_entry pe;
	int tid;

	if (proto != IPPROTO_TCP && proto != IPPROTO_UDP &&
	    proto != IPPROTO_ICMPV6 && proto != IPPROTO_IPIP)
		return -EINVAL;

	tid = mvpp2_prs_tcam_first_free(priv, MVPP2_PE_FIRST_FREE_TID,
					MVPP2_PE_LAST_FREE_TID);
	if (tid < 0)
		return tid;

	memset(&pe, 0, sizeof(pe));
	mvpp2_prs_tcam_lu_set(&pe, MVPP2_PRS_LU_IP6);
	pe.index = tid;

	/* Finished: go to flowid generation */
	mvpp2_prs_sram_next_lu_set(&pe, MVPP2_PRS_LU_FLOWS);
	mvpp2_prs_sram_bits_set(&pe, MVPP2_PRS_SRAM_LU_GEN_BIT, 1);
	mvpp2_prs_sram_ri_update(&pe, ri, ri_mask);
	mvpp2_prs_sram_offset_set(&pe, MVPP2_PRS_SRAM_UDF_TYPE_L4,
				  sizeof(struct ipv6hdr) - 6,
				  MVPP2_PRS_SRAM_OP_SEL_UDF_ADD);

	mvpp2_prs_tcam_data_byte_set(&pe, 0, proto, MVPP2_PRS_TCAM_PROTO_MASK);
	mvpp2_prs_tcam_ai_update(&pe, MVPP2_PRS_IPV6_NO_EXT_AI_BIT,
				 MVPP2_PRS_IPV6_NO_EXT_AI_BIT);
	/* Unmask all ports */
	mvpp2_prs_tcam_port_map_set(&pe, MVPP2_PRS_PORT_MASK);

	/* Write HW */
	mvpp2_prs_shadow_set(priv, pe.index, MVPP2_PRS_LU_IP6);
	mvpp2_prs_hw_write(priv, &pe);

	return 0;
}

/* IPv6 L3 multicast entry */
static int mvpp2_prs_ip6_cast(struct mvpp2 *priv, unsigned short l3_cast)
{
	struct mvpp2_prs_entry pe;
	int tid;

	if (l3_cast != MVPP2_PRS_L3_MULTI_CAST)
		return -EINVAL;

	tid = mvpp2_prs_tcam_first_free(priv, MVPP2_PE_FIRST_FREE_TID,
					MVPP2_PE_LAST_FREE_TID);
	if (tid < 0)
		return tid;

	memset(&pe, 0, sizeof(pe));
	mvpp2_prs_tcam_lu_set(&pe, MVPP2_PRS_LU_IP6);
	pe.index = tid;

	/* Finished: go to flowid generation */
	mvpp2_prs_sram_next_lu_set(&pe, MVPP2_PRS_LU_IP6);
	mvpp2_prs_sram_ri_update(&pe, MVPP2_PRS_RI_L3_MCAST,
				 MVPP2_PRS_RI_L3_ADDR_MASK);
	mvpp2_prs_sram_ai_update(&pe, MVPP2_PRS_IPV6_NO_EXT_AI_BIT,
				 MVPP2_PRS_IPV6_NO_EXT_AI_BIT);
	/* Shift back to IPv6 NH */
	mvpp2_prs_sram_shift_set(&pe, -18, MVPP2_PRS_SRAM_OP_SEL_SHIFT_ADD);

	mvpp2_prs_tcam_data_byte_set(&pe, 0, MVPP2_PRS_IPV6_MC,
				     MVPP2_PRS_IPV6_MC_MASK);
	mvpp2_prs_tcam_ai_update(&pe, 0, MVPP2_PRS_IPV6_NO_EXT_AI_BIT);
	/* Unmask all ports */
	mvpp2_prs_tcam_port_map_set(&pe, MVPP2_PRS_PORT_MASK);

	/* Update shadow table and hw entry */
	mvpp2_prs_shadow_set(priv, pe.index, MVPP2_PRS_LU_IP6);
	mvpp2_prs_hw_write(priv, &pe);

	return 0;
}

/* Set prs dedicated flow for the port */
static int mvpp2_prs_flow_id_set(struct mvpp2_port *port, u32 flow_id,
				 u32 ri, u32 ri_mask)
{
	struct mvpp2_prs_entry pe;
	struct mvpp2 *priv = port->priv;
	int tid;
	unsigned int pmap = 0;

	memset(&pe, 0, sizeof(pe));

	tid = mvpp2_prs_flow_find(priv, flow_id, ri, ri_mask);

	/* Such entry not exist */
	if (tid < 0) {
		/* Go through the all entires from last to first */
		tid = mvpp2_prs_tcam_first_free(priv,
						MVPP2_PE_LAST_FREE_TID,
						MVPP2_PE_FIRST_FREE_TID);
		if (tid < 0)
			return tid;

		pe.index = tid;

		/* Set flow ID*/
		mvpp2_prs_sram_ai_update(&pe, flow_id, MVPP2_PRS_FLOW_ID_MASK);
		mvpp2_prs_sram_bits_set(&pe, MVPP2_PRS_SRAM_LU_DONE_BIT, 1);

		/* Update shadow table */
		mvpp2_prs_shadow_set(priv, pe.index, MVPP2_PRS_LU_FLOWS);

		/* Update result data and mask */
		mvpp2_prs_tcam_data_dword_set(&pe, 0, ri, ri_mask);
	} else {
		mvpp2_prs_init_from_hw(priv, &pe, tid);
		pmap = mvpp2_prs_tcam_port_map_get(&pe);
	}

	mvpp2_prs_tcam_lu_set(&pe, MVPP2_PRS_LU_FLOWS);
	mvpp2_prs_tcam_port_map_set(&pe, (1 << port->id) | pmap);
	mvpp2_prs_hw_write(priv, &pe);

	return 0;
}

/* Set prs flow for the port */
static int mvpp2_prs_def_flow(struct mvpp2_port *port)
{
	int index, ret;
	struct mvpp2_prs_flow_id *entry;

	for (index = 0; index < MVPP2_PRS_FL_TCAM_NUM; index++) {
		entry = &mvpp2_prs_flow_id_array[index];
		ret = mvpp2_prs_flow_id_set(port,
					    entry->flow_id,
					    entry->prs_result.ri,
					    entry->prs_result.ri_mask);
		if (ret)
			return ret;
	}
	return 0;
}

static u8 mvpp2_cpu2rxq(struct mvpp2_port *port)
{
	/* The CPU that port bind, each port has a nibble
	 * indexed by port_id, nibble value is CPU id
	 */
	u8 cos_width, bind_cpu;

	cos_width = ilog2(roundup_pow_of_two(num_cos_queues));
	bind_cpu = (rx_cpu_map >> (4 * port->id)) & 0xF;
	return(port->first_rxq + (bind_cpu << cos_width));
}

static u8 mvpp2_cosval_queue_map(struct mvpp2_port *port,
				 u8 cos_value)
{
	int cos_width, cos_mask;

	cos_width = ilog2(roundup_pow_of_two(
			  port->cos_cfg.num_cos_queues));
	cos_mask  = (1 << cos_width) - 1;

	return((port->cos_cfg.pri_map >> (cos_value * 4)) & cos_mask);
}

static int mvpp2_width_calc(struct mvpp2_port *port,
			    u32 *cpu_width, u32 *cos_width)
{
	u32 rxq_width;

	*cpu_width = ilog2(roundup_pow_of_two(used_hifs));
	*cos_width = ilog2(roundup_pow_of_two(port->cos_cfg.num_cos_queues));
	rxq_width = ilog2(roundup_pow_of_two(port->nrxqs));
	if (*cpu_width + *cos_width > rxq_width) {
		netdev_err(port->dev, "cpu_width=%u or cos_queue_width=%u invalid\n",
			   *cpu_width, *cos_width);
		*cpu_width = 0;
		*cos_width = 0;
		return -1;
	}
	return 0;
}

static void mvpp2_prs_flow_id_attr_set(int flow_id, int ri, int ri_mask)
{
	int flow_attr = 0;

	flow_attr |= MVPP2_PRS_FL_ATTR_VLAN_BIT;
	if (ri_mask & MVPP2_PRS_RI_VLAN_MASK &&
	    (ri & MVPP2_PRS_RI_VLAN_MASK) == MVPP2_PRS_RI_VLAN_NONE)
		flow_attr &= ~MVPP2_PRS_FL_ATTR_VLAN_BIT;

	if ((ri & MVPP2_PRS_RI_L3_PROTO_MASK) == MVPP2_PRS_RI_L3_IP4 ||
	    (ri & MVPP2_PRS_RI_L3_PROTO_MASK) == MVPP2_PRS_RI_L3_IP4_OPT ||
	    (ri & MVPP2_PRS_RI_L3_PROTO_MASK) == MVPP2_PRS_RI_L3_IP4_OTHER)
		flow_attr |= MVPP2_PRS_FL_ATTR_IP4_BIT;

	if ((ri & MVPP2_PRS_RI_L3_PROTO_MASK) == MVPP2_PRS_RI_L3_IP6 ||
	    (ri & MVPP2_PRS_RI_L3_PROTO_MASK) == MVPP2_PRS_RI_L3_IP6_EXT)
		flow_attr |= MVPP2_PRS_FL_ATTR_IP6_BIT;

	if ((ri & MVPP2_PRS_RI_L3_PROTO_MASK) == MVPP2_PRS_RI_L3_ARP)
		flow_attr |= MVPP2_PRS_FL_ATTR_ARP_BIT;

	if (ri & MVPP2_PRS_RI_IP_FRAG_MASK)
		flow_attr |= MVPP2_PRS_FL_ATTR_FRAG_BIT;

	if ((ri & MVPP2_PRS_RI_L4_PROTO_MASK) == MVPP2_PRS_RI_L4_TCP)
		flow_attr |= MVPP2_PRS_FL_ATTR_TCP_BIT;

	if ((ri & MVPP2_PRS_RI_L4_PROTO_MASK) == MVPP2_PRS_RI_L4_UDP)
		flow_attr |= MVPP2_PRS_FL_ATTR_UDP_BIT;

	mvpp2_prs_flow_id_attr_tbl[flow_id] = flow_attr;
}

/* Init lookup id attribute array */
static void mvpp2_prs_flow_id_attr_init(void)
{
	int index;
	u32 ri, ri_mask, flow_id;

	for (index = 0; index < MVPP2_PRS_FL_TCAM_NUM; index++) {
		ri = mvpp2_prs_flow_id_array[index].prs_result.ri;
		ri_mask = mvpp2_prs_flow_id_array[index].prs_result.ri_mask;
		flow_id = mvpp2_prs_flow_id_array[index].flow_id;

		mvpp2_prs_flow_id_attr_set(flow_id, ri, ri_mask);
	}
}

static int mvpp2_prs_flow_id_attr_get(int flow_id)
{
	return mvpp2_prs_flow_id_attr_tbl[flow_id];
}

/* Parser per-port initialization */
static void mvpp2_prs_hw_port_init(struct mvpp2 *priv, int port, int lu_first,
				   int lu_max, int offset)
{
	u32 val;

	/* Set lookup ID */
	val = mvpp2_read(priv, MVPP2_PRS_INIT_LOOKUP_REG);
	val &= ~MVPP2_PRS_PORT_LU_MASK(port);
	val |=  MVPP2_PRS_PORT_LU_VAL(port, lu_first);
	mvpp2_write(priv, MVPP2_PRS_INIT_LOOKUP_REG, val);

	/* Set maximum number of loops for packet received from port */
	val = mvpp2_read(priv, MVPP2_PRS_MAX_LOOP_REG(port));
	val &= ~MVPP2_PRS_MAX_LOOP_MASK(port);
	val |= MVPP2_PRS_MAX_LOOP_VAL(port, lu_max);
	mvpp2_write(priv, MVPP2_PRS_MAX_LOOP_REG(port), val);

	/* Set initial offset for packet header extraction for the first
	 * searching loop
	 */
	val = mvpp2_read(priv, MVPP2_PRS_INIT_OFFS_REG(port));
	val &= ~MVPP2_PRS_INIT_OFF_MASK(port);
	val |= MVPP2_PRS_INIT_OFF_VAL(port, offset);
	mvpp2_write(priv, MVPP2_PRS_INIT_OFFS_REG(port), val);
}

/* Default flow entries initialization for all ports */
static void mvpp2_prs_def_flow_init(struct mvpp2 *priv)
{
	struct mvpp2_prs_entry pe;
	int port;

	for (port = 0; port < MVPP2_MAX_PORTS; port++) {
		memset(&pe, 0, sizeof(pe));
		mvpp2_prs_tcam_lu_set(&pe, MVPP2_PRS_LU_FLOWS);
		pe.index = MVPP2_PE_FIRST_DEFAULT_FLOW - port;

		/* Mask all ports */
		mvpp2_prs_tcam_port_map_set(&pe, 0);

		/* Set flow ID*/
		mvpp2_prs_sram_ai_update(&pe, port, MVPP2_PRS_FLOW_ID_MASK);
		mvpp2_prs_sram_bits_set(&pe, MVPP2_PRS_SRAM_LU_DONE_BIT, 1);

		/* Update shadow table and hw entry */
		mvpp2_prs_shadow_set(priv, pe.index, MVPP2_PRS_LU_FLOWS);
		mvpp2_prs_hw_write(priv, &pe);
	}
}

/* Set default entry for Marvell Header field */
static void mvpp2_prs_mh_init(struct mvpp2 *priv)
{
	struct mvpp2_prs_entry pe;

	memset(&pe, 0, sizeof(pe));

	pe.index = MVPP2_PE_MH_DEFAULT;
	mvpp2_prs_tcam_lu_set(&pe, MVPP2_PRS_LU_MH);
	mvpp2_prs_sram_shift_set(&pe, MVPP2_MH_SIZE,
				 MVPP2_PRS_SRAM_OP_SEL_SHIFT_ADD);
	mvpp2_prs_sram_next_lu_set(&pe, MVPP2_PRS_LU_MAC);

	/* Unmask all ports */
	mvpp2_prs_tcam_port_map_set(&pe, MVPP2_PRS_PORT_MASK);

	/* Update shadow table and hw entry */
	mvpp2_prs_shadow_set(priv, pe.index, MVPP2_PRS_LU_MH);
	mvpp2_prs_hw_write(priv, &pe);
}

/* Set default entires (place holder) for promiscuous, non-promiscuous and
 * multicast MAC addresses
 */
static void mvpp2_prs_mac_init(struct mvpp2 *priv)
{
	struct mvpp2_prs_entry pe;

	memset(&pe, 0, sizeof(pe));

	/* Non-promiscuous mode for all ports - DROP unknown packets */
	pe.index = MVPP2_PE_MAC_NON_PROMISCUOUS;
	mvpp2_prs_tcam_lu_set(&pe, MVPP2_PRS_LU_MAC);

	mvpp2_prs_sram_ri_update(&pe, MVPP2_PRS_RI_DROP_MASK,
				 MVPP2_PRS_RI_DROP_MASK);
	mvpp2_prs_sram_bits_set(&pe, MVPP2_PRS_SRAM_LU_GEN_BIT, 1);
	mvpp2_prs_sram_next_lu_set(&pe, MVPP2_PRS_LU_FLOWS);

	/* Unmask all ports */
	mvpp2_prs_tcam_port_map_set(&pe, MVPP2_PRS_PORT_MASK);

	/* Update shadow table and hw entry */
	mvpp2_prs_shadow_set(priv, pe.index, MVPP2_PRS_LU_MAC);
	mvpp2_prs_hw_write(priv, &pe);

	/* Create dummy entries for drop all and promiscuous modes */
	mvpp2_prs_mac_drop_all_set(priv, 0, false);
	mvpp2_prs_mac_promisc_set(priv, 0, MVPP2_PRS_L2_UNI_CAST, false);
	mvpp2_prs_mac_promisc_set(priv, 0, MVPP2_PRS_L2_MULTI_CAST, false);
}

/* Set default entries for various types of dsa packets */
static void mvpp2_prs_dsa_init(struct mvpp2 *priv)
{
	struct mvpp2_prs_entry pe;

	/* None tagged EDSA entry - place holder */
	mvpp2_prs_dsa_tag_set(priv, 0, false, MVPP2_PRS_UNTAGGED,
			      MVPP2_PRS_EDSA);

	/* Tagged EDSA entry - place holder */
	mvpp2_prs_dsa_tag_set(priv, 0, false, MVPP2_PRS_TAGGED, MVPP2_PRS_EDSA);

	/* None tagged DSA entry - place holder */
	mvpp2_prs_dsa_tag_set(priv, 0, false, MVPP2_PRS_UNTAGGED,
			      MVPP2_PRS_DSA);

	/* Tagged DSA entry - place holder */
	mvpp2_prs_dsa_tag_set(priv, 0, false, MVPP2_PRS_TAGGED, MVPP2_PRS_DSA);

	/* None tagged EDSA ethertype entry - place holder*/
	mvpp2_prs_dsa_tag_ethertype_set(priv, 0, false,
					MVPP2_PRS_UNTAGGED, MVPP2_PRS_EDSA);

	/* Tagged EDSA ethertype entry - place holder*/
	mvpp2_prs_dsa_tag_ethertype_set(priv, 0, false,
					MVPP2_PRS_TAGGED, MVPP2_PRS_EDSA);

	/* None tagged DSA ethertype entry */
	mvpp2_prs_dsa_tag_ethertype_set(priv, 0, true,
					MVPP2_PRS_UNTAGGED, MVPP2_PRS_DSA);

	/* Tagged DSA ethertype entry */
	mvpp2_prs_dsa_tag_ethertype_set(priv, 0, true,
					MVPP2_PRS_TAGGED, MVPP2_PRS_DSA);

	/* Set default entry, in case DSA or EDSA tag not found */
	memset(&pe, 0, sizeof(pe));
	mvpp2_prs_tcam_lu_set(&pe, MVPP2_PRS_LU_DSA);
	pe.index = MVPP2_PE_DSA_DEFAULT;
	mvpp2_prs_sram_next_lu_set(&pe, MVPP2_PRS_LU_VLAN);

	/* Shift 0 bytes */
	mvpp2_prs_sram_shift_set(&pe, 0, MVPP2_PRS_SRAM_OP_SEL_SHIFT_ADD);
	mvpp2_prs_shadow_set(priv, pe.index, MVPP2_PRS_LU_MAC);

	/* Clear all sram ai bits for next iteration */
	mvpp2_prs_sram_ai_update(&pe, 0, MVPP2_PRS_SRAM_AI_MASK);

	/* Unmask all ports */
	mvpp2_prs_tcam_port_map_set(&pe, MVPP2_PRS_PORT_MASK);

	mvpp2_prs_hw_write(priv, &pe);
}

/* Initialize parser entries for VID filtering */
static void mvpp2_prs_vid_init(struct mvpp2 *priv)
{
	struct mvpp2_prs_entry pe;

	memset(&pe, 0, sizeof(pe));

	/* Set default vid entry */
	pe.index = MVPP2_PE_VID_FLTR_DEFAULT;
	mvpp2_prs_tcam_lu_set(&pe, MVPP2_PRS_LU_VID);

	mvpp2_prs_tcam_ai_update(&pe, 0, MVPP2_PRS_EDSA_VID_AI_BIT);

	/* Skip VLAN header - Set offset to 4 bytes */
	mvpp2_prs_sram_shift_set(&pe, MVPP2_VLAN_TAG_LEN,
				 MVPP2_PRS_SRAM_OP_SEL_SHIFT_ADD);

	/* Clear all ai bits for next iteration */
	mvpp2_prs_sram_ai_update(&pe, 0, MVPP2_PRS_SRAM_AI_MASK);

	mvpp2_prs_sram_next_lu_set(&pe, MVPP2_PRS_LU_L2);

	/* Unmask all ports */
	mvpp2_prs_tcam_port_map_set(&pe, MVPP2_PRS_PORT_MASK);

	/* Update shadow table and hw entry */
	mvpp2_prs_shadow_set(priv, pe.index, MVPP2_PRS_LU_VID);
	mvpp2_prs_hw_write(priv, &pe);

	/* Set default vid entry for extended DSA*/
	memset(&pe, 0, sizeof(pe));

	/* Set default vid entry */
	pe.index = MVPP2_PE_VID_EDSA_FLTR_DEFAULT;
	mvpp2_prs_tcam_lu_set(&pe, MVPP2_PRS_LU_VID);

	mvpp2_prs_tcam_ai_update(&pe, MVPP2_PRS_EDSA_VID_AI_BIT,
				 MVPP2_PRS_EDSA_VID_AI_BIT);

	/* Skip VLAN header - Set offset to 8 bytes */
	mvpp2_prs_sram_shift_set(&pe, MVPP2_VLAN_TAG_EDSA_LEN,
				 MVPP2_PRS_SRAM_OP_SEL_SHIFT_ADD);

	/* Clear all ai bits for next iteration */
	mvpp2_prs_sram_ai_update(&pe, 0, MVPP2_PRS_SRAM_AI_MASK);

	mvpp2_prs_sram_next_lu_set(&pe, MVPP2_PRS_LU_L2);

	/* Unmask all ports */
	mvpp2_prs_tcam_port_map_set(&pe, MVPP2_PRS_PORT_MASK);

	/* Update shadow table and hw entry */
	mvpp2_prs_shadow_set(priv, pe.index, MVPP2_PRS_LU_VID);
	mvpp2_prs_hw_write(priv, &pe);
}

/* Match basic ethertypes */
static int mvpp2_prs_etype_init(struct mvpp2 *priv)
{
	struct mvpp2_prs_entry pe;
	int tid;

	/* Ethertype: PPPoE */
	tid = mvpp2_prs_tcam_first_free(priv, MVPP2_PE_FIRST_FREE_TID,
					MVPP2_PE_LAST_FREE_TID);
	if (tid < 0)
		return tid;

	memset(&pe, 0, sizeof(pe));
	mvpp2_prs_tcam_lu_set(&pe, MVPP2_PRS_LU_L2);
	pe.index = tid;

	mvpp2_prs_match_etype(&pe, 0, ETH_P_PPP_SES);

	mvpp2_prs_sram_shift_set(&pe, MVPP2_PPPOE_HDR_SIZE,
				 MVPP2_PRS_SRAM_OP_SEL_SHIFT_ADD);
	mvpp2_prs_sram_next_lu_set(&pe, MVPP2_PRS_LU_PPPOE);
	mvpp2_prs_sram_ri_update(&pe, MVPP2_PRS_RI_PPPOE_MASK,
				 MVPP2_PRS_RI_PPPOE_MASK);

	/* Update shadow table and hw entry */
	mvpp2_prs_shadow_set(priv, pe.index, MVPP2_PRS_LU_L2);
	priv->prs_shadow[pe.index].udf = MVPP2_PRS_UDF_L2_DEF;
	priv->prs_shadow[pe.index].finish = false;
	mvpp2_prs_shadow_ri_set(priv, pe.index, MVPP2_PRS_RI_PPPOE_MASK,
				MVPP2_PRS_RI_PPPOE_MASK);
	mvpp2_prs_hw_write(priv, &pe);

	/* Ethertype: ARP */
	tid = mvpp2_prs_tcam_first_free(priv, MVPP2_PE_FIRST_FREE_TID,
					MVPP2_PE_LAST_FREE_TID);
	if (tid < 0)
		return tid;

	memset(&pe, 0, sizeof(pe));
	mvpp2_prs_tcam_lu_set(&pe, MVPP2_PRS_LU_L2);
	pe.index = tid;

	mvpp2_prs_match_etype(&pe, 0, ETH_P_ARP);

	/* Generate flow in the next iteration*/
	mvpp2_prs_sram_next_lu_set(&pe, MVPP2_PRS_LU_FLOWS);
	mvpp2_prs_sram_bits_set(&pe, MVPP2_PRS_SRAM_LU_GEN_BIT, 1);
	mvpp2_prs_sram_ri_update(&pe, MVPP2_PRS_RI_L3_ARP,
				 MVPP2_PRS_RI_L3_PROTO_MASK);
	/* Set L3 offset */
	mvpp2_prs_sram_offset_set(&pe, MVPP2_PRS_SRAM_UDF_TYPE_L3,
				  MVPP2_ETH_TYPE_LEN,
				  MVPP2_PRS_SRAM_OP_SEL_UDF_ADD);

	/* Update shadow table and hw entry */
	mvpp2_prs_shadow_set(priv, pe.index, MVPP2_PRS_LU_L2);
	priv->prs_shadow[pe.index].udf = MVPP2_PRS_UDF_L2_DEF;
	priv->prs_shadow[pe.index].finish = true;
	mvpp2_prs_shadow_ri_set(priv, pe.index, MVPP2_PRS_RI_L3_ARP,
				MVPP2_PRS_RI_L3_PROTO_MASK);
	mvpp2_prs_hw_write(priv, &pe);

	/* Ethertype: LBTD */
	tid = mvpp2_prs_tcam_first_free(priv, MVPP2_PE_FIRST_FREE_TID,
					MVPP2_PE_LAST_FREE_TID);
	if (tid < 0)
		return tid;

	memset(&pe, 0, sizeof(pe));
	mvpp2_prs_tcam_lu_set(&pe, MVPP2_PRS_LU_L2);
	pe.index = tid;

	mvpp2_prs_match_etype(&pe, 0, MVPP2_IP_LBDT_TYPE);

	/* Generate flow in the next iteration*/
	mvpp2_prs_sram_next_lu_set(&pe, MVPP2_PRS_LU_FLOWS);
	mvpp2_prs_sram_bits_set(&pe, MVPP2_PRS_SRAM_LU_GEN_BIT, 1);
	mvpp2_prs_sram_ri_update(&pe, MVPP2_PRS_RI_CPU_CODE_RX_SPEC |
				 MVPP2_PRS_RI_UDF3_RX_SPECIAL,
				 MVPP2_PRS_RI_CPU_CODE_MASK |
				 MVPP2_PRS_RI_UDF3_MASK);
	/* Set L3 offset */
	mvpp2_prs_sram_offset_set(&pe, MVPP2_PRS_SRAM_UDF_TYPE_L3,
				  MVPP2_ETH_TYPE_LEN,
				  MVPP2_PRS_SRAM_OP_SEL_UDF_ADD);

	/* Update shadow table and hw entry */
	mvpp2_prs_shadow_set(priv, pe.index, MVPP2_PRS_LU_L2);
	priv->prs_shadow[pe.index].udf = MVPP2_PRS_UDF_L2_DEF;
	priv->prs_shadow[pe.index].finish = true;
	mvpp2_prs_shadow_ri_set(priv, pe.index, MVPP2_PRS_RI_CPU_CODE_RX_SPEC |
				MVPP2_PRS_RI_UDF3_RX_SPECIAL,
				MVPP2_PRS_RI_CPU_CODE_MASK |
				MVPP2_PRS_RI_UDF3_MASK);
	mvpp2_prs_hw_write(priv, &pe);

	/* Ethertype: IPv4 without options */
	tid = mvpp2_prs_tcam_first_free(priv, MVPP2_PE_FIRST_FREE_TID,
					MVPP2_PE_LAST_FREE_TID);
	if (tid < 0)
		return tid;

	memset(&pe, 0, sizeof(pe));
	mvpp2_prs_tcam_lu_set(&pe, MVPP2_PRS_LU_L2);
	pe.index = tid;

	mvpp2_prs_match_etype(&pe, 0, ETH_P_IP);
	mvpp2_prs_tcam_data_byte_set(&pe, MVPP2_ETH_TYPE_LEN,
				     MVPP2_PRS_IPV4_HEAD | MVPP2_PRS_IPV4_IHL,
				     MVPP2_PRS_IPV4_HEAD_MASK |
				     MVPP2_PRS_IPV4_IHL_MASK);

	mvpp2_prs_sram_next_lu_set(&pe, MVPP2_PRS_LU_IP4);
	mvpp2_prs_sram_ri_update(&pe, MVPP2_PRS_RI_L3_IP4,
				 MVPP2_PRS_RI_L3_PROTO_MASK);
	/* Skip eth_type + 4 bytes of IP header */
	mvpp2_prs_sram_shift_set(&pe, MVPP2_ETH_TYPE_LEN + 4,
				 MVPP2_PRS_SRAM_OP_SEL_SHIFT_ADD);
	/* Set L3 offset */
	mvpp2_prs_sram_offset_set(&pe, MVPP2_PRS_SRAM_UDF_TYPE_L3,
				  MVPP2_ETH_TYPE_LEN,
				  MVPP2_PRS_SRAM_OP_SEL_UDF_ADD);

	/* Update shadow table and hw entry */
	mvpp2_prs_shadow_set(priv, pe.index, MVPP2_PRS_LU_L2);
	priv->prs_shadow[pe.index].udf = MVPP2_PRS_UDF_L2_DEF;
	priv->prs_shadow[pe.index].finish = false;
	mvpp2_prs_shadow_ri_set(priv, pe.index, MVPP2_PRS_RI_L3_IP4,
				MVPP2_PRS_RI_L3_PROTO_MASK);
	mvpp2_prs_hw_write(priv, &pe);

	/* Ethertype: IPv4 with options */
	tid = mvpp2_prs_tcam_first_free(priv, MVPP2_PE_FIRST_FREE_TID,
					MVPP2_PE_LAST_FREE_TID);
	if (tid < 0)
		return tid;

	pe.index = tid;

	/* Clear tcam data before updating */
	pe.tcam.byte[MVPP2_PRS_TCAM_DATA_BYTE(MVPP2_ETH_TYPE_LEN)] = 0x0;
	pe.tcam.byte[MVPP2_PRS_TCAM_DATA_BYTE_EN(MVPP2_ETH_TYPE_LEN)] = 0x0;

	mvpp2_prs_tcam_data_byte_set(&pe, MVPP2_ETH_TYPE_LEN,
				     MVPP2_PRS_IPV4_HEAD,
				     MVPP2_PRS_IPV4_HEAD_MASK);

	/* Clear ri before updating */
	pe.sram.word[MVPP2_PRS_SRAM_RI_WORD] = 0x0;
	pe.sram.word[MVPP2_PRS_SRAM_RI_CTRL_WORD] = 0x0;
	mvpp2_prs_sram_ri_update(&pe, MVPP2_PRS_RI_L3_IP4_OPT,
				 MVPP2_PRS_RI_L3_PROTO_MASK);

	/* Update shadow table and hw entry */
	mvpp2_prs_shadow_set(priv, pe.index, MVPP2_PRS_LU_L2);
	priv->prs_shadow[pe.index].udf = MVPP2_PRS_UDF_L2_DEF;
	priv->prs_shadow[pe.index].finish = false;
	mvpp2_prs_shadow_ri_set(priv, pe.index, MVPP2_PRS_RI_L3_IP4_OPT,
				MVPP2_PRS_RI_L3_PROTO_MASK);
	mvpp2_prs_hw_write(priv, &pe);

	/* Ethertype: IPv6 without options */
	tid = mvpp2_prs_tcam_first_free(priv, MVPP2_PE_FIRST_FREE_TID,
					MVPP2_PE_LAST_FREE_TID);
	if (tid < 0)
		return tid;

	memset(&pe, 0, sizeof(pe));
	mvpp2_prs_tcam_lu_set(&pe, MVPP2_PRS_LU_L2);
	pe.index = tid;

	mvpp2_prs_match_etype(&pe, 0, ETH_P_IPV6);

	/* Skip DIP of IPV6 header */
	mvpp2_prs_sram_shift_set(&pe, MVPP2_ETH_TYPE_LEN + 8 +
				 MVPP2_MAX_L3_ADDR_SIZE,
				 MVPP2_PRS_SRAM_OP_SEL_SHIFT_ADD);
	mvpp2_prs_sram_next_lu_set(&pe, MVPP2_PRS_LU_IP6);
	mvpp2_prs_sram_ri_update(&pe, MVPP2_PRS_RI_L3_IP6,
				 MVPP2_PRS_RI_L3_PROTO_MASK);
	/* Set L3 offset */
	mvpp2_prs_sram_offset_set(&pe, MVPP2_PRS_SRAM_UDF_TYPE_L3,
				  MVPP2_ETH_TYPE_LEN,
				  MVPP2_PRS_SRAM_OP_SEL_UDF_ADD);

	mvpp2_prs_shadow_set(priv, pe.index, MVPP2_PRS_LU_L2);
	priv->prs_shadow[pe.index].udf = MVPP2_PRS_UDF_L2_DEF;
	priv->prs_shadow[pe.index].finish = false;
	mvpp2_prs_shadow_ri_set(priv, pe.index, MVPP2_PRS_RI_L3_IP6,
				MVPP2_PRS_RI_L3_PROTO_MASK);
	mvpp2_prs_hw_write(priv, &pe);

	/* Default entry for MVPP2_PRS_LU_L2 - Unknown ethtype */
	memset(&pe, 0, sizeof(struct mvpp2_prs_entry));
	mvpp2_prs_tcam_lu_set(&pe, MVPP2_PRS_LU_L2);
	pe.index = MVPP2_PE_ETH_TYPE_UN;

	/* Unmask all ports */
	mvpp2_prs_tcam_port_map_set(&pe, MVPP2_PRS_PORT_MASK);

	/* Generate flow in the next iteration*/
	mvpp2_prs_sram_bits_set(&pe, MVPP2_PRS_SRAM_LU_GEN_BIT, 1);
	mvpp2_prs_sram_next_lu_set(&pe, MVPP2_PRS_LU_FLOWS);
	mvpp2_prs_sram_ri_update(&pe, MVPP2_PRS_RI_L3_UN,
				 MVPP2_PRS_RI_L3_PROTO_MASK);
	/* Set L3 offset even it's unknown L3 */
	mvpp2_prs_sram_offset_set(&pe, MVPP2_PRS_SRAM_UDF_TYPE_L3,
				  MVPP2_ETH_TYPE_LEN,
				  MVPP2_PRS_SRAM_OP_SEL_UDF_ADD);

	/* Update shadow table and hw entry */
	mvpp2_prs_shadow_set(priv, pe.index, MVPP2_PRS_LU_L2);
	priv->prs_shadow[pe.index].udf = MVPP2_PRS_UDF_L2_DEF;
	priv->prs_shadow[pe.index].finish = true;
	mvpp2_prs_shadow_ri_set(priv, pe.index, MVPP2_PRS_RI_L3_UN,
				MVPP2_PRS_RI_L3_PROTO_MASK);
	mvpp2_prs_hw_write(priv, &pe);

	return 0;
}

/* Configure vlan entries and detect up to 2 successive VLAN tags.
 * Possible options:
 * 0x8100, 0x88A8
 * 0x8100, 0x8100
 * 0x8100
 * 0x88A8
 */
static int mvpp2_prs_vlan_init(struct platform_device *pdev, struct mvpp2 *priv)
{
	struct mvpp2_prs_entry pe;
	int err;

	priv->prs_double_vlans = devm_kcalloc(&pdev->dev, sizeof(bool),
					      MVPP2_PRS_DBL_VLANS_MAX,
					      GFP_KERNEL);
	if (!priv->prs_double_vlans)
		return -ENOMEM;

	/* Double VLAN: 0x8100, 0x88A8 */
	err = mvpp2_prs_double_vlan_add(priv, ETH_P_8021Q, ETH_P_8021AD,
					MVPP2_PRS_PORT_MASK);
	if (err)
		return err;

	/* Double VLAN: 0x8100, 0x8100 */
	err = mvpp2_prs_double_vlan_add(priv, ETH_P_8021Q, ETH_P_8021Q,
					MVPP2_PRS_PORT_MASK);
	if (err)
		return err;

	/* Single VLAN: 0x88a8 */
	err = mvpp2_prs_vlan_add(priv, ETH_P_8021AD, MVPP2_PRS_SINGLE_VLAN_AI,
				 MVPP2_PRS_PORT_MASK);
	if (err)
		return err;

	/* Single VLAN: 0x8100 */
	err = mvpp2_prs_vlan_add(priv, ETH_P_8021Q, MVPP2_PRS_SINGLE_VLAN_AI,
				 MVPP2_PRS_PORT_MASK);
	if (err)
		return err;

	/* Set default double vlan entry */
	memset(&pe, 0, sizeof(pe));
	mvpp2_prs_tcam_lu_set(&pe, MVPP2_PRS_LU_VLAN);
	pe.index = MVPP2_PE_VLAN_DBL;

	mvpp2_prs_sram_next_lu_set(&pe, MVPP2_PRS_LU_VID);

	/* Clear ai for next iterations */
	mvpp2_prs_sram_ai_update(&pe, 0, MVPP2_PRS_SRAM_AI_MASK);
	mvpp2_prs_sram_ri_update(&pe, MVPP2_PRS_RI_VLAN_DOUBLE,
				 MVPP2_PRS_RI_VLAN_MASK);

	mvpp2_prs_tcam_ai_update(&pe, MVPP2_PRS_DBL_VLAN_AI_BIT,
				 MVPP2_PRS_DBL_VLAN_AI_BIT);
	/* Unmask all ports */
	mvpp2_prs_tcam_port_map_set(&pe, MVPP2_PRS_PORT_MASK);

	/* Update shadow table and hw entry */
	mvpp2_prs_shadow_set(priv, pe.index, MVPP2_PRS_LU_VLAN);
	mvpp2_prs_hw_write(priv, &pe);

	/* Set default vlan none entry */
	memset(&pe, 0, sizeof(pe));
	mvpp2_prs_tcam_lu_set(&pe, MVPP2_PRS_LU_VLAN);
	pe.index = MVPP2_PE_VLAN_NONE;

	mvpp2_prs_sram_next_lu_set(&pe, MVPP2_PRS_LU_L2);
	mvpp2_prs_sram_ri_update(&pe, MVPP2_PRS_RI_VLAN_NONE,
				 MVPP2_PRS_RI_VLAN_MASK);

	/* Unmask all ports */
	mvpp2_prs_tcam_port_map_set(&pe, MVPP2_PRS_PORT_MASK);

	/* Update shadow table and hw entry */
	mvpp2_prs_shadow_set(priv, pe.index, MVPP2_PRS_LU_VLAN);
	mvpp2_prs_hw_write(priv, &pe);

	return 0;
}

/* Set entries for PPPoE ethertype */
static int mvpp2_prs_pppoe_init(struct mvpp2 *priv)
{
	struct mvpp2_prs_entry pe;
	int tid;

	/* IPv4 over PPPoE with options */
	tid = mvpp2_prs_tcam_first_free(priv, MVPP2_PE_FIRST_FREE_TID,
					MVPP2_PE_LAST_FREE_TID);
	if (tid < 0)
		return tid;

	memset(&pe, 0, sizeof(pe));
	mvpp2_prs_tcam_lu_set(&pe, MVPP2_PRS_LU_PPPOE);
	pe.index = tid;

	mvpp2_prs_match_etype(&pe, 0, PPP_IP);

	mvpp2_prs_sram_next_lu_set(&pe, MVPP2_PRS_LU_IP4);
	mvpp2_prs_sram_ri_update(&pe, MVPP2_PRS_RI_L3_IP4_OPT,
				 MVPP2_PRS_RI_L3_PROTO_MASK);
	/* Skip eth_type + 4 bytes of IP header */
	mvpp2_prs_sram_shift_set(&pe, MVPP2_ETH_TYPE_LEN + 4,
				 MVPP2_PRS_SRAM_OP_SEL_SHIFT_ADD);
	/* Set L3 offset */
	mvpp2_prs_sram_offset_set(&pe, MVPP2_PRS_SRAM_UDF_TYPE_L3,
				  MVPP2_ETH_TYPE_LEN,
				  MVPP2_PRS_SRAM_OP_SEL_UDF_ADD);

	/* Update shadow table and hw entry */
	mvpp2_prs_shadow_set(priv, pe.index, MVPP2_PRS_LU_PPPOE);
	mvpp2_prs_hw_write(priv, &pe);

	/* IPv4 over PPPoE without options */
	tid = mvpp2_prs_tcam_first_free(priv, MVPP2_PE_FIRST_FREE_TID,
					MVPP2_PE_LAST_FREE_TID);
	if (tid < 0)
		return tid;

	pe.index = tid;

	mvpp2_prs_tcam_data_byte_set(&pe, MVPP2_ETH_TYPE_LEN,
				     MVPP2_PRS_IPV4_HEAD | MVPP2_PRS_IPV4_IHL,
				     MVPP2_PRS_IPV4_HEAD_MASK |
				     MVPP2_PRS_IPV4_IHL_MASK);

	/* Clear ri before updating */
	pe.sram.word[MVPP2_PRS_SRAM_RI_WORD] = 0x0;
	pe.sram.word[MVPP2_PRS_SRAM_RI_CTRL_WORD] = 0x0;
	mvpp2_prs_sram_ri_update(&pe, MVPP2_PRS_RI_L3_IP4,
				 MVPP2_PRS_RI_L3_PROTO_MASK);

	/* Update shadow table and hw entry */
	mvpp2_prs_shadow_set(priv, pe.index, MVPP2_PRS_LU_PPPOE);
	mvpp2_prs_hw_write(priv, &pe);

	/* IPv6 over PPPoE */
	tid = mvpp2_prs_tcam_first_free(priv, MVPP2_PE_FIRST_FREE_TID,
					MVPP2_PE_LAST_FREE_TID);
	if (tid < 0)
		return tid;

	memset(&pe, 0, sizeof(pe));
	mvpp2_prs_tcam_lu_set(&pe, MVPP2_PRS_LU_PPPOE);
	pe.index = tid;

	mvpp2_prs_match_etype(&pe, 0, PPP_IPV6);

	mvpp2_prs_sram_next_lu_set(&pe, MVPP2_PRS_LU_IP6);
	mvpp2_prs_sram_ri_update(&pe, MVPP2_PRS_RI_L3_IP6,
				 MVPP2_PRS_RI_L3_PROTO_MASK);
	/* Skip eth_type + 4 bytes of IPv6 header */
	mvpp2_prs_sram_shift_set(&pe, MVPP2_ETH_TYPE_LEN + 4,
				 MVPP2_PRS_SRAM_OP_SEL_SHIFT_ADD);
	/* Set L3 offset */
	mvpp2_prs_sram_offset_set(&pe, MVPP2_PRS_SRAM_UDF_TYPE_L3,
				  MVPP2_ETH_TYPE_LEN,
				  MVPP2_PRS_SRAM_OP_SEL_UDF_ADD);

	/* Update shadow table and hw entry */
	mvpp2_prs_shadow_set(priv, pe.index, MVPP2_PRS_LU_PPPOE);
	mvpp2_prs_hw_write(priv, &pe);

	/* Non-IP over PPPoE */
	tid = mvpp2_prs_tcam_first_free(priv, MVPP2_PE_FIRST_FREE_TID,
					MVPP2_PE_LAST_FREE_TID);
	if (tid < 0)
		return tid;

	memset(&pe, 0, sizeof(pe));
	mvpp2_prs_tcam_lu_set(&pe, MVPP2_PRS_LU_PPPOE);
	pe.index = tid;

	mvpp2_prs_sram_ri_update(&pe, MVPP2_PRS_RI_L3_UN,
				 MVPP2_PRS_RI_L3_PROTO_MASK);

	/* Finished: go to flowid generation */
	mvpp2_prs_sram_next_lu_set(&pe, MVPP2_PRS_LU_FLOWS);
	mvpp2_prs_sram_bits_set(&pe, MVPP2_PRS_SRAM_LU_GEN_BIT, 1);
	/* Set L3 offset even if it's unknown L3 */
	mvpp2_prs_sram_offset_set(&pe, MVPP2_PRS_SRAM_UDF_TYPE_L3,
				  MVPP2_ETH_TYPE_LEN,
				  MVPP2_PRS_SRAM_OP_SEL_UDF_ADD);

	/* Update shadow table and hw entry */
	mvpp2_prs_shadow_set(priv, pe.index, MVPP2_PRS_LU_PPPOE);
	mvpp2_prs_hw_write(priv, &pe);

	return 0;
}

/* Initialize entries for IPv4 */
static int mvpp2_prs_ip4_init(struct mvpp2 *priv)
{
	struct mvpp2_prs_entry pe;
	int err;

	/* Set entries for TCP, UDP and IGMP over IPv4 */
	err = mvpp2_prs_ip4_proto(priv, IPPROTO_TCP, MVPP2_PRS_RI_L4_TCP,
				  MVPP2_PRS_RI_L4_PROTO_MASK);
	if (err)
		return err;

	err = mvpp2_prs_ip4_proto(priv, IPPROTO_UDP, MVPP2_PRS_RI_L4_UDP,
				  MVPP2_PRS_RI_L4_PROTO_MASK);
	if (err)
		return err;

	err = mvpp2_prs_ip4_proto(priv, IPPROTO_IGMP,
				  MVPP2_PRS_RI_CPU_CODE_RX_SPEC |
				  MVPP2_PRS_RI_UDF3_RX_SPECIAL,
				  MVPP2_PRS_RI_CPU_CODE_MASK |
				  MVPP2_PRS_RI_UDF3_MASK);
	if (err)
		return err;

	/* IPv4 Broadcast */
	err = mvpp2_prs_ip4_cast(priv, MVPP2_PRS_L3_BROAD_CAST);
	if (err)
		return err;

	/* IPv4 Multicast */
	err = mvpp2_prs_ip4_cast(priv, MVPP2_PRS_L3_MULTI_CAST);
	if (err)
		return err;

	/* Default IPv4 entry for unknown protocols */
	memset(&pe, 0, sizeof(pe));
	mvpp2_prs_tcam_lu_set(&pe, MVPP2_PRS_LU_IP4);
	pe.index = MVPP2_PE_IP4_PROTO_UN;

	/* Set next lu to IPv4 */
	mvpp2_prs_sram_next_lu_set(&pe, MVPP2_PRS_LU_IP4);
	mvpp2_prs_sram_shift_set(&pe, 12, MVPP2_PRS_SRAM_OP_SEL_SHIFT_ADD);
	/* Set L4 offset */
	mvpp2_prs_sram_offset_set(&pe, MVPP2_PRS_SRAM_UDF_TYPE_L4,
				  sizeof(struct iphdr) - 4,
				  MVPP2_PRS_SRAM_OP_SEL_UDF_ADD);
	mvpp2_prs_sram_ai_update(&pe, MVPP2_PRS_IPV4_DIP_AI_BIT,
				 MVPP2_PRS_IPV4_DIP_AI_BIT);
	mvpp2_prs_sram_ri_update(&pe, MVPP2_PRS_RI_L4_OTHER,
				 MVPP2_PRS_RI_L4_PROTO_MASK);

	mvpp2_prs_tcam_ai_update(&pe, 0, MVPP2_PRS_IPV4_DIP_AI_BIT);
	/* Unmask all ports */
	mvpp2_prs_tcam_port_map_set(&pe, MVPP2_PRS_PORT_MASK);

	/* Update shadow table and hw entry */
	mvpp2_prs_shadow_set(priv, pe.index, MVPP2_PRS_LU_IP4);
	mvpp2_prs_hw_write(priv, &pe);

	/* Default IPv4 entry for unicast address */
	memset(&pe, 0, sizeof(pe));
	mvpp2_prs_tcam_lu_set(&pe, MVPP2_PRS_LU_IP4);
	pe.index = MVPP2_PE_IP4_ADDR_UN;

	/* Finished: go to flowid generation */
	mvpp2_prs_sram_next_lu_set(&pe, MVPP2_PRS_LU_FLOWS);
	mvpp2_prs_sram_bits_set(&pe, MVPP2_PRS_SRAM_LU_GEN_BIT, 1);
	mvpp2_prs_sram_ri_update(&pe, MVPP2_PRS_RI_L3_UCAST,
				 MVPP2_PRS_RI_L3_ADDR_MASK);

	mvpp2_prs_tcam_ai_update(&pe, MVPP2_PRS_IPV4_DIP_AI_BIT,
				 MVPP2_PRS_IPV4_DIP_AI_BIT);
	/* Unmask all ports */
	mvpp2_prs_tcam_port_map_set(&pe, MVPP2_PRS_PORT_MASK);

	/* Update shadow table and hw entry */
	mvpp2_prs_shadow_set(priv, pe.index, MVPP2_PRS_LU_IP4);
	mvpp2_prs_hw_write(priv, &pe);

	return 0;
}

/* Initialize entries for IPv6 */
static int mvpp2_prs_ip6_init(struct mvpp2 *priv)
{
	struct mvpp2_prs_entry pe;
	int tid, err;

	/* Set entries for TCP, UDP and ICMP over IPv6 */
	err = mvpp2_prs_ip6_proto(priv, IPPROTO_TCP,
				  MVPP2_PRS_RI_L4_TCP,
				  MVPP2_PRS_RI_L4_PROTO_MASK);
	if (err)
		return err;

	err = mvpp2_prs_ip6_proto(priv, IPPROTO_UDP,
				  MVPP2_PRS_RI_L4_UDP,
				  MVPP2_PRS_RI_L4_PROTO_MASK);
	if (err)
		return err;

	err = mvpp2_prs_ip6_proto(priv, IPPROTO_ICMPV6,
				  MVPP2_PRS_RI_CPU_CODE_RX_SPEC |
				  MVPP2_PRS_RI_UDF3_RX_SPECIAL,
				  MVPP2_PRS_RI_CPU_CODE_MASK |
				  MVPP2_PRS_RI_UDF3_MASK);
	if (err)
		return err;

	/* IPv4 is the last header. This is similar case as 6-TCP or 17-UDP */
	/* Result Info: UDF7=1, DS lite */
	err = mvpp2_prs_ip6_proto(priv, IPPROTO_IPIP,
				  MVPP2_PRS_RI_UDF7_IP6_LITE,
				  MVPP2_PRS_RI_UDF7_MASK);
	if (err)
		return err;

	/* IPv6 multicast */
	err = mvpp2_prs_ip6_cast(priv, MVPP2_PRS_L3_MULTI_CAST);
	if (err)
		return err;

	/* Entry for checking hop limit */
	tid = mvpp2_prs_tcam_first_free(priv, MVPP2_PE_FIRST_FREE_TID,
					MVPP2_PE_LAST_FREE_TID);
	if (tid < 0)
		return tid;

	memset(&pe, 0, sizeof(pe));
	mvpp2_prs_tcam_lu_set(&pe, MVPP2_PRS_LU_IP6);
	pe.index = tid;

	/* Finished: go to flowid generation */
	mvpp2_prs_sram_next_lu_set(&pe, MVPP2_PRS_LU_FLOWS);
	mvpp2_prs_sram_bits_set(&pe, MVPP2_PRS_SRAM_LU_GEN_BIT, 1);
	mvpp2_prs_sram_ri_update(&pe, MVPP2_PRS_RI_L3_UN |
				 MVPP2_PRS_RI_DROP_MASK,
				 MVPP2_PRS_RI_L3_PROTO_MASK |
				 MVPP2_PRS_RI_DROP_MASK);

	mvpp2_prs_tcam_data_byte_set(&pe, 1, 0x00, MVPP2_PRS_IPV6_HOP_MASK);
	mvpp2_prs_tcam_ai_update(&pe, MVPP2_PRS_IPV6_NO_EXT_AI_BIT,
				 MVPP2_PRS_IPV6_NO_EXT_AI_BIT);

	/* Update shadow table and hw entry */
	mvpp2_prs_shadow_set(priv, pe.index, MVPP2_PRS_LU_IP4);
	mvpp2_prs_hw_write(priv, &pe);

	/* Default IPv6 entry for unknown protocols */
	memset(&pe, 0, sizeof(pe));
	mvpp2_prs_tcam_lu_set(&pe, MVPP2_PRS_LU_IP6);
	pe.index = MVPP2_PE_IP6_PROTO_UN;

	/* Finished: go to flowid generation */
	mvpp2_prs_sram_next_lu_set(&pe, MVPP2_PRS_LU_FLOWS);
	mvpp2_prs_sram_bits_set(&pe, MVPP2_PRS_SRAM_LU_GEN_BIT, 1);
	mvpp2_prs_sram_ri_update(&pe, MVPP2_PRS_RI_L4_OTHER,
				 MVPP2_PRS_RI_L4_PROTO_MASK);
	/* Set L4 offset relatively to our current place */
	mvpp2_prs_sram_offset_set(&pe, MVPP2_PRS_SRAM_UDF_TYPE_L4,
				  sizeof(struct ipv6hdr) - 4,
				  MVPP2_PRS_SRAM_OP_SEL_UDF_ADD);

	mvpp2_prs_tcam_ai_update(&pe, MVPP2_PRS_IPV6_NO_EXT_AI_BIT,
				 MVPP2_PRS_IPV6_NO_EXT_AI_BIT);
	/* Unmask all ports */
	mvpp2_prs_tcam_port_map_set(&pe, MVPP2_PRS_PORT_MASK);

	/* Update shadow table and hw entry */
	mvpp2_prs_shadow_set(priv, pe.index, MVPP2_PRS_LU_IP4);
	mvpp2_prs_hw_write(priv, &pe);

	/* Default IPv6 entry for unknown ext protocols */
	memset(&pe, 0, sizeof(struct mvpp2_prs_entry));
	mvpp2_prs_tcam_lu_set(&pe, MVPP2_PRS_LU_IP6);
	pe.index = MVPP2_PE_IP6_EXT_PROTO_UN;

	/* Finished: go to flowid generation */
	mvpp2_prs_sram_next_lu_set(&pe, MVPP2_PRS_LU_FLOWS);
	mvpp2_prs_sram_bits_set(&pe, MVPP2_PRS_SRAM_LU_GEN_BIT, 1);
	mvpp2_prs_sram_ri_update(&pe, MVPP2_PRS_RI_L4_OTHER,
				 MVPP2_PRS_RI_L4_PROTO_MASK);

	mvpp2_prs_tcam_ai_update(&pe, MVPP2_PRS_IPV6_EXT_AI_BIT,
				 MVPP2_PRS_IPV6_EXT_AI_BIT);
	/* Unmask all ports */
	mvpp2_prs_tcam_port_map_set(&pe, MVPP2_PRS_PORT_MASK);

	/* Update shadow table and hw entry */
	mvpp2_prs_shadow_set(priv, pe.index, MVPP2_PRS_LU_IP4);
	mvpp2_prs_hw_write(priv, &pe);

	/* Default IPv6 entry for unicast address */
	memset(&pe, 0, sizeof(struct mvpp2_prs_entry));
	mvpp2_prs_tcam_lu_set(&pe, MVPP2_PRS_LU_IP6);
	pe.index = MVPP2_PE_IP6_ADDR_UN;

	/* Finished: go to IPv6 again */
	mvpp2_prs_sram_next_lu_set(&pe, MVPP2_PRS_LU_IP6);
	mvpp2_prs_sram_ri_update(&pe, MVPP2_PRS_RI_L3_UCAST,
				 MVPP2_PRS_RI_L3_ADDR_MASK);
	mvpp2_prs_sram_ai_update(&pe, MVPP2_PRS_IPV6_NO_EXT_AI_BIT,
				 MVPP2_PRS_IPV6_NO_EXT_AI_BIT);
	/* Shift back to IPV6 NH */
	mvpp2_prs_sram_shift_set(&pe, -18, MVPP2_PRS_SRAM_OP_SEL_SHIFT_ADD);

	mvpp2_prs_tcam_ai_update(&pe, 0, MVPP2_PRS_IPV6_NO_EXT_AI_BIT);
	/* Unmask all ports */
	mvpp2_prs_tcam_port_map_set(&pe, MVPP2_PRS_PORT_MASK);

	/* Update shadow table and hw entry */
	mvpp2_prs_shadow_set(priv, pe.index, MVPP2_PRS_LU_IP6);
	mvpp2_prs_hw_write(priv, &pe);

	return 0;
}

/* Find tcam entry with matched pair <vid,port> */
static int mvpp2_prs_vid_range_find(struct mvpp2 *priv, int pmap, u16 vid,
				    u16 mask)
{
	struct mvpp2_prs_entry pe;
	unsigned char byte[2], enable[2];
	u16 rvid, rmask;
	int tid;

	/* Go through the all entries with MVPP2_PRS_LU_VID */
	for (tid = MVPP2_PE_VID_FILT_RANGE_START;
	     tid <= MVPP2_PE_VID_FILT_RANGE_END; tid++) {
		if (!priv->prs_shadow[tid].valid ||
		    priv->prs_shadow[tid].lu != MVPP2_PRS_LU_VID)
			continue;

		mvpp2_prs_init_from_hw(priv, &pe, tid);

		mvpp2_prs_tcam_data_byte_get(&pe, 2, &byte[0], &enable[0]);
		mvpp2_prs_tcam_data_byte_get(&pe, 3, &byte[1], &enable[1]);

		rvid = ((byte[0] & 0xf) << 8) + byte[1];
		rmask = ((enable[0] & 0xf) << 8) + enable[1];

		if (rvid != vid || rmask != mask)
			continue;

		return tid;
	}

	return -ENOENT;
}

/* Write parser entry for VID filtering */
static int mvpp2_prs_vid_entry_add(struct mvpp2_port *port, u16 vid)
{
	struct mvpp2 *priv = port->priv;
	struct mvpp2_prs_entry pe;
	int tid;
	unsigned int mask = 0xfff, reg_val, shift;
	unsigned int vid_start = MVPP2_PE_VID_FILT_RANGE_START +
				 port->id * MVPP2_PRS_VLAN_FILT_MAX;

	memset(&pe, 0, sizeof(pe));

	/* Scan TCAM and see if entry with this <vid,port> already exist */
	tid = mvpp2_prs_vid_range_find(priv, (1 << port->id), vid, mask);

	reg_val = mvpp2_read(priv, MVPP2_MH_REG(port->id));
	if (reg_val & MVPP2_DSA_EXTENDED)
		shift = MVPP2_VLAN_TAG_EDSA_LEN;
	else
		shift = MVPP2_VLAN_TAG_LEN;

	/* No such entry */
	if (tid < 0) {

		/* Go through all entries from first to last in vlan range */
		tid = mvpp2_prs_tcam_first_free(priv, vid_start,
						vid_start +
						MVPP2_PRS_VLAN_FILT_MAX_ENTRY);

		/* There isn't room for a new VID filter */
		if (tid < 0)
			return tid;

		mvpp2_prs_tcam_lu_set(&pe, MVPP2_PRS_LU_VID);
		pe.index = tid;

		/* Mask all ports */
		mvpp2_prs_tcam_port_map_set(&pe, 0);
	} else {
		mvpp2_prs_init_from_hw(priv, &pe, tid);
	}

	/* Enable the current port */
	mvpp2_prs_tcam_port_set(&pe, port->id, true);

	/* Continue - set next lookup */
	mvpp2_prs_sram_next_lu_set(&pe, MVPP2_PRS_LU_L2);

	/* Skip VLAN header - Set offset to 4 or 8 bytes */
	mvpp2_prs_sram_shift_set(&pe, shift, MVPP2_PRS_SRAM_OP_SEL_SHIFT_ADD);

	/* Set match on VID */
	mvpp2_prs_match_vid(&pe, MVPP2_PRS_VID_TCAM_BYTE, vid);

	/* Clear all ai bits for next iteration */
	mvpp2_prs_sram_ai_update(&pe, 0, MVPP2_PRS_SRAM_AI_MASK);

	/* Update shadow table */
	mvpp2_prs_shadow_set(priv, pe.index, MVPP2_PRS_LU_VID);
	mvpp2_prs_hw_write(priv, &pe);

	return 0;
}

/* Write parser entry for VID filtering */
static void mvpp2_prs_vid_entry_remove(struct mvpp2_port *port, u16 vid)
{
	struct mvpp2 *priv = port->priv;
	int tid;

	/* Scan TCAM and see if entry with this <vid,port> already exist */
	tid = mvpp2_prs_vid_range_find(priv, (1 << port->id), vid, 0xfff);

	/* No such entry */
	if (tid < 0)
		return;

	mvpp2_prs_hw_inv(priv, tid);
	priv->prs_shadow[tid].valid = false;
}

/* Remove all existing VID filters on this port */
static void mvpp2_prs_vid_remove_all(struct mvpp2_port *port)
{
	struct mvpp2 *priv = port->priv;
	int tid;

	for (tid = MVPP2_PRS_VID_PORT_FIRST(port->id);
	     tid <= MVPP2_PRS_VID_PORT_LAST(port->id); tid++) {
		if (priv->prs_shadow[tid].valid)
			mvpp2_prs_vid_entry_remove(port, tid);
	}
}

/* Remove VID filering entry for this port */
static void mvpp2_prs_vid_disable_filtering(struct mvpp2_port *port)
{
	struct mvpp2 *priv = port->priv;
	unsigned int tid = MVPP2_PRS_VID_PORT_DFLT(port->id);

	/* Invalidate the guard entry */
	mvpp2_prs_hw_inv(priv, tid);

	priv->prs_shadow[tid].valid = false;
}

/* Add guard entry that drops packets when no VID is matched on this port */
static void mvpp2_prs_vid_enable_filtering(struct mvpp2_port *port)
{
	struct mvpp2_prs_entry pe;
	struct mvpp2 *priv = port->priv;
	unsigned int tid = MVPP2_PRS_VID_PORT_DFLT(port->id);
	unsigned int reg_val, shift;

	if (priv->prs_shadow[tid].valid)
		return;

	memset(&pe, 0, sizeof(pe));

	pe.index = tid;

	reg_val = mvpp2_read(priv, MVPP2_MH_REG(port->id));
	if (reg_val & MVPP2_DSA_EXTENDED)
		shift = MVPP2_VLAN_TAG_EDSA_LEN;
	else
		shift = MVPP2_VLAN_TAG_LEN;

	mvpp2_prs_tcam_lu_set(&pe, MVPP2_PRS_LU_VID);

	/* Mask all ports */
	mvpp2_prs_tcam_port_map_set(&pe, 0);

	/* Update port mask */
	mvpp2_prs_tcam_port_set(&pe, port->id, true);

	/* Continue - set next lookup */
	mvpp2_prs_sram_next_lu_set(&pe, MVPP2_PRS_LU_L2);

	/* Skip VLAN header - Set offset to 4 or 8 bytes */
	mvpp2_prs_sram_shift_set(&pe, shift, MVPP2_PRS_SRAM_OP_SEL_SHIFT_ADD);

	/* Drop VLAN packets that don't belong to any VIDs on this port */
	mvpp2_prs_sram_ri_update(&pe, MVPP2_PRS_RI_DROP_MASK,
				 MVPP2_PRS_RI_DROP_MASK);

	/* Clear all ai bits for next iteration */
	mvpp2_prs_sram_ai_update(&pe, 0, MVPP2_PRS_SRAM_AI_MASK);

	/* Update shadow table */
	mvpp2_prs_shadow_set(priv, pe.index, MVPP2_PRS_LU_VID);
	mvpp2_prs_hw_write(priv, &pe);
}

/* Parser default initialization */
static int mvpp2_prs_default_init(struct platform_device *pdev,
				  struct mvpp2 *priv)
{
	int err, index, i;

	/* Enable tcam table */
	mvpp2_write(priv, MVPP2_PRS_TCAM_CTRL_REG, MVPP2_PRS_TCAM_EN_MASK);

	/* Clear all tcam and sram entries */
	for (index = 0; index < MVPP2_PRS_TCAM_SRAM_SIZE; index++) {
		mvpp2_write(priv, MVPP2_PRS_TCAM_IDX_REG, index);
		for (i = 0; i < MVPP2_PRS_TCAM_WORDS; i++)
			mvpp2_write(priv, MVPP2_PRS_TCAM_DATA_REG(i), 0);

		mvpp2_write(priv, MVPP2_PRS_SRAM_IDX_REG, index);
		for (i = 0; i < MVPP2_PRS_SRAM_WORDS; i++)
			mvpp2_write(priv, MVPP2_PRS_SRAM_DATA_REG(i), 0);
	}

	/* Invalidate all tcam entries */
	for (index = 0; index < MVPP2_PRS_TCAM_SRAM_SIZE; index++)
		mvpp2_prs_hw_inv(priv, index);

	priv->prs_shadow = devm_kcalloc(&pdev->dev, MVPP2_PRS_TCAM_SRAM_SIZE,
					sizeof(*priv->prs_shadow),
					GFP_KERNEL);
	if (!priv->prs_shadow)
		return -ENOMEM;

	/* Always start from lookup = 0 */
	for (index = 0; index < MVPP2_MAX_PORTS; index++)
		mvpp2_prs_hw_port_init(priv, index, MVPP2_PRS_LU_MH,
				       MVPP2_PRS_PORT_LU_MAX, 0);

	mvpp2_prs_def_flow_init(priv);

	mvpp2_prs_mh_init(priv);

	mvpp2_prs_mac_init(priv);

	mvpp2_prs_dsa_init(priv);

	mvpp2_prs_vid_init(priv);

	err = mvpp2_prs_etype_init(priv);
	if (err)
		return err;

	err = mvpp2_prs_vlan_init(pdev, priv);
	if (err)
		return err;

	err = mvpp2_prs_pppoe_init(priv);
	if (err)
		return err;

	err = mvpp2_prs_ip6_init(priv);
	if (err)
		return err;

	err = mvpp2_prs_ip4_init(priv);
	if (err)
		return err;

	return 0;
}

/* Compare MAC DA with tcam entry data */
static bool mvpp2_prs_mac_range_equals(struct mvpp2_prs_entry *pe,
				       const u8 *da, unsigned char *mask)
{
	unsigned char tcam_byte, tcam_mask;
	int index;

	for (index = 0; index < ETH_ALEN; index++) {
		mvpp2_prs_tcam_data_byte_get(pe, index, &tcam_byte, &tcam_mask);
		if (tcam_mask != mask[index])
			return false;

		if ((tcam_mask & tcam_byte) != (da[index] & mask[index]))
			return false;
	}

	return true;
}

/* Find tcam entry with matched pair <MAC DA, port> */
static int
mvpp2_prs_mac_da_range_find(struct mvpp2 *priv, int pmap, const u8 *da,
			    unsigned char *mask, int udf_type)
{
	struct mvpp2_prs_entry pe;
	int tid;

	/* Go through the all entires with MVPP2_PRS_LU_MAC */
	for (tid = MVPP2_PE_MAC_RANGE_START;
	     tid <= MVPP2_PE_MAC_RANGE_END; tid++) {
		unsigned int entry_pmap;

		if (!priv->prs_shadow[tid].valid ||
		    priv->prs_shadow[tid].lu != MVPP2_PRS_LU_MAC ||
		    priv->prs_shadow[tid].udf != udf_type)
			continue;

		mvpp2_prs_init_from_hw(priv, &pe, tid);
		entry_pmap = mvpp2_prs_tcam_port_map_get(&pe);

		if (mvpp2_prs_mac_range_equals(&pe, da, mask) &&
		    entry_pmap == pmap)
			return tid;
	}

	return -ENOENT;
}

/* Update parser's mac da entry */
static int mvpp2_prs_mac_da_accept(struct mvpp2_port *port, const u8 *da,
				   bool add)
{
	unsigned char mask[ETH_ALEN] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
	struct mvpp2 *priv = port->priv;
	unsigned int pmap, len, ri;
	struct mvpp2_prs_entry pe;
	int tid;

	memset(&pe, 0, sizeof(pe));

	/* Scan TCAM and see if entry with this <MAC DA, port> already exist */
	tid = mvpp2_prs_mac_da_range_find(priv, BIT(port->id), da, mask,
					  MVPP2_PRS_UDF_MAC_DEF);

	/* No such entry */
	if (tid < 0) {
		if (!add)
			return 0;

		/* Create new TCAM entry */
		/* Go through the all entries from first to last */
		tid = mvpp2_prs_tcam_first_free(priv,
						MVPP2_PE_MAC_RANGE_START,
						MVPP2_PE_MAC_RANGE_END);
		if (tid < 0)
			return tid;

		pe.index = tid;

		/* Mask all ports */
		mvpp2_prs_tcam_port_map_set(&pe, 0);
	} else {
		mvpp2_prs_init_from_hw(priv, &pe, tid);
	}

	mvpp2_prs_tcam_lu_set(&pe, MVPP2_PRS_LU_MAC);

	/* Update port mask */
	mvpp2_prs_tcam_port_set(&pe, port->id, add);

	/* Invalidate the entry if no ports are left enabled */
	pmap = mvpp2_prs_tcam_port_map_get(&pe);
	if (pmap == 0) {
		if (add)
			return -EINVAL;

		mvpp2_prs_hw_inv(priv, pe.index);
		priv->prs_shadow[pe.index].valid = false;
		return 0;
	}

	/* Continue - set next lookup */
	mvpp2_prs_sram_next_lu_set(&pe, MVPP2_PRS_LU_DSA);

	/* Set match on DA */
	len = ETH_ALEN;
	while (len--)
		mvpp2_prs_tcam_data_byte_set(&pe, len, da[len], 0xff);

	/* Set result info bits */
	if (is_broadcast_ether_addr(da)) {
		ri = MVPP2_PRS_RI_L2_BCAST;
	} else if (is_multicast_ether_addr(da)) {
		ri = MVPP2_PRS_RI_L2_MCAST;
	} else {
		ri = MVPP2_PRS_RI_L2_UCAST;

		if (ether_addr_equal(da, port->dev->dev_addr))
			ri |= MVPP2_PRS_RI_MAC_ME_MASK;
	}

	mvpp2_prs_sram_ri_update(&pe, ri, MVPP2_PRS_RI_L2_CAST_MASK |
				 MVPP2_PRS_RI_MAC_ME_MASK);
	mvpp2_prs_shadow_ri_set(priv, pe.index, ri, MVPP2_PRS_RI_L2_CAST_MASK |
				MVPP2_PRS_RI_MAC_ME_MASK);

	/* Shift to ethertype */
	mvpp2_prs_sram_shift_set(&pe, 2 * ETH_ALEN,
				 MVPP2_PRS_SRAM_OP_SEL_SHIFT_ADD);

	/* Update shadow table and hw entry */
	priv->prs_shadow[pe.index].udf = MVPP2_PRS_UDF_MAC_DEF;
	mvpp2_prs_shadow_set(priv, pe.index, MVPP2_PRS_LU_MAC);
	mvpp2_prs_hw_write(priv, &pe);

	return 0;
}

static int mvpp2_prs_update_mac_da(struct net_device *dev, const u8 *da)
{
	struct mvpp2_port *port = netdev_priv(dev);
	int err;

	/* Remove old parser entry */
	err = mvpp2_prs_mac_da_accept(port, dev->dev_addr, false);
	if (err)
		return err;

	/* Add new parser entry */
	err = mvpp2_prs_mac_da_accept(port, da, true);
	if (err)
		return err;

	/* Set addr in the device */
	ether_addr_copy(dev->dev_addr, da);

	return 0;
}

static void mvpp2_prs_mac_del_all(struct mvpp2_port *port)
{
	struct mvpp2 *priv = port->priv;
	struct mvpp2_prs_entry pe;
	unsigned long pmap;
	int index, tid;

	for (tid = MVPP2_PE_MAC_RANGE_START;
	     tid <= MVPP2_PE_MAC_RANGE_END; tid++) {
		unsigned char da[ETH_ALEN], da_mask[ETH_ALEN];

		if (!priv->prs_shadow[tid].valid ||
		    priv->prs_shadow[tid].lu != MVPP2_PRS_LU_MAC ||
		    priv->prs_shadow[tid].udf != MVPP2_PRS_UDF_MAC_DEF)
			continue;

		mvpp2_prs_init_from_hw(priv, &pe, tid);

		pmap = mvpp2_prs_tcam_port_map_get(&pe);

		/* We only want entries active on this port */
		if (!test_bit(port->id, &pmap))
			continue;

		/* Read mac addr from entry */
		for (index = 0; index < ETH_ALEN; index++)
			mvpp2_prs_tcam_data_byte_get(&pe, index, &da[index],
						     &da_mask[index]);

		/* Special cases : Don't remove broadcast and port's own
		 * address
		 */
		if (is_broadcast_ether_addr(da) ||
		    ether_addr_equal(da, port->dev->dev_addr))
			continue;

		/* Remove entry from TCAM */
		mvpp2_prs_mac_da_accept(port, da, false);
	}
}

static int mvpp2_prs_tag_mode_set(struct mvpp2 *priv, int port, int type)
{
	switch (type) {
	case MVPP2_TAG_TYPE_EDSA:
		/* Add port to EDSA entries */
		mvpp2_prs_dsa_tag_set(priv, port, true,
				      MVPP2_PRS_TAGGED, MVPP2_PRS_EDSA);
		mvpp2_prs_dsa_tag_set(priv, port, true,
				      MVPP2_PRS_UNTAGGED, MVPP2_PRS_EDSA);
		/* Remove port from DSA entries */
		mvpp2_prs_dsa_tag_set(priv, port, false,
				      MVPP2_PRS_TAGGED, MVPP2_PRS_DSA);
		mvpp2_prs_dsa_tag_set(priv, port, false,
				      MVPP2_PRS_UNTAGGED, MVPP2_PRS_DSA);
		break;

	case MVPP2_TAG_TYPE_DSA:
		/* Add port to DSA entries */
		mvpp2_prs_dsa_tag_set(priv, port, true,
				      MVPP2_PRS_TAGGED, MVPP2_PRS_DSA);
		mvpp2_prs_dsa_tag_set(priv, port, true,
				      MVPP2_PRS_UNTAGGED, MVPP2_PRS_DSA);
		/* Remove port from EDSA entries */
		mvpp2_prs_dsa_tag_set(priv, port, false,
				      MVPP2_PRS_TAGGED, MVPP2_PRS_EDSA);
		mvpp2_prs_dsa_tag_set(priv, port, false,
				      MVPP2_PRS_UNTAGGED, MVPP2_PRS_EDSA);
		break;

	case MVPP2_TAG_TYPE_MH:
	case MVPP2_TAG_TYPE_NONE:
		/* Remove port form EDSA and DSA entries */
		mvpp2_prs_dsa_tag_set(priv, port, false,
				      MVPP2_PRS_TAGGED, MVPP2_PRS_DSA);
		mvpp2_prs_dsa_tag_set(priv, port, false,
				      MVPP2_PRS_UNTAGGED, MVPP2_PRS_DSA);
		mvpp2_prs_dsa_tag_set(priv, port, false,
				      MVPP2_PRS_TAGGED, MVPP2_PRS_EDSA);
		mvpp2_prs_dsa_tag_set(priv, port, false,
				      MVPP2_PRS_UNTAGGED, MVPP2_PRS_EDSA);
		break;

	default:
		if (type < 0 || type > MVPP2_TAG_TYPE_EDSA)
			return -EINVAL;
	}

	return 0;
}

/* Classifier configuration routines */
static void mvpp2_cls_sw_lkp_rxq_set(struct mvpp2_cls_lookup_entry *lkp,
				     int rxq)
{
	lkp->data &= ~MVPP2_FLOWID_RXQ_MASK;
	lkp->data |= rxq << MVPP2_FLOWID_RXQ_OFFS;
}

static void mvpp2_cls_sw_lkp_en_set(struct mvpp2_cls_lookup_entry *lkp, int en)
{
	lkp->data &= ~MVPP2_FLOWID_EN_MASK;
	lkp->data |= en << MVPP2_FLOWID_EN_OFFS;
}

static void mvpp2_cls_sw_lkp_flow_get(struct mvpp2_cls_lookup_entry *lkp,
				      int *flow_idx)
{
	*flow_idx = (lkp->data & MVPP2_FLOWID_FLOW_MASK) >> MVPP2_FLOWID_FLOW;
}

static void mvpp2_cls_sw_lkp_flow_set(struct mvpp2_cls_lookup_entry *lkp,
				      int flow_idx)
{
	lkp->data &= ~MVPP2_FLOWID_FLOW_MASK;
	lkp->data |= flow_idx << MVPP2_FLOWID_FLOW;
}

/* Update classification flow table registers */
static void mvpp2_cls_flow_write(struct mvpp2 *priv,
				 struct mvpp2_cls_flow_entry *fe)
{
	mvpp2_write(priv, MVPP2_CLS_FLOW_INDEX_REG, fe->index);
	mvpp2_write(priv, MVPP2_CLS_FLOW_TBL0_REG,  fe->data[0]);
	mvpp2_write(priv, MVPP2_CLS_FLOW_TBL1_REG,  fe->data[1]);
	mvpp2_write(priv, MVPP2_CLS_FLOW_TBL2_REG,  fe->data[2]);
}

/* Update classification lookup table register */
static void mvpp2_cls_lookup_write(struct mvpp2 *priv,
				   struct mvpp2_cls_lookup_entry *le)
{
	u32 val;

	val = (le->way << MVPP2_CLS_LKP_INDEX_WAY_OFFS) | le->lkpid;
	mvpp2_write(priv, MVPP2_CLS_LKP_INDEX_REG, val);
	mvpp2_write(priv, MVPP2_CLS_LKP_TBL_REG, le->data);
}

static void mvpp2_cls_lookup_read(struct mvpp2 *priv, int lkpid, int way,
				  struct mvpp2_cls_lookup_entry *le)
{
	unsigned int val = 0;

	/* write index reg */
	val = (way << MVPP2_CLS_LKP_INDEX_WAY_OFFS) | lkpid;
	mvpp2_write(priv, MVPP2_CLS_LKP_INDEX_REG, val);
	le->way = way;
	le->lkpid = lkpid;
	le->data = mvpp2_read(priv, MVPP2_CLS_LKP_TBL_REG);
}

/* Classifier flows table APIs */
static void mvpp2_cls_sw_flow_engine_get(struct mvpp2_cls_flow_entry *fe,
					 int *engine, int *is_last)
{
	*engine = (fe->data[0] & MVPP2_FLOW_ENGINE_MASK) >> MVPP2_FLOW_ENGINE;
	*is_last = fe->data[0] & MVPP2_FLOW_LAST_MASK;
}

static void mvpp2_cls_sw_flow_extra_get(struct mvpp2_cls_flow_entry *fe,
					int *type, int *prio)
{
	*type = (fe->data[1] & MVPP2_FLOW_LKP_TYPE_MASK) >> MVPP2_FLOW_LKP_TYPE;
	*prio = (fe->data[1] & MVPP2_FLOW_FIELD_PRIO_MASK)
		>> MVPP2_FLOW_FIELD_PRIO;
}

static void mvpp2_cls_sw_flow_extra_set(struct mvpp2_cls_flow_entry *fe,
					int type, int prio)
{
	fe->data[1] &= ~MVPP2_FLOW_LKP_TYPE_MASK;
	fe->data[1] |= (type << MVPP2_FLOW_LKP_TYPE);
	fe->data[1] &= ~MVPP2_FLOW_FIELD_PRIO_MASK;
	fe->data[1] |= (prio << MVPP2_FLOW_FIELD_PRIO);
}

static void mvpp2_cls_flow_read(struct mvpp2 *priv, int index,
				struct mvpp2_cls_flow_entry *fe)
{
	fe->index = index;
	/* write index */
	mvpp2_write(priv, MVPP2_CLS_FLOW_INDEX_REG, index);
	fe->data[0] = mvpp2_read(priv, MVPP2_CLS_FLOW_TBL0_REG);
	fe->data[1] = mvpp2_read(priv, MVPP2_CLS_FLOW_TBL1_REG);
	fe->data[2] = mvpp2_read(priv, MVPP2_CLS_FLOW_TBL2_REG);
}

/* Operations on flow entry */
static void mvpp2_cls_sw_flow_hek_num_set(struct mvpp2_cls_flow_entry *fe,
					  int num_of_fields)
{
	fe->data[1] &= ~MVPP2_FLOW_FIELDS_NUM_MASK;
	fe->data[1] |= (num_of_fields << MVPP2_FLOW_FIELDS_NUM);
}

static int mvpp2_cls_sw_flow_hek_set(struct mvpp2_cls_flow_entry *fe,
				     int field_index, int field_id)
{
	int num_of_fields;

	/* get current num_of_fields */
	num_of_fields = ((fe->data[1] &
		MVPP2_FLOW_FIELDS_NUM_MASK) >> MVPP2_FLOW_FIELDS_NUM);

	if (num_of_fields < (field_index + 1)) {
		pr_debug("%s:num of heks=%d ,idx(%d) out of range\n",
			 __func__, num_of_fields, field_index);
		return -1;
	}

	fe->data[2] &= ~MVPP2_FLOW_FIELD_MASK(field_index);
	fe->data[2] |= (field_id <<  MVPP2_FLOW_FIELD_ID(field_index));

	return 0;
}

static void mvpp2_cls_sw_flow_eng_set(struct mvpp2_cls_flow_entry *fe,
				      int engine, bool is_last)
{
	fe->data[0] &= ~MVPP2_FLOW_LAST_MASK;
	fe->data[0] &= ~MVPP2_FLOW_ENGINE_MASK;
	fe->data[0] |= is_last;
	fe->data[0] |= engine << MVPP2_FLOW_ENGINE;
	fe->data[0] |= MVPP2_FLOW_PORT_ID_SEL_MASK;
}

/* To init flow table according to different flow */
static void mvpp2_cls_flow_cos(struct mvpp2 *priv,
			       struct mvpp2_cls_flow_entry *fe,
			       int lkpid, int cos_type)
{
	int hek_num, field_id, lkp_type, is_last;
	int index = priv->cls_shadow->flow_free_start;

	switch (cos_type) {
	case MVPP2_COS_TYPE_VLAN:
		lkp_type = MVPP2_CLS_LKP_VLAN_PRI;
		break;
	case MVPP2_COS_TYPE_DSCP:
		lkp_type = MVPP2_CLS_LKP_DSCP_PRI;
		break;
	default:
		lkp_type = MVPP2_CLS_LKP_DEFAULT;
		break;
	}
	hek_num = 0;
	if ((lkpid == MVPP2_PRS_FL_NON_IP_UNTAG &&
	     cos_type == MVPP2_COS_TYPE_DFLT) ||
	    (lkpid == MVPP2_PRS_FL_NON_IP_TAG &&
		cos_type == MVPP2_COS_TYPE_VLAN))
		is_last = 1;
	else
		is_last = 0;

	/* Set SW */
	memset(fe, 0, sizeof(struct mvpp2_cls_flow_entry));
	mvpp2_cls_sw_flow_hek_num_set(fe, hek_num);
	if (hek_num)
		mvpp2_cls_sw_flow_hek_set(fe, 0, field_id);
	mvpp2_cls_sw_flow_eng_set(fe, MVPP2_CLS_ENGINE_C2, is_last);
	mvpp2_cls_sw_flow_extra_set(fe, lkp_type, MVPP2_CLS_FL_COS_PRI);
	fe->index = index;

	/* Write HW */
	mvpp2_cls_flow_write(priv, fe);

	/* Update Shadow */
	priv->cls_shadow->flow_info[lkpid -
			MVPP2_PRS_FL_START].flow_entry[cos_type] = index;
	/* Update first available flow entry */
	priv->cls_shadow->flow_free_start++;
}

/* Init flow entry for RSS hash in PP22 */
static void mvpp2_cls_flow_rss_hash(struct mvpp2 *priv,
				    struct mvpp2_cls_flow_entry *fe,
				    int lkpid, int rss_mode)
{
	int field_id[4] = {0};
	int entry_idx = priv->cls_shadow->flow_free_start;
	int lkpid_attr = mvpp2_prs_flow_id_attr_get(lkpid);

	/* IP4 packet */
	if (lkpid_attr & MVPP2_PRS_FL_ATTR_IP4_BIT) {
		field_id[0] = MVPP2_CLS_FIELD_IP4SA;
		field_id[1] = MVPP2_CLS_FIELD_IP4DA;
	} else if (lkpid_attr & MVPP2_PRS_FL_ATTR_IP6_BIT) {
		field_id[0] = MVPP2_CLS_FIELD_IP6SA;
		field_id[1] = MVPP2_CLS_FIELD_IP6DA;
	}
	/* L4 port */
	field_id[2] = MVPP2_CLS_FIELD_L4SIP;
	field_id[3] = MVPP2_CLS_FIELD_L4DIP;

	/* Set SW */
	memset(fe, 0, sizeof(struct mvpp2_cls_flow_entry));
	if (rss_mode == MVPP22_RSS_2T) {
		mvpp2_cls_sw_flow_hek_num_set(fe, 2);
		mvpp2_cls_sw_flow_eng_set(fe, MVPP2_CLS_ENGINE_C3HA, 1);
		mvpp2_cls_sw_flow_hek_set(fe, 0, field_id[0]);
		mvpp2_cls_sw_flow_hek_set(fe, 1, field_id[1]);
	} else {
		mvpp2_cls_sw_flow_hek_num_set(fe, 4);
		mvpp2_cls_sw_flow_hek_set(fe, 0, field_id[0]);
		mvpp2_cls_sw_flow_hek_set(fe, 1, field_id[1]);
		mvpp2_cls_sw_flow_hek_set(fe, 2, field_id[2]);
		mvpp2_cls_sw_flow_hek_set(fe, 3, field_id[3]);
		mvpp2_cls_sw_flow_eng_set(fe, MVPP2_CLS_ENGINE_C3HB, 1);
	}
	mvpp2_cls_sw_flow_extra_set(fe,
				    MVPP2_CLS_LKP_HASH, MVPP2_CLS_FL_RSS_PRI);
	fe->index = entry_idx;

	/* Update last for TCP & UDP NF flow */
	if (((lkpid_attr &
	      (MVPP2_PRS_FL_ATTR_TCP_BIT | MVPP2_PRS_FL_ATTR_UDP_BIT)) &&
	     !(lkpid_attr & MVPP2_PRS_FL_ATTR_FRAG_BIT))) {
		if (!priv->cls_shadow->flow_info[lkpid -
			MVPP2_PRS_FL_START].flow_entry_rss1) {
			int engine_c3 = (rss_mode == MVPP22_RSS_2T) ?
				MVPP2_CLS_ENGINE_C3HA : MVPP2_CLS_ENGINE_C3HB;

			mvpp2_cls_sw_flow_eng_set(fe, engine_c3, 0);
		}
	}

	/* Write HW */
	mvpp2_cls_flow_write(priv, fe);

	/* Update Shadow */
	if (priv->cls_shadow->flow_info[lkpid -
		MVPP2_PRS_FL_START].flow_entry_rss1 == 0)
		priv->cls_shadow->flow_info[lkpid -
			MVPP2_PRS_FL_START].flow_entry_rss1 = entry_idx;
	else
		priv->cls_shadow->flow_info[lkpid -
			MVPP2_PRS_FL_START].flow_entry_rss2 = entry_idx;

	/* Update first available flow entry */
	priv->cls_shadow->flow_free_start++;
}

/* Init cls flow table according to different flow id */
static void mvpp2_cls_flow_tbl_config(struct mvpp2 *priv)
{
	int lkpid, rss_mode, lkpid_attr;
	struct mvpp2_cls_flow_entry fe;

	for (lkpid = MVPP2_PRS_FL_START; lkpid < MVPP2_PRS_FL_LAST; lkpid++) {
		/* Get lookup id attribute */
		lkpid_attr = mvpp2_prs_flow_id_attr_get(lkpid);
		/* Default rss hash is based on 5T */
		rss_mode = rss_mode ? MVPP22_RSS_2T : MVPP22_RSS_5T;
		/* For frag packets or non-TCP&UDP, rss must be based on 2T */
		if ((lkpid_attr & MVPP2_PRS_FL_ATTR_FRAG_BIT) ||
		    !(lkpid_attr & (MVPP2_PRS_FL_ATTR_TCP_BIT |
		    MVPP2_PRS_FL_ATTR_UDP_BIT)))
			rss_mode = MVPP22_RSS_2T;

		/* For untagged IP packets, only need default
		 * rule and dscp rule
		 */
		if ((lkpid_attr & (MVPP2_PRS_FL_ATTR_IP4_BIT |
		     MVPP2_PRS_FL_ATTR_IP6_BIT)) &&
		    (!(lkpid_attr & MVPP2_PRS_FL_ATTR_VLAN_BIT))) {
			/* Default rule */
			mvpp2_cls_flow_cos(priv, &fe, lkpid,
					   MVPP2_COS_TYPE_DFLT);
			/* DSCP rule */
			mvpp2_cls_flow_cos(priv, &fe, lkpid,
					   MVPP2_COS_TYPE_DSCP);
			/* RSS hash rule */
			if ((!(lkpid_attr & MVPP2_PRS_FL_ATTR_FRAG_BIT)) &&
			    (lkpid_attr & (MVPP2_PRS_FL_ATTR_TCP_BIT |
				MVPP2_PRS_FL_ATTR_UDP_BIT))) {
				/* RSS hash rules for TCP/UDP rss mode update*/
				mvpp2_cls_flow_rss_hash(priv, &fe, lkpid,
							MVPP22_RSS_2T);
				mvpp2_cls_flow_rss_hash(priv, &fe, lkpid,
							MVPP22_RSS_5T);
			} else {
				mvpp2_cls_flow_rss_hash(priv, &fe, lkpid,
							rss_mode);
			}
		}

		/* For tagged IP packets, only need vlan rule and dscp rule */
		if ((lkpid_attr & (MVPP2_PRS_FL_ATTR_IP4_BIT |
		    MVPP2_PRS_FL_ATTR_IP6_BIT)) &&
		    (lkpid_attr & MVPP2_PRS_FL_ATTR_VLAN_BIT)) {
			/* VLAN rule */
			mvpp2_cls_flow_cos(priv, &fe, lkpid,
					   MVPP2_COS_TYPE_VLAN);
			/* DSCP rule */
			mvpp2_cls_flow_cos(priv, &fe, lkpid,
					   MVPP2_COS_TYPE_DSCP);
			/* RSS hash rule */
			if ((!(lkpid_attr & MVPP2_PRS_FL_ATTR_FRAG_BIT)) &&
			    (lkpid_attr & (MVPP2_PRS_FL_ATTR_TCP_BIT |
				MVPP2_PRS_FL_ATTR_UDP_BIT))) {
				/* TCP & UDP rss mode update */
				mvpp2_cls_flow_rss_hash(priv, &fe, lkpid,
							MVPP22_RSS_2T);
				mvpp2_cls_flow_rss_hash(priv, &fe, lkpid,
							MVPP22_RSS_5T);
			} else {
				mvpp2_cls_flow_rss_hash(priv, &fe, lkpid,
							rss_mode);
			}
		}

		/* For non-IP packets, only need default rule if untagged,
		 * vlan rule also needed if tagged
		 */
		if (!(lkpid_attr & (MVPP2_PRS_FL_ATTR_IP4_BIT |
		     MVPP2_PRS_FL_ATTR_IP6_BIT))) {
			/* Default rule */
			mvpp2_cls_flow_cos(priv, &fe, lkpid,
					   MVPP2_COS_TYPE_DFLT);
			/* VLAN rule if tagged */
			if (lkpid_attr & MVPP2_PRS_FL_ATTR_VLAN_BIT)
				mvpp2_cls_flow_cos(priv, &fe, lkpid,
						   MVPP2_COS_TYPE_VLAN);
		}
	}
}

/* Update the flow index for flow of lkpid */
static void mvpp2_cls_lkp_flow_set(struct mvpp2 *priv, int lkpid, int way,
				   int flow_idx)
{
	struct mvpp2_cls_lookup_entry le;

	mvpp2_cls_lookup_read(priv, lkpid, way, &le);
	mvpp2_cls_sw_lkp_flow_set(&le, flow_idx);
	mvpp2_cls_lookup_write(priv, &le);
}

/* Init lookup decoding table with lookup id */
static void mvpp2_cls_lookup_tbl_config(struct mvpp2 *priv)
{
	u32 index, flow_idx, i;
	struct mvpp2_cls_lookup_entry le;
	struct mvpp2_cls_flow_info *flow_info;

	memset(&le, 0, sizeof(struct mvpp2_cls_lookup_entry));
	/* Enable classifier engine */
	mvpp2_cls_sw_lkp_en_set(&le, 1);

	index = 0;
	while (index < (MVPP2_PRS_FL_LAST - MVPP2_PRS_FL_START)) {
		flow_info = &priv->cls_shadow->flow_info[index++];

		/* Find the min non-zero idx in flow_entry_dflt,
		 * flow_entry_vlan, and flow_entry_dscp
		 */
		flow_idx = MVPP2_FLOW_TBL_SIZE;
		for (i = 0; i < MVPP2_COS_TYPE_NUM; i++) {
			if (!flow_info->flow_entry[i])
				continue;
			flow_idx = min(flow_idx, flow_info->flow_entry[i]);
		}

		le.lkpid = flow_info->lkpid;
		/* Set flow pointer index */
		mvpp2_cls_sw_lkp_flow_set(&le, flow_idx);
		/* Set initial lkp rx queue */
		mvpp2_cls_sw_lkp_rxq_set(&le, 0x0);

		/* Update lookup ID table entry */
		le.way = 0;
		mvpp2_cls_lookup_write(priv, &le);
		le.way = 1;
		mvpp2_cls_lookup_write(priv, &le);
	}
}

/* Classifier default initialization */
static int mvpp2_cls_init(struct platform_device *pdev, struct mvpp2 *priv)
{
	struct mvpp2_cls_lookup_entry le;
	struct mvpp2_cls_flow_entry fe;
	int index;

	/* Enable classifier */
	mvpp2_write(priv, MVPP2_CLS_MODE_REG, MVPP2_CLS_MODE_ACTIVE_MASK);

	/* Clear classifier flow table */
	memset(&fe.data, 0, sizeof(fe.data));
	for (index = 0; index < MVPP2_FLOW_TBL_SIZE; index++) {
		fe.index = index;
		mvpp2_cls_flow_write(priv, &fe);
	}

	/* Clear classifier lookup table */
	le.data = 0;
	for (index = 0; index < MVPP2_CLS_LKP_TBL_SIZE; index++) {
		le.lkpid = index;
		le.way = 0;
		mvpp2_cls_lookup_write(priv, &le);

		le.way = 1;
		mvpp2_cls_lookup_write(priv, &le);
	}

	priv->cls_shadow = devm_kcalloc(&pdev->dev, 1,
					sizeof(struct mvpp2_cls_shadow),
					GFP_KERNEL);
	if (!priv->cls_shadow)
		return -ENOMEM;

	priv->cls_shadow->flow_info =
		devm_kcalloc(&pdev->dev,
			     (MVPP2_PRS_FL_LAST - MVPP2_PRS_FL_START),
			     sizeof(struct mvpp2_cls_flow_info), GFP_KERNEL);
	if (!priv->cls_shadow->flow_info)
		return -ENOMEM;

	/* Start from entry 1 to allocate flow table */
	priv->cls_shadow->flow_free_start = 1;
	priv->cls_shadow->flow_swap_area = MVPP2_CLS_FLOWS_TBL_SWAP_IDX;

	for (index = 0; index < (MVPP2_PRS_FL_LAST - MVPP2_PRS_FL_START);
		index++)
		priv->cls_shadow->flow_info[index].lkpid = index +
			MVPP2_PRS_FL_START;

	/* Init flow table */
	mvpp2_cls_flow_tbl_config(priv);

	/* Init lookup table */
	mvpp2_cls_lookup_tbl_config(priv);

	return 0;
}

static void mvpp2_cls_flow_port_add(struct mvpp2 *priv, int index, int port_id)
{
	u32 data;

	/* Write flow index */
	mvpp2_write(priv, MVPP2_CLS_FLOW_INDEX_REG, index);
	/* Read first data with port info */
	data = mvpp2_read(priv, MVPP2_CLS_FLOW_TBL0_REG);
	/* Add the port */
	data |= ((1 << port_id) << MVPP2_FLOW_PORT_ID);
	/* Update the register */
	mvpp2_write(priv, MVPP2_CLS_FLOW_TBL0_REG, data);
}

static void mvpp2_cls_flow_port_del(struct mvpp2 *priv, int index, int port_id)
{
	u32 data;

	/* Write flow index */
	mvpp2_write(priv, MVPP2_CLS_FLOW_INDEX_REG, index);
	/* Read first data with port info */
	data = mvpp2_read(priv, MVPP2_CLS_FLOW_TBL0_REG);
	/* Delete the port */
	data &= ~(((1 << port_id) << MVPP2_FLOW_PORT_ID));
	/* Update the register */
	mvpp2_write(priv, MVPP2_CLS_FLOW_TBL0_REG, data);
}

static int mvpp2_cls_flow_swap(struct mvpp2 *priv, int lkpid,
			       int *flow_idx, bool put_to_tmp)
{
	struct mvpp2_cls_flow_entry fe;
	struct mvpp2_cls_flow_info *flow_info;
	int index = lkpid - MVPP2_PRS_FL_START;
	int swap_idx;
	int i;

	if (!put_to_tmp) {
		/* Swap back to given index. Unlock swap-area */
		mvpp2_cls_lkp_flow_set(priv, lkpid, 0, *flow_idx);
		mvpp2_cls_lkp_flow_set(priv, lkpid, 1, *flow_idx);
		priv->cls_shadow->flow_swap_area =
			MVPP2_CLS_FLOWS_TBL_SWAP_IDX;
		return 0;
	}

	/* Prepare a temporary flow table for lkpid flow in swap-area
	 * and swap into it by flow_set()
	 */
	if (priv->cls_shadow->flow_swap_area != MVPP2_CLS_FLOWS_TBL_SWAP_IDX)
		return -EAGAIN;
	priv->cls_shadow->flow_swap_area = -1; /* ~lock */
	swap_idx = MVPP2_CLS_FLOWS_TBL_SWAP_IDX;
	*flow_idx = swap_idx;
	flow_info = &priv->cls_shadow->flow_info[index];

	for (i = 0; i < MVPP2_COS_TYPE_NUM; i++) {
		if (!flow_info->flow_entry[i])
			continue;
		mvpp2_cls_flow_read(priv, flow_info->flow_entry[i], &fe);
		fe.index = swap_idx++;
		mvpp2_cls_flow_write(priv, &fe);
	}
	if (flow_info->flow_entry_rss1) {
		mvpp2_cls_flow_read(priv, flow_info->flow_entry_rss1, &fe);
		fe.index = swap_idx++;
		mvpp2_cls_flow_write(priv, &fe);
	}
	if (flow_info->flow_entry_rss2) {
		mvpp2_cls_flow_read(priv, flow_info->flow_entry_rss2, &fe);
		fe.index = swap_idx++;
		mvpp2_cls_flow_write(priv, &fe);
	}
	mvpp2_cls_lkp_flow_set(priv, lkpid, 0, *flow_idx);
	mvpp2_cls_lkp_flow_set(priv, lkpid, 1, *flow_idx);
	return 0;
}

/* Classifier configuration routines */
/* Update classification lookup table register */
static void mvpp2_cls_port_config(struct mvpp2_port *port)
{
	struct mvpp2_cls_lookup_entry le;
	u32 val;

	/* Set way for the port */
	val = mvpp2_read(port->priv, MVPP2_CLS_PORT_WAY_REG);
	val &= ~MVPP2_CLS_PORT_WAY_MASK(port->id);
	mvpp2_write(port->priv, MVPP2_CLS_PORT_WAY_REG, val);

	/* Pick the entry to be accessed in lookup ID decoding table
	 * according to the way and lkpid.
	 */
	le.lkpid = port->id;
	le.way = 0;
	le.data = 0;

	/* Set initial CPU queue for receiving packets */
	le.data &= ~MVPP2_CLS_LKP_TBL_RXQ_MASK;
	le.data |= port->first_rxq;

	/* Disable classification engines */
	le.data &= ~MVPP2_CLS_LKP_TBL_LOOKUP_EN_MASK;

	/* Update lookup ID table entry */
	mvpp2_cls_lookup_write(port->priv, &le);
}

/* Set CPU queue number for oversize packets */
static void mvpp2_cls_oversize_rxq_set(struct mvpp2_port *port)
{
	u32 val;

	mvpp2_write(port->priv, MVPP2_CLS_OVERSIZE_RXQ_LOW_REG(port->id),
		    port->first_rxq & MVPP2_CLS_OVERSIZE_RXQ_LOW_MASK);

	mvpp2_write(port->priv, MVPP2_CLS_SWFWD_P2HQ_REG(port->id),
		    (port->first_rxq >> MVPP2_CLS_OVERSIZE_RXQ_LOW_BITS));

	val = mvpp2_read(port->priv, MVPP2_CLS_SWFWD_PCTRL_REG);
	val |= MVPP2_CLS_SWFWD_PCTRL_MASK(port->id);
	mvpp2_write(port->priv, MVPP2_CLS_SWFWD_PCTRL_REG, val);
}

/* Classifier Engine-C2 (_cls_c2_ and _c2_) */

static int mvpp2_cls_c2_hw_inv(struct mvpp2 *priv, int index)
{
	if (!priv || index >= MVPP2_CLS_C2_TCAM_SIZE)
		return -EINVAL;

	/* write index reg */
	mvpp2_write(priv, MVPP2_CLS2_TCAM_IDX_REG, index);

	/* set invalid bit*/
	mvpp2_write(priv, MVPP2_CLS2_TCAM_INV_REG, (1 <<
		MVPP2_CLS2_TCAM_INV_INVALID_OFF));

	/* trigger */
	mvpp2_write(priv, MVPP2_CLS2_TCAM_DATA_REG(4), 0);

	return 0;
}

static void mvpp2_cls_c2_hw_inv_all(struct mvpp2 *priv)
{
	int index;

	for (index = 0; index < MVPP2_CLS_C2_TCAM_SIZE; index++)
		mvpp2_cls_c2_hw_inv(priv, index);
}

/* C2 rule and Qos table */
static int mvpp2_cls_c2_hw_write(struct mvpp2 *priv, int index,
				 struct mvpp2_cls_c2_entry *c2)
{
	int tcm_idx;

	if (!c2 || index >= MVPP2_CLS_C2_TCAM_SIZE)
		return -EINVAL;

	c2->index = index;

	/* write index reg */
	mvpp2_write(priv, MVPP2_CLS2_TCAM_IDX_REG, index);

	mvpp2_cls_c2_hw_inv(priv, index);

	/* write action_tbl CLSC2_ACT_DATA */
	mvpp2_write(priv, MVPP2_CLS2_ACT_DATA_REG, c2->sram.regs.action_tbl);

	/* write actions CLSC2_ACT */
	mvpp2_write(priv, MVPP2_CLS2_ACT_REG, c2->sram.regs.actions);

	/* write qos_attr CLSC2_ATTR0 */
	mvpp2_write(priv, MVPP2_CLS2_ACT_QOS_ATTR_REG, c2->sram.regs.qos_attr);

	/* write hwf_attr CLSC2_ATTR1 */
	mvpp2_write(priv, MVPP2_CLS2_ACT_HWF_ATTR_REG, c2->sram.regs.hwf_attr);

	/* write rss_attr CLSC2_ATTR2 */
	mvpp2_write(priv, MVPP2_CLS2_ACT_DUP_ATTR_REG, c2->sram.regs.rss_attr);

	/* write valid bit*/
	c2->inv = 0;
	mvpp2_write(priv, MVPP2_CLS2_TCAM_INV_REG,
		    ((c2->inv) << MVPP2_CLS2_TCAM_INV_INVALID_OFF));

	for (tcm_idx = 0; tcm_idx < MVPP2_CLS_C2_TCAM_WORDS; tcm_idx++)
		mvpp2_write(priv, MVPP2_CLS2_TCAM_DATA_REG(tcm_idx),
			    c2->tcam.words[tcm_idx]);

	return 0;
}

static int mvpp2_cls_c2_qos_hw_write(struct mvpp2 *priv,
				     struct mvpp2_cls_c2_qos_entry *qos)
{
	unsigned int reg_val = 0;

	if (!qos || qos->tbl_sel > MVPP2_QOS_TBL_SEL_DSCP)
		return -EINVAL;

	if (qos->tbl_sel == MVPP2_QOS_TBL_SEL_DSCP) {
		/* dscp */
		if (qos->tbl_id >=  MVPP2_QOS_TBL_NUM_DSCP ||
		    qos->tbl_line >= MVPP2_QOS_TBL_LINE_NUM_DSCP)
			return -EINVAL;
	} else {
		/* pri */
		if (qos->tbl_id >=  MVPP2_QOS_TBL_NUM_PRI ||
		    qos->tbl_line >= MVPP2_QOS_TBL_LINE_NUM_PRI)
			return -EINVAL;
	}
	/* write index reg */
	reg_val |= (qos->tbl_line << MVPP2_CLS2_DSCP_PRI_INDEX_LINE_OFF);
	reg_val |= (qos->tbl_sel << MVPP2_CLS2_DSCP_PRI_INDEX_SEL_OFF);
	reg_val |= (qos->tbl_id << MVPP2_CLS2_DSCP_PRI_INDEX_TBL_ID_OFF);
	mvpp2_write(priv, MVPP2_CLS2_DSCP_PRI_INDEX_REG, reg_val);

	/* write data reg */
	mvpp2_write(priv, MVPP2_CLS2_QOS_TBL_REG, qos->data);

	return 0;
}

static void mvpp2_cls_c2_qos_hw_clear_all(struct mvpp2 *priv)
{
	struct mvpp2_cls_c2_qos_entry qos;

	memset(&qos, 0, sizeof(struct mvpp2_cls_c2_qos_entry));

	/* clear DSCP tables */
	qos.tbl_sel = MVPP2_QOS_TBL_SEL_DSCP;
	for (qos.tbl_id = 0; qos.tbl_id < MVPP2_QOS_TBL_NUM_DSCP;
		qos.tbl_id++) {
		for (qos.tbl_line = 0; qos.tbl_line <
			MVPP2_QOS_TBL_LINE_NUM_DSCP; qos.tbl_line++) {
			mvpp2_cls_c2_qos_hw_write(priv, &qos);
		}
	}

	/* clear PRIO tables */
	qos.tbl_sel = MVPP2_QOS_TBL_SEL_PRI;
	for (qos.tbl_id = 0; qos.tbl_id <
		MVPP2_QOS_TBL_NUM_PRI; qos.tbl_id++)
		for (qos.tbl_line = 0; qos.tbl_line <
			MVPP2_QOS_TBL_LINE_NUM_PRI; qos.tbl_line++) {
			mvpp2_cls_c2_qos_hw_write(priv, &qos);
		}
}

static int mvpp2_cls_c2_qos_tbl_set(struct mvpp2_cls_c2_entry *c2,
				    int tbl_id, int tbl_sel)
{
	if (!c2 || tbl_sel > 1)
		return -EINVAL;

	if (tbl_sel == 1) {
		/* dscp */
		if (tbl_id >= MVPP2_QOS_TBL_NUM_DSCP)
			return -EINVAL;
	} else {
		/* pri */
		if (tbl_id >= MVPP2_QOS_TBL_NUM_PRI)
			return -EINVAL;
	}
	c2->sram.regs.action_tbl = (tbl_id <<
			MVPP2_CLS2_ACT_DATA_TBL_ID_OFF) |
			(tbl_sel << MVPP2_CLS2_ACT_DATA_TBL_SEL_OFF);

	return 0;
}

static int mvpp2_cls_c2_color_set(struct mvpp2_cls_c2_entry *c2, int cmd,
				  int from)
{
	if (!c2 || cmd > MVPP2_COLOR_ACTION_TYPE_RED_LOCK)
		return -EINVAL;

	c2->sram.regs.actions &= ~MVPP2_CLS2_ACT_COLOR_MASK;
	c2->sram.regs.actions |= (cmd << MVPP2_CLS2_ACT_COLOR_OFF);

	if (from == 1)
		c2->sram.regs.action_tbl |= (1 <<
			MVPP2_CLS2_ACT_DATA_TBL_COLOR_OFF);
	else
		c2->sram.regs.action_tbl &= ~(1 <<
			MVPP2_CLS2_ACT_DATA_TBL_COLOR_OFF);

	return 0;
}

static int mvpp2_cls_c2_prio_set(struct mvpp2_cls_c2_entry *c2, int cmd,
				 int prio, int from)
{
	if (!c2 || cmd > MVPP2_ACTION_TYPE_UPDT_LOCK ||
	    prio >= MVPP2_QOS_TBL_LINE_NUM_PRI)
		return -EINVAL;

	/* set command */
	c2->sram.regs.actions &= ~MVPP2_CLS2_ACT_PRI_MASK;
	c2->sram.regs.actions |= (cmd << MVPP2_CLS2_ACT_PRI_OFF);

	/* set modify priority value */
	c2->sram.regs.qos_attr &= ~MVPP2_CLS2_ACT_QOS_ATTR_PRI_MASK;
	c2->sram.regs.qos_attr |= ((prio << MVPP2_CLS2_ACT_QOS_ATTR_PRI_OFF) &
		MVPP2_CLS2_ACT_QOS_ATTR_PRI_MASK);

	if (from == 1)
		c2->sram.regs.action_tbl |= (1 <<
			MVPP2_CLS2_ACT_DATA_TBL_PRI_DSCP_OFF);
	else
		c2->sram.regs.action_tbl &= ~(1 <<
			MVPP2_CLS2_ACT_DATA_TBL_PRI_DSCP_OFF);

	return 0;
}

static int mvpp2_cls_c2_dscp_set(struct mvpp2_cls_c2_entry *c2,
				 int cmd, int dscp, int from)
{
	if (!c2 || cmd > MVPP2_ACTION_TYPE_UPDT_LOCK ||
	    dscp >= MVPP2_QOS_TBL_LINE_NUM_DSCP)
		return -EINVAL;

	/* set command */
	c2->sram.regs.actions &= ~MVPP2_CLS2_ACT_DSCP_MASK;
	c2->sram.regs.actions |= (cmd << MVPP2_CLS2_ACT_DSCP_OFF);

	/* set modify DSCP value */
	c2->sram.regs.qos_attr &= ~MVPP2_CLS2_ACT_QOS_ATTR_DSCP_MASK;
	c2->sram.regs.qos_attr |= ((dscp <<
		MVPP2_CLS2_ACT_QOS_ATTR_DSCP_OFF) &
		MVPP2_CLS2_ACT_QOS_ATTR_DSCP_MASK);

	if (from == 1)
		c2->sram.regs.action_tbl |= (1 <<
			MVPP2_CLS2_ACT_DATA_TBL_PRI_DSCP_OFF);
	else
		c2->sram.regs.action_tbl &= ~(1 <<
			MVPP2_CLS2_ACT_DATA_TBL_PRI_DSCP_OFF);

	return 0;
}

static int mvpp2_cls_c2_queue_low_set(struct mvpp2_cls_c2_entry *c2,
				      int cmd, int queue, int from)
{
	if (!c2 || cmd > MVPP2_ACTION_TYPE_UPDT_LOCK ||
	    queue >= (1 << MVPP2_CLS2_ACT_QOS_ATTR_QL_BITS))
		return -EINVAL;

	/* set command */
	c2->sram.regs.actions &= ~MVPP2_CLS2_ACT_QL_MASK;
	c2->sram.regs.actions |= (cmd << MVPP2_CLS2_ACT_QL_OFF);

	/* set modify Low queue value */
	c2->sram.regs.qos_attr &= ~MVPP2_CLS2_ACT_QOS_ATTR_QL_MASK;
	c2->sram.regs.qos_attr |= ((queue <<
			MVPP2_CLS2_ACT_QOS_ATTR_QL_OFF) &
			MVPP2_CLS2_ACT_QOS_ATTR_QL_MASK);

	if (from == 1)
		c2->sram.regs.action_tbl |= (1 <<
			MVPP2_CLS2_ACT_DATA_TBL_LOW_Q_OFF);
	else
		c2->sram.regs.action_tbl &= ~(1 <<
			MVPP2_CLS2_ACT_DATA_TBL_LOW_Q_OFF);

	return 0;
}

static int mvpp2_cls_c2_queue_high_set(struct mvpp2_cls_c2_entry *c2,
				       int cmd, int queue, int from)
{
	if (!c2 || cmd > MVPP2_ACTION_TYPE_UPDT_LOCK ||
	    queue >= (1 << MVPP2_CLS2_ACT_QOS_ATTR_QH_BITS))
		return -EINVAL;

	/* set command */
	c2->sram.regs.actions &= ~MVPP2_CLS2_ACT_QH_MASK;
	c2->sram.regs.actions |= (cmd << MVPP2_CLS2_ACT_QH_OFF);

	/* set modify High queue value */
	c2->sram.regs.qos_attr &= ~MVPP2_CLS2_ACT_QOS_ATTR_QH_MASK;
	c2->sram.regs.qos_attr |= ((queue <<
			MVPP2_CLS2_ACT_QOS_ATTR_QH_OFF) &
			MVPP2_CLS2_ACT_QOS_ATTR_QH_MASK);

	if (from == 1)
		c2->sram.regs.action_tbl |= (1 <<
			MVPP2_CLS2_ACT_DATA_TBL_HIGH_Q_OFF);
	else
		c2->sram.regs.action_tbl &= ~(1 <<
			MVPP2_CLS2_ACT_DATA_TBL_HIGH_Q_OFF);

	return 0;
}

static int mvpp2_cls_c2_forward_set(struct mvpp2_cls_c2_entry *c2, int cmd)
{
	if (!c2 || cmd > MVPP2_FRWD_ACTION_TYPE_HWF_LOW_LATENCY_LOCK)
		return -EINVAL;

	c2->sram.regs.actions &= ~MVPP2_CLS2_ACT_FRWD_MASK;
	c2->sram.regs.actions |= (cmd << MVPP2_CLS2_ACT_FRWD_OFF);

	return 0;
}

static int mvpp2_cls_c2_rss_set(struct mvpp2_cls_c2_entry *c2, int cmd,
				int rss_en)
{
	if (!c2 || cmd > MVPP2_ACTION_TYPE_UPDT_LOCK || rss_en >=
			(1 << MVPP2_CLS2_ACT_DUP_ATTR_RSSEN_BITS))
		return -EINVAL;

	c2->sram.regs.actions &= ~MVPP2_CLS2_ACT_RSS_MASK;
	c2->sram.regs.actions |= (cmd << MVPP2_CLS2_ACT_RSS_OFF);

	c2->sram.regs.rss_attr &= ~MVPP2_CLS2_ACT_DUP_ATTR_RSSEN_MASK;
	c2->sram.regs.rss_attr |= (rss_en <<
			MVPP2_CLS2_ACT_DUP_ATTR_RSSEN_OFF);

	return 0;
}

static int mvpp2_cls_c2_flow_id_en(struct mvpp2_cls_c2_entry *c2, int flowid_en)
{
	if (!c2)
		return -EINVAL;

	/* set Flow ID enable or disable */
	if (flowid_en)
		c2->sram.regs.actions |= (1 << MVPP2_CLS2_ACT_FLD_EN_OFF);
	else
		c2->sram.regs.actions &= ~(1 << MVPP2_CLS2_ACT_FLD_EN_OFF);

	return 0;
}

static int mvpp2_cls_c2_tcam_byte_set(struct mvpp2_cls_c2_entry *c2,
				      unsigned int offs, unsigned char byte,
				      unsigned char enable)
{
	if (!c2 || offs >= MVPP2_CLS_C2_TCAM_DATA_BYTES)
		return -EINVAL;

	c2->tcam.bytes[MVPP2_PRS_TCAM_DATA_BYTE(offs)] = byte;
	c2->tcam.bytes[MVPP2_PRS_TCAM_DATA_BYTE_EN(offs)] = enable;

	return 0;
}

static int mvpp2_cls_c2_qos_queue_set(struct mvpp2_cls_c2_qos_entry *qos,
				      u8 queue)
{
	if (!qos || queue >= (1 << MVPP2_CLS2_QOS_TBL_QUEUENUM_BITS))
		return -EINVAL;

	qos->data &= ~MVPP2_CLS2_QOS_TBL_QUEUENUM_MASK;
	qos->data |= (((u32)queue) << MVPP2_CLS2_QOS_TBL_QUEUENUM_OFF);
	return 0;
}

static int mvpp2_c2_tcam_set(struct mvpp2 *priv,
			     struct mvpp2_c2_add_entry *c2_add_entry,
			     unsigned int c2_hw_idx)
{
	int ret_code;
	struct mvpp2_cls_c2_entry c2_entry;
	int hek_offs;
	unsigned char hek_byte[MVPP2_CLS_C2_HEK_OFF_MAX],
		      hek_byte_mask[MVPP2_CLS_C2_HEK_OFF_MAX];

	if (!c2_add_entry || !priv || c2_hw_idx >= MVPP2_CLS_C2_TCAM_SIZE)
		return -EINVAL;

	/* Clear C2 sw data */
	memset(&c2_entry, 0, sizeof(struct mvpp2_cls_c2_entry));

	/* Set QOS table, selection and ID */
	ret_code =
		mvpp2_cls_c2_qos_tbl_set(&c2_entry,
					 c2_add_entry->qos_info.qos_tbl_index,
					 c2_add_entry->qos_info.qos_tbl_type);
	if (ret_code)
		return ret_code;

	/* Set color, cmd and source */
	ret_code = mvpp2_cls_c2_color_set(&c2_entry,
					  c2_add_entry->action.color_act,
					  c2_add_entry->qos_info.color_src);
	if (ret_code)
		return ret_code;

	/* Set priority(pbit), cmd, value(not from qos table) and source */
	ret_code = mvpp2_cls_c2_prio_set(&c2_entry,
					 c2_add_entry->action.pri_act,
					 c2_add_entry->qos_value.pri,
					 c2_add_entry->qos_info.pri_dscp_src);
	if (ret_code)
		return ret_code;

	/* Set DSCP, cmd, value(not from qos table) and source */
	ret_code = mvpp2_cls_c2_dscp_set(&c2_entry,
					 c2_add_entry->action.dscp_act,
					 c2_add_entry->qos_value.dscp,
					 c2_add_entry->qos_info.pri_dscp_src);
	if (ret_code)
		return ret_code;

	/* Set queue low, cmd, value, and source */
	ret_code = mvpp2_cls_c2_queue_low_set(&c2_entry,
					      c2_add_entry->action.q_low_act,
					      c2_add_entry->qos_value.q_low,
					      c2_add_entry->qos_info.q_low_src);
	if (ret_code)
		return ret_code;

	/* Set queue high, cmd, value and source */
	ret_code =
		mvpp2_cls_c2_queue_high_set(&c2_entry,
					    c2_add_entry->action.q_high_act,
					    c2_add_entry->qos_value.q_high,
					    c2_add_entry->qos_info.q_high_src);
	if (ret_code)
		return ret_code;

	/* Set forward */
	ret_code = mvpp2_cls_c2_forward_set(&c2_entry,
					    c2_add_entry->action.frwd_act);
	if (ret_code)
		return ret_code;

	/* Set RSS */
	ret_code = mvpp2_cls_c2_rss_set(&c2_entry,
					c2_add_entry->action.rss_act,
					c2_add_entry->rss_en);
	if (ret_code)
		return ret_code;

	/* Set flowID(not for multicast) */
	ret_code = mvpp2_cls_c2_flow_id_en(&c2_entry,
					   c2_add_entry->action.flowid_act);
	if (ret_code)
		return ret_code;

	/* Set C2 HEK */
	memset(hek_byte, 0, MVPP2_CLS_C2_HEK_OFF_MAX);
	memset(hek_byte_mask, 0, MVPP2_CLS_C2_HEK_OFF_MAX);

	/* HEK offs 8, lookup type, port type */
	hek_byte[MVPP2_CLS_C2_HEK_OFF_LKP_PORT_TYPE] =
		(c2_add_entry->port.port_type <<
			MVPP2_CLS_C2_HEK_PORT_TYPE_OFFS) |
		(c2_add_entry->lkp_type <<
			MVPP2_CLS_C2_HEK_LKP_TYPE_OFFS);
	hek_byte_mask[MVPP2_CLS_C2_HEK_OFF_LKP_PORT_TYPE] =
			MVPP2_CLS_C2_HEK_PORT_TYPE_MASK |
			((c2_add_entry->lkp_type_mask <<
				MVPP2_CLS_C2_HEK_LKP_TYPE_OFFS) &
				MVPP2_CLS_C2_HEK_LKP_TYPE_MASK);
	/* HEK offs 9, port ID */
	hek_byte[MVPP2_CLS_C2_HEK_OFF_PORT_ID] =
		c2_add_entry->port.port_value;
	hek_byte_mask[MVPP2_CLS_C2_HEK_OFF_PORT_ID] =
		c2_add_entry->port.port_mask;

	for (hek_offs = MVPP2_CLS_C2_HEK_OFF_PORT_ID; hek_offs >=
			MVPP2_CLS_C2_HEK_OFF_BYTE0; hek_offs--) {
		ret_code = mvpp2_cls_c2_tcam_byte_set(&c2_entry, hek_offs,
						      hek_byte[hek_offs],
						      hek_byte_mask[hek_offs]);
		if (ret_code)
			return ret_code;
	}

	/* Write C2 entry data to HW */
	ret_code = mvpp2_cls_c2_hw_write(priv, c2_hw_idx, &c2_entry);
	if (ret_code)
		return ret_code;

	return 0;
}

static int mvpp2_c2_init(struct platform_device *pdev, struct mvpp2 *priv)
{
	int i;

	/* Invalid all C2 and QoS entries */
	mvpp2_cls_c2_hw_inv_all(priv);

	mvpp2_cls_c2_qos_hw_clear_all(priv);

	/* Set CLSC2_TCAM_CTRL to enable C2, or C2 does not work */
	mvpp2_write(priv, MVPP2_CLS2_TCAM_CTRL_REG,
		    MVPP2_CLS2_TCAM_CTRL_EN_MASK);

	/* Allocate mem for c2 shadow */
	priv->c2_shadow = devm_kcalloc(&pdev->dev, 1,
				       sizeof(struct mvpp2_c2_shadow),
				       GFP_KERNEL);
	if (!priv->c2_shadow)
		return -ENOMEM;

	/* Init the rule idx to invalid value */
	for (i = 0; i < 8; i++) {
		priv->c2_shadow->rule_idx_info[i].vlan_pri_idx =
			MVPP2_CLS_C2_TCAM_SIZE;
		priv->c2_shadow->rule_idx_info[i].dscp_pri_idx =
			MVPP2_CLS_C2_TCAM_SIZE;
		priv->c2_shadow->rule_idx_info[i].default_rule_idx =
			MVPP2_CLS_C2_TCAM_SIZE;
	}
	priv->c2_shadow->c2_tcam_free_start = 0;

	return 0;
}

static int mvpp2_c2_rule_add(struct mvpp2_port *port,
			     struct mvpp2_c2_add_entry *c2_add_entry)
{
	int ret, lkp_type, c2_index = 0;
	bool first_free_update = false;
	struct mvpp2_c2_rule_idx *rule_idx;

	rule_idx = &port->priv->c2_shadow->rule_idx_info[port->id];

	if (!port || !c2_add_entry)
		return -EINVAL;

	lkp_type = c2_add_entry->lkp_type;
	/* Write rule in C2 TCAM */
	if (lkp_type == MVPP2_CLS_LKP_VLAN_PRI) {
		if (rule_idx->vlan_pri_idx == MVPP2_CLS_C2_TCAM_SIZE) {
			/* If the C2 rule is new, apply a free c2 rule index */
			c2_index =
				port->priv->c2_shadow->c2_tcam_free_start;
			first_free_update = true;
		} else {
			/* If the C2 rule is exist one,
			 * take the C2 index from shadow
			 */
			c2_index = rule_idx->vlan_pri_idx;
			first_free_update = false;
		}
	} else if (lkp_type == MVPP2_CLS_LKP_DSCP_PRI) {
		if (rule_idx->dscp_pri_idx == MVPP2_CLS_C2_TCAM_SIZE) {
			c2_index =
				port->priv->c2_shadow->c2_tcam_free_start;
			first_free_update = true;
		} else {
			c2_index = rule_idx->dscp_pri_idx;
			first_free_update = false;
		}
	} else if (lkp_type == MVPP2_CLS_LKP_DEFAULT) {
		if (rule_idx->default_rule_idx == MVPP2_CLS_C2_TCAM_SIZE) {
			c2_index =
				port->priv->c2_shadow->c2_tcam_free_start;
			first_free_update = true;
		} else {
			c2_index = rule_idx->default_rule_idx;
			first_free_update = false;
		}
	} else {
		return -EINVAL;
	}

	/* Write C2 TCAM HW */
	ret = mvpp2_c2_tcam_set(port->priv, c2_add_entry, c2_index);
	if (ret)
		return ret;

	/* Update first free rule */
	if (first_free_update)
		port->priv->c2_shadow->c2_tcam_free_start++;

	/* Update shadow */
	if (lkp_type == MVPP2_CLS_LKP_VLAN_PRI)
		rule_idx->vlan_pri_idx = c2_index;
	else if (lkp_type == MVPP2_CLS_LKP_DSCP_PRI)
		rule_idx->dscp_pri_idx = c2_index;
	else if (lkp_type == MVPP2_CLS_LKP_DEFAULT)
		rule_idx->default_rule_idx = c2_index;

	return 0;
}

/* Fill the qos table with queue */
static void mvpp2_cls_c2_qos_tbl_fill(struct mvpp2_port *port,
				      u8 tbl_sel, u8 tbl_id, u8 start_queue)
{
	struct mvpp2_cls_c2_qos_entry qos_entry;
	u32 pri, line_num;
	u8 cos_value, cos_queue, queue;

	if (tbl_sel == MVPP2_QOS_TBL_SEL_PRI)
		line_num = MVPP2_QOS_TBL_LINE_NUM_PRI;
	else
		line_num = MVPP2_QOS_TBL_LINE_NUM_DSCP;

	memset(&qos_entry, 0, sizeof(struct mvpp2_cls_c2_qos_entry));
	qos_entry.tbl_id = tbl_id;
	qos_entry.tbl_sel = tbl_sel;

	/* Fill the QoS dscp/pbit table */
	for (pri = 0; pri < line_num; pri++) {
		/* cos_value equal to dscp/8 or pbit value */
		cos_value = ((tbl_sel == MVPP2_QOS_TBL_SEL_PRI) ?
			pri : (pri / 8));
		/* each nibble of pri_map stands for a cos-value,
		 * nibble value is the queue
		 */
		cos_queue = mvpp2_cosval_queue_map(port, cos_value);
		qos_entry.tbl_line = pri;
		/* map cos queue to physical queue */
		/* Physical queue contains 2 parts: port ID and CPU ID,
		 * CPU ID will be used in RSS
		 */
		queue = start_queue + cos_queue;
		mvpp2_cls_c2_qos_queue_set(&qos_entry, queue);
		mvpp2_cls_c2_qos_hw_write(port->priv, &qos_entry);
	}
}

static void mvpp2_cls_c2_entry_common_set(struct mvpp2_c2_add_entry *entry,
					  u8 port, u8 lkp_type)
{
	memset(entry, 0, sizeof(struct mvpp2_c2_add_entry));
	/* Port info */
	entry->port.port_type = MVPP2_SRC_PORT_TYPE_PHY;
	entry->port.port_value = (1 << port);
	entry->port.port_mask = 0xff;
	/* Lookup type */
	entry->lkp_type = lkp_type;
	entry->lkp_type_mask = 0x3F;
	/* Action info */
	entry->action.color_act = MVPP2_COLOR_ACTION_TYPE_NO_UPDT_LOCK;
	entry->action.pri_act = MVPP2_ACTION_TYPE_NO_UPDT_LOCK;
	entry->action.dscp_act = MVPP2_ACTION_TYPE_NO_UPDT_LOCK;
	entry->action.q_low_act = MVPP2_ACTION_TYPE_UPDT_LOCK;
	entry->action.q_high_act = MVPP2_ACTION_TYPE_UPDT_LOCK;
	entry->action.rss_act = MVPP2_ACTION_TYPE_UPDT_LOCK;
	/* To CPU */
	entry->action.frwd_act = MVPP2_FRWD_ACTION_TYPE_SWF_LOCK;
}

/* C2 rule set */
static int mvpp2_cls_c2_rule_set(struct mvpp2_port *port, u8 start_queue)
{
	struct mvpp2_c2_add_entry c2_init_entry;
	int ret;
	u8 cos_value, cos_queue, queue, lkp_type;

	/* QoS of pbit rule */
	for (lkp_type = MVPP2_CLS_LKP_VLAN_PRI; lkp_type <=
			MVPP2_CLS_LKP_DEFAULT; lkp_type++) {
		/* Set common part of C2 rule */
		mvpp2_cls_c2_entry_common_set(&c2_init_entry, port->id,
					      lkp_type);

		/* QoS info */
		if (lkp_type != MVPP2_CLS_LKP_DEFAULT) {
			u8 tbl_sel = MVPP2_QOS_TBL_SEL_PRI;

			/* QoS info from C2 QoS table */
			/* Set the QoS table index equal to port ID */
			c2_init_entry.qos_info.qos_tbl_index = port->id;
			c2_init_entry.qos_info.q_low_src =
					MVPP2_QOS_SRC_DSCP_PBIT_TBL;
			c2_init_entry.qos_info.q_high_src =
					MVPP2_QOS_SRC_DSCP_PBIT_TBL;
			if (lkp_type == MVPP2_CLS_LKP_VLAN_PRI) {
				c2_init_entry.qos_info.qos_tbl_type =
					MVPP2_QOS_TBL_SEL_PRI;
				tbl_sel = MVPP2_QOS_TBL_SEL_PRI;
			} else if (lkp_type == MVPP2_CLS_LKP_DSCP_PRI) {
				c2_init_entry.qos_info.qos_tbl_type =
					MVPP2_QOS_TBL_SEL_DSCP;
				tbl_sel = MVPP2_QOS_TBL_SEL_DSCP;
			}
			/* Fill qos table */
			mvpp2_cls_c2_qos_tbl_fill(port, tbl_sel,
						  port->id, start_queue);
		} else {
			/* QoS info from C2 action table */
			c2_init_entry.qos_info.q_low_src =
					MVPP2_QOS_SRC_ACTION_TBL;
			c2_init_entry.qos_info.q_high_src =
					MVPP2_QOS_SRC_ACTION_TBL;
			cos_value = port->cos_cfg.default_cos;
			cos_queue = mvpp2_cosval_queue_map(port, cos_value);
			/* map to physical queue */
			/* Physical queue contains 2 parts: port ID and CPU ID,
			 * CPU ID will be used in RSS
			 */
			queue = start_queue + cos_queue;
			c2_init_entry.qos_value.q_low = ((u16)queue) &
				((1 << MVPP2_CLS2_ACT_QOS_ATTR_QL_BITS) - 1);
			c2_init_entry.qos_value.q_high = ((u16)queue) >>
					MVPP2_CLS2_ACT_QOS_ATTR_QL_BITS;
		}
		/* RSS En in PP22 */
		c2_init_entry.rss_en = port->rss_cfg.rss_en;

		/* Add rule to C2 TCAM */
		ret = mvpp2_c2_rule_add(port, &c2_init_entry);
		if (ret)
			return ret;
	}

	return 0;
}

/* The function get the queue in the C2 rule with input index */
static u8 mvpp2_cls_c2_rule_queue_get(struct mvpp2 *priv, u32 rule_idx)
{
	u32 reg_val;
	u8 queue;

	/* Write index reg */
	mvpp2_write(priv, MVPP2_CLS2_TCAM_IDX_REG, rule_idx);

	/* Read Reg CLSC2_ATTR0 */
	reg_val = mvpp2_read(priv, MVPP2_CLS2_ACT_QOS_ATTR_REG);
	queue = (reg_val & (MVPP2_CLS2_ACT_QOS_ATTR_QL_MASK |
			MVPP2_CLS2_ACT_QOS_ATTR_QH_MASK)) >>
			MVPP2_CLS2_ACT_QOS_ATTR_QL_OFF;
	return queue;
}

/* The function set the qos queue in one C2 rule */
static void mvpp2_cls_c2_rule_queue_set(struct mvpp2 *priv, u32 rule_idx,
					u8 queue)
{
	u32 reg_val;

	/* Write index reg */
	mvpp2_write(priv, MVPP2_CLS2_TCAM_IDX_REG, rule_idx);

	/* Read Reg CLSC2_ATTR0, update value with Queue, write back */
	reg_val = mvpp2_read(priv, MVPP2_CLS2_ACT_QOS_ATTR_REG);
	reg_val &= ~(MVPP2_CLS2_ACT_QOS_ATTR_QL_MASK |
			MVPP2_CLS2_ACT_QOS_ATTR_QH_MASK);
	reg_val |= (((u32)queue) << MVPP2_CLS2_ACT_QOS_ATTR_QL_OFF);
	mvpp2_write(priv, MVPP2_CLS2_ACT_QOS_ATTR_REG, reg_val);
}

/* The function get the queue in the pbit table entry */
static u8 mvpp2_cls_c2_pbit_tbl_queue_get(struct mvpp2 *priv, u8 tbl_id,
					  u8 tbl_line)
{
	u8 queue;
	u32 reg_val = 0;

	/* write index reg */
	reg_val |= (tbl_line << MVPP2_CLS2_DSCP_PRI_INDEX_LINE_OFF);
	reg_val |= MVPP2_QOS_TBL_SEL_PRI << MVPP2_CLS2_DSCP_PRI_INDEX_SEL_OFF;
	reg_val |= (tbl_id << MVPP2_CLS2_DSCP_PRI_INDEX_TBL_ID_OFF);
	mvpp2_write(priv, MVPP2_CLS2_DSCP_PRI_INDEX_REG, reg_val);
	/* Read Reg CLSC2_DSCP_PRI */
	reg_val = mvpp2_read(priv, MVPP2_CLS2_QOS_TBL_REG);
	queue = (reg_val &  MVPP2_CLS2_QOS_TBL_QUEUENUM_MASK) >>
			MVPP2_CLS2_QOS_TBL_QUEUENUM_OFF;
	return queue;
}

/* The function set the queue in the pbit table entry */
static void mvpp2_cls_c2_pbit_tbl_queue_set(struct mvpp2 *priv,
					    u8 tbl_id, u8 tbl_line, u8 queue)
{
	u32 reg_val = 0;

	/* write index reg */
	reg_val |= (tbl_line << MVPP2_CLS2_DSCP_PRI_INDEX_LINE_OFF);
	reg_val |= MVPP2_QOS_TBL_SEL_PRI << MVPP2_CLS2_DSCP_PRI_INDEX_SEL_OFF;
	reg_val |= (tbl_id << MVPP2_CLS2_DSCP_PRI_INDEX_TBL_ID_OFF);
	mvpp2_write(priv, MVPP2_CLS2_DSCP_PRI_INDEX_REG, reg_val);

	/* Read Reg CLSC2_DSCP_PRI */
	reg_val = mvpp2_read(priv, MVPP2_CLS2_QOS_TBL_REG);
	reg_val &= (~MVPP2_CLS2_QOS_TBL_QUEUENUM_MASK);
	reg_val |= (((u32)queue) << MVPP2_CLS2_QOS_TBL_QUEUENUM_OFF);

	/* Write Reg CLSC2_DSCP_PRI */
	mvpp2_write(priv, MVPP2_CLS2_QOS_TBL_REG, reg_val);
}

/* Config cos classifier:
 * 0: cos based on vlan pri;
 * 1: cos based on dscp;
 * 2: cos based on vlan for tagged packets,
 *		and based on dscp for untagged IP packets;
 * 3: cos based on dscp for IP packets, and based on vlan for non-IP packets
 */
static int mvpp2_cos_classifier_set(struct mvpp2_port *port, u8 cos_mode)
{
	int index, flow_idx, lkpid, fe_idx, i, flow_entry_rss;
	struct mvpp2 *priv = port->priv;
	struct mvpp2_cls_flow_info *flow_info;
	u32 *p_fe;

	index = 0;
	while (index < (MVPP2_PRS_FL_LAST - MVPP2_PRS_FL_START)) {
		lkpid = index + MVPP2_PRS_FL_START;
		flow_info = &priv->cls_shadow->flow_info[index++];

		/* Prepare a temp table for the lkpid and swap into */
		if (mvpp2_cls_flow_swap(priv, lkpid, &flow_idx, true))
			return -EAGAIN;

		/* First, remove the port from original table */
		flow_idx = MVPP2_FLOW_TBL_SIZE;
		for (i = MVPP2_COS_TYPE_NUM - 1; i >= 0; i--) {
			if (!flow_info->flow_entry[i])
				continue;
			mvpp2_cls_flow_port_del(priv, flow_info->flow_entry[i],
						port->id);
			/* Keep min-index for Swap-back/restore step */
			flow_idx =
				min(flow_idx, (int)flow_info->flow_entry[i]);
		}

		/* Second, set/add the port in original table */
		fe_idx = -1;
		p_fe = flow_info->flow_entry;
		if (mvpp2_prs_flow_id_attr_get(lkpid) &
		    MVPP2_PRS_FL_ATTR_VLAN_BIT) {
			if (cos_mode == MVPP2_COS_CLS_VLAN ||
			    cos_mode == MVPP2_COS_CLS_VLAN_DSCP ||
			    (cos_mode == MVPP2_COS_CLS_DSCP_VLAN &&
			     lkpid == MVPP2_PRS_FL_NON_IP_TAG))
				fe_idx = p_fe[MVPP2_COS_TYPE_VLAN];
			/* Hanlde NON-IP tagged packet */
			else if (cos_mode == MVPP2_COS_CLS_DSCP &&
				 lkpid == MVPP2_PRS_FL_NON_IP_TAG)
				fe_idx = p_fe[MVPP2_COS_TYPE_DFLT];
			else if (cos_mode == MVPP2_COS_CLS_DSCP ||
				 cos_mode == MVPP2_COS_CLS_DSCP_VLAN)
				fe_idx = p_fe[MVPP2_COS_TYPE_DSCP];
		} else {
			if (lkpid == MVPP2_PRS_FL_NON_IP_UNTAG ||
			    cos_mode == MVPP2_COS_CLS_VLAN)
				fe_idx = p_fe[MVPP2_COS_TYPE_DFLT];
			else if (cos_mode == MVPP2_COS_CLS_DSCP ||
				 cos_mode == MVPP2_COS_CLS_VLAN_DSCP ||
				 cos_mode == MVPP2_COS_CLS_DSCP_VLAN)
				fe_idx = p_fe[MVPP2_COS_TYPE_DSCP];
		}
		if (fe_idx >= 0)
			mvpp2_cls_flow_port_add(priv, fe_idx, port->id);

		/* Third, restore lookup table */
		flow_entry_rss = flow_info->flow_entry_rss1;
		if (flow_entry_rss)
			flow_idx = min(flow_idx, flow_entry_rss);
		flow_entry_rss = flow_info->flow_entry_rss2;
		if (flow_entry_rss)
			flow_idx = min(flow_idx, flow_entry_rss);
		mvpp2_cls_flow_swap(priv, lkpid, &flow_idx, false);
	}

	/* Update it in priv */
	port->cos_cfg.cos_classifier = cos_mode;

	return 0;
}

static void mvpp22_rss_c2_enable(struct mvpp2_port *port, bool en)
{
	int lkp_type;
	u32 idx[MVPP2_CLS_LKP_MAX];
	struct mvpp2_c2_rule_idx *rule_idx;
	struct mvpp2 *priv = port->priv;
	int reg_val;

	rule_idx = &port->priv->c2_shadow->rule_idx_info[port->id];

	/* Get the C2 index from shadow */
	idx[MVPP2_CLS_LKP_VLAN_PRI] = rule_idx->vlan_pri_idx;
	idx[MVPP2_CLS_LKP_DSCP_PRI] = rule_idx->dscp_pri_idx;
	idx[MVPP2_CLS_LKP_DEFAULT] = rule_idx->default_rule_idx;

	/* MVPP2_CLS_LKP_HASH has no corresponding C2 rule, skip it */
	for (lkp_type = MVPP2_CLS_LKP_VLAN_PRI; lkp_type < MVPP2_CLS_LKP_MAX;
	     lkp_type++) {
		/* write index reg */
		mvpp2_write(priv, MVPP2_CLS2_TCAM_IDX_REG, idx[lkp_type]);
		/* Update rss_attr in reg CLSC2_ATTR2 */
		reg_val = mvpp2_read(priv, MVPP2_CLS2_ACT_DUP_ATTR_REG);
		if (en)
			reg_val |= MVPP2_CLS2_ACT_DUP_ATTR_RSSEN_MASK;
		else
			reg_val &= ~MVPP2_CLS2_ACT_DUP_ATTR_RSSEN_MASK;
		mvpp2_write(priv, MVPP2_CLS2_ACT_DUP_ATTR_REG, reg_val);
	}
}

int mvpp2_update_flow_info(struct mvpp2 *priv)
{
	struct mvpp2_cls_flow_info *flow_info;
	struct mvpp2_cls_lookup_entry le;
	struct mvpp2_cls_flow_entry fe;
	int flow_index, lkp_type, prio, is_last, engine, update_rss2;
	int i, j, cos_type;

	for (i = 0; i < (MVPP2_PRS_FL_LAST - MVPP2_PRS_FL_START); i++) {
		is_last = 0;
		update_rss2 = 0;
		flow_info = &priv->cls_shadow->flow_info[i];
		mvpp2_cls_lookup_read(priv, MVPP2_PRS_FL_START + i, 0, &le);
		mvpp2_cls_sw_lkp_flow_get(&le, &flow_index);

		for (j = 0; is_last == 0; j++) {
			mvpp2_cls_flow_read(priv, flow_index + j, &fe);
			mvpp2_cls_sw_flow_engine_get(&fe, &engine, &is_last);
			mvpp2_cls_sw_flow_extra_get(&fe, &lkp_type, &prio);

			if (lkp_type == MVPP2_CLS_LKP_HASH) {
				if (!update_rss2) {
					flow_info->flow_entry_rss1 =
								flow_index + j;
					update_rss2 = 1;
				} else {
					flow_info->flow_entry_rss2 =
								flow_index + j;
				}
				continue;
			}

			cos_type = MVPP2_COS_TYPE_DFLT;
			if (lkp_type == MVPP2_CLS_LKP_VLAN_PRI)
				cos_type = MVPP2_COS_TYPE_VLAN;
			else if (lkp_type == MVPP2_CLS_LKP_DSCP_PRI)
				cos_type = MVPP2_COS_TYPE_DSCP;
			flow_info->flow_entry[cos_type] = flow_index + j;
		}
	}
	return 0;
}

/* Update RSS hash mode for non-fragemnted UDP packet per port */
static int mvpp22_rss_udp_mode_set(struct mvpp2_port *port, int rss_mode)
{
	int index, flow_idx, flow_idx_rss, lkpid, lkpid_attr;
	int data[MVPP2_COS_TYPE_NUM], j;
	struct mvpp2 *priv = port->priv;
	struct mvpp2_cls_flow_info *flow_info;
	int err;

	err = mvpp2_update_flow_info(priv);
	if (err) {
		netdev_err(port->dev, "cannot update flow info\n");
		return err;
	}

	if (rss_mode != MVPP22_RSS_2T &&
	    rss_mode != MVPP22_RSS_5T) {
		pr_err("Invalid rss mode:%d\n", rss_mode);
		return -EINVAL;
	}

	index = 0;
	while (index < (MVPP2_PRS_FL_LAST - MVPP2_PRS_FL_START)) {
		lkpid = index + MVPP2_PRS_FL_START;
		flow_info = &priv->cls_shadow->flow_info[index++];
		data[0] = MVPP2_FLOW_TBL_SIZE;
		data[1] = MVPP2_FLOW_TBL_SIZE;
		data[2] = MVPP2_FLOW_TBL_SIZE;
		/* Get lookup ID attribute */
		lkpid_attr = mvpp2_prs_flow_id_attr_get(lkpid);
		/* Only non-frag TCP & UDP can set rss mode */
		if ((lkpid_attr &
		     (MVPP2_PRS_FL_ATTR_TCP_BIT | MVPP2_PRS_FL_ATTR_UDP_BIT)) &&
		    !(lkpid_attr & MVPP2_PRS_FL_ATTR_FRAG_BIT)) {
			/* Prepare a temp table for the lkpid and swap into */
			if (mvpp2_cls_flow_swap(priv, lkpid, &flow_idx, true))
				return -EAGAIN;

			/* First, remove the port from original table */
			mvpp2_cls_flow_port_del(priv,
						flow_info->flow_entry_rss1,
						port->id);
			mvpp2_cls_flow_port_del(priv,
						flow_info->flow_entry_rss2,
						port->id);

			/* Second, update port's original table */
			if (rss_mode == MVPP22_RSS_2T)
				flow_idx_rss = flow_info->flow_entry_rss1;
			else
				flow_idx_rss = flow_info->flow_entry_rss2;

			mvpp2_cls_flow_port_add(priv, flow_idx_rss, port->id);

			/*Find the ptr of flow table as min flow index */
			for (j = 0; j < MVPP2_COS_TYPE_NUM; j++) {
				if (!flow_info->flow_entry[j])
					continue;
				data[j] = flow_info->flow_entry[j];
			}
			flow_idx_rss = min(flow_info->flow_entry_rss1,
					   flow_info->flow_entry_rss2);
			flow_idx = min(min(data[0], data[1]),
				       min(data[2], flow_idx_rss));
			/*Third, restore lookup table */
			mvpp2_cls_flow_swap(priv, lkpid, &flow_idx, false);

		} else if (flow_info->flow_entry_rss1) {
			flow_idx_rss = flow_info->flow_entry_rss1;
			mvpp2_cls_flow_port_add(priv, flow_idx_rss, port->id);
		}
	}
	/* Record it in priv */
	port->rss_cfg.rss_mode = rss_mode;

	return 0;
}

/* Update the default CPU to handle the non-IP packets */
static int mvpp22_rss_default_cpu_set(struct mvpp2_port *port, int default_cpu)
{
	u8 index, queue, q_cpu_mask;
	u32 cpu_width, cos_width;
	struct mvpp2 *priv = port->priv;

	if (!(*cpumask_bits(cpu_online_mask) & (1 << default_cpu)))
		return -EINVAL;

	/* Calculate width */
	mvpp2_width_calc(port, &cpu_width, &cos_width);
	q_cpu_mask = (1 << cpu_width) - 1;

	/* Update LSB[cpu_width + cos_width - 1 : cos_width]
	 * of queue (queue high and low) on c2 rule.
	 */
	index = priv->c2_shadow->rule_idx_info[port->id].default_rule_idx;
	queue = mvpp2_cls_c2_rule_queue_get(priv, index);
	queue &= ~(q_cpu_mask << cos_width);
	queue |= (default_cpu << cos_width);
	mvpp2_cls_c2_rule_queue_set(priv, index, queue);

	/* Update LSB[cpu_width + cos_width - 1 : cos_width]
	 * of queue on pbit table, table id equals to port id
	 */
	for (index = 0; index < MVPP2_QOS_TBL_LINE_NUM_PRI; index++) {
		queue = mvpp2_cls_c2_pbit_tbl_queue_get(priv, port->id, index);
		queue &= ~(q_cpu_mask << cos_width);
		queue |= (default_cpu << cos_width);
		mvpp2_cls_c2_pbit_tbl_queue_set(priv, port->id, index, queue);
	}
	/* Update default cpu in cfg */
	port->rss_cfg.dflt_cpu = default_cpu;

	return 0;
}

static int mvpp22_rss_enable(struct mvpp2_port *port, bool en, bool from_open)
{
	int ret;

	if (from_open) {
		if (!en)
			return 0; /* mvpp2_cls_c2_rule_set already done */
	} else {
		if (port->rss_cfg.rss_en == en)
			return 0;
		mvpp22_rss_c2_enable(port, en);
		port->rss_cfg.rss_en = en;
	}

	if (en)
		ret = mvpp22_rss_default_cpu_set(port, port->rss_cfg.dflt_cpu);
	else
		ret = mvpp2_cls_c2_rule_set(port, mvpp2_cpu2rxq(port));
	if (ret) {
		port->rss_cfg.rss_en = !en;
		netdev_err(port->dev, "RSS %s failed on port(%d)\n",
			   en ? "enable" : "disable", port->id);
	}
	return ret;
}

static void *mvpp2_frag_alloc(const struct mvpp2_bm_pool *pool)
{
	if (likely(pool->frag_size <= PAGE_SIZE))
		return netdev_alloc_frag(pool->frag_size);
	else
		return kmalloc(pool->frag_size, GFP_ATOMIC);
}

static void mvpp2_frag_free(const struct mvpp2_bm_pool *pool, void *data)
{
	if (likely(pool->frag_size <= PAGE_SIZE))
		skb_free_frag(data);
	else
		kfree(data);
}

/* Buffer Manager configuration routines */

/* Create pool */
static int mvpp2_bm_pool_create(struct platform_device *pdev,
				struct mvpp2 *priv,
				struct mvpp2_bm_pool *bm_pool, int size)
{
	u32 val;

	/* Number of buffer pointers must be a multiple of 16, as per
	 * hardware constraints
	 */
	if (!IS_ALIGNED(size, 16))
		return -EINVAL;

	/* PPv2.1 needs 8 bytes per buffer pointer, PPv2.2 needs 16
	 * bytes per buffer pointer
	 */
	if (priv->hw_version == MVPP21)
		bm_pool->size_bytes = 2 * sizeof(u32) * size;
	else
		bm_pool->size_bytes = 2 * sizeof(u64) * size;

	bm_pool->virt_addr = dma_alloc_coherent(&pdev->dev, bm_pool->size_bytes,
						&bm_pool->dma_addr,
						GFP_KERNEL);
	if (!bm_pool->virt_addr)
		return -ENOMEM;

	if (!IS_ALIGNED((unsigned long)bm_pool->virt_addr,
			MVPP2_BM_POOL_PTR_ALIGN)) {
		dma_free_coherent(&pdev->dev, bm_pool->size_bytes,
				  bm_pool->virt_addr, bm_pool->dma_addr);
		dev_err(&pdev->dev, "BM pool %d is not %d bytes aligned\n",
			bm_pool->id, MVPP2_BM_POOL_PTR_ALIGN);
		return -ENOMEM;
	}

	mvpp2_write(priv, MVPP2_BM_POOL_BASE_REG(bm_pool->id),
		    lower_32_bits(bm_pool->dma_addr));
	mvpp2_write(priv, MVPP2_BM_POOL_SIZE_REG(bm_pool->id), size);

	val = mvpp2_read(priv, MVPP2_BM_POOL_CTRL_REG(bm_pool->id));
	val |= MVPP2_BM_START_MASK;
	mvpp2_write(priv, MVPP2_BM_POOL_CTRL_REG(bm_pool->id), val);

	bm_pool->size = size;
	bm_pool->pkt_size = 0;
	bm_pool->buf_num = 0;

	return 0;
}

/* Set pool buffer size */
static void mvpp2_bm_pool_bufsize_set(struct mvpp2 *priv,
				      struct mvpp2_bm_pool *bm_pool,
				      int buf_size)
{
	u32 val;

	bm_pool->buf_size = buf_size;

	val = ALIGN(buf_size, 1 << MVPP2_POOL_BUF_SIZE_OFFSET);
	mvpp2_write(priv, MVPP2_POOL_BUF_SIZE_REG(bm_pool->id), val);
}

static void mvpp2_bm_bufs_get_addrs(struct device *dev, struct mvpp2 *priv,
				    struct mvpp2_bm_pool *bm_pool,
				    dma_addr_t *dma_addr,
				    phys_addr_t *phys_addr)
{
	int cpu = get_cpu();

	*dma_addr = mvpp2_percpu_read(priv, cpu,
				      MVPP2_BM_PHY_ALLOC_REG(bm_pool->id));

	if (priv->hw_version != MVPP21 && sizeof(dma_addr_t) == 8) {
		u32 val;
		u32 dma_addr_highbits;

		val = mvpp2_percpu_read(priv, cpu, MVPP22_BM_ADDR_HIGH_ALLOC);
		dma_addr_highbits = (val & MVPP22_BM_ADDR_HIGH_PHYS_MASK);
		*dma_addr |= (u64)dma_addr_highbits << 32;
	}
	*phys_addr = dma_to_phys(dev, *dma_addr);

	put_cpu();
}

/* Free all buffers from the pool */
static void mvpp2_bm_bufs_free(struct device *dev, struct mvpp2 *priv,
			       struct mvpp2_bm_pool *bm_pool, int buf_num)
{
	int i;

	if (buf_num > bm_pool->buf_num) {
		WARN(1, "Pool does not have so many bufs pool(%d) bufs(%d)\n",
		     bm_pool->id, buf_num);
		buf_num = bm_pool->buf_num;
	}

	for (i = 0; i < buf_num; i++) {
		dma_addr_t buf_dma_addr;
		phys_addr_t buf_phys_addr;
		void *data;

		mvpp2_bm_bufs_get_addrs(dev, priv, bm_pool,
					&buf_dma_addr, &buf_phys_addr);

		dma_unmap_single(dev, buf_dma_addr,
				 bm_pool->buf_size, DMA_FROM_DEVICE);

		data = (void *)phys_to_virt(buf_phys_addr);
		if (!data)
			break;

		mvpp2_frag_free(bm_pool, data);
	}

	/* Update BM driver with number of buffers removed from pool */
	bm_pool->buf_num -= i;
}

/* Check number of buffers in BM pool */
int mvpp2_check_hw_buf_num(struct mvpp2 *priv, struct mvpp2_bm_pool *bm_pool)
{
	int buf_num = 0;

	buf_num += mvpp2_read(priv, MVPP2_BM_POOL_PTRS_NUM_REG(bm_pool->id)) &
				    MVPP22_BM_POOL_PTRS_NUM_MASK;
	buf_num += mvpp2_read(priv, MVPP2_BM_BPPI_PTRS_NUM_REG(bm_pool->id)) &
				    MVPP2_BM_BPPI_PTR_NUM_MASK;

	/* HW has one buffer ready which is not reflected in the counters */
	if (buf_num)
		buf_num += 1;

	return buf_num;
}

/* Cleanup pool */
static int mvpp2_bm_pool_destroy(struct platform_device *pdev,
				 struct mvpp2 *priv,
				 struct mvpp2_bm_pool *bm_pool)
{
	int buf_num;
	u32 val;

	buf_num = mvpp2_check_hw_buf_num(priv, bm_pool);
	mvpp2_bm_bufs_free(&pdev->dev, priv, bm_pool, buf_num);

	/* Check buffer counters after free */
	buf_num = mvpp2_check_hw_buf_num(priv, bm_pool);
	if (buf_num) {
		WARN(1, "cannot free all buffers in pool %d, buf_num left %d\n",
		     bm_pool->id, bm_pool->buf_num);
		return 0;
	}

	val = mvpp2_read(priv, MVPP2_BM_POOL_CTRL_REG(bm_pool->id));
	val |= MVPP2_BM_STOP_MASK;
	mvpp2_write(priv, MVPP2_BM_POOL_CTRL_REG(bm_pool->id), val);

	dma_free_coherent(&pdev->dev, bm_pool->size_bytes,
			  bm_pool->virt_addr,
			  bm_pool->dma_addr);
	return 0;
}

static int mvpp2_bm_pools_init(struct platform_device *pdev,
			       struct mvpp2 *priv)
{
	int i, err, size;
	struct mvpp2_bm_pool *bm_pool;

	/* Create all pools with maximum size */
	size = MVPP2_BM_POOL_SIZE_MAX;
	for (i = 0; i < MVPP2_BM_POOLS_NUM; i++) {
		bm_pool = &priv->bm_pools[i];
		bm_pool->id = i;
		err = mvpp2_bm_pool_create(pdev, priv, bm_pool, size);
		if (err)
			goto err_unroll_pools;
		mvpp2_bm_pool_bufsize_set(priv, bm_pool, 0);
	}
	return 0;

err_unroll_pools:
	dev_err(&pdev->dev, "failed to create BM pool %d, size %d\n", i, size);
	for (i = i - 1; i >= 0; i--)
		mvpp2_bm_pool_destroy(pdev, priv, &priv->bm_pools[i]);
	return err;
}

static int mvpp2_bm_init(struct platform_device *pdev, struct mvpp2 *priv)
{
	int i, err;

	for (i = 0; i < MVPP2_BM_POOLS_NUM; i++) {
		/* Mask BM all interrupts */
		mvpp2_write(priv, MVPP2_BM_INTR_MASK_REG(i), 0);
		/* Clear BM cause register */
		mvpp2_write(priv, MVPP2_BM_INTR_CAUSE_REG(i), 0);
	}

	/* Allocate and initialize BM pools */
	priv->bm_pools = devm_kcalloc(&pdev->dev, MVPP2_BM_POOLS_NUM,
				      sizeof(*priv->bm_pools), GFP_KERNEL);
	if (!priv->bm_pools)
		return -ENOMEM;

	err = mvpp2_bm_pools_init(pdev, priv);
	if (err < 0)
		return err;
	return 0;
}

static void mvpp2_setup_bm_pool(void)
{
	/* Short pool */
	mvpp2_pools[MVPP2_BM_SHORT].buf_num  = MVPP2_BM_SHORT_BUF_NUM;
	mvpp2_pools[MVPP2_BM_SHORT].pkt_size = MVPP2_BM_SHORT_PKT_SIZE;

	/* Long pool */
	mvpp2_pools[MVPP2_BM_LONG].buf_num  = MVPP2_BM_LONG_BUF_NUM;
	mvpp2_pools[MVPP2_BM_LONG].pkt_size = MVPP2_BM_LONG_PKT_SIZE;

	/* Jumbo pool */
	mvpp2_pools[MVPP2_BM_JUMBO].buf_num  = MVPP2_BM_JUMBO_BUF_NUM;
	mvpp2_pools[MVPP2_BM_JUMBO].pkt_size = MVPP2_BM_JUMBO_PKT_SIZE;
}

/* Attach long pool to rxq */
static void mvpp2_rxq_long_pool_set(struct mvpp2_port *port,
				    int lrxq, int long_pool)
{
	u32 val, mask;
	int prxq;

	/* Get queue physical ID */
	prxq = port->rxqs[lrxq]->id;

	if (port->priv->hw_version == MVPP21)
		mask = MVPP21_RXQ_POOL_LONG_MASK;
	else
		mask = MVPP22_RXQ_POOL_LONG_MASK;

	val = mvpp2_read(port->priv, MVPP2_RXQ_CONFIG_REG(prxq));
	val &= ~mask;
	val |= (long_pool << MVPP2_RXQ_POOL_LONG_OFFS) & mask;
	mvpp2_write(port->priv, MVPP2_RXQ_CONFIG_REG(prxq), val);
}

/* Attach short pool to rxq */
static void mvpp2_rxq_short_pool_set(struct mvpp2_port *port,
				     int lrxq, int short_pool)
{
	u32 val, mask;
	int prxq;

	/* Get queue physical ID */
	prxq = port->rxqs[lrxq]->id;

	if (port->priv->hw_version == MVPP21)
		mask = MVPP21_RXQ_POOL_SHORT_MASK;
	else
		mask = MVPP22_RXQ_POOL_SHORT_MASK;

	val = mvpp2_read(port->priv, MVPP2_RXQ_CONFIG_REG(prxq));
	val &= ~mask;
	val |= (short_pool << MVPP2_RXQ_POOL_SHORT_OFFS) & mask;
	mvpp2_write(port->priv, MVPP2_RXQ_CONFIG_REG(prxq), val);
}

static dma_addr_t mvpp2_buf_alloc(struct mvpp2_port *port,
				  struct mvpp2_bm_pool *bm_pool,
				  gfp_t gfp_mask)
{
	dma_addr_t dma_addr;
	void *data;

	data = mvpp2_frag_alloc(bm_pool);
	if (!data)
		return (dma_addr_t)data;

	dma_addr = dma_map_single(port->dev->dev.parent, data,
				  MVPP2_RX_BUF_SIZE(bm_pool->pkt_size),
				  DMA_FROM_DEVICE);
	if (unlikely(dma_mapping_error(port->dev->dev.parent, dma_addr))) {
		mvpp2_frag_free(bm_pool, data);
		dma_addr = 0;
	}
	return dma_addr;
}

/* Release buffer to BM */
static inline void mvpp2_bm_pool_put(struct mvpp2_port *port, int pool,
				     dma_addr_t buf_dma_addr)
{
	int sw_thread = mvpp2_check_sw_thread(smp_processor_id());
	unsigned long flags = 0;

	if (port->priv->spinlocks_bitmap & MV_BM_LOCK)
		spin_lock_irqsave(&port->priv->bm_spinlock[sw_thread], flags);

	/* MVPP2_BM_VIRT_RLS_REG is not interpreted by HW, and simply
	 * returned in the "cookie" field of the RX descriptor.
	 * For performance reasons don't store VA|PA and don't use "cookie".
	 * VA/PA obtained faster from dma_to_phys(dma-addr) and phys_to_virt.
	 */
	if (port->priv->hw_version != MVPP21 && sizeof(dma_addr_t) == 8) {
		u32 val = upper_32_bits(buf_dma_addr) &
				MVPP22_BM_ADDR_HIGH_PHYS_RLS_MASK;
		mvpp2_percpu_write_relaxed(port->priv, sw_thread,
					   MVPP22_BM_ADDR_HIGH_RLS_REG, val);
	}
	mvpp2_percpu_write_relaxed(port->priv, sw_thread,
				   MVPP2_BM_PHY_RLS_REG(pool), buf_dma_addr);

	if (port->priv->spinlocks_bitmap & MV_BM_LOCK)
		spin_unlock_irqrestore(&port->priv->bm_spinlock[sw_thread],
				       flags);
}

/* Allocate buffers for the pool */
static int mvpp2_bm_bufs_add(struct mvpp2_port *port,
			     struct mvpp2_bm_pool *bm_pool, int buf_num)
{
	int i, buf_size, total_size;
	dma_addr_t dma_addr;

	buf_size = MVPP2_RX_BUF_SIZE(bm_pool->pkt_size);
	total_size = MVPP2_RX_TOTAL_SIZE(buf_size);

	if (buf_num < 0 ||
	    (buf_num + bm_pool->buf_num > bm_pool->size)) {
		netdev_err(port->dev,
			   "cannot allocate %d buffers for pool %d\n",
			   buf_num, bm_pool->id);
		return 0;
	}

	for (i = 0; i < buf_num; i++) {
		dma_addr = mvpp2_buf_alloc(port, bm_pool, GFP_KERNEL);
		if (!dma_addr)
			break;

		mvpp2_bm_pool_put(port, bm_pool->id, dma_addr);
	}

	/* Update BM driver with number of buffers added to pool */
	bm_pool->buf_num += i;

	netdev_dbg(port->dev,
		   "pool %d: pkt_size=%4d, buf_size=%4d, total_size=%4d\n",
		   bm_pool->id, bm_pool->pkt_size, buf_size, total_size);

	netdev_dbg(port->dev,
		   "pool %d: %d of %d buffers added\n",
		   bm_pool->id, i, buf_num);
	return i;
}

/* Notify the driver that BM pool is being used as specific type and return the
 * pool pointer on success
 */
static struct mvpp2_bm_pool *
mvpp2_bm_pool_use(struct mvpp2_port *port, int pool, int pkt_size)
{
	struct mvpp2_bm_pool *new_pool = &port->priv->bm_pools[pool];
	int num;

	if (pool < MVPP2_BM_SHORT || pool > MVPP2_BM_JUMBO) {
		netdev_err(port->dev, "Invalid pool %d\n", pool);
		return NULL;
	}

	/* Allocate buffers in case BM pool is used as long pool, but packet
	 * size doesn't match MTU or BM pool hasn't being used yet
	 */
	if (new_pool->pkt_size == 0) {
		int pkts_num;

		/* Set default buffer number or free all the buffers in case
		 * the pool is not empty
		 */
		pkts_num = new_pool->buf_num;
		if (pkts_num == 0)
			pkts_num = mvpp2_pools[pool].buf_num;
		else
			mvpp2_bm_bufs_free(port->dev->dev.parent,
					   port->priv, new_pool, pkts_num);

		new_pool->pkt_size = pkt_size;
		new_pool->frag_size =
			SKB_DATA_ALIGN(MVPP2_RX_BUF_SIZE(pkt_size)) +
			MVPP2_SKB_SHINFO_SIZE;

		/* Allocate buffers for this pool */
		num = mvpp2_bm_bufs_add(port, new_pool, pkts_num);
		if (num != pkts_num) {
			WARN(1, "pool %d: %d of %d allocated\n",
			     new_pool->id, num, pkts_num);
			return NULL;
		}
	}

	mvpp2_bm_pool_bufsize_set(port->priv, new_pool,
				  MVPP2_RX_BUF_SIZE(new_pool->pkt_size));

	return new_pool;
}

/* Initialize pools for swf */
static int mvpp2_swf_bm_pool_init(struct mvpp2_port *port)
{
	int rxq;
	enum mvpp2_bm_pool_log_num long_log_pool, short_log_pool;

	/* If port pkt_size is higher than 1518B:
	 * HW Long pool - SW Jumbo pool, HW Short pool - SW Short pool
	 * else: HW Long pool - SW Long pool, HW Short pool - SW Short pool
	 */
	if (port->pkt_size > MVPP2_BM_LONG_PKT_SIZE) {
		long_log_pool = MVPP2_BM_JUMBO;
		short_log_pool = MVPP2_BM_LONG;
	} else {
		long_log_pool = MVPP2_BM_LONG;
		short_log_pool = MVPP2_BM_SHORT;
	}

	if (!port->pool_long) {
		port->pool_long =
			mvpp2_bm_pool_use(port, long_log_pool,
					  mvpp2_pools[long_log_pool].pkt_size);
		if (!port->pool_long)
			return -ENOMEM;

		port->pool_long->port_map |= (1 << port->id);

		for (rxq = 0; rxq < port->nrxqs; rxq++)
			mvpp2_rxq_long_pool_set(port, rxq, port->pool_long->id);
	}

	if (!port->pool_short) {
		port->pool_short =
			mvpp2_bm_pool_use(port, short_log_pool,
					  mvpp2_pools[short_log_pool].pkt_size);
		if (!port->pool_short)
			return -ENOMEM;

		port->pool_short->port_map |= (1 << port->id);

		for (rxq = 0; rxq < port->nrxqs; rxq++)
			mvpp2_rxq_short_pool_set(port, rxq,
						 port->pool_short->id);
	}

	return 0;
}

static int mvpp2_bm_update_mtu(struct net_device *dev, int mtu)
{
	struct mvpp2_port *port = netdev_priv(dev);
	enum mvpp2_bm_pool_log_num new_long_pool;
	int pkt_size = MVPP2_RX_PKT_SIZE(mtu);

	/* If port MTU is higher than 1518B:
	 * HW Long pool - SW Jumbo pool, HW Short pool - SW Short pool
	 * else: HW Long pool - SW Long pool, HW Short pool - SW Short pool
	 */
	if (pkt_size > MVPP2_BM_LONG_PKT_SIZE)
		new_long_pool = MVPP2_BM_JUMBO;
	else
		new_long_pool = MVPP2_BM_LONG;

	if (new_long_pool != port->pool_long->id) {
		/* Remove port from old short & long pool */
		port->pool_long = mvpp2_bm_pool_use(port, port->pool_long->id,
						    port->pool_long->pkt_size);
		port->pool_long->port_map &= ~(1 << port->id);
		port->pool_long = NULL;

		port->pool_short = mvpp2_bm_pool_use(port, port->pool_short->id,
						     port->pool_short->pkt_size);
		port->pool_short->port_map &= ~(1 << port->id);
		port->pool_short = NULL;

		port->pkt_size =  pkt_size;

		/* Add port to new short & long pool */
		mvpp2_swf_bm_pool_init(port);

		/* Update L4 checksum when jumbo enable/disable on port */
		if (new_long_pool == MVPP2_BM_JUMBO && port->id != 0) {
			dev->features &= ~(NETIF_F_IP_CSUM | NETIF_F_IPV6_CSUM);
			dev->hw_features &= ~(NETIF_F_IP_CSUM |
					      NETIF_F_IPV6_CSUM);
		} else {
			dev->features |= NETIF_F_IP_CSUM | NETIF_F_IPV6_CSUM;
			dev->hw_features |= NETIF_F_IP_CSUM | NETIF_F_IPV6_CSUM;
		}
	}

	dev->mtu = mtu;
	dev->wanted_features = dev->features;

	netdev_update_features(dev);
	return 0;
}

static inline void mvpp2_interrupts_enable(struct mvpp2_port *port)
{
	int i, sw_thread_mask = 0;

	for (i = 0; i < port->nqvecs; i++)
		sw_thread_mask |= port->qvecs[i].sw_thread_mask;

	mvpp2_write(port->priv, MVPP2_ISR_ENABLE_REG(port->id),
		    MVPP2_ISR_ENABLE_INTERRUPT(sw_thread_mask));
}

static inline void mvpp2_interrupts_disable(struct mvpp2_port *port)
{
	int i, sw_thread_mask = 0;

	for (i = 0; i < port->nqvecs; i++)
		sw_thread_mask |= port->qvecs[i].sw_thread_mask;

	mvpp2_write(port->priv, MVPP2_ISR_ENABLE_REG(port->id),
		    MVPP2_ISR_DISABLE_INTERRUPT(sw_thread_mask));
}

static inline void mvpp2_qvec_interrupt_enable(struct mvpp2_queue_vector *qvec)
{
	struct mvpp2_port *port = qvec->port;

	mvpp2_write(port->priv, MVPP2_ISR_ENABLE_REG(port->id),
		    MVPP2_ISR_ENABLE_INTERRUPT(qvec->sw_thread_mask));
}

static inline void mvpp2_qvec_interrupt_disable(struct mvpp2_queue_vector *qvec)
{
	struct mvpp2_port *port = qvec->port;

	mvpp2_write(port->priv, MVPP2_ISR_ENABLE_REG(port->id),
		    MVPP2_ISR_DISABLE_INTERRUPT(qvec->sw_thread_mask));
}

/* Mask the current CPU's Rx/Tx interrupts
 * Called by on_each_cpu(), guaranteed to run with migration disabled,
 * using smp_processor_id() is OK.
 */
static void mvpp2_interrupts_mask(void *arg)
{
	struct mvpp2_port *port = arg;

	if (queue_mode == MVPP2_SINGLE_RESOURCE_MODE && smp_processor_id() != 0)
		return;

	mvpp2_percpu_write(port->priv, smp_processor_id(),
			   MVPP2_ISR_RX_TX_MASK_REG(port->id), 0);
}

/* Unmask the current CPU's Rx/Tx interrupts.
 * Called by on_each_cpu(), guaranteed to run with migration disabled,
 * using smp_processor_id() is OK.
 */
static void mvpp2_interrupts_unmask(void *arg)
{
	struct mvpp2_port *port = arg;
	u32 val;

	if (queue_mode == MVPP2_SINGLE_RESOURCE_MODE && smp_processor_id() != 0)
		return;

	if (port->flags & MVPP22_F_IF_MUSDK)
		return;

	val = MVPP2_CAUSE_MISC_SUM_MASK;

	if (port->priv->hw_version == MVPP21)
		val |= MVPP21_CAUSE_RXQ_OCCUP_DESC_ALL_MASK;
	else
		val |= MVPP22_CAUSE_RXQ_OCCUP_DESC_ALL_MASK;

	if (port->has_tx_irqs)
		val |= MVPP2_CAUSE_TXQ_OCCUP_DESC_ALL_MASK;

	mvpp2_percpu_write(port->priv, smp_processor_id(),
			   MVPP2_ISR_RX_TX_MASK_REG(port->id), val);
}

static void
mvpp2_shared_interrupt_mask_unmask(struct mvpp2_port *port, bool mask)
{
	u32 val;
	int i;

	if (port->priv->hw_version == MVPP21)
		return;

	if (mask)
		val = 0;
	else
		val = MVPP22_CAUSE_RXQ_OCCUP_DESC_ALL_MASK;

	for (i = 0; i < port->nqvecs; i++) {
		struct mvpp2_queue_vector *v = port->qvecs + i;

		if (v->type != MVPP2_QUEUE_VECTOR_SHARED)
			continue;

		mvpp2_percpu_write(port->priv, v->sw_thread_id,
				   MVPP2_ISR_RX_TX_MASK_REG(port->id), val);
	}
}

/* Port configuration routines */

static void mvpp22_gop_init_rgmii(struct mvpp2_port *port)
{
	struct mvpp2 *priv = port->priv;
	u32 val;

	regmap_read(priv->sysctrl_base, GENCONF_PORT_CTRL0, &val);
	val |= GENCONF_PORT_CTRL0_BUS_WIDTH_SELECT;
	regmap_write(priv->sysctrl_base, GENCONF_PORT_CTRL0, val);

	regmap_read(priv->sysctrl_base, GENCONF_CTRL0, &val);
	if (port->gop_id == 2)
		val |= GENCONF_CTRL0_PORT0_RGMII | GENCONF_CTRL0_PORT1_RGMII;
	else if (port->gop_id == 3)
		val |= GENCONF_CTRL0_PORT1_RGMII_MII;
	regmap_write(priv->sysctrl_base, GENCONF_CTRL0, val);
}

static void mvpp22_gop_init_sgmii(struct mvpp2_port *port)
{
	struct mvpp2 *priv = port->priv;
	u32 val;

	regmap_read(priv->sysctrl_base, GENCONF_PORT_CTRL0, &val);
	val |= GENCONF_PORT_CTRL0_BUS_WIDTH_SELECT |
	       GENCONF_PORT_CTRL0_RX_DATA_SAMPLE;
	regmap_write(priv->sysctrl_base, GENCONF_PORT_CTRL0, val);

	if (port->gop_id > 1) {
		regmap_read(priv->sysctrl_base, GENCONF_CTRL0, &val);
		if (port->gop_id == 2)
			val &= ~GENCONF_CTRL0_PORT0_RGMII;
		else if (port->gop_id == 3)
			val &= ~GENCONF_CTRL0_PORT1_RGMII_MII;
		regmap_write(priv->sysctrl_base, GENCONF_CTRL0, val);
	}
}

static void mvpp22_gop_init_10gkr(struct mvpp2_port *port)
{
	struct mvpp2 *priv = port->priv;
	void __iomem *mpcs = priv->iface_base + MVPP22_MPCS_BASE(port->gop_id);
	void __iomem *xpcs = priv->iface_base + MVPP22_XPCS_BASE(port->gop_id);
	u32 val;

	/* XPCS */
	val = readl(xpcs + MVPP22_XPCS_CFG0);
	val &= ~(MVPP22_XPCS_CFG0_PCS_MODE(0x3) |
		 MVPP22_XPCS_CFG0_ACTIVE_LANE(0x3));
	val |= MVPP22_XPCS_CFG0_ACTIVE_LANE(2);
	writel(val, xpcs + MVPP22_XPCS_CFG0);

	/* MPCS */
	val = readl(mpcs + MVPP22_MPCS_CTRL);
	val &= ~MVPP22_MPCS_CTRL_FWD_ERR_CONN;
	writel(val, mpcs + MVPP22_MPCS_CTRL);

	val = readl(mpcs + MVPP22_MPCS_CLK_RESET);
	val &= ~(MVPP22_MPCS_CLK_RESET_DIV_RATIO(0x7) | MAC_CLK_RESET_MAC |
		 MAC_CLK_RESET_SD_RX | MAC_CLK_RESET_SD_TX);
	val |= MVPP22_MPCS_CLK_RESET_DIV_RATIO(1);
	writel(val, mpcs + MVPP22_MPCS_CLK_RESET);

	val &= ~MVPP22_MPCS_CLK_RESET_DIV_SET;
	val |= MAC_CLK_RESET_MAC | MAC_CLK_RESET_SD_RX | MAC_CLK_RESET_SD_TX;
	writel(val, mpcs + MVPP22_MPCS_CLK_RESET);
}

static int mvpp22_gop_init(struct mvpp2_port *port)
{
	struct mvpp2 *priv = port->priv;
	u32 val;

	if (!priv->sysctrl_base)
		return 0;

	switch (port->phy_interface) {
	case PHY_INTERFACE_MODE_RGMII:
	case PHY_INTERFACE_MODE_RGMII_ID:
	case PHY_INTERFACE_MODE_RGMII_RXID:
	case PHY_INTERFACE_MODE_RGMII_TXID:
		if (port->gop_id == 0)
			goto invalid_conf;
		mvpp22_gop_init_rgmii(port);
		break;
	case PHY_INTERFACE_MODE_SGMII:
	case PHY_INTERFACE_MODE_1000BASEX:
	case PHY_INTERFACE_MODE_2500BASEX:
		mvpp22_gop_init_sgmii(port);
		break;
	case PHY_INTERFACE_MODE_10GKR:
		if (port->gop_id != 0)
			goto invalid_conf;
		mvpp22_gop_init_10gkr(port);
		break;
	default:
		goto unsupported_conf;
	}

	regmap_read(priv->sysctrl_base, GENCONF_PORT_CTRL1, &val);
	val |= GENCONF_PORT_CTRL1_RESET(port->gop_id) |
	       GENCONF_PORT_CTRL1_EN(port->gop_id);
	regmap_write(priv->sysctrl_base, GENCONF_PORT_CTRL1, val);

	regmap_read(priv->sysctrl_base, GENCONF_PORT_CTRL0, &val);
	val |= GENCONF_PORT_CTRL0_CLK_DIV_PHASE_CLR;
	regmap_write(priv->sysctrl_base, GENCONF_PORT_CTRL0, val);

	regmap_read(priv->sysctrl_base, GENCONF_SOFT_RESET1, &val);
	val |= GENCONF_SOFT_RESET1_GOP;
	regmap_write(priv->sysctrl_base, GENCONF_SOFT_RESET1, val);

unsupported_conf:
	return 0;

invalid_conf:
	netdev_err(port->dev, "Invalid port configuration\n");
	return -EINVAL;
}

static void mvpp22_gop_unmask_irq(struct mvpp2_port *port)
{
	u32 val;

	if (phy_interface_mode_is_rgmii(port->phy_interface) ||
	    port->phy_interface == PHY_INTERFACE_MODE_SGMII ||
	    port->phy_interface == PHY_INTERFACE_MODE_1000BASEX ||
	    port->phy_interface == PHY_INTERFACE_MODE_2500BASEX) {
		/* Enable the GMAC link status irq for this port */
		val = readl(port->base + MVPP22_GMAC_INT_SUM_MASK);
		val |= MVPP22_GMAC_INT_SUM_MASK_LINK_STAT;
		writel(val, port->base + MVPP22_GMAC_INT_SUM_MASK);
	}

	if (port->gop_id == 0) {
		/* Enable the XLG/GIG irqs for this port */
		val = readl(port->base + MVPP22_XLG_EXT_INT_MASK);
		if (port->phy_interface == PHY_INTERFACE_MODE_10GKR)
			val |= MVPP22_XLG_EXT_INT_MASK_XLG;
		else
			val |= MVPP22_XLG_EXT_INT_MASK_GIG;
		writel(val, port->base + MVPP22_XLG_EXT_INT_MASK);
	}
}

static void mvpp22_gop_mask_irq(struct mvpp2_port *port)
{
	u32 val;

	if (port->gop_id == 0) {
		val = readl(port->base + MVPP22_XLG_EXT_INT_MASK);
		val &= ~(MVPP22_XLG_EXT_INT_MASK_XLG |
			 MVPP22_XLG_EXT_INT_MASK_GIG);
		writel(val, port->base + MVPP22_XLG_EXT_INT_MASK);
	}

	if (phy_interface_mode_is_rgmii(port->phy_interface) ||
	    port->phy_interface == PHY_INTERFACE_MODE_SGMII ||
	    port->phy_interface == PHY_INTERFACE_MODE_1000BASEX ||
	    port->phy_interface == PHY_INTERFACE_MODE_2500BASEX) {
		val = readl(port->base + MVPP22_GMAC_INT_SUM_MASK);
		val &= ~MVPP22_GMAC_INT_SUM_MASK_LINK_STAT;
		writel(val, port->base + MVPP22_GMAC_INT_SUM_MASK);
	}
}

static void mvpp22_gop_setup_irq(struct mvpp2_port *port)
{
	u32 val;

	if (phy_interface_mode_is_rgmii(port->phy_interface) ||
	    port->phy_interface == PHY_INTERFACE_MODE_SGMII ||
	    port->phy_interface == PHY_INTERFACE_MODE_1000BASEX ||
	    port->phy_interface == PHY_INTERFACE_MODE_2500BASEX) {
		val = readl(port->base + MVPP22_GMAC_INT_MASK);
		val |= MVPP22_GMAC_INT_MASK_LINK_STAT;
		writel(val, port->base + MVPP22_GMAC_INT_MASK);
	}

	if (port->gop_id == 0) {
		val = readl(port->base + MVPP22_XLG_INT_MASK);
		val |= MVPP22_XLG_INT_MASK_LINK;
		writel(val, port->base + MVPP22_XLG_INT_MASK);
	}

	mvpp22_gop_unmask_irq(port);
}

/* Sets the PHY mode of the COMPHY (which configures the serdes lanes).
 *
 * The PHY mode used by the PPv2 driver comes from the network subsystem, while
 * the one given to the COMPHY comes from the generic PHY subsystem. Hence they
 * differ.
 *
 * The COMPHY configures the serdes lanes regardless of the actual use of the
 * lanes by the physical layer. This is why configurations like
 * "PPv2 (2500BaseX) - COMPHY (2500SGMII)" are valid.
 */
static int mvpp22_comphy_init(struct mvpp2_port *port)
{
	enum phy_mode mode;
	int ret;

	if (!port->comphy)
		return 0;

	switch (port->phy_interface) {
	case PHY_INTERFACE_MODE_SGMII:
	case PHY_INTERFACE_MODE_1000BASEX:
		mode = PHY_MODE_SGMII;
		break;
	case PHY_INTERFACE_MODE_2500BASEX:
		mode = PHY_MODE_2500SGMII;
		break;
	case PHY_INTERFACE_MODE_10GKR:
		mode = PHY_MODE_10GKR;
		break;
	default:
		return -EINVAL;
	}

	ret = phy_set_mode(port->comphy, mode);
	if (ret)
		return ret;

	return phy_power_on(port->comphy);
}

static void mvpp2_xlg_port_reset(struct mvpp2_port *port, bool active)
{
	u32 val;
	bool orig_active;

	val = readl(port->base + MVPP22_XLG_CTRL0_REG);
	/* Reset is active when RESET_DISable bit is zero */
	orig_active = !(val & MVPP22_XLG_CTRL0_MAC_RESET_DIS);
	if (orig_active != active) {
		if (active)
			val &= ~MVPP22_XLG_CTRL0_MAC_RESET_DIS;
		else
			val |= MVPP22_XLG_CTRL0_MAC_RESET_DIS;
		writel(val, port->base + MVPP22_XLG_CTRL0_REG);
	}
}

static void mvpp2_port_enable(struct mvpp2_port *port)
{
	u32 val;

	/* Only GOP port 0 has an XLG MAC */
	if (port->gop_id == 0 &&
	    (port->phy_interface == PHY_INTERFACE_MODE_XAUI ||
	     port->phy_interface == PHY_INTERFACE_MODE_10GKR)) {
		val = readl(port->base + MVPP22_XLG_CTRL0_REG);
		val |= MVPP22_XLG_CTRL0_PORT_EN |
		       MVPP22_XLG_CTRL0_MAC_RESET_DIS;
		val &= ~MVPP22_XLG_CTRL0_MIB_CNT_DIS;
		writel(val, port->base + MVPP22_XLG_CTRL0_REG);
	} else {
		val = readl(port->base + MVPP2_GMAC_CTRL_0_REG);
		val |= MVPP2_GMAC_PORT_EN_MASK;
		val |= MVPP2_GMAC_MIB_CNTR_EN_MASK;
		writel(val, port->base + MVPP2_GMAC_CTRL_0_REG);
	}
}

static void mvpp2_port_disable(struct mvpp2_port *port)
{
	u32 val;

	/* Only GOP port 0 has an XLG MAC */
	if (port->gop_id == 0 &&
	    (port->phy_interface == PHY_INTERFACE_MODE_XAUI ||
	     port->phy_interface == PHY_INTERFACE_MODE_10GKR)) {
		/* Disable & Reset should be done separately */
		val = readl(port->base + MVPP22_XLG_CTRL0_REG);
		val &= ~MVPP22_XLG_CTRL0_PORT_EN;
		writel(val, port->base + MVPP22_XLG_CTRL0_REG);
		mvpp2_xlg_port_reset(port, true);
	} else {
		val = readl(port->base + MVPP2_GMAC_CTRL_0_REG);
		val &= ~(MVPP2_GMAC_PORT_EN_MASK);
		writel(val, port->base + MVPP2_GMAC_CTRL_0_REG);
	}
}

/* Set IEEE 802.3x Flow Control Xon Packet Transmission Mode */
static void mvpp2_port_periodic_xon_disable(struct mvpp2_port *port)
{
	u32 val;

	val = readl(port->base + MVPP2_GMAC_CTRL_1_REG) &
		    ~MVPP2_GMAC_PERIODIC_XON_EN_MASK;
	writel(val, port->base + MVPP2_GMAC_CTRL_1_REG);
}

/* Configure loopback port */
static void mvpp2_port_loopback_set(struct mvpp2_port *port,
				    const struct phylink_link_state *state)
{
	u32 val;

	val = readl(port->base + MVPP2_GMAC_CTRL_1_REG);

	if (state->speed == 1000)
		val |= MVPP2_GMAC_GMII_LB_EN_MASK;
	else
		val &= ~MVPP2_GMAC_GMII_LB_EN_MASK;

	if (port->phy_interface == PHY_INTERFACE_MODE_SGMII ||
	    port->phy_interface == PHY_INTERFACE_MODE_1000BASEX ||
	    port->phy_interface == PHY_INTERFACE_MODE_2500BASEX)
		val |= MVPP2_GMAC_PCS_LB_EN_MASK;
	else
		val &= ~MVPP2_GMAC_PCS_LB_EN_MASK;

	writel(val, port->base + MVPP2_GMAC_CTRL_1_REG);
}

struct mvpp2_ethtool_counter {
	unsigned int offset;
	const char string[ETH_GSTRING_LEN];
	bool reg_is_64b;
};

static u64 mvpp2_read_count(struct mvpp2_port *port,
			    const struct mvpp2_ethtool_counter *counter)
{
	u64 val;

	val = readl(port->stats_base + counter->offset);
	if (counter->reg_is_64b)
		val += (u64)readl(port->stats_base + counter->offset + 4) << 32;

	return val;
}

/* Due to the fact that software statistics and hardware statistics are, by
 * design, incremented at different moments in the chain of packet processing,
 * it is very likely that incoming packets could have been dropped after being
 * counted by hardware but before reaching software statistics (most probably
 * multicast packets), and in the oppposite way, during transmission, FCS bytes
 * are added in between as well as TSO skb will be split and header bytes added.
 * Hence, statistics gathered from userspace with ifconfig (software) and
 * ethtool (hardware) cannot be compared.
 */
static const struct mvpp2_ethtool_counter mvpp2_ethtool_regs[] = {
	{ MVPP2_MIB_GOOD_OCTETS_RCVD, "good_octets_received", true },
	{ MVPP2_MIB_BAD_OCTETS_RCVD, "bad_octets_received" },
	{ MVPP2_MIB_CRC_ERRORS_SENT, "crc_errors_sent" },
	{ MVPP2_MIB_UNICAST_FRAMES_RCVD, "unicast_frames_received" },
	{ MVPP2_MIB_BROADCAST_FRAMES_RCVD, "broadcast_frames_received" },
	{ MVPP2_MIB_MULTICAST_FRAMES_RCVD, "multicast_frames_received" },
	{ MVPP2_MIB_FRAMES_64_OCTETS, "frames_64_octets" },
	{ MVPP2_MIB_FRAMES_65_TO_127_OCTETS, "frames_65_to_127_octet" },
	{ MVPP2_MIB_FRAMES_128_TO_255_OCTETS, "frames_128_to_255_octet" },
	{ MVPP2_MIB_FRAMES_256_TO_511_OCTETS, "frames_256_to_511_octet" },
	{ MVPP2_MIB_FRAMES_512_TO_1023_OCTETS, "frames_512_to_1023_octet" },
	{ MVPP2_MIB_FRAMES_1024_TO_MAX_OCTETS, "frames_1024_to_max_octet" },
	{ MVPP2_MIB_GOOD_OCTETS_SENT, "good_octets_sent", true },
	{ MVPP2_MIB_UNICAST_FRAMES_SENT, "unicast_frames_sent" },
	{ MVPP2_MIB_MULTICAST_FRAMES_SENT, "multicast_frames_sent" },
	{ MVPP2_MIB_BROADCAST_FRAMES_SENT, "broadcast_frames_sent" },
	{ MVPP2_MIB_FC_SENT, "fc_sent" },
	{ MVPP2_MIB_FC_RCVD, "fc_received" },
	{ MVPP2_MIB_RX_FIFO_OVERRUN, "rx_fifo_overrun" },
	{ MVPP2_MIB_UNDERSIZE_RCVD, "undersize_received" },
	{ MVPP2_MIB_FRAGMENTS_ERR_RCVD, "fragments_err_received" },
	{ MVPP2_MIB_OVERSIZE_RCVD, "oversize_received" },
	{ MVPP2_MIB_JABBER_RCVD, "jabber_received" },
	{ MVPP2_MIB_MAC_RCV_ERROR, "mac_receive_error" },
	{ MVPP2_MIB_BAD_CRC_EVENT, "bad_crc_event" },
	{ MVPP2_MIB_COLLISION, "collision" },
	{ MVPP2_MIB_LATE_COLLISION, "late_collision" },

	/* Extend counters */
#define MVPP2_FIRST_CNT		MVPP2_OVERRUN_DROP_REG(0)
	{ MVPP2_OVERRUN_DROP_REG(0),	" rx_ppv2_overrun" },
	{ MVPP2_CLS_DROP_REG(0),	" rx_cls_drop    " },
	{ MVPP2_RX_PKT_FULLQ_DROP_REG,	" rx_fullq_drop  " },
	{ MVPP2_RX_PKT_EARLY_DROP_REG,	" rx_early_drop  " },
	{ MVPP2_RX_PKT_BM_DROP_REG,	" rx_bm_drop     " },
};

static const char mvpp22_priv_flags_strings[][ETH_GSTRING_LEN] = {
	"musdk",
};

#define MVPP22_F_IF_MUSDK_PRIV	BIT(0)

static void mvpp2_ethtool_get_cntr_sizes(struct mvpp2_port *port,
					 int *total_size, int *mib_size)
{
	int i = 0;

	*total_size = ARRAY_SIZE(mvpp2_ethtool_regs);
	while (i < *total_size) {
		if (mvpp2_ethtool_regs[i].offset == MVPP2_FIRST_CNT)
			break;
		i++;
	}
	*mib_size = i;
}

/* hw_get_stats - update the ethtool_stats accumulator from HW-registers
 * The HW-registers/counters are cleared on read.
 */
static void mvpp2_hw_get_stats(struct mvpp2_port *port, u64 *pstats)
{
	int i, array_size, mib_size, queue;
	unsigned int reg_offs;
	u64 *ptmp;

	mvpp2_ethtool_get_cntr_sizes(port, &array_size, &mib_size);

	for (i = 0; i < mib_size; i++)
		*pstats++ += mvpp2_read_count(port, &mvpp2_ethtool_regs[i]);

	/* Extend counters */
	*pstats++ += mvpp2_read(port->priv, MVPP2_OVERRUN_DROP_REG(port->id));
	*pstats++ += mvpp2_read(port->priv, MVPP2_CLS_DROP_REG(port->id));
	ptmp = pstats;
	queue = port->first_rxq;
	while (queue < (port->first_rxq + port->nrxqs)) {
		mvpp2_write(port->priv, MVPP2_CNT_IDX_REG, queue++);
		pstats = ptmp;
		i = mib_size + 2;
		while (i < array_size) {
			reg_offs = mvpp2_ethtool_regs[i++].offset;
			*pstats++ += mvpp2_read(port->priv, reg_offs);
		}
	}
}

static void mvpp2_hw_clear_stats(struct mvpp2_port *port)
{
	int i, array_size, mib_size, queue;
	unsigned int reg_offs;

	mvpp2_ethtool_get_cntr_sizes(port, &array_size, &mib_size);

	for (i = 0; i < mib_size; i++)
		mvpp2_read_count(port, &mvpp2_ethtool_regs[i]);

	/* Extend counters */
	mvpp2_read(port->priv, MVPP2_OVERRUN_DROP_REG(port->id));
	mvpp2_read(port->priv, MVPP2_CLS_DROP_REG(port->id));
	queue = port->first_rxq;
	while (queue < (port->first_rxq + port->nrxqs)) {
		mvpp2_write(port->priv, MVPP2_CNT_IDX_REG, queue++);
		i = mib_size + 2;
		while (i < array_size) {
			reg_offs = mvpp2_ethtool_regs[i++].offset;
			mvpp2_read(port->priv, reg_offs);
		}
	}
}

static void mvpp2_ethtool_get_strings(struct net_device *netdev, u32 sset,
				      u8 *data)
{
	int i, array_size, mib_size;
	struct mvpp2_port *port = netdev_priv(netdev);

	switch (sset) {
	case ETH_SS_STATS:
		mvpp2_ethtool_get_cntr_sizes(port, &array_size, &mib_size);
		for (i = 0; i < array_size; i++)
			memcpy(data + i * ETH_GSTRING_LEN,
			       &mvpp2_ethtool_regs[i].string, ETH_GSTRING_LEN);
		break;
	case ETH_SS_PRIV_FLAGS:
		memcpy(data, mvpp22_priv_flags_strings,
		       ARRAY_SIZE(mvpp22_priv_flags_strings) * ETH_GSTRING_LEN);
	}
}

static void mvpp2_gather_hw_statistics(struct work_struct *work)
{
	struct delayed_work *del_work = to_delayed_work(work);
	struct mvpp2_port *port = container_of(del_work, struct mvpp2_port,
					       stats_work);

	/* Update the statistic buffer by q-work only, not by ethtool-S */
	mutex_lock(&port->gather_stats_lock);
	mvpp2_hw_get_stats(port, port->ethtool_stats);
	mutex_unlock(&port->gather_stats_lock);
	queue_delayed_work(port->priv->stats_queue, &port->stats_work,
			   MVPP2_MIB_COUNTERS_STATS_DELAY);
}

static void mvpp2_ethtool_get_stats(struct net_device *dev,
				    struct ethtool_stats *stats, u64 *data)
{
	struct mvpp2_port *port = netdev_priv(dev);

	/* Use statistic already accumulated in ethtool_stats by q-work
	 * and copy under mutex-lock it into given ethtool-data-buffer.
	 */
	mutex_lock(&port->gather_stats_lock);
	memcpy(data, port->ethtool_stats,
	       sizeof(u64) * ARRAY_SIZE(mvpp2_ethtool_regs));
	mutex_unlock(&port->gather_stats_lock);
}

static int mvpp2_ethtool_get_sset_count(struct net_device *dev, int sset)
{
	struct mvpp2_port *port = netdev_priv(dev);
	int array_size, mib_size;

	switch (sset) {
	case ETH_SS_STATS:
		mvpp2_ethtool_get_cntr_sizes(port, &array_size, &mib_size);
		return array_size;

	case ETH_SS_PRIV_FLAGS:
		return (port->priv->hw_version == MVPP21) ?
			0 : ARRAY_SIZE(mvpp22_priv_flags_strings);
	}
	return -EOPNOTSUPP;
}

static void mvpp2_port_reset(struct mvpp2_port *port)
{
	u32 val;

	/* Read the GOP statistics to reset the hardware counters */
	mvpp2_hw_clear_stats(port);

	val = readl(port->base + MVPP2_GMAC_CTRL_2_REG) &
		    ~MVPP2_GMAC_PORT_RESET_MASK;
	writel(val, port->base + MVPP2_GMAC_CTRL_2_REG);

	while (readl(port->base + MVPP2_GMAC_CTRL_2_REG) &
	       MVPP2_GMAC_PORT_RESET_MASK)
		continue;
}

/* Change maximum receive size of the port */
static inline void mvpp2_gmac_max_rx_size_set(struct mvpp2_port *port)
{
	u32 val;

	val = readl(port->base + MVPP2_GMAC_CTRL_0_REG);
	val &= ~MVPP2_GMAC_MAX_RX_SIZE_MASK;
	val |= (((port->pkt_size - MVPP2_MH_SIZE) / 2) <<
		    MVPP2_GMAC_MAX_RX_SIZE_OFFS);
	writel(val, port->base + MVPP2_GMAC_CTRL_0_REG);
}

/* Change maximum receive size of the port */
static inline void mvpp2_xlg_max_rx_size_set(struct mvpp2_port *port)
{
	u32 val;

	val =  readl(port->base + MVPP22_XLG_CTRL1_REG);
	val &= ~MVPP22_XLG_CTRL1_FRAMESIZELIMIT_MASK;
	val |= ((port->pkt_size - MVPP2_MH_SIZE) / 2) <<
	       MVPP22_XLG_CTRL1_FRAMESIZELIMIT_OFFS;
	writel(val, port->base + MVPP22_XLG_CTRL1_REG);
}

/* Set defaults to the MVPP2 port */
static void mvpp2_defaults_set(struct mvpp2_port *port)
{
	int tx_port_num, val, queue, ptxq, lrxq;

	if (port->priv->hw_version == MVPP21) {
		/* Update TX FIFO MIN Threshold */
		val = readl(port->base + MVPP2_GMAC_PORT_FIFO_CFG_1_REG);
		val &= ~MVPP2_GMAC_TX_FIFO_MIN_TH_ALL_MASK;
		/* Min. TX threshold must be less than minimal packet length */
		val |= MVPP2_GMAC_TX_FIFO_MIN_TH_MASK(64 - 4 - 2);
		writel(val, port->base + MVPP2_GMAC_PORT_FIFO_CFG_1_REG);
	}

	/* Disable Legacy WRR, Disable EJP, Release from reset */
	tx_port_num = mvpp2_egress_port(port);
	mvpp2_write(port->priv, MVPP2_TXP_SCHED_PORT_INDEX_REG,
		    tx_port_num);
	mvpp2_write(port->priv, MVPP2_TXP_SCHED_CMD_1_REG, 0);

	/* Set TX-Queues to Round-Robin vs hw-default Fixed-Priority=0xFF */
	mvpp2_write(port->priv, MVPP2_TXP_SCHED_FIXED_PRIO_REG, 0);

	/* Close bandwidth for all queues */
	for (queue = 0; queue < MVPP2_MAX_TXQ; queue++) {
		ptxq = mvpp2_txq_phys(port->id, queue);
		mvpp2_write(port->priv,
			    MVPP2_TXQ_SCHED_TOKEN_CNTR_REG(ptxq), 0);
	}

	/* Set refill period to 1 usec, refill tokens
	 * and bucket size to maximum
	 */
	mvpp2_write(port->priv, MVPP2_TXP_SCHED_PERIOD_REG,
		    port->priv->tclk / USEC_PER_SEC);
	val = mvpp2_read(port->priv, MVPP2_TXP_SCHED_REFILL_REG);
	val &= ~MVPP2_TXP_REFILL_PERIOD_ALL_MASK;
	val |= MVPP2_TXP_REFILL_PERIOD_MASK(1);
	val |= MVPP2_TXP_REFILL_TOKENS_ALL_MASK;
	mvpp2_write(port->priv, MVPP2_TXP_SCHED_REFILL_REG, val);
	val = MVPP2_TXP_TOKEN_SIZE_MAX;
	mvpp2_write(port->priv, MVPP2_TXP_SCHED_TOKEN_SIZE_REG, val);

	/* Set MaximumLowLatencyPacketSize value to 256 */
	mvpp2_write(port->priv, MVPP2_RX_CTRL_REG(port->id),
		    MVPP2_RX_USE_PSEUDO_FOR_CSUM_MASK |
		    MVPP2_RX_LOW_LATENCY_PKT_SIZE(256));

	/* Enable Rx cache snoop */
	for (lrxq = 0; lrxq < port->nrxqs; lrxq++) {
		queue = port->rxqs[lrxq]->id;
		val = mvpp2_read(port->priv, MVPP2_RXQ_CONFIG_REG(queue));
		val |= MVPP2_SNOOP_PKT_SIZE_MASK |
			   MVPP2_SNOOP_BUF_HDR_MASK;
		mvpp2_write(port->priv, MVPP2_RXQ_CONFIG_REG(queue), val);
	}

	/* At default, mask all interrupts to all present cpus */
	mvpp2_interrupts_disable(port);
}

/* Enable/disable receiving packets */
static void mvpp2_ingress_enable(struct mvpp2_port *port)
{
	u32 val;
	int lrxq, queue;

	for (lrxq = 0; lrxq < port->nrxqs; lrxq++) {
		queue = port->rxqs[lrxq]->id;
		val = mvpp2_read(port->priv, MVPP2_RXQ_CONFIG_REG(queue));
		val &= ~MVPP2_RXQ_DISABLE_MASK;
		mvpp2_write(port->priv, MVPP2_RXQ_CONFIG_REG(queue), val);
	}
}

static void mvpp2_ingress_disable(struct mvpp2_port *port)
{
	u32 val;
	int lrxq, queue;

	for (lrxq = 0; lrxq < port->nrxqs; lrxq++) {
		queue = port->rxqs[lrxq]->id;
		val = mvpp2_read(port->priv, MVPP2_RXQ_CONFIG_REG(queue));
		val |= MVPP2_RXQ_DISABLE_MASK;
		mvpp2_write(port->priv, MVPP2_RXQ_CONFIG_REG(queue), val);
	}
}

/* Enable transmit via physical egress queue
 * - HW starts take descriptors from DRAM
 */
static void mvpp2_egress_enable(struct mvpp2_port *port)
{
	u32 qmap;
	int queue;
	int tx_port_num = mvpp2_egress_port(port);

	if (port->flags && MVPP22_F_IF_MUSDK)
		return;

	/* Enable all initialized TXs. */
	qmap = 0;
	for (queue = 0; queue < port->ntxqs; queue++) {
		struct mvpp2_tx_queue *txq = port->txqs[queue];

		if (txq->descs)
			qmap |= (1 << queue);
	}

	mvpp2_write(port->priv, MVPP2_TXP_SCHED_PORT_INDEX_REG, tx_port_num);
	mvpp2_write(port->priv, MVPP2_TXP_SCHED_Q_CMD_REG, qmap);
}

/* Disable transmit via physical egress queue
 * - HW doesn't take descriptors from DRAM
 */
static void mvpp2_egress_disable(struct mvpp2_port *port)
{
	u32 reg_data;
	int delay;
	int tx_port_num = mvpp2_egress_port(port);

	if (port->flags && MVPP22_F_IF_MUSDK)
		return;

	/* Issue stop command for active channels only */
	mvpp2_write(port->priv, MVPP2_TXP_SCHED_PORT_INDEX_REG, tx_port_num);
	reg_data = (mvpp2_read(port->priv, MVPP2_TXP_SCHED_Q_CMD_REG)) &
		    MVPP2_TXP_SCHED_ENQ_MASK;
	if (reg_data != 0)
		mvpp2_write(port->priv, MVPP2_TXP_SCHED_Q_CMD_REG,
			    (reg_data << MVPP2_TXP_SCHED_DISQ_OFFSET));

	/* Wait for all Tx activity to terminate. */
	delay = 0;
	do {
		if (delay >= MVPP2_TX_DISABLE_TIMEOUT_MSEC) {
			netdev_warn(port->dev,
				    "Tx stop timed out, status=0x%08x\n",
				    reg_data);
			break;
		}
		mdelay(1);
		delay++;

		/* Check port TX Command register that all
		 * Tx queues are stopped
		 */
		reg_data = mvpp2_read(port->priv, MVPP2_TXP_SCHED_Q_CMD_REG);
	} while (reg_data & MVPP2_TXP_SCHED_ENQ_MASK);
}

/* Rx descriptors helper methods */

/* Get number of Rx descriptors occupied by received packets */
static inline int
mvpp2_rxq_received(struct mvpp2_port *port, int rxq_id)
{
	u32 val = mvpp2_read(port->priv, MVPP2_RXQ_STATUS_REG(rxq_id));

	return val & MVPP2_RXQ_OCCUPIED_MASK;
}

/* Update Rx queue status with the number of occupied and available
 * Rx descriptor slots.
 */
static inline void
mvpp2_rxq_status_update(struct mvpp2_port *port, int rxq_id,
			int used_count, int free_count)
{
	/* Decrement the number of used descriptors and increment count
	 * increment the number of free descriptors.
	 */
	u32 val = used_count | (free_count << MVPP2_RXQ_NUM_NEW_OFFSET);

	mvpp2_write(port->priv, MVPP2_RXQ_STATUS_UPDATE_REG(rxq_id), val);
}

/* Get pointer to next RX descriptor to be processed by SW */
static inline struct mvpp2_rx_desc *
mvpp2_rxq_next_desc_get(struct mvpp2_rx_queue *rxq)
{
	int rx_desc = rxq->next_desc_to_proc;

	rxq->next_desc_to_proc = MVPP2_QUEUE_NEXT_DESC(rxq, rx_desc);
	prefetch(rxq->descs + rxq->next_desc_to_proc);
	return rxq->descs + rx_desc;
}

/* Set rx queue offset */
static void mvpp2_rxq_offset_set(struct mvpp2_port *port,
				 int prxq, int offset)
{
	u32 val;

	/* Convert offset from bytes to units of 32 bytes */
	offset = offset >> 5;

	val = mvpp2_read(port->priv, MVPP2_RXQ_CONFIG_REG(prxq));
	val &= ~MVPP2_RXQ_PACKET_OFFSET_MASK;

	/* Offset is in */
	val |= ((offset << MVPP2_RXQ_PACKET_OFFSET_OFFS) &
		    MVPP2_RXQ_PACKET_OFFSET_MASK);

	mvpp2_write(port->priv, MVPP2_RXQ_CONFIG_REG(prxq), val);
}

/* Tx descriptors helper methods */

/* Get pointer to next Tx descriptor to be processed (send) by HW */
static struct mvpp2_tx_desc *
mvpp2_txq_next_desc_get(struct mvpp2_tx_queue *txq)
{
	int tx_desc = txq->next_desc_to_proc;

	txq->next_desc_to_proc = MVPP2_QUEUE_NEXT_DESC(txq, tx_desc);
	return txq->descs + tx_desc;
}

/* Update HW with number of aggregated Tx descriptors to be sent
 *
 * Called only from mvpp2_tx(), so migration is disabled, using
 * sw_thread is OK.
 */
static void mvpp2_aggr_txq_pend_desc_add(struct mvpp2_port *port, int pending,
					 int sw_thread)
{
	/* aggregated access - relevant TXQ number is written in TX desc */
	writel(pending,
	       port->priv->swth_base[sw_thread] + MVPP2_AGGR_TXQ_UPDATE_REG);
}

/* Check if there are enough free descriptors in aggregated txq.
 * If not, update the number of occupied descriptors and repeat the check.
 *
 * Called only from mvpp2_tx(), so migration is disabled, using
 * aggr_txq->id is OK.
 */
static int mvpp2_aggr_desc_num_check(struct mvpp2 *priv,
				     struct mvpp2_tx_queue *aggr_txq, int num)
{
	if ((aggr_txq->count + num) > MVPP2_AGGR_TXQ_SIZE) {
		/* Update number of occupied aggregated Tx descriptors */
		int cpu = aggr_txq->id;
		u32 val = mvpp2_read_relaxed(priv,
					     MVPP2_AGGR_TXQ_STATUS_REG(cpu));

		aggr_txq->count = val & MVPP2_AGGR_TXQ_PENDING_MASK;

		if ((aggr_txq->count + num) > MVPP2_AGGR_TXQ_SIZE)
			return -ENOMEM;
	}
	return 0;
}

/* Reserved Tx descriptors allocation request
 *
 * Called only from mvpp2_txq_reserved_desc_num_proc(), itself called
 * only by mvpp2_tx(), so migration is disabled, using
 * cpu is OK.
 */
static int mvpp2_txq_alloc_reserved_desc(struct mvpp2 *priv,
					 struct mvpp2_tx_queue *txq, int num,
					 int cpu)
{
	u32 val;

	val = (txq->id << MVPP2_TXQ_RSVD_REQ_Q_OFFSET) | num;
	mvpp2_percpu_write_relaxed(priv, cpu, MVPP2_TXQ_RSVD_REQ_REG, val);

	val = mvpp2_percpu_read_relaxed(priv, cpu, MVPP2_TXQ_RSVD_RSLT_REG);

	return val & MVPP2_TXQ_RSVD_RSLT_MASK;
}

/* Check if there are enough reserved descriptors for transmission.
 * If not, request chunk of reserved descriptors and check again.
 */
static int mvpp2_txq_reserved_desc_num_proc(struct mvpp2 *priv,
					    struct mvpp2_tx_queue *txq,
					    struct mvpp2_txq_pcpu *txq_pcpu,
					    int num)
{
	int req, cpu, desc_count;
	struct mvpp2_txq_pcpu *txq_pcpu_aux;

	if (txq_pcpu->reserved_num >= num)
		return 0;

	/* Not enough descriptors reserved! Update the reserved descriptor
	 * count and check again.
	 */
	if (num <= MAX_SKB_FRAGS) {
		req = MVPP2_CPU_DESC_CHUNK;
	} else {
		/* Compute total of used descriptors */
		desc_count = 0;
		for (cpu = 0; cpu < used_hifs; cpu++) {
			txq_pcpu_aux = per_cpu_ptr(txq->pcpu, cpu);
			desc_count += txq_pcpu_aux->reserved_num;
		}
		req = max(MVPP2_CPU_DESC_CHUNK, num - txq_pcpu->reserved_num);
		/* Check the reservation is possible */
		if ((desc_count + req) > txq->size)
			return -ENOMEM;
	}

	txq_pcpu->reserved_num += mvpp2_txq_alloc_reserved_desc(priv, txq, req,
								txq_pcpu->cpu);

	/* Check the resulting reservation is enough */
	if (txq_pcpu->reserved_num < num)
		return -ENOMEM;
	return 0;
}

/* Release the last allocated Tx descriptor. Useful to handle DMA
 * mapping failures in the Tx path.
 */
static void mvpp2_txq_desc_put(struct mvpp2_tx_queue *txq)
{
	if (txq->next_desc_to_proc == 0)
		txq->next_desc_to_proc = txq->last_desc - 1;
	else
		txq->next_desc_to_proc--;
}

/* Set Tx descriptors fields relevant for CSUM calculation */
static u32 mvpp2_txq_desc_csum(int l3_offs, int l3_proto,
			       int ip_hdr_len, int l4_proto)
{
	u32 command;

	/* fields: L3_offset, IP_hdrlen, L3_type, G_IPv4_chk,
	 * G_L4_chk, L4_type required only for checksum calculation
	 */
	command = (l3_offs << MVPP2_TXD_L3_OFF_SHIFT);
	command |= (ip_hdr_len << MVPP2_TXD_IP_HLEN_SHIFT);
	command |= MVPP2_TXD_IP_CSUM_DISABLE;

	if (l3_proto == htons(ETH_P_IP)) {
		command &= ~MVPP2_TXD_IP_CSUM_DISABLE;	/* enable IPv4 csum */
		command &= ~MVPP2_TXD_L3_IP6;		/* enable IPv4 */
	} else {
		command |= MVPP2_TXD_L3_IP6;		/* enable IPv6 */
	}

	if (l4_proto == IPPROTO_TCP) {
		command &= ~MVPP2_TXD_L4_UDP;		/* enable TCP */
		command &= ~MVPP2_TXD_L4_CSUM_FRAG;	/* generate L4 csum */
	} else if (l4_proto == IPPROTO_UDP) {
		command |= MVPP2_TXD_L4_UDP;		/* enable UDP */
		command &= ~MVPP2_TXD_L4_CSUM_FRAG;	/* generate L4 csum */
	} else {
		command |= MVPP2_TXD_L4_CSUM_NOT;
	}

	return command;
}

/* Get number of sent descriptors and decrement counter.
 * The number of sent descriptors is returned.
 * Per-CPU access
 *
 * Called only from mvpp2_txq_done(), called from mvpp2_tx()
 * (migration disabled) and from the TX completion tasklet (migration
 * disabled) so using cpu is OK.
 */
static inline int mvpp2_txq_sent_desc_proc(struct mvpp2_port *port,
					   struct mvpp2_tx_queue *txq, int cpu)
{
	u32 val;

	/* Reading status reg resets transmitted descriptor counter */
	val = mvpp2_percpu_read_relaxed(port->priv, cpu,
					MVPP2_TXQ_SENT_REG(txq->id));

	return (val & MVPP2_TRANSMITTED_COUNT_MASK) >>
		MVPP2_TRANSMITTED_COUNT_OFFSET;
}

/* Called through on_each_cpu(), so runs on all CPUs, with migration
 * disabled, therefore using smp_processor_id() is OK.
 */
static void mvpp2_txq_sent_counter_clear(void *arg)
{
	struct mvpp2_port *port = arg;
	int queue;

	if (queue_mode == MVPP2_SINGLE_RESOURCE_MODE && smp_processor_id() != 0)
		return;

	for (queue = 0; queue < port->ntxqs; queue++) {
		int id = port->txqs[queue]->id;

		mvpp2_percpu_read(port->priv, smp_processor_id(),
				  MVPP2_TXQ_SENT_REG(id));
	}
}

/* Avoid wrong tx_done calling for netif_tx_wake at time of
 * dev-stop or linkDown processing by flag MVPP2_F_IF_TX_ON.
 * Set/clear it on each cpu.
 */
static inline bool mvpp2_tx_stopped(struct mvpp2_port *port)
{
	return !(port->flags & MVPP2_F_IF_TX_ON);
}

static void mvpp2_txqs_on(void *arg)
{
	((struct mvpp2_port *)arg)->flags |= MVPP2_F_IF_TX_ON;
}

static void mvpp2_txqs_off(void *arg)
{
	((struct mvpp2_port *)arg)->flags &= ~MVPP2_F_IF_TX_ON;
}

static void mvpp2_txqs_on_tasklet_cb(unsigned long data)
{
	/* Activated/runs on 1 cpu only (with link_status_irq)
	 * to update/guarantee TX_ON coherency on other cpus
	 */
	struct mvpp2_port *port = (struct mvpp2_port *)data;

	if (mvpp2_tx_stopped(port))
		on_each_cpu(mvpp2_txqs_off, port, 1);
	else
		on_each_cpu(mvpp2_txqs_on, port, 1);
}

static void mvpp2_txqs_on_tasklet_init(struct mvpp2_port *port)
{
	/* Init called only for port with link_status_isr */
	tasklet_init(&port->txqs_on_tasklet,
		     mvpp2_txqs_on_tasklet_cb,
		     (unsigned long)port);
}

static void mvpp2_txqs_on_tasklet_kill(struct mvpp2_port *port)
{
	if (port->txqs_on_tasklet.func)
		tasklet_kill(&port->txqs_on_tasklet);
}

static void mvpp2_tx_start_all_queues(struct net_device *dev)
{
	struct mvpp2_port *port = netdev_priv(dev);

	if (port->flags & MVPP22_F_IF_MUSDK)
		return;
	/* Never called from IRQ. Update all cpus directly */
	on_each_cpu(mvpp2_txqs_on, port, 1);
	netif_tx_start_all_queues(dev);
}

static void mvpp2_tx_wake_all_queues(struct net_device *dev)
{
	struct mvpp2_port *port = netdev_priv(dev);

	if (port->flags & MVPP22_F_IF_MUSDK)
		return;
	if (irqs_disabled()) {
		/* IRQ context. Set THIS, update other cpus over tasklet */
		mvpp2_txqs_on((void *)port);
		tasklet_schedule(&port->txqs_on_tasklet);
	} else {
		on_each_cpu(mvpp2_txqs_on, port, 1);
	}
	netif_tx_wake_all_queues(dev);
}

static void mvpp2_tx_stop_all_queues(struct net_device *dev)
{
	struct mvpp2_port *port = netdev_priv(dev);

	if (port->flags & MVPP22_F_IF_MUSDK)
		return;
	if (irqs_disabled()) {
		/* IRQ context. Set THIS, update other cpus over tasklet */
		mvpp2_txqs_off((void *)port);
		tasklet_schedule(&port->txqs_on_tasklet);
	} else {
		on_each_cpu(mvpp2_txqs_off, port, 1);
	}
	netif_tx_stop_all_queues(dev);
}

/* Set max sizes for Tx queues */
static void mvpp2_txp_max_tx_size_set(struct mvpp2_port *port)
{
	u32	val, size, mtu;
	int	txq, tx_port_num;

	mtu = port->pkt_size * 8;
	if (mtu > MVPP2_TXP_MTU_MAX)
		mtu = MVPP2_TXP_MTU_MAX;

	/* WA for wrong Token bucket update: Set MTU value = 3*real MTU value */
	mtu = 3 * mtu;

	/* Indirect access to registers */
	tx_port_num = mvpp2_egress_port(port);
	mvpp2_write(port->priv, MVPP2_TXP_SCHED_PORT_INDEX_REG, tx_port_num);

	/* Set MTU */
	val = mvpp2_read(port->priv, MVPP2_TXP_SCHED_MTU_REG);
	val &= ~MVPP2_TXP_MTU_MAX;
	val |= mtu;
	mvpp2_write(port->priv, MVPP2_TXP_SCHED_MTU_REG, val);

	/* TXP token size and all TXQs token size must be larger that MTU */
	val = mvpp2_read(port->priv, MVPP2_TXP_SCHED_TOKEN_SIZE_REG);
	size = val & MVPP2_TXP_TOKEN_SIZE_MAX;
	if (size < mtu) {
		size = mtu;
		val &= ~MVPP2_TXP_TOKEN_SIZE_MAX;
		val |= size;
		mvpp2_write(port->priv, MVPP2_TXP_SCHED_TOKEN_SIZE_REG, val);
	}

	for (txq = 0; txq < port->ntxqs; txq++) {
		val = mvpp2_read(port->priv,
				 MVPP2_TXQ_SCHED_TOKEN_SIZE_REG(txq));
		size = val & MVPP2_TXQ_TOKEN_SIZE_MAX;

		if (size < mtu) {
			size = mtu;
			val &= ~MVPP2_TXQ_TOKEN_SIZE_MAX;
			val |= size;
			mvpp2_write(port->priv,
				    MVPP2_TXQ_SCHED_TOKEN_SIZE_REG(txq),
				    val);
		}
	}
}

/* Set the number of packets that will be received before Rx interrupt
 * will be generated by HW.
 */
static void mvpp2_rx_pkts_coal_set(struct mvpp2_port *port,
				   struct mvpp2_rx_queue *rxq)
{
	int cpu = get_cpu();

	if (rxq->pkts_coal > MVPP2_OCCUPIED_THRESH_MASK)
		rxq->pkts_coal = MVPP2_OCCUPIED_THRESH_MASK;

	mvpp2_percpu_write(port->priv, cpu, MVPP2_RXQ_NUM_REG, rxq->id);
	mvpp2_percpu_write(port->priv, cpu, MVPP2_RXQ_THRESH_REG,
			   rxq->pkts_coal);

	put_cpu();
}

/* Set pkts-coalescing HW with a given or configured VALUE for all TXQs */
static void mvpp2_tx_pkts_coal_set_util(struct mvpp2_port *port,
					int hif, u32 val)
{
	int num_hifs, queue;
	struct mvpp2_tx_queue *txq;

	if (hif < 0) {
		/* For all HIFs */
		hif = 0;
		num_hifs = used_hifs;
	} else if (hif < used_hifs) {
		/* For a given hif only */
		num_hifs = hif + 1;
	} else {
		/* used_hifs < hif < num_possible_cpus */
		return;
	}

	/* Not VALUE not specified, use configuration */
	if (val == -1) {
		txq = port->txqs[0];
		val = txq->done_pkts_coal;
	}
	for (queue = 0; queue < port->ntxqs; queue++) {
		txq = port->txqs[queue];
		for (hif = 0; hif < num_hifs; hif++) {
			mvpp2_percpu_write(port->priv, hif,
					   MVPP2_TXQ_NUM_REG, txq->id);
			mvpp2_percpu_write(port->priv, hif,
					   MVPP2_TXQ_THRESH_REG, val);
		}
	}
}

static void mvpp2_tx_pkts_coal_set_zero_pcpu(void *arg)
{
	struct mvpp2_port *port = arg;

	mvpp2_tx_pkts_coal_set_util(port, smp_processor_id(), 0);
}

static void mvpp2_tx_pkts_coal_set(struct mvpp2_port *port)
{
	/* Download registers of all CPUs with Configured value */
	mvpp2_tx_pkts_coal_set_util(port, -1, -1);
}

static u32 mvpp2_usec_to_cycles(u32 usec, unsigned long clk_hz)
{
	u64 tmp = (u64)clk_hz * usec;

	do_div(tmp, USEC_PER_SEC);

	return tmp > U32_MAX ? U32_MAX : tmp;
}

static u32 mvpp2_cycles_to_usec(u32 cycles, unsigned long clk_hz)
{
	u64 tmp = (u64)cycles * USEC_PER_SEC;

	do_div(tmp, clk_hz);

	return tmp > U32_MAX ? U32_MAX : tmp;
}

/* Set the time delay in usec before Rx interrupt */
static void mvpp2_rx_time_coal_set(struct mvpp2_port *port,
				   struct mvpp2_rx_queue *rxq)
{
	unsigned long freq = port->priv->tclk;
	u32 val = mvpp2_usec_to_cycles(rxq->time_coal, freq);

	if (val > MVPP2_MAX_ISR_RX_THRESHOLD) {
		rxq->time_coal =
			mvpp2_cycles_to_usec(MVPP2_MAX_ISR_RX_THRESHOLD, freq);

		/* re-evaluate to get actual register value */
		val = mvpp2_usec_to_cycles(rxq->time_coal, freq);
	}

	mvpp2_write(port->priv, MVPP2_ISR_RX_THRESHOLD_REG(rxq->id), val);
}

static void mvpp2_tx_time_coal_set(struct mvpp2_port *port)
{
	unsigned long freq = port->priv->tclk;
	u32 val = mvpp2_usec_to_cycles(port->tx_time_coal, freq);

	if (val > MVPP2_MAX_ISR_TX_THRESHOLD) {
		port->tx_time_coal =
			mvpp2_cycles_to_usec(MVPP2_MAX_ISR_TX_THRESHOLD, freq);

		/* re-evaluate to get actual register value */
		val = mvpp2_usec_to_cycles(port->tx_time_coal, freq);
	}

	mvpp2_write(port->priv, MVPP2_ISR_TX_THRESHOLD_REG(port->id), val);
}

/* Free Tx queue skbuffs */
static void mvpp2_txq_bufs_free(struct mvpp2_port *port,
				struct mvpp2_tx_queue *txq,
				struct mvpp2_txq_pcpu *txq_pcpu, int num)
{
	int i;

	for (i = 0; i < num; i++) {
		struct mvpp2_txq_pcpu_buf *tx_buf =
			txq_pcpu->buffs + txq_pcpu->txq_get_index;

		if (!IS_TSO_HEADER(txq_pcpu, tx_buf->dma)) {
			dma_unmap_single(port->dev->dev.parent, tx_buf->dma,
					 tx_buf->size, DMA_TO_DEVICE);
			mvpp2_recycle_put(txq_pcpu);
			/* sets tx_buf->skb=NULL if put to recycle */
		}
		if (tx_buf->skb)
			dev_kfree_skb_any(tx_buf->skb);

		mvpp2_txq_inc_get(txq_pcpu);
	}
}

static inline struct mvpp2_rx_queue *mvpp2_get_rx_queue(struct mvpp2_port *port,
							u32 cause)
{
	int queue = fls(cause) - 1;

	return port->rxqs[queue];
}

static inline struct mvpp2_tx_queue *mvpp2_get_tx_queue(struct mvpp2_port *port,
							u32 cause)
{
	int queue = fls(cause) - 1;

	return port->txqs[queue];
}

/* Handle end of transmission */
static void mvpp2_txq_done(struct mvpp2_port *port, struct mvpp2_tx_queue *txq,
			   struct mvpp2_txq_pcpu *txq_pcpu)
{
	struct netdev_queue *nq = netdev_get_tx_queue(port->dev, txq->log_id);
	int tx_done;

	if (queue_mode != MVPP2_SINGLE_RESOURCE_MODE) {
		if (txq_pcpu->cpu != smp_processor_id())
			netdev_err(port->dev, "wrong cpu on the end of Tx processing\n");
	} else {
		if (txq_pcpu->cpu != 0)
			netdev_err(port->dev, "wrong cpu on the end of Tx processing\n");
	}

	tx_done = mvpp2_txq_sent_desc_proc(port, txq, txq_pcpu->cpu);
	if (!tx_done)
		return;
	mvpp2_txq_bufs_free(port, txq, txq_pcpu, tx_done);

	txq_pcpu->count -= tx_done;

	if (netif_tx_queue_stopped(nq) && !mvpp2_tx_stopped(port) &&
	    txq_pcpu->count <= txq_pcpu->wake_threshold)
		netif_tx_wake_queue(nq);
}

static unsigned int mvpp2_tx_done(struct mvpp2_port *port, u32 cause,
				  int cpu)
{
	struct mvpp2_tx_queue *txq;
	struct mvpp2_txq_pcpu *txq_pcpu;
	unsigned int tx_todo = 0;

	while (cause) {
		txq = mvpp2_get_tx_queue(port, cause);
		if (!txq)
			break;

		txq_pcpu = per_cpu_ptr(txq->pcpu, cpu);

		if (txq_pcpu->count) {
			mvpp2_txq_done(port, txq, txq_pcpu);
			tx_todo += txq_pcpu->count;
		}

		cause &= ~(1 << txq->log_id);
	}
	return tx_todo;
}

/* Rx/Tx queue initialization/cleanup methods */

/* Allocate and initialize descriptors for aggr TXQ */
static int mvpp2_aggr_txq_init(struct platform_device *pdev,
			       struct mvpp2_tx_queue *aggr_txq, int cpu,
			       struct mvpp2 *priv)
{
	u32 txq_dma;

	/* Allocate memory for TX descriptors */
	aggr_txq->descs = dma_zalloc_coherent(&pdev->dev,
					      MVPP2_AGGR_TXQ_SIZE
					      * MVPP2_DESC_ALIGNED_SIZE,
					      &aggr_txq->descs_dma, GFP_KERNEL);
	if (!aggr_txq->descs)
		return -ENOMEM;

	aggr_txq->last_desc = MVPP2_AGGR_TXQ_SIZE - 1;

	/* Aggr TXQ no reset WA */
	aggr_txq->next_desc_to_proc = mvpp2_read(priv,
						 MVPP2_AGGR_TXQ_INDEX_REG(cpu));

	/* Set Tx descriptors queue starting address indirect
	 * access
	 */
	if (priv->hw_version == MVPP21)
		txq_dma = aggr_txq->descs_dma;
	else
		txq_dma = aggr_txq->descs_dma >>
			MVPP22_AGGR_TXQ_DESC_ADDR_OFFS;

	mvpp2_write(priv, MVPP2_AGGR_TXQ_DESC_ADDR_REG(cpu), txq_dma);
	mvpp2_write(priv, MVPP2_AGGR_TXQ_DESC_SIZE_REG(cpu),
		    MVPP2_AGGR_TXQ_SIZE);

	return 0;
}

/* Create a specified Rx queue */
static int mvpp2_rxq_init(struct mvpp2_port *port,
			  struct mvpp2_rx_queue *rxq)

{
	u32 rxq_dma;
	int cpu;

	rxq->size = port->rx_ring_size;

	/* Allocate memory for RX descriptors */
	rxq->descs = dma_alloc_coherent(port->dev->dev.parent,
					rxq->size * MVPP2_DESC_ALIGNED_SIZE,
					&rxq->descs_dma, GFP_KERNEL);
	if (!rxq->descs)
		return -ENOMEM;

	rxq->last_desc = rxq->size - 1;

	/* Zero occupied and non-occupied counters - direct access */
	mvpp2_write(port->priv, MVPP2_RXQ_STATUS_REG(rxq->id), 0);

	/* Set Rx descriptors queue starting address - indirect access */
	cpu = get_cpu();
	mvpp2_percpu_write(port->priv, cpu, MVPP2_RXQ_NUM_REG, rxq->id);
	if (port->priv->hw_version == MVPP21)
		rxq_dma = rxq->descs_dma;
	else
		rxq_dma = rxq->descs_dma >> MVPP22_DESC_ADDR_OFFS;
	mvpp2_percpu_write(port->priv, cpu, MVPP2_RXQ_DESC_ADDR_REG, rxq_dma);
	mvpp2_percpu_write(port->priv, cpu, MVPP2_RXQ_DESC_SIZE_REG, rxq->size);
	mvpp2_percpu_write(port->priv, cpu, MVPP2_RXQ_INDEX_REG, 0);
	put_cpu();

	/* Set Offset */
	mvpp2_rxq_offset_set(port, rxq->id, NET_SKB_PAD);

	/* Set coalescing pkts and time */
	mvpp2_rx_pkts_coal_set(port, rxq);
	mvpp2_rx_time_coal_set(port, rxq);

	/* Add number of descriptors ready for receiving packets */
	mvpp2_rxq_status_update(port, rxq->id, 0, rxq->size);

	return 0;
}

/* Push packets received by the RXQ to BM pool */
static void mvpp2_rxq_drop_pkts(struct mvpp2_port *port,
				struct mvpp2_rx_queue *rxq)
{
	int rx_received, i;

	rx_received = mvpp2_rxq_received(port, rxq->id);
	if (!rx_received)
		return;

	for (i = 0; i < rx_received; i++) {
		struct mvpp2_rx_desc *rx_desc = mvpp2_rxq_next_desc_get(rxq);
		u32 status;
		int pool;

		mvpp2_rx_desc_endian(port->priv->hw_version, rx_desc);
		status = mvpp2_rxdesc_status_get(port, rx_desc);
		pool = (status & MVPP2_RXD_BM_POOL_ID_MASK) >>
			MVPP2_RXD_BM_POOL_ID_OFFS;

		mvpp2_bm_pool_put(port, pool,
				  mvpp2_rxdesc_dma_addr_get(port, rx_desc));
	}
	mvpp2_rxq_status_update(port, rxq->id, rx_received, rx_received);
}

/* Cleanup Rx queue */
static void mvpp2_rxq_deinit(struct mvpp2_port *port,
			     struct mvpp2_rx_queue *rxq)
{
	int cpu;

	mvpp2_rxq_drop_pkts(port, rxq);

	if (rxq->descs)
		dma_free_coherent(port->dev->dev.parent,
				  rxq->size * MVPP2_DESC_ALIGNED_SIZE,
				  rxq->descs,
				  rxq->descs_dma);

	rxq->descs             = NULL;
	rxq->last_desc         = 0;
	rxq->next_desc_to_proc = 0;
	rxq->descs_dma         = 0;

	/* Clear Rx descriptors queue starting address and size;
	 * free descriptor number
	 */
	mvpp2_write(port->priv, MVPP2_RXQ_STATUS_REG(rxq->id), 0);
	cpu = get_cpu();
	mvpp2_percpu_write(port->priv, cpu, MVPP2_RXQ_NUM_REG, rxq->id);
	mvpp2_percpu_write(port->priv, cpu, MVPP2_RXQ_DESC_ADDR_REG, 0);
	mvpp2_percpu_write(port->priv, cpu, MVPP2_RXQ_DESC_SIZE_REG, 0);
	put_cpu();
}

/* Disable all rx/ingress queues, called by mvpp2_init */
static void mvpp2_rxq_disable_all(struct mvpp2 *priv)
{
	int i;
	u32 val;

	for (i = 0; i < MVPP2_RXQ_MAX_NUM; i++) {
		val = mvpp2_read(priv, MVPP2_RXQ_CONFIG_REG(i));
		val |= MVPP2_RXQ_DISABLE_MASK;
		mvpp2_write(priv, MVPP2_RXQ_CONFIG_REG(i), val);
	}
}

/* Create and initialize a Tx queue */
static int mvpp2_txq_init(struct mvpp2_port *port,
			  struct mvpp2_tx_queue *txq)
{
	u32 val;
	int cpu, desc, desc_per_txq, tx_port_num;
	struct mvpp2_txq_pcpu *txq_pcpu;

	txq->size = port->tx_ring_size;

	/* Allocate memory for Tx descriptors */
	txq->descs = dma_alloc_coherent(port->dev->dev.parent,
					txq->size * MVPP2_DESC_ALIGNED_SIZE,
				&txq->descs_dma, GFP_KERNEL);
	if (!txq->descs)
		return -ENOMEM;

	txq->last_desc = txq->size - 1;

	/* Set Tx descriptors queue starting address - indirect access */
	cpu = get_cpu();
	mvpp2_percpu_write(port->priv, cpu, MVPP2_TXQ_NUM_REG, txq->id);
	mvpp2_percpu_write(port->priv, cpu, MVPP2_TXQ_DESC_ADDR_REG,
			   txq->descs_dma);
	mvpp2_percpu_write(port->priv, cpu, MVPP2_TXQ_DESC_SIZE_REG,
			   txq->size & MVPP2_TXQ_DESC_SIZE_MASK);
	mvpp2_percpu_write(port->priv, cpu, MVPP2_TXQ_INDEX_REG, 0);
	mvpp2_percpu_write(port->priv, cpu, MVPP2_TXQ_RSVD_CLR_REG,
			   txq->id << MVPP2_TXQ_RSVD_CLR_OFFSET);
	val = mvpp2_percpu_read(port->priv, cpu, MVPP2_TXQ_PENDING_REG);
	val &= ~MVPP2_TXQ_PENDING_MASK;
	mvpp2_percpu_write(port->priv, cpu, MVPP2_TXQ_PENDING_REG, val);

	/* Calculate base address in prefetch buffer. We reserve 16 descriptors
	 * for each existing TXQ.
	 * TCONTS for PON port must be continuous from 0 to MVPP2_MAX_TCONT
	 * GBE ports assumed to be continuous from 0 to MVPP2_MAX_PORTS
	 */
	desc_per_txq = 16;
	desc = (port->id * MVPP2_MAX_TXQ * desc_per_txq) +
	       (txq->log_id * desc_per_txq);

	mvpp2_percpu_write(port->priv, cpu, MVPP2_TXQ_PREF_BUF_REG,
			   MVPP2_PREF_BUF_PTR(desc) | MVPP2_PREF_BUF_SIZE_16 |
			   MVPP2_PREF_BUF_THRESH(desc_per_txq / 2));
	put_cpu();

	/* WRR / EJP configuration - indirect access */
	tx_port_num = mvpp2_egress_port(port);
	mvpp2_write(port->priv, MVPP2_TXP_SCHED_PORT_INDEX_REG, tx_port_num);

	val = mvpp2_read(port->priv, MVPP2_TXQ_SCHED_REFILL_REG(txq->log_id));
	val &= ~MVPP2_TXQ_REFILL_PERIOD_ALL_MASK;
	val |= MVPP2_TXQ_REFILL_PERIOD_MASK(1);
	val |= MVPP2_TXQ_REFILL_TOKENS_ALL_MASK;
	mvpp2_write(port->priv, MVPP2_TXQ_SCHED_REFILL_REG(txq->log_id), val);

	val = MVPP2_TXQ_TOKEN_SIZE_MAX;
	mvpp2_write(port->priv, MVPP2_TXQ_SCHED_TOKEN_SIZE_REG(txq->log_id),
		    val);

	for (cpu = 0; cpu < used_hifs; cpu++) {
		txq_pcpu = per_cpu_ptr(txq->pcpu, cpu);
		txq_pcpu->size = txq->size;
		txq_pcpu->buffs = kmalloc_array(txq_pcpu->size,
						sizeof(*txq_pcpu->buffs),
						GFP_KERNEL);
		if (!txq_pcpu->buffs)
			return -ENOMEM;

		txq_pcpu->count = 0;
		txq_pcpu->reserved_num = 0;
		txq_pcpu->txq_put_index = 0;
		txq_pcpu->txq_get_index = 0;
		txq_pcpu->tso_headers = NULL;

		txq_pcpu->stop_threshold = txq->size -
						MVPP2_MAX_SKB_DESCS(used_hifs);
		txq_pcpu->wake_threshold = txq_pcpu->stop_threshold -
						MVPP2_TX_PAUSE_HYSTERESIS;

		txq_pcpu->tso_headers =
			dma_alloc_coherent(port->dev->dev.parent,
					   txq_pcpu->size * TSO_HEADER_SIZE,
					   &txq_pcpu->tso_headers_dma,
					   GFP_KERNEL);
		if (!txq_pcpu->tso_headers)
			return -ENOMEM;
	}

	return 0;
}

/* Free allocated TXQ resources */
static void mvpp2_txq_deinit(struct mvpp2_port *port,
			     struct mvpp2_tx_queue *txq)
{
	struct mvpp2_txq_pcpu *txq_pcpu;
	int cpu;

	for (cpu = 0; cpu < used_hifs; cpu++) {
		txq_pcpu = per_cpu_ptr(txq->pcpu, cpu);
		kfree(txq_pcpu->buffs);

		if (txq_pcpu->tso_headers)
			dma_free_coherent(port->dev->dev.parent,
					  txq_pcpu->size * TSO_HEADER_SIZE,
					  txq_pcpu->tso_headers,
					  txq_pcpu->tso_headers_dma);

		txq_pcpu->tso_headers = NULL;
	}

	if (txq->descs)
		dma_free_coherent(port->dev->dev.parent,
				  txq->size * MVPP2_DESC_ALIGNED_SIZE,
				  txq->descs, txq->descs_dma);

	txq->descs             = NULL;
	txq->last_desc         = 0;
	txq->next_desc_to_proc = 0;
	txq->descs_dma         = 0;

	/* Set minimum bandwidth for disabled TXQs */
	mvpp2_write(port->priv, MVPP2_TXQ_SCHED_TOKEN_CNTR_REG(txq->id), 0);

	/* Set Tx descriptors queue starting address and size */
	cpu = get_cpu();
	mvpp2_percpu_write(port->priv, cpu, MVPP2_TXQ_NUM_REG, txq->id);
	mvpp2_percpu_write(port->priv, cpu, MVPP2_TXQ_DESC_ADDR_REG, 0);
	mvpp2_percpu_write(port->priv, cpu, MVPP2_TXQ_DESC_SIZE_REG, 0);
	put_cpu();
}

/* Cleanup Tx ports */
static void mvpp2_txq_clean(struct mvpp2_port *port, struct mvpp2_tx_queue *txq)
{
	struct mvpp2_txq_pcpu *txq_pcpu;
	int delay, pending, cpu;
	u32 val;

	cpu = get_cpu();
	mvpp2_percpu_write(port->priv, cpu, MVPP2_TXQ_NUM_REG, txq->id);
	val = mvpp2_percpu_read(port->priv, cpu, MVPP2_TXQ_PREF_BUF_REG);
	val |= MVPP2_TXQ_DRAIN_EN_MASK;
	mvpp2_percpu_write(port->priv, cpu, MVPP2_TXQ_PREF_BUF_REG, val);

	/* The napi queue has been stopped so wait for all packets
	 * to be transmitted.
	 */
	delay = 0;
	do {
		if (delay >= MVPP2_TX_PENDING_TIMEOUT_MSEC) {
			netdev_warn(port->dev,
				    "port %d: cleaning queue %d timed out\n",
				    port->id, txq->log_id);
			break;
		}
		mdelay(1);
		delay++;

		pending = mvpp2_percpu_read(port->priv, cpu,
					    MVPP2_TXQ_PENDING_REG);
		pending &= MVPP2_TXQ_PENDING_MASK;
	} while (pending);

	val &= ~MVPP2_TXQ_DRAIN_EN_MASK;
	mvpp2_percpu_write(port->priv, cpu, MVPP2_TXQ_PREF_BUF_REG, val);
	put_cpu();

	for (cpu = 0; cpu < used_hifs; cpu++) {
		txq_pcpu = per_cpu_ptr(txq->pcpu, cpu);

		/* Release all packets */
		mvpp2_txq_bufs_free(port, txq, txq_pcpu, txq_pcpu->count);

		/* Reset queue */
		txq_pcpu->count = 0;
		txq_pcpu->txq_put_index = 0;
		txq_pcpu->txq_get_index = 0;
	}
}

/* Cleanup all Tx queues */
static void mvpp2_cleanup_txqs(struct mvpp2_port *port)
{
	struct mvpp2_tx_queue *txq;
	int queue;
	u32 val;

	val = mvpp2_read(port->priv, MVPP2_TX_PORT_FLUSH_REG);

	/* Reset Tx ports and delete Tx queues */
	val |= MVPP2_TX_PORT_FLUSH_MASK(port->id);
	mvpp2_write(port->priv, MVPP2_TX_PORT_FLUSH_REG, val);

	for (queue = 0; queue < port->ntxqs; queue++) {
		txq = port->txqs[queue];
		mvpp2_txq_clean(port, txq);
		mvpp2_txq_deinit(port, txq);
	}

	on_each_cpu(mvpp2_txq_sent_counter_clear, port, 1);

	val &= ~MVPP2_TX_PORT_FLUSH_MASK(port->id);
	mvpp2_write(port->priv, MVPP2_TX_PORT_FLUSH_REG, val);
}

/* Cleanup all Rx queues */
static void mvpp2_cleanup_rxqs(struct mvpp2_port *port)
{
	int queue;

	for (queue = 0; queue < port->nrxqs; queue++)
		mvpp2_rxq_deinit(port, port->rxqs[queue]);
}

/* Init all Rx queues for port */
static int mvpp2_setup_rxqs(struct mvpp2_port *port)
{
	int queue, err;

	for (queue = 0; queue < port->nrxqs; queue++) {
		err = mvpp2_rxq_init(port, port->rxqs[queue]);
		if (err)
			goto err_cleanup;
	}
	return 0;

err_cleanup:
	mvpp2_cleanup_rxqs(port);
	return err;
}

/* Init all tx queues for port */
static int mvpp2_setup_txqs(struct mvpp2_port *port)
{
	struct mvpp2_tx_queue *txq;
	int queue, err, cpu;

	for (queue = 0; queue < port->ntxqs; queue++) {
		txq = port->txqs[queue];
		err = mvpp2_txq_init(port, txq);
		if (err)
			goto err_cleanup;
	}

	/* XPS mapping queues to 0..N cpus (may be less than ntxqs) */
	for (cpu = 0; cpu < used_hifs; cpu++)
		netif_set_xps_queue(port->dev, cpumask_of(cpu), cpu);

	if (port->has_tx_irqs) {
		/* Download time-coal. The pkts-coal done in start_dev */
		mvpp2_tx_time_coal_set(port);
	}

	on_each_cpu(mvpp2_txq_sent_counter_clear, port, 1);
	return 0;

err_cleanup:
	mvpp2_cleanup_txqs(port);
	return err;
}

/* The callback for per-port interrupt */
static irqreturn_t mvpp2_isr(int irq, void *dev_id)
{
	struct mvpp2_queue_vector *qv = dev_id;

	mvpp2_qvec_interrupt_disable(qv);

	napi_schedule(&qv->napi);

	return IRQ_HANDLED;
}

/* Per-port interrupt for link status changes */
static irqreturn_t mvpp2_link_status_isr(int irq, void *dev_id)
{
	struct mvpp2_port *port = (struct mvpp2_port *)dev_id;
	struct net_device *dev = port->dev;
	bool event = false, link = false;
	u32 val;

	mvpp22_gop_mask_irq(port);

	if (port->gop_id == 0 &&
	    port->phy_interface == PHY_INTERFACE_MODE_10GKR) {
		val = readl(port->base + MVPP22_XLG_INT_STAT);
		if (val & MVPP22_XLG_INT_STAT_LINK) {
			event = true;
			val = readl(port->base + MVPP22_XLG_STATUS);
			if (val & MVPP22_XLG_STATUS_LINK_UP)
				link = true;
		}
	} else if (phy_interface_mode_is_rgmii(port->phy_interface) ||
		   port->phy_interface == PHY_INTERFACE_MODE_SGMII ||
		   port->phy_interface == PHY_INTERFACE_MODE_1000BASEX ||
		   port->phy_interface == PHY_INTERFACE_MODE_2500BASEX) {
		val = readl(port->base + MVPP22_GMAC_INT_STAT);
		if (val & MVPP22_GMAC_INT_STAT_LINK) {
			event = true;
			val = readl(port->base + MVPP2_GMAC_STATUS0);
			if (val & MVPP2_GMAC_STATUS0_LINK_UP)
				link = true;
		}
	}

	if (port->phylink) {
		phylink_mac_change(port->phylink, link);
		goto handled;
	}

	if (!netif_running(dev) || !event)
		goto handled;

	if (link) {
		mvpp2_interrupts_enable(port);

		mvpp2_egress_enable(port);
		mvpp2_ingress_enable(port);
		netif_carrier_on(dev);
		mvpp2_tx_wake_all_queues(dev);
	} else {
		mvpp2_tx_stop_all_queues(dev);
		netif_carrier_off(dev);
		mvpp2_ingress_disable(port);
		mvpp2_egress_disable(port);

		mvpp2_interrupts_disable(port);
	}

handled:
	mvpp22_gop_unmask_irq(port);
	return IRQ_HANDLED;
}

static void mvpp2_timer_set(struct mvpp2_port_pcpu *port_pcpu)
{
	ktime_t interval;

	if (!port_pcpu->tx_done_timer_scheduled) {
		port_pcpu->tx_done_timer_scheduled = true;
		interval = MVPP2_TXDONE_HRTIMER_PERIOD_NS;
		hrtimer_start(&port_pcpu->tx_done_timer, interval,
			      HRTIMER_MODE_REL_PINNED);
	}
}

static void mvpp2_tx_proc_cb(unsigned long data)
{
	struct net_device *dev = (struct net_device *)data;
	struct mvpp2_port *port = netdev_priv(dev);
	int sw_thread = mvpp2_check_sw_thread(smp_processor_id());
	struct mvpp2_port_pcpu *port_pcpu;
	unsigned int tx_todo, cause;

	if (!netif_running(dev))
		return;
	port_pcpu = per_cpu_ptr(port->pcpu, sw_thread);
	port_pcpu->tx_done_timer_scheduled = false;

	/* Process all the Tx queues */
	cause = (1 << port->ntxqs) - 1;
	tx_todo = mvpp2_tx_done(port, cause, sw_thread);

	/* Set the timer in case not all the packets were processed */
	if (tx_todo)
		mvpp2_timer_set(port_pcpu);
}

static enum hrtimer_restart mvpp2_hr_timer_cb(struct hrtimer *timer)
{
	struct mvpp2_port_pcpu *port_pcpu = container_of(timer,
							 struct mvpp2_port_pcpu,
							 tx_done_timer);

	tasklet_schedule(&port_pcpu->tx_done_tasklet);

	return HRTIMER_NORESTART;
}

/* Bulk-timer could be started/restarted by XMIT, timer-cb or Tasklet.
 *  XMIT calls bulk-restart() which is CONDITIONAL (restart vs request).
 *  Timer-cb has own condition-logic, calls hrtimer_forward().
 *  Tasklet has own condition-logic, calls unconditional bulk-start().
 *  The flags scheduled::restart_req are used in the state-logic.
 */
static inline void mvpp2_bulk_timer_restart(struct mvpp2_port_pcpu *port_pcpu)
{
	if (!port_pcpu->bulk_timer_scheduled) {
		port_pcpu->bulk_timer_scheduled = true;
		hrtimer_start(&port_pcpu->bulk_timer, MVPP2_TX_BULK_TIME,
			      HRTIMER_MODE_REL_PINNED);
	} else {
		port_pcpu->bulk_timer_restart_req = true;
	}
}

static void mvpp2_bulk_timer_start(struct mvpp2_port_pcpu *port_pcpu)
{
	port_pcpu->bulk_timer_scheduled = true;
	port_pcpu->bulk_timer_restart_req = false;
	hrtimer_start(&port_pcpu->bulk_timer, MVPP2_TX_BULK_TIME,
		      HRTIMER_MODE_REL_PINNED);
}

static enum hrtimer_restart mvpp2_bulk_timer_cb(struct hrtimer *timer)
{
	/* ISR context */
	struct mvpp2_port_pcpu *port_pcpu =
		container_of(timer, struct mvpp2_port_pcpu, bulk_timer);

	if (!port_pcpu->bulk_timer_scheduled) {
		/* All pending are already flushed by xmit */
		return HRTIMER_NORESTART;
	}
	if (port_pcpu->bulk_timer_restart_req) {
		/* Not flushed but restart requested by xmit */
		port_pcpu->bulk_timer_scheduled = true;
		port_pcpu->bulk_timer_restart_req = false;
		hrtimer_forward_now(timer, MVPP2_TX_BULK_TIME);
		return HRTIMER_RESTART;
	}
	/* Expired and need the flush for pending */
	tasklet_schedule(&port_pcpu->bulk_tasklet);
	return HRTIMER_NORESTART;
}

static void mvpp2_bulk_tasklet_cb(unsigned long data)
{
	struct net_device *dev = (struct net_device *)data;
	struct mvpp2_port *port = netdev_priv(dev);
	struct mvpp2_port_pcpu *port_pcpu;
	struct mvpp2_tx_queue *aggr_txq;
	int frags;
	int sw_thread = mvpp2_check_sw_thread(smp_processor_id());

	port_pcpu = per_cpu_ptr(port->pcpu, sw_thread);

	if (!port_pcpu->bulk_timer_scheduled) {
		/* Flushed by xmit-softirq since timer-irq */
		return;
	}
	port_pcpu->bulk_timer_scheduled = false;
	if (port_pcpu->bulk_timer_restart_req) {
		/* Restart requested by xmit-softirq since timer-irq */
		mvpp2_bulk_timer_start(port_pcpu);
		return;
	}

	/* Full time expired. Flush pending packets here */
	aggr_txq = &port->priv->aggr_txqs[sw_thread];
	frags = aggr_txq->pending;
	if (!frags)
		return; /* Flushed by xmit */
	aggr_txq->pending -= frags;
	mvpp2_aggr_txq_pend_desc_add(port, frags, sw_thread);
}

/* Main RX/TX processing routines */

/* Display more error info */
static void mvpp2_rx_error(struct mvpp2_port *port,
			   struct mvpp2_rx_desc *rx_desc)
{
	u32 status = mvpp2_rxdesc_status_get(port, rx_desc);
	size_t sz = mvpp2_rxdesc_size_get(port, rx_desc);
	char *err_str = NULL;

	switch (status & MVPP2_RXD_ERR_CODE_MASK) {
	case MVPP2_RXD_ERR_CRC:
		err_str = "crc";
		break;
	case MVPP2_RXD_ERR_OVERRUN:
		err_str = "overrun";
		break;
	case MVPP2_RXD_ERR_RESOURCE:
		err_str = "resource";
		break;
	}
	if (err_str)
		pr_err_ratelimited("%s: rx %s error, status=%08x, size=%d\n",
				   port->dev->name, err_str, status, (int)sz);
}

/* Handle RX checksum offload */
static void mvpp2_rx_csum(struct mvpp2_port *port, u32 status,
			  struct sk_buff *skb)
{
	if (((status & MVPP2_RXD_L3_IP4) &&
	     !(status & MVPP2_RXD_IP4_HEADER_ERR)) ||
	    (status & MVPP2_RXD_L3_IP6))
		if (((status & MVPP2_RXD_L4_UDP) ||
		     (status & MVPP2_RXD_L4_TCP)) &&
		     (status & MVPP2_RXD_L4_CSUM_OK)) {
			skb->csum = 0;
			skb->ip_summed = CHECKSUM_UNNECESSARY;
			return;
		}

	skb->ip_summed = CHECKSUM_NONE;
}

/* Allocate a new skb and add it to BM pool */
static int mvpp2_rx_refill(struct mvpp2_port *port,
			   struct mvpp2_bm_pool *bm_pool, int pool)
{
	dma_addr_t dma_addr = mvpp2_buf_alloc(port, bm_pool, GFP_ATOMIC);

	if (!dma_addr)
		return -ENOMEM;

	mvpp2_bm_pool_put(port, pool, dma_addr);

	return 0;
}

/* Handle tx checksum */
static u32 mvpp2_skb_tx_csum(struct mvpp2_port *port, struct sk_buff *skb)
{
	if (skb->ip_summed == CHECKSUM_PARTIAL) {
		int ip_hdr_len = 0;
		u8 l4_proto;

		if (skb->protocol == htons(ETH_P_IP)) {
			struct iphdr *ip4h = ip_hdr(skb);

			/* Calculate IPv4 checksum and L4 checksum */
			ip_hdr_len = ip4h->ihl;
			l4_proto = ip4h->protocol;
		} else if (skb->protocol == htons(ETH_P_IPV6)) {
			struct ipv6hdr *ip6h = ipv6_hdr(skb);

			/* Read l4_protocol from one of IPv6 extra headers */
			if (skb_network_header_len(skb) > 0)
				ip_hdr_len = (skb_network_header_len(skb) >> 2);
			l4_proto = ip6h->nexthdr;
		} else {
			return MVPP2_TXD_L4_CSUM_NOT;
		}

		return mvpp2_txq_desc_csum(skb_network_offset(skb),
				skb->protocol, ip_hdr_len, l4_proto);
	}

	return MVPP2_TXD_L4_CSUM_NOT | MVPP2_TXD_IP_CSUM_DISABLE;
}

/* Global may be called by debugfs */
void mvpp2_recycle_dis_cfg(bool disable)
{
	mvpp2_share.recycle_dis = disable;
}

void mvpp2_recycle_stats(void)
{
	int cpu;
	enum mvpp2_bm_pool_log_num pl_id;
	struct mvpp2_recycle_pcpu *pcpu;

	pr_info("Recycle-stats: %d open ports on %d CP110s\n",
		mvpp2_share.num_open_ports, mvpp2_share.num_cp);
	if (!mvpp2_share.recycle_base)
		return;
	pcpu = mvpp2_share.recycle;
	for (cpu = 0; cpu < used_hifs; cpu++) {
		for (pl_id = 0; pl_id < MVPP2_BM_POOLS_NUM; pl_id++) {
			pr_info("| cpu[%d].pool_%d: idx=%d\n",
				cpu, pl_id, pcpu->idx[pl_id]);
		}
		pr_info("| ___[%d].skb_____idx=%d__\n",
			cpu, pcpu->idx[MVPP2_BM_POOLS_NUM]);
		pcpu++;
	}
}

static int mvpp2_recycle_open(void)
{
	int cpu, pl_id, size;
	struct mvpp2_recycle_pcpu *pcpu;
	phys_addr_t addr;

	mvpp2_share.num_open_ports++;
	wmb(); /* for num_open_ports */

	if (mvpp2_share.recycle_base)
		return 0;

	/* Allocate pool-tree */
	size = sizeof(*pcpu) * used_hifs + L1_CACHE_BYTES;
	mvpp2_share.recycle_base = kzalloc(size, GFP_KERNEL);
	if (!mvpp2_share.recycle_base)
		goto err;
	/* Use Address aligned to L1_CACHE_BYTES */
	addr = (phys_addr_t)mvpp2_share.recycle_base + (L1_CACHE_BYTES - 1);
	addr &= ~(L1_CACHE_BYTES - 1);
	mvpp2_share.recycle = (void *)addr;

	pcpu = mvpp2_share.recycle;
	for (cpu = 0; cpu < used_hifs; cpu++) {
		for (pl_id = 0; pl_id <= MVPP2_BM_POOLS_NUM; pl_id++)
			pcpu->idx[pl_id] = -1;
		pcpu++;
	}
	return 0;
err:
	pr_err("mvpp2 error: cannot allocate recycle pool\n");
	return -ENOMEM;
}

static void mvpp2_recycle_close(void)
{
	int cpu, pl_id, i;
	struct mvpp2_recycle_pcpu *pcpu;
	struct mvpp2_recycle_pool *pool;

	mvpp2_share.num_open_ports--;
	wmb(); /* for num_open_ports */

	/* Do nothing if recycle is not used at all or in use by port/ports */
	if (mvpp2_share.num_open_ports || !mvpp2_share.recycle_base)
		return;

	/* Usable (recycle_base!=NULL), but last port gone down
	 * Let's free all accumulated buffers.
	 */
	pcpu = mvpp2_share.recycle;
	for (cpu = 0; cpu < used_hifs; cpu++) {
		for (pl_id = 0; pl_id <= MVPP2_BM_POOLS_NUM; pl_id++) {
			pool = &pcpu->pool[pl_id];
			for (i = 0; i <= pcpu->idx[pl_id]; i++) {
				if (!pool->pbuf[i])
					continue;
				if (pl_id < MVPP2_BM_POOLS_NUM)
					kfree(pool->pbuf[i]);
				else
					kmem_cache_free(skbuff_head_cache,
							pool->pbuf[i]);
			}
		}
		pcpu++;
	}
	kfree(mvpp2_share.recycle_base);
	mvpp2_share.recycle_base = NULL;
}

static int mvpp2_recycle_get_bm_id(struct sk_buff *skb)
{
	u32 hash;

	/* Keep checking ordering for performance */
	if (!skb)
		return -1;
	hash = skb_get_hash_raw(skb);
	/* Check hash */
	if ((hash & ~MVPP2_RXTX_HASH_BMID_MASK) != MVPP2_RXTX_HASH)
		return -1;
	/* Check if skb could be free */
	if (skb_shared(skb) || skb_cloned(skb))
		return -1;
	/* Get bm-pool-id */
	hash &= ~MVPP2_RXTX_HASH;
	if (hash >= MVPP2_BM_POOLS_NUM)
		return -1;

	return (int)hash;
}

static void mvpp2_recycle_put(struct mvpp2_txq_pcpu *txq_pcpu)
{
	struct mvpp2_recycle_pcpu *pcpu;
	struct mvpp2_recycle_pool *pool;
	short int idx, pool_id;
	struct mvpp2_txq_pcpu_buf *tx_buf =
			txq_pcpu->buffs + txq_pcpu->txq_get_index;
	struct sk_buff *skb = tx_buf->skb;

	pool_id = mvpp2_recycle_get_bm_id(skb);
	if (pool_id < 0)
		return; /* non-recyclable */

	/* This skb could be destroyed. Put into recycle */
	pcpu = mvpp2_share.recycle + txq_pcpu->cpu;
	idx = pcpu->idx[pool_id];
	if (idx < MVPP2_RECYCLE_FULL) {
		pool = &pcpu->pool[pool_id];
		pool->pbuf[++idx] = skb->head; /* pre-increment */
		pcpu->idx[pool_id] = idx;
		skb->head = NULL;
	}
	idx = pcpu->idx[MVPP2_BM_POOLS_NUM];
	if (idx < MVPP2_RECYCLE_FULL_SKB) {
		pool = &pcpu->pool[MVPP2_BM_POOLS_NUM];
		pool->pbuf[++idx] = skb;
		pcpu->idx[MVPP2_BM_POOLS_NUM] = idx;
		tx_buf->skb = NULL;
	}
}

static struct sk_buff *mvpp2_recycle_get(struct mvpp2_port *port,
					 int *refill_needed,
					 struct mvpp2_bm_pool *bm_pool)
{
	int sw_thread;
	struct mvpp2_recycle_pcpu *pcpu;
	struct mvpp2_recycle_pool *pool;
	short int idx;
	void *frag;
	struct sk_buff *skb;
	dma_addr_t dma_addr;

	if (unlikely(mvpp2_share.recycle_dis))
		goto end;

	sw_thread = mvpp2_check_sw_thread(smp_processor_id());
	pcpu = mvpp2_share.recycle + sw_thread;

	/* GET bm buffer */
	idx = pcpu->idx[bm_pool->id];
	pool = &pcpu->pool[bm_pool->id];

	if (idx >= 0) {
		frag = pool->pbuf[idx];
		pcpu->idx[bm_pool->id]--; /* post-decrement */
	} else {
		/* Allocate 2 buffers, put 1, use another now */
		pcpu->idx[bm_pool->id] = 0;
		pool->pbuf[0] = mvpp2_frag_alloc(bm_pool);
		frag = NULL;
	}
	if (!frag)
		frag = mvpp2_frag_alloc(bm_pool);

	/* refill the buffer into BM */
	dma_addr = dma_map_single(port->dev->dev.parent, frag,
				  MVPP2_RX_BUF_SIZE(bm_pool->pkt_size),
				  DMA_FROM_DEVICE);
	if (unlikely(dma_mapping_error(port->dev->dev.parent, dma_addr))) {
		pcpu->idx[bm_pool->id]++; /* Return back to recycle */
		netdev_err(port->dev, "failed to refill BM pool-%d (%d:%p)\n",
			   bm_pool->id, pcpu->idx[bm_pool->id], frag);
	} else {
		mvpp2_bm_pool_put(port, bm_pool->id, dma_addr);
		*refill_needed = 0;
	}

	/* GET skb buffer */
	idx = pcpu->idx[MVPP2_BM_POOLS_NUM];
	if (idx >= 0) {
		pool = &pcpu->pool[MVPP2_BM_POOLS_NUM];
		skb = pool->pbuf[idx];
		pcpu->idx[MVPP2_BM_POOLS_NUM]--;
		return skb;
	}
end:
	return kmem_cache_alloc(skbuff_head_cache, GFP_ATOMIC);
}

static inline void mvpp2_skb_set_extra(struct sk_buff *skb,
				       struct napi_struct *napi,
				       u32 status,
				       u8 rxq_id,
				       struct mvpp2_bm_pool *bm_pool)
{
	u32 hash;
	enum pkt_hash_types hash_type;

	/* Improve performance and set identification for RX-TX fast-forward */
	hash = MVPP2_RXTX_HASH | bm_pool->id;
	hash_type = (status & (MVPP2_RXD_L4_UDP | MVPP2_RXD_L4_TCP)) ?
		PKT_HASH_TYPE_L4 : PKT_HASH_TYPE_L3;
	skb_set_hash(skb, hash, hash_type);
	skb_mark_napi_id(skb, napi);
	skb_record_rx_queue(skb, (u16)rxq_id);
}

/* This is "fast inline" clone of __build_skb+build_skb,
 * and also with setting mv-extra information
 */
static inline
struct sk_buff *mvpp2_build_skb(void *data, unsigned int frag_size,
				struct napi_struct *napi,
				struct mvpp2_port *port,
				u32 rx_status,
				u8 rxq_id,
				struct mvpp2_bm_pool *bm_pool,
				int *refill_needed)
{
	struct skb_shared_info *shinfo;
	struct sk_buff *skb;
	unsigned int size = frag_size ? : ksize(data);

	skb = mvpp2_recycle_get(port, refill_needed, bm_pool);
	if (!skb)
		return NULL;

	size -= SKB_DATA_ALIGN(sizeof(struct skb_shared_info));

	memset(skb, 0, offsetof(struct sk_buff, tail));
	skb->truesize = SKB_TRUESIZE(size);
	refcount_set(&skb->users, 1);
	skb->head = data;
	skb->data = data;
	skb_reset_tail_pointer(skb);
	skb->end = skb->tail + size;
	skb->mac_header = (typeof(skb->mac_header))~0U;
	skb->transport_header = (typeof(skb->transport_header))~0U;

	/* make sure we initialize shinfo sequentially */
	shinfo = skb_shinfo(skb);
	memset(shinfo, 0, offsetof(struct skb_shared_info, dataref));
	atomic_set(&shinfo->dataref, 1);

	/* From build_skb wrapper */
	if (frag_size) {
		skb->head_frag = 1;
		if (page_is_pfmemalloc(virt_to_head_page(data)))
			skb->pfmemalloc = 1;
	}

	mvpp2_skb_set_extra(skb, napi, rx_status, rxq_id, bm_pool);

	return skb;
}

/* Main rx processing */
static int mvpp2_rx(struct mvpp2_port *port, struct napi_struct *napi,
		    int rx_todo, struct mvpp2_rx_queue *rxq)
{
	struct net_device *dev = port->dev;
	int rx_received;
	int rx_done = 0;
	u32 rcvd_pkts = 0;
	u32 rcvd_bytes = 0;

	/* Get number of received packets and clamp the to-do */
	rx_received = mvpp2_rxq_received(port, rxq->id);
	if (rx_todo > rx_received)
		rx_todo = rx_received;

	while (rx_done < rx_todo) {
		struct mvpp2_rx_desc *rx_desc = mvpp2_rxq_next_desc_get(rxq);
		struct mvpp2_bm_pool *bm_pool;
		struct sk_buff *skb;
		unsigned int frag_size;
		dma_addr_t dma_addr;
		phys_addr_t phys_addr;
		u32 rx_status;
		int pool, rx_bytes, err;
		void *data;

		rx_done++;
		mvpp2_rx_desc_endian(port->priv->hw_version, rx_desc);
		rx_status = mvpp2_rxdesc_status_get(port, rx_desc);
		rx_bytes = mvpp2_rxdesc_size_get(port, rx_desc);
		rx_bytes -= MVPP2_MH_SIZE;
		dma_addr = mvpp2_rxdesc_dma_addr_get(port, rx_desc);
		phys_addr = dma_to_phys(port->dev->dev.parent, dma_addr);
		data = (void *)phys_to_virt(phys_addr);

		pool = (rx_status & MVPP2_RXD_BM_POOL_ID_MASK) >>
			MVPP2_RXD_BM_POOL_ID_OFFS;
		bm_pool = &port->priv->bm_pools[pool];

		/* In case of an error, release the requested buffer pointer
		 * to the Buffer Manager. This request process is controlled
		 * by the hardware, and the information about the buffer is
		 * comprised by the RX descriptor.
		 */
		if (rx_status & MVPP2_RXD_ERR_SUMMARY) {
err_drop_frame:
			dev->stats.rx_errors++;
			mvpp2_rx_error(port, rx_desc);
			/* Return the buffer to the pool */
			mvpp2_bm_pool_put(port, pool, dma_addr);
			continue;
		}

		if (bm_pool->frag_size > PAGE_SIZE)
			frag_size = 0;
		else
			frag_size = bm_pool->frag_size;

		prefetch(data + NET_SKB_PAD); /* packet header */

		dma_unmap_single(dev->dev.parent, dma_addr,
				 bm_pool->buf_size, DMA_FROM_DEVICE);

		skb = mvpp2_build_skb(data, frag_size,
				      napi, port, rx_status, rxq->id,
				      bm_pool, &rx_received);
		if (!skb) {
			netdev_warn(port->dev, "skb build failed\n");
			goto err_drop_frame;
		}

		if (!rx_received)
			goto refill_done; /* done by build-skb */

		err = mvpp2_rx_refill(port, bm_pool, pool);
		if (err) {
			netdev_err(port->dev, "failed to refill BM pools\n");
			goto err_drop_frame;
		}
refill_done:
		rcvd_pkts++;
		rcvd_bytes += rx_bytes;

		skb_reserve(skb, MVPP2_MH_SIZE + NET_SKB_PAD);
		skb_put(skb, rx_bytes);
		skb->protocol = eth_type_trans(skb, dev);
		mvpp2_rx_csum(port, rx_status, skb);

		napi_gro_receive(napi, skb);
	}

	if (rcvd_pkts) {
		struct mvpp2_pcpu_stats *stats = this_cpu_ptr(port->stats);

		u64_stats_update_begin(&stats->syncp);
		stats->rx_packets += rcvd_pkts;
		stats->rx_bytes   += rcvd_bytes;
		u64_stats_update_end(&stats->syncp);
	}

	/* Update HW Rx queue management counters with RX-done */
	mvpp2_rxq_status_update(port, rxq->id, rx_done, rx_done);

	return rx_todo;
}

static inline void
tx_desc_unmap_put(struct mvpp2_port *port, struct mvpp2_tx_queue *txq,
		  struct mvpp2_tx_desc *desc, int cpu)
{
	struct mvpp2_txq_pcpu *txq_pcpu = per_cpu_ptr(txq->pcpu, cpu);

	dma_addr_t buf_dma_addr =
		mvpp2_txdesc_dma_addr_get(port, desc);
	size_t buf_sz =
		mvpp2_txdesc_size_get(port, desc);
	if (!IS_TSO_HEADER(txq_pcpu, buf_dma_addr))
		dma_unmap_single(port->dev->dev.parent, buf_dma_addr,
				 buf_sz, DMA_TO_DEVICE);
	mvpp2_txq_desc_put(txq);
}

/* Handle tx fragmentation processing */
static int mvpp2_tx_frag_process(struct mvpp2_port *port, struct sk_buff *skb,
				 struct mvpp2_tx_queue *aggr_txq,
				 struct mvpp2_tx_queue *txq)
{
	struct mvpp2_txq_pcpu *txq_pcpu = per_cpu_ptr(txq->pcpu, aggr_txq->id);
	struct mvpp2_tx_desc *tx_desc;
	int i;
	dma_addr_t buf_dma_addr;

	for (i = 0; i < skb_shinfo(skb)->nr_frags; i++) {
		skb_frag_t *frag = &skb_shinfo(skb)->frags[i];
		void *addr = page_address(frag->page.p) + frag->page_offset;

		tx_desc = mvpp2_txq_next_desc_get(aggr_txq);
		mvpp2_txdesc_txq_set(port, tx_desc, txq->id);
		mvpp2_txdesc_size_set(port, tx_desc, frag->size);

		buf_dma_addr = dma_map_single(port->dev->dev.parent, addr,
					      frag->size,
					      DMA_TO_DEVICE);
		if (dma_mapping_error(port->dev->dev.parent, buf_dma_addr)) {
			mvpp2_txq_desc_put(txq);
			goto cleanup;
		}

		mvpp2_txdesc_dma_addr_set(port, tx_desc, buf_dma_addr);

		if (i == (skb_shinfo(skb)->nr_frags - 1)) {
			/* Last descriptor */
			mvpp2_txdesc_cmd_set(port, tx_desc,
					     MVPP2_TXD_L_DESC);
			mvpp2_txq_inc_put(port, txq_pcpu, skb, tx_desc);
		} else {
			/* Descriptor in the middle: Not First, Not Last */
			mvpp2_txdesc_cmd_set(port, tx_desc, 0);
			mvpp2_txq_inc_put(port, txq_pcpu, NULL, tx_desc);
		}
	}

	return 0;
cleanup:
	/* Release all descriptors that were used to map fragments of
	 * this packet, as well as the corresponding DMA mappings
	 */
	for (i = i - 1; i >= 0; i--) {
		tx_desc = txq->descs + i;
		tx_desc_unmap_put(port, txq, tx_desc, aggr_txq->id);
	}

	return -ENOMEM;
}

static inline void mvpp2_tso_put_hdr(struct sk_buff *skb,
				     struct net_device *dev,
				     struct mvpp2_tx_queue *txq,
				     struct mvpp2_tx_queue *aggr_txq,
				     struct mvpp2_txq_pcpu *txq_pcpu,
				     int hdr_sz)
{
	struct mvpp2_port *port = netdev_priv(dev);
	struct mvpp2_tx_desc *tx_desc = mvpp2_txq_next_desc_get(aggr_txq);
	dma_addr_t addr;

	mvpp2_txdesc_txq_set(port, tx_desc, txq->id);
	mvpp2_txdesc_size_set(port, tx_desc, hdr_sz);

	addr = txq_pcpu->tso_headers_dma +
	       txq_pcpu->txq_put_index * TSO_HEADER_SIZE;
	mvpp2_txdesc_dma_addr_set(port, tx_desc, addr);

	mvpp2_txdesc_cmd_set(port, tx_desc, mvpp2_skb_tx_csum(port, skb) |
					    MVPP2_TXD_F_DESC |
					    MVPP2_TXD_PADDING_DISABLE);
	mvpp2_txq_inc_put(port, txq_pcpu, NULL, tx_desc);
}

static inline int mvpp2_tso_put_data(struct sk_buff *skb,
				     struct net_device *dev, struct tso_t *tso,
				     struct mvpp2_tx_queue *txq,
				     struct mvpp2_tx_queue *aggr_txq,
				     struct mvpp2_txq_pcpu *txq_pcpu,
				     int sz, bool left, bool last)
{
	struct mvpp2_port *port = netdev_priv(dev);
	struct mvpp2_tx_desc *tx_desc = mvpp2_txq_next_desc_get(aggr_txq);
	dma_addr_t buf_dma_addr;

	mvpp2_txdesc_txq_set(port, tx_desc, txq->id);
	mvpp2_txdesc_size_set(port, tx_desc, sz);

	buf_dma_addr = dma_map_single(dev->dev.parent, tso->data, sz,
				      DMA_TO_DEVICE);
	if (unlikely(dma_mapping_error(dev->dev.parent, buf_dma_addr))) {
		mvpp2_txq_desc_put(txq);
		return -ENOMEM;
	}

	mvpp2_txdesc_dma_addr_set(port, tx_desc, buf_dma_addr);

	if (!left) {
		mvpp2_txdesc_cmd_set(port, tx_desc, MVPP2_TXD_L_DESC);
		if (last) {
			mvpp2_txq_inc_put(port, txq_pcpu, skb, tx_desc);
			return 0;
		}
	} else {
		mvpp2_txdesc_cmd_set(port, tx_desc, 0);
	}

	mvpp2_txq_inc_put(port, txq_pcpu, NULL, tx_desc);
	return 0;
}

static int mvpp2_tx_tso(struct sk_buff *skb, struct net_device *dev,
			struct mvpp2_tx_queue *txq,
			struct mvpp2_tx_queue *aggr_txq,
			struct mvpp2_txq_pcpu *txq_pcpu)
{
	struct mvpp2_port *port = netdev_priv(dev);
	struct tso_t tso;
	int hdr_sz = skb_transport_offset(skb) + tcp_hdrlen(skb);
	int i, len, descs = 0;

	/* Check number of available descriptors */
	if (mvpp2_aggr_desc_num_check(port->priv, aggr_txq,
				      tso_count_descs(skb)) ||
	    mvpp2_txq_reserved_desc_num_proc(port->priv, txq, txq_pcpu,
					     tso_count_descs(skb)))
		return 0;

	tso_start(skb, &tso);
	len = skb->len - hdr_sz;
	while (len > 0) {
		int left = min_t(int, skb_shinfo(skb)->gso_size, len);
		char *hdr = txq_pcpu->tso_headers +
			    txq_pcpu->txq_put_index * TSO_HEADER_SIZE;

		len -= left;
		descs++;

		tso_build_hdr(skb, hdr, &tso, left, len == 0);
		mvpp2_tso_put_hdr(skb, dev, txq, aggr_txq, txq_pcpu, hdr_sz);

		while (left > 0) {
			int sz = min_t(int, tso.size, left);

			left -= sz;
			descs++;

			if (mvpp2_tso_put_data(skb, dev, &tso, txq, aggr_txq,
					       txq_pcpu, sz, left, len == 0))
				goto release;
			tso_build_data(skb, &tso, sz);
		}
	}

	return descs;

release:
	for (i = descs - 1; i >= 0; i--) {
		struct mvpp2_tx_desc *tx_desc = txq->descs + i;

		tx_desc_unmap_put(port, txq, tx_desc, aggr_txq->id);
	}
	return 0;
}

/* Main tx processing */
static int mvpp2_tx(struct sk_buff *skb, struct net_device *dev)
{
	int sw_thread = mvpp2_check_sw_thread(smp_processor_id());
	struct mvpp2_port *port = netdev_priv(dev);
	struct mvpp2_tx_queue *txq, *aggr_txq;
	struct mvpp2_txq_pcpu *txq_pcpu;
	struct mvpp2_tx_desc *tx_desc;
	dma_addr_t buf_dma_addr;
	unsigned long flags = 0;
	int frags = 0;
	u16 txq_id;
	u32 tx_cmd;

	txq_id = skb_get_queue_mapping(skb);
	txq = port->txqs[txq_id];
	txq_pcpu = per_cpu_ptr(txq->pcpu, sw_thread);
	aggr_txq = &port->priv->aggr_txqs[sw_thread];

	if (port->priv->spinlocks_bitmap & MV_AGGR_QUEUE_LOCK)
		spin_lock_irqsave(&aggr_txq->spinlock, flags);

	if (skb_is_gso(skb)) {
		frags = mvpp2_tx_tso(skb, dev, txq, aggr_txq, txq_pcpu);
		goto out;
	}
	frags = skb_shinfo(skb)->nr_frags + 1;

	/* Check number of available descriptors */
	if (mvpp2_aggr_desc_num_check(port->priv, aggr_txq, frags) ||
	    mvpp2_txq_reserved_desc_num_proc(port->priv, txq,
					     txq_pcpu, frags)) {
		frags = 0;
		goto out;
	}

	/* Get a descriptor for the first part of the packet */
	tx_desc = mvpp2_txq_next_desc_get(aggr_txq);
	mvpp2_txdesc_txq_set(port, tx_desc, txq->id);
	mvpp2_txdesc_size_set(port, tx_desc, skb_headlen(skb));

	buf_dma_addr = dma_map_single(dev->dev.parent, skb->data,
				      skb_headlen(skb), DMA_TO_DEVICE);
	if (unlikely(dma_mapping_error(dev->dev.parent, buf_dma_addr))) {
		mvpp2_txq_desc_put(txq);
		frags = 0;
		goto out;
	}

	mvpp2_txdesc_dma_addr_set(port, tx_desc, buf_dma_addr);

	tx_cmd = mvpp2_skb_tx_csum(port, skb);

	if (frags == 1) {
		/* First and Last descriptor */
		tx_cmd |= MVPP2_TXD_F_DESC | MVPP2_TXD_L_DESC;
		mvpp2_txdesc_cmd_set(port, tx_desc, tx_cmd);
		mvpp2_txq_inc_put(port, txq_pcpu, skb, tx_desc);
	} else {
		/* First but not Last */
		tx_cmd |= MVPP2_TXD_F_DESC | MVPP2_TXD_PADDING_DISABLE;
		mvpp2_txdesc_cmd_set(port, tx_desc, tx_cmd);
		mvpp2_txq_inc_put(port, txq_pcpu, NULL, tx_desc);

		/* Continue with other skb fragments */
		if (mvpp2_tx_frag_process(port, skb, aggr_txq, txq)) {
			tx_desc_unmap_put(port, txq, tx_desc, aggr_txq->id);
			frags = 0;
		}
	}

out:
	if (frags > 0) {
		struct mvpp2_pcpu_stats *stats = this_cpu_ptr(port->stats);
		struct netdev_queue *nq = netdev_get_tx_queue(dev, txq_id);
		bool deferred_tx;
		struct mvpp2_port_pcpu *port_pcpu;

		txq_pcpu->reserved_num -= frags;
		txq_pcpu->count += frags;
		aggr_txq->count += frags;

		/* Enable transmit; may be deferred with Bulk-timer */
		port_pcpu = per_cpu_ptr(port->pcpu, sw_thread);
		deferred_tx = (frags == 1) &&
			(aggr_txq->pending < (txq->done_pkts_coal / 2));

		if (deferred_tx) {
			aggr_txq->pending += frags;
			mvpp2_bulk_timer_restart(port_pcpu);
		} else {
			port_pcpu->bulk_timer_scheduled = false;
			port_pcpu->bulk_timer_restart_req = false;
			frags += aggr_txq->pending;
			aggr_txq->pending = 0;
			mvpp2_aggr_txq_pend_desc_add(port, frags, sw_thread);
		}

		if (txq_pcpu->count >= txq_pcpu->stop_threshold)
			netif_tx_stop_queue(nq);

		u64_stats_update_begin(&stats->syncp);
		stats->tx_packets++;
		stats->tx_bytes += skb->len;
		u64_stats_update_end(&stats->syncp);
	} else {
		dev->stats.tx_dropped++;
		dev_kfree_skb_any(skb);
	}

	/* Finalize TX processing */
	if (!port->has_tx_irqs && txq_pcpu->count >= txq->done_pkts_coal)
		mvpp2_txq_done(port, txq, txq_pcpu);

	/* Set the timer in case not all frags were processed */
	if (!port->has_tx_irqs && txq_pcpu->count <= frags &&
	    txq_pcpu->count > 0) {
		struct mvpp2_port_pcpu *port_pcpu = per_cpu_ptr(port->pcpu,
								sw_thread);

		mvpp2_timer_set(port_pcpu);
	}

	if (port->priv->spinlocks_bitmap & MV_AGGR_QUEUE_LOCK)
		spin_unlock_irqrestore(&aggr_txq->spinlock, flags);

	return NETDEV_TX_OK;
}

static inline void mvpp2_cause_error(struct net_device *dev, int cause)
{
	if (cause & MVPP2_CAUSE_FCS_ERR_MASK)
		netdev_err(dev, "FCS error\n");
	if (cause & MVPP2_CAUSE_RX_FIFO_OVERRUN_MASK)
		netdev_err(dev, "rx fifo overrun error\n");
	if (cause & MVPP2_CAUSE_TX_FIFO_UNDERRUN_MASK)
		netdev_err(dev, "tx fifo underrun error\n");
}

static int mvpp21_poll(struct napi_struct *napi, int budget)
{
	u32 cause, cause_rx, cause_misc;
	int rx_done = 0;
	struct mvpp2_port *port = netdev_priv(napi->dev);
	struct mvpp2_queue_vector *qv;
	int cpu = smp_processor_id();

	qv = container_of(napi, struct mvpp2_queue_vector, napi);

	/* Bits 0-15: each bit indicates received packets on the Rx queue
	 * (bit 0 is for Rx queue 0).
	 *
	 * Each CPU has its own Rx cause register
	 */
	cause = mvpp2_percpu_read_relaxed(port->priv, qv->sw_thread_id,
					  MVPP2_ISR_RX_TX_CAUSE_REG(port->id));

	cause_misc = cause & MVPP2_CAUSE_MISC_SUM_MASK;
	if (cause_misc) {
		mvpp2_cause_error(port->dev, cause_misc);

		/* Clear the cause register */
		mvpp2_write(port->priv, MVPP2_ISR_MISC_CAUSE_REG, 0);
		mvpp2_percpu_write(port->priv, cpu,
				   MVPP2_ISR_RX_TX_CAUSE_REG(port->id),
				   cause & ~MVPP2_CAUSE_MISC_SUM_MASK);
	}

	/* Process RX packets */
	cause_rx = cause & MVPP21_CAUSE_RXQ_OCCUP_DESC_ALL_MASK;
	cause_rx <<= qv->first_rxq;
	cause_rx |= qv->pending_cause_rx;
	while (cause_rx && budget > 0) {
		int count;
		struct mvpp2_rx_queue *rxq;

		rxq = mvpp2_get_rx_queue(port, cause_rx);
		if (!rxq)
			break;

		count = mvpp2_rx(port, napi, budget, rxq);
		rx_done += count;
		budget -= count;
		if (budget > 0) {
			/* Clear the bit associated to this Rx queue
			 * so that next iteration will continue from
			 * the next Rx queue.
			 */
			cause_rx &= ~(1 << rxq->logic_rxq);
		}
	}

	if (budget > 0) {
		cause_rx = 0;
		napi_complete_done(napi, rx_done);

		mvpp2_qvec_interrupt_enable(qv);
	}
	qv->pending_cause_rx = cause_rx;
	return rx_done;
}

static int mvpp22_poll(struct napi_struct *napi, int budget)
{
	u32 cause_rx_tx, cause_rx, cause_tx, cause_misc;
	int rx_done = 0;
	int cpu = mvpp2_check_sw_thread(smp_processor_id());
	struct mvpp2_port *port = netdev_priv(napi->dev);
	struct mvpp2_queue_vector *qv;

	qv = container_of(napi, struct mvpp2_queue_vector, napi);

	/* Rx/Tx cause register
	 *
	 * Bits 0-7: each bit indicates received packets on the Rx queue
	 * (bit 0 is for Rx queue 0).
	 *
	 * Bits 16-23: each bit indicates transmitted packets on the Tx queue
	 * (bit 16 is for Tx queue 0).
	 *
	 * Each CPU has its own Rx/Tx cause register
	 */
	cause_rx_tx = mvpp2_percpu_read_relaxed(port->priv, qv->sw_thread_id,
						MVPP2_ISR_RX_TX_CAUSE_REG(port->id));

	cause_misc = cause_rx_tx & MVPP2_CAUSE_MISC_SUM_MASK;
	if (cause_misc) {
		mvpp2_cause_error(port->dev, cause_misc);

		/* Clear the cause register */
		mvpp2_write(port->priv, MVPP2_ISR_MISC_CAUSE_REG, 0);
		mvpp2_percpu_write(port->priv, cpu,
				   MVPP2_ISR_RX_TX_CAUSE_REG(port->id),
				   cause_rx_tx & ~MVPP2_CAUSE_MISC_SUM_MASK);
	}

	cause_tx = cause_rx_tx & MVPP2_CAUSE_TXQ_OCCUP_DESC_ALL_MASK;
	if (cause_tx) {
		cause_tx >>= MVPP2_CAUSE_TXQ_OCCUP_DESC_ALL_OFFSET;
		mvpp2_tx_done(port, cause_tx, qv->sw_thread_id);
	}

	/* Process RX packets */
	cause_rx = cause_rx_tx & MVPP22_CAUSE_RXQ_OCCUP_DESC_ALL_MASK;
	cause_rx <<= qv->first_rxq;
	cause_rx |= qv->pending_cause_rx;
	while (cause_rx && budget > 0) {
		int count;
		struct mvpp2_rx_queue *rxq;

		rxq = mvpp2_get_rx_queue(port, cause_rx);
		if (!rxq)
			break;

		count = mvpp2_rx(port, napi, budget, rxq);
		rx_done += count;
		budget -= count;
		if (budget > 0) {
			/* Clear the bit associated to this Rx queue
			 * so that next iteration will continue from
			 * the next Rx queue.
			 */
			cause_rx &= ~(1 << rxq->logic_rxq);
		}
	}

	if (budget > 0) {
		cause_rx = 0;
		napi_complete_done(napi, rx_done);

		mvpp2_qvec_interrupt_enable(qv);
	}
	qv->pending_cause_rx = cause_rx;
	return rx_done;
}

static void mvpp22_mode_reconfigure(struct mvpp2_port *port)
{
	u32 ctrl3;

	/* comphy reconfiguration */
	mvpp22_comphy_init(port);

	/* gop reconfiguration */
	mvpp22_gop_init(port);

	/* Only GOP port 0 has an XLG MAC */
	if (port->gop_id == 0) {
		ctrl3 = readl(port->base + MVPP22_XLG_CTRL3_REG);
		ctrl3 &= ~MVPP22_XLG_CTRL3_MACMODESELECT_MASK;

		if (port->phy_interface == PHY_INTERFACE_MODE_XAUI ||
		    port->phy_interface == PHY_INTERFACE_MODE_10GKR)
			ctrl3 |= MVPP22_XLG_CTRL3_MACMODESELECT_10G;
		else
			ctrl3 |= MVPP22_XLG_CTRL3_MACMODESELECT_GMAC;

		writel(ctrl3, port->base + MVPP22_XLG_CTRL3_REG);
	}

	if (port->gop_id == 0 &&
	    (port->phy_interface == PHY_INTERFACE_MODE_XAUI ||
	     port->phy_interface == PHY_INTERFACE_MODE_10GKR))
		mvpp2_xlg_max_rx_size_set(port);
	else
		mvpp2_gmac_max_rx_size_set(port);
}

/* Set hw internals when starting port */
static void mvpp2_start_dev(struct mvpp2_port *port)
{
	int i;

	mvpp2_txp_max_tx_size_set(port);

	/* stop_dev() sets Coal to ZERO. Care to restore it now */
	if (port->has_tx_irqs)
		mvpp2_tx_pkts_coal_set(port);

	for (i = 0; i < port->nqvecs; i++)
		napi_enable(&port->qvecs[i].napi);

	/* Enable interrupts on all CPUs */
	mvpp2_interrupts_enable(port);

	if (port->priv->hw_version != MVPP21)
		mvpp22_mode_reconfigure(port);

	if (port->phylink) {
		phylink_start(port->phylink);
	} else {
		/* Phylink isn't used as of now for ACPI, so the MAC has to be
		 * configured manually when the interface is started. This will
		 * be removed as soon as the phylink ACPI support lands in.
		 */
		struct phylink_link_state state = {
			.interface = port->phy_interface,
			.link = 1,
		};
		mvpp2_mac_config(port->dev, MLO_AN_INBAND, &state);
	}

	mvpp2_tx_start_all_queues(port->dev);
}

/* Set hw internals when stopping port */
static void mvpp2_stop_dev(struct mvpp2_port *port)
{
	int i;

	/* Under active traffic the BM/RX and TX PP2-HW could be non-empty.
	 * Stop asap new packets ariving from both RX and TX directions,
	 * but do NOT disable egress free/send-out and interrupts tx-done,
	 * yeild the context for gracefull finishing (msleep, not mdelay).
	 * This sequence especially important for scenarious with further
	 * queue-cleanup -- ifconfig-down and ethtool-ring-size
	 * Flush all tx-done by forcing pkts-coal to ZERO
	 */
	mvpp2_tx_stop_all_queues(port->dev);
	mvpp2_ingress_disable(port);
	if (port->has_tx_irqs)
		on_each_cpu(mvpp2_tx_pkts_coal_set_zero_pcpu, port, 1);

	msleep(40);

	mvpp2_egress_disable(port);

	/* Disable interrupts on all CPUs */
	mvpp2_interrupts_disable(port);

	for (i = 0; i < port->nqvecs; i++)
		napi_disable(&port->qvecs[i].napi);

	if (port->phylink)
		phylink_stop(port->phylink);
	phy_power_off(port->comphy);
}

static int mvpp2_check_ringparam_valid(struct net_device *dev,
				       struct ethtool_ringparam *ring)
{
	u16 new_rx_pending = ring->rx_pending;
	u16 new_tx_pending = ring->tx_pending;

	if (ring->rx_pending == 0 || ring->tx_pending == 0)
		return -EINVAL;

	if (ring->rx_pending > MVPP2_MAX_RXD_MAX)
		new_rx_pending = MVPP2_MAX_RXD_MAX;
	else if (!IS_ALIGNED(ring->rx_pending, 16))
		new_rx_pending = ALIGN(ring->rx_pending, 16);

	if (ring->tx_pending > MVPP2_MAX_TXD_MAX)
		new_tx_pending = MVPP2_MAX_TXD_MAX;
	else if (!IS_ALIGNED(ring->tx_pending, 32))
		new_tx_pending = ALIGN(ring->tx_pending, 32);

	if (new_tx_pending < MVPP2_MIN_TXD(used_hifs))
		new_tx_pending = MVPP2_MIN_TXD(used_hifs);

	if (ring->rx_pending != new_rx_pending) {
		netdev_info(dev, "illegal Rx ring size value %d, round to %d\n",
			    ring->rx_pending, new_rx_pending);
		ring->rx_pending = new_rx_pending;
	}

	if (ring->tx_pending != new_tx_pending) {
		netdev_info(dev, "illegal Tx ring size value %d, round to %d\n",
			    ring->tx_pending, new_tx_pending);
		ring->tx_pending = new_tx_pending;
	}

	return 0;
}

static void mvpp21_get_mac_address(struct mvpp2_port *port, unsigned char *addr)
{
	u32 mac_addr_l, mac_addr_m, mac_addr_h;

	mac_addr_l = readl(port->base + MVPP2_GMAC_CTRL_1_REG);
	mac_addr_m = readl(port->priv->lms_base + MVPP2_SRC_ADDR_MIDDLE);
	mac_addr_h = readl(port->priv->lms_base + MVPP2_SRC_ADDR_HIGH);
	addr[0] = (mac_addr_h >> 24) & 0xFF;
	addr[1] = (mac_addr_h >> 16) & 0xFF;
	addr[2] = (mac_addr_h >> 8) & 0xFF;
	addr[3] = mac_addr_h & 0xFF;
	addr[4] = mac_addr_m & 0xFF;
	addr[5] = (mac_addr_l >> MVPP2_GMAC_SA_LOW_OFFS) & 0xFF;
}

static int mvpp2_irqs_init(struct mvpp2_port *port)
{
	int err, i;

	for (i = 0; i < port->nqvecs; i++) {
		struct mvpp2_queue_vector *qv = port->qvecs + i;

		if (qv->type == MVPP2_QUEUE_VECTOR_PRIVATE)
			irq_set_status_flags(qv->irq, IRQ_NO_BALANCING);

		err = request_irq(qv->irq, mvpp2_isr, 0, port->dev->name, qv);
		if (err)
			goto err;

		if (qv->type == MVPP2_QUEUE_VECTOR_PRIVATE)
			irq_set_affinity_hint(qv->irq,
					      cpumask_of(qv->sw_thread_id));
	}

	return 0;
err:
	for (i = 0; i < port->nqvecs; i++) {
		struct mvpp2_queue_vector *qv = port->qvecs + i;

		irq_set_affinity_hint(qv->irq, NULL);
		free_irq(qv->irq, qv);
	}

	return err;
}

static void mvpp2_irqs_deinit(struct mvpp2_port *port)
{
	int i;

	for (i = 0; i < port->nqvecs; i++) {
		struct mvpp2_queue_vector *qv = port->qvecs + i;

		irq_set_affinity_hint(qv->irq, NULL);
		irq_clear_status_flags(qv->irq, IRQ_NO_BALANCING);
		free_irq(qv->irq, qv);
	}
}

static bool mvpp22_rss_is_supported(struct mvpp2_port *port)
{
	/* RSS Feature is supported only for
	 *  static configuration Queue-Multi-Mode and
	 *  dynamic regular Kernel-mode port configuration
	 */
	return (queue_mode == MVPP2_QDIST_MULTI_MODE) &&
		!(port->flags & MVPP2_F_LOOPBACK) &&
		!(port->flags & MVPP22_F_IF_MUSDK);
}

/* Allocate a rss table for each phisical rxq having same cos priority */
static int mvpp22_rss_rxq_set(struct mvpp2_port *port)
{
	u32 cpu_width, cos_width, cos_mask;
	unsigned int reg_val;
	int rxq, rxq_idx, tbl_ptr, err;

	/* Calculate width */
	err = mvpp2_width_calc(port, &cpu_width, &cos_width);
	if (err)
		return err;
	cos_mask = ((1 << cos_width) - 1);

	for (rxq = 0; rxq < port->nrxqs; rxq++) {
		rxq_idx = port->rxqs[rxq]->id;
		tbl_ptr = rxq_idx & cos_mask;

		/* rss_tbl_entry_set - access by Pointer */
		/* Write index */
		reg_val = rxq_idx << MVPP22_RSS_IDX_RXQ_NUM_OFF;
		mvpp2_write(port->priv, MVPP22_RSS_IDX_REG, reg_val);
		/* Write entry */
		reg_val &= (~MVPP22_RSS_RXQ2RSS_TBL_POINT_MASK);
		reg_val |= tbl_ptr << MVPP22_RSS_RXQ2RSS_TBL_POINT_OFF;
		mvpp2_write(port->priv, MVPP22_RSS_RXQ2RSS_TBL_REG, reg_val);
	}

	return 0;
}

static inline u32 mvpp22_rxfh_indir(struct mvpp2_port *port, u32 rxq)
{
	int nrxqs, cpus = used_hifs;

	/* Number of RXQs per CPU */
	nrxqs = port->nrxqs / cpus;

	/* Indirection to better distribute the paquets on the CPUs when
	 * configuring the RSS queues.
	 */
	return (rxq * nrxqs + rxq / cpus) % port->nrxqs;
}

static void mvpp22_rss_rxfh_indir_init(struct mvpp2 *priv)
{
	int i;

	for (i = 0; i < MVPP22_RSS_TABLE_ENTRIES; i++)
		priv->indir[i] = i % used_hifs;
}

/* Translate CPU sequence number to real CPU ID */
static int mvpp22_cpu_id_from_indir_get(struct mvpp2_port *port, int entry)
{
	u32 i, seq = 0;
	u32 cpu_seq = port->priv->indir[entry];

	for (i = 0; i < used_hifs; i++) {
		if ((*cpumask_bits(cpu_online_mask)) & (1 << i)) {
			if (i == cpu_seq)
				return i;
			seq++;
		}
	}
	return -1;
}

/*  Set the RSS table according to CPU weight from ethtool */
static int mvpp22_rss_rxfh_indir_set(struct mvpp2_port *port)
{
	int tbl_id, entry, width, rxq;
	u32 cos_width, cpu_width, cpu_id;
	int num_cos_queues = port->cos_cfg.num_cos_queues;
	u32 reg_val;

	/* Calculate cpu and cos width */
	mvpp2_width_calc(port, &cpu_width, &cos_width);
	width = cos_width + cpu_width;

	for (tbl_id = 0; tbl_id < num_cos_queues; tbl_id++) {
		for (entry = 0; entry < MVPP22_RSS_TABLE_ENTRIES; entry++) {
			cpu_id = mvpp22_cpu_id_from_indir_get(port, entry);
			if (cpu_id < 0)
				return -EINVAL;
			/* rss_tbl_entry_set - access by Entry */
			/* Write index */
			reg_val = (entry << MVPP22_RSS_IDX_ENTRY_NUM_OFF |
				   tbl_id << MVPP22_RSS_IDX_TBL_NUM_OFF);
			mvpp2_write(port->priv, MVPP22_RSS_IDX_REG, reg_val);
			/* Write entry */
			reg_val &= ~MVPP22_RSS_TBL_ENTRY_MASK;
			rxq = (cpu_id << cos_width) | tbl_id;
			reg_val |= (rxq << MVPP22_RSS_TBL_ENTRY_OFF);
			mvpp2_write(port->priv, MVPP22_RSS_TBL_ENTRY_REG,
				    reg_val);
			reg_val &= ~MVPP22_RSS_WIDTH_MASK;
			reg_val |= (width << MVPP22_RSS_WIDTH_OFF);
			mvpp2_write(port->priv, MVPP22_RSS_WIDTH_REG, reg_val);
		}
	}
	return 0;
}

static int mvpp2_open_prs_cls_rss(struct net_device *dev)
{
	struct mvpp2_port *port = netdev_priv(dev);
	unsigned char mac_bcast[ETH_ALEN] = {
			0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
	struct mvpp2 *priv = port->priv;
	int err;

	err = mvpp2_prs_mac_da_accept(port, mac_bcast, true);
	if (err) {
		netdev_err(dev, "mvpp2_prs_mac_da_accept BC failed\n");
		return err;
	}
	err = mvpp2_prs_mac_da_accept(port, dev->dev_addr, true);
	if (err) {
		netdev_err(dev, "mvpp2_prs_mac_da_accept own addr failed\n");
		return err;
	}
	err = mvpp2_prs_tag_mode_set(priv, port->id, MVPP2_TAG_TYPE_MH);
	if (err) {
		netdev_err(dev, "mvpp2_prs_tag_mode_set failed\n");
		return err;
	}
	err = mvpp2_prs_def_flow(port);
	if (err) {
		netdev_err(dev, "mvpp2_prs_def_flow failed\n");
		return err;
	}
	err = mvpp2_update_flow_info(priv);
	if (err) {
		netdev_err(port->dev, "cannot update flow info\n");
		return err;
	}

	/* Set CoS classifier */
	err = mvpp2_cos_classifier_set(port, port->cos_cfg.cos_classifier);
	if (err) {
		netdev_err(port->dev, "cannot set cos classifier\n");
		return err;
	}

	/* Init C2 rules */
	err = mvpp2_cls_c2_rule_set(port, mvpp2_cpu2rxq(port));
	if (err) {
		netdev_err(port->dev, "cannot init C2 rules\n");
		return err;
	}

	if (priv->hw_version == MVPP21)
		return 0;

	/* Assign rss table for rxq belong to this port */
	mvpp22_rss_rxq_set(port);

	if (!mvpp22_rss_is_supported(port))
		return 0;

	/* RSS start */
	err = mvpp22_rss_udp_mode_set(port, port->rss_cfg.rss_mode);
	if (err) {
		netdev_err(port->dev, "cannot set rss mode\n");
		return err;
	}
	err = mvpp22_rss_rxfh_indir_set(port);
	if (err) {
		netdev_err(port->dev, "cannot init rss rxfh indir\n");
		return err;
	}
	err = mvpp22_rss_enable(port, port->rss_cfg.rss_en, true);

	return err;
}

static int mvpp2_open(struct net_device *dev)
{
	struct mvpp2_port *port = netdev_priv(dev);
	struct mvpp2 *priv = port->priv;
	struct mvpp2_port_pcpu *port_pcpu;
	bool valid_link = false;
	int err, cpu;

	/* Allocate the Rx/Tx queues */
	err = mvpp2_setup_rxqs(port);
	if (err) {
		netdev_err(port->dev, "cannot allocate Rx queues\n");
		return err;
	}

	err = mvpp2_setup_txqs(port);
	if (err) {
		netdev_err(port->dev, "cannot allocate Tx queues\n");
		goto err_cleanup_rxqs;
	}

	/* Recycle buffer pool for performance optimization */
	mvpp2_recycle_open();

	err = mvpp2_irqs_init(port);
	if (err) {
		netdev_err(port->dev, "cannot init IRQs\n");
		goto err_cleanup_txqs;
	}

	/* Phylink isn't supported yet in ACPI mode */
	if (port->of_node) {
		err = phylink_of_phy_connect(port->phylink, port->of_node, 0);
		if (err) {
			netdev_err(port->dev, "could not attach PHY (%d)\n",
				   err);
			goto err_free_irq;
		}

		valid_link = true;
	}

	if (priv->hw_version != MVPP21 && port->link_irq &&
	    (!port->phylink || !port->has_phy)) {
		mvpp2_txqs_on_tasklet_init(port);
		err = request_irq(port->link_irq, mvpp2_link_status_isr, 0,
				  dev->name, port);
		if (err) {
			netdev_err(port->dev, "cannot request link IRQ %d\n",
				   port->link_irq);
			goto err_free_irq;
		}

		mvpp22_gop_setup_irq(port);

		/* In default link is down */
		netif_carrier_off(port->dev);

		valid_link = true;
	} else {
		port->link_irq = 0;
	}

	if (!valid_link) {
		netdev_err(port->dev,
			   "invalid configuration: no dt or link IRQ");
		goto err_free_irq;
	}

	/* Init bulk-transmit timer */
	for (cpu = 0; cpu < used_hifs; cpu++) {
		port_pcpu = per_cpu_ptr(port->pcpu, cpu);
		port_pcpu->bulk_timer_scheduled = false;
		port_pcpu->bulk_timer_restart_req = false;
	}

	/* Unmask interrupts on all CPUs */
	on_each_cpu(mvpp2_interrupts_unmask, port, 1);
	mvpp2_shared_interrupt_mask_unmask(port, false);

	mvpp2_start_dev(port);

	/* Start hardware statistics gathering */
	queue_delayed_work(priv->stats_queue, &port->stats_work,
			   MVPP2_MIB_COUNTERS_STATS_DELAY);

	if (port->flags & MVPP22_F_IF_MUSDK)
		return 0;
	/* For MUSDK prs/cls/rss should be open ONCE only on MUSDK-enabling */
	err = mvpp2_open_prs_cls_rss(dev);
	if (err < 0)
		goto err_free_all;

	return 0;

err_free_all:
	if (port->phylink)
		phylink_disconnect_phy(port->phylink);
	if (port->link_irq)
		free_irq(port->link_irq, port);
err_free_irq:
	mvpp2_irqs_deinit(port);
err_cleanup_txqs:
	mvpp2_cleanup_txqs(port);
err_cleanup_rxqs:
	mvpp2_cleanup_rxqs(port);
	return err;
}

static int mvpp2_stop(struct net_device *dev)
{
	struct mvpp2_port *port = netdev_priv(dev);
	struct mvpp2_port_pcpu *port_pcpu;
	int cpu;

	mvpp2_stop_dev(port);

	/* Mask interrupts on all CPUs */
	on_each_cpu(mvpp2_interrupts_mask, port, 1);
	mvpp2_shared_interrupt_mask_unmask(port, true);

	if (port->phylink)
		phylink_disconnect_phy(port->phylink);
	if (port->link_irq)
		free_irq(port->link_irq, port);

	mvpp2_irqs_deinit(port);

	if (!port->has_tx_irqs) {
		for (cpu = 0; cpu < used_hifs; cpu++) {
			port_pcpu = per_cpu_ptr(port->pcpu, cpu);

			hrtimer_cancel(&port_pcpu->tx_done_timer);
			port_pcpu->tx_done_timer_scheduled = false;
			tasklet_kill(&port_pcpu->tx_done_tasklet);
		}
	}
	/* Cancel bulk tasklet and timer */
	for (cpu = 0; cpu < used_hifs; cpu++) {
		port_pcpu = per_cpu_ptr(port->pcpu, cpu);
		hrtimer_cancel(&port_pcpu->bulk_timer);
		tasklet_kill(&port_pcpu->bulk_tasklet);
	}

	mvpp2_txqs_on_tasklet_kill(port);
	mvpp2_cleanup_rxqs(port);
	mvpp2_cleanup_txqs(port);

	cancel_delayed_work_sync(&port->stats_work);

	mvpp2_recycle_close();

	return 0;
}

static int mvpp2_prs_mac_da_accept_list(struct mvpp2_port *port,
					struct netdev_hw_addr_list *list)
{
	struct netdev_hw_addr *ha;
	int ret;

	netdev_hw_addr_list_for_each(ha, list) {
		ret = mvpp2_prs_mac_da_accept(port, ha->addr, true);
		if (ret)
			return ret;
	}

	return 0;
}

static void mvpp2_set_rx_promisc(struct mvpp2_port *port, bool enable)
{
	if (!enable && (port->dev->features & NETIF_F_HW_VLAN_CTAG_FILTER))
		mvpp2_prs_vid_enable_filtering(port);
	else
		mvpp2_prs_vid_disable_filtering(port);

	mvpp2_prs_mac_promisc_set(port->priv, port->id,
				  MVPP2_PRS_L2_UNI_CAST, enable);

	mvpp2_prs_mac_promisc_set(port->priv, port->id,
				  MVPP2_PRS_L2_MULTI_CAST, enable);
}

static void mvpp2_set_rx_mode(struct net_device *dev)
{
	struct mvpp2_port *port = netdev_priv(dev);

	/* Clear the whole UC and MC list */
	mvpp2_prs_mac_del_all(port);

	if (dev->flags & IFF_PROMISC) {
		mvpp2_set_rx_promisc(port, true);
		return;
	}

	mvpp2_set_rx_promisc(port, false);

	if (netdev_uc_count(dev) > MVPP2_PRS_MAC_UC_FILT_MAX ||
	    mvpp2_prs_mac_da_accept_list(port, &dev->uc))
		mvpp2_prs_mac_promisc_set(port->priv, port->id,
					  MVPP2_PRS_L2_UNI_CAST, true);

	if (dev->flags & IFF_ALLMULTI) {
		mvpp2_prs_mac_promisc_set(port->priv, port->id,
					  MVPP2_PRS_L2_MULTI_CAST, true);
		return;
	}

	if (netdev_mc_count(dev) > MVPP2_PRS_MAC_MC_FILT_MAX ||
	    mvpp2_prs_mac_da_accept_list(port, &dev->mc))
		mvpp2_prs_mac_promisc_set(port->priv, port->id,
					  MVPP2_PRS_L2_MULTI_CAST, true);
}

static int mvpp2_set_mac_address(struct net_device *dev, void *p)
{
	const struct sockaddr *addr = p;
	int err;

	if (!is_valid_ether_addr(addr->sa_data))
		return -EADDRNOTAVAIL;

	err = mvpp2_prs_update_mac_da(dev, addr->sa_data);
	if (err) {
		/* Reconfigure parser accept the original MAC address */
		mvpp2_prs_update_mac_da(dev, dev->dev_addr);
		netdev_err(dev, "failed to change MAC address\n");
	}
	return err;
}

static int mvpp2_change_mtu(struct net_device *dev, int mtu)
{
	struct mvpp2_port *port = netdev_priv(dev);
	int err;

	if (port->flags & MVPP22_F_IF_MUSDK) {
		netdev_err(dev, "MTU can not be modified for port in MUSDK mode\n");
		return -EPERM;
	}

	if (!IS_ALIGNED(MVPP2_RX_PKT_SIZE(mtu), 8)) {
		netdev_info(dev, "illegal MTU value %d, round to %d\n", mtu,
			    ALIGN(MVPP2_RX_PKT_SIZE(mtu), 8));
		mtu = ALIGN(MVPP2_RX_PKT_SIZE(mtu), 8);
	}

	if (!netif_running(dev)) {
		err = mvpp2_bm_update_mtu(dev, mtu);
		if (!err) {
			port->pkt_size =  MVPP2_RX_PKT_SIZE(mtu);
			return 0;
		}

		/* Reconfigure BM to the original MTU */
		err = mvpp2_bm_update_mtu(dev, dev->mtu);
		if (err)
			goto log_error;
	}

	mvpp2_stop_dev(port);

	err = mvpp2_bm_update_mtu(dev, mtu);
	if (!err) {
		port->pkt_size =  MVPP2_RX_PKT_SIZE(mtu);
		goto out_start;
	}

	/* Reconfigure BM to the original MTU */
	err = mvpp2_bm_update_mtu(dev, dev->mtu);
	if (err)
		goto log_error;

out_start:
	mvpp2_start_dev(port);
	mvpp2_egress_enable(port);
	mvpp2_ingress_enable(port);

	return 0;
log_error:
	netdev_err(dev, "failed to change MTU\n");
	return err;
}

static void
mvpp2_get_stats64(struct net_device *dev, struct rtnl_link_stats64 *stats)
{
	struct mvpp2_port *port = netdev_priv(dev);
	unsigned int start;
	int cpu;

	for_each_possible_cpu(cpu) {
		struct mvpp2_pcpu_stats *cpu_stats;
		u64 rx_packets;
		u64 rx_bytes;
		u64 tx_packets;
		u64 tx_bytes;

		cpu_stats = per_cpu_ptr(port->stats, cpu);
		do {
			start = u64_stats_fetch_begin_irq(&cpu_stats->syncp);
			rx_packets = cpu_stats->rx_packets;
			rx_bytes   = cpu_stats->rx_bytes;
			tx_packets = cpu_stats->tx_packets;
			tx_bytes   = cpu_stats->tx_bytes;
		} while (u64_stats_fetch_retry_irq(&cpu_stats->syncp, start));

		stats->rx_packets += rx_packets;
		stats->rx_bytes   += rx_bytes;
		stats->tx_packets += tx_packets;
		stats->tx_bytes   += tx_bytes;
	}

	stats->rx_errors	= dev->stats.rx_errors;
	stats->rx_dropped	= dev->stats.rx_dropped;
	stats->tx_dropped	= dev->stats.tx_dropped;
}

static int mvpp2_ioctl(struct net_device *dev, struct ifreq *ifr, int cmd)
{
	struct mvpp2_port *port = netdev_priv(dev);

	if (!port->phylink)
		return -ENOTSUPP;

	return phylink_mii_ioctl(port->phylink, ifr, cmd);
}

static int mvpp2_vlan_rx_add_vid(struct net_device *dev, __be16 proto, u16 vid)
{
	int ret;
	struct mvpp2_port *port = netdev_priv(dev);

	ret = mvpp2_prs_vid_entry_add(port, vid);
	if (ret)
		netdev_err(dev, "rx-vlan-filter offloading cannot accept more than %d VIDs per port\n",
			   MVPP2_PRS_VLAN_FILT_MAX - 1);
	return ret;
}

static int mvpp2_vlan_rx_kill_vid(struct net_device *dev, __be16 proto, u16 vid)
{
	struct mvpp2_port *port = netdev_priv(dev);

	mvpp2_prs_vid_entry_remove(port, vid);
	return 0;
}

static int mvpp2_set_features(struct net_device *dev,
			      netdev_features_t features)
{
	struct mvpp2_port *port = netdev_priv(dev);
	netdev_features_t changed = dev->features ^ features;

	if (changed & NETIF_F_HW_VLAN_CTAG_FILTER) {
		if (features & NETIF_F_HW_VLAN_CTAG_FILTER) {
			mvpp2_prs_vid_enable_filtering(port);
		} else {
			/* Invalidate all registered VID filters for this
			 * port
			 */
			mvpp2_prs_vid_remove_all(port);

			mvpp2_prs_vid_disable_filtering(port);
		}
	}

	if (changed & NETIF_F_RXHASH) {
		if (!mvpp22_rss_is_supported(port))
			return -EOPNOTSUPP;
		mvpp22_rss_enable(port, !!(features & NETIF_F_RXHASH), false);
	}

	return 0;
}

/* Ethtool methods */

static int mvpp2_ethtool_nway_reset(struct net_device *dev)
{
	struct mvpp2_port *port = netdev_priv(dev);

	if (!port->phylink)
		return -ENOTSUPP;

	return phylink_ethtool_nway_reset(port->phylink);
}

/* Set interrupt coalescing for ethtools */
static int mvpp2_ethtool_set_coalesce(struct net_device *dev,
				      struct ethtool_coalesce *c)
{
	struct mvpp2_port *port = netdev_priv(dev);
	struct mvpp2_tx_queue *txq;
	int queue;

	for (queue = 0; queue < port->nrxqs; queue++) {
		struct mvpp2_rx_queue *rxq = port->rxqs[queue];

		rxq->time_coal = c->rx_coalesce_usecs;
		rxq->pkts_coal = c->rx_max_coalesced_frames;
		mvpp2_rx_pkts_coal_set(port, rxq);
		mvpp2_rx_time_coal_set(port, rxq);
	}

	/* Set TX time and pkts coalescing configuration */
	if (port->has_tx_irqs)
		port->tx_time_coal = c->tx_coalesce_usecs;

	for (queue = 0; queue < port->ntxqs; queue++) {
		txq = port->txqs[queue];
		txq->done_pkts_coal = c->tx_max_coalesced_frames;
		if (port->has_tx_irqs &&
		    txq->done_pkts_coal > MVPP2_TXQ_THRESH_MASK)
			txq->done_pkts_coal = MVPP2_TXQ_THRESH_MASK;
	}

	if (port->has_tx_irqs) {
		/* Download configured values into MVPP2 HW */
		mvpp2_tx_time_coal_set(port);
		mvpp2_tx_pkts_coal_set(port);
	}

	return 0;
}

/* get coalescing for ethtools */
static int mvpp2_ethtool_get_coalesce(struct net_device *dev,
				      struct ethtool_coalesce *c)
{
	struct mvpp2_port *port = netdev_priv(dev);

	c->rx_coalesce_usecs       = port->rxqs[0]->time_coal;
	c->rx_max_coalesced_frames = port->rxqs[0]->pkts_coal;
	c->tx_max_coalesced_frames = port->txqs[0]->done_pkts_coal;
	c->tx_coalesce_usecs       = port->tx_time_coal;
	return 0;
}

static void mvpp2_ethtool_get_drvinfo(struct net_device *dev,
				      struct ethtool_drvinfo *drvinfo)
{
	struct mvpp2_port *port = netdev_priv(dev);

	strlcpy(drvinfo->driver, MVPP2_DRIVER_NAME,
		sizeof(drvinfo->driver));
	strlcpy(drvinfo->version, MVPP2_DRIVER_VERSION,
		sizeof(drvinfo->version));
	strlcpy(drvinfo->bus_info, dev_name(&dev->dev),
		sizeof(drvinfo->bus_info));
	drvinfo->n_priv_flags = (port->priv->hw_version == MVPP21) ?
			0 : ARRAY_SIZE(mvpp22_priv_flags_strings);
}

static void mvpp2_ethtool_get_ringparam(struct net_device *dev,
					struct ethtool_ringparam *ring)
{
	struct mvpp2_port *port = netdev_priv(dev);

	ring->rx_max_pending = MVPP2_MAX_RXD_MAX;
	ring->tx_max_pending = MVPP2_MAX_TXD_MAX;
	ring->rx_pending = port->rx_ring_size;
	ring->tx_pending = port->tx_ring_size;
}

static int mvpp2_ethtool_set_ringparam(struct net_device *dev,
				       struct ethtool_ringparam *ring)
{
	struct mvpp2_port *port = netdev_priv(dev);
	u16 prev_rx_ring_size = port->rx_ring_size;
	u16 prev_tx_ring_size = port->tx_ring_size;
	int err;

	err = mvpp2_check_ringparam_valid(dev, ring);
	if (err)
		return err;

	if (!netif_running(dev)) {
		port->rx_ring_size = ring->rx_pending;
		port->tx_ring_size = ring->tx_pending;
		return 0;
	}

	/* The interface is running, so we have to force a
	 * reallocation of the queues
	 */
	mvpp2_stop_dev(port);
	mvpp2_cleanup_rxqs(port);
	mvpp2_cleanup_txqs(port);

	port->rx_ring_size = ring->rx_pending;
	port->tx_ring_size = ring->tx_pending;

	err = mvpp2_setup_rxqs(port);
	if (err) {
		/* Reallocate Rx queues with the original ring size */
		port->rx_ring_size = prev_rx_ring_size;
		ring->rx_pending = prev_rx_ring_size;
		err = mvpp2_setup_rxqs(port);
		if (err)
			goto err_out;
	}
	err = mvpp2_setup_txqs(port);
	if (err) {
		/* Reallocate Tx queues with the original ring size */
		port->tx_ring_size = prev_tx_ring_size;
		ring->tx_pending = prev_tx_ring_size;
		err = mvpp2_setup_txqs(port);
		if (err)
			goto err_clean_rxqs;
	}

	mvpp2_start_dev(port);
	mvpp2_egress_enable(port);
	mvpp2_ingress_enable(port);

	return 0;

err_clean_rxqs:
	mvpp2_cleanup_rxqs(port);
err_out:
	netdev_err(dev, "failed to change ring parameters");
	return err;
}

static u32 mvpp2_ethtool_get_rxfh_indir_size(struct net_device *dev)
{
	struct mvpp2_port *port = netdev_priv(dev);

	return mvpp22_rss_is_supported(port) ?
		MVPP22_RSS_TABLE_ENTRIES : 0;
}

static int mvpp22_get_rss_hash_opts(struct mvpp2_port *port,
				    struct ethtool_rxnfc *info)
{
	switch (info->cmd) {
	case IPV4_FLOW:
	case IPV6_FLOW:
		info->data = RXH_IP_SRC | RXH_IP_DST;
		break;
	case TCP_V4_FLOW:
	case TCP_V6_FLOW:
		info->data = RXH_IP_SRC | RXH_IP_DST | RXH_L4_B_0_1 |
			     RXH_L4_B_2_3;
		break;
	case UDP_V4_FLOW:
	case UDP_V6_FLOW:
		info->data = RXH_IP_SRC | RXH_IP_DST;
		if (port->rss_cfg.rss_mode == MVPP22_RSS_5T)
			info->data |= RXH_L4_B_0_1 | RXH_L4_B_2_3;
		break;
	default:
		return -EOPNOTSUPP;
	}

	return 0;
}

static int mvpp22_set_rss_hash_opts(struct mvpp2_port *port,
				    struct ethtool_rxnfc *info)
{
	u32 mask;
	int rss_mode;

	mask = RXH_IP_SRC | RXH_IP_DST | RXH_L4_B_0_1 | RXH_L4_B_2_3;
	if (info->data & ~mask)
		return -EINVAL;

	switch (info->flow_type) {
	case IPV4_FLOW:
	case IPV6_FLOW:
		mask = RXH_IP_SRC | RXH_IP_DST;
		if ((info->data & mask) != mask)
			return -EINVAL;
		break;
	case TCP_V4_FLOW:
	case TCP_V6_FLOW:
		if ((info->data & mask) != mask)
			return -EOPNOTSUPP;
		break;
	case UDP_V4_FLOW:
	case UDP_V6_FLOW:
		mask = RXH_IP_SRC | RXH_IP_DST;
		if ((info->data & mask) != mask)
			return -EOPNOTSUPP;

		mask = RXH_L4_B_0_1 | RXH_L4_B_2_3;

		if ((info->data & mask) != mask)
			rss_mode = MVPP22_RSS_5T;
		else if (!(info->data & mask))
			rss_mode = MVPP22_RSS_2T;
		else
			return -EOPNOTSUPP;

		mvpp22_rss_udp_mode_set(port, rss_mode);
		break;
	default:
		return -EOPNOTSUPP;
	}

	return 0;
}

static int mvpp2_ethtool_get_rxnfc(struct net_device *dev,
				   struct ethtool_rxnfc *info, u32 *rules)
{
	struct mvpp2_port *port = netdev_priv(dev);
	int ret = 0;

	if (!mvpp22_rss_is_supported(port))
		return -EOPNOTSUPP;

	switch (info->cmd) {
	case ETHTOOL_GRXRINGS:
		info->data = mvpp2_ethtool_get_rxfh_indir_size(dev);
		break;
	case ETHTOOL_GRXFH:
		ret = mvpp22_get_rss_hash_opts(port, info);
		break;
	default:
		return -ENOTSUPP;
	}
	return ret;
}

static int mvpp2_ethtool_set_rxnfc(struct net_device *dev,
				   struct ethtool_rxnfc *info)
{
	struct mvpp2_port *port = netdev_priv(dev);
	int ret = 0;

	if (!mvpp22_rss_is_supported(port))
		return -EOPNOTSUPP;

	switch (info->cmd) {
	case ETHTOOL_SRXFH:
		ret = mvpp22_set_rss_hash_opts(port, info);
		break;
	default:
		return -ENOTSUPP;
	}
	return ret;
}

static int mvpp2_ethtool_get_rxfh(struct net_device *dev, u32 *indir, u8 *key,
				  u8 *hfunc)
{
	struct mvpp2_port *port = netdev_priv(dev);

	if (!mvpp22_rss_is_supported(port))
		return -EOPNOTSUPP;

	if (indir)
		memcpy(indir, port->priv->indir, sizeof(port->priv->indir[0]) *
		       mvpp2_ethtool_get_rxfh_indir_size(dev));

	if (hfunc)
		*hfunc = ETH_RSS_HASH_TOP;

	return 0;
}

static int mvpp2_ethtool_set_rxfh(struct net_device *dev, const u32 *indir,
				  const u8 *key, const u8 hfunc)
{
	struct mvpp2_port *port = netdev_priv(dev);
	int ret, size, i;
	u32 indir_orig[MVPP22_RSS_TABLE_ENTRIES];

	if (!mvpp22_rss_is_supported(port))
		return -EOPNOTSUPP;

	if (hfunc != ETH_RSS_HASH_NO_CHANGE && hfunc != ETH_RSS_HASH_TOP)
		return -ENOTSUPP;
	if (key || !indir)
		return -ENOTSUPP;

	size = sizeof(*indir) * MVPP22_RSS_TABLE_ENTRIES;

	/* Check input.
	 * For example, "weight 0 0 0 0 1" may have "4" inside of indir
	 */
	for (i = 0; i < MVPP22_RSS_TABLE_ENTRIES; i++)
		if (indir[i] >= used_hifs)
			return -EINVAL;

	memcpy(indir_orig, port->priv->indir, size);
	memcpy(port->priv->indir, indir, size);
	ret = mvpp22_rss_rxfh_indir_set(port);
	if (ret) {
		netdev_err(dev, "fail to change rxfh indir table");
		/* Rollback to original indir[] table */
		memcpy(port->priv->indir, indir_orig, size);
		mvpp22_rss_rxfh_indir_set(port);
	}
	return ret;
}

static u32 mvpp22_get_priv_flags(struct net_device *dev)
{
	struct mvpp2_port *port = netdev_priv(dev);
	u32 priv_flags = 0;

	if (port->flags & MVPP22_F_IF_MUSDK)
		priv_flags |= MVPP22_F_IF_MUSDK_PRIV;
	return priv_flags;
}

static int mvpp2_port_musdk_cfg(struct net_device *dev, bool ena, u8 *rss_en)
{
	struct mvpp2_port_us_cfg {
		unsigned int nqvecs;
		unsigned int nrxqs;
		unsigned int ntxqs;
		int mtu;
		bool rxhash_en;
		u8 rss_en;
	} *us;

	struct mvpp2_port *port = netdev_priv(dev);

	if (ena) {
		/* Disable Queues and IntVec allocations for MUSDK,
		 * but save original values.
		 */
		us = kzalloc(sizeof(*us), GFP_KERNEL);
		if (!us)
			return -ENOMEM;
		port->us_cfg = (void *)us;
		us->nqvecs = port->nqvecs;
		us->nrxqs  = port->nrxqs;
		us->ntxqs = port->ntxqs;
		us->mtu = dev->mtu;
		us->rss_en = *rss_en;
		us->rxhash_en = !!(dev->hw_features & NETIF_F_RXHASH);

		port->nqvecs = 0;
		port->nrxqs  = 0;
		port->ntxqs  = 0;
		if (us->rxhash_en) {
			dev->hw_features &= ~NETIF_F_RXHASH;
			netdev_update_features(dev);
		}
	} else {
		/* Back to Kernel mode */
		us = port->us_cfg;
		port->nqvecs = us->nqvecs;
		port->nrxqs  = us->nrxqs;
		port->ntxqs  = us->ntxqs;
		*rss_en = us->rss_en;
		if (us->rxhash_en) {
			dev->hw_features |= NETIF_F_RXHASH;
			netdev_update_features(dev);
		}
		kfree(us);
		port->us_cfg = NULL;
	}
	return 0;
}

static int mvpp2_port_musdk_set(struct net_device *dev, bool ena)
{
	struct mvpp2_port *port = netdev_priv(dev);
	bool running = netif_running(dev);
	int err, warn0, warn1;
	u8 rss_en;

	/* This procedure is called by ethtool change or by Module-remove.
	 * For "remove" do anything only if we are in musdk-mode
	 * and toggling back to Kernel-mode is really required.
	 */
	if (!ena && !port->us_cfg)
		return 0;

	if (running)
		mvpp2_stop(dev);

	if (ena) {
		rss_en = port->rss_cfg.rss_en;
		warn0 = mvpp22_rss_enable(port, false, false);
		warn1 = mvpp2_open_prs_cls_rss(dev);
		err = mvpp2_port_musdk_cfg(dev, ena, &rss_en);
		port->flags |= MVPP22_F_IF_MUSDK;
	} else {
		mvpp2_port_musdk_cfg(dev, ena, &rss_en);
		port->flags &= ~MVPP22_F_IF_MUSDK;
		warn0 = mvpp22_rss_enable(port, rss_en, false);
		warn1 = 0;
		err = 0;
	}

	if (err || warn0 || warn1) {
		netdev_err(dev, "musdk set=%d: error=%d warn0=%d warn1=%d\n",
			   ena, err, warn0, warn1);
		if (err)
			return err;
		/* print Error message but continue */
	}

	if (running)
		mvpp2_open(dev);
	return 0;
}

static int mvpp22_set_priv_flags(struct net_device *dev, u32 priv_flags)
{
	struct mvpp2_port *port = netdev_priv(dev);
	bool f_old, f_new;
	int err = 0;

	f_old = port->flags & MVPP22_F_IF_MUSDK;
	f_new = priv_flags & MVPP22_F_IF_MUSDK_PRIV;
	if (f_old != f_new)
		err = mvpp2_port_musdk_set(dev, f_new);

	return err;
}

static void mvpp2_ethtool_get_pause_param(struct net_device *dev,
					  struct ethtool_pauseparam *pause)
{
	struct mvpp2_port *port = netdev_priv(dev);

	if (!port->phylink)
		return;

	phylink_ethtool_get_pauseparam(port->phylink, pause);
}

static int mvpp2_ethtool_set_pause_param(struct net_device *dev,
					 struct ethtool_pauseparam *pause)
{
	struct mvpp2_port *port = netdev_priv(dev);

	if (!port->phylink)
		return -ENOTSUPP;

	return phylink_ethtool_set_pauseparam(port->phylink, pause);
}

static int mvpp2_ethtool_get_link_ksettings(struct net_device *dev,
					    struct ethtool_link_ksettings *cmd)
{
	struct mvpp2_port *port = netdev_priv(dev);

	if (!port->phylink)
		return -ENOTSUPP;

	return phylink_ethtool_ksettings_get(port->phylink, cmd);
}

static int mvpp2_ethtool_set_link_ksettings(struct net_device *dev,
					    const struct ethtool_link_ksettings *cmd)
{
	struct mvpp2_port *port = netdev_priv(dev);

	if (!port->phylink)
		return -ENOTSUPP;

	return phylink_ethtool_ksettings_set(port->phylink, cmd);
}

/* Device ops */

static const struct net_device_ops mvpp2_netdev_ops = {
	.ndo_open		= mvpp2_open,
	.ndo_stop		= mvpp2_stop,
	.ndo_start_xmit		= mvpp2_tx,
	.ndo_set_rx_mode	= mvpp2_set_rx_mode,
	.ndo_set_mac_address	= mvpp2_set_mac_address,
	.ndo_change_mtu		= mvpp2_change_mtu,
	.ndo_get_stats64	= mvpp2_get_stats64,
	.ndo_do_ioctl		= mvpp2_ioctl,
	.ndo_vlan_rx_add_vid	= mvpp2_vlan_rx_add_vid,
	.ndo_vlan_rx_kill_vid	= mvpp2_vlan_rx_kill_vid,
	.ndo_set_features	= mvpp2_set_features,
};

static const struct ethtool_ops mvpp2_eth_tool_ops = {
	.nway_reset		= mvpp2_ethtool_nway_reset,
	.get_link		= ethtool_op_get_link,
	.set_coalesce		= mvpp2_ethtool_set_coalesce,
	.get_coalesce		= mvpp2_ethtool_get_coalesce,
	.get_drvinfo		= mvpp2_ethtool_get_drvinfo,
	.get_ringparam		= mvpp2_ethtool_get_ringparam,
	.set_ringparam		= mvpp2_ethtool_set_ringparam,
	.get_strings		= mvpp2_ethtool_get_strings,
	.get_ethtool_stats	= mvpp2_ethtool_get_stats,
	.get_sset_count		= mvpp2_ethtool_get_sset_count,
	.get_pauseparam		= mvpp2_ethtool_get_pause_param,
	.set_pauseparam		= mvpp2_ethtool_set_pause_param,
	.get_link_ksettings	= mvpp2_ethtool_get_link_ksettings,
	.set_link_ksettings	= mvpp2_ethtool_set_link_ksettings,
	.get_rxnfc		= mvpp2_ethtool_get_rxnfc,
	.set_rxnfc		= mvpp2_ethtool_set_rxnfc,
	.get_rxfh_indir_size	= mvpp2_ethtool_get_rxfh_indir_size,
	.get_rxfh		= mvpp2_ethtool_get_rxfh,
	.set_rxfh		= mvpp2_ethtool_set_rxfh,
	.get_priv_flags		= mvpp22_get_priv_flags,
	.set_priv_flags		= mvpp22_set_priv_flags,
};

/* Used for PPv2.1, PPv2.2 with the old Device Tree binding that
 * had a single IRQ defined per-port and single resource mode.
 */
static int mvpp2_simple_queue_vectors_init(struct mvpp2_port *port,
					   struct device_node *port_node)
{
	struct mvpp2_queue_vector *v = &port->qvecs[0];

	v->first_rxq = 0;
	v->nrxqs = port->nrxqs;
	v->type = MVPP2_QUEUE_VECTOR_SHARED;
	v->sw_thread_id = 0;
	v->sw_thread_mask = *cpumask_bits(cpu_online_mask);
	v->port = port;
	v->irq = irq_of_parse_and_map(port_node, 0);
	if (v->irq <= 0)
		return -EINVAL;
	netif_napi_add(port->dev, &v->napi, mvpp21_poll,
		       NAPI_POLL_WEIGHT);

	port->nqvecs = 1;

	return 0;
}

static int mvpp2_multi_queue_vectors_init(struct mvpp2_port *port,
					  struct device_node *port_node)
{
	struct mvpp2_queue_vector *v;
	int i, ret;

	if (queue_mode == MVPP2_SINGLE_RESOURCE_MODE)
		port->nqvecs = 1;
	else
		port->nqvecs = num_possible_cpus();

	if (queue_mode == MVPP2_QDIST_SINGLE_MODE)
		port->nqvecs += 1;

	for (i = 0; i < port->nqvecs; i++) {
		char irqname[16];

		v = port->qvecs + i;

		v->port = port;
		v->type = MVPP2_QUEUE_VECTOR_PRIVATE;
		v->sw_thread_id = i;
		v->sw_thread_mask = BIT(i);

		snprintf(irqname, sizeof(irqname), "hif%d", i);

		if (queue_mode == MVPP2_QDIST_MULTI_MODE) {
			v->first_rxq = i * MVPP2_DEFAULT_RXQ;
			v->nrxqs = MVPP2_DEFAULT_RXQ;
		} else if (i == (port->nqvecs - 1)) {
			if (queue_mode == MVPP2_QDIST_SINGLE_MODE)
				v->type = MVPP2_QUEUE_VECTOR_SHARED;
			else
				v->type = MVPP2_QUEUE_VECTOR_PRIVATE;
			v->first_rxq = 0;
			v->nrxqs = port->nrxqs;
		}

		if (port_node)
			v->irq = of_irq_get_byname(port_node, irqname);
		else
			v->irq = fwnode_irq_get(port->fwnode, i);
		if (v->irq <= 0) {
			ret = -EINVAL;
			goto err;
		}

		netif_napi_add(port->dev, &v->napi, mvpp22_poll,
			       NAPI_POLL_WEIGHT);
	}

	return 0;

err:
	for (i = 0; i < port->nqvecs; i++)
		irq_dispose_mapping(port->qvecs[i].irq);
	return ret;
}

static int mvpp2_queue_vectors_init(struct mvpp2_port *port,
				    struct device_node *port_node)
{
	if (port->has_tx_irqs)
		return mvpp2_multi_queue_vectors_init(port, port_node);
	else
		return mvpp2_simple_queue_vectors_init(port, port_node);
}

static void mvpp2_queue_vectors_deinit(struct mvpp2_port *port)
{
	int i;

	for (i = 0; i < port->nqvecs; i++)
		irq_dispose_mapping(port->qvecs[i].irq);
}

/* Configure Rx queue group interrupt for this port */
static void mvpp2_rx_irqs_setup(struct mvpp2_port *port)
{
	struct mvpp2 *priv = port->priv;
	u32 val;
	int i;

	if (priv->hw_version == MVPP21) {
		mvpp2_write(priv, MVPP21_ISR_RXQ_GROUP_REG(port->id),
			    port->nrxqs);
		return;
	}

	/* Handle the more complicated PPv2.2 case */
	for (i = 0; i < port->nqvecs; i++) {
		struct mvpp2_queue_vector *qv = port->qvecs + i;

		if (!qv->nrxqs)
			continue;

		val = qv->sw_thread_id;
		val |= port->id << MVPP22_ISR_RXQ_GROUP_INDEX_GROUP_OFFSET;
		mvpp2_write(priv, MVPP22_ISR_RXQ_GROUP_INDEX_REG, val);

		val = qv->first_rxq;
		val |= qv->nrxqs << MVPP22_ISR_RXQ_SUB_GROUP_SIZE_OFFSET;
		mvpp2_write(priv, MVPP22_ISR_RXQ_SUB_GROUP_CONFIG_REG, val);
	}
}

static void mvpp2_port_init_params(struct mvpp2_port *port)
{
	/* CoS init config */
	port->cos_cfg.cos_classifier = cos_classifier;
	port->cos_cfg.default_cos = default_cos;
	port->cos_cfg.num_cos_queues = num_cos_queues;
	port->cos_cfg.pri_map = pri_map;

	/* RSS init config */
	port->rss_cfg.dflt_cpu = default_cpu;
	/* RSS is disabled as default, it can be update when running */
	port->rss_cfg.rss_en = 0;
	port->rss_cfg.rss_mode = rss_mode ?
				MVPP22_RSS_2T : MVPP22_RSS_5T;
}

/* Initialize port HW */
static int mvpp2_port_init(struct mvpp2_port *port)
{
	struct device *dev = port->dev->dev.parent;
	struct mvpp2 *priv = port->priv;
	struct mvpp2_txq_pcpu *txq_pcpu;
	int queue, cpu, err;

	/* Checks for hardware constraints */
	if (port->first_rxq + port->nrxqs >
	    MVPP2_MAX_PORTS * priv->max_port_rxqs)
		return -EINVAL;

	if (port->nrxqs % 4 || port->nrxqs > priv->max_port_rxqs ||
	    port->ntxqs > MVPP2_MAX_TXQ)
		return -EINVAL;

	/* Disable port */
	mvpp2_egress_disable(port);
	mvpp2_port_disable(port);

	port->tx_time_coal = MVPP2_TXDONE_COAL_USEC;

	port->txqs = devm_kcalloc(dev, port->ntxqs, sizeof(*port->txqs),
				  GFP_KERNEL);
	if (!port->txqs)
		return -ENOMEM;

	/* Associate physical Tx queues to this port and initialize.
	 * The mapping is predefined.
	 */
	for (queue = 0; queue < port->ntxqs; queue++) {
		int queue_phy_id = mvpp2_txq_phys(port->id, queue);
		struct mvpp2_tx_queue *txq;

		txq = devm_kzalloc(dev, sizeof(*txq), GFP_KERNEL);
		if (!txq) {
			err = -ENOMEM;
			goto err_free_percpu;
		}

		txq->pcpu = alloc_percpu(struct mvpp2_txq_pcpu);
		if (!txq->pcpu) {
			err = -ENOMEM;
			goto err_free_percpu;
		}

		txq->id = queue_phy_id;
		txq->log_id = queue;
		txq->done_pkts_coal = MVPP2_TXDONE_COAL_PKTS_THRESH;
		for (cpu = 0; cpu < used_hifs; cpu++) {
			txq_pcpu = per_cpu_ptr(txq->pcpu, cpu);
			txq_pcpu->cpu = cpu;
		}

		port->txqs[queue] = txq;
	}

	port->rxqs = devm_kcalloc(dev, port->nrxqs, sizeof(*port->rxqs),
				  GFP_KERNEL);
	if (!port->rxqs) {
		err = -ENOMEM;
		goto err_free_percpu;
	}

	/* Allocate and initialize Rx queue for this port */
	for (queue = 0; queue < port->nrxqs; queue++) {
		struct mvpp2_rx_queue *rxq;

		/* Map physical Rx queue to port's logical Rx queue */
		rxq = devm_kzalloc(dev, sizeof(*rxq), GFP_KERNEL);
		if (!rxq) {
			err = -ENOMEM;
			goto err_free_percpu;
		}
		/* Map this Rx queue to a physical queue */
		rxq->id = port->first_rxq + queue;
		rxq->port = port->id;
		rxq->logic_rxq = queue;

		port->rxqs[queue] = rxq;
	}

	mvpp2_rx_irqs_setup(port);

	/* Create Rx descriptor rings */
	for (queue = 0; queue < port->nrxqs; queue++) {
		struct mvpp2_rx_queue *rxq = port->rxqs[queue];

		rxq->size = port->rx_ring_size;
		rxq->pkts_coal = MVPP2_RX_COAL_PKTS;
		rxq->time_coal = MVPP2_RX_COAL_USEC;
	}

	mvpp2_ingress_disable(port);

	/* Port default configuration */
	mvpp2_defaults_set(port);

	/* Port's classifier configuration */
	mvpp2_cls_oversize_rxq_set(port);
	mvpp2_cls_port_config(port);

	/* Provide an initial Rx packet size */
	port->pkt_size = MVPP2_RX_PKT_SIZE(port->dev->mtu);

	/* Initialize pools for swf */
	err = mvpp2_swf_bm_pool_init(port);
	if (err)
		goto err_free_percpu;

	return 0;

err_free_percpu:
	for (queue = 0; queue < port->ntxqs; queue++) {
		if (!port->txqs[queue])
			continue;
		free_percpu(port->txqs[queue]->pcpu);
	}
	return err;
}

/* Checks if the port DT description has more then 1 interrupt
 * described. On PPv2.1, there are single interrupts. On PPv2.2,
 * there are available up tp 9 interrupts, but we need to keep
 * support for old DTs.
 */
static bool mvpp2_check_if_multi_irq(struct mvpp2 *priv,
				     struct device_node *port_node)
{
	char irq_name[16];
	int ret, i;

	if (priv->hw_version == MVPP21)
		return false;

	for (i = 0; i < MVPP2_MAX_THREADS; i++) {
		snprintf(irq_name, sizeof(irq_name), "hif%d", i);
		ret = of_property_match_string(port_node, "interrupt-names",
					       irq_name);
		if (ret < 0)
			return false;
	}

	return true;
}

static void mvpp2_port_copy_mac_addr(struct net_device *dev, struct mvpp2 *priv,
				     struct fwnode_handle *fwnode,
				     char **mac_from)
{
	struct mvpp2_port *port = netdev_priv(dev);
	char hw_mac_addr[ETH_ALEN] = {0};
	char fw_mac_addr[ETH_ALEN];

	if (fwnode_get_mac_address(fwnode, fw_mac_addr, ETH_ALEN)) {
		*mac_from = "firmware node";
		ether_addr_copy(dev->dev_addr, fw_mac_addr);
		return;
	}

	if (priv->hw_version == MVPP21) {
		mvpp21_get_mac_address(port, hw_mac_addr);
		if (is_valid_ether_addr(hw_mac_addr)) {
			*mac_from = "hardware";
			ether_addr_copy(dev->dev_addr, hw_mac_addr);
			return;
		}
	}

	*mac_from = "random";
	eth_hw_addr_random(dev);
}

static void mvpp2_phylink_validate(struct net_device *dev,
				   unsigned long *supported,
				   struct phylink_link_state *state)
{
	__ETHTOOL_DECLARE_LINK_MODE_MASK(mask) = { 0, };

	phylink_set(mask, Autoneg);
	phylink_set_port_modes(mask);
	phylink_set(mask, Pause);
	phylink_set(mask, Asym_Pause);

	switch (state->interface) {
	case PHY_INTERFACE_MODE_10GKR:
		phylink_set(mask, 10000baseCR_Full);
		phylink_set(mask, 10000baseSR_Full);
		phylink_set(mask, 10000baseLR_Full);
		phylink_set(mask, 10000baseLRM_Full);
		phylink_set(mask, 10000baseER_Full);
		phylink_set(mask, 10000baseKR_Full);
		/* Fall-through */
	default:
		phylink_set(mask, 10baseT_Half);
		phylink_set(mask, 10baseT_Full);
		phylink_set(mask, 100baseT_Half);
		phylink_set(mask, 100baseT_Full);
		phylink_set(mask, 10000baseT_Full);
		/* Fall-through */
	case PHY_INTERFACE_MODE_1000BASEX:
	case PHY_INTERFACE_MODE_2500BASEX:
		phylink_set(mask, 1000baseT_Full);
		phylink_set(mask, 1000baseX_Full);
		phylink_set(mask, 2500baseX_Full);
	}

	bitmap_and(supported, supported, mask, __ETHTOOL_LINK_MODE_MASK_NBITS);
	bitmap_and(state->advertising, state->advertising, mask,
		   __ETHTOOL_LINK_MODE_MASK_NBITS);
}

static void mvpp22_xlg_link_state(struct mvpp2_port *port,
				  struct phylink_link_state *state)
{
	u32 val;

	state->speed = SPEED_10000;
	state->duplex = 1;
	state->an_complete = 1;

	val = readl(port->base + MVPP22_XLG_STATUS);
	state->link = !!(val & MVPP22_XLG_STATUS_LINK_UP);

	state->pause = 0;
	val = readl(port->base + MVPP22_XLG_CTRL0_REG);
	if (val & MVPP22_XLG_CTRL0_TX_FLOW_CTRL_EN)
		state->pause |= MLO_PAUSE_TX;
	if (val & MVPP22_XLG_CTRL0_RX_FLOW_CTRL_EN)
		state->pause |= MLO_PAUSE_RX;
}

static void mvpp2_gmac_link_state(struct mvpp2_port *port,
				  struct phylink_link_state *state)
{
	u32 val;

	val = readl(port->base + MVPP2_GMAC_STATUS0);

	state->an_complete = !!(val & MVPP2_GMAC_STATUS0_AN_COMPLETE);
	state->link = !!(val & MVPP2_GMAC_STATUS0_LINK_UP);
	state->duplex = !!(val & MVPP2_GMAC_STATUS0_FULL_DUPLEX);

	switch (port->phy_interface) {
	case PHY_INTERFACE_MODE_1000BASEX:
		state->speed = SPEED_1000;
		break;
	case PHY_INTERFACE_MODE_2500BASEX:
		state->speed = SPEED_2500;
		break;
	default:
		if (val & MVPP2_GMAC_STATUS0_GMII_SPEED)
			state->speed = SPEED_1000;
		else if (val & MVPP2_GMAC_STATUS0_MII_SPEED)
			state->speed = SPEED_100;
		else
			state->speed = SPEED_10;
	}

	state->pause = 0;
	if (val & MVPP2_GMAC_STATUS0_RX_PAUSE)
		state->pause |= MLO_PAUSE_RX;
	if (val & MVPP2_GMAC_STATUS0_TX_PAUSE)
		state->pause |= MLO_PAUSE_TX;
}

static int mvpp2_phylink_mac_link_state(struct net_device *dev,
					struct phylink_link_state *state)
{
	struct mvpp2_port *port = netdev_priv(dev);
	u32 mode;

	if (port->priv->hw_version != MVPP21 && port->gop_id == 0) {
		mode = readl(port->base + MVPP22_XLG_CTRL3_REG);
		mode &= MVPP22_XLG_CTRL3_MACMODESELECT_MASK;

		if (mode == MVPP22_XLG_CTRL3_MACMODESELECT_10G) {
			mvpp22_xlg_link_state(port, state);
			return 1;
		}
	}

	mvpp2_gmac_link_state(port, state);
	return 1;
}

static void mvpp2_mac_an_restart(struct net_device *dev)
{
	struct mvpp2_port *port = netdev_priv(dev);
	u32 val;

	if (port->phy_interface != PHY_INTERFACE_MODE_SGMII)
		return;

	val = readl(port->base + MVPP2_GMAC_AUTONEG_CONFIG);
	/* The RESTART_AN bit is cleared by the h/w after restarting the AN
	 * process.
	 */
	val |= MVPP2_GMAC_IN_BAND_RESTART_AN | MVPP2_GMAC_IN_BAND_AUTONEG;
	writel(val, port->base + MVPP2_GMAC_AUTONEG_CONFIG);
}

static void mvpp2_xlg_config(struct mvpp2_port *port, unsigned int mode,
			     const struct phylink_link_state *state)
{
	u32 ctrl0, ctrl4;

	ctrl0 = readl(port->base + MVPP22_XLG_CTRL0_REG);
	ctrl4 = readl(port->base + MVPP22_XLG_CTRL4_REG);

	if (state->pause & MLO_PAUSE_TX)
		ctrl0 |= MVPP22_XLG_CTRL0_TX_FLOW_CTRL_EN;
	if (state->pause & MLO_PAUSE_RX)
		ctrl0 |= MVPP22_XLG_CTRL0_RX_FLOW_CTRL_EN;

	ctrl4 &= ~MVPP22_XLG_CTRL4_MACMODSELECT_GMAC;
	ctrl4 |= MVPP22_XLG_CTRL4_FWD_FC | MVPP22_XLG_CTRL4_FWD_PFC;
	ctrl4 |= MVPP22_XLG_CTRL4_EN_IDLE_CHECK;

	writel(ctrl0, port->base + MVPP22_XLG_CTRL0_REG);
	writel(ctrl4, port->base + MVPP22_XLG_CTRL4_REG);
}

static void mvpp2_gmac_config(struct mvpp2_port *port, unsigned int mode,
			      const struct phylink_link_state *state)
{
	u32 an, ctrl0, ctrl2, ctrl4;
	bool mode_basex;

	an = readl(port->base + MVPP2_GMAC_AUTONEG_CONFIG);
	ctrl0 = readl(port->base + MVPP2_GMAC_CTRL_0_REG);
	ctrl2 = readl(port->base + MVPP2_GMAC_CTRL_2_REG);
	ctrl4 = readl(port->base + MVPP22_GMAC_CTRL_4_REG);

	/* Force link down */
	an &= ~MVPP2_GMAC_FORCE_LINK_PASS;
	an |= MVPP2_GMAC_FORCE_LINK_DOWN;
	writel(an, port->base + MVPP2_GMAC_AUTONEG_CONFIG);

	/* Set the GMAC in a reset state */
	ctrl2 |= MVPP2_GMAC_PORT_RESET_MASK;
	writel(ctrl2, port->base + MVPP2_GMAC_CTRL_2_REG);

	an &= ~(MVPP2_GMAC_CONFIG_MII_SPEED | MVPP2_GMAC_CONFIG_GMII_SPEED |
		MVPP2_GMAC_AN_SPEED_EN | MVPP2_GMAC_FC_ADV_EN |
		MVPP2_GMAC_FC_ADV_ASM_EN | MVPP2_GMAC_FLOW_CTRL_AUTONEG |
		MVPP2_GMAC_CONFIG_FULL_DUPLEX | MVPP2_GMAC_AN_DUPLEX_EN |
		MVPP2_GMAC_FORCE_LINK_DOWN);
	ctrl0 &= ~MVPP2_GMAC_PORT_TYPE_MASK;
	ctrl2 &= ~(MVPP2_GMAC_PORT_RESET_MASK | MVPP2_GMAC_PCS_ENABLE_MASK);

	mode_basex = state->interface == PHY_INTERFACE_MODE_1000BASEX ||
			state->interface == PHY_INTERFACE_MODE_2500BASEX;

	if (mode_basex) {
		/* 1000BaseX and 2500BaseX ports cannot negotiate speed nor can
		 * they negotiate duplex: they are always operating with a fixed
		 * speed of 1000/2500Mbps in full duplex, so force 1000/2500
		 * speed and full duplex here.
		 */
		ctrl0 |= MVPP2_GMAC_PORT_TYPE_MASK;
		an |= MVPP2_GMAC_CONFIG_GMII_SPEED |
		      MVPP2_GMAC_CONFIG_FULL_DUPLEX;
	} else if (!phy_interface_mode_is_rgmii(state->interface)) {
		an |= MVPP2_GMAC_AN_SPEED_EN | MVPP2_GMAC_FLOW_CTRL_AUTONEG;
	}

	if (state->duplex)
		an |= MVPP2_GMAC_CONFIG_FULL_DUPLEX;
	if (phylink_test(state->advertising, Pause))
		an |= MVPP2_GMAC_FC_ADV_EN;
	if (phylink_test(state->advertising, Asym_Pause))
		an |= MVPP2_GMAC_FC_ADV_ASM_EN;

	if (mode_basex || state->interface == PHY_INTERFACE_MODE_SGMII) {
		an |= MVPP2_GMAC_IN_BAND_AUTONEG;
		ctrl2 |= MVPP2_GMAC_INBAND_AN_MASK | MVPP2_GMAC_PCS_ENABLE_MASK;

		ctrl4 &= ~(MVPP22_CTRL4_EXT_PIN_GMII_SEL |
			   MVPP22_CTRL4_RX_FC_EN | MVPP22_CTRL4_TX_FC_EN);
		ctrl4 |= MVPP22_CTRL4_SYNC_BYPASS_DIS |
			 MVPP22_CTRL4_DP_CLK_SEL |
			 MVPP22_CTRL4_QSGMII_BYPASS_ACTIVE;

		if (state->pause & MLO_PAUSE_TX)
			ctrl4 |= MVPP22_CTRL4_TX_FC_EN;
		if (state->pause & MLO_PAUSE_RX)
			ctrl4 |= MVPP22_CTRL4_RX_FC_EN;
	}

	if (phy_interface_mode_is_rgmii(state->interface)) {
		an |= MVPP2_GMAC_IN_BAND_AUTONEG_BYPASS;

		if (state->duplex)
			an |= MVPP2_GMAC_CONFIG_FULL_DUPLEX;
		if (state->speed == SPEED_1000)
			an |= MVPP2_GMAC_CONFIG_GMII_SPEED;
		else if (state->speed == SPEED_100)
			an |= MVPP2_GMAC_CONFIG_MII_SPEED;

		ctrl4 &= ~MVPP22_CTRL4_DP_CLK_SEL;
		ctrl4 |= MVPP22_CTRL4_EXT_PIN_GMII_SEL |
			 MVPP22_CTRL4_SYNC_BYPASS_DIS |
			 MVPP22_CTRL4_QSGMII_BYPASS_ACTIVE;
	}

	writel(ctrl0, port->base + MVPP2_GMAC_CTRL_0_REG);
	writel(ctrl2, port->base + MVPP2_GMAC_CTRL_2_REG);
	writel(ctrl4, port->base + MVPP22_GMAC_CTRL_4_REG);
	writel(an, port->base + MVPP2_GMAC_AUTONEG_CONFIG);
}

static void mvpp2_mac_config(struct net_device *dev, unsigned int mode,
			     const struct phylink_link_state *state)
{
	struct mvpp2_port *port = netdev_priv(dev);

	/* Check for invalid configuration */
	if (state->interface == PHY_INTERFACE_MODE_10GKR && port->gop_id != 0) {
		netdev_err(dev, "Invalid mode on %s\n", dev->name);
		return;
	}

	/* phylink state-machine depends upon netif_carrier_ok. Don't change it
	 * here, the phylink would set it correctly and call mac_link_up/down.
	 * For quick-stop switch off TX queues instead of the carrier.
	 */
	if (state->link && netif_carrier_ok(dev) && port->has_phy)
		return; /* already in UP */
	mvpp2_tx_stop_all_queues(port->dev);

	/* Make sure the port is disabled when reconfiguring the mode */
	mvpp2_port_disable(port);

	if (port->priv->hw_version != MVPP21 &&
	    port->phy_interface != state->interface) {
		port->phy_interface = state->interface;

		/* Reconfigure the serdes lanes */
		phy_power_off(port->comphy);
		mvpp22_mode_reconfigure(port);
	}

	/* mac (re)configuration */
	if (state->interface == PHY_INTERFACE_MODE_10GKR)
		mvpp2_xlg_config(port, mode, state);
	else if (phy_interface_mode_is_rgmii(state->interface) ||
		 state->interface == PHY_INTERFACE_MODE_SGMII ||
		 state->interface == PHY_INTERFACE_MODE_1000BASEX ||
		 state->interface == PHY_INTERFACE_MODE_2500BASEX)
		mvpp2_gmac_config(port, mode, state);

	if (port->priv->hw_version == MVPP21 && port->flags & MVPP2_F_LOOPBACK)
		mvpp2_port_loopback_set(port, state);

	/* If the port already was up, make sure it's still in the same state */
	if (state->link || !port->has_phy) {
		mvpp2_port_enable(port);

		mvpp2_egress_enable(port);
		mvpp2_ingress_enable(port);
		mvpp2_tx_wake_all_queues(dev);
	}
}

static void mvpp2_mac_link_up(struct net_device *dev, unsigned int mode,
			      phy_interface_t interface, struct phy_device *phy)
{
	struct mvpp2_port *port = netdev_priv(dev);
	u32 val;

	if (!phylink_autoneg_inband(mode) &&
	    interface != PHY_INTERFACE_MODE_10GKR) {
		val = readl(port->base + MVPP2_GMAC_AUTONEG_CONFIG);
		val &= ~MVPP2_GMAC_FORCE_LINK_DOWN;
		if (phy_interface_mode_is_rgmii(port->phy_interface))
			val |= MVPP2_GMAC_FORCE_LINK_PASS;
		writel(val, port->base + MVPP2_GMAC_AUTONEG_CONFIG);
	}

	mvpp2_port_enable(port);

	mvpp2_egress_enable(port);
	mvpp2_ingress_enable(port);
	mvpp2_tx_wake_all_queues(dev);
}

static void mvpp2_mac_link_down(struct net_device *dev, unsigned int mode,
				phy_interface_t interface)
{
	struct mvpp2_port *port = netdev_priv(dev);
	u32 val;

	if (!phylink_autoneg_inband(mode) &&
	    interface != PHY_INTERFACE_MODE_10GKR) {
		val = readl(port->base + MVPP2_GMAC_AUTONEG_CONFIG);
		val &= ~MVPP2_GMAC_FORCE_LINK_PASS;
		val |= MVPP2_GMAC_FORCE_LINK_DOWN;
		writel(val, port->base + MVPP2_GMAC_AUTONEG_CONFIG);
	}

	mvpp2_tx_stop_all_queues(dev);
	mvpp2_egress_disable(port);
	mvpp2_ingress_disable(port);

	/* When using link interrupts to notify phylink of a MAC state change,
	 * we do not want the port to be disabled (we want to receive further
	 * interrupts, to be notified when the port will have a link later).
	 */
	if (!port->has_phy)
		return;

	mvpp2_port_disable(port);
}

static const struct phylink_mac_ops mvpp2_phylink_ops = {
	.validate = mvpp2_phylink_validate,
	.mac_link_state = mvpp2_phylink_mac_link_state,
	.mac_an_restart = mvpp2_mac_an_restart,
	.mac_config = mvpp2_mac_config,
	.mac_link_up = mvpp2_mac_link_up,
	.mac_link_down = mvpp2_mac_link_down,
};

/* Ports initialization */
static int mvpp2_port_probe(struct platform_device *pdev,
			    struct fwnode_handle *port_fwnode,
			    struct mvpp2 *priv)
{
	struct phy *comphy = NULL;
	struct mvpp2_port *port;
	struct mvpp2_port_pcpu *port_pcpu;
	struct device_node *port_node = to_of_node(port_fwnode);
	struct net_device *dev;
	struct resource *res;
	struct phylink *phylink;
	char *mac_from = "";
	unsigned int ntxqs, nrxqs;
	bool has_tx_irqs;
	u32 id;
	int features;
	int phy_mode;
	int err, i, cpu;
	dma_addr_t p;

	if (port_node)
		has_tx_irqs = mvpp2_check_if_multi_irq(priv, port_node);
	else
		has_tx_irqs = true;

	if (port_node && !mvpp2_check_if_multi_irq(priv, port_node) &&
	    queue_mode == MVPP2_QDIST_MULTI_MODE) {
		dev_err(&pdev->dev, "missing IRQ's to support multi queue mode\n");
		return -EINVAL;
	}

	ntxqs = MVPP2_MAX_TXQ;
	if (priv->hw_version != MVPP21 && queue_mode == MVPP2_QDIST_MULTI_MODE)
		nrxqs = MVPP2_DEFAULT_RXQ * num_possible_cpus();
	else
		nrxqs = MVPP2_DEFAULT_RXQ;

	dev = alloc_etherdev_mqs(sizeof(*port), ntxqs, nrxqs);
	if (!dev)
		return -ENOMEM;

	phy_mode = fwnode_get_phy_mode(port_fwnode);
	if (phy_mode < 0) {
		dev_err(&pdev->dev, "incorrect phy mode\n");
		err = phy_mode;
		goto err_free_netdev;
	}

	if (port_node) {
		comphy = devm_of_phy_get(&pdev->dev, port_node, NULL);
		if (IS_ERR(comphy)) {
			if (PTR_ERR(comphy) == -EPROBE_DEFER) {
				err = -EPROBE_DEFER;
				goto err_free_netdev;
			}
			comphy = NULL;
		}
	}

	if (fwnode_property_read_u32(port_fwnode, "port-id", &id)) {
		err = -EINVAL;
		dev_err(&pdev->dev, "missing port-id value\n");
		goto err_free_netdev;
	}

	dev->tx_queue_len = MVPP2_MAX_TXD_MAX;
	dev->watchdog_timeo = 5 * HZ;
	dev->netdev_ops = &mvpp2_netdev_ops;
	dev->ethtool_ops = &mvpp2_eth_tool_ops;

	port = netdev_priv(dev);
	port->dev = dev;
	port->fwnode = port_fwnode;
	port->has_phy = !!of_find_property(port_node, "phy", NULL);
	port->ntxqs = ntxqs;
	port->nrxqs = nrxqs;
	port->priv = priv;
	port->has_tx_irqs = has_tx_irqs;

	err = mvpp2_queue_vectors_init(port, port_node);
	if (err)
		goto err_free_netdev;

	if (port_node)
		port->link_irq = of_irq_get_byname(port_node, "link");
	else
		port->link_irq = fwnode_irq_get(port_fwnode, port->nqvecs + 1);
	if (port->link_irq == -EPROBE_DEFER) {
		err = -EPROBE_DEFER;
		goto err_deinit_qvecs;
	}
	if (port->link_irq <= 0)
		/* the link irq is optional */
		port->link_irq = 0;

	if (fwnode_property_read_bool(port_fwnode, "marvell,loopback"))
		port->flags |= MVPP2_F_LOOPBACK;

	port->id = id;
	if (priv->hw_version == MVPP21)
		port->first_rxq = port->id * port->nrxqs;
	else
		port->first_rxq = port->id * priv->max_port_rxqs;

	port->of_node = port_node;
	port->phy_interface = phy_mode;
	port->comphy = comphy;

	if (priv->hw_version == MVPP21) {
		res = platform_get_resource(pdev, IORESOURCE_MEM, 2 + id);
		port->base = devm_ioremap_resource(&pdev->dev, res);
		if (IS_ERR(port->base)) {
			err = PTR_ERR(port->base);
			goto err_free_irq;
		}

		port->stats_base = port->priv->lms_base +
				   MVPP21_MIB_COUNTERS_OFFSET +
				   port->gop_id * MVPP21_MIB_COUNTERS_PORT_SZ;
	} else {
		if (fwnode_property_read_u32(port_fwnode, "gop-port-id",
					     &port->gop_id)) {
			err = -EINVAL;
			dev_err(&pdev->dev, "missing gop-port-id value\n");
			goto err_deinit_qvecs;
		}

		port->base = priv->iface_base + MVPP22_GMAC_BASE(port->gop_id);
		port->stats_base = port->priv->iface_base +
				   MVPP22_MIB_COUNTERS_OFFSET +
				   port->gop_id * MVPP22_MIB_COUNTERS_PORT_SZ;
	}

	/* Alloc per-cpu and ethtool stats */
	port->stats = netdev_alloc_pcpu_stats(struct mvpp2_pcpu_stats);
	if (!port->stats) {
		err = -ENOMEM;
		goto err_free_irq;
	}

	p = (dma_addr_t)devm_kcalloc(&pdev->dev,
				     ARRAY_SIZE(mvpp2_ethtool_regs) +
				     L1_CACHE_BYTES,
				     sizeof(u64), GFP_KERNEL);
	if (!p) {
		err = -ENOMEM;
		goto err_free_stats;
	}
	p = (p + ~CACHE_LINE_MASK) & CACHE_LINE_MASK;
	port->ethtool_stats = (void *)p;

	mutex_init(&port->gather_stats_lock);
	INIT_DELAYED_WORK(&port->stats_work, mvpp2_gather_hw_statistics);

	mvpp2_port_copy_mac_addr(dev, priv, port_fwnode, &mac_from);

	port->tx_ring_size = MVPP2_MAX_TXD_DFLT;
	port->rx_ring_size = MVPP2_MAX_RXD_DFLT;
	SET_NETDEV_DEV(dev, &pdev->dev);

	mvpp2_port_init_params(port);
	err = mvpp2_port_init(port);
	if (err < 0) {
		dev_err(&pdev->dev, "failed to init port %d\n", id);
		goto err_free_stats;
	}

	mvpp2_port_periodic_xon_disable(port);

	mvpp2_port_reset(port);

	port->pcpu = alloc_percpu(struct mvpp2_port_pcpu);
	if (!port->pcpu) {
		err = -ENOMEM;
		goto err_free_txq_pcpu;
	}

	/* Init tx-done timer and tasklet */
	if (!port->has_tx_irqs) {
		for (cpu = 0; cpu < used_hifs; cpu++) {
			port_pcpu = per_cpu_ptr(port->pcpu, cpu);

			hrtimer_init(&port_pcpu->tx_done_timer, CLOCK_MONOTONIC,
				     HRTIMER_MODE_REL_PINNED);
			port_pcpu->tx_done_timer.function = mvpp2_hr_timer_cb;
			port_pcpu->tx_done_timer_scheduled = false;

			tasklet_init(&port_pcpu->tx_done_tasklet,
				     mvpp2_tx_proc_cb,
				     (unsigned long)dev);
		}
	}
	/* Init bulk timer and tasklet */
	for (cpu = 0; cpu < used_hifs; cpu++) {
		port_pcpu = per_cpu_ptr(port->pcpu, cpu);
		hrtimer_init(&port_pcpu->bulk_timer,
			     CLOCK_MONOTONIC, HRTIMER_MODE_REL_PINNED);
		port_pcpu->bulk_timer.function = mvpp2_bulk_timer_cb;
		tasklet_init(&port_pcpu->bulk_tasklet,
			     mvpp2_bulk_tasklet_cb, (unsigned long)dev);
	}

	features = NETIF_F_SG | NETIF_F_IP_CSUM | NETIF_F_TSO;
	dev->features = features;
	dev->hw_features |= features | NETIF_F_GRO |
			    NETIF_F_HW_VLAN_CTAG_FILTER;

	if (port->pool_long->id == MVPP2_BM_JUMBO && port->id != 0) {
		dev->features &= ~(NETIF_F_IP_CSUM | NETIF_F_IPV6_CSUM);
		dev->hw_features &= ~(NETIF_F_IP_CSUM | NETIF_F_IPV6_CSUM);
	} else {
		dev->features |= NETIF_F_IP_CSUM | NETIF_F_IPV6_CSUM;
		dev->hw_features |= NETIF_F_IP_CSUM | NETIF_F_IPV6_CSUM;
	}

	dev->vlan_features |= features;
	dev->gso_max_segs = MVPP2_MAX_TSO_SEGS;
	dev->priv_flags |= IFF_UNICAST_FLT;

	/* MTU range: 68 - 9704 */
	dev->min_mtu = ETH_MIN_MTU;
	/* 9704 == 9728 - 20 and rounding to 8 */
	dev->max_mtu = MVPP2_BM_JUMBO_PKT_SIZE;

	if (mvpp22_rss_is_supported(port))
		dev->hw_features |= NETIF_F_RXHASH;

	/* Phylink isn't used w/ ACPI as of now */
	if (port_node) {
		phylink = phylink_create(dev, port_fwnode, phy_mode,
					 &mvpp2_phylink_ops);
		if (IS_ERR(phylink)) {
			err = PTR_ERR(phylink);
			goto err_free_port_pcpu;
		}
		port->phylink = phylink;
	} else {
		port->phylink = NULL;
	}

	err = register_netdev(dev);
	if (err < 0) {
		dev_err(&pdev->dev, "failed to register netdev\n");
		goto err_phylink;
	}
	netdev_info(dev, "Using %s mac address %pM\n", mac_from, dev->dev_addr);

	priv->port_list[priv->port_count++] = port;

	return 0;

err_phylink:
	if (port->phylink)
		phylink_destroy(port->phylink);
err_free_port_pcpu:
	free_percpu(port->pcpu);
err_free_txq_pcpu:
	for (i = 0; i < port->ntxqs; i++)
		free_percpu(port->txqs[i]->pcpu);
err_free_stats:
	free_percpu(port->stats);
err_free_irq:
	if (port->link_irq)
		irq_dispose_mapping(port->link_irq);
err_deinit_qvecs:
	mvpp2_queue_vectors_deinit(port);
err_free_netdev:
	free_netdev(dev);
	return err;
}

/* Ports removal routine */
static void mvpp2_port_remove(struct mvpp2_port *port)
{
	int i;

	mvpp2_port_musdk_set(port->dev, false);
	unregister_netdev(port->dev);
	if (port->phylink)
		phylink_destroy(port->phylink);
	free_percpu(port->pcpu);
	free_percpu(port->stats);
	for (i = 0; i < port->ntxqs; i++)
		free_percpu(port->txqs[i]->pcpu);
	mvpp2_queue_vectors_deinit(port);
	if (port->link_irq)
		irq_dispose_mapping(port->link_irq);
	free_netdev(port->dev);
}

/* Initialize decoding windows */
static void mvpp2_conf_mbus_windows(const struct mbus_dram_target_info *dram,
				    struct mvpp2 *priv)
{
	u32 win_enable;
	int i;

	for (i = 0; i < 6; i++) {
		mvpp2_write(priv, MVPP2_WIN_BASE(i), 0);
		mvpp2_write(priv, MVPP2_WIN_SIZE(i), 0);

		if (i < 4)
			mvpp2_write(priv, MVPP2_WIN_REMAP(i), 0);
	}

	win_enable = 0;

	for (i = 0; i < dram->num_cs; i++) {
		const struct mbus_dram_window *cs = dram->cs + i;

		mvpp2_write(priv, MVPP2_WIN_BASE(i),
			    (cs->base & 0xffff0000) | (cs->mbus_attr << 8) |
			    dram->mbus_dram_target_id);

		mvpp2_write(priv, MVPP2_WIN_SIZE(i),
			    (cs->size - 1) & 0xffff0000);

		win_enable |= (1 << i);
	}

	mvpp2_write(priv, MVPP2_BASE_ADDR_ENABLE, win_enable);
}

/* Initialize Rx FIFO's */
static void mvpp2_rx_fifo_init(struct mvpp2 *priv)
{
	int port;

	for (port = 0; port < MVPP2_MAX_PORTS; port++) {
		mvpp2_write(priv, MVPP2_RX_DATA_FIFO_SIZE_REG(port),
			    MVPP2_RX_FIFO_PORT_DATA_SIZE_4KB);
		mvpp2_write(priv, MVPP2_RX_ATTR_FIFO_SIZE_REG(port),
			    MVPP2_RX_FIFO_PORT_ATTR_SIZE_4KB);
	}

	mvpp2_write(priv, MVPP2_RX_MIN_PKT_SIZE_REG,
		    MVPP2_RX_FIFO_PORT_MIN_PKT);
	mvpp2_write(priv, MVPP2_RX_FIFO_INIT_REG, 0x1);
}

static void mvpp22_rx_fifo_init(struct mvpp2 *priv)
{
	int port;

	/* The FIFO size parameters are set depending on the maximum speed a
	 * given port can handle:
	 * - Port 0: 10Gbps
	 * - Port 1: 2.5Gbps
	 * - Ports 2 and 3: 1Gbps
	 */

	mvpp2_write(priv, MVPP2_RX_DATA_FIFO_SIZE_REG(0),
		    MVPP2_RX_FIFO_PORT_DATA_SIZE_32KB);
	mvpp2_write(priv, MVPP2_RX_ATTR_FIFO_SIZE_REG(0),
		    MVPP2_RX_FIFO_PORT_ATTR_SIZE_32KB);

	mvpp2_write(priv, MVPP2_RX_DATA_FIFO_SIZE_REG(1),
		    MVPP2_RX_FIFO_PORT_DATA_SIZE_8KB);
	mvpp2_write(priv, MVPP2_RX_ATTR_FIFO_SIZE_REG(1),
		    MVPP2_RX_FIFO_PORT_ATTR_SIZE_8KB);

	for (port = 2; port < MVPP2_MAX_PORTS; port++) {
		mvpp2_write(priv, MVPP2_RX_DATA_FIFO_SIZE_REG(port),
			    MVPP2_RX_FIFO_PORT_DATA_SIZE_4KB);
		mvpp2_write(priv, MVPP2_RX_ATTR_FIFO_SIZE_REG(port),
			    MVPP2_RX_FIFO_PORT_ATTR_SIZE_4KB);
	}

	mvpp2_write(priv, MVPP2_RX_MIN_PKT_SIZE_REG,
		    MVPP2_RX_FIFO_PORT_MIN_PKT);
	mvpp2_write(priv, MVPP2_RX_FIFO_INIT_REG, 0x1);
}

/* Initialize Tx FIFO's
 * The CP110's total tx-fifo size is 19kB.
 * Use large-size 10kB for fast port but 3kB for others.
 */
static void mvpp22_tx_fifo_init(struct mvpp2 *priv)
{
	int port, size, thrs;

	for (port = 0; port < MVPP2_MAX_PORTS; port++) {
		if (port == 0) {
			size = MVPP22_TX_FIFO_DATA_SIZE_10KB;
			thrs = MVPP2_TX_FIFO_THRESHOLD_10KB;
		} else {
			size = MVPP22_TX_FIFO_DATA_SIZE_3KB;
			thrs = MVPP2_TX_FIFO_THRESHOLD_3KB;
		}
		mvpp2_write(priv, MVPP22_TX_FIFO_SIZE_REG(port), size);
		mvpp2_write(priv, MVPP22_TX_FIFO_THRESH_REG(port), thrs);
	}
}

static void mvpp2_axi_init(struct mvpp2 *priv)
{
	u32 val, rdval, wrval;

	mvpp2_write(priv, MVPP22_BM_ADDR_HIGH_RLS_REG, 0x0);

	/* AXI Bridge Configuration */

	rdval = MVPP22_AXI_CODE_CACHE_RD_CACHE
		<< MVPP22_AXI_ATTR_CACHE_OFFS;
	rdval |= MVPP22_AXI_CODE_DOMAIN_OUTER_DOM
		<< MVPP22_AXI_ATTR_DOMAIN_OFFS;

	wrval = MVPP22_AXI_CODE_CACHE_WR_CACHE
		<< MVPP22_AXI_ATTR_CACHE_OFFS;
	wrval |= MVPP22_AXI_CODE_DOMAIN_OUTER_DOM
		<< MVPP22_AXI_ATTR_DOMAIN_OFFS;

	/* BM */
	mvpp2_write(priv, MVPP22_AXI_BM_WR_ATTR_REG, wrval);
	mvpp2_write(priv, MVPP22_AXI_BM_RD_ATTR_REG, rdval);

	/* Descriptors */
	mvpp2_write(priv, MVPP22_AXI_AGGRQ_DESCR_RD_ATTR_REG, rdval);
	mvpp2_write(priv, MVPP22_AXI_TXQ_DESCR_WR_ATTR_REG, wrval);
	mvpp2_write(priv, MVPP22_AXI_TXQ_DESCR_RD_ATTR_REG, rdval);
	mvpp2_write(priv, MVPP22_AXI_RXQ_DESCR_WR_ATTR_REG, wrval);

	/* Buffer Data */
	mvpp2_write(priv, MVPP22_AXI_TX_DATA_RD_ATTR_REG, rdval);
	mvpp2_write(priv, MVPP22_AXI_RX_DATA_WR_ATTR_REG, wrval);

	val = MVPP22_AXI_CODE_CACHE_NON_CACHE
		<< MVPP22_AXI_CODE_CACHE_OFFS;
	val |= MVPP22_AXI_CODE_DOMAIN_SYSTEM
		<< MVPP22_AXI_CODE_DOMAIN_OFFS;
	mvpp2_write(priv, MVPP22_AXI_RD_NORMAL_CODE_REG, val);
	mvpp2_write(priv, MVPP22_AXI_WR_NORMAL_CODE_REG, val);

	val = MVPP22_AXI_CODE_CACHE_RD_CACHE
		<< MVPP22_AXI_CODE_CACHE_OFFS;
	val |= MVPP22_AXI_CODE_DOMAIN_OUTER_DOM
		<< MVPP22_AXI_CODE_DOMAIN_OFFS;

	mvpp2_write(priv, MVPP22_AXI_RD_SNOOP_CODE_REG, val);

	val = MVPP22_AXI_CODE_CACHE_WR_CACHE
		<< MVPP22_AXI_CODE_CACHE_OFFS;
	val |= MVPP22_AXI_CODE_DOMAIN_OUTER_DOM
		<< MVPP22_AXI_CODE_DOMAIN_OFFS;

	mvpp2_write(priv, MVPP22_AXI_WR_SNOOP_CODE_REG, val);
}

/* Initialize network controller common part HW */
static int mvpp2_init(struct platform_device *pdev, struct mvpp2 *priv)
{
	const struct mbus_dram_target_info *dram_target_info;
	int err, i;
	u32 val;
	dma_addr_t p;

	/* MBUS windows configuration */
	dram_target_info = mv_mbus_dram_info();
	if (dram_target_info)
		mvpp2_conf_mbus_windows(dram_target_info, priv);

	if (priv->hw_version != MVPP21)
		mvpp2_axi_init(priv);

	/* Disable HW PHY polling */
	if (priv->hw_version == MVPP21) {
		val = readl(priv->lms_base + MVPP2_PHY_AN_CFG0_REG);
		val |= MVPP2_PHY_AN_STOP_SMI0_MASK;
		writel(val, priv->lms_base + MVPP2_PHY_AN_CFG0_REG);
	} else {
		val = readl(priv->iface_base + MVPP22_SMI_MISC_CFG_REG);
		val &= ~MVPP22_SMI_POLLING_EN;
		writel(val, priv->iface_base + MVPP22_SMI_MISC_CFG_REG);
	}

	/* Allocate and initialize aggregated TXQs
	 * The aggr_txqs[per-cpu] entry should be aligned onto cache.
	 * So allocate more than needed and round-up the pointer.
	 */
	val = sizeof(*priv->aggr_txqs) * used_hifs + L1_CACHE_BYTES;
	p = (dma_addr_t)devm_kcalloc(&pdev->dev, used_hifs, val,
				       GFP_KERNEL);
	if (!p)
		return -ENOMEM;
	p = (p + ~CACHE_LINE_MASK) & CACHE_LINE_MASK;
	priv->aggr_txqs = (struct mvpp2_tx_queue *)p;

	for (i = 0; i < used_hifs; i++) {
		priv->aggr_txqs[i].id = i;
		priv->aggr_txqs[i].size = MVPP2_AGGR_TXQ_SIZE;
		err = mvpp2_aggr_txq_init(pdev, &priv->aggr_txqs[i], i, priv);
		if (err < 0)
			return err;
	}

	/* Fifo Init */
	if (priv->hw_version == MVPP21) {
		mvpp2_rx_fifo_init(priv);
	} else {
		mvpp22_rx_fifo_init(priv);
		mvpp22_tx_fifo_init(priv);
	}

	if (priv->hw_version == MVPP21)
		writel(MVPP2_EXT_GLOBAL_CTRL_DEFAULT,
		       priv->lms_base + MVPP2_MNG_EXTENDED_GLOBAL_CTRL_REG);

	/* Allow cache snoop when transmiting packets */
	mvpp2_write(priv, MVPP2_TX_SNOOP_REG, 0x1);

	/* Buffer Manager initialization */
	err = mvpp2_bm_init(pdev, priv);
	if (err < 0)
		goto end;

	/* Parser/ClassifierS default initialization */
	mvpp2_prs_flow_id_attr_init();
	err = mvpp2_prs_default_init(pdev, priv);
	if (err < 0)
		goto end;
	err = mvpp2_cls_init(pdev, priv);
	if (err < 0)
		goto end;
	err = mvpp2_c2_init(pdev, priv);
	if (err < 0)
		goto end;

	/* Disable all existing ingress queues */
	mvpp2_rxq_disable_all(priv);
end:
	return err;
}

static int mvpp2_probe(struct platform_device *pdev)
{
	const struct acpi_device_id *acpi_id;
	struct fwnode_handle *fwnode = pdev->dev.fwnode;
	struct fwnode_handle *port_fwnode;
	struct mvpp2 *priv;
	struct resource *res;
	void __iomem *base;
	int i;
	int err;

	priv = devm_kzalloc(&pdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	if (has_acpi_companion(&pdev->dev)) {
		acpi_id = acpi_match_device(pdev->dev.driver->acpi_match_table,
					    &pdev->dev);
		priv->hw_version = (unsigned long)acpi_id->driver_data;
	} else {
		priv->hw_version =
			(unsigned long)of_device_get_match_data(&pdev->dev);
	}

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	base = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(base))
		return PTR_ERR(base);

	if (priv->hw_version == MVPP21) {
		res = platform_get_resource(pdev, IORESOURCE_MEM, 1);
		priv->lms_base = devm_ioremap_resource(&pdev->dev, res);
		if (IS_ERR(priv->lms_base))
			return PTR_ERR(priv->lms_base);
		queue_mode = MVPP2_QDIST_SINGLE_MODE;
	} else {
		res = platform_get_resource(pdev, IORESOURCE_MEM, 1);
		if (has_acpi_companion(&pdev->dev)) {
			/* In case the MDIO memory region is declared in
			 * the ACPI, it can already appear as 'in-use'
			 * in the OS. Because it is overlapped by second
			 * region of the network controller, make
			 * sure it is released, before requesting it again.
			 * The care is taken by mvpp2 driver to avoid
			 * concurrent access to this memory region.
			 */
			release_resource(res);
		}
		priv->iface_base = devm_ioremap_resource(&pdev->dev, res);
		if (IS_ERR(priv->iface_base))
			return PTR_ERR(priv->iface_base);
	}

	used_hifs = (queue_mode == MVPP2_SINGLE_RESOURCE_MODE) ?
		    1 : num_online_cpus();

	if (priv->hw_version != MVPP21 && dev_of_node(&pdev->dev)) {
		priv->sysctrl_base =
			syscon_regmap_lookup_by_phandle(pdev->dev.of_node,
							"marvell,system-controller");
		if (IS_ERR(priv->sysctrl_base))
			/* The system controller regmap is optional for dt
			 * compatibility reasons. When not provided, the
			 * configuration of the GoP relies on the
			 * firmware/bootloader.
			 */
			priv->sysctrl_base = NULL;
	}

	mvpp2_setup_bm_pool();

	for (i = 0; i < MVPP2_MAX_THREADS; i++) {
		u32 addr_space_sz;

		addr_space_sz = (priv->hw_version == MVPP21 ?
				 MVPP21_ADDR_SPACE_SZ : MVPP22_ADDR_SPACE_SZ);
		priv->swth_base[i] = base + i * addr_space_sz;
	}

	if (priv->hw_version == MVPP21)
		priv->max_port_rxqs = 8;
	else
		priv->max_port_rxqs = 32;

	if (dev_of_node(&pdev->dev)) {
		priv->pp_clk = devm_clk_get(&pdev->dev, "pp_clk");
		if (IS_ERR(priv->pp_clk))
			return PTR_ERR(priv->pp_clk);
		err = clk_prepare_enable(priv->pp_clk);
		if (err < 0)
			return err;

		priv->gop_clk = devm_clk_get(&pdev->dev, "gop_clk");
		if (IS_ERR(priv->gop_clk)) {
			err = PTR_ERR(priv->gop_clk);
			goto err_pp_clk;
		}
		err = clk_prepare_enable(priv->gop_clk);
		if (err < 0)
			goto err_pp_clk;

		if (priv->hw_version != MVPP21) {
			priv->mg_clk = devm_clk_get(&pdev->dev, "mg_clk");
			if (IS_ERR(priv->mg_clk)) {
				err = PTR_ERR(priv->mg_clk);
				goto err_gop_clk;
			}

			err = clk_prepare_enable(priv->mg_clk);
			if (err < 0)
				goto err_gop_clk;
		}

		priv->axi_clk = devm_clk_get(&pdev->dev, "axi_clk");
		if (IS_ERR(priv->axi_clk)) {
			err = PTR_ERR(priv->axi_clk);
			if (err == -EPROBE_DEFER)
				goto err_gop_clk;
			priv->axi_clk = NULL;
		} else {
			err = clk_prepare_enable(priv->axi_clk);
			if (err < 0)
				goto err_gop_clk;
		}

		/* Get system's tclk rate */
		priv->tclk = clk_get_rate(priv->pp_clk);
	} else if (device_property_read_u32(&pdev->dev, "clock-frequency",
					    &priv->tclk)) {
		dev_err(&pdev->dev, "missing clock-frequency value\n");
		return -EINVAL;
	}

	priv->custom_dma_mask = false;
	if (priv->hw_version != MVPP21) {
		/* If dma_mask points to coherent_dma_mask, setting both will
		 * override the value of the other. This is problematic as the
		 * PPv2 driver uses a 32-bit-mask for coherent accesses (txq,
		 * rxq, bm) and a 40-bit mask for all other accesses.
		 */
		if (pdev->dev.dma_mask == &pdev->dev.coherent_dma_mask) {
			pdev->dev.dma_mask = kzalloc(sizeof(*pdev->dev.dma_mask),
						     GFP_KERNEL);
			if (!pdev->dev.dma_mask) {
				err = -ENOMEM;
				goto err_mg_clk;
			}

			priv->custom_dma_mask = true;
		}

		err = dma_set_mask(&pdev->dev, DMA_BIT_MASK(40));
		if (err)
			goto err_dma_mask;

		/* Sadly, the BM pools all share the same register to
		 * store the high 32 bits of their address. So they
		 * must all have the same high 32 bits, which forces
		 * us to restrict coherent memory to DMA_BIT_MASK(32).
		 */
		err = dma_set_coherent_mask(&pdev->dev, DMA_BIT_MASK(32));
		if (err)
			goto err_dma_mask;
	}

	/* Assign the reserved memory region to the device for DMA allocations,
	 * if a memory-region phandle is found.
	 */
	if (dev_of_node(&pdev->dev))
		of_reserved_mem_device_init_by_idx(&pdev->dev,
						   pdev->dev.of_node, 0);

	/* Initialize network controller */
	err = mvpp2_init(pdev, priv);
	if (err < 0) {
		dev_err(&pdev->dev, "failed to initialize controller\n");
		goto err_mem_device;
	}

	if (priv->hw_version != MVPP21)
		mvpp22_rss_rxfh_indir_init(priv);

	/* Initialize ports */
	fwnode_for_each_available_child_node(fwnode, port_fwnode) {
		err = mvpp2_port_probe(pdev, port_fwnode, priv);
		if (err < 0)
			goto err_port_probe;
	}

	if (priv->port_count == 0) {
		dev_err(&pdev->dev, "no ports enabled\n");
		err = -ENODEV;
		goto err_mem_device;
	}

	/* Add datapath locks bitmap */
	if (queue_mode == MVPP2_SINGLE_RESOURCE_MODE)
		priv->spinlocks_bitmap = MV_AGGR_QUEUE_LOCK | MV_BM_LOCK;

	/* Statistics must be gathered regularly because some of them (like
	 * packets counters) are 32-bit registers and could overflow quite
	 * quickly. For instance, a 10Gb link used at full bandwidth with the
	 * smallest packets (64B) will overflow a 32-bit counter in less than
	 * 30 seconds. Then, use a workqueue to fill 64-bit counters.
	 */
	snprintf(priv->queue_name, sizeof(priv->queue_name),
		 "stats-wq-%s%s", netdev_name(priv->port_list[0]->dev),
		 priv->port_count > 1 ? "+" : "");
	priv->stats_queue = create_singlethread_workqueue(priv->queue_name);
	if (!priv->stats_queue) {
		err = -ENOMEM;
		goto err_port_probe;
	}

	platform_set_drvdata(pdev, priv);
	mvpp2_share.num_cp++;
	return 0;

err_port_probe:
	i = 0;
	fwnode_for_each_available_child_node(fwnode, port_fwnode) {
		if (priv->port_list[i])
			mvpp2_port_remove(priv->port_list[i]);
		i++;
	}
err_mem_device:
	of_reserved_mem_device_release(&pdev->dev);
err_dma_mask:
	if (priv->custom_dma_mask) {
		kfree(pdev->dev.dma_mask);
		pdev->dev.dma_mask = &pdev->dev.coherent_dma_mask;
	}
err_mg_clk:
	clk_disable_unprepare(priv->axi_clk);
	if (priv->hw_version != MVPP21)
		clk_disable_unprepare(priv->mg_clk);
err_gop_clk:
	clk_disable_unprepare(priv->gop_clk);
err_pp_clk:
	clk_disable_unprepare(priv->pp_clk);
	return err;
}

static int mvpp2_remove(struct platform_device *pdev)
{
	struct mvpp2 *priv = platform_get_drvdata(pdev);
	struct fwnode_handle *fwnode = pdev->dev.fwnode;
	struct fwnode_handle *port_fwnode;
	int i = 0;

	flush_workqueue(priv->stats_queue);
	destroy_workqueue(priv->stats_queue);

	fwnode_for_each_available_child_node(fwnode, port_fwnode) {
		if (priv->port_list[i]) {
			mutex_destroy(&priv->port_list[i]->gather_stats_lock);
			mvpp2_port_remove(priv->port_list[i]);
		}
		i++;
	}

	for (i = 0; i < MVPP2_BM_POOLS_NUM; i++) {
		struct mvpp2_bm_pool *bm_pool = &priv->bm_pools[i];

		mvpp2_bm_pool_destroy(pdev, priv, bm_pool);
	}

	for (i = 0; i < used_hifs; i++) {
		struct mvpp2_tx_queue *aggr_txq = &priv->aggr_txqs[i];

		dma_free_coherent(&pdev->dev,
				  MVPP2_AGGR_TXQ_SIZE * MVPP2_DESC_ALIGNED_SIZE,
				  aggr_txq->descs,
				  aggr_txq->descs_dma);
	}

	if (priv->custom_dma_mask) {
		kfree(pdev->dev.dma_mask);
		pdev->dev.dma_mask = &pdev->dev.coherent_dma_mask;
	}

	if (is_acpi_node(port_fwnode))
		return 0;

	clk_disable_unprepare(priv->axi_clk);
	clk_disable_unprepare(priv->mg_clk);
	clk_disable_unprepare(priv->pp_clk);
	clk_disable_unprepare(priv->gop_clk);

	return 0;
}

static const struct of_device_id mvpp2_match[] = {
	{
		.compatible = "marvell,armada-375-pp2",
		.data = (void *)MVPP21,
	},
	{
		.compatible = "marvell,armada-7k-pp22",
		.data = (void *)MVPP22,
	},
	{ }
};
MODULE_DEVICE_TABLE(of, mvpp2_match);

static const struct acpi_device_id mvpp2_acpi_match[] = {
	{ "MRVL0110", MVPP22 },
	{ },
};
MODULE_DEVICE_TABLE(acpi, mvpp2_acpi_match);

static struct platform_driver mvpp2_driver = {
	.probe = mvpp2_probe,
	.remove = mvpp2_remove,
	.driver = {
		.name = MVPP2_DRIVER_NAME,
		.of_match_table = mvpp2_match,
		.acpi_match_table = ACPI_PTR(mvpp2_acpi_match),
	},
};

module_platform_driver(mvpp2_driver);

MODULE_DESCRIPTION("Marvell PPv2 Ethernet Driver - www.marvell.com");
MODULE_AUTHOR("Marcin Wojtas <mw@semihalf.com>");
MODULE_LICENSE("GPL v2");
