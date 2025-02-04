/* Freescale backplane driver.
 *   Author: Shaohui Xie <Shaohui.Xie@freescale.com>
 *
 * Copyright 2015 Freescale Semiconductor, Inc.
 *
 * Licensed under the GPL-2 or later.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mii.h>
#include <linux/mdio.h>
#include <linux/ethtool.h>
#include <linux/phy.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/of_net.h>
#include <linux/of_address.h>
#include <linux/of_platform.h>
#include <linux/timer.h>
#include <linux/delay.h>
#include <linux/workqueue.h>

/* XFI PCS Device Identifier */
#define FSL_PCS_PHY_ID				0x0083e400

/* Freescale XFI PCS registers */
#define FSL_XFI_PCS_SR1				0x1
#define FSL_PCS_RX_LINK_STAT_MASK		0x4

/* Freescale KR PMD registers */
#define FSL_KR_PMD_CTRL				0x96
#define FSL_KR_PMD_STATUS			0x97
#define FSL_KR_LP_CU				0x98
#define FSL_KR_LP_STATUS			0x99
#define FSL_KR_LD_CU				0x9a
#define FSL_KR_LD_STATUS			0x9b

/* Freescale KR PMD defines */
#define PMD_RESET				0x1
#define PMD_STATUS_SUP_STAT			0x4
#define PMD_STATUS_FRAME_LOCK			0x2
#define TRAIN_EN				0x3
#define TRAIN_DISABLE				0x1
#define RX_STAT					0x1

#define FSL_KR_RX_LINK_STAT_MASK		0x1000
#define FSL_XFI_PCS_10GR_SR1                    0x20

/* Freescale KX PCS mode register */
#define FSL_PCS_IF_MODE				0x8014

/* Freescale KX PCS mode register init value */
#define IF_MODE_INIT				0x8

/* Freescale KX/KR AN registers */
#define FSL_AN_AD1				0x11
#define FSL_AN_BP_STAT				0x30

/* Freescale KX/KR AN registers defines */
#define AN_CTRL_INIT				0x1200
#define KX_AN_AD1_INIT				0x25
#define KR_AN_AD1_INIT				0x85
#define AN_LNK_UP_MASK				0x4
#define KR_AN_MASK				0x8
#define TRAIN_FAIL				0x8

/* C(-1) */
#define BIN_M1					0
/* C(1) */
#define BIN_LONG				1
#define BIN_M1_SEL				6
#define BIN_Long_SEL				7
#define CDR_SEL_MASK				0x00070000
#define BIN_SNAPSHOT_NUM			5
#define BIN_M1_THRESHOLD			3
#define BIN_LONG_THRESHOLD			2

#define PRE_COE_SHIFT				22
#define POST_COE_SHIFT				16
#define ZERO_COE_SHIFT				8

#define PRE_COE_MAX				0x0
#define PRE_COE_MIN				0x8
#define POST_COE_MAX				0x0
#define POST_COE_MIN				0x10
#define ZERO_COE_MAX				0x30
#define ZERO_COE_MIN				0x0

#define TECR0_INIT				0x24200000
#define RATIO_PREQ				0x3
#define RATIO_PST1Q				0xd
#define RATIO_EQ				0x20

#define GCR0_RESET_MASK				0x600000
#define GCR1_SNP_START_MASK			0x00000040
#define GCR1_CTL_SNP_START_MASK			0x00002000
#define GCR1_REIDL_TH_MASK			0x00700000
#define GCR1_REIDL_EX_SEL_MASK			0x000c0000
#define GCR1_REIDL_ET_MAS_MASK			0x00004000
#define TECR0_AMP_RED_MASK			0x0000003f

#define RECR1_CTL_SNP_DONE_MASK			0x00000002
#define RECR1_SNP_DONE_MASK			0x00000004
#define TCSR1_SNP_DATA_MASK			0x0000ffc0
#define TCSR1_SNP_DATA_SHIFT			6
#define TCSR1_EQ_SNPBIN_SIGN_MASK		0x100

#define RECR1_GAINK2_MASK			0x0f000000
#define RECR1_GAINK2_SHIFT			24
#define RECR1_GAINK3_MASK			0x000f0000
#define RECR1_GAINK3_SHIFT			16
#define RECR1_OFFSET_MASK			0x00003f80
#define RECR1_OFFSET_SHIFT			7
#define RECR1_BLW_MASK				0x00000f80
#define RECR1_BLW_SHIFT				7
#define EYE_CTRL_SHIFT				12
#define BASE_WAND_SHIFT				10

#define XGKR_TIMEOUT				1050

#define INCREMENT				1
#define DECREMENT				2
#define TIMEOUT_LONG				3
#define TIMEOUT_M1				3

#define RX_READY_MASK				0x8000
#define PRESET_MASK				0x2000
#define INIT_MASK				0x1000
#define COP1_MASK				0x30
#define COP1_SHIFT				4
#define COZ_MASK				0xc
#define COZ_SHIFT				2
#define COM1_MASK				0x3
#define COM1_SHIFT				0
#define REQUEST_MASK				0x3f
#define LD_ALL_MASK			(PRESET_MASK | INIT_MASK | \
					COP1_MASK | COZ_MASK | COM1_MASK)

#define NEW_ALGORITHM_TRAIN_TX
#ifdef	NEW_ALGORITHM_TRAIN_TX
#define	FORCE_INC_COP1_NUMBER			0
#define	FORCE_INC_COM1_NUMBER			1
#endif

#define VAL_INVALID 0xff

static const u32 preq_table[] = {0x0, 0x1, 0x3, 0x5,
				 0x7, 0x9, 0xb, 0xc, VAL_INVALID};
static const u32 pst1q_table[] = {0x0, 0x1, 0x3, 0x5, 0x7,
				  0x9, 0xb, 0xd, 0xf, 0x10, VAL_INVALID};

enum backplane_mode {
	PHY_BACKPLANE_1000BASE_KX,
	PHY_BACKPLANE_10GBASE_KR,
	PHY_BACKPLANE_XFI,
	PHY_BACKPLANE_INVAL
};

enum coe_filed {
	COE_COP1,
	COE_COZ,
	COE_COM
};

enum coe_update {
	COE_NOTUPDATED,
	COE_UPDATED,
	COE_MIN,
	COE_MAX,
	COE_INV
};

enum train_state {
	DETECTING_LP,
	TRAINED,
};

struct per_lane_ctrl_status {
	__be32 gcr0;	/* 0x.000 - General Control Register 0 */
	__be32 gcr1;	/* 0x.004 - General Control Register 1 */
	__be32 gcr2;	/* 0x.008 - General Control Register 2 */
	__be32 resv1;	/* 0x.00C - Reserved */
	__be32 recr0;	/* 0x.010 - Receive Equalization Control Register 0 */
	__be32 recr1;	/* 0x.014 - Receive Equalization Control Register 1 */
	__be32 tecr0;	/* 0x.018 - Transmit Equalization Control Register 0 */
	__be32 resv2;	/* 0x.01C - Reserved */
	__be32 tlcr0;	/* 0x.020 - TTL Control Register 0 */
	__be32 tlcr1;	/* 0x.024 - TTL Control Register 1 */
	__be32 tlcr2;	/* 0x.028 - TTL Control Register 2 */
	__be32 tlcr3;	/* 0x.02C - TTL Control Register 3 */
	__be32 tcsr0;	/* 0x.030 - Test Control/Status Register 0 */
	__be32 tcsr1;	/* 0x.034 - Test Control/Status Register 1 */
	__be32 tcsr2;	/* 0x.038 - Test Control/Status Register 2 */
	__be32 tcsr3;	/* 0x.03C - Test Control/Status Register 3 */
};

struct tx_condition {
	bool bin_m1_late_early;
	bool bin_long_late_early;
	bool bin_m1_stop;
	bool bin_long_stop;
	bool tx_complete;
	bool sent_init;
	int m1_min_max_cnt;
	int long_min_max_cnt;
#ifdef	NEW_ALGORITHM_TRAIN_TX
	int pre_inc;
	int post_inc;
#endif
};

struct fsl_xgkr_inst {
	void *reg_base;
	struct phy_device *phydev;
	struct tx_condition tx_c;
	struct delayed_work xgkr_wk;
	enum train_state state;
	u32 ld_update;
	u32 ld_status;
	u32 ratio_preq;
	u32 ratio_pst1q;
	u32 adpt_eq;
	int bp_mode;
};

static void tx_condition_init(struct tx_condition *tx_c)
{
	tx_c->bin_m1_late_early = true;
	tx_c->bin_long_late_early = false;
	tx_c->bin_m1_stop = false;
	tx_c->bin_long_stop = false;
	tx_c->tx_complete = false;
	tx_c->sent_init = false;
	tx_c->m1_min_max_cnt = 0;
	tx_c->long_min_max_cnt = 0;
#ifdef	NEW_ALGORITHM_TRAIN_TX
	tx_c->pre_inc = FORCE_INC_COM1_NUMBER;
	tx_c->post_inc = FORCE_INC_COP1_NUMBER;
#endif
}

void tune_tecr0(struct fsl_xgkr_inst *inst)
{
	struct per_lane_ctrl_status *reg_base = inst->reg_base;
	u32 val;

	val = TECR0_INIT |
		inst->adpt_eq << ZERO_COE_SHIFT |
		inst->ratio_preq << PRE_COE_SHIFT |
		inst->ratio_pst1q << POST_COE_SHIFT;

	/* reset the lane */
	iowrite32(ioread32(&reg_base->gcr0) & ~GCR0_RESET_MASK,
		    &reg_base->gcr0);
	udelay(1);
	iowrite32(val, &reg_base->tecr0);
	udelay(1);
	/* unreset the lane */
	iowrite32(ioread32(&reg_base->gcr0) | GCR0_RESET_MASK,
		    &reg_base->gcr0);
	udelay(1);
}

static void start_lt(struct phy_device *phydev)
{
	phy_write_mmd(phydev, MDIO_MMD_PMAPMD, FSL_KR_PMD_CTRL, TRAIN_EN);
}

static void stop_lt(struct phy_device *phydev)
{
	phy_write_mmd(phydev, MDIO_MMD_PMAPMD, FSL_KR_PMD_CTRL, TRAIN_DISABLE);
}

static void reset_gcr0(struct fsl_xgkr_inst *inst)
{
	struct per_lane_ctrl_status *reg_base = inst->reg_base;

	iowrite32(ioread32(&reg_base->gcr0) & ~GCR0_RESET_MASK,
		    &reg_base->gcr0);
	udelay(1);
	iowrite32(ioread32(&reg_base->gcr0) | GCR0_RESET_MASK,
		    &reg_base->gcr0);
	udelay(1);
}

void lane_set_1gkx(void *reg)
{
	struct per_lane_ctrl_status *reg_base = reg;
	u32 val;

	/* reset the lane */
	iowrite32(ioread32(&reg_base->gcr0) & ~GCR0_RESET_MASK,
		    &reg_base->gcr0);
	udelay(1);

	/* set gcr1 for 1GKX */
	val = ioread32(&reg_base->gcr1);
	val &= ~(GCR1_REIDL_TH_MASK | GCR1_REIDL_EX_SEL_MASK |
		 GCR1_REIDL_ET_MAS_MASK);
	iowrite32(val, &reg_base->gcr1);
	udelay(1);

	/* set tecr0 for 1GKX */
	val = ioread32(&reg_base->tecr0);
	val &= ~TECR0_AMP_RED_MASK;
	iowrite32(val, &reg_base->tecr0);
	udelay(1);

	/* unreset the lane */
	iowrite32(ioread32(&reg_base->gcr0) | GCR0_RESET_MASK,
		    &reg_base->gcr0);
	udelay(1);
}

static void reset_lt(struct phy_device *phydev)
{
	phy_write_mmd(phydev, MDIO_MMD_PMAPMD, MDIO_CTRL1, PMD_RESET);
	phy_write_mmd(phydev, MDIO_MMD_PMAPMD, FSL_KR_PMD_CTRL, TRAIN_DISABLE);
	phy_write_mmd(phydev, MDIO_MMD_PMAPMD, FSL_KR_LD_CU, 0);
	phy_write_mmd(phydev, MDIO_MMD_PMAPMD, FSL_KR_LD_STATUS, 0);
	phy_write_mmd(phydev, MDIO_MMD_PMAPMD, FSL_KR_PMD_STATUS, 0);
	phy_write_mmd(phydev, MDIO_MMD_PMAPMD, FSL_KR_LP_CU, 0);
	phy_write_mmd(phydev, MDIO_MMD_PMAPMD, FSL_KR_LP_STATUS, 0);
}

static void start_xgkr_state_machine(struct delayed_work *work)
{
	queue_delayed_work(system_power_efficient_wq, work,
			   msecs_to_jiffies(XGKR_TIMEOUT));
}

static void start_xgkr_an(struct phy_device *phydev)
{
	struct fsl_xgkr_inst *inst = phydev->priv;

	if (inst->bp_mode != PHY_BACKPLANE_XFI) {
		reset_lt(phydev);
		phy_write_mmd(phydev, MDIO_MMD_AN, FSL_AN_AD1, KR_AN_AD1_INIT);
		phy_write_mmd(phydev, MDIO_MMD_AN, MDIO_CTRL1, AN_CTRL_INIT);
	}

	/* start state machine*/
	start_xgkr_state_machine(&inst->xgkr_wk);
}

static void start_1gkx_an(struct phy_device *phydev)
{
	phy_write_mmd(phydev, MDIO_MMD_PCS, FSL_PCS_IF_MODE, IF_MODE_INIT);
	phy_write_mmd(phydev, MDIO_MMD_AN, FSL_AN_AD1, KX_AN_AD1_INIT);
	phy_read_mmd(phydev, MDIO_MMD_AN, MDIO_STAT1);
	phy_write_mmd(phydev, MDIO_MMD_AN, MDIO_CTRL1, AN_CTRL_INIT);
}

static void ld_coe_status(struct fsl_xgkr_inst *inst)
{
	phy_write_mmd(inst->phydev, MDIO_MMD_PMAPMD,
		      FSL_KR_LD_STATUS, inst->ld_status);
}

static void ld_coe_update(struct fsl_xgkr_inst *inst)
{
	dev_dbg(&inst->phydev->mdio.dev, "sending request: %x\n", inst->ld_update);
	phy_write_mmd(inst->phydev, MDIO_MMD_PMAPMD,
		      FSL_KR_LD_CU, inst->ld_update);
}

static void init_inst(struct fsl_xgkr_inst *inst, int reset)
{
	if (inst->bp_mode == PHY_BACKPLANE_XFI) {
		reset_gcr0(inst);
		inst->state = DETECTING_LP;
		return;
	}

	if (reset) {
		inst->ratio_preq = RATIO_PREQ;
		inst->ratio_pst1q = RATIO_PST1Q;
		inst->adpt_eq = RATIO_EQ;
		tune_tecr0(inst);
	}

	tx_condition_init(&inst->tx_c);
	inst->state = DETECTING_LP;
	inst->ld_status &= RX_READY_MASK;
	ld_coe_status(inst);
	inst->ld_update = 0;
	inst->ld_status &= ~RX_READY_MASK;
	ld_coe_status(inst);
}

#ifdef	NEW_ALGORITHM_TRAIN_TX
static int get_median_gaink2(u32 *reg)
{
	int gaink2_snap_shot[BIN_SNAPSHOT_NUM];
	u32 rx_eq_snp;
	struct per_lane_ctrl_status *reg_base;
	int timeout;
	int i, j, tmp, pos;

	reg_base = (struct per_lane_ctrl_status *)reg;

	for (i = 0; i < BIN_SNAPSHOT_NUM; i++) {
		/* wait RECR1_CTL_SNP_DONE_MASK has cleared */
		timeout = 100;
		while (ioread32(&reg_base->recr1) &
		       RECR1_CTL_SNP_DONE_MASK) {
			udelay(1);
			timeout--;
			if (timeout == 0)
				break;
		}

		/* start snap shot */
		iowrite32((ioread32(&reg_base->gcr1) |
			    GCR1_CTL_SNP_START_MASK),
			    &reg_base->gcr1);

		/* wait for SNP done */
		timeout = 100;
		while (!(ioread32(&reg_base->recr1) &
		       RECR1_CTL_SNP_DONE_MASK)) {
			udelay(1);
			timeout--;
			if (timeout == 0)
				break;
		}

		/* read and save the snap shot */
		rx_eq_snp = ioread32(&reg_base->recr1);
		gaink2_snap_shot[i] = (rx_eq_snp & RECR1_GAINK2_MASK) >>
					RECR1_GAINK2_SHIFT;

		/* terminate the snap shot by setting GCR1[REQ_CTL_SNP] */
		iowrite32((ioread32(&reg_base->gcr1) &
			    ~GCR1_CTL_SNP_START_MASK),
			    &reg_base->gcr1);
	}

	/* get median of the 5 snap shot */
	for (i = 0; i < BIN_SNAPSHOT_NUM - 1; i++) {
		tmp = gaink2_snap_shot[i];
		pos = i;
		for (j = i + 1; j < BIN_SNAPSHOT_NUM; j++) {
			if (gaink2_snap_shot[j] < tmp) {
				tmp = gaink2_snap_shot[j];
				pos = j;
			}
		}

		gaink2_snap_shot[pos] = gaink2_snap_shot[i];
		gaink2_snap_shot[i] = tmp;
	}

	return gaink2_snap_shot[2];
}
#endif

static bool is_bin_early(int bin_sel, void *reg)
{
	bool early = false;
	int bin_snap_shot[BIN_SNAPSHOT_NUM];
	int i, negative_count = 0;
	struct per_lane_ctrl_status *reg_base = reg;
	int timeout;

	for (i = 0; i < BIN_SNAPSHOT_NUM; i++) {
		/* wait RECR1_SNP_DONE_MASK has cleared */
		timeout = 100;
		while ((ioread32(&reg_base->recr1) & RECR1_SNP_DONE_MASK)) {
			udelay(1);
			timeout--;
			if (timeout == 0)
				break;
		}

		/* set TCSR1[CDR_SEL] to BinM1/BinLong */
		if (bin_sel == BIN_M1) {
			iowrite32((ioread32(&reg_base->tcsr1) &
				    ~CDR_SEL_MASK) | BIN_M1_SEL,
				    &reg_base->tcsr1);
		} else {
			iowrite32((ioread32(&reg_base->tcsr1) &
				    ~CDR_SEL_MASK) | BIN_Long_SEL,
				    &reg_base->tcsr1);
		}

		/* start snap shot */
		iowrite32(ioread32(&reg_base->gcr1) | GCR1_SNP_START_MASK,
			    &reg_base->gcr1);

		/* wait for SNP done */
		timeout = 100;
		while (!(ioread32(&reg_base->recr1) & RECR1_SNP_DONE_MASK)) {
			udelay(1);
			timeout--;
			if (timeout == 0)
				break;
		}

		/* read and save the snap shot */
		bin_snap_shot[i] = (ioread32(&reg_base->tcsr1) &
				TCSR1_SNP_DATA_MASK) >> TCSR1_SNP_DATA_SHIFT;
		if (bin_snap_shot[i] & TCSR1_EQ_SNPBIN_SIGN_MASK)
			negative_count++;

		/* terminate the snap shot by setting GCR1[REQ_CTL_SNP] */
		iowrite32(ioread32(&reg_base->gcr1) & ~GCR1_SNP_START_MASK,
			    &reg_base->gcr1);
	}

	if (((bin_sel == BIN_M1) && (negative_count > BIN_M1_THRESHOLD)) ||
	    ((bin_sel == BIN_LONG && (negative_count > BIN_LONG_THRESHOLD)))) {
		early = true;
	}

	return early;
}

static void train_tx(struct fsl_xgkr_inst *inst)
{
	struct phy_device *phydev = inst->phydev;
	struct tx_condition *tx_c = &inst->tx_c;
	bool bin_m1_early, bin_long_early;
	u32 lp_status, old_ld_update;
	u32 status_cop1, status_coz, status_com1;
	u32 req_cop1, req_coz, req_com1, req_preset, req_init;
	u32 temp;
#ifdef	NEW_ALGORITHM_TRAIN_TX
	u32 median_gaink2;
#endif

recheck:
	if (tx_c->bin_long_stop && tx_c->bin_m1_stop) {
		tx_c->tx_complete = true;
		inst->ld_status |= RX_READY_MASK;
		ld_coe_status(inst);
		/* tell LP we are ready */
		phy_write_mmd(phydev, MDIO_MMD_PMAPMD,
			      FSL_KR_PMD_STATUS, RX_STAT);
		return;
	}

	/* We start by checking the current LP status. If we got any responses,
	 * we can clear up the appropriate update request so that the
	 * subsequent code may easily issue new update requests if needed.
	 */
	lp_status = phy_read_mmd(phydev, MDIO_MMD_PMAPMD, FSL_KR_LP_STATUS) &
				 REQUEST_MASK;
	status_cop1 = (lp_status & COP1_MASK) >> COP1_SHIFT;
	status_coz = (lp_status & COZ_MASK) >> COZ_SHIFT;
	status_com1 = (lp_status & COM1_MASK) >> COM1_SHIFT;

	old_ld_update = inst->ld_update;
	req_cop1 = (old_ld_update & COP1_MASK) >> COP1_SHIFT;
	req_coz = (old_ld_update & COZ_MASK) >> COZ_SHIFT;
	req_com1 = (old_ld_update & COM1_MASK) >> COM1_SHIFT;
	req_preset = old_ld_update & PRESET_MASK;
	req_init = old_ld_update & INIT_MASK;

	/* IEEE802.3-2008, 72.6.10.2.3.1
	 * We may clear PRESET when all coefficients show UPDATED or MAX.
	 */
	if (req_preset) {
		if ((status_cop1 == COE_UPDATED || status_cop1 == COE_MAX) &&
		    (status_coz == COE_UPDATED || status_coz == COE_MAX) &&
		    (status_com1 == COE_UPDATED || status_com1 == COE_MAX)) {
			inst->ld_update &= ~PRESET_MASK;
		}
	}

	/* IEEE802.3-2008, 72.6.10.2.3.2
	 * We may clear INITIALIZE when no coefficients show NOT UPDATED.
	 */
	if (req_init) {
		if (status_cop1 != COE_NOTUPDATED &&
		    status_coz != COE_NOTUPDATED &&
		    status_com1 != COE_NOTUPDATED) {
			inst->ld_update &= ~INIT_MASK;
		}
	}

	/* IEEE802.3-2008, 72.6.10.2.3.2
	 * we send initialize to the other side to ensure default settings
	 * for the LP. Naturally, we should do this only once.
	 */
	if (!tx_c->sent_init) {
		if (!lp_status && !(old_ld_update & (LD_ALL_MASK))) {
			inst->ld_update = INIT_MASK;
			tx_c->sent_init = true;
		}
	}

	/* IEEE802.3-2008, 72.6.10.2.3.3
	 * We set coefficient requests to HOLD when we get the information
	 * about any updates On clearing our prior response, we also update
	 * our internal status.
	 */
	if (status_cop1 != COE_NOTUPDATED) {
		if (req_cop1) {
			inst->ld_update &= ~COP1_MASK;
#ifdef	NEW_ALGORITHM_TRAIN_TX
			if (tx_c->post_inc) {
				if (req_cop1 == INCREMENT &&
				    status_cop1 == COE_MAX) {
					tx_c->post_inc = 0;
					tx_c->bin_long_stop = true;
					tx_c->bin_m1_stop = true;
				} else {
					tx_c->post_inc -= 1;
				}

				ld_coe_update(inst);
				goto recheck;
			}
#endif
			if ((req_cop1 == DECREMENT && status_cop1 == COE_MIN) ||
			    (req_cop1 == INCREMENT && status_cop1 == COE_MAX)) {
				dev_dbg(&inst->phydev->mdio.dev, "COP1 hit limit %s",
					(status_cop1 == COE_MIN) ?
					"DEC MIN" : "INC MAX");
				tx_c->long_min_max_cnt++;
				if (tx_c->long_min_max_cnt >= TIMEOUT_LONG) {
					tx_c->bin_long_stop = true;
					ld_coe_update(inst);
					goto recheck;
				}
			}
		}
	}

	if (status_coz != COE_NOTUPDATED) {
		if (req_coz)
			inst->ld_update &= ~COZ_MASK;
	}

	if (status_com1 != COE_NOTUPDATED) {
		if (req_com1) {
			inst->ld_update &= ~COM1_MASK;
#ifdef	NEW_ALGORITHM_TRAIN_TX
			if (tx_c->pre_inc) {
				if (req_com1 == INCREMENT &&
				    status_com1 == COE_MAX)
					tx_c->pre_inc = 0;
				else
					tx_c->pre_inc -= 1;

				ld_coe_update(inst);
				goto recheck;
			}
#endif
			/* Stop If we have reached the limit for a parameter. */
			if ((req_com1 == DECREMENT && status_com1 == COE_MIN) ||
			    (req_com1 == INCREMENT && status_com1 == COE_MAX)) {
				dev_dbg(&inst->phydev->mdio.dev, "COM1 hit limit %s",
					(status_com1 == COE_MIN) ?
					"DEC MIN" : "INC MAX");
				tx_c->m1_min_max_cnt++;
				if (tx_c->m1_min_max_cnt >= TIMEOUT_M1) {
					tx_c->bin_m1_stop = true;
					ld_coe_update(inst);
					goto recheck;
				}
			}
		}
	}

	if (old_ld_update != inst->ld_update) {
		ld_coe_update(inst);
		/* Redo these status checks and updates until we have no more
		 * changes, to speed up the overall process.
		 */
		goto recheck;
	}

	/* Do nothing if we have pending request. */
	if ((req_coz || req_com1 || req_cop1))
		return;
	else if (lp_status)
		/* No pending request but LP status was not reverted to
		 * not updated.
		 */
		return;

#ifdef	NEW_ALGORITHM_TRAIN_TX
	if (!(inst->ld_update & (PRESET_MASK | INIT_MASK))) {
		if (tx_c->pre_inc) {
			inst->ld_update = INCREMENT << COM1_SHIFT;
			ld_coe_update(inst);
			return;
		}

		if (status_cop1 != COE_MAX) {
			median_gaink2 = get_median_gaink2(inst->reg_base);
			if (median_gaink2 == 0xf) {
				tx_c->post_inc = 1;
			} else {
				/* Gaink2 median lower than "F" */
				tx_c->bin_m1_stop = true;
				tx_c->bin_long_stop = true;
				goto recheck;
			}
		} else {
			/* C1 MAX */
			tx_c->bin_m1_stop = true;
			tx_c->bin_long_stop = true;
			goto recheck;
		}

		if (tx_c->post_inc) {
			inst->ld_update = INCREMENT << COP1_SHIFT;
			ld_coe_update(inst);
			return;
		}
	}
#endif

	/* snapshot and select bin */
	bin_m1_early = is_bin_early(BIN_M1, inst->reg_base);
	bin_long_early = is_bin_early(BIN_LONG, inst->reg_base);

	if (!tx_c->bin_m1_stop && !tx_c->bin_m1_late_early && bin_m1_early) {
		tx_c->bin_m1_stop = true;
		goto recheck;
	}

	if (!tx_c->bin_long_stop &&
	    tx_c->bin_long_late_early && !bin_long_early) {
		tx_c->bin_long_stop = true;
		goto recheck;
	}

	/* IEEE802.3-2008, 72.6.10.2.3.3
	 * We only request coefficient updates when no PRESET/INITIALIZE is
	 * pending. We also only request coefficient updates when the
	 * corresponding status is NOT UPDATED and nothing is pending.
	 */
	if (!(inst->ld_update & (PRESET_MASK | INIT_MASK))) {
		if (!tx_c->bin_long_stop) {
			/* BinM1 correction means changing COM1 */
			if (!status_com1 && !(inst->ld_update & COM1_MASK)) {
				/* Avoid BinM1Late by requesting an
				 * immediate decrement.
				 */
				if (!bin_m1_early) {
					/* request decrement c(-1) */
					temp = DECREMENT << COM1_SHIFT;
					inst->ld_update = temp;
					ld_coe_update(inst);
					tx_c->bin_m1_late_early = bin_m1_early;
					return;
				}
			}

			/* BinLong correction means changing COP1 */
			if (!status_cop1 && !(inst->ld_update & COP1_MASK)) {
				/* Locate BinLong transition point (if any)
				 * while avoiding BinM1Late.
				 */
				if (bin_long_early) {
					/* request increment c(1) */
					temp = INCREMENT << COP1_SHIFT;
					inst->ld_update = temp;
				} else {
					/* request decrement c(1) */
					temp = DECREMENT << COP1_SHIFT;
					inst->ld_update = temp;
				}

				ld_coe_update(inst);
				tx_c->bin_long_late_early = bin_long_early;
			}
			/* We try to finish BinLong before we do BinM1 */
			return;
		}

		if (!tx_c->bin_m1_stop) {
			/* BinM1 correction means changing COM1 */
			if (!status_com1 && !(inst->ld_update & COM1_MASK)) {
				/* Locate BinM1 transition point (if any) */
				if (bin_m1_early) {
					/* request increment c(-1) */
					temp = INCREMENT << COM1_SHIFT;
					inst->ld_update = temp;
				} else {
					/* request decrement c(-1) */
					temp = DECREMENT << COM1_SHIFT;
					inst->ld_update = temp;
				}

				ld_coe_update(inst);
				tx_c->bin_m1_late_early = bin_m1_early;
			}
		}
	}
}

static int is_link_up(struct phy_device *phydev)
{
	struct fsl_xgkr_inst *inst = phydev->priv;
	int val;

	if (phydev->speed == SPEED_10000 && inst->bp_mode == PHY_BACKPLANE_XFI) {
		phy_read_mmd(phydev, MDIO_MMD_PCS, FSL_XFI_PCS_SR1);
		val = phy_read_mmd(phydev, MDIO_MMD_PCS, FSL_XFI_PCS_SR1);
		return (val & FSL_PCS_RX_LINK_STAT_MASK) ? 1 : 0;
	}
	phy_read_mmd(phydev, MDIO_MMD_PCS, FSL_XFI_PCS_10GR_SR1);
	val = phy_read_mmd(phydev, MDIO_MMD_PCS, FSL_XFI_PCS_10GR_SR1);

	return (val & FSL_KR_RX_LINK_STAT_MASK) ? 1 : 0;
}

static int is_link_training_fail(struct phy_device *phydev)
{
	int val;
	int timeout = 100;

	val = phy_read_mmd(phydev, MDIO_MMD_PMAPMD, FSL_KR_PMD_STATUS);
	if (!(val & TRAIN_FAIL) && (val & RX_STAT)) {
		/* check LNK_STAT for sure */
		while (timeout--) {
			if (is_link_up(phydev))
				return 0;

			usleep_range(100, 500);
		}
	}

	return 1;
}

static int check_rx(struct phy_device *phydev)
{
	return phy_read_mmd(phydev, MDIO_MMD_PMAPMD, FSL_KR_LP_STATUS) &
			    RX_READY_MASK;
}

/* Coefficient values have hardware restrictions */
static int is_ld_valid(struct fsl_xgkr_inst *inst)
{
	u32 ratio_pst1q = inst->ratio_pst1q;
	u32 adpt_eq = inst->adpt_eq;
	u32 ratio_preq = inst->ratio_preq;

	if ((ratio_pst1q + adpt_eq + ratio_preq) > 48)
		return 0;

	if (((ratio_pst1q + adpt_eq + ratio_preq) * 4) >=
	    ((adpt_eq - ratio_pst1q - ratio_preq) * 17))
		return 0;

	if (ratio_preq > ratio_pst1q)
		return 0;

	if (ratio_preq > 8)
		return 0;

	if (adpt_eq < 26)
		return 0;

	if (ratio_pst1q > 16)
		return 0;

	return 1;
}

static int is_value_allowed(const u32 *val_table, u32 val)
{
	int i;

	for (i = 0;; i++) {
		if (*(val_table + i) == VAL_INVALID)
			return 0;
		if (*(val_table + i) == val)
			return 1;
	}
}

static int inc_dec(struct fsl_xgkr_inst *inst, int field, int request)
{
	u32 ld_limit[3], ld_coe[3], step[3];

	ld_coe[0] = inst->ratio_pst1q;
	ld_coe[1] = inst->adpt_eq;
	ld_coe[2] = inst->ratio_preq;

	/* Information specific to the Freescale SerDes for 10GBase-KR:
	 * Incrementing C(+1) means *decrementing* RATIO_PST1Q
	 * Incrementing C(0) means incrementing ADPT_EQ
	 * Incrementing C(-1) means *decrementing* RATIO_PREQ
	 */
	step[0] = -1;
	step[1] = 1;
	step[2] = -1;

	switch (request) {
	case INCREMENT:
		ld_limit[0] = POST_COE_MAX;
		ld_limit[1] = ZERO_COE_MAX;
		ld_limit[2] = PRE_COE_MAX;
		if (ld_coe[field] != ld_limit[field])
			ld_coe[field] += step[field];
		else
			/* MAX */
			return 2;
		break;
	case DECREMENT:
		ld_limit[0] = POST_COE_MIN;
		ld_limit[1] = ZERO_COE_MIN;
		ld_limit[2] = PRE_COE_MIN;
		if (ld_coe[field] != ld_limit[field])
			ld_coe[field] -= step[field];
		else
			/* MIN */
			return 1;
		break;
	default:
		break;
	}

	if (is_ld_valid(inst)) {
		/* accept new ld */
		inst->ratio_pst1q = ld_coe[0];
		inst->adpt_eq = ld_coe[1];
		inst->ratio_preq = ld_coe[2];
		/* only some values for preq and pst1q can be used.
		 * for preq: 0x0, 0x1, 0x3, 0x5, 0x7, 0x9, 0xb, 0xc.
		 * for pst1q: 0x0, 0x1, 0x3, 0x5, 0x7, 0x9, 0xb, 0xd, 0xf, 0x10.
		 */
		if (!is_value_allowed((const u32 *)&preq_table, ld_coe[2])) {
			dev_dbg(&inst->phydev->mdio.dev,
				"preq skipped value: %d\n", ld_coe[2]);
			return 0;
		}

		if (!is_value_allowed((const u32 *)&pst1q_table, ld_coe[0])) {
			dev_dbg(&inst->phydev->mdio.dev,
				"pst1q skipped value: %d\n", ld_coe[0]);
			return 0;
		}

		tune_tecr0(inst);
	} else {
		if (request == DECREMENT)
			/* MIN */
			return 1;
		if (request == INCREMENT)
			/* MAX */
			return 2;
	}

	return 0;
}

static void min_max_updated(struct fsl_xgkr_inst *inst, int field, int new_ld)
{
	u32 ld_coe[] = {COE_UPDATED, COE_MIN, COE_MAX};
	u32 mask, val;

	switch (field) {
	case COE_COP1:
		mask = COP1_MASK;
		val = ld_coe[new_ld] << COP1_SHIFT;
		break;
	case COE_COZ:
		mask = COZ_MASK;
		val = ld_coe[new_ld] << COZ_SHIFT;
		break;
	case COE_COM:
		mask = COM1_MASK;
		val = ld_coe[new_ld] << COM1_SHIFT;
		break;
	default:
		return;
	}

	inst->ld_status &= ~mask;
	inst->ld_status |= val;
}

static void check_request(struct fsl_xgkr_inst *inst, int request)
{
	int cop1_req, coz_req, com_req;
	int old_status, new_ld_sta;

	cop1_req = (request & COP1_MASK) >> COP1_SHIFT;
	coz_req = (request & COZ_MASK) >> COZ_SHIFT;
	com_req = (request & COM1_MASK) >> COM1_SHIFT;

	/* IEEE802.3-2008, 72.6.10.2.5
	 * Ensure we only act on INCREMENT/DECREMENT when we are in NOT UPDATED
	 */
	old_status = inst->ld_status;

	if (cop1_req && !(inst->ld_status & COP1_MASK)) {
		new_ld_sta = inc_dec(inst, COE_COP1, cop1_req);
		min_max_updated(inst, COE_COP1, new_ld_sta);
	}

	if (coz_req && !(inst->ld_status & COZ_MASK)) {
		new_ld_sta = inc_dec(inst, COE_COZ, coz_req);
		min_max_updated(inst, COE_COZ, new_ld_sta);
	}

	if (com_req && !(inst->ld_status & COM1_MASK)) {
		new_ld_sta = inc_dec(inst, COE_COM, com_req);
		min_max_updated(inst, COE_COM, new_ld_sta);
	}

	if (old_status != inst->ld_status)
		ld_coe_status(inst);
}

static void preset(struct fsl_xgkr_inst *inst)
{
	/* These are all MAX values from the IEEE802.3 perspective. */
	inst->ratio_pst1q = POST_COE_MAX;
	inst->adpt_eq = ZERO_COE_MAX;
	inst->ratio_preq = PRE_COE_MAX;

	tune_tecr0(inst);
	inst->ld_status &= ~(COP1_MASK | COZ_MASK | COM1_MASK);
	inst->ld_status |= COE_MAX << COP1_SHIFT |
			   COE_MAX << COZ_SHIFT |
			   COE_MAX << COM1_SHIFT;
	ld_coe_status(inst);
}

static void initialize(struct fsl_xgkr_inst *inst)
{
	inst->ratio_preq = RATIO_PREQ;
	inst->ratio_pst1q = RATIO_PST1Q;
	inst->adpt_eq = RATIO_EQ;

	tune_tecr0(inst);
	inst->ld_status &= ~(COP1_MASK | COZ_MASK | COM1_MASK);
	inst->ld_status |= COE_UPDATED << COP1_SHIFT |
			   COE_UPDATED << COZ_SHIFT |
			   COE_UPDATED << COM1_SHIFT;
	ld_coe_status(inst);
}

static void train_rx(struct fsl_xgkr_inst *inst)
{
	struct phy_device *phydev = inst->phydev;
	int request, old_ld_status;

	/* get request from LP */
	request = phy_read_mmd(phydev, MDIO_MMD_PMAPMD, FSL_KR_LP_CU) &
			      (LD_ALL_MASK);
	old_ld_status = inst->ld_status;

	/* IEEE802.3-2008, 72.6.10.2.5
	 * Ensure we always go to NOT UDPATED for status reporting in
	 * response to HOLD requests.
	 * IEEE802.3-2008, 72.6.10.2.3.1/2
	 * ... but only if PRESET/INITIALIZE are not active to ensure
	 * we keep status until they are released.
	 */
	if (!(request & (PRESET_MASK | INIT_MASK))) {
		if (!(request & COP1_MASK))
			inst->ld_status &= ~COP1_MASK;

		if (!(request & COZ_MASK))
			inst->ld_status &= ~COZ_MASK;

		if (!(request & COM1_MASK))
			inst->ld_status &= ~COM1_MASK;

		if (old_ld_status != inst->ld_status)
			ld_coe_status(inst);
	}

	/* As soon as the LP shows ready, no need to do any more updates. */
	if (check_rx(phydev)) {
		/* LP receiver is ready */
		if (inst->ld_status & (COP1_MASK | COZ_MASK | COM1_MASK)) {
			inst->ld_status &= ~(COP1_MASK | COZ_MASK | COM1_MASK);
			ld_coe_status(inst);
		}
	} else {
		/* IEEE802.3-2008, 72.6.10.2.3.1/2
		 * only act on PRESET/INITIALIZE if all status is NOT UPDATED.
		 */
		if (request & (PRESET_MASK | INIT_MASK)) {
			if (!(inst->ld_status &
			      (COP1_MASK | COZ_MASK | COM1_MASK))) {
				if (request & PRESET_MASK)
					preset(inst);

				if (request & INIT_MASK)
					initialize(inst);
			}
		}

		/* LP Coefficient are not in HOLD */
		if (request & REQUEST_MASK)
			check_request(inst, request & REQUEST_MASK);
	}
}

static void xgkr_start_train(struct phy_device *phydev)
{
	struct fsl_xgkr_inst *inst = phydev->priv;
	struct tx_condition *tx_c = &inst->tx_c;
	int val = 0, i;
	int lt_state;
	unsigned long dead_line;
	int rx_ok, tx_ok;

	init_inst(inst, 0);
	start_lt(phydev);

	for (i = 0; i < 2;) {
		dead_line = jiffies + msecs_to_jiffies(500);
		while (time_before(jiffies, dead_line)) {
			val = phy_read_mmd(phydev, MDIO_MMD_PMAPMD,
					   FSL_KR_PMD_STATUS);
			if (val & TRAIN_FAIL) {
				/* LT failed already, reset lane to avoid
				 * it run into hanging, then start LT again.
				 */
				reset_gcr0(inst);
				start_lt(phydev);
			} else if ((val & PMD_STATUS_SUP_STAT) &&
				   (val & PMD_STATUS_FRAME_LOCK))
				break;
			usleep_range(100, 500);
		}

		if (!((val & PMD_STATUS_FRAME_LOCK) &&
		      (val & PMD_STATUS_SUP_STAT))) {
			i++;
			continue;
		}

		/* init process */
		rx_ok = false;
		tx_ok = false;
		/* the LT should be finished in 500ms, failed or OK. */
		dead_line = jiffies + msecs_to_jiffies(500);

		while (time_before(jiffies, dead_line)) {
			/* check if the LT is already failed */
			lt_state = phy_read_mmd(phydev, MDIO_MMD_PMAPMD,
						FSL_KR_PMD_STATUS);
			if (lt_state & TRAIN_FAIL) {
				reset_gcr0(inst);
				break;
			}

			rx_ok = check_rx(phydev);
			tx_ok = tx_c->tx_complete;

			if (rx_ok && tx_ok)
				break;

			if (!rx_ok)
				train_rx(inst);

			if (!tx_ok)
				train_tx(inst);

			usleep_range(100, 500);
		}

		i++;
		/* check LT result */
		if (is_link_training_fail(phydev)) {
			init_inst(inst, 0);
			continue;
		} else {
			stop_lt(phydev);
			inst->state = TRAINED;
			break;
		}
	}
}

static void xgkr_state_machine(struct work_struct *work)
{
	struct delayed_work *dwork = to_delayed_work(work);
	struct fsl_xgkr_inst *inst = container_of(dwork,
						  struct fsl_xgkr_inst,
						  xgkr_wk);
	struct phy_device *phydev = inst->phydev;
	int an_state;
	bool needs_train = false;

	mutex_lock(&phydev->lock);

	switch (inst->state) {
	case DETECTING_LP:
		if (inst->bp_mode == PHY_BACKPLANE_XFI) {
			if (is_link_up(phydev)) {
				dev_info(&phydev->mdio.dev, "XFI link detected\n");
				inst->state = TRAINED;
			}
		} else {
			phy_read_mmd(phydev, MDIO_MMD_AN, FSL_AN_BP_STAT);
			an_state = phy_read_mmd(phydev, MDIO_MMD_AN, FSL_AN_BP_STAT);
			if ((an_state & KR_AN_MASK))
				needs_train = true;
		}
		break;
	case TRAINED:
		if (!is_link_up(phydev)) {
			dev_info(&phydev->mdio.dev,
				 "Detect hotplug, restart training\n");
			init_inst(inst, 1);
			if (inst->bp_mode != PHY_BACKPLANE_XFI)
				start_xgkr_an(phydev);
			inst->state = DETECTING_LP;
		}
		break;
	}

	if (needs_train)
		xgkr_start_train(phydev);

	mutex_unlock(&phydev->lock);
	queue_delayed_work(system_power_efficient_wq, &inst->xgkr_wk,
			   msecs_to_jiffies(XGKR_TIMEOUT));
}

static int fsl_backplane_probe(struct phy_device *phydev)
{
	struct fsl_xgkr_inst *xgkr_inst;
	struct device_node *phy_node, *lane_node;
	struct resource res_lane;
	const char *bm;
	int ret;
	int bp_mode;
	u32 lane[2];

	phy_node = phydev->mdio.dev.of_node;
	bp_mode = of_property_read_string(phy_node, "backplane-mode", &bm);
	if (bp_mode < 0)
		return 0;

	if (!strcasecmp(bm, "1000base-kx")) {
		bp_mode = PHY_BACKPLANE_1000BASE_KX;
	} else if (!strcasecmp(bm, "10gbase-kr")) {
		bp_mode = PHY_BACKPLANE_10GBASE_KR;
	} else if (!strcasecmp(bm, "xfi")) {
		bp_mode = PHY_BACKPLANE_XFI;
	} else {
		dev_err(&phydev->mdio.dev, "Unknown backplane-mode\n");
		return -EINVAL;
	}

	lane_node = of_parse_phandle(phy_node, "fsl,lane-handle", 0);
	if (!lane_node) {
		dev_err(&phydev->mdio.dev, "parse fsl,lane-handle failed\n");
		return -EINVAL;
	}

	ret = of_address_to_resource(lane_node, 0, &res_lane);
	if (ret) {
		dev_err(&phydev->mdio.dev, "could not obtain memory map\n");
		return ret;
	}

	of_node_put(lane_node);
	ret = of_property_read_u32_array(phy_node, "fsl,lane-reg",
					 (u32 *)&lane, 2);
	if (ret) {
		dev_err(&phydev->mdio.dev, "could not get fsl,lane-reg\n");
		return -EINVAL;
	}

	phydev->priv = devm_ioremap_nocache(&phydev->mdio.dev,
					    res_lane.start + lane[0],
					    lane[1]);
	if (!phydev->priv) {
		dev_err(&phydev->mdio.dev, "ioremap_nocache failed\n");
		return -ENOMEM;
	}

	if (bp_mode == PHY_BACKPLANE_1000BASE_KX) {
		phydev->speed = SPEED_1000;
		/* configure the lane for 1000BASE-KX */
		lane_set_1gkx(phydev->priv);
		return 0;
	}

	xgkr_inst = devm_kzalloc(&phydev->mdio.dev,
				 sizeof(*xgkr_inst), GFP_KERNEL);
	if (!xgkr_inst)
		return -ENOMEM;

	xgkr_inst->reg_base = phydev->priv;
	xgkr_inst->phydev = phydev;
	xgkr_inst->bp_mode = bp_mode;
	phydev->priv = xgkr_inst;
	phydev->link = 0;

	if (bp_mode == PHY_BACKPLANE_10GBASE_KR || bp_mode == PHY_BACKPLANE_XFI) {
		phydev->speed = SPEED_10000;
		INIT_DELAYED_WORK(&xgkr_inst->xgkr_wk, xgkr_state_machine);
	}

	dev_info(&phydev->mdio.dev, "probed\n");

	return 0;
}

static int fsl_backplane_aneg_done(struct phy_device *phydev)
{
	return 1;
}

static int fsl_backplane_config_aneg(struct phy_device *phydev)
{
	if (phydev->speed == SPEED_10000) {
		struct fsl_xgkr_inst *inst = phydev->priv;
		phydev->supported |= SUPPORTED_10000baseKR_Full;
		if (inst->bp_mode == PHY_BACKPLANE_XFI)
			init_inst(inst, 0);
		else
			start_xgkr_an(phydev);
	} else if (phydev->speed == SPEED_1000) {
		phydev->supported |= SUPPORTED_1000baseKX_Full;
		start_1gkx_an(phydev);
	}

	phydev->advertising = phydev->supported;
	phydev->duplex = 1;

	return 0;
}

static int fsl_backplane_suspend(struct phy_device *phydev)
{
	if (phydev->speed == SPEED_10000) {
		struct fsl_xgkr_inst *xgkr_inst = phydev->priv;

		cancel_delayed_work_sync(&xgkr_inst->xgkr_wk);
	}
	return 0;
}

static int fsl_backplane_resume(struct phy_device *phydev)
{
	if (phydev->speed == SPEED_10000) {
		struct fsl_xgkr_inst *xgkr_inst = phydev->priv;

		init_inst(xgkr_inst, 1);
		queue_delayed_work(system_power_efficient_wq,
				   &xgkr_inst->xgkr_wk,
				   msecs_to_jiffies(XGKR_TIMEOUT));
	}
	return 0;
}

static int fsl_backplane_read_status(struct phy_device *phydev)
{
	if (is_link_up(phydev))
		phydev->link = 1;
	else
		phydev->link = 0;

	return 0;
}

static struct phy_driver fsl_backplane_driver[] = {
	{
	.phy_id		= FSL_PCS_PHY_ID,
	.name		= "Freescale Backplane",
	.phy_id_mask	= 0xffffffff,
	.features	= SUPPORTED_Backplane | SUPPORTED_Autoneg |
			  SUPPORTED_MII,
	.probe          = fsl_backplane_probe,
	.aneg_done      = fsl_backplane_aneg_done,
	.config_aneg	= fsl_backplane_config_aneg,
	.read_status	= fsl_backplane_read_status,
	.suspend	= fsl_backplane_suspend,
	.resume		= fsl_backplane_resume,
	},
};

module_phy_driver(fsl_backplane_driver);

static struct mdio_device_id __maybe_unused freescale_tbl[] = {
	{ FSL_PCS_PHY_ID, 0xffffffff },
	{ }
};

MODULE_DEVICE_TABLE(mdio, freescale_tbl);

MODULE_DESCRIPTION("Freescale Backplane driver");
MODULE_AUTHOR("Shaohui Xie <Shaohui.Xie@freescale.com>");
MODULE_LICENSE("GPL v2");
