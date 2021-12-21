/* Copyright (c) 2010-2014, 2016-2019 The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/types.h>
#include <linux/uaccess.h>
#include <linux/spinlock.h>
#include <linux/list.h>
#include <linux/sched.h>
#include <linux/wait.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/delay.h>
#include <linux/debugfs.h>
#include <linux/platform_device.h>
#include <linux/sysfs.h>
#include <linux/device.h>
#include <linux/of.h>
#include <linux/slab.h>
#include <linux/ipc_logging.h>
#include <linux/of_device.h>
#include <soc/qcom/subsystem_restart.h>
#include <soc/qcom/scm.h>
#include <dsp/apr_audio-v2.h>
#include <dsp/audio_notifier.h>
#include <ipc/apr.h>
#include <ipc/apr_tal.h>

#define APR_PKT_IPC_LOG_PAGE_CNT 2

static struct device *apr_dev_ptr;
static struct apr_q6 q6;
static struct apr_client client[APR_DEST_MAX][APR_CLIENT_MAX];
static void *apr_pkt_ctx;
static wait_queue_head_t dsp_wait;
static wait_queue_head_t modem_wait;
static bool is_modem_up;
static bool is_initial_boot;
static bool is_child_devices_loaded;

/**
 * apr_get_modem_state:
 *
 * Returns current modem load status
 *
 */
enum apr_subsys_state apr_get_modem_state(void)
{
	return atomic_read(&q6.modem_state);
}
EXPORT_SYMBOL(apr_get_modem_state);

/**
 * apr_set_modem_state - Update modem load status.
 *
 * @state: State to update modem load status
 *
 */
void apr_set_modem_state(enum apr_subsys_state state)
{
	atomic_set(&q6.modem_state, state);
}
EXPORT_SYMBOL(apr_set_modem_state);

enum apr_subsys_state apr_cmpxchg_modem_state(enum apr_subsys_state prev,
					      enum apr_subsys_state new)
{
	return atomic_cmpxchg(&q6.modem_state, prev, new);
}

static void apr_modem_down(unsigned long opcode)
{
	apr_set_modem_state(APR_SUBSYS_DOWN);
	dispatch_event(opcode, APR_DEST_MODEM);
}

static void apr_modem_up(void)
{
	if (apr_cmpxchg_modem_state(APR_SUBSYS_DOWN, APR_SUBSYS_UP) ==
							APR_SUBSYS_DOWN)
		wake_up(&modem_wait);
	is_modem_up = 1;
}

enum apr_subsys_state apr_get_q6_state(void)
{
	return atomic_read(&q6.q6_state);
}
EXPORT_SYMBOL(apr_get_q6_state);

int apr_set_q6_state(enum apr_subsys_state state)
{
	pr_debug("%s: setting adsp state %d\n", __func__, state);
	if (state < APR_SUBSYS_DOWN || state > APR_SUBSYS_LOADED)
		return -EINVAL;
	atomic_set(&q6.q6_state, state);
	return 0;
}
EXPORT_SYMBOL(apr_set_q6_state);

enum apr_subsys_state apr_cmpxchg_q6_state(enum apr_subsys_state prev,
					   enum apr_subsys_state new)
{
	return atomic_cmpxchg(&q6.q6_state, prev, new);
}

static void apr_adsp_down(unsigned long opcode)
{
	apr_set_q6_state(APR_SUBSYS_DOWN);
	dispatch_event(opcode, APR_DEST_QDSP6);
}

static void apr_add_child_devices(struct work_struct *work)
{
	int ret;

	ret = of_platform_populate(apr_dev_ptr->of_node,
			NULL, NULL, apr_dev_ptr);
	if (ret)
		dev_err(apr_dev_ptr, "%s: failed to add child nodes, ret=%d\n",
			__func__, ret);
}

static void apr_adsp_up(void)
{
	if (apr_cmpxchg_q6_state(APR_SUBSYS_DOWN, APR_SUBSYS_LOADED) ==
							APR_SUBSYS_DOWN)
		wake_up(&dsp_wait);

	if (!is_child_devices_loaded) {
		schedule_delayed_work(&add_chld_dev_work,
				msecs_to_jiffies(100));
		is_child_devices_loaded = true;
	}
}

int apr_wait_for_device_up(int dest_id)
{
	int rc = -1;

	if (dest_id == APR_DEST_MODEM)
		rc = wait_event_interruptible_timeout(modem_wait,
				    (apr_get_modem_state() == APR_SUBSYS_UP),
				    (1 * HZ));
	else if (dest_id == APR_DEST_QDSP6)
		rc = wait_event_interruptible_timeout(dsp_wait,
				    (apr_get_q6_state() == APR_SUBSYS_UP),
				    (1 * HZ));
	else
		pr_err("%s: unknown dest_id %d\n", __func__, dest_id);
	/* returns left time */
	return rc;
}

int apr_load_adsp_image(void)
{
	int rc = 0;

	mutex_lock(&q6.lock);
	if (apr_get_q6_state() == APR_SUBSYS_UP) {
		q6.pil = subsystem_get("adsp");
		if (IS_ERR(q6.pil)) {
			rc = PTR_ERR(q6.pil);
			pr_err("APR: Unable to load q6 image, error:%d\n", rc);
		} else {
			apr_set_q6_state(APR_SUBSYS_LOADED);
			pr_debug("APR: Image is loaded, stated\n");
		}
	} else if (apr_get_q6_state() == APR_SUBSYS_LOADED) {
		pr_debug("APR: q6 image already loaded\n");
	} else {
		pr_debug("APR: cannot load state %d\n", apr_get_q6_state());
	}
	mutex_unlock(&q6.lock);
	return rc;
}

struct apr_client *apr_get_client(int dest_id, int client_id)
{
	return &client[dest_id][client_id];
}

/**
 * apr_send_pkt - Clients call to send packet
 * to destination processor.
 *
 * @handle: APR service handle
 * @buf: payload to send to destination processor.
 *
 * Returns Bytes(>0)pkt_size on success or error on failure.
 */
int apr_send_pkt(void *handle, uint32_t *buf)
{
	struct apr_svc *svc = handle;
	struct apr_client *clnt;
	struct apr_hdr *hdr;
	uint16_t dest_id;
	uint16_t client_id;
	uint16_t w_len;
	int rc;
	unsigned long flags;

	if (!handle || !buf) {
		pr_err("APR: Wrong parameters\n");
		return -EINVAL;
	}
	if (svc->need_reset) {
		pr_err_ratelimited("apr: send_pkt service need reset\n");
		return -ENETRESET;
	}

	if ((svc->dest_id == APR_DEST_QDSP6) &&
	    (apr_get_q6_state() != APR_SUBSYS_LOADED)) {
		pr_err_ratelimited("%s: Still dsp is not Up\n", __func__);
		return -ENETRESET;
	} else if ((svc->dest_id == APR_DEST_MODEM) &&
		   (apr_get_modem_state() == APR_SUBSYS_DOWN)) {
		pr_err("apr: Still Modem is not Up\n");
		return -ENETRESET;
	}

	spin_lock_irqsave(&svc->w_lock, flags);
	dest_id = svc->dest_id;
	client_id = svc->client_id;
	clnt = &client[dest_id][client_id];

	if (!client[dest_id][client_id].handle) {
		pr_err_ratelimited("APR: Still service is not yet opened\n");
		spin_unlock_irqrestore(&svc->w_lock, flags);
		return -EINVAL;
	}
	hdr = (struct apr_hdr *)buf;

	hdr->src_domain = APR_DOMAIN_APPS;
	hdr->src_svc = svc->id;
	hdr->dest_domain = svc->dest_domain;
	hdr->dest_svc = svc->id;

	if (unlikely(apr_cf_debug)) {
		APR_PKT_INFO(
		"Tx: src_addr[0x%X] dest_addr[0x%X] opcode[0x%X] token[0x%X]",
		(hdr->src_domain << 8) | hdr->src_svc,
		(hdr->dest_domain << 8) | hdr->dest_svc, hdr->opcode,
		hdr->token);
	}

	rc = apr_tal_write(clnt->handle, buf,
			(struct apr_pkt_priv *)&svc->pkt_owner,
			hdr->pkt_size);
	if (rc >= 0) {
		w_len = rc;
		if (w_len != hdr->pkt_size) {
			pr_err("%s: Unable to write whole APR pkt successfully: %d\n",
			       __func__, rc);
			rc = -EINVAL;
		}
	} else {
		pr_err("%s: Write APR pkt failed with error %d\n",
			__func__, rc);
	}
	spin_unlock_irqrestore(&svc->w_lock, flags);

	return rc;
}
EXPORT_SYMBOL(apr_send_pkt);

int apr_pkt_config(void *handle, struct apr_pkt_cfg *cfg)
{
	struct apr_svc *svc = (struct apr_svc *)handle;
	uint16_t dest_id;
	uint16_t client_id;
	struct apr_client *clnt;

	if (!handle) {
		pr_err("%s: Invalid handle\n", __func__);
		return -EINVAL;
	}

	if (svc->need_reset) {
		pr_err("%s: service need reset\n", __func__);
		return -ENETRESET;
	}

	svc->pkt_owner = cfg->pkt_owner;
	dest_id = svc->dest_id;
	client_id = svc->client_id;
	clnt = &client[dest_id][client_id];

	return apr_tal_rx_intents_config(clnt->handle,
		cfg->intents.num_of_intents, cfg->intents.size);
}


/**
 * apr_start_rx_rt - Clients call to vote for thread
 * priority upgrade whenever needed.
 *
 * @handle: APR service handle
 *
 * Returns 0 on success or error otherwise.
 */
int apr_start_rx_rt(void *handle)
{
	int rc = 0;
	struct apr_svc *svc = handle;
	uint16_t dest_id = 0;
	uint16_t client_id = 0;

	if (!svc) {
		pr_err("%s: Invalid APR handle\n", __func__);
		return -EINVAL;
	}

	mutex_lock(&svc->m_lock);
	dest_id = svc->dest_id;
	client_id = svc->client_id;

	if ((client_id >= APR_CLIENT_MAX) || (dest_id >= APR_DEST_MAX)) {
		pr_err("%s: %s invalid. client_id = %u, dest_id = %u\n",
		       __func__,
		       client_id >= APR_CLIENT_MAX ? "Client ID" : "Dest ID",
		       client_id, dest_id);
		rc = -EINVAL;
		goto exit;
	}

	if (!client[dest_id][client_id].handle) {
		pr_err("%s: Client handle is NULL\n", __func__);
		rc = -EINVAL;
		goto exit;
	}

	rc = apr_tal_start_rx_rt(client[dest_id][client_id].handle);
	if (rc)
		pr_err("%s: failed to set RT thread priority for APR RX. rc = %d\n",
			__func__, rc);

exit:
	mutex_unlock(&svc->m_lock);
	return rc;
}
EXPORT_SYMBOL(apr_start_rx_rt);

/**
 * apr_end_rx_rt - Clients call to unvote for thread
 * priority upgrade (perviously voted with
 * apr_start_rx_rt()).
 *
 * @handle: APR service handle
 *
 * Returns 0 on success or error otherwise.
 */
int apr_end_rx_rt(void *handle)
{
	int rc = 0;
	struct apr_svc *svc = handle;
	uint16_t dest_id = 0;
	uint16_t client_id = 0;

	if (!svc) {
		pr_err("%s: Invalid APR handle\n", __func__);
		return -EINVAL;
	}

	mutex_lock(&svc->m_lock);
	dest_id = svc->dest_id;
	client_id = svc->client_id;

	if ((client_id >= APR_CLIENT_MAX) || (dest_id >= APR_DEST_MAX)) {
		pr_err("%s: %s invalid. client_id = %u, dest_id = %u\n",
		       __func__,
		       client_id >= APR_CLIENT_MAX ? "Client ID" : "Dest ID",
		       client_id, dest_id);
		rc = -EINVAL;
		goto exit;
	}

	if (!client[dest_id][client_id].handle) {
		pr_err("%s: Client handle is NULL\n", __func__);
		rc = -EINVAL;
		goto exit;
	}

	rc = apr_tal_end_rx_rt(client[dest_id][client_id].handle);
	if (rc)
		pr_err("%s: failed to reset RT thread priority for APR RX. rc = %d\n",
			__func__, rc);

exit:
	mutex_unlock(&svc->m_lock);
	return rc;
}
EXPORT_SYMBOL(apr_end_rx_rt);


/* Dispatch the Reset events to Modem and audio clients */
static void dispatch_event(unsigned long code, uint16_t proc)
{
	struct apr_client *apr_client;
	struct apr_client_data data;
	struct apr_svc *svc;
	uint16_t clnt;
	int i, j;

	memset(&data, 0, sizeof(data));
	data.opcode = RESET_EVENTS;
	data.reset_event = code;

	/* Service domain can be different from the processor */
	data.reset_proc = apr_get_reset_domain(proc);

	clnt = APR_CLIENT_AUDIO;
	apr_client = &client[proc][clnt];
	for (i = 0; i < APR_SVC_MAX; i++) {
		mutex_lock(&apr_client->svc[i].m_lock);
		if (apr_client->svc[i].fn) {
			apr_client->svc[i].need_reset = 0x1;
			apr_client->svc[i].fn(&data, apr_client->svc[i].priv);
		}
		if (apr_client->svc[i].port_cnt) {
			svc = &(apr_client->svc[i]);
			svc->need_reset = 0x1;
			for (j = 0; j < APR_MAX_PORTS; j++)
				if (svc->port_fn[j])
					svc->port_fn[j](&data,
						svc->port_priv[j]);
		}
		mutex_unlock(&apr_client->svc[i].m_lock);
	}

	clnt = APR_CLIENT_VOICE;
	apr_client = &client[proc][clnt];
	for (i = 0; i < APR_SVC_MAX; i++) {
		mutex_lock(&apr_client->svc[i].m_lock);
		if (apr_client->svc[i].fn) {
			apr_client->svc[i].need_reset = 0x1;
			apr_client->svc[i].fn(&data, apr_client->svc[i].priv);
		}
		if (apr_client->svc[i].port_cnt) {
			svc = &(apr_client->svc[i]);
			svc->need_reset = 0x1;
			for (j = 0; j < APR_MAX_PORTS; j++)
				if (svc->port_fn[j])
					svc->port_fn[j](&data,
						svc->port_priv[j]);
		}
		mutex_unlock(&apr_client->svc[i].m_lock);
	}
}

static int apr_notifier_service_cb(struct notifier_block *this,
				   unsigned long opcode, void *data)
{
	struct audio_notifier_cb_data *cb_data = data;

	if (cb_data == NULL) {
		pr_err("%s: Callback data is NULL!\n", __func__);
		goto done;
	}

	pr_debug("%s: Service opcode 0x%lx, domain %d\n",
		__func__, opcode, cb_data->domain);

	switch (opcode) {
	case AUDIO_NOTIFIER_SERVICE_DOWN:
		/*
		 * Use flag to ignore down notifications during
		 * initial boot. There is no benefit from error
		 * recovery notifications during initial boot
		 * up since everything is expected to be down.
		 */
		if (is_initial_boot) {
			is_initial_boot = false;
			break;
		}
		if (cb_data->domain == AUDIO_NOTIFIER_MODEM_DOMAIN)
			apr_modem_down(opcode);
		else
			apr_adsp_down(opcode);
		break;
	case AUDIO_NOTIFIER_SERVICE_UP:
		is_initial_boot = false;
		if (cb_data->domain == AUDIO_NOTIFIER_MODEM_DOMAIN)
			apr_modem_up();
		else
			apr_adsp_up();
		break;
	default:
		break;
	}
done:
	return NOTIFY_OK;
}

static struct notifier_block adsp_service_nb = {
	.notifier_call  = apr_notifier_service_cb,
	.priority = 0,
};

static struct notifier_block modem_service_nb = {
	.notifier_call  = apr_notifier_service_cb,
	.priority = 0,
};

#ifdef CONFIG_DEBUG_FS
static int __init apr_debug_init(void)
{
	debugfs_apr_debug = debugfs_create_file("msm_apr_debug",
						 S_IFREG | 0444, NULL, NULL,
						 &apr_debug_ops);
	return 0;
}
#else
static int __init apr_debug_init(void)
(
	return 0;
)
#endif

static void apr_cleanup(void)
{
	int i, j, k;

	subsys_notif_deregister("apr_modem");
	subsys_notif_deregister("apr_adsp");
	if (apr_reset_workqueue)
		destroy_workqueue(apr_reset_workqueue);
	mutex_destroy(&q6.lock);
	for (i = 0; i < APR_DEST_MAX; i++) {
		for (j = 0; j < APR_CLIENT_MAX; j++) {
			mutex_destroy(&client[i][j].m_lock);
			for (k = 0; k < APR_SVC_MAX; k++)
				mutex_destroy(&client[i][j].svc[k].m_lock);
		}
	}
}

static int apr_probe(struct platform_device *pdev)
{
	int i, j, k;

	init_waitqueue_head(&dsp_wait);
	init_waitqueue_head(&modem_wait);

	for (i = 0; i < APR_DEST_MAX; i++)
		for (j = 0; j < APR_CLIENT_MAX; j++) {
			mutex_init(&client[i][j].m_lock);
			for (k = 0; k < APR_SVC_MAX; k++) {
				mutex_init(&client[i][j].svc[k].m_lock);
				spin_lock_init(&client[i][j].svc[k].w_lock);
			}
		}
	apr_set_subsys_state();
	mutex_init(&q6.lock);
	apr_reset_workqueue = create_singlethread_workqueue("apr_driver");
	if (!apr_reset_workqueue)
		return -ENOMEM;

	apr_pkt_ctx = ipc_log_context_create(APR_PKT_IPC_LOG_PAGE_CNT,
						"apr", 0);
	if (!apr_pkt_ctx)
		pr_err("%s: Unable to create ipc log context\n", __func__);

	is_initial_boot = true;
	subsys_notif_register("apr_adsp", AUDIO_NOTIFIER_ADSP_DOMAIN,
			      &adsp_service_nb);
	subsys_notif_register("apr_modem", AUDIO_NOTIFIER_MODEM_DOMAIN,
			      &modem_service_nb);

	apr_tal_init();
	apr_dev_ptr = &pdev->dev;
	INIT_DELAYED_WORK(&add_chld_dev_work, apr_add_child_devices);
	return apr_debug_init();
}

static int apr_remove(struct platform_device *pdev)
{
	apr_cleanup();
	return 0;
}

static const struct of_device_id apr_machine_of_match[]  = {
	{ .compatible = "qcom,msm-audio-apr", },
	{},
};

static struct platform_driver apr_driver = {
	.probe = apr_probe,
	.remove = apr_remove,
	.driver = {
		.name = "audio_apr",
		.owner = THIS_MODULE,
		.of_match_table = apr_machine_of_match,
	}
};

static int __init apr_init(void)
{
	platform_driver_register(&apr_driver);
	apr_dummy_init();
	return 0;
}
module_init(apr_init);

static void __exit apr_exit(void)
{
	apr_dummy_exit();
	platform_driver_unregister(&apr_driver);
}
module_exit(apr_exit);

MODULE_DESCRIPTION("APR DRIVER");
MODULE_LICENSE("GPL v2");
MODULE_DEVICE_TABLE(of, apr_machine_of_match);
