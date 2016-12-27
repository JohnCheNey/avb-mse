/*************************************************************************/ /*
 avb-mse

 Copyright (C) 2016 Renesas Electronics Corporation

 License        Dual MIT/GPLv2

 The contents of this file are subject to the MIT license as set out below.

 Permission is hereby granted, free of charge, to any person obtaining a copy
 of this software and associated documentation files (the "Software"), to deal
 in the Software without restriction, including without limitation the rights
 to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 copies of the Software, and to permit persons to whom the Software is
 furnished to do so, subject to the following conditions:

 The above copyright notice and this permission notice shall be included in
 all copies or substantial portions of the Software.

 Alternatively, the contents of this file may be used under the terms of
 the GNU General Public License Version 2 ("GPL") in which case the provisions
 of GPL are applicable instead of those above.

 If you wish to allow use of your version of this file only under the terms of
 GPL, and not to allow others to use your version of this file under the terms
 of the MIT license, indicate your decision by deleting the provisions above
 and replace them with the notice and other provisions required by GPL as set
 out in the file called "GPL-COPYING" included in this distribution. If you do
 not delete the provisions above, a recipient may use your version of this file
 under the terms of either the MIT license or GPL.

 This License is also included in this distribution in the file called
 "MIT-COPYING".

 EXCEPT AS OTHERWISE STATED IN A NEGOTIATED AGREEMENT: (A) THE SOFTWARE IS
 PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING
 BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR
 PURPOSE AND NONINFRINGEMENT; AND (B) IN NO EVENT SHALL THE AUTHORS OR
 COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.


 GPLv2:
 If you wish to use this file under the terms of GPL, following terms are
 effective.

 This program is free software; you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation; version 2 of the License.

 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with this program; if not, write to the Free Software
 Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
*/ /*************************************************************************/

#undef pr_fmt
#define pr_fmt(fmt) KBUILD_MODNAME "/" fmt

#include <linux/init.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/kernel.h>
#include <uapi/linux/if_ether.h>

#include "ravb_mse_kernel.h"
#include "mse_core.h"
#include "mse_packetizer.h"
#include "avtp.h"
#include "jpeg.h"

#define NSEC                    (1000000000L)
#define SEQNUM_INIT             (-1)

#define ETHFRAMELEN_MAX_IPG     (ETHFRAMELEN_MAX + 12)
#define AVTP_PAYLOAD_MAX        (ETHFRAMELEN_MAX - AVTP_PAYLOAD_OFFSET)

#define TRANSMIT_RATE_BASE      (1000000)

#define MSE_PACKETIZER_MAX      (10)

struct avtp_cvf_mjpeg_param {
	char dest_addr[MSE_MAC_LEN_MAX];
	char source_addr[MSE_MAC_LEN_MAX];
	int payload_size;
	int uniqueid;
	int priority;
	int vid;
};

struct cvf_mjpeg_packetizer {
	bool used_f;
	bool header_f;
	bool sos_f;
	bool dqt_f;
	bool sof_f;
	bool dri_f;
	bool eoi_f;

	int send_seq_num;
	int old_seq_num;
	int seq_num_err;

	enum MJPEG_TYPE type;
	u8 quant;
	u8 max_comp;
	int width;
	int height;

	size_t eoi_offset;
	size_t jpeg_offset;

	u8 packet_template[ETHFRAMELEN_MAX];

	struct mse_network_config net_config;
	struct mse_video_config video_config;
};

struct cvf_mjpeg_packetizer cvf_mjpeg_packetizer_table[MSE_PACKETIZER_MAX];

static int mse_packetizer_cvf_mjpeg_open(void)
{
	struct cvf_mjpeg_packetizer *cvf_mjpeg;
	int index;

	for (index = 0; cvf_mjpeg_packetizer_table[index].used_f &&
	     index < ARRAY_SIZE(cvf_mjpeg_packetizer_table); index++)
		;

	if (index >= ARRAY_SIZE(cvf_mjpeg_packetizer_table))
		return -EPERM;

	cvf_mjpeg = &cvf_mjpeg_packetizer_table[index];

	cvf_mjpeg->used_f = true;
	cvf_mjpeg->send_seq_num = 0;
	cvf_mjpeg->old_seq_num = SEQNUM_INIT;
	cvf_mjpeg->seq_num_err = SEQNUM_INIT;

	pr_debug("[%s] index=%d\n", __func__, index);

	return index;
}

static int mse_packetizer_cvf_mjpeg_release(int index)
{
	struct cvf_mjpeg_packetizer *cvf_mjpeg;

	if (index >= ARRAY_SIZE(cvf_mjpeg_packetizer_table))
		return -EPERM;

	cvf_mjpeg = &cvf_mjpeg_packetizer_table[index];
	memset(cvf_mjpeg, 0, sizeof(*cvf_mjpeg));

	pr_debug("[%s] index=%d\n", __func__, index);

	return 0;
}

static void mse_packetizer_cvf_mjpeg_flag_init(
					struct cvf_mjpeg_packetizer *cvf_mjpeg)
{
	cvf_mjpeg->header_f = true;
	cvf_mjpeg->sos_f = false;
	cvf_mjpeg->dqt_f = false;
	cvf_mjpeg->sof_f = false;
	cvf_mjpeg->dri_f = false;
	cvf_mjpeg->eoi_f = false;
	cvf_mjpeg->max_comp = 0;
	cvf_mjpeg->eoi_offset = 0;
	cvf_mjpeg->jpeg_offset = 0;
}

static int mse_packetizer_cvf_mjpeg_packet_init(int index)
{
	struct cvf_mjpeg_packetizer *cvf_mjpeg;

	if (index >= ARRAY_SIZE(cvf_mjpeg_packetizer_table))
		return -EPERM;

	pr_debug("[%s] index=%d\n", __func__, index);

	cvf_mjpeg = &cvf_mjpeg_packetizer_table[index];

	cvf_mjpeg->send_seq_num = 0;
	cvf_mjpeg->old_seq_num = SEQNUM_INIT;
	cvf_mjpeg->seq_num_err = SEQNUM_INIT;

	cvf_mjpeg->quant = MJPEG_QUANT_DYNAMIC;
	cvf_mjpeg->type = MJPEG_TYPE_420;

	mse_packetizer_cvf_mjpeg_flag_init(cvf_mjpeg);

	return 0;
}

static int mse_packetizer_cvf_mjpeg_set_network_config(
					int index,
					struct mse_network_config *config)
{
	struct cvf_mjpeg_packetizer *cvf_mjpeg;

	if (index >= ARRAY_SIZE(cvf_mjpeg_packetizer_table))
		return -EPERM;

	pr_debug("[%s] index=%d\n", __func__, index);

	cvf_mjpeg = &cvf_mjpeg_packetizer_table[index];
	cvf_mjpeg->net_config = *config;

	return 0;
}

static int mse_packetizer_cvf_mjpeg_header_build(
					void *dst,
					struct avtp_cvf_mjpeg_param *param)
{
	int hlen, len;
	u8 cfi;
	u8 streamid[AVTP_STREAMID_SIZE];

	memset(dst, 0, ETHFRAMELEN_MAX);   /* clear MAC frame buffer */

	/* Ethernet frame header */
	set_ieee8021q_dest(dst, (u8 *)param->dest_addr);
	set_ieee8021q_source(dst, (u8 *)param->source_addr);

	/* IEEE802.1Q Q-tag */
	cfi = 0;

	set_ieee8021q_tpid(dst, ETH_P_8021Q);
	/* pcp:3bit, cfi:1bit, vid:12bit */
	set_ieee8021q_tci(dst,
			  (param->priority << 13) | (cfi << 12) | param->vid);
	set_ieee8021q_ethtype(dst, ETH_P_1722);

	hlen = AVTP_CVF_MJPEG_PAYLOAD_OFFSET;
	len = param->payload_size;

	/* 1722 header update + payload */
	mse_make_streamid(streamid, param->source_addr, param->uniqueid);

	avtp_copy_cvf_mjpeg_template(dst);

	avtp_set_stream_id(dst, streamid);
	avtp_set_stream_data_length(dst, len);

	return hlen + len;
}

static int mse_packetizer_cvf_mjpeg_set_video_config(
					int index,
					struct mse_video_config *config)
{
	struct cvf_mjpeg_packetizer *cvf_mjpeg;
	struct avtp_cvf_mjpeg_param param;
	struct mse_network_config *net_config;

	if (index >= ARRAY_SIZE(cvf_mjpeg_packetizer_table))
		return -EPERM;

	pr_debug("[%s] index=%d\n", __func__, index);

	cvf_mjpeg = &cvf_mjpeg_packetizer_table[index];
	cvf_mjpeg->video_config = *config;
	net_config = &cvf_mjpeg->net_config;

	memcpy(param.dest_addr, net_config->dest_addr, MSE_MAC_LEN_MAX);
	memcpy(param.source_addr, net_config->source_addr, MSE_MAC_LEN_MAX);

	param.uniqueid = net_config->uniqueid;
	param.priority = net_config->priority;
	param.vid = net_config->vlanid;
	param.payload_size = 0;

	mse_packetizer_cvf_mjpeg_header_build(cvf_mjpeg->packet_template,
					      &param);

	return 0;
}

static int mse_packetizer_cvf_mjpeg_calc_cbs(int index,
					     struct mse_cbsparam *cbs)
{
	struct cvf_mjpeg_packetizer *cvf_mjpeg;
	struct mse_network_config *net_config;
	u64 value;
	u64 bandwidth_fraction_denominator, bandwidth_fraction_numerator;

	if (index >= ARRAY_SIZE(cvf_mjpeg_packetizer_table))
		return -EPERM;

	pr_debug("[%s] index=%d\n", __func__, index);
	cvf_mjpeg = &cvf_mjpeg_packetizer_table[index];
	net_config = &cvf_mjpeg->net_config;

	bandwidth_fraction_denominator =
		(u64)net_config->port_transmit_rate / TRANSMIT_RATE_BASE;

	if (!bandwidth_fraction_denominator) {
		pr_err("[%s] Link speed %lu bps is not support\n",
		       __func__, net_config->port_transmit_rate);
		return -EPERM;
	}

	bandwidth_fraction_numerator = (u64)cvf_mjpeg->video_config.bitrate *
				       (u64)ETHFRAMELEN_MAX_IPG;
	do_div(bandwidth_fraction_numerator, TRANSMIT_RATE_BASE);
	value = (u64)UINT_MAX * bandwidth_fraction_numerator;
	do_div(value, bandwidth_fraction_denominator);
	do_div(value, AVTP_PAYLOAD_MAX);
	if (value > UINT_MAX) {
		pr_err("[%s] cbs error value=0x%016llx\n", __func__, value);
		return -EPERM;
	}

	cbs->bandwidth_fraction = (u32)value;

	value = (u64)USHRT_MAX * bandwidth_fraction_numerator;
	do_div(value, bandwidth_fraction_denominator);
	do_div(value, AVTP_PAYLOAD_MAX);
	cbs->send_slope = (u32)value;

	value = (u64)USHRT_MAX * (bandwidth_fraction_denominator *
		(u64)AVTP_PAYLOAD_MAX - bandwidth_fraction_numerator);
	do_div(value, bandwidth_fraction_denominator);
	do_div(value, AVTP_PAYLOAD_MAX);
	cbs->idle_slope = (u32)value;

	return 0;
}

static int mse_packetizer_cvf_mjpeg_packetize(int index,
					      void *packet,
					      size_t *packet_size,
					      void *buffer,
					      size_t buffer_size,
					      size_t *buffer_processed,
					      unsigned int *timestamp)
{
	struct cvf_mjpeg_packetizer *cvf_mjpeg;
	struct mjpeg_restart_header rheader;
	struct mjpeg_quant_header qheader;
	struct mjpeg_quant_table qtable[JPEG_QUANT_NUM];
	struct mjpeg_component comp[JPEG_COMP_NUM];
	u8 *buf, *payload;
	size_t offset = 0, data_len, end_len = 0;
	size_t payload_size;
	u32 header_len = 0, quant_len = 0;
	int i, ret;

	if (index >= ARRAY_SIZE(cvf_mjpeg_packetizer_table))
		return -EPERM;

	cvf_mjpeg = &cvf_mjpeg_packetizer_table[index];

	pr_debug("[%s] index=%d seqnum=%d process=%zu/%zu t=%d\n",
		 __func__, index, cvf_mjpeg->send_seq_num, *buffer_processed,
		 buffer_size, *timestamp);

	buf = (u8 *)(buffer + *buffer_processed);
	data_len = buffer_size - *buffer_processed;
	memset(qtable, 0, sizeof(qtable));
	memset(comp, 0, sizeof(comp));

	if (*buffer_processed == 0)
		cvf_mjpeg->header_f = true;

	while (offset < data_len && cvf_mjpeg->header_f &&
	       !cvf_mjpeg->eoi_f && !cvf_mjpeg->sos_f) {
		u8 mk;

		mk = jpeg_get_marker(buf, data_len, &offset);
		switch (mk) {
		case JPEG_MARKER_KIND_NIL:
		case JPEG_MARKER_KIND_SOI:
			break;

		case JPEG_MARKER_KIND_SOF0:
			ret = jpeg_read_sof(buf,
					    data_len,
					    &offset,
					    &cvf_mjpeg->type,
					    &cvf_mjpeg->max_comp,
					    comp,
					    qtable,
					    ARRAY_SIZE(qtable),
					    &cvf_mjpeg->height,
					    &cvf_mjpeg->width);
			if (ret) {
				pr_err("[%s] invalid SOF0\n", __func__);
				goto header_error;
			}
			cvf_mjpeg->sof_f = true;
			break;

		case JPEG_MARKER_KIND_EOI:
			cvf_mjpeg->eoi_offset = offset;
			cvf_mjpeg->eoi_f = true;
			break;

		case JPEG_MARKER_KIND_SOS:
			header_len = offset +
				     JPEG_GET_HEADER_SIZE(buf, offset);
			cvf_mjpeg->sos_f = true;
			break;

		case JPEG_MARKER_KIND_DQT:
			ret = jpeg_read_dqt(buf, data_len, &offset, qtable);
			if (ret) {
				pr_err("[%s] invalid DQT\n", __func__);
				goto header_error;
			}
			cvf_mjpeg->dqt_f = true;
			break;

		case JPEG_MARKER_KIND_DRI:
			ret = jpeg_read_dri(buf, data_len, &offset, &rheader);
			if (ret < 0) {
				pr_err("[%s] invalid DRI\n", __func__);
				goto header_error;
			} else if (ret) {
				cvf_mjpeg->dri_f = true;
				cvf_mjpeg->type |= MJPEG_TYPE_RESTART_BIT;
			}
			break;

		default:
			pr_debug("[%s] skip marker 0x%X\n", __func__, mk);
			offset += JPEG_GET_HEADER_SIZE(buf, offset);
			break;
		}
	}

	if (!cvf_mjpeg->dqt_f || !cvf_mjpeg->sof_f) {
		pr_err("[%s] Not support JPEG format sof=%d dqt=%d\n",
		       __func__, cvf_mjpeg->dqt_f, cvf_mjpeg->sof_f);
		goto header_error;
	}

	/* Search EOI */
	if (!cvf_mjpeg->eoi_f) {
		cvf_mjpeg->eoi_f = true;
		cvf_mjpeg->eoi_offset = data_len;
		for (i = offset; i < data_len; i++) {
			if (buf[i] == 0xFF &&
			    buf[i + 1] == JPEG_MARKER_KIND_EOI) {
				cvf_mjpeg->eoi_offset = i + 2;
			}
		}
	}

	payload_size = ETH_FRAME_LEN - AVTP_CVF_MJPEG_PAYLOAD_OFFSET;
	data_len = payload_size;

	if (cvf_mjpeg->quant >= MJPEG_QUANT_QTABLE_BIT &&
	    !cvf_mjpeg->jpeg_offset) {
		memset(&qheader, 0, sizeof(qheader));

		for (i = 0; i <= cvf_mjpeg->max_comp; i++) {
			u8 qlen, qid;

			qid = comp[i].qt;
			if (qid >= ARRAY_SIZE(qtable)) {
				pr_err("[%s] Invalid qid=%d\n", __func__, qid);
				goto header_error;
			}

			qlen = qtable[qid].size;
			if (!qlen) {
				pr_err("[%s] Invalid qlen=0\n", __func__);
				goto header_error;
			}

			qheader.precision |= (qlen == JPEG_DQT_QUANT_SIZE8) ?
					     0 : (1 << i);
			quant_len += qlen;
		}
		qheader.length = htons(quant_len);
	}

	/* set header */
	memcpy(packet, cvf_mjpeg->packet_template,
	       AVTP_CVF_MJPEG_PAYLOAD_OFFSET);
	avtp_set_sequence_num(packet, cvf_mjpeg->send_seq_num++);
	avtp_set_timestamp(packet, (u32)*timestamp);
	avtp_set_cvf_mjpeg_tspec(packet, 0);
	avtp_set_cvf_mjpeg_offset(packet, cvf_mjpeg->jpeg_offset);
	avtp_set_cvf_mjpeg_type(packet, cvf_mjpeg->type);
	avtp_set_cvf_mjpeg_q(packet, cvf_mjpeg->quant);
	avtp_set_cvf_mjpeg_width(packet, cvf_mjpeg->width);
	avtp_set_cvf_mjpeg_height(packet, cvf_mjpeg->height);

	payload = packet + AVTP_CVF_MJPEG_PAYLOAD_OFFSET;

	if (cvf_mjpeg->dri_f) {
		memcpy(payload, &rheader, sizeof(rheader));
		payload += sizeof(rheader);
		data_len -= sizeof(rheader);
	}

	/* only first packet */
	if (quant_len > 0) {
		memcpy(payload, &qheader, sizeof(qheader));
		payload += sizeof(qheader);
		data_len -= sizeof(qheader);

		for (i = 0; i <= cvf_mjpeg->max_comp; i++) {
			u8 qlen, qid;

			qid = comp[i].qt;
			qlen = qtable[qid].size;
			memcpy(payload, qtable[qid].data, qlen);

			pr_debug("[%s] component %d id=%d len=%d\n",
				 __func__, i, qid, qlen);

			payload += qlen;
		}
		data_len -= quant_len;
	}

	buf += header_len;
	*buffer_processed += header_len;

	if (cvf_mjpeg->eoi_f)
		/* length of EOI marker */
		end_len = cvf_mjpeg->eoi_offset - *buffer_processed;
	else
		/* length of buffer */
		end_len = buffer_size - *buffer_processed;

	/* adjustment end packet */
	if (data_len >= end_len) {
		if (cvf_mjpeg->eoi_f) {
			pr_debug("[%s] last frame seq=%d\n",
				 __func__, cvf_mjpeg->send_seq_num - 1);
			/* M bit */
			avtp_set_cvf_m(packet, true);
		}
		avtp_set_stream_data_length(
					packet,
					payload_size - (data_len - end_len));
		data_len = end_len;
	} else {
		avtp_set_stream_data_length(packet, payload_size);
	}

	/* set jpeg data */
	memcpy(payload, buf, data_len);

	/* set packet length */
	*packet_size = AVTP_CVF_MJPEG_PAYLOAD_OFFSET +
		avtp_get_stream_data_length(packet);

	/* read buffer length */
	*buffer_processed += data_len;

	if (cvf_mjpeg->eoi_f && !(cvf_mjpeg->eoi_offset - *buffer_processed))
		/* jpeg data end */
		mse_packetizer_cvf_mjpeg_flag_init(cvf_mjpeg);
	else
		cvf_mjpeg->jpeg_offset += data_len;

	/* buffer end */
	if (*buffer_processed == buffer_size)
		return MSE_PACKETIZE_STATUS_COMPLETE;
	else
		return MSE_PACKETIZE_STATUS_CONTINUE;

header_error:
	/* find next header */
	mse_packetizer_cvf_mjpeg_flag_init(cvf_mjpeg);

	return -EPERM;
}

static int mse_packetizer_cvf_mjpeg_depacketize(int index,
						void *buffer,
						size_t buffer_size,
						size_t *buffer_processed,
						unsigned int *timestamp,
						void *packet,
						size_t packet_size)
{
	struct cvf_mjpeg_packetizer *cvf_mjpeg;
	struct mjpeg_restart_header *rheader;
	struct mjpeg_quant_header qheader;
	u8 *data, *qt, tspec;
	int seq_num;
	size_t data_len;
	u16 dri = 0;
	u32 offset, width, height;

	if (index >= ARRAY_SIZE(cvf_mjpeg_packetizer_table))
		return -EPERM;

	pr_debug("[%s] index=%d\n", __func__, index);

	cvf_mjpeg = &cvf_mjpeg_packetizer_table[index];

	if (avtp_get_subtype(packet) != AVTP_SUBTYPE_CVF) {
		pr_err("[%s] error subtype=%d\n",
		       __func__, avtp_get_subtype(packet));
		return -EINVAL;
	}

	/* seq_num check */
	seq_num = avtp_get_sequence_num(packet);
	if (cvf_mjpeg->old_seq_num != seq_num &&
	    cvf_mjpeg->old_seq_num != SEQNUM_INIT) {
		if (cvf_mjpeg->seq_num_err == SEQNUM_INIT) {
			pr_err("sequence number discontinuity %d->%d=%d\n",
			       cvf_mjpeg->old_seq_num, seq_num, (seq_num + 1 +
			       AVTP_SEQUENCE_NUM_MAX - cvf_mjpeg->old_seq_num) %
			       (AVTP_SEQUENCE_NUM_MAX + 1));
			cvf_mjpeg->seq_num_err = 1;
		} else {
			cvf_mjpeg->seq_num_err++;
		}
	} else {
		if (cvf_mjpeg->seq_num_err != SEQNUM_INIT) {
			pr_err("sequence number recovery %d count=%d\n",
			       seq_num, cvf_mjpeg->seq_num_err);
			cvf_mjpeg->seq_num_err = SEQNUM_INIT;
		}
	}
	cvf_mjpeg->old_seq_num = (seq_num + 1 + (AVTP_SEQUENCE_NUM_MAX + 1)) %
				 (AVTP_SEQUENCE_NUM_MAX + 1);

	data = (u8 *)packet + AVTP_CVF_MJPEG_PAYLOAD_OFFSET;
	data_len = avtp_get_stream_data_length(packet);
	*timestamp = avtp_get_timestamp(packet);

	tspec = avtp_get_cvf_mjpeg_tspec(packet);
	offset = avtp_get_cvf_mjpeg_offset(packet);
	cvf_mjpeg->type = avtp_get_cvf_mjpeg_type(packet);
	cvf_mjpeg->quant = avtp_get_cvf_mjpeg_q(packet);
	/* convert from blocks to pixels */
	width = avtp_get_cvf_mjpeg_width(packet) * PIXEL_DIV_NUM;
	height = avtp_get_cvf_mjpeg_height(packet) * PIXEL_DIV_NUM;
	if (!width || !height) {
		pr_err("[%s] error widthxheight=%ux%u\n",
		       __func__, width, height);
		return -EPERM;
	}

	pr_debug("[%s] tspec=%u, offset=%u, type=%u, quant=%u, pixel=%ux%u\n",
		 __func__, tspec, offset, cvf_mjpeg->type, cvf_mjpeg->quant,
		 width, height);

	if (cvf_mjpeg->type >= MJPEG_TYPE_RESTART_BIT) {
		rheader = (struct mjpeg_restart_header *)data;
		dri = ntohs(rheader->restart_interval);
		pr_debug("[%s] restart interval=%d\n", __func__, dri);
		data += sizeof(struct mjpeg_restart_header);
		data_len -= sizeof(struct mjpeg_restart_header);
	}

	if ((cvf_mjpeg->quant & MJPEG_QUANT_QTABLE_BIT) && !offset) {
		memcpy(&qheader, data, sizeof(qheader));
		if (cvf_mjpeg->quant == MJPEG_QUANT_DYNAMIC &&
		    !ntohs(qheader.length))
			return -EPERM;

		data += sizeof(struct mjpeg_quant_header);
		data_len -= sizeof(struct mjpeg_quant_header);

		qt = data;
		data += ntohs(qheader.length);
		data_len -= ntohs(qheader.length);
	} else {
		memset(&qheader, 0, sizeof(qheader));
		qt = NULL;
	}

	/* make header for first data */
	if (!offset) {
		u8 header[1024];
		u32 len;

		memset(header, 0, sizeof(header));
		len = jpeg_make_header(cvf_mjpeg->type,
				       cvf_mjpeg->quant,
				       header,
				       width,
				       height,
				       qt,
				       &qheader,
				       dri);

		if (*buffer_processed + len >= buffer_size) {
			pr_err("[%s] buffer overrun header\n", __func__);
			return -EPERM;
		}

		memcpy(buffer + *buffer_processed, header, len);
		*buffer_processed += len;
	}

	if (*buffer_processed + data_len >= buffer_size) {
		pr_err("[%s] buffer overrun data\n", __func__);
		return -EPERM;
	}

	/* data copy */
	memcpy(buffer + *buffer_processed, data, data_len);
	*buffer_processed += data_len;

	pr_debug("[%s] data_len=%zu processed=%zu\n",
		 __func__, data_len, *buffer_processed);

	/* for debug */
	avtp_set_sequence_num(packet, 0);

	/* TODO buffer over check */
	if (!avtp_get_cvf_m(packet))
		return MSE_PACKETIZE_STATUS_CONTINUE;

	pr_info("[%s] M bit enable seq=%d size=%zu/%zu\n", __func__,
		cvf_mjpeg->old_seq_num - 1, *buffer_processed, buffer_size);

	return MSE_PACKETIZE_STATUS_COMPLETE;
}

struct mse_packetizer_ops mse_packetizer_cvf_mjpeg_ops = {
	.id = MSE_PACKETIZER_CVF_MJPEG,
	.open = mse_packetizer_cvf_mjpeg_open,
	.release = mse_packetizer_cvf_mjpeg_release,
	.init = mse_packetizer_cvf_mjpeg_packet_init,
	.set_network_config = mse_packetizer_cvf_mjpeg_set_network_config,
	.set_video_config = mse_packetizer_cvf_mjpeg_set_video_config,
	.calc_cbs = mse_packetizer_cvf_mjpeg_calc_cbs,
	.packetize = mse_packetizer_cvf_mjpeg_packetize,
	.depacketize = mse_packetizer_cvf_mjpeg_depacketize,
};
