#include "glog/logging.h"
#include "server_base/audio_utils.h"
#include <fstream>
#include <filesystem>
#include <iostream>
#include <cassert>
#include <strings.h>
#include <stdlib.h>
#include <math.h>

namespace WL::Service::Base {
    // 获取唯一文件名
    static std::atomic_uint64_t uniqueFileBase = ATOMIC_VAR_INIT(0);

template <>
void writeFormat<float>(std::ofstream& stream) {
  write<short>(stream, 3);
}

void* newBufferWithWAVHeader(const void *data, size_t size) 
{
    char* buffer = (char*)malloc(44 + size);
    writeWAVHeader(buffer, size, 16000, 1);
    memcpy(buffer + 44, data, size);
    return (void*)buffer;
}  

void writeWAVHeader(
    char* buffer,
    size_t buffersize,
    int sampleRate,
    short channels)
{
    strncpy(buffer, "RIFF", 4);
    int value = 36 + buffersize;
    memcpy(buffer+4, &value, 4);
    strncpy(buffer+8, "WAVEfmt ", 8);
    value = 16;
    memcpy(buffer+16, &value, 4);
    short svalue = 1;
    memcpy(buffer+20, &svalue, 2);
    svalue = channels;
    memcpy(buffer+22, &svalue, 2);
    value = sampleRate;
    memcpy(buffer+24, &value, 4);
    value = sampleRate * channels * sizeof(short);
    memcpy(buffer+28, &value, 4);
    svalue = channels * sizeof(short);
    memcpy(buffer+32, &svalue, 2);
    svalue = 8 * sizeof(short);
    memcpy(buffer+34, &svalue, 2);
    strncpy(buffer+36, "data", 4);
    value = buffersize;
    memcpy(buffer+40, &buffersize, 4);
}

sox_encodinginfo_t *fill_filetype_encoding(sox_encodinginfo_t *encoding, const char* filetype)
{
    if (filetype==NULL || *filetype=='\0' || strcasecmp(filetype, "wav")==0)
        return NULL;
	
    encoding->reverse_bytes = sox_option_default;
	encoding->reverse_nibbles = sox_option_default;
	encoding->reverse_bits = sox_option_default;
	encoding->opposite_endian = sox_false;
	encoding->bits_per_sample = 0;
	encoding->compression = 0;
    if (strcasecmp(filetype, "mp3")==0)
    {
        encoding->encoding = SOX_ENCODING_MP3;
		encoding->compression = 32;
        return encoding;
    }
    if (strcasecmp(filetype, "ogg") == 0) 
    {
		encoding->encoding = SOX_ENCODING_VORBIS;
		encoding->compression = 4;
        return encoding;
	} 
    if (strcasecmp(filetype, "flac") == 0) 
    {
		encoding->encoding = SOX_ENCODING_FLAC;
        encoding->compression = 6;
		return encoding;
	}
    if (strcasecmp(filetype, "amr-nb") == 0)
    {
        encoding->encoding = SOX_ENCODING_AMR_NB;
        return encoding;
    }
    return NULL;
}

int get_filetype_rate(const char* filetype)
{
    if (strcasecmp(filetype, "wav")==0 || strcasecmp(filetype, "raw")==0 ||strcasecmp(filetype, "")==0)
    {
        return 16000;
    }
    if (strcasecmp(filetype, "mp3")==0)
    {
        return 16000;
    }
    if (strcasecmp(filetype, "ogg") == 0) 
    {
		return 16000;
	} 
    if (strcasecmp(filetype, "flac") == 0) 
    {
		return 16000;
	}
    if (strcasecmp(filetype, "amr-nb") == 0)
    {
        return 8000;
    }
    return 16000;
}

void dumpSndFile(const snd_file& sndFile) {
    LOG(INFO) << "snd_file offset " << sndFile.offset
              << ", size " << sndFile.size
              << ", timeMs " << sndFile.timems;

    const std::vector<snd_part>& parts = sndFile.parts;
    for (int i = 0; i < parts.size(); i++) {
        LOG(INFO) << "snd_part " << i
                << ", offset " << parts[i].offset
                << ", length " << parts[i].length
                << ", startms " << parts[i].startms
                << ", timems " << parts[i].timems
                << ", breakms " << parts[i].breakms
                << ", phonecount " << parts[i].phonecount;

        std::string soxListStr;
        for (int j = 0; j < parts[i].soxlist.size(); j++) {
            soxListStr += parts[i].soxlist[j];
            soxListStr += " ";
        }
        LOG(INFO) << soxListStr;
    }
}

void* process_sox_decode_wav(const void *data, size_t size, const char* sourcefiletype, size_t *outsize)
{
    void* outbuf = NULL;
    sox_format_t *in = sox_open_mem_read((void *)data, size, NULL, NULL, sourcefiletype);
    if (in == NULL)
    {
        LOG(ERROR) << "sox_open_mem_read failed";
        return NULL;
    }
    VLOG(1) << "sox_in: err=" << in->sox_errstr << " rate=" << in->signal.rate << " channels=" << in->signal.channels << " precision=" << in->signal.precision << " length="  << in->signal.length;
    char tmpheader[44];
    writeWAVHeader(tmpheader, 0, 16000, 1);
    sox_format_t *tmp = sox_open_mem_read((void *)tmpheader, 44, NULL, NULL, "wav");
    if (tmp == NULL)
    {
        LOG(ERROR) << "sox_open_mem_read failed";
        return NULL;
    }
    sox_format_t *out = sox_open_memstream_write((char **)&outbuf, outsize, &tmp->signal, &tmp->encoding, "wav", NULL);
    sox_close(tmp);
    if (out == NULL)
    {
        LOG(ERROR) << "sox_open_mem_write failed";
        sox_close(in);
        return NULL;
    }
    VLOG(1) << "sox_out: err=" << out->sox_errstr << " rate=" << out->signal.rate << " channels=" << out->signal.channels << " precision=" << out->signal.precision << " length="  << out->signal.length; 
    sox_effects_chain_t *chain = sox_create_effects_chain(&in->encoding, &out->encoding);
    if (chain == NULL)
    {
        sox_close(out);
        sox_close(in);
        return NULL;
    }
    sox_signalinfo_t interm_signal = in->signal;
    char *inargs[1];
    inargs[0] = (char *)in;  
    sox_effect_t *ei = sox_create_effect(sox_find_effect("input"));
    if (ei != NULL && sox_effect_options(ei, 1, inargs) == SOX_SUCCESS)
    {
        sox_add_effect(chain, ei, &interm_signal, &in->signal);
        VLOG(0) << chain->length << ") sox_input: rate=" << interm_signal.rate << " channels=" << interm_signal.channels << " precision=" << interm_signal.precision << " length="  << interm_signal.length;     
        free(ei);
    }
    else
    {
        LOG(ERROR) << "sox_add_effect_option(input) failed";
        if (ei != NULL) free(ei);
        sox_delete_effects_chain(chain);
        sox_close(out);
        sox_close(in);
        return NULL;
    }
    char *outargs[1];
    outargs[0] = (char *)out;
    sox_effect_t *eo = sox_create_effect(sox_find_effect("output"));
    if (eo != NULL && sox_effect_options(eo, 1, outargs) == SOX_SUCCESS)
    {
        sox_add_effect(chain, eo, &interm_signal, &out->signal);
        VLOG(0) << chain->length << ") sox_output" << " rate=" << interm_signal.rate << " channels=" << interm_signal.channels << " precision=" << interm_signal.precision << " length="  << interm_signal.length;     
        free(eo);
    }
    else
    {
        LOG(ERROR) << "sox_add_effect_option(output) failed";
        if (eo != NULL) free(eo);
        sox_delete_effects_chain(chain);
        sox_close(out);
        sox_close(in);
        return NULL;
    }
    if (sox_flow_effects(chain, NULL, NULL) != SOX_SUCCESS)
    {
        LOG(ERROR) << "sox_flow_effects failed";
        sox_delete_effects_chain(chain);
        sox_close(out);
        sox_close(in);
        return NULL;
    }
    VLOG(0) << "Wav) size=" << outsize << " rate=" << out->signal.rate << " channels=" << out->signal.channels << " precision=" << out->signal.precision << " length="  << out->signal.length;
    sox_delete_effects_chain(chain);
    sox_close(out);
    sox_close(in);
    return outbuf;
}

snd_file process_sox_chain_list_type(std::vector<std::tuple<std::string, int, int>> &soxlist, const void *data, size_t size, const char* filetype, const char* sourcefiletype)
{
    snd_file out_snd = { NULL, 0 };
    if ( (0 == soxlist.size() || (1 == soxlist.size() && std::get<0>(soxlist[0]).empty())) && strcasecmp(filetype, sourcefiletype)==0 )
    {
        out_snd.buffer = (char *)data;
        out_snd.size = size;
        out_snd.offset = 0;
        out_snd.timems = (*sourcefiletype=='\0' || strcasecmp(sourcefiletype, "raw")==0) ? size/32 : strcasecmp(filetype, "wav")==0 ? (size-44)/32 : -1;
        return out_snd;
    }
    char* inbuf = NULL;
    if (*sourcefiletype=='\0' || strcasecmp(sourcefiletype, "raw")==0)
    {
        if (inbuf==NULL)
        {
            return out_snd;
        }
        writeWAVHeader(inbuf, soxlist.size()>1 ? std::get<1>(soxlist[0]) : size, 16000, 1);
        memcpy(inbuf + 44, data, size);
        out_snd.buffer = inbuf;
        out_snd.offset = 0;
        out_snd.size = size + 44;
        out_snd.timems = size/32;
        if ( (0 == soxlist.size() || (1 == soxlist.size() && std::get<0>(soxlist[0]).empty())) && (strcasecmp(filetype, "wav")==0 || strcmp(filetype, "")==0) )
        {
            return out_snd;
        }
        sourcefiletype = "wav";
    }
    else if (strcasecmp(sourcefiletype, "wav")==0)
    {
        out_snd.buffer = (void*)data;
        out_snd.offset = 0;
        out_snd.size = size;
        out_snd.timems = (size-44)/32;
        inbuf = (char*)data;
        size -= 44;
    }
    else
    {
        out_snd.buffer = (void*)data;
        out_snd.offset = 0;
        out_snd.size = size;
        out_snd.timems = -1;
        if ( soxlist.size() <= 1 )
        {
            inbuf = (char*)data;
        }
        else
        {
            size_t insize = 0;
            inbuf = (char*)process_sox_decode_wav(data, size, sourcefiletype, &insize);
            if (inbuf==NULL)
            {
                LOG(ERROR) << "decode_wav failed";
                return out_snd;
            }
            out_snd.size = insize;
            size = insize;
            sourcefiletype = "wav";
        }
    }
    //for unknown reason, calling sox_init() and sox_quit() will crash at the second call of open_memstream_write(filetype=mp3)
    //if (sox_init() != SOX_SUCCESS)
    //{
    //    LOG(ERROR) << "sox_init failed";
    //   return out_snd;
    //}
    sox_format_t *in=NULL;
    size_t totalin = 0;
    size_t totalout = 0;
    size_t totalms = 0;
    void *outbuf = NULL;
    size_t outsize = size * 16 + 44; 
    for (size_t i=0; i<soxlist.size()+1; i++)
    {
        if (i < soxlist.size() && std::get<0>(soxlist[i]).substr(0, 4)=="pad=")
        {
            size_t pos = std::get<0>(soxlist[i]).find('@');
            int pad = (int)round(atof(std::get<0>(soxlist[i]).substr(4).c_str()) * 32000);
            int breakms = (pos == std::string::npos) ? pad/32 : (int)round(atof(std::get<0>(soxlist[i]).substr(pos+1).c_str()) * 1000);
            int duration = std::get<1>(soxlist[i])/2*2 - (i > 0 ? std::get<1>(soxlist[i-1])/2*2 : 0);
            totalin += duration;
            memset((char*)outbuf + totalout + 44, 0, pad);
            totalout += pad;
            totalms += pad/32;
            auto last = out_snd.parts.rbegin();
            if (last != out_snd.parts.rend())
            {
                last->length += pad;
                last->timems += pad/32;
                last->padms = pad > 0 ? pad / 32 : - duration / 32;
                last->breakms = breakms;
            }
            VLOG(0) << "[" << i << "] pad=" << totalout << " in=" << totalin << " timems=" << totalms;
            continue;
        }    
        if (soxlist.size()==i && i>1) //last of multiple sox section performs filetype conversion
        {
            if (strcasecmp(filetype, "wav")==0 || strcasecmp(filetype, "")==0) //wav don't need conversion, just fill header
            {
                writeWAVHeader((char*)outbuf, totalout, 16000, 1);
                out_snd.buffer = outbuf;
                out_snd.offset = 0;
                out_snd.size = totalout + 44;
                out_snd.timems = totalms;
                break;
            }
            else if (strcasecmp(filetype, "raw")==0) //raw don't need conversion, just offset buffer
            {
                out_snd.buffer = outbuf;
                out_snd.offset = 44;
                out_snd.size = totalout;
                out_snd.timems = totalms;
                break;
            }
            writeWAVHeader((char*)outbuf, totalout, 16000, 1);
            in = sox_open_mem_read((void *)outbuf, totalout + 44, NULL, NULL, "wav");
        }
        else if (strcasecmp(sourcefiletype, "wav")!=0)
        {
            in = sox_open_mem_read((char*)inbuf, size, NULL, NULL, sourcefiletype);
            totalin += size;
        }
        else if (i < soxlist.size() && std::get<0>(soxlist[i]).empty() && std::get<1>(soxlist[i]) > 0)
        {
            if (i == 0) //first of multiple sox sections
            {
                outbuf = malloc(size * 16 + 44);
                if (outbuf == NULL)
                {
                    LOG(ERROR) << "outbuf malloc failed";
                    sox_close(in);
                    //sox_quit();
                    return out_snd;
                }
            }
            int partsize = std::get<1>(soxlist[i])/2*2-totalin;
            memcpy((char*)outbuf + totalout + 44, (char*)inbuf + totalin + 44, partsize);
            totalout += partsize;
            totalin += partsize;
            totalms += partsize/32;
            VLOG(0) << "[" << i << "] cpy=" << totalout << " in=" << totalin << " timems=" << totalms;
            continue;
        }
        else if (i==0)
        {
            if (soxlist.size() > 1 && std::get<1>(soxlist[0])/2*2 < (int)size)
            {
                writeWAVHeader(inbuf, std::get<1>(soxlist[0])/2*2, 16000, 1);
            }
            in = sox_open_mem_read((char*)inbuf, soxlist.size()>1 ? std::get<1>(soxlist[0])/2*2+44 : size+44, NULL, NULL, "wav");
            totalin += soxlist.size()>1 ? std::get<1>(soxlist[i])/2*2 : size;
        }
        else
        {
            //char temp[44];
            //memcpy(temp, (char*)data + totalin - 44, 44);
            writeWAVHeader((char*)inbuf + totalin, std::get<1>(soxlist[i])/2*2 - totalin, 16000, 1);
            in = sox_open_mem_read((char*)inbuf + totalin, std::get<1>(soxlist[i])/2*2 - totalin + 44, NULL, NULL, "wav");
            //memcpy((char*)data + totalin - 44, temp, 44);
            totalin += std::get<1>(soxlist[i])/2*2 - (i > 0 ? std::get<1>(soxlist[i-1])/2*2 : 0);
        }
        if (in == NULL)
        {
            LOG(ERROR) << "sox_open_mem_read failed";
            //sox_quit();
            free(outbuf);
            return out_snd;
        }
        VLOG(1) << "sox_in: err=" << in->sox_errstr << " rate=" << in->signal.rate << " channels=" << in->signal.channels << " precision=" << in->signal.precision << " length="  << in->signal.length;
        sox_format_t *out;
        if (soxlist.size()==i || soxlist.size()<=1) //last of multiple sox sections or not multiple sox sections
        {
            sox_encodinginfo_t out_encoding;
            if (get_filetype_rate(sourcefiletype) != get_filetype_rate(filetype))
            {
                sox_signalinfo_t outsignal = in->signal;
                outsignal.rate=get_filetype_rate(filetype);
                out = sox_open_memstream_write((char **)&out_snd.buffer, &out_snd.size, &outsignal, fill_filetype_encoding(&out_encoding, filetype), filetype, NULL);
            }
            else
            {
                out = sox_open_memstream_write((char **)&out_snd.buffer, &out_snd.size, &in->signal, fill_filetype_encoding(&out_encoding, filetype), filetype, NULL);
            }
        }
        else if (i == 0) //first of multiple sox sections
        {
            outbuf = malloc(size * 16 + 44);
            if (outbuf == NULL)
            {
                LOG(ERROR) << "outbuf malloc failed";
                sox_close(in);
                //sox_quit();
                return out_snd;
            }
            out = sox_open_mem_write((char*)outbuf + totalout + 44, outsize - totalout - 44, &in->signal, NULL, "raw", NULL);
        }
        else //neither first nor last of multiple sox sections
        {
            out = sox_open_mem_write((char*)outbuf + totalout + 44, outsize - totalout - 44, &in->signal, NULL, "raw", NULL);
        }
        if (out == NULL)
        {
            LOG(ERROR) << "sox_open_mem_write failed";
            sox_close(in);
            //sox_quit();
            free(outbuf);
            return out_snd;
        }
        VLOG(1) << "sox_out: err=" << out->sox_errstr << " rate=" << out->signal.rate << " channels=" << out->signal.channels << " precision=" << out->signal.precision << " length="  << out->signal.length; 
        sox_effects_chain_t *chain = sox_create_effects_chain(&in->encoding, &out->encoding);
        if (chain == NULL)
        {
            sox_close(out);
            sox_close(in);
            //sox_quit();
            free(outbuf);
            return out_snd;
        }
        sox_signalinfo_t interm_signal = in->signal;
        char *inargs[1];
        inargs[0] = (char *)in;  
        sox_effect_t *ei = sox_create_effect(sox_find_effect("input"));
        if (ei != NULL && sox_effect_options(ei, 1, inargs) == SOX_SUCCESS)
        {
            sox_add_effect(chain, ei, &interm_signal, &in->signal);
            VLOG(1) << chain->length << ") sox_input: rate=" << interm_signal.rate << " channels=" << interm_signal.channels << " precision=" << interm_signal.precision << " length="  << interm_signal.length;     
            free(ei);
        }
        else
        {
            LOG(ERROR) << "sox_add_effect_option(input) failed";
            if (ei != NULL) free(ei);
            sox_delete_effects_chain(chain);
            sox_close(out);
            sox_close(in);
            //sox_quit();
            free(outbuf);
            return out_snd;
        }
        size_t start = 0;
        std::string sox = (i==soxlist.size()) ? std::string("") : std::get<0>(soxlist[i]);
        std::vector<std::string> outputsox;
        while (start < sox.length())
        {
            std::string cmd;
            char* userargs[10];
            size_t userargc=0;
            size_t end = sox.find('#', start);
            if (end == std::string::npos)
            {
                end = sox.length();        
            }
            size_t pos = sox.find('=', start);
            if (pos > start && pos < end)
            {
                cmd = sox.substr(start, pos-start);
                char *value = (char *)sox.substr(pos+1, end-pos-1).c_str();
                while (value != NULL && *value != '\0' && userargc < 10)
                {
                    userargs[userargc] = value;
                    value = strchr(value, '_');
                    if (value!=NULL)
                    {
                        *value = '\0';
                        value++;
                    }
                    userargc++;
                }
            }
            else
            {
                cmd = sox.substr(start, end);
            }
            sox_effect_t *eu = sox_create_effect(sox_find_effect(cmd.c_str()));
            if (eu != NULL)
            {
                if (sox_effect_options(eu, userargc, userargs)==SOX_SUCCESS)
                {
                    if (sox_add_effect(chain, eu, &interm_signal, &out->signal)==SOX_SUCCESS)
                    {
                        outputsox.push_back(sox.substr(start, end));
                        VLOG(0) << chain->length << ") sox_" << cmd << " " << (userargc>0?userargs[0]:"") << " " << (userargc>1?userargs[1]:"") << " rate=" << interm_signal.rate << " channels=" << interm_signal.channels << " precision=" << interm_signal.precision << " length="  << interm_signal.length;     
                    }
                }
                free(eu);
            }
            start = end + 1;
        }
        char* rateargs[1];
        rateargs[0] = (char *)(out->signal.rate==8000 ? "8k" : "16k");
        if (interm_signal.rate != out->signal.rate)
        {
            sox_effect_t *er = sox_create_effect(sox_find_effect("rate"));
            if (er != NULL)
            {
                if (sox_effect_options(er, 1, rateargs) == SOX_SUCCESS)
                {
                    if (sox_add_effect(chain, er, &interm_signal, &out->signal) == SOX_SUCCESS)
                    {
                        outputsox.push_back("rate " + std::string(rateargs[0]));
                        VLOG(0) << chain->length << ") sox_rate" << " rate=" << interm_signal.rate << " channels=" << interm_signal.channels << " precision=" << interm_signal.precision << " length="  << interm_signal.length;
                    }                    
                }
                free(er);
            }
        }
        if (interm_signal.channels != out->signal.channels) 
        {
            sox_effect_t *ec = sox_create_effect(sox_find_effect("channels"));
            if (ec != NULL)
            {
                if (sox_effect_options(ec, 0, rateargs) == SOX_SUCCESS)
                {
                    if (sox_add_effect(chain, ec, &interm_signal, &out->signal) == SOX_SUCCESS)
                    {
                        outputsox.push_back("channel");
                        VLOG(0) << chain->length << ") sox_channels" << " rate=" << interm_signal.rate << " channels=" << interm_signal.channels << " precision=" << interm_signal.precision << " length="  << interm_signal.length;
                    }
                }
                free(ec);
            }
        }
        char *outargs[1];
        outargs[0] = (char *)out;
        sox_effect_t *eo = sox_create_effect(sox_find_effect("output"));
        if (eo != NULL && sox_effect_options(eo, 1, outargs) == SOX_SUCCESS)
        {
            sox_add_effect(chain, eo, &interm_signal, &out->signal);
            VLOG(1) << chain->length << ") sox_output" << " rate=" << interm_signal.rate << " channels=" << interm_signal.channels << " precision=" << interm_signal.precision << " length="  << interm_signal.length;     
            free(eo);
        }
        else
        {
            LOG(ERROR) << "sox_add_effect_option(output) failed";
            if (eo != NULL) free(eo);
            sox_delete_effects_chain(chain);
            sox_close(out);
            sox_close(in);
            //sox_quit();
            free(outbuf);
            return out_snd;
        }
        if (sox_flow_effects(chain, NULL, NULL) != SOX_SUCCESS)
        {
            LOG(ERROR) << "sox_flow_effects failed";
            sox_delete_effects_chain(chain);
            sox_close(out);
            sox_close(in);
            //sox_quit();
            free(outbuf);
            return out_snd;
        }
        sox_delete_effects_chain(chain);
        sox_close(out);
        sox_close(in);
        if (soxlist.size()==i || (i==0 && soxlist.size()<=1)) //last of multiple sox section or single sox section
        {
            VLOG(0) << "[Out] size=" << out_snd.size << " rate=" << out->signal.rate << " channels=" << out->signal.channels << " precision=" << out->signal.precision << " length="  << out->signal.length;
            if (outbuf != NULL)
            {
                free(outbuf);
            }
            if (strcasecmp(filetype, "wav")==0 || strcasecmp(filetype, "")==0)
            {
                writeWAVHeader((char*)out_snd.buffer, out_snd.size-44, 16000, 1);
            }
            out_snd.timems = (i>0) ? totalms : (strcasecmp(filetype, "wav")==0 || strcasecmp(filetype, "")==0 || strcasecmp(filetype, "raw")==0) ? (out_snd.size-44) /32 : interm_signal.length / 16;
            break;
        }
        else
        {
            out_snd.parts.push_back(snd_part(totalout, totalout + interm_signal.length * 2, totalms, totalms + interm_signal.length / 16, std::get<2>(soxlist[i]), outputsox));
            totalout += interm_signal.length * 2;
            totalms += interm_signal.length / 16;
            VLOG(0) << "[" << i << "] out=" << totalout << " in=" << totalin << " timems=" << totalms;
        }
    }
    if (out_snd.buffer != inbuf && data != inbuf)
    {
        free(inbuf);
    }
    //sox_quit();
    return out_snd;
}

snd_file process_sox_chain_list(std::vector<std::tuple<std::string, int, int>> &soxlist, const void *data, size_t size, const char* filetype)
{
    snd_file out_snd = { NULL, 0 };
    if ( (0 == soxlist.size() || (1 == soxlist.size() && std::get<0>(soxlist[0]).empty())) && strcasecmp(filetype, "raw")==0 )
    {
        out_snd.buffer = (char *)data;
        out_snd.size = size;
        out_snd.offset = 0;
        out_snd.timems = size/32;
        return out_snd;
    }
    char* inbuf = (char*)malloc(size + 44);
    if (inbuf==NULL)
    {
        return out_snd;
    }
    writeWAVHeader(inbuf, soxlist.size()>1 ? std::get<1>(soxlist[0])/2*2 : size, 16000, 1);
    memcpy(inbuf + 44, data, size);
    out_snd.buffer = inbuf;
    out_snd.offset = 0;
    out_snd.size = size + 44;
    out_snd.timems = size/32;
    //LOG(INFO)<<out_snd.size;
    if ( (0 == soxlist.size() || (1 == soxlist.size() && std::get<0>(soxlist[0]).empty())) && (strcasecmp(filetype, "wav")==0 || strcmp(filetype, "")==0) )
    {
        return out_snd;
    }
    //for unknown reason, calling sox_init() and sox_quit() will crash at the second call of open_memstream_write(filetype=mp3)
    //if (sox_init() != SOX_SUCCESS)
    //{
    //    LOG(ERROR) << "sox_init failed";
    //   return out_snd;
    //}
    sox_format_t *in=NULL;
    size_t totalin = 0;
    size_t totalout = 0;
    size_t totalms = 0;
    void *outbuf = NULL;
    size_t outsize = size * 16 + 44; 
    size_t tmpsize = 0;
    for (size_t i=0; i<soxlist.size()+1; i++)
    {
        if (i < soxlist.size() && std::get<0>(soxlist[i]).substr(0, 4)=="pad=")
        {
            size_t pos = std::get<0>(soxlist[i]).find('@');
            int pad = (int)round(atof(std::get<0>(soxlist[i]).substr(4).c_str()) * 32000);
            int breakms = (pos == std::string::npos) ? pad/32 : (int)round(atof(std::get<0>(soxlist[i]).substr(pos+1).c_str()) * 1000);
            int duration = std::get<1>(soxlist[i])/2*2 - (i > 0 ? std::get<1>(soxlist[i-1])/2*2 : 0);
            totalin += duration;
            if (i == 0) //first of multiple sox sections
            {
                outbuf = malloc(size * 16 + 44);
                if (outbuf == NULL)
                {
                    LOG(ERROR) << "outbuf malloc failed";
                    return out_snd;
                }
            }
            memset((char*)outbuf + totalout + 44, 0, pad);
            totalout += pad;
            totalms += pad/32;
            auto last = out_snd.parts.rbegin();
            if (last != out_snd.parts.rend())
            {
                last->length += pad;
                last->timems += pad/32;
                last->padms = pad > 0 ? pad / 32 : - duration / 32;
                last->breakms = breakms;
            }
            VLOG(0) << "[" << i << "] pad=" << totalout << " in=" << totalin << " timems=" << totalms;
            continue;
        }
        if (soxlist.size()==i && i>1) //last of multiple sox section performs filetype conversion
        {
            if (strcasecmp(filetype, "wav")==0 || strcasecmp(filetype, "")==0) //wav don't need conversion, just fill header
            {
                writeWAVHeader((char*)outbuf, totalout, 16000, 1);
                out_snd.buffer = outbuf;
                out_snd.offset = 0;
                out_snd.size = totalout + 44;
                out_snd.timems = totalms;
                break;
            }
            else if (strcasecmp(filetype, "raw")==0) //raw don't need conversion, just offset buffer
            {
                out_snd.buffer = outbuf;
                out_snd.offset = 44;
                out_snd.size = totalout;
                out_snd.timems = totalms;
                break;
            }
            writeWAVHeader((char*)outbuf, totalout, 16000, 1);
            in = sox_open_mem_read((void *)outbuf, totalout + 44, NULL, NULL, "wav");
        }
        else if (i < soxlist.size() && std::get<0>(soxlist[i]).empty() && std::get<1>(soxlist[i]) > 0)
        {
            if (i == 0) //first of multiple sox sections
            {
                outbuf = malloc(size * 16 + 44);
                if (outbuf == NULL)
                {
                    LOG(ERROR) << "outbuf malloc failed";
                    sox_close(in);
                    //sox_quit();
                    return out_snd;
                }
            }
            int partsize = std::get<1>(soxlist[i])/2*2-totalin;
            memcpy((char*)outbuf + totalout + 44, (char*)inbuf + totalin + 44, partsize);
            totalout += partsize;
            totalin += partsize;
            totalms += partsize/32;
            VLOG(0) << "[" << i << "] cpy=" << totalout << " in=" << totalin << " timems=" << totalms;
            continue;
        }
        else if (i==0)
        {
            in = sox_open_mem_read((char*)inbuf, soxlist.size()>1 ? std::get<1>(soxlist[0])/2*2+44 : size+44, NULL, NULL, "wav");
            totalin += soxlist.size()>1 ? std::get<1>(soxlist[i])/2*2 : size;
        }
        else
        {
            //char temp[44];
            //memcpy(temp, (char*)data + totalin - 44, 44);
            if (i == soxlist.size()) {
                in = sox_open_mem_read((char*)inbuf, soxlist.size()>1 ? std::get<1>(soxlist[0])/2*2+44 : size+44, NULL, NULL, "wav");
                totalin += soxlist.size()>1 ? std::get<1>(soxlist[i])/2*2 : size;
            } else {
                writeWAVHeader((char *)inbuf + totalin, std::get<1>(soxlist[i]) / 2 * 2 - totalin, 16000, 1);
                in = sox_open_mem_read((char *)inbuf + totalin, std::get<1>(soxlist[i]) / 2 * 2 - totalin + 44, NULL, NULL, "wav");
                // memcpy((char*)data + totalin - 44, temp, 44);
                totalin += std::get<1>(soxlist[i]) / 2 * 2 - (i > 0 ? std::get<1>(soxlist[i - 1]) / 2 * 2 : 0);
            }
        }
        if (in == NULL)
        {
            LOG(ERROR) << "sox_open_mem_read failed";
            //sox_quit();
            free(outbuf);
            return out_snd;
        }
        VLOG(2) << "sox_in: err=" << in->sox_errstr << " rate=" << in->signal.rate << " channels=" << in->signal.channels << " precision=" << in->signal.precision << " length="  << in->signal.length;
        sox_format_t *out;
        if (soxlist.size()==i || soxlist.size()<=1) //last of multiple sox sections or not multiple sox sections
        {
            sox_encodinginfo_t out_encoding;
            out = sox_open_memstream_write((char **)&out_snd.buffer, &out_snd.size, &in->signal, fill_filetype_encoding(&out_encoding, filetype), filetype, NULL);
        }
        else if (i == 0) //first of multiple sox sections
        {
            outbuf = malloc(size * 16 + 44);
            if (outbuf == NULL)
            {
                LOG(ERROR) << "outbuf malloc failed";
                sox_close(in);
                //sox_quit();
                return out_snd;
            }
            out = sox_open_mem_write((char*)outbuf + totalout + 44, outsize - totalout - 44, &in->signal, NULL, "raw", NULL);
        }
        else //neither first nor last of multiple sox sections
        {
            out = sox_open_mem_write((char*)outbuf + totalout + 44, outsize - totalout - 44, &in->signal, NULL, "raw", NULL);
        }
        if (out == NULL)
        {
            LOG(ERROR) << "sox_open_mem_write failed";
            sox_close(in);
            //sox_quit();
            free(outbuf);
            return out_snd;
        }
        VLOG(2) << "sox_out: err=" << out->sox_errstr << " rate=" << out->signal.rate << " channels=" << out->signal.channels << " precision=" << out->signal.precision << " length="  << out->signal.length; 
        sox_effects_chain_t *chain = sox_create_effects_chain(&in->encoding, &out->encoding);
        if (chain == NULL)
        {
            sox_close(out);
            sox_close(in);
            //sox_quit();
            free(outbuf);
            return out_snd;
        }
        sox_signalinfo_t interm_signal = in->signal;
        char *inargs[1];
        inargs[0] = (char *)in;  
        sox_effect_t *ei = sox_create_effect(sox_find_effect("input"));
        if (ei != NULL && sox_effect_options(ei, 1, inargs) == SOX_SUCCESS)
        {
            sox_add_effect(chain, ei, &interm_signal, &in->signal);
            VLOG(1) << chain->length << ") sox_input: rate=" << interm_signal.rate << " channels=" << interm_signal.channels << " precision=" << interm_signal.precision << " length="  << interm_signal.length;     
            free(ei);
        }
        else
        {
            LOG(ERROR) << "sox_add_effect_option(input) failed";
            if (ei != NULL) free(ei);
            sox_delete_effects_chain(chain);
            sox_close(out);
            sox_close(in);
            //sox_quit();
            free(outbuf);
            return out_snd;
        }
        size_t start = 0;
        std::string sox = (i==soxlist.size()) ? std::string("") : std::get<0>(soxlist[i]);
        std::vector<std::string> outputsox;
        while (start < sox.length())
        {
            std::string cmd;
            std::string cmdparam;
            char* userargs[10];
            size_t userargc=0;
            size_t end = sox.find('#', start);
            if (end == std::string::npos)
            {
                end = sox.length();        
            }
            size_t pos = sox.find('=', start);
            if (pos > start && pos < end)
            {
                cmd = sox.substr(start, pos-start);
                cmdparam = sox.substr(pos+1, end-pos-1);
                char *value = (char *)cmdparam.c_str();
                while (value != NULL && *value != '\0' && userargc < 10)
                {
                    userargs[userargc] = value;
                    value = strchr(value, '_');
                    if (value!=NULL)
                    {
                        *value = '\0';
                        value++;
                    }
                    userargc++;
                }
            }
            else
            {
                cmd = sox.substr(start, end);
            }
            sox_effect_t *eu = sox_create_effect(sox_find_effect(cmd.c_str()));
            if (eu != NULL)
            {
                if (sox_effect_options(eu, userargc, userargs)==SOX_SUCCESS)
                {
                    if (sox_add_effect(chain, eu, &interm_signal, &out->signal) == SOX_SUCCESS)
                    {
                        outputsox.push_back(sox.substr(start, end));
                        VLOG(0) << chain->length << ") sox_" << cmd << " " << (userargc>0?userargs[0]:"") << " " << (userargc>1?userargs[1]:"") << " rate=" << interm_signal.rate << " channels=" << interm_signal.channels << " precision=" << interm_signal.precision << " length="  << interm_signal.length;
                    }
                }
                free(eu);
            }
            start = end + 1;
        }
        char* rateargs[1];
        rateargs[0] = (char *)(out->signal.rate==8000 ? "8k" : "16k");
        if (interm_signal.rate != out->signal.rate)
        {
            sox_effect_t *er = sox_create_effect(sox_find_effect("rate"));
            if (er != NULL)
            {
                if (sox_effect_options(er, 1, rateargs) == SOX_SUCCESS)
                {
                    if (sox_add_effect(chain, er, &interm_signal, &out->signal) == SOX_SUCCESS)
                    {
                        outputsox.push_back("rate " + std::string(rateargs[0]));
                        VLOG(0) << chain->length << ") sox_rate" << " rate=" << interm_signal.rate << " channels=" << interm_signal.channels << " precision=" << interm_signal.precision << " length="  << interm_signal.length;
                    }
                }
                free(er);
            }
        }
        if (interm_signal.channels != out->signal.channels) 
        {
            sox_effect_t *ec = sox_create_effect(sox_find_effect("channels"));
            if (ec != NULL)
            {
                if (sox_effect_options(ec, 0, rateargs) == SOX_SUCCESS)
                {
                    if (sox_add_effect(chain, ec, &interm_signal, &out->signal) == SOX_SUCCESS)
                    {
                        outputsox.push_back("channel");
                        VLOG(0) << chain->length << ") sox_channels" << " rate=" << interm_signal.rate << " channels=" << interm_signal.channels << " precision=" << interm_signal.precision << " length="  << interm_signal.length;     
                    }
                }
                free(ec);
            }
        }
        char *outargs[1];
        outargs[0] = (char *)out;
        sox_effect_t *eo = sox_create_effect(sox_find_effect("output"));
        if (eo != NULL && sox_effect_options(eo, 1, outargs) == SOX_SUCCESS)
        {
            sox_add_effect(chain, eo, &interm_signal, &out->signal);
            VLOG(1) << chain->length << ") sox_output" << " rate=" << interm_signal.rate << " channels=" << interm_signal.channels << " precision=" << interm_signal.precision << " length="  << interm_signal.length;     
            free(eo);
        }
        else
        {
            LOG(ERROR) << "sox_add_effect_option(output) failed";
            if (eo != NULL) free(eo);
            sox_delete_effects_chain(chain);
            sox_close(out);
            sox_close(in);
            //sox_quit();
            free(outbuf);
            return out_snd;
        }
        if (sox_flow_effects(chain, NULL, NULL) != SOX_SUCCESS)
        {
            LOG(ERROR) << "sox_flow_effects failed";
            sox_delete_effects_chain(chain);
            sox_close(out);
            sox_close(in);
            //sox_quit();
            free(outbuf);
            return out_snd;
        }
        if (soxlist.size()==i || (i==0 && soxlist.size()<=1)) //last of multiple sox section or single sox section
        {
            tmpsize = out_snd.size;
            VLOG(0) << "[Out] size=" << out_snd.size << " rate=" << out->signal.rate << " channels=" << out->signal.channels << " precision=" << out->signal.precision << " length="  << out->signal.length;
            sox_delete_effects_chain(chain);
            sox_close(out);
            sox_close(in);
            if (outbuf != NULL)
            {
                free(outbuf);
            }
            if (strcasecmp(filetype, "wav")==0 || strcasecmp(filetype, "")==0)
            {
                writeWAVHeader((char*)out_snd.buffer, out_snd.size-44, 16000, 1);
            }
            out_snd.timems = (i>0) ? totalms : (strcasecmp(filetype, "wav")==0 || strcasecmp(filetype, "")==0 || strcasecmp(filetype, "raw")==0) ? (out_snd.size-44) /32 : interm_signal.length / 16;
            break;
        }
        else
        {
            sox_delete_effects_chain(chain);
            sox_close(out);
            sox_close(in);
            out_snd.parts.push_back(snd_part(totalout, totalout + interm_signal.length * 2, totalms, totalms + interm_signal.length / 16, std::get<2>(soxlist[i]), outputsox));
            totalout += interm_signal.length * 2;
            totalms += interm_signal.length / 16;
            VLOG(0) << "[" << i << "] out=" << totalout << " in=" << totalin << " timems=" << totalms;
        }
    }
    if (out_snd.size == 0 && tmpsize > 0) {
        out_snd.size = -tmpsize;
    }
    if (out_snd.buffer != inbuf)
    {
        free(inbuf);
    }
    //sox_quit();
    return out_snd;
}
/*
if (strcasecmp(filetype, "wav")==0 || strcasecmp(filetype, "")==0)
{
    writeWAVHeader((char*)outbuf, totalout, 16000, 1);
    out_snd.buff = outbuf;
    out_snd.size = totalout + 44;
}
else if (strcasecmp(filetype, "raw")==0 || strcasecmp(filetype, "")==0)
{
    out_snd.buff = (char*)outbuf + 44;
    out_snd.size = totalout;
}
else
{
    writeWAVHeader((char*)outbuf, totalout, 16000, 1);
    sox_format_t *in = sox_open_mem_read((void *)outbuf, totalout + 44, NULL, NULL, "wav");
    if (in == NULL)
    {
        LOG(ERROR) << "sox_open_mem_read failed";
        return out_snd;
    }
    LOG(INFO) << "sox_in: err=" << in->sox_errstr << " rate=" << in->signal.rate << " channels=" << in->signal.channels << " precision=" << in->signal.precision << " length="  << in->signal.length;
    sox_encodinginfo_t out_encoding;
    sox_format_t *out = sox_open_memstream_write((char **)&out_snd.buff, &out_snd.size, &in->signal, fill_filetype_encoding(&out_encoding, filetype), filetype, NULL);
    if (out == NULL)
    {
        LOG(ERROR) << "sox_open_mem_write failed";
        sox_close(in);
        return out_snd;
    }
    LOG(INFO) << "sox_out: err=" << out->sox_errstr << " rate=" << out->signal.rate << " channels=" << out->signal.channels << " precision=" << out->signal.precision << " length="  << out->signal.length; 
    sox_effects_chain_t *chain = sox_create_effects_chain(&in->encoding, &out->encoding);
    if (chain == NULL)
    {
        sox_close(out);
        sox_close(in);
        return out_snd;
    }
    sox_signalinfo_t interm_signal = in->signal;
    char *inargs[1];
    inargs[0] = (char *)in;  
    sox_effect_t *ei = sox_create_effect(sox_find_effect("input"));
    if (ei != NULL && sox_effect_options(ei, 1, inargs) == SOX_SUCCESS)
    {
        sox_add_effect(chain, ei, &interm_signal, &in->signal);
        LOG(INFO) << chain->length << ") sox_input: rate=" << interm_signal.rate << " channels=" << interm_signal.channels << " precision=" << interm_signal.precision << " length="  << interm_signal.length;     
        free(ei);
    }
    else
    {
        LOG(ERROR) << "sox_add_effect_option(input) failed";
        if (ei != NULL) free(ei);
        sox_delete_effects_chain(chain);
        sox_close(out);
        sox_close(in);
        return out_snd;
    }
    char *outargs[1];
    outargs[0] = (char *)out;
    sox_effect_t *eo = sox_create_effect(sox_find_effect("output"));
    if (eo != NULL && sox_effect_options(eo, 1, outargs) == SOX_SUCCESS)
    {
        sox_add_effect(chain, eo, &interm_signal, &out->signal);
        LOG(INFO) << chain->length << ") sox_output" << " rate=" << interm_signal.rate << " channels=" << interm_signal.channels << " precision=" << interm_signal.precision << " length="  << interm_signal.length;     
        free(eo);
    }
    else
    {
        LOG(ERROR) << "sox_add_effect_option(output) failed";
        if (eo != NULL) free(eo);
        sox_delete_effects_chain(chain);
        sox_close(out);
        sox_close(in);
        return out_snd;
    }
    if (sox_flow_effects(chain, NULL, NULL) != SOX_SUCCESS)
    {
        LOG(ERROR) << "sox_flow_effects failed";
        sox_delete_effects_chain(chain);
        sox_close(out);
        sox_close(in);
        return out_snd;
    }
    sox_delete_effects_chain(chain);
    sox_close(out);
    sox_close(in);
    LOG(INFO) << "Out) size=" << out_snd.size << " rate=" << out->signal.rate << " channels=" << out->signal.channels << " precision=" << out->signal.precision << " length="  << out->signal.length;
    free(outbuf);
}
*/

/*
snd_file process_sox_chain(std::string sox, const void *data, size_t size, const char* filetype)
{
    snd_file out_snd = { NULL, 0 };
    if (sox_init() != SOX_SUCCESS)
    {
        LOG(ERROR) << "sox_init failed";
        return out_snd;
    }
    sox_format_t *in = sox_open_mem_read((void *)data, size, NULL, NULL, "wav");
    if (in == NULL)
    {
        LOG(ERROR) << "sox_open_mem_read failed";
        return out_snd;
    }
    LOG(INFO) << "sox_in: err=" << in->sox_errstr << " rate=" << in->signal.rate << " channels=" << in->signal.channels << " precision=" << in->signal.precision << " length="  << in->signal.length;
    sox_encodinginfo_t out_encoding;
    sox_format_t *out = sox_open_memstream_write((char **)&out_snd.buffer, &out_snd.size, &in->signal, fill_filetype_encoding(&out_encoding, filetype), filetype, NULL);
    if (out == NULL)
    {
        LOG(ERROR) << "sox_open_mem_write failed";
        sox_close(in);
        return out_snd;
    }
    LOG(INFO) << "sox_out: err=" << out->sox_errstr << " rate=" << out->signal.rate << " channels=" << out->signal.channels << " precision=" << out->signal.precision << " length="  << out->signal.length; 
    sox_effects_chain_t *chain = sox_create_effects_chain(&in->encoding, &out->encoding);
    if (chain == NULL)
    {
        sox_close(out);
        sox_close(in);
        return out_snd;
    }
    sox_signalinfo_t interm_signal = in->signal;
    char *inargs[1];
    inargs[0] = (char *)in;  
    sox_effect_t *ei = sox_create_effect(sox_find_effect("input"));
    if (ei != NULL && sox_effect_options(ei, 1, inargs) == SOX_SUCCESS)
    {
        sox_add_effect(chain, ei, &interm_signal, &in->signal);
        LOG(INFO) << chain->length << ") sox_input: rate=" << interm_signal.rate << " channels=" << interm_signal.channels << " precision=" << interm_signal.precision << " length="  << interm_signal.length;     
        free(ei);
    }
    else
    {
        LOG(ERROR) << "sox_add_effect_option(input) failed";
        if (ei != NULL) free(ei);
        sox_delete_effects_chain(chain);
        sox_close(out);
        sox_close(in);
        return out_snd;
    }
    size_t start = 0;
    while (start < sox.length())
    {
        std::string cmd;
        char* userargs[10];
        size_t userargc=0;
        size_t end = sox.find('#', start);
        if (end == std::string::npos)
        {
            end = sox.length();        
        }
        size_t pos = sox.find('=', start);
        if (pos > start && pos < end)
        {
            cmd = sox.substr(start, pos-start);
            char *value = (char *)sox.substr(pos+1, end-pos-1).c_str();
            while (value != NULL && *value != '\0' && userargc < 10)
            {
                userargs[userargc] = value;
                value = strchr(value, '_');
                if (value!=NULL)
                {
                    *value = '\0';
                    value++;
                }
                userargc++;
            }
        }
        else
        {
            cmd = sox.substr(start, end);
        }
        sox_effect_t *eu = sox_create_effect(sox_find_effect(cmd.c_str()));
        if (eu != NULL)
        {
            if (sox_effect_options(eu, userargc, userargs)==SOX_SUCCESS)
            {
                sox_add_effect(chain, eu, &interm_signal, &out->signal);
                LOG(INFO) << chain->length << ") sox_" << cmd << " " << (userargc>0?userargs[0]:"") << " " << (userargc>1?userargs[1]:"") << " rate=" << interm_signal.rate << " channels=" << interm_signal.channels << " precision=" << interm_signal.precision << " length="  << interm_signal.length;     
            }
            free(eu);
        }
        start = end + 1;
    }
    char* rateargs[1];
    rateargs[0] = (char *)"16k";
    if (interm_signal.rate != out->signal.rate)
    {
        sox_effect_t *er = sox_create_effect(sox_find_effect("rate"));
        if (er != NULL)
        {
            if (sox_effect_options(er, 0, rateargs) == SOX_SUCCESS)
            {
                sox_add_effect(chain, er, &interm_signal, &out->signal);
                LOG(INFO) << chain->length << ") sox_rate" << " rate=" << interm_signal.rate << " channels=" << interm_signal.channels << " precision=" << interm_signal.precision << " length="  << interm_signal.length;     
            }
            free(er);
        }
    }
    if (interm_signal.channels != out->signal.channels) 
    {
        sox_effect_t *ec = sox_create_effect(sox_find_effect("channels"));
        if (ec != NULL)
        {
            if (sox_effect_options(ec, 0, rateargs) == SOX_SUCCESS)
            {
                sox_add_effect(chain, ec, &interm_signal, &out->signal);
                LOG(INFO) << chain->length << ") sox_channels" << " rate=" << interm_signal.rate << " channels=" << interm_signal.channels << " precision=" << interm_signal.precision << " length="  << interm_signal.length;     
            }
            free(ec);
        }
    }
    char *outargs[1];
    outargs[0] = (char *)out;
    sox_effect_t *eo = sox_create_effect(sox_find_effect("output"));
    if (eo != NULL && sox_effect_options(eo, 1, outargs) == SOX_SUCCESS)
    {
        sox_add_effect(chain, eo, &interm_signal, &out->signal);
        LOG(INFO) << chain->length << ") sox_output" << " rate=" << interm_signal.rate << " channels=" << interm_signal.channels << " precision=" << interm_signal.precision << " length="  << interm_signal.length;     
        free(eo);
    }
    else
    {
        LOG(ERROR) << "sox_add_effect_option(output) failed";
        if (eo != NULL) free(eo);
        sox_delete_effects_chain(chain);
        sox_close(out);
        sox_close(in);
        return out_snd;
    }
    if (sox_flow_effects(chain, NULL, NULL) != SOX_SUCCESS)
    {
        LOG(ERROR) << "sox_flow_effects failed";
        sox_delete_effects_chain(chain);
        sox_close(out);
        sox_close(in);
        return out_snd;
    }
    sox_delete_effects_chain(chain);
    sox_close(out);
    sox_close(in);
    sox_quit();
    LOG(INFO) << "Out) size=" << out_snd.size << " rate=" << out->signal.rate << " channels=" << out->signal.channels << " precision=" << out->signal.precision << " length="  << out->signal.length;     
    return out_snd;
}
*/

    snd_file process_sox_effect_chain(std::vector<std::tuple<std::string, int, int>>& soxList,
                                      const void* data,
                                      size_t size,
                                      const char* filetype) {
        // 初始化结构体
        snd_file out_snd;
        out_snd.buffer = nullptr;
        out_snd.offset = 0;
        out_snd.size = 0;
        out_snd.timems = 0;
        out_snd.parts.clear();

        if (soxList.empty() && strcasecmp(filetype, "raw") == 0) {
            // 表示没有效果要处理，同时是裸数据，那么直接通过
            out_snd.buffer = (char*) data;
            out_snd.size = size;
            out_snd.timems = size / 2 / 16000 * 1000;
            return out_snd;
        }

        // 将输入构建成为wav的形式
        char* inbuf = (char*) calloc(size + 44, sizeof(char));
        if (inbuf == nullptr) {
            // 分配内存失败
            LOG(ERROR) << "Calloc buffer failed, size " << size + 44;
            return out_snd;
        }

        // 将wav文件头写入到inbuf中
        writeWAVHeader(inbuf, size, 16000, 1);
        // 将数据写入到inbuf中
        memcpy(inbuf + 44, data, size);

//        // dump inbuf数据
//        std::ofstream outStream("inbuf.wav",
//                                std::ios::out | std::ios::binary);
//        for (int i = 0; i < size + 44; i++) {
//            outStream.write(inbuf + i, sizeof(char));
//        }

        out_snd.buffer = inbuf;
        out_snd.offset = 0;
        out_snd.size = size + 44;
        out_snd.timems = size / 32;

        if (soxList.empty() && (strcasecmp(filetype, "wav") == 0 || strcasecmp(filetype, "") == 0)) {
            return out_snd;
        }

        // Reading and writing audio files stored in memory buffers
        // rather than actual files.
//        char* inBufferData = inbuf;
//        int inBufferSize = int(size) + 44;
//
//        sox_format_t* in, *out;
//
//        in = sox_open_mem_read(inBufferData, inBufferSize, NULL, NULL, NULL);
//        out = sox_open_write("outBuffer.wav", &in->signal, NULL, NULL, NULL, NULL);
//
//#define MAX_SAMPLES (size_t)2048
//        size_t numberRead;
//        sox_sample_t samples[MAX_SAMPLES];
//        while ((numberRead = sox_read(in, samples, MAX_SAMPLES))) {
//            sox_write(out, samples, numberRead);
//        }
//        sox_close(in);
//        sox_close(out);

        // 效果处理
        sox_format_t* in, *out;     // input and output files
        sox_effects_chain_t* chain;
        sox_effect_t* e;
        char* args[10];

        sox_signalinfo_t interm_signal; // intermediate points in the chain.
        char* inBufferData = inbuf;
        int inBufferSize = int(size) + 44;

        // 默认使用MP3进行输出
        sox_encodinginfo_t out_encoding = {
                SOX_ENCODING_MP3,
                0,
                32,
                sox_option_default,
                sox_option_default,
                sox_option_default,
                sox_false
        };

        if (strcasecmp(filetype, "wav") == 0) {
            out_encoding.encoding = SOX_ENCODING_FLOAT;
            out_encoding.compression = 0;
        }

        sox_signalinfo_t out_signal = {
                16000,
                1,
                0,
                0,
                nullptr
        };

        in = sox_open_mem_read(inBufferData,
                               inBufferSize,
                               nullptr,
                               nullptr,
                               "wav");

        uint64_t unique_num = uniqueFileBase.fetch_add(1);
        std::string file_name;
        if (strcasecmp(filetype, "mp3") == 0)
            file_name = std::to_string(unique_num) + ".mp3";
        else if (strcasecmp(filetype, "wav") == 0)
            file_name = std::to_string(unique_num) + ".wav";

        out = sox_open_write(file_name.c_str(),
                             &out_signal,
                             &out_encoding,
                             nullptr,
                             nullptr,
                             nullptr);

        chain = sox_create_effects_chain(&in->encoding, &out->encoding);

        interm_signal = in->signal; // deep copy

        e = sox_create_effect(sox_find_effect("input"));
        args[0] = (char*) in;
        assert(sox_effect_options(e, 1, args) == SOX_SUCCESS);
        assert(sox_add_effect(chain, e, &interm_signal, &in->signal) == SOX_SUCCESS);
        free(e);

        if (in->signal.rate != out->signal.rate) {
            e = sox_create_effect(sox_find_effect("rate"));
            assert(sox_effect_options(e, 0, nullptr) == SOX_SUCCESS);
            assert(sox_add_effect(chain, e, &interm_signal, &out->signal) == SOX_SUCCESS);
            free(e);
        }

        if (in->signal.channels != out->signal.channels) {
            e = sox_create_effect(sox_find_effect("channels"));
            assert (sox_effect_options(e, 0, nullptr) == SOX_SUCCESS);
            assert(sox_add_effect(chain, e, &interm_signal, &out->signal) == SOX_SUCCESS);
            free(e);
        }

        // 增加需要处理的效果
        std::vector<std::string> effects;
        effects.reserve(soxList.size());
        for (auto& l : soxList) {
            effects.push_back(std::get<0>(l));
        }

        for (auto& effect : effects) {
            size_t equal_pos = effect.find('=');
            if (equal_pos != std::string::npos) {
                std::string command = effect.substr(0, equal_pos);
                std::string param = effect.substr(equal_pos + 1);   // 跳过等号

                e = sox_create_effect(sox_find_effect(command.c_str()));
                args[0] = (char*) param.c_str();
                assert(sox_effect_options(e, 1, args) == SOX_SUCCESS);
                assert(sox_add_effect(chain, e, &interm_signal, &out->signal) == SOX_SUCCESS);
                free(e);

                if (command == "tempo") {
                    // 如果是变速，需要修改时间长度
                    out_snd.timems = int(double(out_snd.timems) / strtod(param.c_str(), nullptr));
                }
            } else {
                e = sox_create_effect(sox_find_effect(effect.c_str()));
                assert(sox_effect_options(e, 0, nullptr) == SOX_SUCCESS);
                assert(sox_add_effect(chain, e, &interm_signal, &out->signal) == SOX_SUCCESS);
                free(e);
            }
        }

        e = sox_create_effect(sox_find_effect("output"));
        args[0] = (char*) out;
        assert(sox_effect_options(e, 1, args) == SOX_SUCCESS);
        assert(sox_add_effect(chain, e, &interm_signal, &out->signal) == SOX_SUCCESS);
        free(e);

        sox_flow_effects(chain, nullptr, nullptr);

        sox_delete_effects_chain(chain);
        sox_close(out);
        sox_close(in);

        uint64_t file_size = std::filesystem::file_size(file_name);

        // 读文件操作
        std::ifstream in_stream;
        in_stream.open(file_name, std::ios::binary);

        char* out_buffer = (char*) calloc(file_size, sizeof(char));
        in_stream.read((char*) out_buffer, long (sizeof(char)) * long(file_size));

        out_snd.buffer = out_buffer;
        out_snd.size = file_size;

        // 关闭流
        in_stream.close();

        if (inbuf) {
            free(inbuf);
            inbuf = nullptr;
        }
        std::remove(file_name.c_str());

        return out_snd;
    }
}
