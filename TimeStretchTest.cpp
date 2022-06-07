//
// Created by wangrl on 2022/6/7.
//

#include <cassert>
#include "glog/logging.h"
#include "sox.h"

int main(int argc, char* argv[]) {
    // Initialize Google’s logging library.
    google::InitGoogleLogging(argv[0]);

    assert(sox_init() == SOX_SUCCESS);

    sox_quit();

    return 0;
}
