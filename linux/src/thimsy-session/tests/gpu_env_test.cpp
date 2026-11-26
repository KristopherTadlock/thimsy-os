// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Thimsy OS contributors
#include <string>
#include <vector>

#include "core/GpuEnv.h"
#include "gtest/gtest.h"

using namespace thimsy::session;

TEST(GpuEnv, SwrastPack) {
    // As thimsy-gpu-pack's gpu_pack.py writes it.
    std::vector<std::string> env;
    std::string err;
    ASSERT_TRUE(gpuEnvFromJson("{\n \"driver\": \"swrast\",\n \"env\": {\n  \"GALLIUM_DRIVER\": \"softpipe\"\n },\n"
                               " \"files\": [\"usr/lib/libEGL.so.1\"]\n}\n",
                               &env, &err))
        << err;
    EXPECT_EQ(env, (std::vector<std::string>{"GALLIUM_DRIVER=softpipe"}));
}

TEST(GpuEnv, PanfrostAndNoFile) {
    std::vector<std::string> env = {"stale=1"};
    ASSERT_TRUE(gpuEnvFromJson("{\"driver\": \"panfrost\", \"env\": {}, \"files\": []}", &env));
    EXPECT_TRUE(env.empty());
    env = {"stale=1"};
    ASSERT_TRUE(gpuEnvFromJson("", &env));   // no gpu.json: nothing to add
    EXPECT_TRUE(env.empty());
    ASSERT_TRUE(gpuEnvFromJson("{\"driver\": \"panfrost\"}", &env));
    EXPECT_TRUE(env.empty());
}

TEST(GpuEnv, KeepsOrderAndSkipsBadEntries) {
    std::vector<std::string> env, skipped;
    ASSERT_TRUE(gpuEnvFromJson("{\"env\": {\"B\": \"2\", \"A_1\": \"x=y\", \"1BAD\": \"v\", \"N\": 3, "
                               "\"has space\": \"v\", \"\": \"v\", \"EMPTY\": \"\"}}",
                               &env, nullptr, &skipped));
    EXPECT_EQ(env, (std::vector<std::string>{"B=2", "A_1=x=y", "EMPTY="}));
    EXPECT_EQ(skipped, (std::vector<std::string>{"1BAD", "N", "has space", ""}));
}

TEST(GpuEnv, RefusesBadDocuments) {
    std::vector<std::string> env;
    std::string err;
    EXPECT_FALSE(gpuEnvFromJson("{not json", &env, &err));
    EXPECT_NE(err.find("gpu.json"), std::string::npos);
    EXPECT_FALSE(gpuEnvFromJson("[1, 2]", &env, &err));
    EXPECT_FALSE(gpuEnvFromJson("{\"env\": [\"GALLIUM_DRIVER=softpipe\"]}", &env, &err));
    EXPECT_NE(err.find("env"), std::string::npos);
}
