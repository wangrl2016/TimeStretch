// Copyright (c) 2020 Microsoft Inc. All Rights Reserved.
// Author: razha@microsoft.com (Ran Zhang)

#include <grpcpp/grpcpp.h>
#include <iostream>
#include <memory>
#include <cstdlib>

extern "C" {
#include <libavformat/avformat.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
#include <libavutil/opt.h>
}

#include <sox.h>
#include "glog/logging.h"
#include "tts/base/flags.h"
#include "tts/base/log.h"
#include "tts/server/server_utils.h"
#include "tts/server/tts_service.grpc.pb.h"
#include "tts/server/tts_service.pb.h"
#include "tts/synth/synth.h"
#include "tts/synth/synth_types.pb.h"
#include "tts/base/audio_utils.h"
#include "tts/base/align_utils.h"

DEFINE_string(address, "0.0.0.0:8080", "service address");

using grpc::Server;
using grpc::ServerBuilder;
using grpc::ServerContext;
using grpc::Status;

using synth::Synth;
using synth::TTSOption;

/**
 * 对音频进行变速处理
 *
 * @param srcData 原始数据
 * @param srcSize 原始数据大小，byte为单位
 * @param destData [out] 目标数据，需要调用者管理分配的内存
 * @param destSize [out] 目标数据大小，byte为单位
 * @param tempo 变速大小，范围为[0.5, 100.0]
 * @param channelCount 原始数据声道数
 * @param sampleRate 原始数据采样率
 * @param sampleFormat 原始数据采样率
 * @return 是否变速成功
 */
bool timeStretch(const void* srcData,
                 size_t srcSize,
                 void** destData,
                 size_t& destSize,
                 float tempo = 1.0,
                 int64_t channelLayout = AV_CH_LAYOUT_MONO,
                 int sampleRate = 16000,
                 AVSampleFormat sampleFormat = AV_SAMPLE_FMT_S16) {
    // Set up the filter graph.
    // The filter chain it uses is:
    // (input) -> abuffer -> atempo -> aformat -> abuffersink -> (output)
    // abuffer: This provides the endpoint where you can feed the decoded samples.
    // atempo: The filter accepts exactly one parameter, the audio tempo.
    // If not specified then the filter will assume nominal 1.0 tempo.
    // Tempo must be in the [0.5, 100.0] range.
    // aformat: This converts the samples to the sample freq, channel layout,
    // and sample format required by the audio device.
    // abuffersink: This provides the endpoint where you can read the samples after
    // they have passed through the filter chain.
    if (tempo < 0.5 || tempo > 100) {
        LOG(ERROR) << "Invalid tempo param, tempo " << tempo;
        return false;
    }
    AVFilterGraph* filterGraph = nullptr;
    const AVFilter* abuffer;
    AVFilterContext* abufferCtx = nullptr;
    const AVFilter* atempo;
    AVFilterContext* atempoCtx = nullptr;
    const AVFilter* aformat;
    AVFilterContext* aformatCtx = nullptr;
    const AVFilter* abuffersink;
    AVFilterContext* abuffersinkCtx = nullptr;

    auto clearFFmpegFilters = [&abufferCtx, &atempoCtx,
            &aformatCtx, &abuffersinkCtx, &filterGraph]() ->void {
        if (abufferCtx)
            avfilter_free(abufferCtx);
        if (atempoCtx)
            avfilter_free(atempoCtx);
        if (aformatCtx)
            avfilter_free(aformatCtx);
        if (abuffersinkCtx)
            avfilter_free(abuffersinkCtx);
        if (filterGraph) {
            avfilter_graph_free(&filterGraph);
        }
    };

    // Create a new filter graph, which will contain all the filters.
    filterGraph = avfilter_graph_alloc();
    if (!filterGraph) {
        LOG(ERROR) << "Unable to create filter graph";
        clearFFmpegFilters();
        return false;
    }

    // Create the abuffer filter;
    // it will be used for feeding the data into the graph.
    abuffer = avfilter_get_by_name("abuffer");
    if (!abuffer) {
        LOG(ERROR) << "Could not find the abuffer filter";
        clearFFmpegFilters();
        return false;
    }
    abufferCtx = avfilter_graph_alloc_filter(filterGraph, abuffer, "src");
    if (!abufferCtx) {
        LOG(ERROR) << "Could not allocate the abuffer instance";
        clearFFmpegFilters();
        return false;
    }
    // Set the filter options through the AVOptions API.
    char chLayoutStr[64];
    av_get_channel_layout_string(chLayoutStr,
                                 sizeof(chLayoutStr),
                                 av_get_channel_layout_nb_channels(channelLayout),
                                 channelLayout);
    av_opt_set(abufferCtx, "channel_layout", chLayoutStr, AV_OPT_SEARCH_CHILDREN);
    av_opt_set(abufferCtx,
               "sample_fmt",
               av_get_sample_fmt_name(sampleFormat),
               AV_OPT_SEARCH_CHILDREN);
    av_opt_set_q(abufferCtx,
                 "time_base",
                 (AVRational) {1, sampleRate},
                 AV_OPT_SEARCH_CHILDREN);
    av_opt_set_int(abufferCtx, "sample_rate", sampleRate, AV_OPT_SEARCH_CHILDREN);
    // Now initialize the filter; we pass NULL options, since we have already
    // set all the options above.
    int err = avfilter_init_str(abufferCtx, nullptr);
    if (err < 0) {
        LOG(ERROR) << "Could not initialize the abuffer filter";
        clearFFmpegFilters();
        return false;
    }

    // Create atempo filter
    atempo = avfilter_get_by_name("atempo");
    if (!atempo) {
        LOG(ERROR) << "Could not find the atempo filter";
        clearFFmpegFilters();
        return false;
    }
    atempoCtx = avfilter_graph_alloc_filter(filterGraph, atempo, "atempo");
    if (!atempoCtx) {
        LOG(ERROR) << "Could not allocate the atempo instance";
        clearFFmpegFilters();
        return false;
    }
    // A different way of passing the options is as key/value pairs in a dictionary.
    AVDictionary* optionsDict = nullptr;
    av_dict_set(&optionsDict, "tempo", std::to_string(tempo).c_str(), 0);
    err = avfilter_init_dict(atempoCtx, &optionsDict);
    av_dict_free(&optionsDict);
    if (err < 0) {
        LOG(ERROR) << "Could not initialize the atempo filter";
        clearFFmpegFilters();
        return false;
    }

    // Create the aformat filter.
    // It ensures that the output is of the format we want.
    aformat = avfilter_get_by_name("aformat");
    if (!aformat) {
        LOG(ERROR) << "Could not find the aformat filter";
        clearFFmpegFilters();
        return false;
    }
    aformatCtx = avfilter_graph_alloc_filter(filterGraph, aformat, "aformat");
    if (!aformatCtx) {
        LOG(ERROR) << "Could not allocate the aformat instance";
        clearFFmpegFilters();
        return false;
    }
    // A third way of passing the options is in a string of the form
    // key1=value1:key2=value2...
    uint8_t optionsStr[1024];
    // 指定输出为单声道
    snprintf((char*) optionsStr, sizeof(optionsStr),
             "sample_fmts=%s:sample_rates=%d:channel_layouts=mono",
             av_get_sample_fmt_name(sampleFormat), sampleRate);
    err = avfilter_init_str(aformatCtx, (char*) optionsStr);
    if (err < 0) {
        LOG(ERROR) << "Could not initialize the aformat filter";
        clearFFmpegFilters();
        return false;
    }

    // Finally, create the abuffersink filter;
    // it will be used to get the filtered data out of the graph.
    abuffersink = avfilter_get_by_name("abuffersink");
    if (!abuffersink) {
        LOG(ERROR) << "Could not find the abuffersink filter";
        clearFFmpegFilters();
        return false;
    }
    abuffersinkCtx = avfilter_graph_alloc_filter(filterGraph, abuffersink, "sink");
    if (!abuffersinkCtx) {
        LOG(ERROR) << "Could not allocate the abuffersink instance";
        clearFFmpegFilters();
        return false;
    }
    // This filter takes no options.
    err = avfilter_init_str(abuffersinkCtx, nullptr);
    if (err < 0) {
        LOG(ERROR) << "Could not initialize the abuffersink instance";
        clearFFmpegFilters();
        return false;
    }

    // Connect the filters;
    // in this simple case the filters just form a linear chain.
    err = avfilter_link(abufferCtx, 0, atempoCtx, 0);
    if (err >= 0)
        err = avfilter_link(atempoCtx, 0, aformatCtx, 0);
    else {
        LOG(ERROR) << "Error connecting filters";
        clearFFmpegFilters();
        return false;
    }
    if (err >= 0)
        err = avfilter_link(aformatCtx, 0, abuffersinkCtx, 0);
    if (err < 0) {
        LOG(ERROR) << "Error connecting filters";
        clearFFmpegFilters();
        return false;
    }

    // Configure the graph.
    err = avfilter_graph_config(filterGraph, nullptr);
    if (err < 0) {
        LOG(ERROR) << "Error configuring the filter graph";
        clearFFmpegFilters();
        return false;
    }

    // 分配内存存储生成的数据，比实际需要的要大
    // 没有使用vector的目的是避免频繁分配
    auto tempDestSize = int(float(srcSize) / tempo + 8192);
    void* tempDestData = calloc(tempDestSize, sizeof(uint8_t));
    if (!tempDestData) {
        LOG(ERROR) << "Error allocating buffer";
        // 释放结构体
        clearFFmpegFilters();
        return false;
    }

    // Allocate the frame we will be using to store the data.
    AVFrame* frame = av_frame_alloc();
    if (!frame) {
        LOG(ERROR) << "Error allocating the frame";
        free(tempDestData);
        clearFFmpegFilters();
        return false;
    }

    int srcIndex = 0; // 记录目前处理过的原始数据下标，以byte为单位
    int destIndex = 0; // 记录目前生成的目标数据下标，以byte为单位
    int bytesPerSample = av_get_bytes_per_sample(sampleFormat);

    // 避免代码重复
    auto consumeFrame = [&abuffersinkCtx, &frame,
            &destIndex, &tempDestSize, &tempDestData, &bytesPerSample]() ->void {
        while (av_buffersink_get_frame(abuffersinkCtx, frame) >= 0) {
            int frameSize = frame->nb_samples * bytesPerSample;
            // 只处理第0声道的数据
            if (destIndex + frameSize > tempDestSize) {
                // 说明内存分配够不大，或者是哪里有问题
                LOG(WARNING) << "Malloc buffer small";
                av_frame_unref(frame);
                break;
            }
            memcpy(((char*)tempDestData) + destIndex, frame->extended_data[0], frameSize);
            destIndex += frameSize;
            av_frame_unref(frame);
        }
    };

    bool flushed = false;
    while (srcIndex < srcSize && !flushed) {
        frame->sample_rate = sampleRate;
        frame->format = sampleFormat;
        frame->channel_layout = channelLayout;
        frame->channels = 1;
        frame->nb_samples = 1024;
        // 存在srcSize - srcIndex < frame->nb_samples * bytesPerSamples的情况
        if (srcSize - srcIndex < frame->nb_samples * bytesPerSample) {
            flushed = true;
            int samplesLeft = int(srcSize - srcIndex) / bytesPerSample;
            frame->nb_samples = samplesLeft;
        }
        err = av_frame_get_buffer(frame, 0);
        if (err < 0) {
            LOG(ERROR) << "Error frame get buffer";
            free(tempDestData);
            av_frame_free(&frame);
            clearFFmpegFilters();
            return false;
        }

        // 将数据复制进入frame中
        memcpy(frame->extended_data[0],
               ((char*)srcData) + srcIndex,
               frame->nb_samples * bytesPerSample);
        srcIndex += frame->nb_samples * bytesPerSample;

        // Send the frame to the input of the filter graph.
        err = av_buffersrc_add_frame(abufferCtx, frame);
        if (err < 0) {
            av_frame_unref(frame);
            LOG(ERROR) << "Error submitting the frame to the filter graph";
            continue;
        }

        // Get all the filtered output that is available.
        consumeFrame();
    }

    // flush
    err = av_buffersrc_add_frame(abufferCtx, nullptr);
    if (err >= 0) {
        consumeFrame();
    }

    (*destData) = tempDestData;
    destSize = destIndex;

    clearFFmpegFilters();
    av_frame_free(&frame);
    return true;
}

void fill_response(server::TTSResponse *response, snd_file &out_snd, std::string speaker, std::string phones, std::string text, std::string filetype, std::string lipsync, std::string cachetype, const void* meldata=NULL, size_t melsize=0)
{
    std::string othersox;
    response->set_data(((const char*)out_snd.buffer) + out_snd.offset, out_snd.size);
    response->set_speaker(speaker);
    response->set_phones(phones);
    response->set_text(text);
    response->set_label_type("TACOTRON2WAVEGLOW");
    if (meldata != NULL && melsize > 0)
    {
        response->set_meldata((const char*)meldata, melsize);
    }
    for (size_t i=0; i<out_snd.parts.size(); i++)
    {
        server::SequenceSsml *ssml = response->add_ssml();
        ssml->set_length(out_snd.parts[i].length);
        ssml->set_phonecount(out_snd.parts[i].phonecount);
        ssml->set_breakms(out_snd.parts[i].breakms);
        for (size_t j=0; j<out_snd.parts[i].soxlist.size(); j++)
        {
            if (out_snd.parts[i].soxlist[j].find("pitch=")==0)
            {
                ssml->set_pitch(atof(out_snd.parts[i].soxlist[j].c_str()+6)/500);
            }
            else if (out_snd.parts[i].soxlist[j].find("vol=")==0)
            {
                ssml->set_volume(atof(out_snd.parts[i].soxlist[j].c_str()+4)-1.0);
            }
            else if (out_snd.parts[i].soxlist[j].find("tempo=")==0)
            {
                ssml->set_rate(atof(out_snd.parts[i].soxlist[j].c_str()+6)-1.0);
            }
            else if (othersox.find(out_snd.parts[i].soxlist[j])==std::string::npos)
            {
                othersox = othersox.empty() ? out_snd.parts[i].soxlist[j] : othersox+"#"+out_snd.parts[i].soxlist[j];
            }
        }
    }
    response->set_sox(othersox);
    response->set_timems(out_snd.timems);
    response->set_filetype(filetype);
    response->set_lipsync(lipsync);
    response->set_cachetype(cachetype);
}

size_t gRPCServerWriter_Callback(const void *data, size_t size, void *context, std::string speaker, std::string phones, std::string text, std::string filetype, std::vector<std::tuple<std::string, int, int>> sox, std::string lipsync, bool islast, std::string cachetype, const void* meldata, size_t melsize)
{
    if (data == NULL || size==0 || context == NULL)
        return 0;
    server::TTSResponse response;
    /*
    if (sox.size() > 0) 
    {
        if (sox.size() == 1 && sox[0].length() > 0)
        {
        size_t outsize = size * 16;
        char* outbuf = (char*)malloc(outsize + 44);
        if (outbuf != NULL)
        {
            writeWAVHeader(outbuf, size, 16000, 1);
            memcpy(outbuf + 44, data, size);
            outsize = process_sox_chain(sox[0], outbuf, size + 44, outbuf, outsize + 44);
            if (outsize > 0)
            {
            writeWAVHeader(outbuf, outsize, 16000, 1);
            response.set_data(outbuf, outsize + 44);
            if (!islast) { 
                ((::grpc::ServerWriter<::server::TTSResponse>*)context)->Write(response);
            } else {
                ((::grpc::ServerWriter<::server::TTSResponse>*)context)->WriteLast(response, ::grpc::WriteOptions().set_last_message());
            }
            free(outbuf);
            return outsize;
            }
            free(outbuf);
        }
        }
    }
    void *buffer = newBufferWithWAVHeader(data, size);
    response.set_data(buffer, size+44);
    free(buffer);
    */

    void* destData = nullptr;
    size_t destSize = 0;
    // 只处理存在一个atempo的情况
    bool success = false;
    for (int i = 0; i < sox.size(); i++) {
        std::string effect = std::get<0>(sox[i]);
        if (effect.find("tempo") != std::string::npos) {
            size_t equal_pos = effect.find('=');
            if (equal_pos != std::string::npos) {
                std::string param = effect.substr(equal_pos + 1);   // 跳过等号
                float tempo = std::stof(param);
                // 进行变速处理
                if (timeStretch(data, size, &destData, destSize, tempo)) {
                    success = true;
                    sox.erase(sox.begin() + i);
                }
            }
            break;
        }
    }

    if (success)
        snd_file out_snd = process_sox_chain_list(sox, destData, destSize, filetype.c_str());
    else
        snd_file out_snd = process_sox_chain_list(sox, data, size, filetype.c_str());

    if (destData)
        free(destData);

    if (out_snd.size > 0 && out_snd.buffer != NULL)
    {
        fill_response(&response, out_snd, speaker, phones, text, filetype, lipsync, cachetype, meldata, melsize);
        if (out_snd.buffer != data)
        {
            free(out_snd.buffer);
        }
    }
    if (!islast) 
    { 
        ((::grpc::ServerWriter<::server::TTSResponse>*)context)->Write(response);
    } 
    else 
    {
        ((::grpc::ServerWriter<::server::TTSResponse>*)context)->WriteLast(response, ::grpc::WriteOptions().set_last_message());
    }
    return size;
}

size_t gRPCTTSResponse_Callback(const void *data, size_t size, void *context, std::string speaker, std::string phones, std::string text, std::string filetype, std::vector<std::tuple<std::string, int, int>> sox, std::string lipsync, bool islast, std::string cachetype, const void* meldata, size_t melsize) 
{
    if (data == NULL || size==0 || context == NULL)
        return 0;
    server::TTSResponse *response = (server::TTSResponse *)context;
    std::string allphones = response->phones().empty() ? phones : (response->phones().substr(0, response->phones().length()-3) + "sp" + phones.substr(3));
    std::string alltext = response->text().empty() ? text : response->text() + " " + text;
    std::string alllipsync = response->lipsync().empty() ? lipsync : merge_lipsync(response->lipsync(), lipsync);
    std::string allcachetype = response->cachetype().find(cachetype)==std::string::npos ? response->cachetype() + cachetype : response->cachetype();

    if (!islast)
    {
        if (response->data().length() > 0)
        {
            std::string alldata = response->data() + std::string((const char *)data, size);
            response->set_data((const char *)alldata.c_str(), alldata.length());
        }
        else
        {
            response->set_data((const char *)data, size);
        }
        response->set_phones(allphones);
        response->set_text(alltext);
        response->set_lipsync(alllipsync);
        response->set_cachetype(allcachetype);
        if (meldata != NULL && melsize > 0)
        {
            if (response->meldata().length() > 0)
            {
                std::string allmeldata = response->meldata() + std::string((const char *)meldata, melsize);
                response->set_meldata((const char *)allmeldata.c_str(), allmeldata.length());
            }
            else
            {
                response->set_meldata((const char *)meldata, melsize);
            }
        }
        return size;
    }
    else
    {
        snd_file out_snd;
        if (response->data().length() > 0)
        {
            std::string alldata = response->data() + std::string((const char *)data, size);
            out_snd = process_sox_chain_list(sox, alldata.c_str(), alldata.length(), filetype.c_str());
        }
        else
        {
            out_snd = process_sox_chain_list(sox, data, size, filetype.c_str());
        }
        if (out_snd.size > 0 && out_snd.buffer != NULL)
        {
            if (response->meldata().length() > 0)
            {
                std::string allmeldata = response->meldata() + (meldata!=NULL && melsize>0 ? std::string((const char*)meldata, melsize) : std::string());
                fill_response(response, out_snd, speaker, allphones, alltext, filetype, alllipsync, allcachetype, allmeldata.c_str(), allmeldata.length());
            }
            else
            {
                fill_response(response, out_snd, speaker, allphones, alltext, filetype, alllipsync, allcachetype, meldata, melsize);
            }
            if (out_snd.buffer != data)
            {
                free(out_snd.buffer);
            }
        }
        return out_snd.size;
    }
}

class TTSServiceImpl final : public server::TTSService::Service 
{
public:
    TTSServiceImpl() 
    {
        string config = "external/ttsdata/speaker.json";
        tts_synth_ = std::make_shared<Synth>(config);
    }

    Status PostProcess(ServerContext *context, const server::PostProcessRequest *request, server::TTSResponse *response) override 
    {
        std::vector<fe::SequenceSsmlInfo> sourcessml;
        for (auto fssml = request->sourcessml().begin(); fssml != request->sourcessml().end(); fssml++)
        {
            sourcessml.push_back(fe::SequenceSsmlInfo(fssml->length(), fssml->phonecount(), fssml->breakms(), fssml->volume(), fssml->pitch(), fssml->rate()));
        }
        std::vector<fe::SequenceSsmlInfo> targetssml;
        for (auto fssml = request->targetssml().begin(); fssml != request->targetssml().end(); fssml++)
        {
            targetssml.push_back(fe::SequenceSsmlInfo(fssml->length(), fssml->phonecount(), fssml->breakms(), fssml->volume(), fssml->pitch(), fssml->rate()));
        }
        auto result = tts_synth_->PostProcess(request->data().c_str(), request->data().length(), request->sourcefiletype(), request->targetfiletype(), 
            sourcessml, targetssml, request->speaker(), request->phones(), request->text(), request->sox(), request->lipsync());
        fill_response(response, std::get<0>(result), request->speaker(), request->phones(), request->text(), request->targetfiletype(), std::get<1>(result), std::get<2>(result), NULL, 0);
        return Status::OK;
    }

    Status Frontend(ServerContext *context, const server::TTSRequest *request, server::FrontendResponse *response) override 
    {
        TTSOption option;
        option.set_speaker(request->speaker());
        option.set_text(request->text());
        option.set_ssml(request->ssml());
        option.set_maxsequence(request->maxsequence());
        //option.set_tacotron2speakerid(request->tacotron2speakerid());
        //option.set_sigma(request->sigma());
        //option.set_beginoffset(request->beginoffset());
        //option.set_endoffset(request->endoffset());
        //option.set_zerocutoff(request->zerocutoff());
        //option.set_sox(request->sox());
        //option.set_filetype(request->filetype());
        //option.set_lipsync(request->lipsync());
        option.set_dialect(request->dialect());
        option.set_meldata(request->meldata());
        LOG(INFO) << "TTS Frontend: " << request->text();
        fe::Utterance utt;
        if (tts_synth_->Frontend(option, utt))
        {
            response->set_speaker(request->speaker());
            for (auto sequence : utt.sequenceset)
            {
                LOG(INFO)<<sequence.text<<"|"<<sequence.phones<<"|"<<sequence.ssml.size();
                server::FrontendSequence *fseq = response->add_sequenceset();
                fseq->set_phones(sequence.phones);
                fseq->set_text(sequence.text);
                for (auto ssml : sequence.ssml)
                {
                    LOG(INFO)<<ssml.length<<" "<<ssml.phonecount<<" "<<ssml.breakms<<" "<<ssml.volume<<" "<<ssml.pitch<<" "<<ssml.rate;
                    server::SequenceSsml *fssml = fseq->add_ssml();
                    fssml->set_length(ssml.length);
                    fssml->set_phonecount(ssml.phonecount);
                    fssml->set_breakms(ssml.breakms);
                    fssml->set_volume(ssml.volume);
                    fssml->set_pitch(ssml.pitch);
                    fssml->set_rate(ssml.rate);
                }
            }
            response->set_label_type("TACOTRON2WAVEGLOW");
            response->set_tacotron2speakerid(request->tacotron2speakerid());
            response->set_sigma(request->sigma());
            response->set_beginoffset(request->beginoffset());
            response->set_endoffset(request->endoffset());
            response->set_zerocutoff(request->zerocutoff());
            response->set_sox(request->sox());
            response->set_filetype(request->filetype());
            response->set_lipsync(request->lipsync());
            response->set_meldata(request->meldata());
        }
        return Status::OK;
    }

    Status BackendStream(ServerContext *context, const server::FrontendResponse *request, ::grpc::ServerWriter<::server::TTSResponse>* writer) override 
    {
        TTSOption option;
        
        option.set_speaker(request->speaker());
        fe::Utterance utt;
        for (auto fseq = request->sequenceset().begin(); fseq != request->sequenceset().end(); fseq++)
        {
            std::vector<fe::SequenceSsmlInfo> ssml;
            for (auto fssml = fseq->ssml().begin(); fssml != fseq->ssml().end(); fssml++)
            {
                ssml.push_back(fe::SequenceSsmlInfo(fssml->length(), fssml->phonecount(), fssml->breakms(), fssml->volume(), fssml->pitch(), fssml->rate()));
            }
            utt.sequenceset.push_back(fe::Sequence(fseq->phones(), fseq->text(), ssml));
        }
        option.set_tacotron2speakerid(request->tacotron2speakerid());
        option.set_sigma(request->sigma());
        option.set_beginoffset(request->beginoffset());
        option.set_endoffset(request->endoffset());
        option.set_zerocutoff(request->zerocutoff());
        option.set_sox(request->sox());
        option.set_filetype(request->filetype());
        option.set_lipsync(request->lipsync());
        option.set_accumulatelipsync(false);
        option.set_meldata(request->meldata());
        tts_synth_->BackendStream(option, utt, gRPCServerWriter_Callback, writer);
        return Status::OK;
    }

    Status Backend(ServerContext *context, const server::FrontendResponse *request, server::TTSResponse *response) override 
    {
        TTSOption option;
        option.set_speaker(request->speaker());
        fe::Utterance utt;
        for (auto fseq = request->sequenceset().begin(); fseq != request->sequenceset().end(); fseq++)
        {
            std::vector<fe::SequenceSsmlInfo> ssml;
            for (auto fssml = fseq->ssml().begin(); fssml != fseq->ssml().end(); fssml++)
            {
                ssml.push_back(fe::SequenceSsmlInfo(fssml->length(), fssml->phonecount(), fssml->breakms(), fssml->volume(), fssml->pitch(), fssml->rate()));
            }
            utt.sequenceset.push_back(fe::Sequence(fseq->phones(), fseq->text(), ssml));
        }
        option.set_tacotron2speakerid(request->tacotron2speakerid());
        option.set_sigma(request->sigma());
        option.set_beginoffset(request->beginoffset());
        option.set_endoffset(request->endoffset());
        option.set_zerocutoff(request->zerocutoff());
        option.set_sox(request->sox());
        option.set_filetype(request->filetype());
        option.set_lipsync(request->lipsync());
        option.set_accumulatelipsync(true);
        option.set_meldata(request->meldata());
        tts_synth_->BackendStream(option, utt, gRPCTTSResponse_Callback, response);
        return Status::OK;
    }

    Status SynthesisStream(ServerContext *context, const server::TTSRequest *request, ::grpc::ServerWriter<::server::TTSResponse>* writer) override 
    {
        TTSOption option;
        option.set_speaker(request->speaker());
        option.set_text(request->text());
        option.set_ssml(request->ssml());
        option.set_maxsequence(request->maxsequence());
        option.set_tacotron2speakerid(request->tacotron2speakerid());
        option.set_sigma(request->sigma());
        option.set_beginoffset(request->beginoffset());
        option.set_endoffset(request->endoffset());
        option.set_zerocutoff(request->zerocutoff());
        option.set_sox(request->sox());
        option.set_filetype(request->filetype());
        option.set_lipsync(request->lipsync());
        option.set_accumulatelipsync(false);
        option.set_dialect(request->dialect());
        option.set_meldata(request->meldata());
        
        tts_synth_->SynthesizeStream(option, gRPCServerWriter_Callback, writer);
        return Status::OK;
    }

    Status Synthesis(ServerContext *context, const server::TTSRequest *request, server::TTSResponse *response) override 
    {
        TTSOption option;
        option.set_speaker(request->speaker());
        option.set_text(request->text());
        option.set_ssml(request->ssml());
        option.set_maxsequence(request->maxsequence());
        option.set_tacotron2speakerid(request->tacotron2speakerid());
        option.set_sigma(request->sigma());
        option.set_beginoffset(request->beginoffset());
        option.set_endoffset(request->endoffset());
        option.set_zerocutoff(request->zerocutoff());
        option.set_sox(request->sox());
        option.set_filetype(request->filetype());
        option.set_lipsync(request->lipsync());
        option.set_accumulatelipsync(true);
        option.set_dialect(request->dialect());
        option.set_meldata(request->meldata());

        tts_synth_->SynthesizeStream(option, gRPCTTSResponse_Callback, response);
        return Status::OK;
    }

    std::shared_ptr<Synth> tts_synth_;
};

int main(int argc, char **argv) 
{
    sox_init();
    gflags::SetUsageMessage("xiaoice tts engine");
    gflags::ParseCommandLineFlags(&argc, &argv, true);

    TTSServiceImpl service;
    std::string server_address(FLAGS_address);
    ServerBuilder builder;
    builder.SetMaxSendMessageSize(1024*1024*1024);
    builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
    builder.RegisterService(&service);
    std::unique_ptr<Server> server(builder.BuildAndStart());
    std::cout << "Server listening on " << server_address << std::endl;
    server->Wait();
    sox_quit();
    return 0;
}
