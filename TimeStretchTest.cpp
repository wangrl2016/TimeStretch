//
// Created by wangrl on 2022/6/7.
//

#include <cassert>
#include <fstream>
#include <vector>
#include <tuple>
#include "glog/logging.h"
#include "sox.h"
#include "server_base/audio_utils.h"

using namespace WL::Service::Base;

int main(int argc, char* argv[]) {
    // Initialize Google’s logging library.
    google::InitGoogleLogging(argv[0]);
    fLI::FLAGS_stderrthreshold = google::INFO;

    assert(sox_init() == SOX_SUCCESS);

    std::ifstream in;
    in.open("/home/wangrl/github/TimeStretch/data", std::ios::binary);

    std::vector<char> dataVec;

//    if (in.is_open()) {
//        LOG(INFO) << "Read data success";
//    }

    char c;
    while (in.read(&c, sizeof(c))) {
        dataVec.push_back(c);
    }

//    LOG(INFO) << "Data size " << dataVec.size();

    int dataSize = 104960;
    // byte为单位
    assert(dataVec.size() == dataSize);

    char* dataArr = (char*) calloc(104960, sizeof(char));

    for (int i = 0; i < dataVec.size(); i++) {
        dataArr[i] = dataVec[i];
    }

    // 对音频流进行处理
    std::vector<std::tuple<std::string, int, int>> soxList;
    // 65表示phone count的个数，该函数中只是赋值
    std::tuple<std::string, int, int> temp = {"tempo=0.5", dataSize, 65};
    soxList.push_back(temp);

//    snd_file sndFile = process_sox_chain_list(soxList,
//                                              dataArr,
//                                              dataSize,
//                                              "mp3");
//
//    dumpSndFile(sndFile);

    snd_file sndFileModify = process_sox_effect_chain(soxList,
                                                      dataArr,
                                                      dataSize,
                                                      "mp3");

    dumpSndFile(sndFileModify);

    // 将内存写入到文件中
    std::ofstream outStream("sndFile.mp3",
                            std::ios::out | std::ios::binary);

    outStream.write((char*) sndFileModify.buffer, (long) sndFileModify.size);

//    std::ofstream outStream("dataVec.out",
//                            std::ios::out | std::ios::binary);
//
//    for (const char& ch : dataVec) {
//        outStream.write(&ch, sizeof ch);
//    }



    sox_quit();

    return 0;
}
