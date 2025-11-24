#include "gaus_rpc.h"
#include "../include/matrix.hpp"
#include "../include/gaussian.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <thread>
#include <vector>

namespace {

struct ServerBanner {
    ServerBanner() {
        std::cout << "[server] Uruchomiono i oczekuję na żądania..." << std::endl;
    }
};

ServerBanner g_banner;

} // namespace

Solution *solve_gauss_1_svc(Matrix *argp, struct svc_req *rqstp) {
    static Solution result;

    const auto request_rows = static_cast<std::size_t>(argp->rows);
    const auto request_cols = static_cast<std::size_t>(argp->cols);
    std::cout << "[server] Otrzymano macierz " << request_rows << "x" << request_cols << std::endl;

    // Konwersja Matrix RPC -> CppMatrix
    CppMatrix cpp_matrix;
    cpp_matrix.rows = argp->rows;
    cpp_matrix.cols = argp->cols;
    cpp_matrix.data.assign(argp->data.data_val, argp->data.data_val + argp->data.data_len);

    const auto parallel_start = std::chrono::steady_clock::now();
    std::vector<double> parallel_solution = gaussian_parallel(cpp_matrix);
    const auto parallel_stop = std::chrono::steady_clock::now();
    const auto parallel_ms = std::chrono::duration_cast<std::chrono::milliseconds>(parallel_stop - parallel_start).count();
    std::cout << "[server] gaussian_parallel zakończone w " << parallel_ms << " ms" << std::endl;

    std::cout << "[server] Uruchamiam gaussian_sequential w tle do porównania" << std::endl;
    std::thread([matrix_copy = cpp_matrix, parallel_solution]() {
        try {
            const auto sequential_start = std::chrono::steady_clock::now();
            auto sequential_solution = gaussian_sequential(matrix_copy);
            const auto sequential_stop = std::chrono::steady_clock::now();
            const auto sequential_ms = std::chrono::duration_cast<std::chrono::milliseconds>(sequential_stop - sequential_start).count();

            bool solutions_match = parallel_solution.size() == sequential_solution.size();
            double max_delta = 0.0;
            constexpr double kTolerance = 1e-6;
            if (solutions_match) {
                for (std::size_t i = 0; i < parallel_solution.size(); ++i) {
                    const double delta = std::fabs(parallel_solution[i] - sequential_solution[i]);
                    max_delta = std::max(max_delta, delta);
                    if (delta > kTolerance) {
                        solutions_match = false;
                        break;
                    }
                }
            }

            if (solutions_match) {
                std::cout << "[server] gaussian_sequential zakończone w " << sequential_ms
                          << " ms; wyniki zgodne (max delta=" << max_delta << ")" << std::endl;
            } else {
                std::cout << "[server] gaussian_sequential zakończone w " << sequential_ms
                          << " ms; UWAGA: Rozbieżne wyniki (max delta=" << max_delta << ")" << std::endl;
            }
        } catch (const std::exception &ex) {
            std::cout << "[server] gaussian_sequential błąd: " << ex.what() << std::endl;
        }
    }).detach();

    // Konwersja Solution C++ -> Solution RPC
    if (result.values.values_val != NULL) {
        free(result.values.values_val);
    }
    result.values.values_len = parallel_solution.size();
    result.values.values_val = (double *)malloc(parallel_solution.size() * sizeof(double));
    std::copy(parallel_solution.begin(), parallel_solution.end(), result.values.values_val);

    return &result;
}