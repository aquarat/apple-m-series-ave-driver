// SPDX-License-Identifier: GPL-2.0-only
/*
 * h264_mbqp: per-frame macroblock QPs of an H.264 stream (docs/95 §12).
 *
 *   h264_mbqp STREAM.h264
 *
 * One line per decoded frame: index, picture type, slice-level QP, mean
 * macroblock QP, min, max. The QPs come from libavcodec's own H.264
 * decoder (software; export_side_data=venc_params), so it needs an FFmpeg
 * built with the native "h264" decoder (Fedora's packaged FFmpeg has only
 * OpenH264, which exports none). Build, against such an FFmpeg:
 *
 *   cc -O2 tools/h264_mbqp.c -o tools/h264_mbqp \
 *      $(pkg-config --cflags --libs libavformat libavcodec libavutil)
 *
 * (with static libraries, add --static to pkg-config). The binary is not
 * tracked.
 */
#include <stdio.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
#include <libavutil/video_enc_params.h>

static void drain(AVCodecContext *ctx, AVFrame *f, int *n)
{
	while (avcodec_receive_frame(ctx, f) == 0) {
		AVFrameSideData *sd = av_frame_get_side_data(f, AV_FRAME_DATA_VIDEO_ENC_PARAMS);
		if (!sd) { printf("%d -\n", (*n)++); continue; }
		AVVideoEncParams *ep = (AVVideoEncParams *)sd->data;
		long tot = 0; int mn = 99, mx = -99;
		for (unsigned i = 0; i < ep->nb_blocks; i++) {
			AVVideoBlockParams *b = av_video_enc_params_block(ep, i);
			int q = ep->qp + b->delta_qp;
			tot += q; if (q < mn) mn = q; if (q > mx) mx = q;
		}
		printf("%d %c %d %.3f %d %d\n", (*n)++, av_get_picture_type_char(f->pict_type), ep->qp,
		       ep->nb_blocks ? (double)tot / ep->nb_blocks : 0.0, mn, mx);
	}
}

int main(int argc, char **argv)
{
	const AVCodec *dec = avcodec_find_decoder_by_name("h264");
	AVCodecContext *ctx = avcodec_alloc_context3(dec);
	av_opt_set(ctx, "export_side_data", "venc_params", 0);
	ctx->thread_count = 1;
	if (avcodec_open2(ctx, dec, NULL) < 0) return 1;
	AVFormatContext *fmt = NULL;
	if (avformat_open_input(&fmt, argv[1], NULL, NULL) < 0) return 2;
	AVPacket *p = av_packet_alloc(); AVFrame *f = av_frame_alloc(); int n = 0;
	while (av_read_frame(fmt, p) == 0) { avcodec_send_packet(ctx, p); av_packet_unref(p); drain(ctx, f, &n); }
	avcodec_send_packet(ctx, NULL); drain(ctx, f, &n);
	return 0;
}
