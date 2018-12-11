//
// Bareflank Hypervisor
// Copyright (C) 2015 Assured Information Security, Inc.
//
// This library is free software; you can redistribute it and/or
// modify it under the terms of the GNU Lesser General Public
// License as published by the Free Software Foundation; either
// version 2.1 of the License, or (at your option) any later version.
//
// This library is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
// Lesser General Public License for more details.
//
// You should have received a copy of the GNU Lesser General Public
// License along with this library; if not, write to the Free Software
// Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA

#include <bfgsl.h>
#include <bfdebug.h>
#include <bfstring.h>
#include <bfaffinity.h>
#include <bfbuilderinterface.h>

#include <list>
#include <memory>
#include <chrono>
#include <thread>
#include <fstream>
#include <iostream>

#include <args.h>
#include <cmdl.h>
#include <file.h>
#include <ioctl.h>
#include <verbose.h>

vcpuid_t g_vcpuid;
domainid_t g_domainid;

auto ctl = std::make_unique<ioctl>();

void
vcpu_thread(vcpuid_t vcpuid)
{
    using namespace std::chrono;

    while(true) {
        auto ret = __run_op(vcpuid, 0, 0);
        switch(run_op_ret(ret)) {
            case __enum_run_op__hlt:
                return;

            case __enum_run_op__fault:
                std::cerr << "[0x" << std::hex << vcpuid << std::dec << "] ";
                std::cerr << "vcpu fault: " << run_op_arg(ret) << '\n';
                return;

            case __enum_run_op__resume_after_interrupt:
                continue;

            case __enum_run_op__yield:
                std::this_thread::sleep_for(microseconds(run_op_arg(ret)));
                continue;

            default:
                std::cerr << "[0x" << std::hex << vcpuid << std::dec << "] ";
                std::cerr << "unknown vcpu ret: " << run_op_ret(ret) << '\n';
                return;
        }
    }
}

static int
attach_to_vm(const args_type &args)
{
    bfignored(args);

    auto ___ = gsl::finally([&]{
        if (__vcpu_op__destroy_vcpu(g_vcpuid) != SUCCESS) {
            std::cerr << "__vcpu_op__destroy_vcpu failed\n";
        }
    });

    g_vcpuid = __vcpu_op__create_vcpu(g_domainid);
    if (g_vcpuid == INVALID_VCPUID) {
        throw std::runtime_error("__vcpu_op__create_vcpu failed");
    }

    attach_to_vm_verbose();

    std::thread t(vcpu_thread, g_vcpuid);
    t.join();

    return EXIT_SUCCESS;
}

static void
create_elf_vm(const args_type &args)
{
    struct create_from_elf_args ioctl_args{};

    if (!args.count("path")) {
        throw cxxopts::OptionException("must specify --path");
    }

    bfn::cmdl cmdl;
    bfn::file file(args["path"].as<std::string>());

    uint64_t size = file.size() * 2;
    if (args.count("size")) {
        size = args["size"].as<uint64_t>();
    }

    uint64_t uart = 0;
    if (args.count("uart")) {
        uart = args["uart"].as<uint64_t>();
        cmdl.add(
            "console=uart,io," + bfn::to_string(uart, 16) + ",115200n8"
        );
    }

    if (args.count("init")) {
        cmdl.add("init=" + args["init"].as<std::string>());
    }

    if (args.count("cmdline")) {
        cmdl.add(args["cmdline"].as<std::string>());
    }

    ioctl_args.file = file.data();
    ioctl_args.file_size = file.size();
    ioctl_args.cmdl = cmdl.data();
    ioctl_args.cmdl_size = cmdl.size();
    ioctl_args.uart = uart;
    ioctl_args.size = size;

    ctl->call_ioctl_create_from_elf(ioctl_args);
    create_elf_vm_verbose();

    g_domainid = ioctl_args.domainid;
}

static int
protected_main(const args_type &args)
{
    if (args.count("elf")) {
        create_elf_vm(args);
    }
    else {
        g_domainid = args["attach"].as<domainid_t>();
    }

    auto ___ = gsl::finally([&]{
        if (args.count("elf")) {
            ctl->call_ioctl_destroy(g_domainid);
        }
    });

    return attach_to_vm(args);
}

int
main(int argc, char *argv[])
{
    set_affinity(0);

    try {
        args_type args = parse_args(argc, argv);
        return protected_main(args);
    }
    catch (const cxxopts::OptionException &e) {
        std::cerr << "invalid arguments: " << e.what() << '\n';
    }
    catch (const std::exception &e) {
        std::cerr << "Caught unhandled exception:" << '\n';
        std::cerr << "    - what(): " << e.what() << '\n';
    }
    catch (...) {
        std::cerr << "Caught unknown exception" << '\n';
    }

    return EXIT_FAILURE;
}
