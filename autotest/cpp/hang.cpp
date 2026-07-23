/******************************************************************************
 *
 * Project:  GDAL Core
 * Purpose:  Test fix for https://github.com/OSGeo/gdal/issues/1488 (concurrency
 *issue with overviews) Author:   Even Rouault, <even dot rouault at spatialys
 *dot com>
 *
 ******************************************************************************
 * Copyright (c) 2019, Even Rouault <even dot rouault at spatialys dot com>
 * Copyright (c) 2019, Thomas Bonfort <thomas.bonfort at gmail.com>
 *
 * SPDX-License-Identifier: MIT
 ****************************************************************************/

#include <string>
#include <thread>
#include <vector>

#include "gdal.h"
#include "cpl_vsi_virtual.h"

#include "gtest_include.h"

namespace
{

TEST(hang, test)
{
    GDALAllRegister();

    std::string url("/vsicurl/https://github.com/PDAL/data/raw/refs/heads/main/"
                    "autzen/autzen-classified.copc.laz");
    uint32_t size = 54;

    bool done = false;

    std::mutex m;
    std::condition_variable cv;

    std::thread t(
        [url, size, &done, &m, &cv]
        {
            std::vector<char> buf(size);

            std::cerr << "Enter!\n";
            VSILFILE *file = VSIFOpenL(url.c_str(), "rb");
            if (!file)
            {
                std::cerr << "Couldn't open file - perhaps no CURL support?\n";
                return;
            }
            std::cerr << "Opened!\n";
            VSIFSeekL(file, 375, SEEK_SET);
            std::cerr << "Seeked!\n";
            VSIFReadL(buf.data(), 1, size, file);
            std::cerr << "Read!\n";
            VSIFCloseL(file);
            std::cerr << "Closed!\n";
            int sum = 0;
            for (char c : buf)
                sum += (uint8_t)c;
            std::cerr << "Sum = " << sum << "!\n";

            std::lock_guard<std::mutex> _(m);
            done = true;
            cv.notify_all();
        });

    std::unique_lock<std::mutex> lock(m);
    cv.wait_for(lock, std::chrono::milliseconds(5000),
                [&done]()
                {
                    if (done)
                        std::cerr << "Completed VSI task!\n";
                    return done;
                });
    if (!done)
    {
        std::cerr << "VSI task timeout.\n";
        std::terminate();
    }
    t.join();
}

}  // namespace
