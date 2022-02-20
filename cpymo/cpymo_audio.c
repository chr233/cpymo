#include "cpymo_audio.h"
#include <assert.h>
#include <cpymo_backend_audio.h>
#include "cpymo_engine.h"

static void cpymo_audio_channel_reset_unsafe(cpymo_audio_channel *c)
{
	if (c->swr_context) swr_free(&c->swr_context);
	if (c->codec_context) avcodec_free_context(&c->codec_context);
	if (c->format_context) avformat_close_input(&c->format_context);
	if (c->io_context) {
		void *buf = c->io_context->buffer;
		avio_context_free(&c->io_context);
		if (buf) av_free(buf);
	}

	cpymo_audio_channel_init(c);
}

void cpymo_audio_channel_reset(cpymo_audio_channel *c)
{
	cpymo_backend_audio_lock();
	c->enabled = false;
	cpymo_backend_audio_unlock();

	cpymo_audio_channel_reset_unsafe(c);
}

static int cpymo_audio_fmt2ffmpeg(
	int f) {
	switch (f) {
	case cpymo_backend_audio_s16: return AV_SAMPLE_FMT_S16;
	case cpymo_backend_audio_s32: return AV_SAMPLE_FMT_S32;
	case cpymo_backend_audio_f32: return AV_SAMPLE_FMT_FLT;
	}

	assert(false);
	return -1;
}

static error_t cpymo_audio_channel_grow_convert_buffer(
	cpymo_audio_channel *c, 
	size_t samples,
	const cpymo_backend_audio_info *info)
{
	const size_t size = av_samples_get_buffer_size(
		NULL,
		(int)info->channels,
		(int)samples,
		cpymo_audio_fmt2ffmpeg(info->format),
		1);

	if (size > c->converted_buf_all_size) {
		c->converted_buf = (uint8_t *)realloc(c->converted_buf, size);
		if (c->converted_buf == NULL) {
			cpymo_audio_channel_reset_unsafe(c);
			return CPYMO_ERR_OUT_OF_MEM;
		}
		c->converted_buf_all_size = size;
	}

	return CPYMO_ERR_SUCC;
}

static error_t cpymo_audio_channel_flush_converter(cpymo_audio_channel *c)
{
	const cpymo_backend_audio_info *info = cpymo_backend_audio_get_info();

	const size_t flush_buffer_samples = 512;
	error_t err = cpymo_audio_channel_grow_convert_buffer(c, flush_buffer_samples, info);
	CPYMO_THROW(err);

	int samples = swr_convert(
		c->swr_context,
		&c->converted_buf,
		(int)flush_buffer_samples,
		NULL,
		0);

	if (samples == 0) return CPYMO_ERR_NO_MORE_CONTENT;
	if (samples < 0) {
		printf("[Warning] swr_convert: %s.", av_err2str(samples));
		return CPYMO_ERR_UNKNOWN;
	}

	c->converted_buf_size = av_samples_get_buffer_size(
		NULL,
		(int)info->channels,
		samples,
		cpymo_audio_fmt2ffmpeg(info->format),
		1);

	c->converted_frame_current_offset = 0;
	return CPYMO_ERR_SUCC;
}

static error_t cpymo_audio_channel_convert_current_frame(cpymo_audio_channel *c)
{
	const cpymo_backend_audio_info *info = cpymo_backend_audio_get_info();

	assert(c->frame->nb_samples != 0);
	
	error_t err = cpymo_audio_channel_grow_convert_buffer(
		c, (size_t)c->frame->nb_samples, info);
	CPYMO_THROW(err);

	int samples = swr_convert(
		c->swr_context,
		&c->converted_buf,
		c->frame->nb_samples,
		(const uint8_t **)c->frame->data,
		c->frame->nb_samples);

	if (samples <= 0) {
		const char *err = av_err2str(samples);
		printf("[Warning] swr_convert: %s.", err);
		return CPYMO_ERR_UNKNOWN;
	}

	c->converted_buf_size = av_samples_get_buffer_size(
		NULL,
		(int)info->channels,
		samples,
		cpymo_audio_fmt2ffmpeg(info->format),
		1);

	c->converted_frame_current_offset = 0;
	return CPYMO_ERR_SUCC;
}

static void cpymo_audio_channel_seek_to_head(cpymo_audio_channel *c)
{
	av_seek_frame(c->format_context, c->stream_id, 0, AVSEEK_FLAG_FRAME | AVSEEK_FLAG_ANY);
}

static error_t cpymo_audio_channel_next_frame(cpymo_audio_channel *c)
{ RETRY: {
	int result = avcodec_receive_frame(c->codec_context, c->frame);

	if (result == 0) {
		// One frame received
		error_t err = cpymo_audio_channel_convert_current_frame(c);
		av_frame_unref(c->frame);
		return err;
	}
	else if (result == AVERROR(EAGAIN)) {
		// No frame received, send more packet to codec.
		result = av_read_frame(c->format_context, c->packet);
		if (result == 0) {
			result = avcodec_send_packet(c->codec_context, c->packet);
			av_packet_unref(c->packet);
			if (result != 0) {
				printf("[Error] avcodec_send_packet: %s.\n", av_err2str(result));
				return CPYMO_ERR_UNKNOWN;
			}
			goto RETRY;
		}
		else if (result == AVERROR_EOF) {
			if (c->loop) {
				cpymo_audio_channel_seek_to_head(c);
			}
			else {
				result = avcodec_send_packet(c->codec_context, NULL);
				if (result != 0) {
					printf("[Error] avcodec_send_packet: %s.\n", av_err2str(result));
					return CPYMO_ERR_UNKNOWN;
				}
			}
			goto RETRY;
		}
		else {
			printf("[Error] av_read_frame: %s.\n", av_err2str(result));
			return CPYMO_ERR_UNKNOWN;
		}
	}
	else if (result == AVERROR_EOF) {
		// No frame received, and no more packet send to codec.
		// Flush Swr buffer.
		return cpymo_audio_channel_flush_converter(c);
	}
	else {
		printf("[Error] av_receive_frame: %s.\n", av_err2str(result));
		return CPYMO_ERR_UNKNOWN;
	}
}}

static void cpymo_audio_mix_samples(
	void *dst_, 
	const void *src_,
	size_t len, 
	cpymo_backend_audio_format fmt,
	float volume)
{

#define MIX_SIGNED(TYPE, TYPE_MAX_VAL) { \
	TYPE *dst = (TYPE *)dst_; \
	const TYPE *src = (TYPE *)src_; \
	len /= sizeof(TYPE); \
	\
	for (size_t i = 0; i < len; ++i) { \
		double src_sample = (float)src[i] / (float)TYPE_MAX_VAL; \
		double dst_sample = (float)dst[i] / (float)TYPE_MAX_VAL; \
		src_sample *= volume * 1.0f; \
		dst_sample += src_sample; \
		\
		if (dst_sample > 1) dst_sample = 1; \
		if (dst_sample < -1) dst_sample = -1; \
		\
		dst[i] = (TYPE)(dst_sample * TYPE_MAX_VAL); \
	}\
}

	switch (fmt) {
	case cpymo_backend_audio_s16: MIX_SIGNED(int16_t, INT16_MAX); break;
	case cpymo_backend_audio_s32: MIX_SIGNED(int32_t, INT32_MAX); break;
	case cpymo_backend_audio_f32: MIX_SIGNED(float, 1.0f); break;
	}
}

static void cpymo_audio_channel_mix_samples(uint8_t *dst, size_t len, cpymo_audio_channel *c)
{
	const cpymo_backend_audio_info *info = cpymo_backend_audio_get_info();

	while (len > 0) {
		uint8_t *src = c->converted_buf + c->converted_frame_current_offset;
		size_t src_size = c->converted_buf_size - c->converted_frame_current_offset;

		if (src_size == 0) {
			error_t err = cpymo_audio_channel_next_frame(c);
			if (err == CPYMO_ERR_SUCC) 
				continue;
			else
				goto FILL_BLANK_AND_RESET;
		}
		else {
			size_t write_size = src_size;
			if (write_size > len) write_size = len;

			cpymo_audio_mix_samples(dst, src, write_size, info->format, c->volume);
			c->converted_frame_current_offset += write_size;
			dst += write_size;
			len -= write_size;
		}
	}

	return;

FILL_BLANK_AND_RESET:
	cpymo_audio_channel_reset_unsafe(c);
	return;
}

static int cpymo_audio_packaged_audio_ffmpeg_read_packet(void *opaque, uint8_t *buf, int buf_size)
{
	cpymo_package_stream_reader *r = (cpymo_package_stream_reader *)opaque;
	size_t size = cpymo_package_stream_reader_read((char *)buf, buf_size, r);
	if (size == 0) return AVERROR_EOF;
	else return (int)size;
}

static int64_t cpymo_audio_packaged_audio_ffmpeg_seek(void *opaque, int64_t offset, int whence)
{
	cpymo_package_stream_reader *r = (cpymo_package_stream_reader *)opaque;

	switch (whence) {
	case AVSEEK_SIZE:
		return (int64_t)r->file_length;
	case SEEK_SET:
		if (cpymo_package_stream_reader_seek((size_t)offset, r) == CPYMO_ERR_SUCC)
			return 0;
		else return AVERROR_EOF;
	default:
		assert(false);
		return -1;
	};
}

error_t cpymo_audio_channel_play_file(
	cpymo_audio_channel *c, 
	const char * filename, const cpymo_package_stream_reader *package_reader, 
	bool loop)
{
	const cpymo_backend_audio_info *info = 
		cpymo_backend_audio_get_info();
	if (info == NULL) return CPYMO_ERR_SUCC;

	if (filename) { assert(package_reader == NULL); }
	if (package_reader) { assert(filename == NULL); }
	assert(!(filename == NULL && package_reader == NULL));

	cpymo_audio_channel_reset(c);
	// everything safe now.

	assert(c->enabled == false);
	
	assert(c->io_context == NULL);

	assert(c->format_context == NULL);

	if (package_reader) {
		c->package_reader = *package_reader;

		const size_t avio_buf_size = 4096 * 4;
		void *io_buffer = av_malloc(avio_buf_size);
		if (io_buffer == NULL) {
			cpymo_audio_channel_reset_unsafe(c);
			return CPYMO_ERR_OUT_OF_MEM;
		}

		c->io_context = avio_alloc_context(
			io_buffer, (int)avio_buf_size, 0, &c->package_reader,
			&cpymo_audio_packaged_audio_ffmpeg_read_packet,
			NULL,
			&cpymo_audio_packaged_audio_ffmpeg_seek);

		if (c->io_context == NULL) {
			cpymo_audio_channel_reset_unsafe(c);
			printf("[Error] avio_alloc_context failed.\n");
			return CPYMO_ERR_CAN_NOT_OPEN_FILE;
		}

		c->format_context = avformat_alloc_context();
		if (c->format_context == NULL) {
			cpymo_audio_channel_reset_unsafe(c);
			return CPYMO_ERR_OUT_OF_MEM;
		}

		c->format_context->pb = c->io_context;
		c->format_context->flags |= AVFMT_FLAG_CUSTOM_IO;
	}

	int result = 
		avformat_open_input(&c->format_context, filename == NULL ? "" : filename, NULL, NULL);

	if (filename == NULL) filename = "package stream reader";

	if (result != 0) {
		printf("[Error] Can not open %s with error ffmpeg error %s.\n",
			filename,
			av_err2str(result));

		cpymo_audio_channel_reset_unsafe(c);
		return CPYMO_ERR_CAN_NOT_OPEN_FILE;
	}

	result = avformat_find_stream_info(c->format_context, NULL);
	if (result != 0) {
		cpymo_audio_channel_reset_unsafe(c);
		printf("[Error] Can not get stream info from %s.\n", filename);
		return CPYMO_ERR_BAD_FILE_FORMAT;
	}

	c->stream_id = av_find_best_stream(
		c->format_context, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
	if (result != 0) {
		cpymo_audio_channel_reset_unsafe(c);
		printf("[Error] Can not find best stream from %s.\n", filename);
		return CPYMO_ERR_BAD_FILE_FORMAT;
	}

	AVStream *stream = c->format_context->streams[c->stream_id];
	assert(c->codec_context == NULL);
	const AVCodec *codec = avcodec_find_decoder(stream->codecpar->codec_id);
	c->codec_context = avcodec_alloc_context3(codec);
	avcodec_parameters_to_context(c->codec_context, stream->codecpar);
	c->codec_context->pkt_timebase = stream->time_base;
	if (c->codec_context == NULL) {
		cpymo_audio_channel_reset_unsafe(c);
		return CPYMO_ERR_UNKNOWN;
	}

	result = avcodec_open2(c->codec_context, codec, NULL);
	if (result != 0) {
		c->codec_context = NULL;
		cpymo_audio_channel_reset_unsafe(c);
		return CPYMO_ERR_UNSUPPORTED;
	}

	assert(c->swr_context == NULL);
	c->swr_context = swr_alloc_set_opts(
		NULL,
		av_get_default_channel_layout((int)info->channels),
		cpymo_audio_fmt2ffmpeg(info->format),
		(int)info->freq,
		stream->codecpar->channels == 1 ?
			AV_CH_LAYOUT_MONO :
			(stream->codecpar->channel_layout == 0 ?
				av_get_default_channel_layout(stream->codecpar->channels) :
				stream->codecpar->channel_layout),
		stream->codecpar->format,
		stream->codecpar->sample_rate,
		0, NULL);
	if (c->swr_context == NULL) {
		cpymo_audio_channel_reset_unsafe(c);
		return CPYMO_ERR_UNKNOWN;
	}

	result = swr_init(c->swr_context);
	if (result < 0) {
		cpymo_audio_channel_reset_unsafe(c);
		return CPYMO_ERR_UNKNOWN;
	}

	if (c->packet == NULL) {
		c->packet = av_packet_alloc();
		if (c->packet == NULL) {
			cpymo_audio_channel_reset_unsafe(c);
			return CPYMO_ERR_OUT_OF_MEM;
		}
	}

	if (c->frame == NULL) {
		c->frame = av_frame_alloc();
		if (c->frame == NULL) {
			cpymo_audio_channel_reset_unsafe(c);
			return CPYMO_ERR_OUT_OF_MEM;
		}
	}

	c->loop = loop;
	c->converted_frame_current_offset = 0;

	// read first frame
	if (cpymo_audio_channel_next_frame(c) != CPYMO_ERR_SUCC) {
		cpymo_audio_channel_reset_unsafe(c);
		return CPYMO_ERR_SUCC;
	}

	cpymo_backend_audio_lock();
	c->enabled = true;
	cpymo_backend_audio_unlock();
	return CPYMO_ERR_SUCC;
}

void cpymo_audio_init(cpymo_audio_system *s)
{
	s->enabled = cpymo_backend_audio_get_info() != NULL;

	for (size_t i = 0; i < CPYMO_AUDIO_MAX_CHANNELS; ++i) {
		cpymo_audio_channel_init(s->channels + i);
		s->channels[i].packet = NULL;
		s->channels[i].frame = NULL;
		s->channels[i].converted_buf = NULL;
		s->channels[i].converted_buf_all_size = 0;
		s->channels[i].volume = 0;
	}
}

void cpymo_audio_free(cpymo_audio_system *s)
{
	if (s->enabled == false) return;

	for (size_t i = 0; i < CPYMO_AUDIO_MAX_CHANNELS; ++i) {
		cpymo_audio_channel_reset(s->channels + i);

		if (s->channels[i].packet) av_packet_free(&s->channels[i].packet);
		if (s->channels[i].frame) av_frame_free(&s->channels[i].frame);
		if (s->channels[i].converted_buf) free(s->channels[i].converted_buf);
	}

	s->enabled = false;
}

bool cpymo_audio_channel_get_samples(void **samples, size_t *len, size_t cid, cpymo_audio_system *s)
{ RETRY: {
	if (!s->enabled) return false;

	cpymo_audio_channel *c = &s->channels[cid];
	if (!c->enabled) return false;

	size_t writeable_size = c->converted_buf_size - c->converted_frame_current_offset;

	if (writeable_size == 0) {
		error_t err = cpymo_audio_channel_next_frame(c);
		if (err == CPYMO_ERR_SUCC) goto RETRY;
		else {
			cpymo_audio_channel_reset_unsafe(c);
			return false;
		}
	}

	*samples = c->converted_buf + c->converted_frame_current_offset;

	if (writeable_size > *len) writeable_size = *len;
	*len = writeable_size;
	c->converted_frame_current_offset += writeable_size;

	return true;
} }

void cpymo_audio_copy_mixed_samples(void * dst, size_t len, cpymo_audio_system *s)
{
	if (s->enabled == false) return;

	memset(dst, 0, len);

	for (size_t i = 0; i < CPYMO_AUDIO_MAX_CHANNELS; ++i) {
		if (s->channels[i].enabled) {
			cpymo_audio_channel_mix_samples(dst, len, s->channels + i);
		}
	}
}

bool cpymo_audio_wait_se(struct cpymo_engine *e, float d)
{
	if (cpymo_input_foward_key_just_pressed(e)) {
		return true;
	}

	return !e->audio.channels[CPYMO_AUDIO_CHANNEL_SE].enabled;
}