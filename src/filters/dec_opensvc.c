/*
 *			GPAC - Multimedia Framework C SDK
 *
 *			Authors: Jean Le Feuvre
 *			Copyright (c) Telecom ParisTech 2010-2017
 *					All rights reserved
 *
 *  This file is part of GPAC / OpenSVC Decoder filter
 *
 *  GPAC is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU Lesser General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  GPAC is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; see the file COPYING.  If not, write to
 *  the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 */


#include <gpac/filters.h>

#ifdef GPAC_HAS_OPENSVC

#include <gpac/avparse.h>
#include <gpac/constants.h>
#include <gpac/internal/media_dev.h>


#if (defined(WIN32) || defined(_WIN32_WCE)) && !defined(__GNUC__)
#  pragma comment(lib, "OpenSVCDecoder")
#endif

#include <OpenSVCDecoder/SVCDecoder_ietr_api.h>

#define SVC_MAX_STREAMS 3

typedef struct
{
	GF_FilterPid *ipid;
	u32 cfg_crc;
	u32 id;
	u32 dep_id;
} GF_SVCStream;

typedef struct{
	u64 cts;
	u32 duration;
	u8 sap_type;
	u8 seek_flag;
} OSVCDecFrameInfo;

typedef struct
{
	GF_FilterPid *opid;
	GF_SVCStream streams[SVC_MAX_STREAMS];
	u32 nb_streams, active_streams;
	u32 width, stride, height, out_size;
	GF_Fraction pixel_ar;

	u32 nalu_size_length;

	/*OpenSVC things*/
	void *codec;
	int LimitDqId;
	int MaxDqId;
	int DqIdTable[8];
	int TemporalId;
	int TemporalCom;

	OSVCDecFrameInfo *frame_infos;
	u32 frame_infos_alloc, frame_infos_size;
	int layers[4];

} GF_OSVCDecCtx;

static GF_Err osvcdec_configure_pid(GF_Filter *filter, GF_FilterPid *pid, Bool is_remove)
{
	Bool found=GF_TRUE;
	const GF_PropertyValue *p;
	GF_M4VDecSpecInfo dsi;
	GF_Err e;
	u32 i, count, dep_id=0, id=0, cfg_crc=0;
	s32 res;
	OPENSVCFRAME Picture;
	GF_OSVCDecCtx *ctx = (GF_OSVCDecCtx*) gf_filter_get_udta(filter);

	if (is_remove) {
		if (ctx->streams[0].ipid == pid) {
			memset(ctx->streams, 0, SVC_MAX_STREAMS*sizeof(GF_SVCStream));
			if (ctx->opid) gf_filter_pid_remove(ctx->opid);
			ctx->opid = NULL;
			ctx->nb_streams = ctx->active_streams = 0;
			if (ctx->codec) SVCDecoder_close(ctx->codec);
			ctx->codec = NULL;
			return GF_OK;
		} else {
			for (i=0; i<ctx->nb_streams; i++) {
				if (ctx->streams[i].ipid == pid) {
					ctx->streams[i].ipid = NULL;
					ctx->streams[i].cfg_crc = 0;
					memmove(&ctx->streams[i], &ctx->streams[i+1], sizeof(GF_SVCStream)*(ctx->nb_streams-1));
					ctx->nb_streams--;
					ctx->active_streams--;
					return GF_OK;
				}
			}
		}
		return GF_OK;
	}
	if (! gf_filter_pid_check_caps(pid))
		return GF_NOT_SUPPORTED;

	p = gf_filter_pid_get_property(pid, GF_PROP_PID_DEPENDENCY_ID);
	if (p) dep_id = p->value.uint;

	p = gf_filter_pid_get_property(pid, GF_PROP_PID_ID);
	if (!p) p = gf_filter_pid_get_property(pid, GF_PROP_PID_ESID);
	if (p) id = p->value.uint;

	p = gf_filter_pid_get_property(pid, GF_PROP_PID_DECODER_CONFIG);
	if (p && p->value.data && p->data_len) {
		cfg_crc = gf_crc_32(p->value.data, p->data_len);
		for (i=0; i<ctx->nb_streams; i++) {
			if ((ctx->streams[i].ipid == pid) && (ctx->streams[i].cfg_crc == cfg_crc)) return GF_OK;
		}
	}

	found = GF_FALSE;
	for (i=0; i<ctx->active_streams; i++) {
		if (ctx->streams[i].ipid == pid) {
			ctx->streams[i].cfg_crc = cfg_crc;
			found = GF_TRUE;
		}
	}
	if (!found) {
		if (ctx->nb_streams==SVC_MAX_STREAMS) {
			return GF_NOT_SUPPORTED;
		}
		//insert new pid in order of dependencies
		for (i=0; i<ctx->nb_streams; i++) {

			if (!dep_id && !ctx->streams[i].dep_id) {
				GF_LOG(GF_LOG_WARNING, GF_LOG_CODEC, ("[SVC Decoder] Detected multiple independent base (%s and %s)\n", gf_filter_pid_get_name(pid), gf_filter_pid_get_name(ctx->streams[i].ipid)));
				return GF_REQUIRES_NEW_INSTANCE;
			}

			if (ctx->streams[i].id == dep_id) {
				if (ctx->nb_streams > i+2)
					memmove(&ctx->streams[i+1], &ctx->streams[i+2], sizeof(GF_SVCStream) * (ctx->nb_streams-i-1));

				ctx->streams[i+1].ipid = pid;
				ctx->streams[i+1].cfg_crc = cfg_crc;
				ctx->streams[i+1].dep_id = dep_id;
				ctx->streams[i+1].id = id;
				gf_filter_pid_set_framing_mode(pid, GF_TRUE);
				found = GF_TRUE;
				break;
			}
			if (ctx->streams[i].dep_id == id) {
				if (ctx->nb_streams > i+1)
					memmove(&ctx->streams[i+1], &ctx->streams[i], sizeof(GF_SVCStream) * (ctx->nb_streams-i));

				ctx->streams[i].ipid = pid;
				ctx->streams[i].cfg_crc = cfg_crc;
				ctx->streams[i].dep_id = dep_id;
				ctx->streams[i].id = id;
				gf_filter_pid_set_framing_mode(pid, GF_TRUE);
				found = GF_TRUE;
				break;
			}
		}
		if (!found) {
			ctx->streams[ctx->nb_streams].ipid = pid;
			ctx->streams[ctx->nb_streams].cfg_crc = cfg_crc;
			ctx->streams[ctx->nb_streams].id = id;
			ctx->streams[ctx->nb_streams].dep_id = dep_id;
			gf_filter_pid_set_framing_mode(pid, GF_TRUE);
		}
		ctx->nb_streams++;
		ctx->active_streams = ctx->nb_streams;
	}

	if (p && p->value.data) {
		GF_AVCConfig *cfg = gf_odf_avc_cfg_read(p->value.data, p->data_len);
		if (!cfg) return GF_NON_COMPLIANT_BITSTREAM;
		if (!dep_id) {
			ctx->nalu_size_length = cfg->nal_unit_size;
			if (SVCDecoder_init(&ctx->codec) == SVC_STATUS_ERROR) return GF_IO_ERR;
		}

		/*decode all NALUs*/
		count = gf_list_count(cfg->sequenceParameterSets);
		SetCommandLayer(ctx->layers, 255, 0, &res, 0);//bufindex can be reset without pb
		for (i=0; i<count; i++) {
			u32 w=0, h=0, sid;
			s32 par_n=0, par_d=0;
			GF_AVCConfigSlot *slc = (GF_AVCConfigSlot*)gf_list_get(cfg->sequenceParameterSets, i);

#ifndef GPAC_DISABLE_AV_PARSERS
			gf_avc_get_sps_info(slc->data, slc->size, &sid, &w, &h, &par_n, &par_d);
#endif
			/*by default use the base layer*/
			if (!i) {
				if ((ctx->width<w) || (ctx->height<h)) {
					ctx->width = w;
					ctx->height = h;
					if ( ((s32)par_n>0) && ((s32)par_d>0) ) {
						ctx->pixel_ar.num = par_n;
						ctx->pixel_ar.den = par_d;
					}
				}
			}
			res = decodeNAL(ctx->codec, (unsigned char *) slc->data, slc->size, &Picture, ctx->layers);
			if (res<0) {
				GF_LOG(GF_LOG_ERROR, GF_LOG_CODEC, ("[SVC Decoder] Error decoding SPS %d\n", res));
			}
			GF_LOG(GF_LOG_DEBUG, GF_LOG_CODEC, ("[SVC Decoder] Attach: SPS id=\"%d\" code=\"%d\" size=\"%d\"\n", slc->id, slc->data[0] & 0x1F, slc->size));
		}

		count = gf_list_count(cfg->pictureParameterSets);
		for (i=0; i<count; i++) {
			u32 sps_id, pps_id;
			GF_AVCConfigSlot *slc = (GF_AVCConfigSlot*)gf_list_get(cfg->pictureParameterSets, i);
			gf_avc_get_pps_info(slc->data, slc->size, &pps_id, &sps_id);
			res = decodeNAL(ctx->codec, (unsigned char *) slc->data, slc->size, &Picture, ctx->layers);
			if (res<0) {
				GF_LOG(GF_LOG_ERROR, GF_LOG_CODEC, ("[SVC Decoder] Error decoding PPS %d\n", res));
			}
			GF_LOG(GF_LOG_DEBUG, GF_LOG_CODEC, ("[SVC Decoder] Attach: PPS id=\"%d\" code=\"%d\" size=\"%d\" sps_id=\"%d\"\n", pps_id, slc->data[0] & 0x1F, slc->size, sps_id));
		}
		gf_odf_avc_cfg_del(cfg);
	} else {
		if (ctx->nalu_size_length) {
			return GF_NOT_SUPPORTED;
		}
		ctx->nalu_size_length = 0;
		if (!ctx->codec) {
			if (SVCDecoder_init(&ctx->codec) == SVC_STATUS_ERROR) return GF_IO_ERR;
			SetCommandLayer(ctx->layers, 255, 0, &res, 0);
		}
		ctx->pixel_ar = (GF_Fraction){1, 1};
	}
	ctx->stride = ctx->width + 32;
	ctx->LimitDqId = -1;
	ctx->MaxDqId = 0;
	ctx->out_size = ctx->stride * ctx->height * 3 / 2;

	if (!ctx->opid) {
		ctx->opid = gf_filter_pid_new(filter);

		gf_filter_pid_copy_properties(ctx->opid, ctx->streams[0].ipid);
		gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_OTI, &PROP_UINT(GPAC_OTI_RAW_MEDIA_STREAM) );
	}
	if (ctx->width) {
		gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_WIDTH, &PROP_UINT(ctx->width) );
		gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_HEIGHT, &PROP_UINT(ctx->height) );
		gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_STRIDE, &PROP_UINT(ctx->stride) );
		if (ctx->pixel_ar.num)
			gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_PAR, &PROP_FRAC(ctx->pixel_ar) );
		gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_PIXFMT, &PROP_UINT(GF_PIXEL_YV12) );
	}
	return GF_OK;
}


static Bool osvcdec_process_event(GF_Filter *filter, GF_FilterEvent *fevt)
{
	GF_OSVCDecCtx *ctx = (GF_OSVCDecCtx*) gf_filter_get_udta(filter);

	if (fevt->base.type == GF_FEVT_QUALITY_SWITCH) {
		if (fevt->quality_switch.up) {
			if (ctx->LimitDqId == -1)
				ctx->LimitDqId = ctx->MaxDqId;
			if (ctx->LimitDqId < ctx->MaxDqId)
				// set layer up (command=1)
				UpdateLayer( ctx->DqIdTable, &ctx->LimitDqId, &ctx->TemporalCom, &ctx->TemporalId, ctx->MaxDqId, 1 );
		} else {
			if (ctx->LimitDqId > 0)
				// set layer down (command=0)
				UpdateLayer( ctx->DqIdTable, &ctx->LimitDqId, &ctx->TemporalCom, &ctx->TemporalId, ctx->MaxDqId, 0 );
		}
		//todo: we should get the set of pids active and trigger the switch up/down based on that
		//rather than not canceling the event
		return GF_FALSE;
	}
	return GF_FALSE;
}

static void osvcdec_drop_frameinfo(GF_OSVCDecCtx *ctx)
{
	if (ctx->frame_infos_size) {
		ctx->frame_infos_size--;
		memmove(&ctx->frame_infos[0], &ctx->frame_infos[1], sizeof(OSVCDecFrameInfo)*ctx->frame_infos_size);
	}
}

static GF_Err osvcdec_process(GF_Filter *filter)
{
	s32 got_pic;
	u64 min_dts = GF_FILTER_NO_TS;
	u64 min_cts = GF_FILTER_NO_TS;
	OPENSVCFRAME pic;
	u32 i, idx, nalu_size, sc_size, nb_eos=0;
	u8 *ptr;
	u32 data_size;
	char *data;
	Bool has_pic = GF_FALSE;
	GF_OSVCDecCtx *ctx = (GF_OSVCDecCtx*) gf_filter_get_udta(filter);
	GF_FilterPacket *dst_pck, *pck_ref = NULL;

	for (idx=0; idx<ctx->active_streams; idx++) {
		u64 dts, cts;
		GF_FilterPacket *pck = gf_filter_pid_get_packet(ctx->streams[idx].ipid);
		if (!pck) {
			if (gf_filter_pid_is_eos(ctx->streams[idx].ipid)) nb_eos++;
			//make sure we do have a packet on the enhancement
			else {
				GF_LOG(GF_LOG_DEBUG, GF_LOG_CODEC, ("[OpenSVC] no input packets on running pid %s - postponing decode\n", gf_filter_pid_get_name(ctx->streams[idx].ipid) ) );
				return GF_OK;
			}
			continue;
		}
		dts = gf_filter_pck_get_dts(pck);
		cts = gf_filter_pck_get_cts(pck);

		data = gf_filter_pck_get_data(pck, &data_size);
		//TODO: this is a clock signaling, for now just trash ..
		if (!data) {
			gf_filter_pid_drop_packet(ctx->streams[idx].ipid);
			idx--;
			continue;
		}
		if (dts==GF_FILTER_NO_TS) dts = cts;
		//get packet with min dts (either a timestamp or a decode order number)
		if (min_dts > dts) {
			min_dts = dts;
			if (cts == GF_FILTER_NO_TS) min_cts = min_dts;
			else min_cts = cts;
			pck_ref = pck;
		}
	}
	if (nb_eos == ctx->active_streams) {
		gf_filter_pid_set_eos(ctx->opid);
		return GF_OK;
	}
	if (min_cts == GF_FILTER_NO_TS) return GF_OK;

	if (ctx->frame_infos_size==ctx->frame_infos_alloc) {
		ctx->frame_infos_alloc += 10;
		ctx->frame_infos = gf_realloc(ctx->frame_infos, sizeof(OSVCDecFrameInfo)*ctx->frame_infos_alloc);
	}
	//queue CTS
	if (!ctx->frame_infos_size || (ctx->frame_infos[ctx->frame_infos_size-1].cts != min_cts)) {
		for (i=0; i<ctx->frame_infos_size; i++) {
			//this is likel continuing decoding if we didn't get a frame in time from the enhancement layer
			if (ctx->frame_infos[i].cts == min_cts)
				break;

			if (ctx->frame_infos[i].cts > min_cts) {
				memmove(&ctx->frame_infos[i+1], &ctx->frame_infos[i], sizeof(OSVCDecFrameInfo) * (ctx->frame_infos_size-i));
				ctx->frame_infos[i].cts = min_cts;
				ctx->frame_infos[i].duration = gf_filter_pck_get_duration(pck_ref);
				ctx->frame_infos[i].sap_type = gf_filter_pck_get_sap(pck_ref);
				ctx->frame_infos[i].seek_flag = gf_filter_pck_get_seek_flag(pck_ref);
				break;
			}
		}
	} else {
		i = ctx->frame_infos_size;
	}

	if (i==ctx->frame_infos_size) {
		ctx->frame_infos[i].cts = min_cts;
		ctx->frame_infos[i].duration = gf_filter_pck_get_duration(pck_ref);
		ctx->frame_infos[i].sap_type = gf_filter_pck_get_sap(pck_ref);
		ctx->frame_infos[i].seek_flag = gf_filter_pck_get_seek_flag(pck_ref);
	}
	ctx->frame_infos_size++;

	pic.Width = pic.Height = 0;
	for (idx=0; idx<ctx->nb_streams; idx++) {
		u64 dts, cts;
		u32 sps_id, pps_id;
		u32 maxDqIdInAU;

		GF_FilterPacket *pck = gf_filter_pid_get_packet(ctx->streams[idx].ipid);
		if (!pck) continue;

		if (idx>=ctx->active_streams) {
			gf_filter_pid_drop_packet(ctx->streams[idx].ipid);
			continue;
		}

		dts = gf_filter_pck_get_dts(pck);
		cts = gf_filter_pck_get_cts(pck);
		if (dts==GF_FILTER_NO_TS) dts = cts;

		if (min_dts != GF_FILTER_NO_TS) {
			if (min_dts != dts) continue;
		} else if (min_cts != cts) {
			continue;
		}

		data = gf_filter_pck_get_data(pck, &data_size);

		maxDqIdInAU = GetDqIdMax((unsigned char *) data, data_size, ctx->nalu_size_length, ctx->DqIdTable, ctx->nalu_size_length ? 1 : 0);
		if (ctx->MaxDqId <= maxDqIdInAU) {
			ctx->MaxDqId = maxDqIdInAU;
		}
		GF_LOG(GF_LOG_INFO, GF_LOG_CODEC, ("[OpenSVC] decode from stream %s - DTS "LLU" PTS "LLU" size %d - max DQID %d\n", gf_filter_pid_get_name(ctx->streams[idx].ipid), dts, cts, data_size, maxDqIdInAU) );


		//we are asked to use a lower quality
		if ((ctx->LimitDqId>=0) && (ctx->LimitDqId < maxDqIdInAU))
			maxDqIdInAU = ctx->LimitDqId;

		/*decode only current layer*/
		SetCommandLayer(ctx->layers, ctx->MaxDqId, maxDqIdInAU, &ctx->TemporalCom, ctx->TemporalId);

		got_pic = 0;
		nalu_size = 0;
		ptr = (u8 *) data;
		sc_size = 0;

		if (!ctx->nalu_size_length) {
			u32 size = gf_media_nalu_next_start_code((u8 *) data, data_size, &sc_size);
			if (sc_size) {
				ptr += size+sc_size;
				assert(data_size >= size+sc_size);
				data_size -= size+sc_size;
			} else {
				/*no annex-B start-code found, discard */
				gf_filter_pid_drop_packet(ctx->streams[idx].ipid);
				idx--;
				continue;
			}
		}

		while (data_size) {
			int res;
			if (ctx->nalu_size_length) {
				for (i=0; i<ctx->nalu_size_length; i++) {
					nalu_size = (nalu_size<<8) + ptr[i];
				}
				ptr += ctx->nalu_size_length;
			} else {
				nalu_size = gf_media_nalu_next_start_code(ptr, data_size, &sc_size);
			}

#ifndef GPAC_DISABLE_LOG
			if (gf_log_tool_level_on(GF_LOG_CODEC, GF_LOG_DEBUG)) {
				switch (ptr[0] & 0x1F) {
				case GF_AVC_NALU_SEQ_PARAM:
				case GF_AVC_NALU_SVC_SUBSEQ_PARAM:
					gf_avc_get_sps_info((char *)ptr, nalu_size, &sps_id, NULL, NULL, NULL, NULL);
					GF_LOG(GF_LOG_DEBUG, GF_LOG_CODEC, ("[SVCDec] PID %s: SPS id=\"%d\" code=\"%d\" size=\"%d\"\n", gf_filter_pid_get_name(ctx->streams[idx].ipid), sps_id, ptr[0] & 0x1F, nalu_size));
					break;
				case GF_AVC_NALU_PIC_PARAM:
					gf_avc_get_pps_info((char *)ptr, nalu_size, &pps_id, &sps_id);
					GF_LOG(GF_LOG_DEBUG, GF_LOG_CODEC, ("[SVC Decoder] PID %s: PPS id=\"%d\" code=\"%d\" size=\"%d\" sps_id=\"%d\"\n", gf_filter_pid_get_name(ctx->streams[idx].ipid), pps_id, ptr[0] & 0x1F, nalu_size, sps_id));
					break;
				case GF_AVC_NALU_VDRD:
					GF_LOG(GF_LOG_DEBUG, GF_LOG_CODEC, ("[SVC Decoder] PID %s: VDRD found\n", gf_filter_pid_get_name(ctx->streams[idx].ipid)));
					break;
				default:
					GF_LOG(GF_LOG_DEBUG, GF_LOG_CODEC, ("[SVC Decoder] PID %s: NALU code=\"%d\" size=\"%d\"\n", gf_filter_pid_get_name(ctx->streams[idx].ipid), ptr[0] & 0x1F, nalu_size));
				}
			}
#endif

			if (!got_pic) {
				res = decodeNAL(ctx->codec, ptr, nalu_size, &pic, ctx->layers);
				if (res>0) got_pic = res;
			} else {
				res = decodeNAL(ctx->codec, ptr, nalu_size, &pic, ctx->layers);
			}
			if (res<0) {
				GF_LOG(GF_LOG_ERROR, GF_LOG_CODEC, ("[SVC Decoder] Error decoding NAL: %d\n", res));
			}

			ptr += nalu_size;
			if (ctx->nalu_size_length) {
				if (data_size < nalu_size + ctx->nalu_size_length) break;
				data_size -= nalu_size + ctx->nalu_size_length;
			} else {
				if (!sc_size || (data_size < nalu_size + sc_size)) break;
				data_size -= nalu_size + sc_size;
				ptr += sc_size;
			}
		}
		gf_filter_pid_drop_packet(ctx->streams[idx].ipid);

		if (got_pic) has_pic = GF_TRUE;
	}
	if (!has_pic) return GF_OK;

	if ((pic.Width != ctx->width) || (pic.Height!=ctx->height)) {
		GF_LOG(GF_LOG_INFO, GF_LOG_CODEC, ("[SVC Decoder] Resizing from %dx%d to %dx%d\n", ctx->width, ctx->height, pic.Width, pic.Height ));
		ctx->width = pic.Width;
		ctx->stride = pic.Width + 32;
		ctx->height = pic.Height;
		ctx->out_size = ctx->stride * ctx->height * 3 / 2;

		gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_WIDTH, &PROP_UINT(ctx->width) );
		gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_HEIGHT, &PROP_UINT(ctx->height) );
		gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_STRIDE, &PROP_UINT(ctx->stride) );
		if (ctx->pixel_ar.num)
			gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_PAR, &PROP_FRAC(ctx->pixel_ar) );

		gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_PIXFMT, &PROP_UINT(GF_PIXEL_YV12) );
	}

	if (ctx->frame_infos[0].seek_flag) {
		osvcdec_drop_frameinfo(ctx);
		return GF_OK;
	}

	dst_pck = gf_filter_pck_new_alloc(ctx->opid, ctx->out_size, &data);
	memcpy(data, pic.pY[0], ctx->stride*ctx->height);
	memcpy(data + ctx->stride * ctx->height, pic.pU[0], ctx->stride*ctx->height/4);
	memcpy(data + 5*ctx->stride * ctx->height/4, pic.pV[0], ctx->stride*ctx->height/4);

	gf_filter_pck_set_cts(dst_pck, ctx->frame_infos[0].cts);
	gf_filter_pck_set_sap(dst_pck, ctx->frame_infos[0].sap_type);
	gf_filter_pck_set_duration(dst_pck, ctx->frame_infos[0].duration);

	GF_LOG(GF_LOG_DEBUG, GF_LOG_CODEC, ("[OpenSVC] decoded out frame PTS "LLU"\n", ctx->frame_infos[0].cts) );
	gf_filter_pck_send(dst_pck);

	osvcdec_drop_frameinfo(ctx);
	return GF_OK;
}

static void osvcdec_finalize(GF_Filter *filter)
{
	GF_OSVCDecCtx *ctx = (GF_OSVCDecCtx*) gf_filter_get_udta(filter);
	if (ctx->codec) SVCDecoder_close(ctx->codec);
	if (ctx->frame_infos) gf_free(ctx->frame_infos);
}

static const GF_FilterCapability OSVCDecInputs[] =
{
	CAP_INC_UINT(GF_PROP_PID_STREAM_TYPE, GF_STREAM_VISUAL),
	CAP_EXC_BOOL(GF_PROP_PID_UNFRAMED, GF_TRUE),
	CAP_INC_UINT(GF_PROP_PID_OTI, GPAC_OTI_VIDEO_AVC),
	CAP_INC_UINT(GF_PROP_PID_OTI, GPAC_OTI_VIDEO_SVC),
};

static const GF_FilterCapability OSVCDecOutputs[] =
{
	CAP_INC_UINT(GF_PROP_PID_STREAM_TYPE, GF_STREAM_VISUAL),
	CAP_INC_UINT(GF_PROP_PID_OTI, GPAC_OTI_RAW_MEDIA_STREAM),
};

GF_FilterRegister OSVCDecRegister = {
	.name = "osvcdec",
	.description = "OpenSVC decoder",
	.private_size = sizeof(GF_OSVCDecCtx),
	INCAPS(OSVCDecInputs),
	OUTCAPS(OSVCDecOutputs),
	.finalize = osvcdec_finalize,
	.configure_pid = osvcdec_configure_pid,
	.process = osvcdec_process,
	.process_event = osvcdec_process_event,
	.max_extra_pids = (SVC_MAX_STREAMS-1),
	.priority = 255
};

#endif //GPAC_HAS_OPENSVC

const GF_FilterRegister *osvcdec_register(GF_FilterSession *session)
{
#ifdef GPAC_HAS_OPENSVC
	return &OSVCDecRegister;
#else
	return NULL;
#endif
}
