#ifndef SERVICE_BASE_AUDIO_UTILS_H_
#define SERVICE_BASE_AUDIO_UTILS_H_

#include<fstream>
#include<string.h>
#include<sstream>
#include<typeinfo>
#include <tuple>
// #include "sox.h"

namespace WL::Service::Base {

void writeWAVHeader(
    char* buffer,
    size_t buffersize,
    int sampleRate,
    short channels);

template <typename T>
void write(std::ofstream& stream, const T& t) {
  stream.write((const char*)&t, sizeof(T));
}

template <typename T>
void writeFormat(std::ofstream& stream) {
  write<short>(stream, 1);
}

template <>
void writeFormat<float>(std::ofstream& stream);

template <typename SampleType>
void writeWAVData(
  char const* outFile,
  SampleType* buf,
  size_t bufSize,
  int sampleRate,
  short channels)
{
    std::ofstream stream(outFile, std::ios::binary);
    stream.write("RIFF", 4);
    write<int>(stream, 36 + bufSize);
    stream.write("WAVE", 4);
    stream.write("fmt ", 4);
    write<int>(stream, 16);
    writeFormat<SampleType>(stream);                                // Format
    write<short>(stream, channels);                                 // Channels
    write<int>(stream, sampleRate);                                 // Sample Rate
    write<int>(stream, sampleRate * channels * sizeof(SampleType)); // Byterate
    write<short>(stream, channels * sizeof(SampleType));            // Frame size
    write<short>(stream, 8 * sizeof(SampleType));                   // Bits per sample
    stream.write("data", 4);
    stream.write((const char*)&bufSize, 4);
    stream.write((const char*)buf, bufSize);
}
// insert 44 bytes WAV header to the front
void* newBufferWithWAVHeader(const void *data, size_t size);
// used as callback function to write audio result
typedef size_t (*writeaudio_t)(const void *data, size_t size, void *context, std::string speaker, std::string phones, std::string text, std::string filetype, std::vector<std::tuple<std::string, int, int>> sox, std::string lipsync, bool islast, void *utt);

typedef struct snd_part {
  size_t offset;
  size_t length;
  size_t startms;
  size_t timems;
  int padms;
  int breakms;
  int phonecount;
  std::vector<std::string> soxlist;

  snd_part(size_t off, size_t len, size_t start, size_t time, int phcnt, std::vector<std::string> &sox)
  {
    offset = off;
    length = len;
    startms = start;
    timems = time;
    breakms = 0;
    phonecount = phcnt;
    soxlist = sox;
  }
} snd_part;

typedef struct snd_file {
	void *buffer;
  size_t offset;
	size_t size;
  size_t timems;
  std::vector<snd_part> parts;
} snd_file;

void dumpSndFile(const snd_file& sndFile);

void* process_sox_decode_wav(const void *data, size_t size, const char* sourcefiletype, size_t *outsize);
snd_file process_sox_chain_list_type(std::vector<std::tuple<std::string, int, int>> &soxlist, const void *data, size_t size, const char* filetype, const char* sourcefiletype);
snd_file process_sox_chain_list(std::vector<std::tuple<std::string, int, int>> &soxlist, const void *data, size_t size, const char* filetype);
//snd_file process_sox_chain(std::string sox, const void *data, size_t size, const char* filetype);

/**
 * 将sox效果作用于音频流上，比如变速
 *
 * @param soxList 实际上std::vector没有作用，相当于size位1，可以通过"tempo=0.94" 104960 65 进行构建
 *                tempo=0.94表示sox音效，104960表示文件大小，65表示phone个数，phone这里没有用到
 * @param data 一串裸数据，比如数据格式为s16le, 声道数量为1, 采样率为16000
 *             可以通过ffplay -f s16le -ac 1 -ar 16000 data进行播放
 *             后续对音频的所有操作都暗示数据格式必须为s16le，声道数为1，采样率为16000
 * @param size 裸数据的大小，byte为单位，和soxList里面size相同
 * @param filetype 想要得到的文件类型，有"wav", "mp3"两种类型
 * @return 返回snd_file结构体，buffer存储目标文件的内存；offset表示目前文件相对于buffer起始位置的偏移；
 *          size表示buffer的总大小，文件的大小为(size - offset)bytes；timems表示文件的时长，ms为单位
 *          经过变速后的时长和最初文件的时长不相等；parts目前为空，没有用到
 *          重点：buffer数据使用者负责清理，返回的数据格式mp3为fltp，wav为flt
 */
snd_file process_sox_effect_chain(std::vector<std::tuple<std::string, int, int>>& soxList,
                                  const void* data,
                                  size_t size,
                                  const char* filetype);
}
#endif
