// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <QString>
#include <signal_contracts/frame.h>
#include <cstddef>
class QCommandLineParser;
struct WholeInputArguments {
    QString preset,input,output;
    std::size_t packet_samples=4096;
    bool frontend_diagnostic=false;
};
void add_whole_input_options(QCommandLineParser&);
WholeInputArguments whole_input_arguments(const QCommandLineParser&);
int run_whole_input(const WholeInputArguments&);

// Serialized finite-ingress lifecycle shared by the runner and its regression.
class WholeInputProgress {
    std::size_t expected_,processed_=0;
    bool finalized_=false;
public:
    explicit WholeInputProgress(std::size_t expected):expected_(expected) {}
    bool accept(std::size_t count) noexcept;
    bool finish(bool collector_complete) noexcept;
    std::size_t processed() const noexcept {return processed_;}
    std::size_t remaining() const noexcept {return expected_-processed_;}
    bool finalized() const noexcept {return finalized_;}
};

class WholeOutputProgress {
public:
    std::size_t ok=0,early=0,unavailable=0,errors=0;
    bool observe(signal_contracts::Status,bool available,bool final) noexcept;
    bool complete() const noexcept {return ok==1 && !early && !errors;}
};
