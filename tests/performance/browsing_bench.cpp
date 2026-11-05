#include "edgeecho/core.hpp"
#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <functional>
#include <string>
#include <vector>

using namespace edgeecho;
namespace {
void measure(const std::string &name, std::size_t count, const std::function<std::size_t()> &run, bool comma) {
    std::vector<double> timings;
    std::size_t items=0;
    for(std::size_t n=0;n<count;++n) {
        const auto begin=std::chrono::steady_clock::now();
        const auto value=run();
        const auto elapsed=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count();
        if(n && value!=items) throw Error("Benchmark result changed across repetitions");
        items=value; timings.push_back(elapsed);
    }
    std::sort(timings.begin(),timings.end());
    const auto percentile=[&](double fraction) {return timings[static_cast<std::size_t>(std::ceil(fraction*static_cast<double>(timings.size())))-1];};
    if(comma) std::cout<<',';
    std::cout<<'"'<<name<<"\":{\"runs\":"<<count<<",\"result_items\":"<<items
             <<",\"p50_ms\":"<<percentile(.5)<<",\"p95_ms\":"<<percentile(.95)
             <<",\"max_ms\":"<<timings.back()<<'}'<<std::flush;
}
}
int main(int argc,char **argv) {
    try {
        if(argc<2 || argc>3) throw Error("Usage: edgeecho_browsing_bench PERFORMANCE_DB [RUNS=100]");
        std::size_t repeats=100;
        if(argc==3) {
            const std::string argument(argv[2]);
            const auto parsed=std::from_chars(argument.data(),argument.data()+argument.size(),repeats);
            if(parsed.ec!=std::errc{} || parsed.ptr!=argument.data()+argument.size())
                throw Error("Runs must be a complete positive integer");
        }
        if(repeats<1 || repeats>1000) throw Error("Runs must be in 1..1000");
        Database db(std::filesystem::u8path(argv[1]));
        constexpr Time t=1793145600000000LL;
        const Filter raw{"performance-vibration","vibration_iio_block","count"};
        const Filter scalar{"demo-vibration","acceleration_rms","count"};
        std::cout<<std::setprecision(12)<<"{\"version\":\""<<version()<<"\",\"build_id\":\""<<build_id()
                 <<"\",\"scope\":\"One offline connection, repeated queries, warm cache uncontrolled; query latency excludes GUI paint and process startup\",\"benchmarks\":{";
        measure("waveform_8192000",repeats,[&] {
            const auto p=db.plot(raw,{t,t+250000000},"x",1500);
            if(p.samples_decoded!=8192000) throw Error("Incorrect waveform fixture");
            return p.points.size();
        },false);
        measure("scalar_200000",repeats,[&] {
            const auto p=db.plot(scalar,{t,t+1999990001},"value",1500);
            if(p.records_scanned!=200000) throw Error("Incorrect scalar fixture");
            return p.points.size();
        },true);
        measure("scalar_tail_second",repeats,[&] {return db.plot(scalar,{t+1998990001,t+1999990001},"value",1500).points.size();},true);
        measure("xyz_10_seconds",repeats,[&] {
            const auto p=db.plots(raw,{t,t+10000000},{"x","y","z"},1500);
            if(p.size()!=3 || p[0].samples_decoded!=327680) throw Error("Incorrect XYZ fixture");
            return p[0].points.size()+p[1].points.size()+p[2].points.size();
        },true);
        measure("probe_10_seconds",repeats,[&] {
            const auto p=db.probe(raw,{t,t+10000000},"x",t+5000000);
            if(!p || p->timestamp!=t+5000000) throw Error("Incorrect exact probe");
            return std::size_t{1};
        },true);
        std::cout<<"}}\n";
        return 0;
    } catch(const std::exception &error) {std::cerr<<error.what()<<'\n';return 1;}
}
