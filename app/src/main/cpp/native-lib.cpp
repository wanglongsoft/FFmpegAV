#include <jni.h>
#include <string>
#include <stdio.h>
#include <stdlib.h>
#include <android/log.h>

#define TAG "FFmpegAV"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, TAG, __VA_ARGS__)

void readAudioFrameFromPCM(uint16_t *pInt, FILE *pFile, uint64_t i, int i1);

extern "C" {
    #include "ffmpeg/include/libavformat/avformat.h"
    #include "ffmpeg/include/libavutil/avutil.h"
    #include "ffmpeg/include/libavformat/avformat.h"
    #include "ffmpeg/include/libavutil/intreadwrite.h"
    #include "ffmpeg/include/libavutil/timestamp.h"
    #include "ffmpeg/include/libavutil/opt.h"
    #include "ffmpeg/include/libavutil/time.h"
    #include "ffmpeg/include/libswresample/swresample.h"
    #include "ffmpeg/include/libswscale/swscale.h"
}

void adts_header(char *szAdtsHeader, int dataLen){
    int aac_type = 1;
    // 采样率下标，下标7表示采样率为22050
    int sampling_frequency_index = 7;
    // 声道数
    int channel_config = 2;

    // ADTS帧长度,包括ADTS长度和AAC声音数据长度的和。
    int adtsLen = dataLen + 7;

    // syncword,标识一个帧的开始，固定为0xFFF,占12bit(byte0占8位,byte1占前4位)
    szAdtsHeader[0] = 0xff;
    szAdtsHeader[1] = 0xf0;

    // ID,MPEG 标示符。0表示MPEG-4，1表示MPEG-2。占1bit(byte1第5位)
    szAdtsHeader[1] |= (0 << 3);

    // layer,固定为0，占2bit(byte1第6、7位)
    szAdtsHeader[1] |= (0 << 1);

    // protection_absent，标识是否进行误码校验。0表示有CRC校验，1表示没有CRC校验。占1bit(byte1第8位)
    szAdtsHeader[1] |= 1;

    // profile,标识使用哪个级别的AAC。1: AAC Main 2:AAC LC 3:AAC SSR 4:AAC LTP。占2bit(byte2第1、2位)
    szAdtsHeader[2] = aac_type<<6;

    // sampling_frequency_index,采样率的下标。占4bit(byte2第3、4、5、6位)
    szAdtsHeader[2] |= (sampling_frequency_index & 0x0f)<<2;

    // private_bit,私有位，编码时设置为0，解码时忽略。占1bit(byte2第7位)
    szAdtsHeader[2] |= (0 << 1);

    // channel_configuration,声道数。占3bit(byte2第8位和byte3第1、2位)
    szAdtsHeader[2] |= (channel_config & 0x04)>>2;
    szAdtsHeader[3] = (channel_config & 0x03)<<6;

    // original_copy,编码时设置为0，解码时忽略。占1bit(byte3第3位)
    szAdtsHeader[3] |= (0 << 5);

    // home,编码时设置为0，解码时忽略。占1bit(byte3第4位)
    szAdtsHeader[3] |= (0 << 4);

    // copyrighted_id_bit,编码时设置为0，解码时忽略。占1bit(byte3第5位)
    szAdtsHeader[3] |= (0 << 3);

    // copyrighted_id_start,编码时设置为0，解码时忽略。占1bit(byte3第6位)
    szAdtsHeader[3] |= (0 << 2);

    // aac_frame_length,ADTS帧长度,包括ADTS长度和AAC声音数据长度的和。占13bit(byte3第7、8位，byte4全部，byte5第1-3位)
    szAdtsHeader[3] |= ((adtsLen & 0x1800) >> 11);
    szAdtsHeader[4] = (uint8_t)((adtsLen & 0x7f8) >> 3);
    szAdtsHeader[5] = (uint8_t)((adtsLen & 0x7) << 5);

    // adts_buffer_fullness，固定为0x7FF。表示是码率可变的码流 。占11bit(byte5后5位，byte6前6位)
    szAdtsHeader[5] |= 0x1f;
    szAdtsHeader[6] = 0xfc;

    // number_of_raw_data_blocks_in_frame,值为a的话表示ADST帧中有a+1个原始帧，(一个AAC原始帧包含一段时间内1024个采样及相关数据)。占2bit（byte6第7、8位）。
    szAdtsHeader[6] |= 0;
}

/*
 在帧前面添加特征码(一般SPS/PPS的帧的特征码用4字节表示，为0X00000001，其他的帧特征码用3个字节表示，为0X000001。也有都用4字节表示的，我们这里采用4字节的方式)
 out是要输出的AVPaket
 sps_pps是SPS和PPS数据的指针，对于非关键帧就传NULL
 sps_pps_size是SPS/PPS数据的大小，对于非关键帧传0
 in是指向当前要处理的帧的头信息的指针
 in_size是当前要处理的帧大小(nal_size)
*/
static int alloc_and_copy(AVPacket *out,
                          const uint8_t *sps_pps, uint32_t sps_pps_size,
                          const uint8_t *in, uint32_t in_size)
{
    uint32_t offset = out->size;// 偏移量，就是out已有数据的大小，后面再写入数据就要从偏移量处开始操作
    uint8_t nal_header_size = 4;// 特征码的大小，SPS/PPS占4字节，其余占3字节
    int err;

    // 每次处理前都要对out进行扩容，扩容的大小就是此次要写入的内容的大小，也就是特征码大小加上sps/pps大小加上加上本帧数据大小
    err = av_grow_packet(out, sps_pps_size + in_size + nal_header_size);
    if (err < 0)
        return err;

    // 1.如果有sps_pps则先将sps_pps拷贝进out（memcpy()函数用于内存拷贝，第一个参数为拷贝要存储的地方，第二个参数是要拷贝的内容，第三个参数是拷贝内容的大小）
    if (sps_pps) {
        memcpy(out->data + offset, sps_pps, sps_pps_size);
    }

    // 2.再设置特征码

    for (int i = 0; i < nal_header_size; i++)
    {
        (out->data + offset + sps_pps_size)[i] = i == nal_header_size - 1 ? 1 : 0;
    }

    //3. 写入原包数据
    memcpy(out->data + offset + sps_pps_size + nal_header_size, in, in_size);

    return 0;
}

/*
读取并拷贝sps/pps数据
codec_extradata是codecpar的扩展数据，sps/pps数据就在这个扩展数据里面
codec_extradata_size是扩展数据大小
out_extradata是输出sps/pps数据的AVPacket包
padding:就是宏AV_INPUT_BUFFER_PADDING_SIZE的值(64)，是用于解码的输入流的末尾必要的额外字节个数，需要它主要是因为一些优化的流读取器一次读取32或者64比特，可能会读取超过size大小内存的末尾。
*/
int h264_extradata_to_annexb(const uint8_t *codec_extradata, const int codec_extradata_size, AVPacket *out_extradata, int padding)
{
    uint16_t unit_size = 0; // sps或者pps数据长度
    uint64_t total_size = 0; // 所有sps或者pps数据长度加上其特征码长度后的总长度

    /*
        out:是一个指向一段内存的指针，这段内存用于存放所有拷贝的sps或者pps数据和其特征码数据
        unit_nb:sps/pps个数
        sps_done：sps数据是否已经处理完毕
        sps_seen：是否有sps数据
        pps_seen：是否有pps数据
        sps_offset：sps数据的偏移，为0
        pps_offset：pps数据的偏移，因为pps数据在sps后面，所以其偏移就是所有sps数据长度+sps的特征码所占字节数
    */

    uint8_t *out = NULL, unit_nb, sps_done = 0,
            sps_seen                   = 0, pps_seen = 0, sps_offset = 0, pps_offset = 0;
    // 扩展数据的前4位是无用的数据，直接跳过拿到真正的扩展数据
    const uint8_t *extradata = codec_extradata + 4;
    static const uint8_t nalu_header[4] = { 0, 0, 0, 1 }; //sps或者pps数据前面的4bit的特征码

    int length_size = (*extradata++ & 0x3) + 1; // 用于指示表示编码数据长度所需字节数

    sps_offset = pps_offset = -1;

    //extradata第二个字节最后5位用于指示sps的个数,一般情况下一个扩展只有一个sps和pps，之后指针指向下一位
    unit_nb = *extradata++ & 0x1f;

    if (!unit_nb) {
        goto pps;
    }else {
        sps_offset = 0;
        sps_seen = 1;
    }

    while (unit_nb--) {//一般先读SPS在读PPS
        int err;

        // unit_size   = AV_RB16(extradata);
        unit_size   = (extradata[0] << 8) | extradata[1]; //再接着2个字节表示sps或者pps数据的长度
        total_size += unit_size + 4; //4表示sps/pps特征码长度, +=可以统计SPS和PPS的总长度
        if (total_size > INT_MAX - padding) {// total_size太大会造成数据溢出，所以要做判断
            LOGD("Too big extradata size, corrupted stream or invalid MP4/AVCC bitstream");
            av_free(out);
            return AVERROR(EINVAL);
        }

        // extradata + 2 + unit_size比整个扩展数据都长了表明数据是异常的
        if (extradata + 2 + unit_size > codec_extradata + codec_extradata_size) {
            LOGD("Packet header is not contained in global extradata, corrupted stream or invalid MP4/AVCC bitstream");
            av_free(out);
            return AVERROR(EINVAL);
        }

        // av_reallocp()函数用于内存扩展，给out扩展总长加padding的长度
        if ((err = av_reallocp(&out, total_size + padding)) < 0)
            return err;

        // 先将4字节的特征码拷贝进out
        memcpy(out + total_size - unit_size - 4, nalu_header, 4);

        // 再将sps/pps数据拷贝进out,extradata + 2是因为那2字节是表示sps/pps长度的，所以要跳过
        memcpy(out + total_size - unit_size, extradata + 2, unit_size);

        // 本次sps/pps数据处理完后，指针extradata跳过本次sps/pps数据
        extradata += 2 + unit_size;
        pps:
        if (!unit_nb && !sps_done++) { // 执行到这里表明sps已经处理完了，接下来处理pps数据
            unit_nb = *extradata++; /* number of pps unit(s) */
            if (unit_nb) {
                pps_offset = total_size;
                pps_seen = 1;// 如果pps个数大于0这给pps_seen赋值1表明数据中有pps
            }
        }
    }

    if (out) {
        memset(out + total_size, 0, padding);
    }

    if (!sps_seen) {
        LOGD("Warning: SPS NALU missing or invalid. The resulting stream may not play.");
    }

    if (!pps_seen) {
        LOGD("Warning: PPS NALU missing or invalid. The resulting stream may not play.");
    }

    out_extradata->data      = out;
    out_extradata->size      = total_size;

    return length_size;
}

/*
    为包数据添加起始码、SPS/PPS等信息后写入文件。
    AVPacket数据包可能包含一帧或几帧数据，对于视频来说只有1帧，对音频来说就包含几帧
    in为要处理的数据包
    file为输出文件的指针
*/

int h264_mp4toannexb(AVFormatContext *fmt_ctx, AVPacket *in, FILE *dst_fd)
{

    AVPacket *out = NULL;
    AVPacket spspps_pkt;

    int len;
    uint8_t unit_type;
    int32_t nal_size;
    uint32_t cumul_size    = 0;
    const uint8_t *buf;
    const uint8_t *buf_end;
    int buf_size;
    int ret = 0, i;

    out = av_packet_alloc();

    buf      = in->data;
    buf_size = in->size;
    buf_end  = in->data + in->size;

    do {
        ret= AVERROR(EINVAL);
        if (buf + 4 /*s->length_size*/ > buf_end)
            goto fail;

        for (nal_size = 0, i = 0; i < 4/*s->length_size*/; i++)
            nal_size = (nal_size << 8) | buf[i];
        buf += 4; /*s->length_size;*/
        unit_type = *buf & 0x1f;

        if (nal_size > buf_end - buf || nal_size < 0)
            goto fail;

        /*
        if (unit_type == 7)
            s->idr_sps_seen = s->new_idr = 1;
        else if (unit_type == 8) {
            s->idr_pps_seen = s->new_idr = 1;
            */
        /* if SPS has not been seen yet, prepend the AVCC one to PPS */
        /*
        if (!s->idr_sps_seen) {
            if (s->sps_offset == -1)
                av_log(ctx, AV_LOG_WARNING, "SPS not present in the stream, nor in AVCC, stream may be unreadable\n");
            else {
                if ((ret = alloc_and_copy(out,
                                     ctx->par_out->extradata + s->sps_offset,
                                     s->pps_offset != -1 ? s->pps_offset : ctx->par_out->extradata_size - s->sps_offset,
                                     buf, nal_size)) < 0)
                    goto fail;
                s->idr_sps_seen = 1;
                goto next_nal;
            }
        }
    }
    */

        /* if this is a new IDR picture following an IDR picture, reset the idr flag.
         * Just check first_mb_in_slice to be 0 as this is the simplest solution.
         * This could be checking idr_pic_id instead, but would complexify the parsing. */
        /*
        if (!s->new_idr && unit_type == 5 && (buf[1] & 0x80))
            s->new_idr = 1;

        */
        /* prepend only to the first type 5 NAL unit of an IDR picture, if no sps/pps are already present */
        if (/*s->new_idr && */unit_type == 5 /*&& !s->idr_sps_seen && !s->idr_pps_seen*/) {

            h264_extradata_to_annexb( fmt_ctx->streams[in->stream_index]->codec->extradata,
                                      fmt_ctx->streams[in->stream_index]->codec->extradata_size,
                                      &spspps_pkt,
                                      AV_INPUT_BUFFER_PADDING_SIZE);

            if ((ret = alloc_and_copy(out,
                                      spspps_pkt.data, spspps_pkt.size,
                                      buf, nal_size)) < 0)
                goto fail;
            /*s->new_idr = 0;*/
            /* if only SPS has been seen, also insert PPS */
        }
            /*else if (s->new_idr && unit_type == 5 && s->idr_sps_seen && !s->idr_pps_seen) {
                if (s->pps_offset == -1) {
                    av_log(ctx, AV_LOG_WARNING, "PPS not present in the stream, nor in AVCC, stream may be unreadable\n");
                    if ((ret = alloc_and_copy(out, NULL, 0, buf, nal_size)) < 0)
                        goto fail;
                } else if ((ret = alloc_and_copy(out,
                                            ctx->par_out->extradata + s->pps_offset, ctx->par_out->extradata_size - s->pps_offset,
                                            buf, nal_size)) < 0)
                    goto fail;
            }*/ else {
            if ((ret = alloc_and_copy(out, NULL, 0, buf, nal_size)) < 0)
                goto fail;
            /*
            if (!s->new_idr && unit_type == 1) {
                s->new_idr = 1;
                s->idr_sps_seen = 0;
                s->idr_pps_seen = 0;
            }
            */
        }

        //SPS、PPS和特征码都添加后将其写入文件
        len = fwrite(out->data, 1, out->size, dst_fd);
        if(len != out->size){
            LOGD("warning, length of writed data isn't equal pkt.size(%d, %d)", len, out->size);
        }

        // fwrite()只是将数据写入缓存，fflush()才将数据正在写入文件
        fflush(dst_fd);

        next_nal:
        buf += nal_size; // 一帧处理完后将指针移到下一帧
        cumul_size += nal_size + 4;// 累计已经处理好的数据长度
    } while (cumul_size < buf_size);

    /*
    ret = av_packet_copy_props(out, in);
    if (ret < 0)
        goto fail;

    */
    fail:
    av_packet_free(&out); // 凡是中途处理失败退出之前都要将促使的out释放，否则会出现内存泄露

    return ret;
}



extern "C"
JNIEXPORT void JNICALL
Java_com_wl_ffmpegav_MainActivity_audioExtraction(JNIEnv *env, jobject thiz, jstring srcfile,
                                                       jstring dstfile) {
    // TODO: implement audioExtraction()
    LOGD("audioExtraction in");
    const char * src_file = env->GetStringUTFChars(srcfile, NULL);
    const char * dst_file = env->GetStringUTFChars(dstfile, NULL);
    FILE *dst_fd = NULL;
    AVFormatContext *fmt_ctx = NULL;
    AVPacket pkt;
    int err_code;
    char errors[1024];
    int audio_stream_index = -1;
    int len;

    dst_fd = fopen(dst_file, "wb");
    if(!dst_fd) {
        LOGD("fopen file : %s fail", dst_file);
        goto end;
    }

    fmt_ctx = avformat_alloc_context();

    if((err_code = avformat_open_input(&fmt_ctx, src_file, NULL, NULL)) < 0){
        av_strerror(err_code, errors, 1024);
        LOGD("Could not open source file: %s, %d(%s)", src_file,
             err_code,
             errors);
        goto end;
    }

    audio_stream_index = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);

    if(audio_stream_index < 0){
        av_strerror(err_code, errors, 1024);
        LOGD("Could not find %s stream in input file %s", av_get_media_type_string(AVMEDIA_TYPE_AUDIO), src_file);
        goto end;
    }

    av_init_packet(&pkt);
    pkt.data = NULL;
    pkt.size = 0;

    while(av_read_frame(fmt_ctx, &pkt) >= 0) {
        if(pkt.stream_index == audio_stream_index){

            char adts_header_buf[7];
            adts_header(adts_header_buf, pkt.size);
            fwrite(adts_header_buf, 1, 7, dst_fd);

            len = fwrite(pkt.data, 1, pkt.size, dst_fd);
            if(len != pkt.size){
                LOGD("warning, length of writed data isn't equal pkt.size(%d, %d)\n", len, pkt.size);
            }
        }
        av_packet_unref(&pkt);
    }

    end:
    if(fmt_ctx != NULL) {
        avformat_close_input(&fmt_ctx);
        avformat_free_context(fmt_ctx);
    }
    if(dst_fd != NULL) {
        fflush(dst_fd);
        fclose(dst_fd);
    }
    env->ReleaseStringUTFChars(srcfile, src_file);
    env->ReleaseStringUTFChars(dstfile, dst_file);
    LOGD("audioExtraction out");
}

extern "C"
JNIEXPORT void JNICALL
Java_com_wl_ffmpegav_MainActivity_videoExtraction(JNIEnv *env, jobject thiz, jstring srcfile,
                                                       jstring dstfile) {
    // TODO: implement videoExtraction()
    LOGD("videoExtraction in");

    const char * src_file = env->GetStringUTFChars(srcfile, NULL);
    const char * dst_file = env->GetStringUTFChars(dstfile, NULL);
    FILE *dst_fd = NULL;
    AVFormatContext *fmt_ctx = NULL;
    AVPacket pkt;
    int err_code;
    char errors[1024];
    int video_stream_index = -1;
    int ret = -1;

    dst_fd = fopen(dst_file, "wb");
    if(!dst_fd) {
        LOGD("fopen file : %s fail", dst_file);
        goto end;
    }

    fmt_ctx = avformat_alloc_context();

    if((err_code = avformat_open_input(&fmt_ctx, src_file, NULL, NULL)) < 0){
        av_strerror(err_code, errors, 1024);
        LOGD("Could not open source file: %s, %d(%s)", src_file,
             err_code,
             errors);
        goto end;
    }

    for (int stream_index = 0; stream_index < fmt_ctx->nb_streams; ++stream_index) {
        AVStream *stream = fmt_ctx->streams[stream_index];
        AVCodecParameters *codecParameters = stream->codecpar;
        AVCodec *codec = avcodec_find_decoder(codecParameters->codec_id);
        AVCodecContext *codecContext = avcodec_alloc_context3(codec);
        ret = avcodec_parameters_to_context(codecContext, codecParameters);
        if(ret >= 0) {

        } else {
            LOGD("avcodec_parameters_to_context fail");
        }
        avcodec_free_context(&codecContext);
    }

    video_stream_index = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);

    if(video_stream_index < 0){
        av_strerror(err_code, errors, 1024);
        LOGD("Could not find %s stream in input file %s", av_get_media_type_string(AVMEDIA_TYPE_VIDEO), src_file);
    }

    av_init_packet(&pkt);
    pkt.data = NULL;
    pkt.size = 0;

    while(av_read_frame(fmt_ctx, &pkt) >= 0) {
        if (pkt.stream_index == video_stream_index) {
            h264_mp4toannexb(fmt_ctx, &pkt, dst_fd);
        }
        av_packet_unref(&pkt);
    }

    end:
    if(fmt_ctx != NULL) {
        avformat_close_input(&fmt_ctx);
        avformat_free_context(fmt_ctx);
    }

    if(dst_fd != NULL) {
        fflush(dst_fd);
        fclose(dst_fd);
    }
    env->ReleaseStringUTFChars(srcfile, src_file);
    env->ReleaseStringUTFChars(dstfile, dst_file);
    LOGD("videoExtraction out");
}

extern "C"
JNIEXPORT void JNICALL
Java_com_wl_ffmpegav_MainActivity_audioVideoFormatMp4ToFlv(JNIEnv *env, jobject thiz,
                                                                jstring srcfile, jstring dstfile) {
    LOGD("audioVideoFormatMp4ToFlv in");
    AVFormatContext *ifmt_ctx = NULL;// 输入上下文环境
    AVFormatContext *ofmt_ctx = NULL;// 输出上下文环境
    AVOutputFormat *ofmt = NULL;// 输出格式
    AVPacket pkt;

    char errors[1024];
    int stream_index = 0;
    int *stream_mapping = NULL;// 数组用于存放输出文件流的Index
    int stream_mapping_size = 0;// 数组用于存放输出文件流的数量
    int ret = 0;

    const char * src_file = env->GetStringUTFChars(srcfile, NULL);
    const char * dst_file = env->GetStringUTFChars(dstfile, NULL);

    LOGD("audioVideoFormatMp4ToFlv srcfile : %s", src_file);
    LOGD("audioVideoFormatMp4ToFlv dst_file : %s", dst_file);

    ifmt_ctx = avformat_alloc_context();
    if(!ifmt_ctx) {
        LOGD("ifmt_ctx alloc fail");
        return;
    }

    ofmt_ctx = avformat_alloc_context();
    if(!ofmt_ctx) {
        LOGD("ofmt_ctx alloc fail");
        return;
    }

    // 打开输入文件为ifmt_ctx分配内存
    if(avformat_open_input(&ifmt_ctx, src_file, NULL, NULL)) {
        LOGD("input file %s open fail", src_file);
    }

    // 检索输入文件的流信息
    if(avformat_find_stream_info(ifmt_ctx, NULL)) {
        LOGD("find stream info fail");
    }

    ret = avformat_alloc_output_context2(&ofmt_ctx, NULL, NULL, dst_file);

    if(!ofmt_ctx) {
        av_strerror(ret, errors, 1024);
        LOGD("avformat_alloc_output_context2 fail : %s", errors);
        goto end;
    }

    // 输出文件格式
    ofmt = ofmt_ctx->oformat;

    // 输入文件流的数量
    stream_mapping_size = ifmt_ctx->nb_streams;

    stream_mapping = (int*) av_mallocz_array(stream_mapping_size, sizeof(* stream_mapping));

    if(!stream_mapping) {
        LOGD("av_mallocz_array fail");
    }

    // 遍历输入文件中的每一路流，对于每一路流都要创建一个新的流进行输出
    for (int i = 0; i < ifmt_ctx->nb_streams; i++) {
        AVStream *out_stream;// 输出流
        AVStream *in_stream = ifmt_ctx->streams[i];// 输入流
        AVCodecParameters *in_codecpar = in_stream->codecpar;// 输入流的编解码参数

        if (in_codecpar->codec_type != AVMEDIA_TYPE_AUDIO &&
            in_codecpar->codec_type != AVMEDIA_TYPE_VIDEO &&
            in_codecpar->codec_type != AVMEDIA_TYPE_SUBTITLE) {
            stream_mapping[i] = -1;
            continue;
        }

        // 对于输出的流的index重写编号
        stream_mapping[i] = stream_index++;

        // 创建一个对应的输出流
        out_stream = avformat_new_stream(ofmt_ctx, NULL);

        if (!out_stream) {
            LOGD("avformat_new_stream fail");
        }

        // 直接将输入流的编解码参数拷贝到输出流中
        if (avcodec_parameters_copy(out_stream->codecpar, in_codecpar) < 0) {
            LOGD("avcodec_parameters_copy fail");
        }

        out_stream->codecpar->codec_tag = 0;
    }

    if (!(ofmt->flags & AVFMT_NOFILE)) {
        if (avio_open(&ofmt_ctx->pb, dst_file, AVIO_FLAG_WRITE) < 0) {
            LOGD("Could not open output file '%s'", dst_file);
        }
    }

    // 写入新的多媒体文件的头
    if(avformat_write_header(ofmt_ctx, NULL) < 0) {
        LOGD("avformat write_header fail");
    }

    while (1) {
        AVStream *in_stream, *out_stream;

        ret = av_read_frame(ifmt_ctx, &pkt);

        if (ret < 0) {
            if(ret == AVERROR_EOF) {// 读取完后退出循环
                LOGD("read pkt complete");
            } else {
                LOGD("read pkt fail");
            }
            break;
        }

        in_stream  = ifmt_ctx->streams[pkt.stream_index];
        if (pkt.stream_index >= stream_mapping_size ||
            stream_mapping[pkt.stream_index] < 0) {
            av_packet_unref(&pkt);
            continue;
        }

        pkt.stream_index = stream_mapping[pkt.stream_index]; // 按照输出流的index给pkt重新编号
        out_stream = ofmt_ctx->streams[pkt.stream_index];// 根据pkt的stream_index获取对应的输出流

        /* copy packet */
//        LOGD("in_stream : %d, %d", in_stream->time_base.num, in_stream->time_base.den);
//        LOGD("out_stream : %d, %d", out_stream->time_base.num, out_stream->time_base.den);
//        LOGD("before : %d, %d, %d", pkt.pts, pkt.dts, pkt.duration);
        pkt.pts = av_rescale_q_rnd(pkt.pts, in_stream->time_base, out_stream->time_base, (AVRounding)(AV_ROUND_NEAR_INF|AV_ROUND_PASS_MINMAX));
        pkt.dts = av_rescale_q_rnd(pkt.dts, in_stream->time_base, out_stream->time_base, (AVRounding)(AV_ROUND_NEAR_INF|AV_ROUND_PASS_MINMAX));
        pkt.duration = av_rescale_q(pkt.duration, in_stream->time_base, out_stream->time_base);
        pkt.pos = -1;

//        LOGD("after : %d, %d, %d", pkt.pts, pkt.dts, pkt.duration);

        ret = av_interleaved_write_frame(ofmt_ctx, &pkt);
        if (ret < 0) {
            LOGD("av_interleaved_write_frame fail");
            break;
        }
        av_packet_unref(&pkt);
    }

    av_write_trailer(ofmt_ctx);

    end:
    if(ifmt_ctx != NULL) {
        avformat_close_input(&ifmt_ctx);
        avformat_free_context(ifmt_ctx);
    }

    if (ofmt_ctx && !(ofmt->flags & AVFMT_NOFILE)) {
        avio_closep(&ofmt_ctx->pb);
    }

    if(ofmt_ctx != NULL) {
        avformat_free_context(ofmt_ctx);
    }

    if(!stream_mapping) {
        av_freep(&stream_mapping);
    }

    env->ReleaseStringUTFChars(srcfile, src_file);
    env->ReleaseStringUTFChars(dstfile, dst_file);
    LOGD("audioVideoFormatMp4ToFlv out");
}

extern "C"
JNIEXPORT void JNICALL
Java_com_wl_ffmpegav_MainActivity_audioVideoClip(JNIEnv *env, jobject thiz, jstring srcfile,
                                                      jstring dstfile, jint from_time,
                                                      jint stop_time) {
    int starttime = from_time;
    int stoptime = stop_time;
    LOGD("audioVideoClip in: %d, %d", starttime, stoptime);
    const char * src_file = env->GetStringUTFChars(srcfile, NULL);
    const char * dst_file = env->GetStringUTFChars(dstfile, NULL);
    int64_t *dts_start_from, *pts_start_from;

    AVOutputFormat *ofmt = NULL;
    AVFormatContext *ifmt_ctx = NULL;
    AVFormatContext *ofmt_ctx = NULL;
    AVPacket pkt;
    char errors[1024];
    int ret;

    ifmt_ctx = avformat_alloc_context();
    if(!ifmt_ctx) {
        LOGD("ifmt_ctx alloc fail");
        goto end;
    }

    ofmt_ctx = avformat_alloc_context();
    if(!ofmt_ctx) {
        LOGD("ofmt_ctx alloc fail");
        goto end;
    }

    // 打开输入文件为ifmt_ctx分配内存
    if(avformat_open_input(&ifmt_ctx, src_file, NULL, NULL)) {
        LOGD("input file %s open fail", src_file);
        goto end;
    }

    // 检索输入文件的流信息
    if(avformat_find_stream_info(ifmt_ctx, NULL)) {
        LOGD("find stream info fail");
        goto end;
    }

    ret = avformat_alloc_output_context2(&ofmt_ctx, NULL, NULL, dst_file);

    if(!ofmt_ctx) {
        av_strerror(ret, errors, 1024);
        LOGD("avformat_alloc_output_context2 fail : %s", errors);
        goto end;
    }

    ofmt = ofmt_ctx->oformat;

    LOGD("ifmt_ctx->nb_streams : %d", ifmt_ctx->nb_streams);
    for (int i = 0; i < ifmt_ctx->nb_streams; i++) {
        AVStream *in_stream = ifmt_ctx->streams[i];
        AVStream *out_stream = avformat_new_stream(ofmt_ctx, NULL);
        if (!out_stream) {
            LOGD("Failed allocating output stream");
            goto end;
        }

        // 直接将输入流的编解码参数拷贝到输出流中
        if (avcodec_parameters_copy(out_stream->codecpar, in_stream->codecpar) < 0) {
            LOGD("avcodec_parameters_copy fail");
            goto end;
        }

        out_stream->codecpar->codec_tag = 0;

    }

    if (!(ofmt->flags & AVFMT_NOFILE)) {
        if (avio_open(&ofmt_ctx->pb, dst_file, AVIO_FLAG_WRITE) < 0) {
            LOGD("Could not open output file '%s'", dst_file);
            goto end;
        }
    }

    ret = avformat_write_header(ofmt_ctx, NULL);
    if (ret < 0) {
        LOGD("Error occurred when opening output file");
        goto end;
    }

    ret = av_seek_frame(ifmt_ctx, -1, starttime * AV_TIME_BASE, AVSEEK_FLAG_ANY);
    if (ret < 0) {
        LOGD("Error seek");
        goto end;
    }

    dts_start_from = (int64_t *) malloc(sizeof(int64_t) *ifmt_ctx->nb_streams);
    memset(dts_start_from, 0, sizeof(int64_t) * ifmt_ctx->nb_streams);
    pts_start_from = (int64_t *) malloc(sizeof(int64_t) *ifmt_ctx->nb_streams);
    memset(pts_start_from, 0, sizeof(int64_t) * ifmt_ctx->nb_streams);

    while (1) {
        AVStream *in_stream, *out_stream;
        ret = av_read_frame(ifmt_ctx, &pkt);
        if (ret < 0) {
            if (ret == AVERROR_EOF) {// 读取完后退出循环
                LOGD("read pkt complete");
            } else {
                LOGD("read pkt fail");
            }
            break;
        }

        in_stream = ifmt_ctx->streams[pkt.stream_index];
        out_stream = ofmt_ctx->streams[pkt.stream_index];
//        LOGD("process time : %lf", av_q2d(in_stream->time_base) * pkt.pts);
        if (av_q2d(in_stream->time_base) * pkt.pts > stoptime) {
            av_packet_unref(&pkt);
            LOGD("time > stop, break");
            break;
        }

        if (dts_start_from[pkt.stream_index] == 0) {
            dts_start_from[pkt.stream_index] = pkt.dts;
        }
        if (pts_start_from[pkt.stream_index] == 0) {
            pts_start_from[pkt.stream_index] = pkt.pts;
            LOGD("pts_start_from :%d", pkt.pts);
        }

        /* copy packet */
        pkt.pts = av_rescale_q_rnd(pkt.pts, in_stream->time_base, out_stream->time_base,
                                   (AVRounding) (AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX));
        pkt.dts = av_rescale_q_rnd(pkt.dts, in_stream->time_base, out_stream->time_base,
                                   (AVRounding) (AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX));
        pkt.duration = (int) av_rescale_q((int64_t) pkt.duration, in_stream->time_base, out_stream->time_base);
        pkt.pos = -1;

        if (pkt.pts < 0) {
            pkt.pts = 0;
        }
        if (pkt.dts < 0) {
            pkt.dts = 0;
        }

        ret = av_interleaved_write_frame(ofmt_ctx, &pkt);

        if (ret < 0) {
            av_strerror(ret, errors, 1024);
            LOGD("av_interleaved_write_frame fail : %s", errors);
            goto end;
        }
        av_packet_unref(&pkt);
    }

    av_write_trailer(ofmt_ctx);

    end:
    if(NULL != ifmt_ctx) {
        avformat_close_input(&ifmt_ctx);
        avformat_free_context(ifmt_ctx);
    }
    if(NULL != ofmt_ctx) {
        if (ofmt_ctx && !(ofmt->flags & AVFMT_NOFILE))
            avio_closep(&ofmt_ctx->pb);
        avformat_free_context(ofmt_ctx);
    }

    free(dts_start_from);
    free(pts_start_from);

    env->ReleaseStringUTFChars(srcfile, src_file);
    env->ReleaseStringUTFChars(dstfile, dst_file);
    LOGD("audioVideoClip out");
}

extern "C"
JNIEXPORT void JNICALL
Java_com_wl_ffmpegav_MainActivity_audioVideoMerge(JNIEnv *env, jobject thiz,
                                                       jstring src_audiofile, jstring src_videofile,
                                                       jstring dstfile) {
    LOGD("mergeAudioVideoFile in");
    const char * srcAudioFile = env->GetStringUTFChars(src_audiofile, NULL);
    const char * srcVideoFile = env->GetStringUTFChars(src_videofile, NULL);
    const char * dstAVFile = env->GetStringUTFChars(dstfile, NULL);
    AVOutputFormat *ofmt = NULL;
    AVFormatContext *iafmt_ctx = NULL;
    AVFormatContext *ivfmt_ctx = NULL;
    AVFormatContext *ofmt_ctx = NULL;

    AVPacket audio_pkt;
    AVPacket video_pkt;
    char errors[1024];
    int ret;
    int video_packet_count = 0;

    AVStream *video_in_stream = NULL;
    AVStream *video_out_stream = NULL;
    AVStream *audio_in_stream = NULL;
    AVStream *audio_out_stream = NULL;
    int out_video_stream_index = -1;
    int out_audio_stream_index = -1;
    LOGD("srcAudioFile : %s", srcAudioFile);
    LOGD("srcVideoFile : %s", srcVideoFile);
    LOGD("dstAVFile : %s", dstAVFile);

    // 打开音频输入文件为iafmt_ctx分配内存
    if(avformat_open_input(&iafmt_ctx, srcAudioFile, NULL, NULL)) {
        LOGD("input file %s open fail", srcAudioFile);
        goto end;
    }

    // 打开视频输入文件为ivfmt_ctx分配内存
    if(avformat_open_input(&ivfmt_ctx, srcVideoFile, NULL, NULL)) {
        LOGD("input file %s open fail", srcVideoFile);
        goto end;
    }

    ret = avformat_find_stream_info(iafmt_ctx, NULL);
    if(ret < 0) {
        LOGD("avformat_find_stream_info fail : iafmt_ctx");
        goto end;
    }

    ret = avformat_find_stream_info(ivfmt_ctx, NULL);
    if(ret < 0) {
        LOGD("avformat_find_stream_info fail : ivfmt_ctx");
        goto end;
    }

    ret = avformat_alloc_output_context2(&ofmt_ctx, NULL, NULL, dstAVFile);

    if(!ofmt_ctx) {
        av_strerror(ret, errors, 1024);
        LOGD("avformat_alloc_output_context2 fail : %s", errors);
        goto end;
    }

    if(ivfmt_ctx->nb_streams == 1) {
        //创建视频输出流
        LOGD("create video stream");
        video_in_stream = ivfmt_ctx->streams[0];
        video_out_stream = avformat_new_stream(ofmt_ctx, NULL);
        if (!video_out_stream) {
            LOGD("Failed allocating output stream");
            goto end;
        }

        // 直接将输入流的编解码参数拷贝到输出流中
        if (avcodec_parameters_copy(video_out_stream->codecpar, video_in_stream->codecpar) < 0) {
            LOGD("avcodec_parameters_copy fail");
            goto end;
        }

        video_out_stream->codecpar->codec_tag = 0;
        LOGD("video_in_stream->codecpar->codec_type : %d", video_in_stream->codecpar->codec_type);
    }

    if(iafmt_ctx->nb_streams == 1) {
        //创建音频输出流
        LOGD("create audio stream");
        audio_in_stream = iafmt_ctx->streams[0];
        audio_out_stream = avformat_new_stream(ofmt_ctx, NULL);
        if (!audio_out_stream) {
            LOGD("Failed allocating output stream");
            goto end;
        }

        // 直接将输入流的编解码参数拷贝到输出流中
        if (avcodec_parameters_copy(audio_out_stream->codecpar, audio_in_stream->codecpar) < 0) {
            LOGD("avcodec_parameters_copy fail");
            goto end;
        }
        audio_out_stream->codecpar->codec_tag = 0;
        LOGD("audio_in_stream->codecpar->codec_type : %d", audio_in_stream->codecpar->codec_type);
    }

    ofmt = ofmt_ctx->oformat;

    if (!(ofmt->flags & AVFMT_NOFILE)) {
        if (avio_open(&ofmt_ctx->pb, dstAVFile, AVIO_FLAG_WRITE) < 0) {
            LOGD("Could not open output file '%s'", dstAVFile);
            goto end;
        }
    }

    for(int i = 0; i < ofmt_ctx->nb_streams; i++) {
        if(ofmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            out_audio_stream_index = i;
        } else if(ofmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            out_video_stream_index = i;
        } else {
            LOGD("unknown stream");
        }
    }

    LOGD("out_audio_stream_index : %d out_video_stream_index : %d", out_audio_stream_index, out_video_stream_index);
    ret = avformat_write_header(ofmt_ctx, NULL);
    if (ret < 0) {
        LOGD("Error occurred when avformat_write_header");
        goto end;
    }

    av_init_packet(&audio_pkt);
    av_init_packet(&video_pkt);

    //拷贝音频压缩数据
    while (1) {
        ret = av_read_frame(iafmt_ctx, &audio_pkt);
        if (ret < 0) {
            if (ret == AVERROR_EOF) {// 读取完后退出循环
                LOGD("read audio pkt complete");
            } else {
                LOGD("read audio pkt fail");
            }
            break;
        }

        /* copy packet */
        audio_pkt.pts = av_rescale_q_rnd(audio_pkt.pts, audio_in_stream->time_base, audio_out_stream->time_base,
                                         (AVRounding) (AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX));
        audio_pkt.dts = av_rescale_q_rnd(audio_pkt.dts, audio_in_stream->time_base, audio_out_stream->time_base,
                                         (AVRounding) (AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX));
        audio_pkt.duration = (int) av_rescale_q((int64_t) audio_pkt.duration, audio_in_stream->time_base, audio_out_stream->time_base);
        audio_pkt.pos = -1;
        audio_pkt.stream_index = out_audio_stream_index;

        if (audio_pkt.pts < 0) {
            audio_pkt.pts = 0;
        }
        if (audio_pkt.dts < 0) {
            audio_pkt.dts = 0;
        }

        ret = av_interleaved_write_frame(ofmt_ctx, &audio_pkt);

        if (ret < 0) {
            av_strerror(ret, errors, 1024);
            LOGD("av_interleaved_write_frame audio fail : %s", errors);
            goto end;
        }
        av_packet_unref(&audio_pkt);
    }

    //拷贝视频压缩数据
    while (1) {
        ret = av_read_frame(ivfmt_ctx, &video_pkt);
        if (ret < 0) {
            if (ret == AVERROR_EOF) {// 读取完后退出循环
                LOGD("read video pkt complete cnt : %d", video_packet_count);
            } else {
                LOGD("read video pkt fail");
            }
            break;
        }

        if(video_pkt.pts == AV_NOPTS_VALUE) {//添加PTS,DTS信息
            AVRational time_base2 = video_in_stream->time_base;
            int64_t calc_duration = (double) AV_TIME_BASE / av_q2d(video_in_stream->r_frame_rate);
            video_pkt.pts = (double)(video_packet_count * calc_duration) / (double)(av_q2d(time_base2) * AV_TIME_BASE);
            video_pkt.dts = video_pkt.pts;
            video_pkt.duration = (double)calc_duration / (double)(av_q2d(time_base2) * AV_TIME_BASE);
            video_packet_count++;
        }

        /* copy packet */
        video_pkt.pts = av_rescale_q_rnd(video_pkt.pts, video_in_stream->time_base, video_out_stream->time_base,
                                         (AVRounding) (AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX));
        video_pkt.dts = av_rescale_q_rnd(video_pkt.dts, video_in_stream->time_base, video_out_stream->time_base,
                                         (AVRounding) (AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX));
        video_pkt.duration = (int) av_rescale_q((int64_t) video_pkt.duration, video_in_stream->time_base, video_out_stream->time_base);
        video_pkt.pos = -1;
        video_pkt.stream_index = out_video_stream_index;//必须设置，否则视频不显示

        if (video_pkt.pts < 0) {
            video_pkt.pts = 0;
        }
        if (video_pkt.dts < 0) {
            video_pkt.dts = 0;
        }

        ret = av_interleaved_write_frame(ofmt_ctx, &video_pkt);

        if (ret < 0) {
            av_strerror(ret, errors, 1024);
            LOGD("av_interleaved_write_frame video fail : %s", errors);
            goto end;
        }
        av_packet_unref(&video_pkt);
    }

    ret = av_write_trailer(ofmt_ctx);
    if (ret != 0) {
        LOGD("Error av_write_trailer");
        goto end;
    }

    end:
    if(NULL != iafmt_ctx) {
        avformat_close_input(&iafmt_ctx);
        avformat_free_context(iafmt_ctx);
    }
    if(NULL != ivfmt_ctx) {
        avformat_close_input(&ivfmt_ctx);
        avformat_free_context(ivfmt_ctx);
    }

    if(NULL != ofmt_ctx) {
        avformat_free_context(ofmt_ctx);
    }

    env->ReleaseStringUTFChars(src_audiofile, srcAudioFile);
    env->ReleaseStringUTFChars(src_videofile, srcVideoFile);
    env->ReleaseStringUTFChars(dstfile, dstAVFile);
    LOGD("mergeAudioVideoFile out");
}

int loadframe(uint8_t *frame_data[], FILE *hInputYUVFile, uint32_t frmIdx, uint32_t width, uint32_t height, int linesize[])
{
    LOGD("loadframe in : %d  linesize : %d   %d  %d", frmIdx, linesize[0], linesize[1], linesize[2]);
    uint64_t y_data_offset;
    uint64_t u_data_offset;
    uint64_t v_data_offset;
    uint32_t result;
    uint32_t dwInFrameSize = 0;
    int anFrameSize[3] = {};
    FILE *y_data_fd = hInputYUVFile;
    FILE *u_data_fd = hInputYUVFile;
    FILE *v_data_fd = hInputYUVFile;
    uint8_t *frame_data_y = frame_data[0];
    uint8_t *frame_data_u = frame_data[1];
    uint8_t *frame_data_v = frame_data[2];
    dwInFrameSize = width * height * 3 / 2;
    anFrameSize[0] = width * height;
    anFrameSize[1] = anFrameSize[2] = width * height / 4;

    //当前帧在文件中的偏移量：当前index * 每一帧的大小
    y_data_offset = (uint64_t) dwInFrameSize * frmIdx;
    u_data_offset = (uint64_t) dwInFrameSize * frmIdx + anFrameSize[0];
    v_data_offset = (uint64_t) dwInFrameSize * frmIdx + anFrameSize[0] + anFrameSize[1];

    //seek到偏移处
    result = fseek(y_data_fd, y_data_offset, SEEK_SET);
    if (result == -1)
    {
        LOGD("loadframe y_data_offset fail");
        return -1;
    }
    //把当前帧的Y、U、V数据分别读取到对应的数组中
    for (int i = 0; i < height; ++i) {
        fread(frame_data_y, 1, linesize[0], y_data_fd);
        frame_data_y += linesize[0];
    }
//    fread(frame_data_y, 1, anFrameSize[0], y_data_fd);
//    LOGD("y data : %d  %d  %d %d", *(frame_data_y), *(frame_data_y + 1), *(frame_data_y + 2), *(frame_data_y + 3));

    result = fseek(u_data_fd, u_data_offset, SEEK_SET);
    if (result == -1)
    {
        LOGD("loadframe u_data_offset fail");
        return -1;
    }
    for (int i = 0; i < height / 2; ++i) {
        fread(frame_data_u, 1, linesize[1], u_data_fd);
        frame_data_u += linesize[1];
    }
//    fread(frame_data_u, 1, anFrameSize[1], u_data_fd);
//    LOGD("u data : %d  %d  %d %d", *(frame_data_u), *(frame_data_u + 1), *(frame_data_u + 2), *(frame_data_u + 3));

    result = fseek(v_data_fd, v_data_offset, SEEK_SET);
    if (result == -1)
    {
        LOGD("loadframe v_data_offset fail");
        return -1;
    }
    for (int i = 0; i < height / 2; ++i) {
        fread(frame_data_v, 1, linesize[2], v_data_fd);
        frame_data_v += linesize[2];
    }
//    fread(frame_data_v, 1, anFrameSize[2], v_data_fd);
//    LOGD("v data : %d  %d  %d %d", *(frame_data_v), *(frame_data_v + 1), *(frame_data_v + 2), *(frame_data_v + 3));
    LOGD("loadframe out");
    return 0;
}

static void encode(AVCodecContext *enc_ctx, AVFrame *frame, AVPacket *pkt,
                   FILE *outfile, int index, char* errors)
{
    LOGD("encode in new : %d", index);
    int ret;

    /* send the frame to the encoder */

    ret = avcodec_send_frame(enc_ctx, frame);
    if (ret < 0) {
        LOGD("avcodec_send_frame fail");
        return;
    }

    while (ret >= 0) {
        ret = avcodec_receive_packet(enc_ctx, pkt);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            av_strerror(ret, errors, 1024);
            LOGD("avcodec_receive_packet : %d  : %s ", ret, errors);
            return;
        } else if (ret < 0) {
            av_strerror(ret, errors, 1024);
            LOGD("avcodec_receive_packet : %d  : %s ", ret, errors);
            break;
        }
        fwrite(pkt->data, 1, pkt->size, outfile);
        av_packet_unref(pkt);
    }
    LOGD("encode out");
}

extern "C"
JNIEXPORT void JNICALL
Java_com_wl_ffmpegav_MainActivity_videoFileEncode(JNIEnv *env, jobject thiz,
                                                       jstring src_yuvfile, jstring dst_h264_file) {
    // TODO: implement videoFileEncode()
    LOGD("videoFileEncode in");
    const char * src_yuv_file = env->GetStringUTFChars(src_yuvfile, NULL);
    const char * dst_file = env->GetStringUTFChars(dst_h264_file, NULL);

    FILE *file_src_yuv = NULL;
    FILE *file_dst_file = NULL;

    const char *codec_name = "libx264";
    uint64_t lumaPlaneSize = 720 * 1280;
    uint64_t chromaPlaneSize = lumaPlaneSize >> 2;
    uint64_t file_size = 0;
    int totalFrames = 0;
    int ret = -1;

    AVFrame *frame = NULL;
    AVPacket *pkt = NULL;
    const AVCodec *codec = NULL;
    AVCodecContext *codec_cxt = NULL;
    char errors[1024];
    uint8_t endcode[] = { 0, 0, 1, 0xb7 };

    codec = avcodec_find_encoder_by_name(codec_name);
    if (!codec) {
        LOGD("codec find fail : %s", codec->name);
        goto end;
    } else {
        LOGD("codec->name : %s", codec->name);
    }

    codec_cxt = avcodec_alloc_context3(codec);
    if (!codec_cxt) {
        LOGD("codec_cxt alloc fail");
        goto end;
    }

    //设置codec 参数
    codec_cxt->bit_rate = 400000;
    codec_cxt->width = 720;
    codec_cxt->height = 1280;
    codec_cxt->time_base = (AVRational){1, 25};
    codec_cxt->framerate = (AVRational){25, 1};
    codec_cxt->gop_size = 10;
    codec_cxt->max_b_frames = 1;
    codec_cxt->pix_fmt = AV_PIX_FMT_YUV420P;
    av_opt_set(codec_cxt->priv_data, "preset", "slow", 0);


    //test code is start
//    codec_cxt->bit_rate = 400000;
//    codec_cxt->width = 352;
//    codec_cxt->height = 288;
//    codec_cxt->time_base = (AVRational){1, 25};
//    codec_cxt->framerate = (AVRational){25, 1};
//    codec_cxt->gop_size = 10;
//    codec_cxt->max_b_frames = 1;
//    codec_cxt->pix_fmt = AV_PIX_FMT_YUV420P;
//    av_opt_set(codec_cxt->priv_data, "preset", "slow", 0);
    //test code is end

    //打开编码器
    ret = avcodec_open2(codec_cxt, codec, NULL);
    if (ret < 0) {
        av_strerror(ret, errors, 1024);
        LOGD("avcodec_open2 fail ret : %d error: %s", ret, errors);
        goto end;
    }

    frame = av_frame_alloc();
    if(NULL == frame) {
        LOGD("frame alloc fail");
        goto end;
    }

    pkt = av_packet_alloc();
    if(NULL == pkt) {
        LOGD("pkt alloc fail");
        goto end;
    }

    frame->format = codec_cxt->pix_fmt;
    frame->width  = codec_cxt->width;
    frame->height = codec_cxt->height;

    ret = av_frame_get_buffer(frame, 32);
    if(ret <  0) {
        LOGD("av_frame_get_buffer fail");
        goto end;
    }

    file_src_yuv = fopen(src_yuv_file, "rb");
    if(NULL == file_src_yuv) {
        LOGD("fopen file : %s fail", src_yuv_file);
        goto end;
    }

    file_dst_file = fopen(dst_file, "wb");
    if(NULL == file_dst_file) {
        LOGD("fopen file : %s fail", dst_file);
        goto end;
    }

    fseek(file_src_yuv, 0, SEEK_END);
    file_size = ftell(file_src_yuv);

    totalFrames = file_size / (lumaPlaneSize + chromaPlaneSize + chromaPlaneSize);

    LOGD("src_yuv_file total frame : %d", totalFrames);

    //遍历每一帧YUV数据
    for (int frm = 0; frm < totalFrames; frm++)
    {
        ret = av_frame_make_writable(frame);
        if(ret < 0) {
            av_strerror(ret, errors, 1024);
            LOGD("av_frame_make_writable fail, ret: %d errors : %s", ret, errors);
            goto end;
        }
        frame->linesize[0] = 720;
        frame->linesize[1] = 360;
        frame->linesize[2] = 360;
        loadframe(frame->data, file_src_yuv, frm, 720, 1280, frame->linesize);
        //处理yuv数据....
        frame->pts = frm;
        encode(codec_cxt, frame, pkt, file_dst_file, frm, errors);
    }

//    for (int i = 0; i < 25; i++) {
//        /* make sure the frame data is writable */
//        ret = av_frame_make_writable(frame);
//        if (ret < 0) {
//            LOGD("av_frame_make_writable fail");
//            goto end;
//        }
//
//        /* prepare a dummy image */
//        /* Y */
//        for (int y = 0; y < codec_cxt->height; y++) {
//            for (int x = 0; x < codec_cxt->width; x++) {
//                frame->data[0][y * frame->linesize[0] + x] = x + y + i * 3;
//            }
//        }
//
//        /* Cb and Cr */
//        for (int y = 0; y < codec_cxt->height/2; y++) {
//            for (int x = 0; x < codec_cxt->width/2; x++) {
//                frame->data[1][y * frame->linesize[1] + x] = 128 + y + i * 2;
//                frame->data[2][y * frame->linesize[2] + x] = 64 + x + i * 5;
//            }
//        }
//
//        LOGD("linesize : %d  %d  %d", frame->linesize[0], frame->linesize[1], frame->linesize[2]);
//        frame->pts = i;
//
//        /* encode the image */
//        encode(codec_cxt, frame, pkt, file_dst_file, i, errors);
//    }

    encode(codec_cxt, NULL, pkt, file_dst_file, 10000, errors);

    fwrite(endcode, 1, sizeof(endcode), file_dst_file);

    end:

    if(NULL != file_src_yuv) {
        fclose(file_src_yuv);
    }

    if(NULL != file_dst_file) {
        fclose(file_dst_file);
    }

    if(codec_cxt != NULL) {
        avcodec_close(codec_cxt);
        avcodec_free_context(&codec_cxt);
    }

    if(NULL != frame) {
        av_frame_free(&frame);
    }

    if(NULL != pkt) {
        av_packet_free(&pkt);
    }

    env->ReleaseStringUTFChars(src_yuvfile, src_yuv_file);
    env->ReleaseStringUTFChars(dst_h264_file, dst_file);
    LOGD("videoFileEncode out");
}

int yuv_data_save(unsigned char *data[], int linesize[], int width, int height, FILE *fd) {
    LOGD("yuv_data_save width : %d, height : %d", width, height);

    uint32_t pitchY = linesize[0];
    uint32_t pitchU = linesize[1];
    uint32_t pitchV = linesize[2];
    uint8_t* avY = data[0];
    uint8_t* avU = data[1];
    uint8_t* avV = data[2];

    LOGD("y data : %d  %d  %d %d", *(avY), *(avY + 1), *(avY + 2), *(avY + 3));
    //YUV数据之Y
    for (int i = 0; i < height; i++) {
        fwrite(avY, 1, pitchY, fd);
        avY += pitchY;
    }

    LOGD("u data : %d  %d  %d %d", *(avU), *(avU + 1), *(avU + 2), *(avU + 3));
    //YUV数据之U，！！！！  height / 2
    for (int i = 0; i < height / 2; i++) {
        fwrite(avU, 1, pitchU, fd);
        avU += pitchU;
    }

    LOGD("v data : %d  %d  %d %d", *(avV), *(avV + 1), *(avV + 2), *(avV + 3));
    //YUV数据之V，！！！！  height / 2
    for (int i = 0; i < height / 2; i++) {
        fwrite(avV, 1, pitchV, fd);
        avV += pitchV;
    }
    LOGD("yuv_data_save width out");
    return 0;
}

void decode(AVCodecContext *dec_ctx, AVFrame *frame, AVPacket *pkt,
            FILE *fd, char * errors)
{
    char buf[1024];
    int ret;

    ret = avcodec_send_packet(dec_ctx, pkt);
    if (ret < 0) {
        av_strerror(ret, errors, 1024);
        LOGD("Error sending a packet for decoding, ret = %d errors = %s", ret, errors);
        return;
    }

    while (ret >= 0) {
        ret = avcodec_receive_frame(dec_ctx, frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            return;
        }
        else if (ret < 0) {
            LOGD("Error during decoding");
            return;
        }

        LOGD("saving frame %5d  pts : %ld  pkt size : %d", dec_ctx->frame_number, frame->pts, frame->pkt_size);
        yuv_data_save(frame->data, frame->linesize, frame->width, frame->height, fd);
    }
}

extern "C"
JNIEXPORT void JNICALL
Java_com_wl_ffmpegav_MainActivity_videoFileDecode(JNIEnv *env, jobject thiz,
                                                       jstring src_mp4_file, jstring dst_yuv_file) {
    // TODO: implement videoFileDecode()
    LOGD("videoFileDecode in");
    const char * src_file = env->GetStringUTFChars(src_mp4_file, NULL);
    const char * dst_file = env->GetStringUTFChars(dst_yuv_file, NULL);

    const AVCodec *codec = NULL;
    AVFormatContext *fmt_ctx = NULL;
    AVCodecContext *c= NULL;
    AVStream * video_stream = NULL;
    AVFrame *frame = NULL;
    char errors[1024];
    int ret = 0;
    AVPacket *pkt = NULL;
    int video_index = -1;
    FILE *dst_file_fd = NULL;
    SwsContext *sws_ctx = NULL;
    pkt = av_packet_alloc();
    if(pkt == NULL) {
        LOGD("av_packet_alloc fail");
        goto end;
    }

    frame = av_frame_alloc();
    if(frame == NULL) {
        LOGD("av_frame_alloc fail");
        goto end;
    }

    c = avcodec_alloc_context3(codec);
    if(c == NULL) {
        LOGD("avcodec_alloc_context3 fail");
        goto end;
    }

    ret = avformat_open_input(&fmt_ctx, src_file, NULL, NULL);

    if(ret < 0) {
        LOGD("avformat_open_input fail");
        goto end;
    }

    video_index = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);

    if(video_index == AVERROR_STREAM_NOT_FOUND) {
        LOGD("av_find_best_stream fail");
        goto end;
    }

    video_stream = fmt_ctx->streams[video_index];

    codec = avcodec_find_decoder(video_stream->codecpar->codec_id);
    if(NULL == codec) {
        LOGD("avcodec_find_decoder fail");
        goto end;
    } else {
        LOGD("avcodec_find_decoder 264 code id : %d", AV_CODEC_ID_H264);
        LOGD("avcodec_find_decoder code id : %d", video_stream->codecpar->codec_id);
    }

    ret = avcodec_parameters_to_context(c, video_stream->codecpar);

    if(ret < 0) {
        LOGD("avcodec_parameters_to_context fail");
        goto end;
    }

    ret = avcodec_open2(c, codec, NULL);
    if (ret < 0) {
        av_strerror(ret, errors, 1024);
        LOGD("avcodec_open2 fail ret = %d errors = %s", ret, errors);
        goto end;
    }

    dst_file_fd = fopen(dst_file, "wb");
    if(NULL == dst_file_fd) {
        LOGD("fopen file fail : %s", dst_file);
        return;
    }

    while(1) {
        ret = av_read_frame(fmt_ctx, pkt);
        if (!ret) {
            if(pkt->stream_index == video_index) {
                decode(c, frame, pkt, dst_file_fd, errors);
                av_packet_unref(pkt);
            } else {
                av_packet_unref(pkt);
            }
        } else {
            if(ret == AVERROR_EOF) {
                LOGD("AVPacket get complete");
                break;
            } else {
                av_strerror(ret, errors, 1024);
                LOGD("AVPacket get error : %s", errors);
                break;
            }
        }
    }

    /* flush the decoder */
    decode(c, frame, NULL, dst_file_fd, errors);

    end:

    if(NULL != fmt_ctx) {
        avformat_close_input(&fmt_ctx);
    }

    if(NULL != dst_file_fd) {
        fflush(dst_file_fd);
        fclose(dst_file_fd);
    }
    avcodec_free_context(&c);
    av_frame_free(&frame);
    av_packet_free(&pkt);
    env->ReleaseStringUTFChars(src_mp4_file, src_file);
    env->ReleaseStringUTFChars(dst_yuv_file, dst_file);
    LOGD("videoFileDecode out");
}

int pcm_data_save(SwrContext *swr_ctx, AVFrame *frame, uint8_t * buffer, FILE *fd, int out_sample_rate, int out_channel_nb, int out_sample_size) {

    uint8_t *out_buffer = buffer;
    swr_convert(swr_ctx, &out_buffer, out_channel_nb * out_sample_size * out_sample_rate, (const uint8_t **) frame->data, frame->nb_samples);
    int out_buffer_size = av_samples_get_buffer_size(NULL, out_channel_nb, frame->nb_samples, AV_SAMPLE_FMT_S16, 1);
    fwrite(out_buffer, 1, out_buffer_size, fd);
    LOGD("pcm_data_save frame->nb_samples : %d  out_buffer_size : %d", frame->nb_samples, out_buffer_size);
    return 0;
}

void decodeAudio(AVCodecContext *dec_ctx, AVFrame *frame, AVPacket *pkt,
                 FILE *fd, char * errors, uint8_t * buffer, SwrContext *swr_ctx, int out_sample_rate, int out_channel_nb, int out_sample_size)
{
    int ret;

    ret = avcodec_send_packet(dec_ctx, pkt);
    if (ret < 0) {
        av_strerror(ret, errors, 1024);
        LOGD("Error sending a packet for decoding, ret = %d errors = %s", ret, errors);
        return;
    }

    while (ret >= 0) {
        ret = avcodec_receive_frame(dec_ctx, frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            return;
        }
        else if (ret < 0) {
            LOGD("Error during decoding");
            return;
        }
        LOGD("saving frame %5d  pts : %ld  pkt size : %d", dec_ctx->frame_number, frame->pts, frame->pkt_size);
        pcm_data_save(swr_ctx, frame, buffer, fd, out_sample_rate, out_channel_nb, out_sample_size);
    }
}

extern "C"
JNIEXPORT void JNICALL
Java_com_wl_ffmpegav_MainActivity_audioFileDecode(JNIEnv *env, jobject thiz,
                                                       jstring src_mp4_file, jstring dst_pcm_file) {
    // TODO: implement audioFileDecode()
    LOGD("audioFileDecode in");
    const char * src_file = env->GetStringUTFChars(src_mp4_file, NULL);
    const char * dst_file = env->GetStringUTFChars(dst_pcm_file, NULL);

    const AVCodec *codec = NULL;
    AVFormatContext *fmt_ctx = NULL;
    AVCodecContext *c= NULL;
    AVStream * audio_stream = NULL;
    AVFrame *frame = NULL;
    SwrContext *swr_ctx = NULL;
    char errors[1024];
    int ret = 0;
    AVPacket *pkt = NULL;
    int audio_index = -1;
    FILE *dst_file_fd = NULL;
    pkt = av_packet_alloc();
    uint8_t *out_buffer = NULL;
    int out_sample_size;
    int out_sample_rate;
    int out_buffers_size;
    int out_channel_layout;
    int out_channel_nb;

    enum AVSampleFormat in_sample_fmt;
    int in_sample_rate;
    int in_channel_layout;
    enum AVSampleFormat out_sample_fmt;
    if(pkt == NULL) {
        LOGD("av_packet_alloc fail");
        goto end;
    }

    frame = av_frame_alloc();
    if(frame == NULL) {
        LOGD("av_frame_alloc fail");
        goto end;
    }

    c = avcodec_alloc_context3(codec);
    if(c == NULL) {
        LOGD("avcodec_alloc_context3 fail");
        goto end;
    }

    ret = avformat_open_input(&fmt_ctx, src_file, NULL, NULL);

    if(ret < 0) {
        LOGD("avformat_open_input fail");
        goto end;
    }

    audio_index = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);

    if(audio_index == AVERROR_STREAM_NOT_FOUND) {
        LOGD("av_find_best_stream fail");
        goto end;
    }

    audio_stream = fmt_ctx->streams[audio_index];

    codec = avcodec_find_decoder(audio_stream->codecpar->codec_id);
    if(NULL == codec) {
        LOGD("avcodec_find_decoder fail");
        goto end;
    }

    ret = avcodec_parameters_to_context(c, audio_stream->codecpar);

    if(ret < 0) {
        LOGD("avcodec_parameters_to_context fail");
        goto end;
    }

    ret = avcodec_open2(c, codec, NULL);
    if (ret < 0) {
        av_strerror(ret, errors, 1024);
        LOGD("avcodec_open2 fail ret = %d errors = %s", ret, errors);
        goto end;
    }

    dst_file_fd = fopen(dst_file, "wb");
    if(NULL == dst_file_fd) {
        LOGD("fopen file fail : %s", dst_file);
        return;
    }

    //初始化音频参数

    in_sample_fmt = c->sample_fmt;
    out_sample_fmt = AV_SAMPLE_FMT_S16;

    in_sample_rate = c->sample_rate;
    out_sample_rate = 44100;

    in_channel_layout = c->channel_layout;
    out_channel_layout = AV_CH_LAYOUT_STEREO;

    out_channel_nb = av_get_channel_layout_nb_channels(out_channel_layout);
    out_sample_size = av_get_bytes_per_sample(AV_SAMPLE_FMT_S16);

    out_buffers_size = out_sample_rate * out_channel_nb * out_sample_size;
    LOGD("out_buffers_size : %d", out_buffers_size);
    out_buffer = (uint8_t *) av_malloc(out_buffers_size);
    if(NULL == out_buffer) {
        LOGD("av_malloc out_buffer fail");
        goto end;
    }
    memset(out_buffer, 0, out_buffers_size);

    swr_ctx = swr_alloc_set_opts(0, out_channel_layout, out_sample_fmt, out_sample_rate,
                                 in_channel_layout, in_sample_fmt, in_sample_rate, 0 ,0);

    //防止有吱吱声音
    swr_init(swr_ctx);
    LOGD("out_channel_nb : %d  out_sample_size : %d", out_channel_nb, out_sample_size);
    //
    while(1) {
        ret = av_read_frame(fmt_ctx, pkt);
        if (!ret) {
            if(pkt->stream_index == audio_index) {
                decodeAudio(c, frame, pkt, dst_file_fd, errors, out_buffer, swr_ctx, out_sample_rate, out_channel_nb, out_sample_size);
                av_packet_unref(pkt);
            } else {
                av_packet_unref(pkt);
            }
        } else {
            if(ret == AVERROR_EOF) {
                LOGD("AVPacket get complete");
                break;
            } else {
                av_strerror(ret, errors, 1024);
                LOGD("AVPacket get error : %s", errors);
                break;
            }
        }
    }

    /* flush the decoder */

    decodeAudio(c, frame, NULL, dst_file_fd, errors, out_buffer, swr_ctx, out_sample_rate, out_channel_nb, out_sample_size);

    end:

    if(NULL != fmt_ctx) {
        avformat_close_input(&fmt_ctx);
    }

    if(NULL != dst_file_fd) {
        fflush(dst_file_fd);
        fclose(dst_file_fd);
    }

    if(swr_ctx != NULL) {
        swr_free(&swr_ctx);
    }
    avcodec_close(c);
    avcodec_free_context(&c);
    av_frame_free(&frame);
    av_packet_free(&pkt);
    av_free(out_buffer);
    env->ReleaseStringUTFChars(src_mp4_file, src_file);
    env->ReleaseStringUTFChars(dst_pcm_file, dst_file);
    LOGD("audioFileDecode out");
}

void encodeAudio(AVCodecContext *ctx, AVFrame *frame, AVPacket *pkt,
                   FILE *output)
{
    int ret;

    /* send the frame for encoding */
    ret = avcodec_send_frame(ctx, frame);
    if (ret < 0) {
        LOGD("avcodec_send_frame ret : %d", ret);
        return;
    }

    /* read all the available output packets (in general there may be any
     * number of them */
    while (ret >= 0) {
        ret = avcodec_receive_packet(ctx, pkt);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            return;
        else if (ret < 0) {
            LOGD("avcodec_receive_packet fail : %d", ret);
            return;
        }

        fwrite(pkt->data, 1, pkt->size, output);
        av_packet_unref(pkt);
    }
}

void readAudioFrameFromPCM(uint16_t *data, FILE *pFile, uint64_t index, int data_size) {
    LOGD("readAudioFrameFromPCM in data_size : %d", data_size);
    int result;
    uint64_t data_offset;
    FILE *audio_data_fd = pFile;
    data_offset = index * 1024 * 2 * 2;
    //seek到偏移处
    result = fseek(audio_data_fd, data_offset, SEEK_SET);
    if (result == -1)
    {
        LOGD("readAudioFrameFromPCM fail, fseek fail");
        return;
    }

    fread(data, 2, data_size, audio_data_fd);

    LOGD("readAudioFrameFromPCM out");
}

extern "C"
JNIEXPORT void JNICALL
Java_com_wl_ffmpegav_MainActivity_audioFileEncode(JNIEnv *env, jobject thiz,
                                                       jstring src_pcmfile, jstring dst_aacfile) {
    LOGD("audioFileEncode in");
    const char * src_file = env->GetStringUTFChars(src_pcmfile, NULL);
    const char * dst_file = env->GetStringUTFChars(dst_aacfile, NULL);
    FILE *src_fd = NULL;
    FILE *dst_fd = NULL;
    AVFrame *frame;
    AVPacket *pkt;
    uint64_t file_size = 0;
    int ret = 0;
    int totalSeconds = 0;
    uint64_t audio_frame_count = 0;
    int audio_frame_size = 0;
    uint16_t *samples;
    const char *codec_name = "libfdk_aac";
    const AVCodec *codec = NULL;
    AVCodecContext *context = NULL;
    src_fd = fopen(src_file, "rb");
    LOGD("audioFileEncode : %s, %s", src_file, dst_file);
    if(NULL == src_fd) {
        LOGD("file open fail : %s", src_file);
        goto end;
    }
    dst_fd = fopen(dst_file, "wb");
    if(NULL == dst_fd) {
        LOGD("file open fail : %s", dst_file);
        goto end;
    }
    codec = avcodec_find_encoder_by_name(codec_name);
    if (!codec) {
        LOGD("codec find fail : %s", codec_name);
        goto end;
    } else {
        LOGD("codec find success : %s", codec_name);
    }

    context = avcodec_alloc_context3(codec);
    if(NULL == context) {
        LOGD("avcodec_alloc_context3 context fail");
        goto end;
    }

    context->sample_fmt = AV_SAMPLE_FMT_S16;
    context->sample_rate = 44100;
    context->channel_layout = AV_CH_LAYOUT_STEREO;
    context->bit_rate = 176400;
    context->channels = 2;

    fseek(src_fd, 0, SEEK_END);
    file_size = ftell(src_fd);

    totalSeconds = file_size / (context->sample_rate * context->channels * 2);
    LOGD("src_fd totalSeconds : %d", totalSeconds);

    if (avcodec_open2(context, codec, NULL) < 0) {
        LOGD("avcodec_open2 fail");
        goto end;
    } else {
        LOGD("avcodec_open2 success");
    }

    frame = av_frame_alloc();
    if(NULL == frame) {
        LOGD("av_frame_alloc frame fail");
        goto end;
    }

    pkt = av_packet_alloc();
    if(NULL == pkt) {
        LOGD("av_packet_alloc pkt fail");
        goto end;
    }

    frame->nb_samples     = context->frame_size;
    frame->format         = context->sample_fmt;
    frame->channel_layout = context->channel_layout;

    ret = av_frame_get_buffer(frame, 0);
    if (ret < 0) {
        LOGD("av_frame_get_buffer fail");
        goto end;
    }

    LOGD("context->frame_size : %d", context->frame_size);

    audio_frame_size = context->frame_size * 2;//双通道采样次数
    audio_frame_count = ceil(((double) file_size) / audio_frame_size / 2);//每次采样数据2字节

    LOGD("audio_frame_count : %d", audio_frame_count);

    for (uint64_t i = 0; i < audio_frame_count; ++i) {
        ret = av_frame_make_writable(frame);
        if(ret < 0) {
            LOGD("av_frame_make_writable fail");
            goto end;
        }
        samples = (uint16_t*)frame->data[0];
        if(i == (audio_frame_count - 1)) {//防止文件读取越界
            audio_frame_size = (file_size - i * context->frame_size * 2 * 2);
        }
        readAudioFrameFromPCM(samples, src_fd, i, audio_frame_size);
        encodeAudio(context, frame, pkt, dst_fd);
    }

    /* flush the encoder */
    encodeAudio(context, NULL, pkt, dst_fd);

    end:
    if(NULL != src_fd) {
        fclose(src_fd);
    }
    if(NULL != dst_fd) {
        fflush(dst_fd);
        fclose(dst_fd);
    }
    if(context != NULL) {
        avcodec_close(context);
        avcodec_free_context(&context);
    }
    if(frame != NULL) {
        av_frame_free(&frame);
    }
    if(pkt != NULL) {
        av_packet_free(&pkt);
    }
    env->ReleaseStringUTFChars(src_pcmfile, src_file);
    env->ReleaseStringUTFChars(dst_aacfile, dst_file);
    LOGD("audioFileEncode out");
}

int saveJpg(AVFrame *pFrame, char *out_name) {

    int width = pFrame->width;
    int height = pFrame->height;
    AVCodecContext *pCodeCtx = NULL;


    AVFormatContext *pFormatCtx = avformat_alloc_context();
    // 设置输出文件格式
    pFormatCtx->oformat = av_guess_format("mjpeg", NULL, NULL);

    // 创建并初始化输出AVIOContext
    if (avio_open(&pFormatCtx->pb, out_name, AVIO_FLAG_READ_WRITE) < 0) {
        printf("Couldn't open output file.");
        return -1;
    }

    // 构建一个新stream
    AVStream *pAVStream = avformat_new_stream(pFormatCtx, 0);
    if (pAVStream == NULL) {
        return -1;
    }

    AVCodecParameters *parameters = pAVStream->codecpar;
    parameters->codec_id = pFormatCtx->oformat->video_codec;
    parameters->codec_type = AVMEDIA_TYPE_VIDEO;
    parameters->format = AV_PIX_FMT_YUVJ420P;
    parameters->width = pFrame->width;
    parameters->height = pFrame->height;

    AVCodec *pCodec = avcodec_find_encoder(pAVStream->codecpar->codec_id);

    if (!pCodec) {
        printf("Could not find encoder\n");
        return -1;
    }

    pCodeCtx = avcodec_alloc_context3(pCodec);
    if (!pCodeCtx) {
        fprintf(stderr, "Could not allocate video codec context\n");
        exit(1);
    }

    if ((avcodec_parameters_to_context(pCodeCtx, pAVStream->codecpar)) < 0) {
        fprintf(stderr, "Failed to copy %s codec parameters to decoder context\n",
                av_get_media_type_string(AVMEDIA_TYPE_VIDEO));
        return -1;
    }

    pCodeCtx->time_base = (AVRational) {1, 25};

    if (avcodec_open2(pCodeCtx, pCodec, NULL) < 0) {
        printf("Could not open codec.");
        return -1;
    }

    int ret = avformat_write_header(pFormatCtx, NULL);
    if (ret < 0) {
        printf("write_header fail\n");
        return -1;
    }

    int y_size = width * height;

    //Encode
    // 给AVPacket分配足够大的空间
    AVPacket pkt;
    av_new_packet(&pkt, y_size * 3);

    // 编码数据
    ret = avcodec_send_frame(pCodeCtx, pFrame);
    if (ret < 0) {
        printf("Could not avcodec_send_frame.");
        return -1;
    }

    // 得到编码后数据
    ret = avcodec_receive_packet(pCodeCtx, &pkt);
    if (ret < 0) {
        printf("Could not avcodec_receive_packet");
        return -1;
    }

    ret = av_write_frame(pFormatCtx, &pkt);

    if (ret < 0) {
        printf("Could not av_write_frame");
        return -1;
    }

    av_packet_unref(&pkt);

    //Write Trailer
    av_write_trailer(pFormatCtx);


    avcodec_close(pCodeCtx);
    avio_close(pFormatCtx->pb);
    avformat_free_context(pFormatCtx);
    return 0;
}

void decode_image(AVCodecContext *dec_ctx, AVFrame *frame, AVPacket *pkt, char * errors, SwsContext *img_convert_ctx)
{
    LOGD("decode_image in");
    char buf[1024];
    int ret;

    ret = avcodec_send_packet(dec_ctx, pkt);
    if (ret < 0) {
        av_strerror(ret, errors, 1024);
        LOGD("Error sending a packet for decoding, ret = %d errors = %s", ret, errors);
        return;
    }

    while (ret >= 0) {
        ret = avcodec_receive_frame(dec_ctx, frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            return;
        }
        else if (ret < 0) {
            LOGD("Error during decoding");
            return;
        }

        LOGD("saving frame %5d  pts : %ld  pkt size : %d", dec_ctx->frame_number, frame->pts, frame->pkt_size);
        snprintf(buf, sizeof(buf), "/storage/emulated/0/filefilm/%s-%d.jpg", "video", dec_ctx->frame_number);
        saveJpg(frame, buf);
    }
    LOGD("decode_image out");
}

extern "C" JNIEXPORT void JNICALL
Java_com_wl_ffmpegav_MainActivity_videoToImg(JNIEnv *env, jobject thiz, jstring src_mp4_file) {
    LOGD("videoToImg in");
    const char * src_file = env->GetStringUTFChars(src_mp4_file, NULL);
    LOGD("videoToImg : %s", src_file);
    AVFormatContext *fmt_ctx = NULL;
    AVCodecContext *codecContext= NULL;
    AVFrame *frame = NULL;
    SwsContext *img_convert_ctx = NULL;
    int frame_count = 0;
    char errors[1024];
    int ret = 0;
    AVPacket *pkt = NULL;
    int video_index = -1;
    pkt = av_packet_alloc();
    if(pkt == NULL) {
        LOGD("av_packet_alloc fail");
        goto end;
    }

    frame = av_frame_alloc();
    if(frame == NULL) {
        LOGD("av_frame_alloc fail");
        goto end;
    }

    ret = avformat_open_input(&fmt_ctx, src_file, NULL, NULL);

    if(ret < 0) {
        LOGD("avformat_open_input fail");
        goto end;
    }

    ret = avformat_find_stream_info(fmt_ctx, 0);
    if(ret < 0) {
        LOGD("avformat_find_stream_info fail");
        goto end;
    }

    for (int stream_index = 0; stream_index < fmt_ctx->nb_streams; ++stream_index) {
        AVStream * stream = fmt_ctx->streams[stream_index];
        AVCodecParameters * codecParameters = stream->codecpar;
        if(codecParameters->codec_type != AVMEDIA_TYPE_VIDEO) {
            continue;
        }
        AVCodec * codec = avcodec_find_decoder(codecParameters->codec_id);
        if(!codec) {
            LOGD("avcodec_find_decoder find fail");
            goto end;
        }

        codecContext = avcodec_alloc_context3(codec);
        if(!codecContext) {
            LOGD("avcodec_alloc_context find fail");
            goto end;
        }
        ret = avcodec_parameters_to_context(codecContext, codecParameters);
        LOGD("avcodec_parameters_to_context ret == %d", ret);
        if(ret < 0) {
            LOGD("avcodec_parameters_to_context fail");
            goto end;
        }
        ret =  avcodec_open2(codecContext, codec, 0);
        LOGD("avcodec_open2 ret == %d", ret);
        if(ret < 0) {
            LOGD("avcodec_open2 fail");
            goto end;
        }
        video_index = stream_index;
    }

    LOGD("video_index : %d", video_index);

    LOGD("sws_getContext in width : %d, height : %d", codecContext->width, codecContext->height);
    img_convert_ctx = sws_getContext(codecContext->width, codecContext->height,
                                     codecContext->pix_fmt,
                                     codecContext->width, codecContext->height,
                                     AV_PIX_FMT_BGR24,
                                     SWS_BICUBIC, NULL, NULL, NULL);
    LOGD("sws_getContext out");

    if (img_convert_ctx == NULL)
    {
        LOGD("Cannot initialize the conversion context");
        goto end;
    }

    while(1) {
        ret = av_read_frame(fmt_ctx, pkt);
        if (!ret) {
            if(pkt->stream_index == video_index) {
                frame_count++;
                decode_image(codecContext, frame, pkt, errors, img_convert_ctx);
                av_packet_unref(pkt);
            } else {
                av_packet_unref(pkt);
            }
            if(frame_count >= 10) {
                break;
            }
        } else {
            if(ret == AVERROR_EOF) {
                LOGD("AVPacket get complete");
                break;
            } else {
                av_strerror(ret, errors, 1024);
                LOGD("AVPacket get error : %s", errors);
                break;
            }
        }
    }

    /* flush the decoder */
    decode_image(codecContext, frame, NULL, errors, img_convert_ctx);
    end:

    if(NULL != fmt_ctx) {
        avformat_close_input(&fmt_ctx);
    }

    if(NULL != img_convert_ctx) {
        sws_freeContext(img_convert_ctx);;
    }

    avcodec_free_context(&codecContext);
    av_frame_free(&frame);
    av_packet_free(&pkt);
    env->ReleaseStringUTFChars(src_mp4_file, src_file);
    LOGD("videoToImg out");
}