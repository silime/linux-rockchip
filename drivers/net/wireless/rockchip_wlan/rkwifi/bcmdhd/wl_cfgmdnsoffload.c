/*
 * BCM mDNS Offload Vendor Command Implementation
 *
 * Copyright (C) 2026 Synaptics Incorporated. All rights reserved.
 *
 * This software is licensed to you under the terms of the
 * GNU General Public License version 2 (the "GPL") with Broadcom special exception.
 *
 * INFORMATION CONTAINED IN THIS DOCUMENT IS PROVIDED "AS-IS," AND SYNAPTICS
 * EXPRESSLY DISCLAIMS ALL EXPRESS AND IMPLIED WARRANTIES, INCLUDING ANY
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE,
 * AND ANY WARRANTIES OF NON-INFRINGEMENT OF ANY INTELLECTUAL PROPERTY RIGHTS.
 * IN NO EVENT SHALL SYNAPTICS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, PUNITIVE, OR CONSEQUENTIAL DAMAGES ARISING OUT OF OR IN CONNECTION
 * WITH THE USE OF THE INFORMATION CONTAINED IN THIS DOCUMENT, HOWEVER CAUSED
 * AND BASED ON ANY THEORY OF LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * NEGLIGENCE OR OTHER TORTIOUS ACTION, AND EVEN IF SYNAPTICS WAS ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE. IF A TRIBUNAL OF COMPETENT JURISDICTION
 * DOES NOT PERMIT THE DISCLAIMER OF DIRECT DAMAGES OR ANY OTHER DAMAGES,
 * SYNAPTICS' TOTAL CUMULATIVE LIABILITY TO ANY PARTY SHALL NOT
 * EXCEED ONE HUNDRED U.S. DOLLARS
 *
 * Copyright (C) 2026, Broadcom.
 *
 *      Unless you and Broadcom execute a separate written software license
 * agreement governing use of this software, this software is licensed to you
 * under the terms of the GNU General Public License version 2 (the "GPL"),
 * available at http://www.broadcom.com/licenses/GPLv2.php, with the
 * following added to such license:
 *
 *      As a special exception, the copyright holders of this software give you
 * permission to link this software with independent modules, and to copy and
 * distribute the resulting executable under terms of your choice, provided that
 * you also meet, for each linked independent module, the terms and conditions of
 * the license of that module.  An independent module is a module which is not
 * derived from this software.  The special exception does not apply to any
 * modifications of the software.
 *
 *
 * <<Broadcom-WL-IPTag/Dual:>>
 */

#include <bcmutils.h>
#include <wlioctl.h>
#include <wlioctl_utils.h>
#include <wl_cfg80211.h>
#include <wl_cfgvendor.h>
#include <wl_android.h>
#include <net/netlink.h>
#include "wl_cfgmdnsoffload.h"

int wl_cfgvendor_mdnsoffload_set_state(struct wiphy *wiphy, struct wireless_dev *wdev,
	const void *data, int len)
{
	const struct nlattr *iter;
	u8 enabled = 0;
	int rem;
	int ret;

	WL_ERR(("%s: Enter\n", __FUNCTION__));
	nla_for_each_attr(iter, data, len, rem) {
	    if (nla_type(iter) == MDNS_OFFLOAD_ATTR_ENABLED) {
	        enabled = nla_get_u8(iter);
	        WL_ERR(("%s: Received state value: %u\n", __FUNCTION__, enabled));
	        break;
	    }
	}

	ret = wldev_iovar_setint(wdev_to_ndev(wdev), "mdns", enabled);
	return ret;

}

int wl_cfgvendor_mdnsoffload_reset_all(struct wiphy *wiphy, struct wireless_dev *wdev,
	const void *data, int len)
{
	int ret;

	WL_ERR(("%s: Enter\n", __FUNCTION__));
	ret = wldev_iovar_setint(wdev_to_ndev(wdev), "mdns_reset", TRUE);

	return ret;
}

int wl_cfgvendor_mdnsoffload_add_responses(struct wiphy *wiphy, struct wireless_dev *wdev,
	const void *data, int len)
{
	const struct nlattr *iter;
	void *proto_data = NULL;
	int proto_len = 0;
	void *criteria_data = NULL;
	int criteria_len = 0;
	int record_key;
	int rem;
	int ret;
	struct sk_buff *skb;
	struct bcm_cfg80211 *cfg = wiphy_priv(wiphy);
	uint16_t rr_count;
	uint16_t payload_len;
	size_t buf_len;
	uint8_t *buf;
	rr_entry_t *criteria_from_netlink;
	int num_criteria;
	int i = 0 ;

	WL_ERR(("%s: Enter\n", __FUNCTION__));
	nla_for_each_attr(iter, data, len, rem) {
	    if (nla_type(iter) == MDNS_OFFLOAD_ATTR_PROTOCOL_DATA) {
	        proto_data = nla_data(iter);
	        proto_len = nla_len(iter);
	        WL_ERR(("%s: Received protocol data with length: %d\n", __FUNCTION__, proto_len));
	        prhex("MDNS response", proto_data, proto_len);
	    } else if (nla_type(iter) == MDNS_OFFLOAD_ATTR_MATCH_CRITERIA) {
	        criteria_data = nla_data(iter);
	        criteria_len = nla_len(iter);
	        WL_ERR(("%s: Received match criteria with length: %d\n", __FUNCTION__, criteria_len));
	        prhex("Match criteria", criteria_data, criteria_len);
	    }
	}

	if (!proto_data || proto_len == 0 || !criteria_data || criteria_len == 0) return -EINVAL;

	if (proto_len > MDNS_OFFLOAD_PAYLOAD_SIZE) {
	    WL_ERR(("%s: Packet length (%d) exceeds max payload size (%d)\n",
	        __FUNCTION__, proto_len, MDNS_OFFLOAD_PAYLOAD_SIZE));
	    return -EINVAL;
	}
	if (criteria_len % sizeof(rr_entry_t) != 0) {
	    WL_ERR(("%s: Invalid criteria_len (%d), not a multiple of struct size\n",
				__FUNCTION__, criteria_len));
		return -EINVAL;
	}
	num_criteria = criteria_len / sizeof(rr_entry_t);
	if (num_criteria > MDNS_MAX_RR) {
		WL_ERR(("%s: Number of criteria (%d) exceeds max (%d)\n",
				__FUNCTION__, num_criteria, MDNS_MAX_RR));
		return -EINVAL;
	}
	rr_count = num_criteria;
	payload_len = proto_len;
	buf_len = 2 + rr_count * sizeof(rr_entry_t) + 2 + proto_len;
	buf = kzalloc(buf_len, GFP_KERNEL);
	if (!buf) return -ENOMEM;

	// copy rr_count
	memcpy(buf, &rr_count, 2);

	// copy rr[]
	criteria_from_netlink = (rr_entry_t *)criteria_data;
	for (i=0; i<num_criteria; i++) {
		uint32_t type = criteria_from_netlink[i].type;
		uint32_t offset = criteria_from_netlink[i].offset;
		memcpy(buf + 2 + i*sizeof(rr_entry_t), &type, 4);
		memcpy(buf + 2 + i*sizeof(rr_entry_t) + 4, &offset, 4);
	}

	// copy payload_len
	memcpy(buf + 2 + rr_count*sizeof(rr_entry_t), &payload_len, 2);

	// copy payload
	memcpy(buf + 2 + rr_count*sizeof(rr_entry_t) + 2, proto_data, proto_len);

	// send to FW
	ret = wldev_iovar_getbuf(wdev_to_ndev(wdev),
			"mdns_payload_add",
			buf,
			buf_len,
			cfg->ioctl_buf,
			WLC_IOCTL_MAXLEN,
			&cfg->ioctl_buf_sync);

	kfree(buf);

	if (ret < 0)
		record_key = -1;
	else {
		memcpy(&record_key, cfg->ioctl_buf, sizeof(record_key));
		record_key = dtoh32(record_key);
	}

	skb = cfg80211_vendor_cmd_alloc_reply_skb(wiphy, ATTRIBUTE_U32_LEN + NLA_HDRLEN);
	if (unlikely(!skb)) {
		WL_ERR(("%s: can't allocate %d bytes\n", __FUNCTION__, ATTRIBUTE_U32_LEN + NLA_HDRLEN));
		ret = -ENOMEM;
		goto fail;
	}

	WL_ERR(("%s: Returning record_key: %d\n", __FUNCTION__, record_key));
	ret = nla_put_u32(skb, MDNS_OFFLOAD_ATTR_RETURN_VALUE, record_key);
	if (unlikely(ret)) {
	    WL_ERR(("Failed to put record_key info , ret=%d\n", ret));
	    goto fail;
	}

	ret = cfg80211_vendor_cmd_reply(skb);
	if (unlikely(ret)) {
	    WL_ERR(("Vendor Command reply failed ret:%d \n", ret));
	}
	return ret;

fail:
	/* Free skb memory */
	if (skb) {
	    kfree_skb(skb);
	}
	return ret;
}

int wl_cfgvendor_mdnsoffload_remove_responses(struct wiphy *wiphy, struct wireless_dev *wdev,
	const void *data, int len)
{
	const struct nlattr *iter;
	int record_key = -1;
	int rem;
	int ret;

	WL_ERR(("%s: Enter\n", __FUNCTION__));
	nla_for_each_attr(iter, data, len, rem) {
	    if (nla_type(iter) == MDNS_OFFLOAD_ATTR_RECORD_KEY) {
	        record_key = nla_get_u32(iter);
	        WL_ERR(("%s: Received record_key: %d\n", __FUNCTION__, record_key));
	        break;
	    }
	}
	if (record_key < 0) return -EINVAL;

	ret = wldev_iovar_setint(wdev_to_ndev(wdev), "mdns_payload_del", record_key);
	return ret;

}

static int wl_cfgvendor_mdnsoffload_get_value_cmd(struct wiphy *wiphy, struct wireless_dev *wdev,
	const void *data, int len, uint16 iovar_cmd)
{
	const struct nlattr *iter;
	int record_key = -1; // Optional key
	int return_value = -1;
	int rem;
	int ret;
	unsigned long long modulus = (unsigned long long)INT_MAX + 1;
	struct sk_buff *skb;

	WL_ERR(("%s: Enter (iovar_cmd: %u)\n", __FUNCTION__, iovar_cmd));
	nla_for_each_attr(iter, data, len, rem) {
	    if (nla_type(iter) == MDNS_OFFLOAD_ATTR_RECORD_KEY) {
	        record_key = nla_get_u32(iter);
	        WL_ERR(("%s: Received optional record_key: %d\n", __FUNCTION__, record_key));
	        break;
	    }
	}

	if (iovar_cmd == WL_MDNS_CMD_GET_HITS) {
		uint16_t key_buf = record_key;
		uint32_t hit_count = 0;
		struct bcm_cfg80211 *cfg = wiphy_priv(wiphy);

		ret = wldev_iovar_getbuf(wdev_to_ndev(wdev),
				"mdns_hit_counter",
				&key_buf,
				sizeof(key_buf),
				cfg->ioctl_buf,
				WLC_IOCTL_MAXLEN,
				NULL);

		if (ret < 0) {
			WL_ERR(("Failed to get mdns_hit_counter\n"));
			return_value = -1;
		} else {
			memcpy(&hit_count, cfg->ioctl_buf, sizeof(hit_count));
			hit_count = dtoh32(hit_count);
			return_value = (int)(hit_count % modulus);
			WL_ERR(("hit_count=%d\n", return_value));
		}
	} else if (iovar_cmd == WL_MDNS_CMD_GET_MISSES) {
		uint32_t miss_count = 0;
		ret = wldev_iovar_getint(wdev_to_ndev(wdev), "mdns_miss_counter", &miss_count);
		if (ret < 0) {
			WL_ERR(("Failed to get mdns_miss_counter\n"));
			return_value = -1;
		} else
			return_value = (int)(miss_count % modulus);
			WL_ERR(("miss_count=%d\n", return_value));

	} else {
		WL_ERR(("%s: Invalid iovar_cmd(%u)\n", __FUNCTION__, iovar_cmd));
		return_value = -1;
	}

	skb = cfg80211_vendor_cmd_alloc_reply_skb(wiphy, ATTRIBUTE_U32_LEN + NLA_HDRLEN);
	if (unlikely(!skb)) {
	    WL_ERR(("%s: can't allocate %d bytes\n", __FUNCTION__, ATTRIBUTE_U32_LEN + NLA_HDRLEN));
	    ret = -ENOMEM;
	    goto fail;
	}

	WL_ERR(("%s: Returning value: %d\n", __FUNCTION__, return_value));
	ret = nla_put_u32(skb, MDNS_OFFLOAD_ATTR_RETURN_VALUE, return_value);
	if (unlikely(ret)) {
	    WL_ERR(("Failed to put counters info , ret=%d\n", ret));
	    goto fail;
	}

	ret = cfg80211_vendor_cmd_reply(skb);
	if (unlikely(ret)) {
	    WL_ERR(("Vendor Command reply failed ret:%d \n", ret));
	}
	return ret;

fail:
	/* Free skb memory */
	if (skb) {
	    kfree_skb(skb);
	}
	return ret;
}

int wl_cfgvendor_mdnsoffload_get_hit_counter(struct wiphy *wiphy, struct wireless_dev *wdev,
	const void *data, int len)
{
	WL_ERR(("%s: Enter\n", __FUNCTION__));
	return wl_cfgvendor_mdnsoffload_get_value_cmd(wiphy, wdev, data, len, WL_MDNS_CMD_GET_HITS);
}

int wl_cfgvendor_mdnsoffload_get_miss_counter(struct wiphy *wiphy, struct wireless_dev *wdev,
	const void *data, int len)
{
	WL_ERR(("%s: Enter\n", __FUNCTION__));
	return wl_cfgvendor_mdnsoffload_get_value_cmd(wiphy, wdev, data, len, WL_MDNS_CMD_GET_MISSES);
}

int wl_cfgvendor_mdnsoffload_add_to_passthrough(struct wiphy *wiphy, struct wireless_dev *wdev,
	const void *data, int len)
{
	const struct nlattr *iter;
	char *qname = NULL;
	int qname_len = 0;
	int rem, ret;
	struct bcm_cfg80211 *cfg = wiphy_priv(wiphy);
	void *buf;
	int payload_size = MDNS_QNAME_MAX_LEN;

	WL_ERR(("%s: Enter\n", __FUNCTION__));
	nla_for_each_attr(iter, data, len, rem) {
		if (nla_type(iter) == MDNS_OFFLOAD_ATTR_QNAME) {
			qname = nla_data(iter);
			qname_len = nla_len(iter);
			if (qname_len > MDNS_QNAME_MAX_LEN) {
				WL_ERR(("%s: Invalid qname length received (%d), exceeds max limit (%d)\n",
					__FUNCTION__, qname_len, MDNS_QNAME_MAX_LEN));
			}
			WL_ERR(("%s: Received qname: %s, len: %d\n", __FUNCTION__, qname, qname_len));
			break;
		}
	}
	if (!qname) return -EINVAL;

	/* Todo: Add the supplied QNAME to the allowlist that can forward queries Todo
	 * the main system when the offload cannot provide responses.
	 * If the addition is successful, return 0, otherwise, return -1.
	 */

	buf = kzalloc(payload_size, GFP_KERNEL);

	if (!buf)
		return -ENOMEM;

	memcpy(buf, qname, qname_len);

	ret = wldev_iovar_setbuf(wdev_to_ndev(wdev),
			"mdns_passthrough_add",
			buf,
			payload_size,
			cfg->ioctl_buf,
			WLC_IOCTL_MAXLEN,
			&cfg->ioctl_buf_sync);

	if (ret < 0)
		WL_ERR(("%s: wldev_iovar_setbuf failed, ret=%d\n",
					__FUNCTION__, ret));

	kfree(buf);
	return ret;
}

int
wl_cfgvendor_mdnsoffload_remove_from_passthrough(struct wiphy *wiphy, struct wireless_dev *wdev,
	const void *data, int len)
{
	struct bcm_cfg80211 *cfg = wiphy_priv(wiphy);
	const struct nlattr *iter;
	char *qname = NULL;
	int qname_len = 0;
	int rem;
	void *buf;
	int payload_size = MDNS_QNAME_MAX_LEN;
	int ret;

	WL_ERR(("%s: Enter\n", __FUNCTION__));
	nla_for_each_attr(iter, data, len, rem) {
		if (nla_type(iter) == MDNS_OFFLOAD_ATTR_QNAME) {
			qname = nla_data(iter);
			qname_len = nla_len(iter);
			if (qname_len > MDNS_QNAME_MAX_LEN) {
				WL_ERR(("%s: Invalid qname length received (%d), exceeds max limit (%d)\n",
					__FUNCTION__, qname_len, MDNS_QNAME_MAX_LEN));
			}
			WL_ERR(("%s: Received qname: %s, len: %d\n", __FUNCTION__, qname, qname_len));
			break;
		}
	}
	if (!qname) return -EINVAL;

	buf = kzalloc(payload_size, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	memcpy(buf, qname, qname_len);

	ret = wldev_iovar_setbuf(wdev_to_ndev(wdev),
			"mdns_passthrough_del",
			buf,
			payload_size,
			cfg->ioctl_buf,
			WLC_IOCTL_MAXLEN,
			&cfg->ioctl_buf_sync);

	kfree(buf);
	return ret;
}

int wl_cfgvendor_mdnsoffload_set_passthrough_behavior(struct wiphy *wiphy, struct wireless_dev *wdev,
	const void *data, int len)
{
	const struct nlattr *iter;
	u8 behavior = 0;
	int rem;
	int ret;

	WL_ERR(("%s: Enter\n", __FUNCTION__));
	nla_for_each_attr(iter, data, len, rem) {
	    if (nla_type(iter) == MDNS_OFFLOAD_ATTR_PASSTHROUGH_BEHAVIOR) {
	        behavior = nla_get_u8(iter);
	        WL_ERR(("%s: Received behavior value: %u\n", __FUNCTION__, behavior));
	        break;
	    }
	}

	ret = wldev_iovar_setint(wdev_to_ndev(wdev), "mdns_passthrough_mode", behavior);
	return ret;
}
