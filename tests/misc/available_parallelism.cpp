// Tests use synthetic files, without mounting or writing any cgroup controller.
#include <cassert>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include "runtime/available_parallelism.cpp"

static void put(std::string const & p, std::string const & s) {
    std::ofstream(p) << s;
}

int main() {
    using lean::cgroup_parallelism;
    char temp[] = "/tmp/lean-cpu-test-XXXXXX";
    std::string dir = mkdtemp(temp);
    mkdir((dir + "/child").c_str(), 0700);
    mkdir((dir + "/child/leaf").c_str(), 0700);
    auto check = [&](std::string const & cg, std::string const & mount, unsigned want) {
        std::istringstream cgroups(cg), mounts(mount);
        unsigned got = cgroup_parallelism(cgroups, mounts, 16);
        assert(got == want);
    };
    std::string v2 = "1 0 0:1 / " + dir + " rw - cgroup2 cgroup rw\n";
    put(dir + "/cpu.max", "max 100000\n");
    put(dir + "/child/cpu.max", "150000 100000\n");
    put(dir + "/child/leaf/cpu.max", "800000 100000\n");
    check("0::/child/leaf\n", v2, 2); // ancestor wins; fractional quotas round up
    put(dir + "/child/cpu.max", "50000 100000\n");
    check("0::/child/leaf\n", v2, 1);
    put(dir + "/child/cpu.max", "max 100000\n");
    check("0::/child/leaf\n", v2, 8);
    put(dir + "/child/leaf/cpu.max", "max 100000\n");
    check("0::/child/leaf\n", v2, 16);
    put(dir + "/child/leaf/cpu.max", "garbage\n");
    check("0::/child/leaf\n", v2, 16);
    put(dir + "/child/leaf/cpu.max", "100000 0\n");
    check("0::/child/leaf\n", v2, 16);
    put(dir + "/cpu.max", "250000 100000\n");
    check("0::/\n", "1 0 0:1 /host/pod " + dir + " rw - cgroup2 cgroup rw\n", 3);
    check("0::/host/pod\n", "1 0 0:1 /host/pod " + dir + " rw - cgroup2 cgroup rw\n", 3);
    check("0::/different\n", "1 0 0:1 /host/pod " + dir + " rw - cgroup2 cgroup rw\n", 16);
    check("0::/child/../escape\n", v2, 16);
    std::string v1 = "1 0 0:1 / " + dir + " rw - cgroup cgroup rw,cpuacct,cpu\n";
    put(dir + "/cpu.cfs_quota_us", "-1\n");
    put(dir + "/cpu.cfs_period_us", "100000\n");
    put(dir + "/child/cpu.cfs_quota_us", "250000\n");
    put(dir + "/child/cpu.cfs_period_us", "100000\n");
    put(dir + "/child/leaf/cpu.cfs_quota_us", "900000\n");
    put(dir + "/child/leaf/cpu.cfs_period_us", "100000\n");
    check("5:cpu,cpuacct:/child/leaf\n", v1, 3);
    check("5:cpuacct:/child/leaf\n", v1, 16); // controller name must match exactly
    check("5:cpu,cpuacct:/child/leaf\n", "1 0 0:1 / " + dir + " rw - cgroup cgroup rw,cpuset\n", 16);
    put(dir + "/child/cpu.cfs_quota_us", "9223372036854775807\n");
    put(dir + "/child/cpu.cfs_period_us", "1\n");
    check("5:cpu:/child\n", v1, 16); // no unsigned overflow
    check("malformed\n", "malformed\n", 16);
    assert(lean::unescape_mount_path("/a\\040b\\134c") == "/a b\\c");
    std::cout << "synthetic cgroup cases passed; live available=" << lean::available_parallelism() << '\n';
    for (auto suffix : {"/cpu.max", "/cpu.cfs_quota_us", "/cpu.cfs_period_us"}) {
        for (auto sub : {"", "/child", "/child/leaf"}) unlink((dir + sub + suffix).c_str());
    }
    rmdir((dir + "/child/leaf").c_str());
    rmdir((dir + "/child").c_str());
    rmdir(dir.c_str());
}
