// Copyright 2026 Cara contributors
// Licensed under GPLv2 or any later version.
#include <iostream>
#include <set>
#include <boost/icl/interval_map.hpp>
#include <boost/icl/interval_set.hpp>
#include <boost/icl/right_open_interval.hpp>

using Interval = boost::icl::right_open_interval<unsigned>;
using Cache = boost::icl::interval_map<unsigned, std::set<unsigned>,
    boost::icl::partial_absorber, std::less, boost::icl::inplace_plus,
    boost::icl::inter_section, Interval>;
using Regions = boost::icl::interval_set<unsigned, std::less, Interval>;

int main() {
    // A framebuffer can expand across existing surfaces and an uncached gap.
    // With the old ICL/std::map lookup, libc++ 22 drops part of this registration.
    Cache cache;
    cache.add({Interval(0, 100), {1}});
    cache.add({Interval(112, 134), {2}});
    cache.add({Interval(0, 150), {3}});
    for (unsigned address = 0; address < 150; ++address) {
        const auto values = cache(address);
        if (!values.count(3) || values.count(1) != (address < 100) ||
            values.count(2) != (address >= 112 && address < 134)) {
            std::cerr << "Expanded surface lost coverage at " << address << '\n';
            return 1;
        }
    }
    auto overlaps = cache.equal_range(Interval(0, 150));
    unsigned covered = 0;
    for (auto it = overlaps.first; it != overlaps.second; ++it)
        covered += it->first.upper() - it->first.lower();
    if (covered != 150) return 2;
    cache.subtract({Interval(0, 150), {3}});
    for (unsigned address = 0; address < 150; ++address) {
        const auto values = cache(address);
        if (values.count(3) || values.count(1) != (address < 100) ||
            values.count(2) != (address >= 112 && address < 134)) return 3;
    }
    Regions invalid;
    invalid.add(Interval(0, 100));
    invalid.add(Interval(112, 134));
    invalid.add(Interval(0, 150));
    if (!boost::icl::contains(invalid, Interval(0, 150))) return 4;
    invalid.subtract(Interval(50, 120));
    if (boost::icl::contains(invalid, 75) || !boost::icl::contains(invalid, 49) ||
        !boost::icl::contains(invalid, 120)) return 5;
    std::cout << "Interval cache regression passed: expansion, overlap lookup, removal, invalid regions\n";
}
