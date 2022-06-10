// Copyright (c) 2020 Microsoft Inc. All Rights Reserved.
// Author: razha@microsoft.com (Ran Zhang)

#include <grpcpp/grpcpp.h>
#include <iostream>
#include <memory>
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
    snd_file out_snd = process_sox_chain_list(sox, data, size, filetype.c_str());
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
