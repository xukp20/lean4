/*
Copyright (c) 2026 Xukangping. All rights reserved.
Released under Apache 2.0 license as described in the file LICENSE.
*/
#include "runtime/available_parallelism.h"
#include <algorithm>
#include <cstdint>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#if defined(__linux__) && defined(LEAN_MULTI_THREAD)
#include <cerrno>
#include <sched.h>
#endif

namespace lean {
#if defined(__linux__) && defined(LEAN_MULTI_THREAD)
static bool has_controller(std::string const & controllers, std::string const & name) {
    return ("," + controllers + ",").find("," + name + ",") != std::string::npos;
}

static std::string unescape_mount_path(std::string const & path) {
    std::string result;
    for (size_t i = 0; i < path.size(); i++) {
        if (path[i] == '\\' && i + 3 < path.size() &&
            path[i+1] >= '0' && path[i+1] <= '7' &&
            path[i+2] >= '0' && path[i+2] <= '7' &&
            path[i+3] >= '0' && path[i+3] <= '7') {
            result += static_cast<char>((path[i+1]-'0')*64 + (path[i+2]-'0')*8 + path[i+3]-'0');
            i += 3;
        } else {
            result += path[i];
        }
    }
    return result;
}

static unsigned quota_parallelism(std::string const & dir, bool v2, unsigned limit) {
    int64_t quota, period;
    if (v2) {
        std::ifstream input(dir + "/cpu.max");
        if (!(input >> quota >> period)) return limit; // includes `max`
    } else {
        std::ifstream quota_file(dir + "/cpu.cfs_quota_us");
        std::ifstream period_file(dir + "/cpu.cfs_period_us");
        if (!(quota_file >> quota) || !(period_file >> period)) return limit;
    }
    if (quota <= 0 || period <= 0) return limit;
    // Round up without overflowing or turning a fractional quota into a zero-worker pool.
    uint64_t workers = quota / period + (quota % period != 0);
    return static_cast<unsigned>(std::min<uint64_t>(limit, workers));
}

static unsigned cgroup_parallelism(std::istream & groups, std::istream & mounts, unsigned limit) {
    std::string v1_path, v2_path, line;
    while (std::getline(groups, line)) {
        auto first = line.find(':');
        auto second = line.find(':', first == std::string::npos ? line.size() : first + 1);
        if (second == std::string::npos) continue;
        std::string controllers = line.substr(first + 1, second - first - 1);
        if (has_controller(controllers, "cpu")) v1_path = line.substr(second + 1);
        if (line.substr(0, first) == "0" && controllers.empty()) v2_path = line.substr(second + 1);
    }
    while (std::getline(mounts, line)) {
        auto separator = line.find(" - ");
        if (separator == std::string::npos) continue;
        std::istringstream before(line.substr(0, separator)), after(line.substr(separator + 3));
        std::string id, parent, device, root, mount, fs, source, options;
        if (!(before >> id >> parent >> device >> root >> mount) || !(after >> fs >> source >> options)) continue;
        bool v2 = fs == "cgroup2";
        if (!v2 && !(fs == "cgroup" && has_controller(options, "cpu"))) continue;
        std::string path = v2 ? v2_path : v1_path;
        root = unescape_mount_path(root);
        mount = unescape_mount_path(mount);
        if (path.empty() || path[0] != '/' || mount.empty() || mount[0] != '/') continue;
        if (("/" + path + "/").find("/../") != std::string::npos) continue;
        std::string relative;
        if (root == "/") {
            relative = path;
        } else if (path == root || path == "/") {
            relative = "";
        } else if (path.compare(0, root.size() + 1, root + "/") == 0) {
            relative = path.substr(root.size());
        } else {
            continue;
        }
        if (mount.size() > 1 && mount.back() == '/') mount.pop_back();
        std::string dir = mount == "/" ? relative : mount + relative;
        if (dir.empty()) dir = "/";
        if (dir.size() > 1 && dir.back() == '/') dir.pop_back();
        // Only inspect visible ancestors within this mount; do not guess hidden host paths.
        for (;;) {
            limit = quota_parallelism(dir, v2, limit);
            if (dir == mount) break;
            auto slash = dir.rfind('/');
            if (slash == std::string::npos) break;
            dir = slash == 0 ? "/" : dir.substr(0, slash);
        }
    }
    return limit;
}

static unsigned affinity_parallelism() {
    // The kernel's mask can be larger than cpu_set_t, even with few online CPUs.
    size_t size = sizeof(cpu_set_t);
    while (size <= std::numeric_limits<unsigned>::max() / 2) {
        std::vector<unsigned long> mask((size + sizeof(unsigned long) - 1) / sizeof(unsigned long));
        if (sched_getaffinity(0, size, reinterpret_cast<cpu_set_t *>(mask.data())) == 0) {
            return CPU_COUNT_S(size, reinterpret_cast<cpu_set_t *>(mask.data()));
        }
        if (errno != EINVAL) break;
        size *= 2;
    }
    return 0;
}
#endif

unsigned available_parallelism() {
#if !defined(LEAN_MULTI_THREAD)
    return 1;
#else
    unsigned limit = std::max(1u, std::thread::hardware_concurrency());
#if defined(__linux__)
    if (unsigned affinity = affinity_parallelism()) limit = affinity;
    std::ifstream groups("/proc/self/cgroup"), mounts("/proc/self/mountinfo");
    limit = cgroup_parallelism(groups, mounts, limit);
#endif
    return limit;
#endif
}
}
