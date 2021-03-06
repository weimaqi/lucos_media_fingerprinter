#include <stdio.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <chromaprint.h>
#include <ftw.h>
#include <stdlib.h>
#include <sqlite3.h>

#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define MIN(a, b) ((a) < (b) ? (a) : (b))

#define BUFFER_SIZE (AVCODEC_MAX_AUDIO_FRAME_SIZE * 2)

/* Define global variables */
ChromaprintContext *chromaprint_ctx;
int max_length = 60;
int16_t *buffer;
sqlite3 *db;

int decode_audio_file(const char *file_name, int max_length, int *duration)
{
	int i, ok = 0, remaining, length, consumed, buffer_size;
	AVFormatContext *format_ctx = NULL;
	AVCodecContext *codec_ctx = NULL;
	AVCodec *codec = NULL;
	AVStream *stream = NULL;
	AVPacket packet, packet_temp;

	if (av_open_input_file(&format_ctx, file_name, NULL, 0, NULL) != 0) {
		fprintf(stderr, "ERROR: couldn't open the file\n");
		return 0;
	}

	if (av_find_stream_info(format_ctx) < 0) {
		fprintf(stderr, "ERROR: couldn't find stream information in the file\n");
		av_close_input_file(format_ctx);
		return ok;
	}

	for (i = 0; i < format_ctx->nb_streams; i++) {
		codec_ctx = format_ctx->streams[i]->codec;
#if LIBAVCODEC_VERSION_INT <= AV_VERSION_INT(52, 20, 0)
		if (codec_ctx && codec_ctx->codec_type == CODEC_TYPE_AUDIO) {
#else
		if (codec_ctx && codec_ctx->codec_type == AVMEDIA_TYPE_AUDIO) {
#endif
			stream = format_ctx->streams[i];
			break;
		}
	}
	if (!stream) {
		fprintf(stderr, "ERROR: couldn't find any audio stream in the file\n");
		av_close_input_file(format_ctx);
		return 0;
	}

	codec = avcodec_find_decoder(codec_ctx->codec_id);
	if (!codec) {
		fprintf(stderr, "ERROR: unknown codec\n");
		if (format_ctx) {
			av_close_input_file(format_ctx);
		}
		return 0;
	}

	if (avcodec_open(codec_ctx, codec) < 0) {
		fprintf(stderr, "ERROR: couldn't open the codec\n");
		if (format_ctx) {
			av_close_input_file(format_ctx);
		}
		return 0;
	}

	if (codec_ctx->sample_fmt != SAMPLE_FMT_S16) {
		fprintf(stderr, "ERROR: unsupported sample format\n");
		if (codec_ctx) {
			avcodec_close(codec_ctx);
		}
		if (format_ctx) {
			av_close_input_file(format_ctx);
		}
		return 0;
	}

	if (codec_ctx->channels <= 0) {
		fprintf(stderr, "ERROR: no channels found in the audio stream\n");
		if (codec_ctx) {
			avcodec_close(codec_ctx);
		}
		if (format_ctx) {
			av_close_input_file(format_ctx);
		}
		return 0;
	}

	*duration = stream->time_base.num * stream->duration / stream->time_base.den;

	av_init_packet(&packet);
	av_init_packet(&packet_temp);

	remaining = max_length * codec_ctx->channels * codec_ctx->sample_rate;
	chromaprint_start(chromaprint_ctx, codec_ctx->sample_rate, codec_ctx->channels);

	while (1) {
		if (av_read_frame(format_ctx, &packet) < 0) {
			break;
		}

		packet_temp.data = packet.data;
		packet_temp.size = packet.size;

		while (packet_temp.size > 0) {
			buffer_size = BUFFER_SIZE;
#if LIBAVCODEC_VERSION_INT <= AV_VERSION_INT(52, 20, 0)
			consumed = avcodec_decode_audio2(codec_ctx,
				buffer, &buffer_size, packet_temp.data, packet_temp.size);
#else
			consumed = avcodec_decode_audio3(codec_ctx,
				buffer, &buffer_size, &packet_temp);
#endif

			if (consumed < 0) {
				break;
			}

			packet_temp.data += consumed;
			packet_temp.size -= consumed;

			if (buffer_size <= 0) {
				if (buffer_size < 0) {
					fprintf(stderr, "WARNING: size returned from avcodec_decode_audioX is too small\n");
				}
				continue;
			}
			if (buffer_size > BUFFER_SIZE) {
				fprintf(stderr, "WARNING: size returned from avcodec_decode_audioX is too large\n");
				continue;
			}

			length = MIN(remaining, buffer_size / 2);
			if (!chromaprint_feed(chromaprint_ctx, buffer, length)) {
				fprintf(stderr, "ERROR: fingerprint calculation failed\n");
				if (codec_ctx) {
					avcodec_close(codec_ctx);
				}
				if (format_ctx) {
					av_close_input_file(format_ctx);
				}
				return 0;
			}

			if (max_length) {
				remaining -= length;
				if (remaining <= 0) {
					break;
				}
			}
		}

		if (packet.data) {
			av_free_packet(&packet);
		}
	}

	if (!chromaprint_finish(chromaprint_ctx)) {
		fprintf(stderr, "ERROR: fingerprint calculation failed\n");
	} else {
		ok = 1;
	}

	if (codec_ctx) {
		avcodec_close(codec_ctx);
	}
	if (format_ctx) {
		av_close_input_file(format_ctx);
	}
	return ok;
}

int save_to_db(const char *path, int duration, char *fingerprint) {
	int rc;
	sqlite3_stmt *stmt;
	
	printf("Save Path=%s, DURATION=%d\n", path, duration);
	
	rc = sqlite3_prepare_v2(db, "REPLACE INTO track_path_fingerprint (path, fingerprint) VALUES (?, ?)",-1,&stmt,0);
	if (rc != SQLITE_OK) {
		fprintf(stderr, "SQL [1] prepare error: %d\n", rc);
		fprintf(stderr, "SQL [1] ERROR: %s\n", sqlite3_errmsg(db));
	} else {
		sqlite3_bind_text(stmt, 1, path, -1, SQLITE_TRANSIENT);
		sqlite3_bind_text(stmt, 2, fingerprint, -1, SQLITE_TRANSIENT);
		rc = sqlite3_step(stmt);
		if (rc != SQLITE_DONE) {
			fprintf(stderr, "SQL [1] step error: %d\n", rc);
			fprintf(stderr, "SQL [1] ERROR: %s\n", sqlite3_errmsg(db));
		}
	}
	
	rc = sqlite3_prepare_v2(db, "INSERT OR IGNORE INTO track (fingerprint) VALUES (?)",-1,&stmt,0);
	if (rc != SQLITE_OK) {
		fprintf(stderr, "SQL [2] prepare error: %d\n", rc);
		fprintf(stderr, "SQL [2] ERROR: %s\n", sqlite3_errmsg(db));
	} else {
		sqlite3_bind_text(stmt, 1, fingerprint, -1, SQLITE_TRANSIENT);
		rc = sqlite3_step(stmt);
		if (rc != SQLITE_DONE) {
			fprintf(stderr, "SQL [2] step error: %d\n", rc);
			fprintf(stderr, "SQL [2] ERROR: %s\n", sqlite3_errmsg(db));
		}
	}
	sqlite3_prepare_v2(db, "SELECT rowid FROM track WHERE id = ?", -1, &stmt, 0);
	sqlite3_bind_int(stmt, 1, (int)sqlite3_last_insert_rowid(db));
	sqlite3_step(stmt);
	printf("Last rowid: %d\n", (int)sqlite3_column_int(stmt, 0));
	
	rc = sqlite3_prepare_v2(db, "UPDATE track SET duration = ? WHERE fingerprint = ?",-1,&stmt,0);
	if (rc != SQLITE_OK) {
		fprintf(stderr, "SQL [3] prepare error: %d\n", rc);
		fprintf(stderr, "SQL [3] ERROR: %s\n", sqlite3_errmsg(db));
	} else {
		sqlite3_bind_int(stmt, 1, duration);
		sqlite3_bind_text(stmt, 2, fingerprint, -1, SQLITE_TRANSIENT);
		rc = sqlite3_step(stmt);
		if (rc != SQLITE_DONE) {
			fprintf(stderr, "SQL [3] step error: %d\n", rc);
			fprintf(stderr, "SQL [3] ERROR: %s\n", sqlite3_errmsg(db));
		}
	}
	return 0;
}

int decode_files(const char *file_name, const struct stat *status, int type) {
	
	
	int duration;
	char *fingerprint;
	
	// If not a file, then ignore
	if (type != FTW_F) {
		return 0;
	}
	printf("Decoding file:%s\n", file_name);

	if (!decode_audio_file(file_name, max_length, &duration)) {
		fprintf(stderr, "ERROR: unable to calculate fingerprint for file %s, skipping\n", file_name);
		return 0;
	}
	if (!chromaprint_get_fingerprint(chromaprint_ctx, &fingerprint)) {
		fprintf(stderr, "ERROR: unable to calculate fingerprint for file %s, skipping\n", file_name);
		return 0;
	}
	if (!duration) {
		fprintf(stderr, "ERROR: file %s has no duration, ignoring\n", file_name);
		return 0;
	} 
	if (!strcmp(fingerprint, "AQAAAAA")) {
		fprintf(stderr, "ERROR: file %s has boring fingerprint (AQAAAA), ignoring\n", file_name);
		return 0;
	} 
	save_to_db(file_name, duration, fingerprint);
	chromaprint_dealloc(fingerprint);
	return 0;
}

int main(int argc, char **argv)
{
	int i, num_file_names = 0;
	char **file_names;
	char *db_path = "../db/media.sqlite";

	file_names = malloc(argc * sizeof(char *));
	for (i = 1; i < argc; i++) {
		char *arg = argv[i];
		if (!strcmp(arg, "-length") && i + 1 < argc) {
			max_length = atoi(argv[++i]);
		} else if (!strcmp(arg, "-db") && i + 1 < argc) {
			db_path = argv[++i];
		} else {
			file_names[num_file_names++] = argv[i];
		}
	}

	if (!num_file_names) {
		printf("usage: %s [OPTIONS] FILE...\n\n", argv[0]);
		printf("Options:\n");
		printf("  -length SECS  length of the audio data used for fingerprint calculation (default 60)\n");
		printf("  -db PATH  the path of the media sqlite database (default ../db/media.sqlite)\n");
		return 2;
	}

	av_register_all();
	av_log_set_level(AV_LOG_ERROR);

	buffer = av_malloc(BUFFER_SIZE + 16);
	chromaprint_ctx = chromaprint_new(CHROMAPRINT_ALGORITHM_DEFAULT);
	
	sqlite3_open(db_path, &db);

	for (i = 0; i < num_file_names; i++) {
		ftw(file_names[i], decode_files, 1);
	}


	chromaprint_free(chromaprint_ctx);
	av_free(buffer);
	free(file_names);
    sqlite3_close(db);

	return 1;
}

