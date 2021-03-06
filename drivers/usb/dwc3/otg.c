/**
 * otg.c - DesignWare USB3 DRD Controller OTG
 *
 * Copyright (c) 2012, Code Aurora Forum. All rights reserved.
 * Copyright (c) 2013 Samsung Electronics Co., Ltd.
 *		http://www.samsung.com
 *
 * Authors: Ido Shayevitz <idos@codeaurora.org>
 *	    Anton Tikhomirov <av.tikhomirov@samsung.com>
 *	    Minho Lee <minho55.lee@samsung.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2  of
 * the License as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/regulator/consumer.h>
#include <linux/pm_runtime.h>
#include <linux/usb/samsung_usb.h>
#include <linux/mfd/samsung/s2mps18-private.h>
#include <soc/samsung/exynos-pm.h>
#include <linux/kernel.h>

#include "core.h"
#include "otg.h"
#include "io.h"
#if defined(CONFIG_CCIC_MAX77705)
#include <linux/ccic/max77705_usbc.h>
#endif
/* -------------------------------------------------------------------------- */
int otg_connection;

#define	DP_CHECK_PHYOFF_ENABLE

#ifdef DP_CHECK_PHYOFF_ENABLE
#define	DPCHK_TIME_MSECS		5000

struct dp_check_timer_struct {
	int				start;
	struct timer_list	timer;
};

struct dp_check_work_struct {
	struct otg_fsm          *fsm;
	struct work_struct      work;
};
static struct dp_check_work_struct             *dp_check_work;

static int dwc3_otg_start_host_phyoff(struct otg_fsm *fsm, int on);
static struct dp_check_timer_struct		dp_check_timer;

static void dwc3_otg_dp_check_delayed_phyoff(struct otg_fsm	*fsm)
{
	int ret = 0;

	pr_info("%s, b_sess:%d, id: %d\n", __func__, fsm->id, fsm->b_sess_vld);

	otg_drv_vbus(fsm, 0);
	ret = dwc3_otg_start_host_phyoff(fsm, 0);
	if (ret)
		pr_err("OTG SM: cannot stop host\n");
	dp_check_timer.start = 0;
}

static void dwc3_otg_dp_check_usb_work(struct work_struct *w)
{
	struct dp_check_work_struct	*dpwork = container_of(w,
					struct dp_check_work_struct, work);
	struct otg_fsm	*fsm = (struct otg_fsm	*)dpwork->fsm;

	pr_info("%s\n", __func__);
	dwc3_otg_dp_check_delayed_phyoff(fsm);
}

void dwc3_otg_dp_check_timer_handler(unsigned long arg)
{
	struct dp_check_work_struct *dpwork =
		(struct dp_check_work_struct *)arg;

	pr_info("%s\n", __func__);
	schedule_work(&dpwork->work);
}

static void dwc3_otg_dp_check_timer_init(struct dp_check_work_struct *work)
{
	setup_timer(&dp_check_timer.timer,
		    dwc3_otg_dp_check_timer_handler, (unsigned long)work);

	dp_check_timer.timer.expires = jiffies +
			msecs_to_jiffies(DPCHK_TIME_MSECS);

	pr_info("%s\n", __func__);
}
#endif

static int dwc3_otg_statemachine(struct otg_fsm *fsm)
{
	struct usb_otg *otg = fsm->otg;
	enum usb_otg_state prev_state = otg->state;
	int ret = 0;

	if (fsm->reset) {
		if (otg->state == OTG_STATE_A_HOST) {
			otg_drv_vbus(fsm, 0);
			otg_start_host(fsm, 0);
		} else if (otg->state == OTG_STATE_B_PERIPHERAL) {
			otg_start_gadget(fsm, 0);
		}

		otg->state = OTG_STATE_UNDEFINED;
		goto exit;
	}

	switch (otg->state) {
	case OTG_STATE_UNDEFINED:
		if (fsm->id)
			otg->state = OTG_STATE_B_IDLE;
		else
			otg->state = OTG_STATE_A_IDLE;
		break;
	case OTG_STATE_B_IDLE:
		if (!fsm->id) {
			otg->state = OTG_STATE_A_IDLE;
		} else if (fsm->b_sess_vld) {
			ret = otg_start_gadget(fsm, 1);
			if (!ret)
				otg->state = OTG_STATE_B_PERIPHERAL;
			else
				pr_err("OTG SM: cannot start gadget\n");
		}
		break;
	case OTG_STATE_B_PERIPHERAL:
		if (!fsm->id || !fsm->b_sess_vld) {
			ret = otg_start_gadget(fsm, 0);
			if (!ret)
				otg->state = OTG_STATE_B_IDLE;
			else
				pr_err("OTG SM: cannot stop gadget\n");
		}
		break;
	case OTG_STATE_A_IDLE:
		if (fsm->id) {
			otg->state = OTG_STATE_B_IDLE;
		} else {
			ret = otg_start_host(fsm, 1);
			if (!ret) {
				otg_drv_vbus(fsm, 1);
				otg->state = OTG_STATE_A_HOST;
			} else {
				pr_err("OTG SM: cannot start host\n");
			}
		}
		break;
	case OTG_STATE_A_HOST:
		if (fsm->id) {
			otg_drv_vbus(fsm, 0);
			ret = otg_start_host(fsm, 0);
			if (!ret)
				otg->state = OTG_STATE_A_IDLE;
			else
				pr_err("OTG SM: cannot stop host\n");
		}
		break;
	default:
		pr_err("OTG SM: invalid state\n");
	}

exit:
	if (!ret)
		ret = (otg->state != prev_state);

	pr_debug("OTG SM: %s => %s\n", usb_otg_state_string(prev_state),
		(ret > 0) ? usb_otg_state_string(otg->state) : "(no change)");

	return ret;
}

/* -------------------------------------------------------------------------- */

static struct dwc3_ext_otg_ops *dwc3_otg_exynos_rsw_probe(struct dwc3 *dwc)
{
	struct dwc3_ext_otg_ops *ops;
	bool ext_otg;

	ext_otg = dwc3_exynos_rsw_available(dwc->dev->parent);
	if (!ext_otg)
		return NULL;

	/* Allocate and init otg instance */
	ops = devm_kzalloc(dwc->dev, sizeof(struct dwc3_ext_otg_ops),
			GFP_KERNEL);
	if (!ops) {
		 dev_err(dwc->dev, "unable to allocate dwc3_ext_otg_ops\n");
		 return NULL;
	}

	ops->setup = dwc3_exynos_rsw_setup;
	ops->exit = dwc3_exynos_rsw_exit;
	ops->start = dwc3_exynos_rsw_start;
	ops->stop = dwc3_exynos_rsw_stop;

	return ops;
}

static void dwc3_otg_set_host_mode(struct dwc3_otg *dotg)
{
	struct dwc3 *dwc = dotg->dwc;
	u32 reg;

	if (dotg->regs) {
		reg = dwc3_readl(dotg->regs, DWC3_OCTL);
		reg &= ~DWC3_OTG_OCTL_PERIMODE;
		dwc3_writel(dotg->regs, DWC3_OCTL, reg);
	} else {
		dwc3_set_mode(dwc, DWC3_GCTL_PRTCAP_HOST);
	}
}

static void dwc3_otg_set_peripheral_mode(struct dwc3_otg *dotg)
{
	struct dwc3 *dwc = dotg->dwc;
	u32 reg;

	if (dotg->regs) {
		reg = dwc3_readl(dotg->regs, DWC3_OCTL);
		reg |= DWC3_OTG_OCTL_PERIMODE;
		dwc3_writel(dotg->regs, DWC3_OCTL, reg);
	} else {
		dwc3_set_mode(dwc, DWC3_GCTL_PRTCAP_DEVICE);
	}
}

static void dwc3_otg_drv_vbus(struct otg_fsm *fsm, int on)
{
	struct dwc3_otg	*dotg = container_of(fsm, struct dwc3_otg, fsm);
	int ret;

	if (IS_ERR(dotg->vbus_reg)) {
		dev_err(dotg->dwc->dev, "vbus regulator is not available\n");
		return;
	}

	if (on)
		ret = regulator_enable(dotg->vbus_reg);
	else
		ret = regulator_disable(dotg->vbus_reg);

	if (ret)
		dev_err(dotg->dwc->dev, "failed to turn Vbus %s\n",
						on ? "on" : "off");
}

void dwc3_otg_ldo_control(struct otg_fsm *fsm, int on)
{
	struct usb_otg	*otg = fsm->otg;
	struct dwc3_otg	*dotg = container_of(otg, struct dwc3_otg, otg);
	struct device	*dev = dotg->dwc->dev;
	int i, ret1, ret2, ret3;

	if (!on && dp_use_informed) {
		ldo_off_delayed = 1;
		dev_info(dev, "return %s, ldo_off_delayed is %d.\n", __func__, ldo_off_delayed);
		return;
	}
	
	dev_info(dev, "Turn %s LDO\n", on ? "on" : "off");

	if (on) {
		for (i = 0; i < dotg->ldos; i++)
			s2m_ldo_set_mode(dotg->ldo_num[i], 0x3);

		if (dotg->ldo_manual_control == 1) {
			ret1 = regulator_enable(dotg->ldo12);
			ret2 = regulator_enable(dotg->ldo13);
			ret3 = regulator_enable(dotg->ldo14);
			if (ret1 || ret2 || ret3) {
				dev_err(dev, "Failed to enable USB LDOs: %d %d %d\n",
					ret1, ret2, ret3);
				return;
			}
		}
	} else {
		for (i = 0; i < dotg->ldos; i++)
			s2m_ldo_set_mode(dotg->ldo_num[i], 0x1);

		if (dotg->ldo_manual_control == 1) {
			ret1 = regulator_disable(dotg->ldo12);
			ret2 = regulator_disable(dotg->ldo13);
			ret3 = regulator_disable(dotg->ldo14);
			if (ret1 || ret2 || ret3) {
				dev_err(dev, "Failed to disable USB LDOs: %d %d %d\n",
					ret1, ret2, ret3);
				return;
			}
		}
	}

	return;
}

#ifdef DP_CHECK_PHYOFF_ENABLE
static int dwc3_otg_start_host_phyoff(struct otg_fsm *fsm, int on)
{
	struct usb_otg	*otg = fsm->otg;
	struct dwc3_otg	*dotg = container_of(otg, struct dwc3_otg, otg);
	struct dwc3	*dwc = dotg->dwc;
	struct device	*dev = dotg->dwc->dev;
	int ret = 0;

	if (!dotg->dwc->xhci) {
		dev_err(dev, "%s: does not have any xhci\n", __func__);
		return -EINVAL;
	}

	dev_info(dev, "Turn off host - %s\n", __func__);

	if (!on) {
		#if defined(CONFIG_CCIC_MAX77705)
		max77705_set_host_turn_on_event(on);
		#endif
		otg_connection = 0;
		platform_device_del(dwc->xhci);
		phy_conn(dwc->usb2_generic_phy, 0);
		dwc3_core_exit(dwc);
		pm_runtime_put_sync(dev);
		dwc3_otg_ldo_control(fsm, 0);
	}

	return ret;
}
#endif

static int dwc3_otg_start_host(struct otg_fsm *fsm, int on)
{
	struct usb_otg	*otg = fsm->otg;
	struct dwc3_otg	*dotg = container_of(otg, struct dwc3_otg, otg);
	struct dwc3	*dwc = dotg->dwc;
	struct device	*dev = dotg->dwc->dev;
	int ret = 0;

	if (!dotg->dwc->xhci) {
		dev_err(dev, "%s: does not have any xhci\n", __func__);
		return -EINVAL;
	}

	dev_info(dev, "Turn %s host\n", on ? "on" : "off");

#ifdef DP_CHECK_PHYOFF_ENABLE
	/* host off */
	if (!on) {
		if (dp_use_informed == 1) {
			dev_info(dev, "%s, dp on, host off -timer start\n",
				__func__);
			mod_timer(&dp_check_timer.timer,
				jiffies + msecs_to_jiffies(DPCHK_TIME_MSECS));
			dp_check_timer.start = 1;
			#if defined(CONFIG_CCIC_MAX77705)
			max77705_set_host_turn_on_event(on);
			#endif
			return 0;
		}
		/* dp_use_informed = 0 */
		if (dp_check_timer.start  == 1) {
			dev_info(dev, "%s, dp off, host off - timer stop\n",
				__func__);
			del_timer_sync(&dp_check_timer.timer);
			dp_check_timer.start = 0;
		}
	} else {
		if (dp_check_timer.start  == 1) {
			dev_info(dev, "%s, dp state %d, host on -timer stop\n",
				__func__, dp_use_informed);
			del_timer_sync(&dp_check_timer.timer);
			dp_check_timer.start = 0;
			#if defined(CONFIG_CCIC_MAX77705)
			max77705_set_host_turn_on_event(on);
			#endif
			return 0;
		}
	}
#endif

	if (on) {
		otg_connection = 1;
		ldo_off_delayed = 0;
		dwc3_otg_ldo_control(fsm, 1);
		pm_runtime_get_sync(dev);
		ret = dwc3_phy_setup(dwc);
		if (ret) {
			dev_err(dwc->dev, "%s: failed to setup phy\n",
					__func__);
			goto err1;
		}
		ret = dwc3_core_init(dwc);
		if (ret) {
			dev_err(dwc->dev, "%s: failed to reinitialize core\n",
					__func__);
			goto err1;
		}

		phy_conn(dwc->usb2_generic_phy, 1);

		/**
		 * In case there is not a resistance to detect VBUS,
		 * DP/DM controls by S/W are needed at this point.
		 */
		if (dwc->is_not_vbus_pad) {
			phy_set(dwc->usb2_generic_phy,
					SET_DPDM_PULLDOWN, NULL);
			phy_set(dwc->usb3_generic_phy,
					SET_DPDM_PULLDOWN, NULL);
		}

		dwc3_otg_set_host_mode(dotg);
		ret = platform_device_add(dwc->xhci);
		if (ret) {
			dev_err(dev, "%s: cannot add xhci\n", __func__);
			goto err2;
		}
#if defined(CONFIG_CCIC_MAX77705)
		max77705_set_host_turn_on_event(on);
#endif
	} else {
#if defined(CONFIG_CCIC_MAX77705)
		max77705_set_host_turn_on_event(on);
#endif
		otg_connection = 0;
		platform_device_del(dwc->xhci);
err2:
		phy_conn(dwc->usb2_generic_phy, 0);

		dwc3_core_exit(dwc);
err1:
		pm_runtime_put_sync(dev);
		dwc3_otg_ldo_control(fsm, 0);
	}

	return ret;
}

static int dwc3_otg_start_gadget(struct otg_fsm *fsm, int on)
{
	struct usb_otg	*otg = fsm->otg;
	struct dwc3_otg	*dotg = container_of(otg, struct dwc3_otg, otg);
	struct dwc3	*dwc = dotg->dwc;
	struct device	*dev = dotg->dwc->dev;
	int ret = 0;

	if (!otg->gadget) {
		dev_err(dev, "%s does not have any gadget\n", __func__);
		return -EINVAL;
	}

	dev_info(dev, "Turn %s gadget %s\n",
			on ? "on" : "off", otg->gadget->name);

#ifdef DP_CHECK_PHYOFF_ENABLE
	if (dp_check_timer.start  == 1) {
		dev_info(dev, "%s - timer stop", __func__);
		del_timer_sync(&dp_check_timer.timer);
		dwc3_otg_dp_check_delayed_phyoff(fsm);
	}
#endif

	if (on) {
		wake_lock(&dotg->wakelock);
		if (dotg->ldo_manual_control == 1)
			dwc3_otg_ldo_control(fsm, 1);
		pm_runtime_get_sync(dev);
		ret = dwc3_phy_setup(dwc);
		if (ret) {
			dev_err(dwc->dev, "%s: failed to setup phy\n",
					__func__);
			goto err1;
		}
		ret = dwc3_core_init(dwc);
		if (ret) {
			dev_err(dwc->dev, "%s: failed to reinitialize core\n",
					__func__);
			goto err1;
		}
		dwc3_otg_set_peripheral_mode(dotg);
		ret = usb_gadget_vbus_connect(otg->gadget);
		if (ret) {
			dev_err(dwc->dev, "%s: vbus connect failed\n",
					__func__);
			goto err2;
		}

	} else {
		if (dwc->is_not_vbus_pad)
			dwc3_gadget_disconnect_proc(dwc);
		/* avoid missing disconnect interrupt */
		ret = wait_for_completion_timeout(&dwc->disconnect,
				msecs_to_jiffies(200));
		if (!ret) {
			dev_err(dwc->dev, "%s: disconnect completion timeout\n",
					__func__);
			return ret;
		}
		ret = usb_gadget_vbus_disconnect(otg->gadget);
		if (ret)
			dev_err(dwc->dev, "%s: vbus disconnect failed\n",
					__func__);
err2:
		dwc3_core_exit(dwc);
err1:
		pm_runtime_put_sync(dev);
		if (dotg->ldo_manual_control == 1)
			dwc3_otg_ldo_control(fsm, 0);
		wake_unlock(&dotg->wakelock);
	}

	return ret;
}

static struct otg_fsm_ops dwc3_otg_fsm_ops = {
	.drv_vbus	= dwc3_otg_drv_vbus,
	.start_host	= dwc3_otg_start_host,
	.start_gadget	= dwc3_otg_start_gadget,
};

/* -------------------------------------------------------------------------- */

void dwc3_otg_run_sm(struct otg_fsm *fsm)
{
	struct dwc3_otg	*dotg = container_of(fsm, struct dwc3_otg, fsm);
	int		state_changed;

	/* Prevent running SM on early system resume */
	if (!dotg->ready)
		return;

	mutex_lock(&fsm->lock);

	do {
		state_changed = dwc3_otg_statemachine(fsm);
	} while (state_changed > 0);

	mutex_unlock(&fsm->lock);
}

/* Bind/Unbind the peripheral controller driver */
static int dwc3_otg_set_peripheral(struct usb_otg *otg,
				struct usb_gadget *gadget)
{
	struct dwc3_otg	*dotg = container_of(otg, struct dwc3_otg, otg);
	struct otg_fsm	*fsm = &dotg->fsm;
	struct device	*dev = dotg->dwc->dev;

	if (gadget) {
		dev_info(dev, "Binding gadget %s\n", gadget->name);

		otg->gadget = gadget;
	} else {
		dev_info(dev, "Unbinding gadget\n");

		mutex_lock(&fsm->lock);

		if (otg->state == OTG_STATE_B_PERIPHERAL) {
			/* Reset OTG Statemachine */
			fsm->reset = 1;
			dwc3_otg_statemachine(fsm);
			fsm->reset = 0;
		}
		otg->gadget = NULL;

		mutex_unlock(&fsm->lock);

		dwc3_otg_run_sm(fsm);
	}

	return 0;
}

static int dwc3_otg_get_id_state(struct dwc3_otg *dotg)
{
	u32 reg = dwc3_readl(dotg->regs, DWC3_OSTS);

	return !!(reg & DWC3_OTG_OSTS_CONIDSTS);
}

static int dwc3_otg_get_b_sess_state(struct dwc3_otg *dotg)
{
	u32 reg = dwc3_readl(dotg->regs, DWC3_OSTS);

	return !!(reg & DWC3_OTG_OSTS_BSESVALID);
}

static irqreturn_t dwc3_otg_interrupt(int irq, void *_dotg)
{
	struct dwc3_otg	*dotg = (struct dwc3_otg *)_dotg;
	struct otg_fsm	*fsm = &dotg->fsm;
	u32 oevt, handled_events = 0;
	irqreturn_t ret = IRQ_NONE;

	oevt = dwc3_readl(dotg->regs, DWC3_OEVT);

	/* ID */
	if (oevt & DWC3_OEVTEN_OTGCONIDSTSCHNGEVNT) {
		fsm->id = dwc3_otg_get_id_state(dotg);
		handled_events |= DWC3_OEVTEN_OTGCONIDSTSCHNGEVNT;
	}

	/* VBus */
	if (oevt & DWC3_OEVTEN_OTGBDEVVBUSCHNGEVNT) {
		fsm->b_sess_vld = dwc3_otg_get_b_sess_state(dotg);
		handled_events |= DWC3_OEVTEN_OTGBDEVVBUSCHNGEVNT;
	}

	if (handled_events) {
		dwc3_writel(dotg->regs, DWC3_OEVT, handled_events);
		ret = IRQ_WAKE_THREAD;
	}

	return ret;
}

static irqreturn_t dwc3_otg_thread_interrupt(int irq, void *_dotg)
{
	struct dwc3_otg	*dotg = (struct dwc3_otg *)_dotg;

	dwc3_otg_run_sm(&dotg->fsm);

	return IRQ_HANDLED;
}

static void dwc3_otg_enable_irq(struct dwc3_otg *dotg)
{
	/* Enable only connector ID status & VBUS change events */
	dwc3_writel(dotg->regs, DWC3_OEVTEN,
			DWC3_OEVTEN_OTGCONIDSTSCHNGEVNT |
			DWC3_OEVTEN_OTGBDEVVBUSCHNGEVNT);
}

static void dwc3_otg_disable_irq(struct dwc3_otg *dotg)
{
	dwc3_writel(dotg->regs, DWC3_OEVTEN, 0x0);
}

static void dwc3_otg_reset(struct dwc3_otg *dotg)
{
	/*
	 * OCFG[2] - OTG-Version = 0
	 * OCFG[1] - HNPCap = 0
	 * OCFG[0] - SRPCap = 0
	 */
	dwc3_writel(dotg->regs, DWC3_OCFG, 0x0);

	/*
	 * OCTL[6] - PeriMode = 1
	 * OCTL[5] - PrtPwrCtl = 0
	 * OCTL[4] - HNPReq = 0
	 * OCTL[3] - SesReq = 0
	 * OCTL[2] - TermSelDLPulse = 0
	 * OCTL[1] - DevSetHNPEn = 0
	 * OCTL[0] - HstSetHNPEn = 0
	 */
	dwc3_writel(dotg->regs, DWC3_OCTL, DWC3_OTG_OCTL_PERIMODE);

	/* Clear all otg events (interrupts) indications  */
	dwc3_writel(dotg->regs, DWC3_OEVT, DWC3_OEVT_CLEAR_ALL);
}

/* -------------------------------------------------------------------------- */

static ssize_t
dwc3_otg_show_state(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct dwc3	*dwc = dev_get_drvdata(dev);
	struct usb_otg	*otg = &dwc->dotg->otg;

	return snprintf(buf, PAGE_SIZE, "%s\n",
			usb_otg_state_string(otg->state));
}

static DEVICE_ATTR(state, S_IRUSR | S_IRGRP,
	dwc3_otg_show_state, NULL);

static ssize_t
dwc3_otg_show_b_sess(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct dwc3	*dwc = dev_get_drvdata(dev);
	struct otg_fsm	*fsm = &dwc->dotg->fsm;

	return snprintf(buf, PAGE_SIZE, "%d\n", fsm->b_sess_vld);
}

static ssize_t
dwc3_otg_store_b_sess(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t n)
{
	struct dwc3	*dwc = dev_get_drvdata(dev);
	struct otg_fsm	*fsm = &dwc->dotg->fsm;
	int		b_sess_vld;

	if (sscanf(buf, "%d", &b_sess_vld) != 1)
		return -EINVAL;

	fsm->b_sess_vld = !!b_sess_vld;

	dwc3_otg_run_sm(fsm);

	return n;
}

static DEVICE_ATTR(b_sess, S_IWUSR | S_IRUSR | S_IRGRP,
	dwc3_otg_show_b_sess, dwc3_otg_store_b_sess);

static ssize_t
dwc3_otg_show_id(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct dwc3	*dwc = dev_get_drvdata(dev);
	struct otg_fsm	*fsm = &dwc->dotg->fsm;

	return snprintf(buf, PAGE_SIZE, "%d\n", fsm->id);
}

static ssize_t
dwc3_otg_store_id(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t n)
{
	struct dwc3	*dwc = dev_get_drvdata(dev);
	struct otg_fsm	*fsm = &dwc->dotg->fsm;
	int id;

	if (sscanf(buf, "%d", &id) != 1)
		return -EINVAL;

	fsm->id = !!id;

	dwc3_otg_run_sm(fsm);

	return n;
}

static DEVICE_ATTR(id, S_IWUSR | S_IRUSR | S_IRGRP,
	dwc3_otg_show_id, dwc3_otg_store_id);

static struct attribute *dwc3_otg_attributes[] = {
	&dev_attr_id.attr,
	&dev_attr_b_sess.attr,
	&dev_attr_state.attr,
	NULL
};

static const struct attribute_group dwc3_otg_attr_group = {
	.attrs = dwc3_otg_attributes,
};

/**
 * dwc3_otg_start
 * @dwc: pointer to our controller context structure
 */
int dwc3_otg_start(struct dwc3 *dwc)
{
	struct dwc3_otg	*dotg = dwc->dotg;
	struct otg_fsm	*fsm = &dotg->fsm;
	int		ret;

	if (dotg->ext_otg_ops) {
		ret = dwc3_ext_otg_start(dotg);
		if (ret) {
			dev_err(dwc->dev, "failed to start external OTG\n");
			return ret;
		}
	} else {
		dotg->regs = dwc->regs;

		dwc3_otg_reset(dotg);

		dotg->fsm.id = dwc3_otg_get_id_state(dotg);
		dotg->fsm.b_sess_vld = dwc3_otg_get_b_sess_state(dotg);

		dotg->irq = platform_get_irq(to_platform_device(dwc->dev), 0);
		ret = devm_request_threaded_irq(dwc->dev, dotg->irq,
				dwc3_otg_interrupt,
				dwc3_otg_thread_interrupt,
				IRQF_SHARED, "dwc3-otg", dotg);
		if (ret) {
			dev_err(dwc->dev, "failed to request irq #%d --> %d\n",
					dotg->irq, ret);
			return ret;
		}

		dwc3_otg_enable_irq(dotg);
	}

	dotg->ready = 1;

	dwc3_otg_run_sm(fsm);

	return 0;
}

/**
 * dwc3_otg_stop
 * @dwc: pointer to our controller context structure
 */
void dwc3_otg_stop(struct dwc3 *dwc)
{
	struct dwc3_otg         *dotg = dwc->dotg;

	if (dotg->ext_otg_ops) {
		dwc3_ext_otg_stop(dotg);
	} else {
		dwc3_otg_disable_irq(dotg);
		free_irq(dotg->irq, dotg);
	}

	dotg->ready = 0;
}

/* -------------------------------------------------------------------------- */
bool otg_is_connect(void)
{
	if (otg_connection)
		return true;
	else
		return false;
}
EXPORT_SYMBOL_GPL(otg_is_connect);

int dwc3_otg_init(struct dwc3 *dwc)
{
	struct dwc3_otg *dotg;
	struct dwc3_ext_otg_ops *ops = NULL;
	int ret = 0;

	/* EXYNOS SoCs don't have HW OTG, but support SW OTG. */
	ops = dwc3_otg_exynos_rsw_probe(dwc);
	if (!ops)
		return 0;

	/* Allocate and init otg instance */
	dotg = devm_kzalloc(dwc->dev, sizeof(struct dwc3_otg), GFP_KERNEL);
	if (!dotg) {
		dev_err(dwc->dev, "unable to allocate dwc3_otg\n");
		return -ENOMEM;
	}

	/* This reference is used by dwc3 modules for checking otg existance */
	dwc->dotg = dotg;
	dotg->dwc = dwc;

	if (of_property_read_bool(dwc->dev->of_node, "ldo_manual_control"))
		dotg->ldo_manual_control = 1;
	else
		dotg->ldo_manual_control = 0;
	dev_info(dwc->dev, "%s, ldo_man_control = %d\n",
			__func__, dotg->ldo_manual_control);

	ret = of_property_read_u32(dwc->dev->of_node,"ldos", &dotg->ldos);
	if (ret < 0) {
		dev_err(dwc->dev, "can't get ldo information\n");
		return -EINVAL;
	}

	if (dotg->ldo_manual_control) {
		/*
		 * ldo 12, 13, 14 is controlled manually,
		 * decrement ldo numbers
		 */
		dotg->ldos = dotg->ldos - 3;
	}

	if (dotg->ldos) {
		dev_info(dwc->dev, "have %d LDOs for USB L2 suspend\n",
			dotg->ldos);
		dotg->ldo_num = (int *)devm_kmalloc(dwc->dev,
			sizeof(int) * (dotg->ldos), GFP_KERNEL);
		ret = of_property_read_u32_array(dwc->dev->of_node,
				"ldo_number", dotg->ldo_num, dotg->ldos);
	} else {
		dev_info(dwc->dev,
			"don't have LDOs for USB L2 suspend\n");
	}

	if (dotg->ldo_manual_control == 1) {
		dotg->ldo12 = regulator_get(dwc->dev, "vdd_ldo12");
		if (IS_ERR(dotg->ldo12) || dotg->ldo12 == NULL) {
			dev_err(dwc->dev, "%s - ldo12_usb regulator_get fail %p %d\n",
				__func__, dotg->ldo12, IS_ERR(dotg->ldo12));
			return -ENODEV;
		}

		dotg->ldo13 = regulator_get(dwc->dev, "vdd_ldo13");
		if (IS_ERR(dotg->ldo13) || dotg->ldo13 == NULL) {
			dev_err(dwc->dev, "%s - ldo13_usb regulator_get fail %p %d\n",
				__func__, dotg->ldo13, IS_ERR(dotg->ldo13));
			return -ENODEV;
		}

		dotg->ldo14 = regulator_get(dwc->dev, "vdd_ldo14");
		if (IS_ERR(dotg->ldo14) || dotg->ldo14 == NULL) {
			dev_err(dwc->dev, "%s - ldo14_usb regulator_get fail %p %d\n",
				__func__, dotg->ldo14, IS_ERR(dotg->ldo14));
			return -ENODEV;
		}
	}

	dotg->ext_otg_ops = ops;

	dotg->otg.set_peripheral = dwc3_otg_set_peripheral;
	dotg->otg.set_host = NULL;

	dotg->otg.state = OTG_STATE_UNDEFINED;

	mutex_init(&dotg->fsm.lock);
	dotg->fsm.ops = &dwc3_otg_fsm_ops;
	dotg->fsm.otg = &dotg->otg;

	dotg->vbus_reg = devm_regulator_get(dwc->dev->parent, "dwc3-vbus");
	if (IS_ERR(dotg->vbus_reg))
		dev_err(dwc->dev, "failed to obtain vbus regulator\n");

	if (dotg->ext_otg_ops) {
		ret = dwc3_ext_otg_setup(dotg);
		if (ret) {
			dev_err(dwc->dev, "failed to setup OTG\n");
			return ret;
		}
	}

#ifdef DP_CHECK_PHYOFF_ENABLE
	dp_check_work = kzalloc(sizeof(*dp_check_work), GFP_KERNEL);
	INIT_WORK(&dp_check_work->work, dwc3_otg_dp_check_usb_work);

	dp_check_work->fsm = &dotg->fsm;
	dwc3_otg_dp_check_timer_init(dp_check_work);
#endif

	wake_lock_init(&dotg->wakelock, WAKE_LOCK_SUSPEND, "dwc3-otg");

	ret = sysfs_create_group(&dwc->dev->kobj, &dwc3_otg_attr_group);
	if (ret)
		dev_err(dwc->dev, "failed to create dwc3 otg attributes\n");

	register_usb_is_connect(otg_is_connect);

	return 0;
}

void dwc3_otg_exit(struct dwc3 *dwc)
{
	struct dwc3_otg *dotg = dwc->dotg;

	if (!dotg->ext_otg_ops)
		return;

	dwc3_ext_otg_exit(dotg);

	sysfs_remove_group(&dwc->dev->kobj, &dwc3_otg_attr_group);
	wake_lock_destroy(&dotg->wakelock);
	free_irq(dotg->irq, dotg);
	dotg->otg.state = OTG_STATE_UNDEFINED;
	kfree(dotg);
	dwc->dotg = NULL;
}
