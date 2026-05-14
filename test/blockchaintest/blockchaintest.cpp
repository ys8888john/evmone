// evmone: Fast Ethereum Virtual Machine implementation
// Copyright 2023 The evmone Authors.
// SPDX-License-Identifier: Apache-2.0

#include "blockchaintest_runner.hpp"
#include "dt_vm.h"
#include <CLI/CLI.hpp>
#include <evmone/evmone.h>
#include <evmone/version.h>
#include <gtest/gtest.h>
#include <test/utils/utils.hpp>
#include <filesystem>
#include <iostream>
#include <memory>

namespace fs = std::filesystem;

namespace
{
namespace vm_manager
{
evmc::VM evmone_advanced{evmc_create_evmone(), {{"advanced", ""}}};
evmc::VM evmone_baseline{evmc_create_evmone()};
evmc::VM external_vm;

std::vector<std::pair<std::string, evmc::VM*>> get_available_vms()
{
    static bool initialized = false;
    static bool external_load_state = false;
    std::vector<std::pair<std::string, evmc::VM*>> vms;
    // vms.emplace_back("evmone_advanced", &evmone_advanced);
    // vms.emplace_back("evmone_baseline", &evmone_baseline);

    // Load external lib
    const char* external_env_options = getenv("EVMONE_EXTERNAL_OPTIONS");
    if (external_env_options != nullptr)
    {
        if (!initialized)
        {
            external_load_state =
                evmone::test::try_load_external(external_env_options, external_vm);
            initialized = true;
        }
    }

    if (external_load_state)
    {
        vms.emplace_back("external_vm", &external_vm);
    }

    return vms;
}
}  // namespace vm_manager

/// Global warmup environment - runs before all tests, not counted in test timing
class WarmupEnvironment : public testing::Environment
{
    std::vector<evmone::test::BlockchainTest> m_tests;
    evmc::VM* m_vm = nullptr;
    fs::path m_test_file;
    bool m_warmup_done = false;

public:
    WarmupEnvironment(fs::path test_file, evmc::VM& vm)
      : m_test_file(std::move(test_file)), m_vm(&vm)
    {}

    void SetUp() override
    {
        // Load tests and run warmup before ANY test timing starts
        std::ifstream f{m_test_file};
        m_tests = evmone::test::load_blockchain_tests(f);

        static const int warmup_runs = []() {
            const char* env = std::getenv("DTVM_WARMUP_RUNS");
            return env ? std::atoi(env) : 0;
        }();

        if (warmup_runs > 0)
        {
            std::cout << "Running " << warmup_runs
                      << " warmup iterations (not counted in test time)..." << std::endl;
            for (int warmup_i = 0; warmup_i < warmup_runs; ++warmup_i)
            {
                evmone::test::run_blockchain_tests(m_tests, *m_vm);
            }
            std::cout << "Warmup complete. Starting timed test run." << std::endl;
        }
        m_warmup_done = true;
    }

    void TearDown() override {}
};

/// Implementation of a gtest Test which runs all blockchain tests from a given file.
class BlockchainGTestFile : public testing::Test
{
    fs::path m_json_test_file;
    evmc::VM& m_vm;
    bool m_trace = false;
    std::vector<evmone::test::BlockchainTest> m_tests;

public:
    explicit BlockchainGTestFile(fs::path json_test_file, evmc::VM& vm, bool trace) noexcept
      : m_json_test_file{std::move(json_test_file)}, m_vm{vm}, m_trace{trace}
    {}

    void TestBody() final
    {
        // Load tests if not already loaded by warmup
        if (m_tests.empty())
        {
            std::ifstream f{m_json_test_file};
            m_tests = evmone::test::load_blockchain_tests(f);
        }

        try
        {
            if (m_trace)
                m_vm.set_option("trace", "1");
            evmone::test::run_blockchain_tests(m_tests, m_vm);
        }
        catch (const evmone::test::UnsupportedTestFeature& ex)
        {
            GTEST_SKIP() << ex.what();
        }
    }

    static void register_one(
        const std::string& suite_name, const fs::path& file, evmc::VM& vm, bool trace)
    {
        // Register warmup environment if warmup is enabled
        static const int warmup_runs = []() {
            const char* env = std::getenv("DTVM_WARMUP_RUNS");
            return env ? std::atoi(env) : 0;
        }();

        if (warmup_runs > 0)
        {
            testing::AddGlobalTestEnvironment(new WarmupEnvironment(file, vm));
        }

        testing::RegisterTest(suite_name.c_str(), file.stem().string().c_str(), nullptr, nullptr,
            file.string().c_str(), 0, [file, &vm, trace]() -> testing::Test* {
                return new BlockchainGTestFile(file, vm, trace);
            });
    }
};

/// Implementation of a gtest Test which runs a single blockchain test.
class BlockchainGTest : public testing::Test
{
    const evmone::test::BlockchainTest m_blockchain_test;
    evmc::VM& m_vm;
    bool m_trace = false;

public:
    explicit BlockchainGTest(
        evmone::test::BlockchainTest blockchain_test, evmc::VM& vm, bool trace) noexcept
      : m_blockchain_test{std::move(blockchain_test)}, m_vm{vm}, m_trace{trace}
    {}

    void TestBody() final
    {
        if (m_trace)
            m_vm.set_option("trace", "1");
        evmone::test::run_blockchain_tests(std::array{m_blockchain_test}, m_vm);
    }

    static void register_one(const evmone::test::BlockchainTest& test,
        const std::string& suite_name, const std::string& test_name, const fs::path& file,
        evmc::VM& vm, bool trace)
    {
        testing::RegisterTest(suite_name.c_str(), test_name.c_str(), nullptr, nullptr,
            file.string().c_str(), 0, [test, &vm, trace]() -> testing::Test* {
                return new BlockchainGTest(test, vm, trace);
            });
    }
};

void register_test_files(const fs::path& root, const std::string& vm_name, evmc::VM& vm, bool trace)
{
    if (is_directory(root))
    {
        std::vector<fs::path> test_files;
        std::copy_if(fs::recursive_directory_iterator{root}, fs::recursive_directory_iterator{},
            std::back_inserter(test_files), [](const fs::directory_entry& entry) {
                // "index.json" files are just lists of tests generated by other tools.
                return entry.is_regular_file() && entry.path().extension() == ".json" &&
                       entry.path().filename() != "index.json";
            });
        std::ranges::sort(test_files);

        for (const auto& p : test_files)
        {
            std::string suite_name = vm_name + "/" + fs::relative(p, root).parent_path().string();
            BlockchainGTestFile::register_one(suite_name, p, vm, trace);
        }
    }
    else  // Treat as a file.
    {
        std::ifstream f{root};
        try
        {
            const auto tests = evmone::test::load_blockchain_tests(f);
            for (const auto& test : tests)
            {
                std::string suite_name = vm_name + "/" + root.string();
                std::string test_name = test.name;
                BlockchainGTest::register_one(test, suite_name, test_name, root, vm, trace);
            }
        }
        catch (const evmone::test::UnsupportedTestFeature& ex)
        {
            std::cerr << ex.what() << ": " << root.string() << '\n';
        }
    }
}
}  // namespace


int main(int argc, char* argv[])
{
    try
    {
        testing::InitGoogleTest(&argc, argv);  // Process GoogleTest flags.

        CLI::App app{"evmone blockchain test runner"};

        app.set_version_flag("--version", "evmone-blockchaintest " EVMONE_VERSION);

        std::vector<std::string> paths;
        app.add_option("path", paths,
               "Path to test file or directory. For a directory, all .json "
               "files (except index.json) are considered test files, and each file is treated as a "
               "separate test. For a file, all tests in the file are treated as separate tests.")
            ->required()
            ->check(CLI::ExistingPath);

        bool trace_flag = false;
        app.add_flag("--trace", trace_flag, "Enable EVM tracing");

        std::optional<std::string> vm_filter;
        app.add_option("--vm", vm_filter,
            "VM filter. Run tests only on VMs containing the specified string (e.g., 'external', "
            "'evmone_advanced').");

        CLI11_PARSE(app, argc, argv);

        auto available_vms = vm_manager::get_available_vms();

        if (available_vms.empty())
        {
            std::cerr << "No VMs available for testing\n";
            return -1;
        }

        if (vm_filter.has_value())
        {
            auto filtered_vms = std::vector<std::pair<std::string, evmc::VM*>>{};
            for (const auto& [name, vm] : available_vms)
            {
                if (name.find(*vm_filter) != std::string::npos)
                {
                    filtered_vms.push_back({name, vm});
                }
            }
            available_vms = std::move(filtered_vms);
        }

        if (available_vms.empty())
        {
            std::cerr << "No VMs match the filter: " << *vm_filter << "\n";
            return -1;
        }

        for (const auto& [vm_name, vm_ptr] : available_vms)
        {
            std::cout << "Registering tests for VM: " << vm_name << std::endl;

            if (trace_flag)
            {
                std::ios::sync_with_stdio(false);
                vm_ptr->set_option("trace", "1");
            }

            for (const auto& p : paths)
                register_test_files(p, vm_name, *vm_ptr, trace_flag);
        }

        return RUN_ALL_TESTS();
    }
    catch (const std::exception& ex)
    {
        std::cerr << ex.what() << "\n";
        return -1;
    }
}
