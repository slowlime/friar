#include <iostream>
#include <print>

#include "args.hpp"

int main(int argc, char **argv) {
    auto args = friar::args::Args::parse_or_exit(argc, argv);
    std::println(std::cerr, "Reading {}...", args.input_file.c_str());

    return 0;
}
