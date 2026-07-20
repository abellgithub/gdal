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

    std::thread t(
        [url, size]
        {
            std::vector<char> buf(size);

            std::cerr << "Enter!\n";
            VSILFILE *file = VSIFOpenL(url.c_str(), "rb");
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
        });

    std::cerr << "Thread running!\n";
    t.join();
    std::cerr << "Thread joined!\n";
}

}  // namespace
