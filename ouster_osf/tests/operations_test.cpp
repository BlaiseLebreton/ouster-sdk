/**
 * Copyright(c) 2021, Ouster, Inc.
 * All rights reserved.
 */

#include "ouster/osf/operations.h"

#include <gtest/gtest.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <stdio.h>

#include <algorithm>
#include <fstream>
#include <jsoncons/json.hpp>
#include <jsoncons/json_type.hpp>
#include <jsoncons_ext/jsonpath/json_query.hpp>
#include <set>
#include <sstream>

#include "fb_utils.h"
#include "osf_test.h"
#include "ouster/osf/basics.h"
#include "ouster/osf/crc32.h"
#include "ouster/osf/file.h"
#include "ouster/osf/meta_lidar_sensor.h"
#include "ouster/osf/reader.h"
#include "ouster/osf/stream_lidar_scan.h"

namespace ouster {
namespace osf {
namespace {

// For some reason windows doesnt like the block_size
// init in the code below
#define BLOCK_SIZE (1024 * 1024)
#define FILESHA_DIGEST_SIZE 64
class FileSha {
   public:
    FileSha(const std::string& filename)
        : context(EVP_MD_CTX_new()),
          block_size(BLOCK_SIZE),
          fsize(ouster::osf::file_size(filename)),
          digest{0},
          digest_size(FILESHA_DIGEST_SIZE) {
        // Using NULL for the openssl C api
        if (context == NULL) handleEvpError();
        if (EVP_DigestInit_ex(context, EVP_sha512(), NULL) != 1) {
            handleEvpError();
        }

        std::vector<char> buf;
        buf.resize(BLOCK_SIZE);
        uint64_t i = ouster::osf::file_size(filename);
        bool finished = false;

        std::fstream reader;
        reader.open(filename, std::fstream::in | std::fstream::binary);

        while (i > 0 && !finished) {
            uint64_t size = block_size;
            if (i < block_size) {
                size = i;
                finished = true;
            }
            reader.read(buf.data(), size);
            if (EVP_DigestUpdate(context, buf.data(), size) != 1) {
                handleEvpError();
            }
            i -= block_size;
        }
        if (EVP_DigestFinal_ex(context, digest, &digest_size) != 1) {
            handleEvpError();
        }
        EVP_MD_CTX_free(context);
    }

    std::string get_string() {
        std::stringstream result;
        char buf[3];
        result << "0x";
        for (uint64_t i = 0; i < digest_size; i++) {
            // std::hex was misbehaving, just use C
            snprintf(buf, 3, "%02x", digest[i]);
            result << std::string(buf);
        }
        return result.str();
    }

   protected:
    void handleEvpError() {
        const size_t buflen = 100;
        char buf[buflen];
        unsigned long errorno;
        std::stringstream sstream;

        sstream << "FileSha Sha Errors:" << std::endl;
        while ((errorno = ERR_get_error()) != 0) {
            ERR_error_string_n(errorno, buf, buflen);
            sstream << buf << std::endl;
        }
        throw sstream.str();
    }

    EVP_MD_CTX* context;
    const uint64_t block_size;
    int64_t fsize;
    unsigned char digest[FILESHA_DIGEST_SIZE];
    unsigned int digest_size;
};

class OperationsTest : public OsfTestWithDataAndFiles {};

TEST_F(OperationsTest, GetOsfDumpInfo) {
    std::string osf_info_str = dump_metadata(
        path_concat(test_data_dir(), "osfs/OS-1-128_v2.3.0_1024x10_lb_n3.osf"),
        true);

    jsoncons::json osf_info_obj;

    bool failure = false;
    try {
        osf_info_obj = jsoncons::json::parse(osf_info_str);
    } catch (const jsoncons::ser_error&) {
        failure = true;
    }
    EXPECT_FALSE(failure);

    ASSERT_TRUE(osf_info_obj.contains("header"));
    EXPECT_TRUE(osf_info_obj["header"].contains("status"));
    EXPECT_TRUE(osf_info_obj["header"].contains("version"));
    EXPECT_TRUE(osf_info_obj["header"].contains("size"));
    EXPECT_TRUE(osf_info_obj["header"].contains("metadata_offset"));
    EXPECT_TRUE(osf_info_obj["header"].contains("chunks_offset"));

    ASSERT_TRUE(osf_info_obj.contains("metadata"));
    EXPECT_TRUE(osf_info_obj["metadata"].contains("id"));
    EXPECT_EQ("ouster_sdk", osf_info_obj["metadata"]["id"].as<std::string>());
    EXPECT_TRUE(osf_info_obj["metadata"].contains("start_ts"));
    EXPECT_TRUE(osf_info_obj["metadata"].contains("end_ts"));
    EXPECT_TRUE(osf_info_obj["metadata"].contains("entries"));
    EXPECT_EQ(3, osf_info_obj["metadata"]["entries"].size());
}

TEST_F(OperationsTest, ParseAndPrintSmoke) {
    parse_and_print(
        path_concat(test_data_dir(), "osfs/OS-1-128_v2.3.0_1024x10_lb_n3.osf"));
}

TEST_F(OperationsTest, FileShaTest) {
    std::fstream test_file_out;
    std::string temp_dir;
    EXPECT_TRUE(make_tmp_dir(temp_dir));
    std::string temp_file = path_concat(temp_dir, "test_file");
    test_file_out.open(temp_file, std::fstream::out | std::fstream::trunc |
                                      std::fstream::binary);
    test_file_out << "Testing here for hashing\n";
    test_file_out.close();
    auto sha = FileSha(temp_file);
    EXPECT_EQ(
        sha.get_string(),
        "0x568c47f13b8a96ab5027037c0a44450fd493e91ba92a95bd1f81e23604d8dd99e687"
        "6d5bbdf3d5b05ec7b9d03e84fd678690e57a1ecbc40863637deab9a35253");

    unlink_path(temp_file);
    remove_dir(temp_dir);
}

TEST_F(OperationsTest, BackupMetadataTest) {
    std::string osf_file_path =
        path_concat(test_data_dir(), "osfs/OS-1-128_v2.3.0_1024x10_lb_n3.osf");
    std::string temp_dir;
    EXPECT_TRUE(make_tmp_dir(temp_dir));
    try {
        std::string temp_file = path_concat(temp_dir, "temp.osf");
        EXPECT_EQ(append_binary_file(temp_file, osf_file_path),
                  file_size(osf_file_path));
        auto size1 = file_size(temp_file);
        auto sha1 = FileSha(temp_file).get_string();
        std::string temp_backup = path_concat(temp_dir, "temp_backup");
        auto size2 = backup_osf_file_metablob(temp_file, temp_backup);
        truncate_file(temp_file, size1 - size2);
        auto sha2 = FileSha(temp_file).get_string();
        std::fstream bad_append_out;
        bad_append_out.open(temp_file, std::fstream::out | std::fstream::app);
        bad_append_out << "Testing here for hashing" << std::endl;
        bad_append_out.close();
        auto sha3 = FileSha(temp_file).get_string();
        auto size3 = restore_osf_file_metablob(temp_file, temp_backup);
        auto sha4 = FileSha(temp_file).get_string();

        EXPECT_NE(size1, size2);
        EXPECT_EQ(size1, size3);
        EXPECT_EQ(sha1, sha4);
        EXPECT_NE(sha1, sha2);
        EXPECT_NE(sha1, sha3);
        EXPECT_NE(sha2, sha3);
        unlink_path(temp_file);
        unlink_path(temp_backup);
    } catch (...) {
        remove_dir(temp_dir);
        throw;
    }
    remove_dir(temp_dir);
}

bool _parse_json(const std::string& json, jsoncons::json& root) {
    bool failure = false;
    try {
        root = jsoncons::json::parse(json);
    } catch (const jsoncons::ser_error&) {
        failure = true;
    }
    return !failure;
}

ouster::sensor::sensor_info _gen_new_metadata(int start_number) {
    ouster::sensor::sensor_info new_metadata;
    new_metadata.sn = 123456;
    new_metadata.fw_rev = "sqrt(-1) friends";
    new_metadata.config.lidar_mode = ouster::sensor::MODE_512x10;
    new_metadata.prod_line = "OS-1-128";

    new_metadata.format.pixels_per_column = 128;
    new_metadata.format.columns_per_packet = 2 + start_number;
    new_metadata.format.columns_per_frame = 3 + start_number;
    new_metadata.format.column_window = {9 + start_number, 10 + start_number};
    new_metadata.format.udp_profile_lidar =
        ouster::sensor::PROFILE_RNG15_RFL8_NIR8;
    new_metadata.format.udp_profile_imu = ouster::sensor::PROFILE_IMU_LEGACY;
    new_metadata.format.fps = 11 + start_number;
    new_metadata.beam_azimuth_angles.resize(
        new_metadata.format.pixels_per_column);
    new_metadata.beam_altitude_angles.resize(
        new_metadata.format.pixels_per_column);
    for (size_t i = 0; i < new_metadata.format.pixels_per_column; i++) {
        new_metadata.beam_azimuth_angles[i] = (double)i;
        new_metadata.beam_altitude_angles[i] = (double)i;
    }
    new_metadata.lidar_origin_to_beam_origin_mm = 22 + start_number;

    new_metadata.init_id = 23 + start_number;
    new_metadata.config.udp_port_lidar = 24 + start_number;
    new_metadata.config.udp_port_imu = 25 + start_number;

    new_metadata.build_date = "2023-02-03T21:45:40Z";
    new_metadata.image_rev = "IDK, ask someone else";
    new_metadata.prod_pn = "import random; print(random.random())";
    new_metadata.status = "Not just good but great";

    for (size_t i = 0; i < 4; i++) {
        for (size_t j = 0; j < 4; j++) {
            double value = 0.0;
            if (i == j) {
                value = 1.0;
            }
            new_metadata.extrinsic(i, j) = value;
        }
    }

    for (size_t i = 0; i < new_metadata.format.pixels_per_column; i++) {
        new_metadata.format.pixel_shift_by_row.push_back(i + start_number);
    }
    return new_metadata;
}

void _verify_empty_metadata(jsoncons::json& test_root, int entry_count = 0) {
    EXPECT_EQ(test_root["metadata"]["chunks"].size(), 0);
    EXPECT_EQ(test_root["metadata"]["entries"].size(), entry_count);
    EXPECT_EQ(test_root["metadata"]["end_ts"], 0);
    EXPECT_EQ(test_root["metadata"]["start_ts"], 0);
    EXPECT_EQ(test_root["metadata"]["id"], "");
}

void _write_init_metadata(std::string& temp_file, uint64_t header_size,
                          MetadataStore meta_store_ = {}) {
    // Copied and modified from writer.cpp under osf/src
    flatbuffers::FlatBufferBuilder metadata_fbb =
        flatbuffers::FlatBufferBuilder(32768);

    std::vector<ouster::osf::gen::ChunkOffset> chunks_{};

    std::vector<flatbuffers::Offset<ouster::osf::gen::MetadataEntry>> entries =
        meta_store_.make_entries(metadata_fbb);
    char id[4] = {0};
    auto metadata = ouster::osf::gen::CreateMetadataDirect(
        metadata_fbb, id, 0, 0, &chunks_, &entries);

    metadata_fbb.FinishSizePrefixed(metadata,
                                    ouster::osf::gen::MetadataIdentifier());

    const uint8_t* buf = metadata_fbb.GetBufferPointer();
    uint32_t metadata_size = metadata_fbb.GetSize();

    uint64_t metadata_offset = header_size;
    uint64_t metadata_saved_size =
        buffer_to_file(buf, metadata_size, temp_file, true);
    EXPECT_TRUE(metadata_saved_size &&
                metadata_saved_size == metadata_size + CRC_BYTES_SIZE);
    EXPECT_TRUE(finish_osf_file(temp_file, metadata_offset,
                                metadata_saved_size) == header_size);
}

TEST_F(OperationsTest, MetadataRewriteTestSimple) {
    std::string temp_dir;
    EXPECT_TRUE(make_tmp_dir(temp_dir));
    std::string temp_file = path_concat(temp_dir, "temp.osf");
    uint64_t header_size = start_osf_file(temp_file);

    _write_init_metadata(temp_file, header_size);

    std::string metadata_json = dump_metadata(temp_file, true);
    jsoncons::json test_root{};
    EXPECT_TRUE(_parse_json(metadata_json, test_root));

    _verify_empty_metadata(test_root);

    ouster::sensor::sensor_info new_metadata = _gen_new_metadata(100);

    osf_file_modify_metadata(temp_file, {new_metadata});
    std::string output_metadata_json = dump_metadata(temp_file, true);
    jsoncons::json output_root{};
    EXPECT_TRUE(_parse_json(output_metadata_json, output_root));
    EXPECT_NE(test_root, output_root);

    jsoncons::json new_root{};
    EXPECT_TRUE(_parse_json(new_metadata.to_json_string(), new_root));
    EXPECT_EQ(new_root,
              output_root["metadata"]["entries"][0]["buffer"]["sensor_info"]);
    unlink_path(temp_file);
}

TEST_F(OperationsTest, MetadataRewriteTestMulti) {
    std::string temp_dir;
    EXPECT_TRUE(make_tmp_dir(temp_dir));
    std::string temp_file = path_concat(temp_dir, "temp.osf");
    uint64_t header_size = start_osf_file(temp_file);

    _write_init_metadata(temp_file, header_size);

    std::string metadata_json = dump_metadata(temp_file, true);
    jsoncons::json test_root{};
    EXPECT_TRUE(_parse_json(metadata_json, test_root));

    _verify_empty_metadata(test_root);

    ouster::sensor::sensor_info new_metadata = _gen_new_metadata(100);
    ouster::sensor::sensor_info new_metadata2 = _gen_new_metadata(200);

    osf_file_modify_metadata(temp_file, {new_metadata, new_metadata2});
    std::string output_metadata_json = dump_metadata(temp_file, true);
    jsoncons::json output_root{};
    EXPECT_TRUE(_parse_json(output_metadata_json, output_root));
    EXPECT_NE(test_root, output_root);

    jsoncons::json new_root{};
    EXPECT_TRUE(_parse_json(new_metadata.to_json_string(), new_root));
    jsoncons::json new_root2{};
    auto temp_string = new_metadata2.to_json_string();
    EXPECT_TRUE(_parse_json(temp_string, new_root2));

    EXPECT_EQ(new_root,
              output_root["metadata"]["entries"][0]["buffer"]["sensor_info"]);
    EXPECT_EQ(new_root2,
              output_root["metadata"]["entries"][1]["buffer"]["sensor_info"]);
    unlink_path(temp_file);
}

TEST_F(OperationsTest, MetadataRewriteTestPreExisting) {
    std::string temp_dir;
    EXPECT_TRUE(make_tmp_dir(temp_dir));
    std::string temp_file = path_concat(temp_dir, "temp.osf");
    uint64_t header_size = start_osf_file(temp_file);
    MetadataStore meta_store_ = {};
    LidarScanStreamMeta pre_existing_data(12345678, {});
    meta_store_.add(pre_existing_data);
    _write_init_metadata(temp_file, header_size, meta_store_);

    std::string metadata_json = dump_metadata(temp_file, true);
    jsoncons::json test_root{};
    EXPECT_TRUE(_parse_json(metadata_json, test_root));

    _verify_empty_metadata(test_root, 1);

    EXPECT_EQ(test_root["metadata"]["entries"][0]["type"],
              "ouster/v1/os_sensor/LidarScanStream");
    EXPECT_EQ(test_root["metadata"]["entries"][0]["buffer"],
              "LidarScanStreamMeta: sensor_id = 12345678, field_types = {}");

    ouster::sensor::sensor_info new_metadata = _gen_new_metadata(100);

    osf_file_modify_metadata(temp_file, {new_metadata});
    std::string output_metadata_json = dump_metadata(temp_file, true);
    jsoncons::json output_root{};
    EXPECT_TRUE(_parse_json(output_metadata_json, output_root));
    EXPECT_NE(test_root, output_root);

    jsoncons::json new_root{};
    EXPECT_TRUE(_parse_json(new_metadata.to_json_string(), new_root));

    EXPECT_EQ(output_root["metadata"]["entries"][0]["buffer"],
              "LidarScanStreamMeta: sensor_id = 12345678, field_types = {}");
    EXPECT_EQ(new_root,
              output_root["metadata"]["entries"][1]["buffer"]["sensor_info"]);
    EXPECT_EQ(output_root["metadata"]["entries"].size(), 2);
    unlink_path(temp_file);
}

}  // namespace
}  // namespace osf
}  // namespace ouster
