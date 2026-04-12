#include "../include/data_reader.hpp"
#include <iostream>
#include <string>

int main(int argc, char** argv) {
    using namespace hft;
    SampleDataGenerator::Params p;

    std::string out = "data/sample_ticks.csv";
    if (argc > 1) out = argv[1];
    if (argc > 2) p.n_ticks = std::stoi(argv[2]);

    SampleDataGenerator::generate_csv(out, p);
    std::cout << "Done.\n";
    return 0;
}
