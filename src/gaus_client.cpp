#include "gaus_rpc.h"
#include "../include/matrix.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

#include <sys/time.h>

namespace {

void print_usage(const char *prog) {
    std::cerr << "Użycie: " << prog
              << " <host> <mode> [rows cols]\n"
              << "  mode = r  -> macierz losowa (wymaga rows cols)\n"
              << "  mode = p  -> predefiniowana macierz 3x4 z oczekiwanym wynikiem\n";
}

void print_matrix(const CppMatrix &m) {
    std::cout << "Macierz " << m.rows << "x" << m.cols << "\n";
    for (std::size_t r = 0; r < m.rows; ++r) {
        std::cout << "|";
        for (std::size_t c = 0; c < m.cols; ++c) {
            if (c + 1 == m.cols) {
                std::cout << " :";
            }
            std::cout << std::setw(10) << std::setprecision(4) << std::fixed
                      << m.data[r * m.cols + c];
        }
        std::cout << " |\n";
    }
}

CppMatrix make_predefined_matrix(std::vector<double> &expected) {
    expected = {2.0, 3.0, -1.0};
    CppMatrix m;
    m.rows = 3;
    m.cols = 4;
    m.data = {
        2.0,  1.0, -1.0,  8.0,
       -3.0, -1.0,  2.0,-11.0,
       -2.0,  1.0,  2.0, -3.0,
    };
    return m;
}

void print_vector(const std::vector<double> &values, const std::string &label) {
    std::cout << label << ": [";
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i != 0) {
            std::cout << ", ";
        }
        std::cout << std::setprecision(6) << std::fixed << values[i];
    }
    std::cout << "]\n";
}

} // namespace

int main(int argc, char *argv[]) {
    if (argc < 3) {
        print_usage(argv[0]);
        return 1;
    }

    const char *host = argv[1];
    const std::string mode = argv[2];

    CppMatrix cpp_matrix;
    std::vector<double> expected_solution;

    if (mode == "p") {
        if (argc != 3) {
            print_usage(argv[0]);
            return 1;
        }
        cpp_matrix = make_predefined_matrix(expected_solution);
        std::cout << "Wybrano macierz testową (tryb p).\n";
    } else if (mode == "r") {
        if (argc != 5) {
            print_usage(argv[0]);
            return 1;
        }
        const std::size_t rows = std::strtoul(argv[3], nullptr, 10);
        const std::size_t cols = std::strtoul(argv[4], nullptr, 10);
        if (rows == 0 || cols == 0) {
            std::cerr << "Wymagane dodatnie wymiary macierzy.\n";
            return 1;
        }
        cpp_matrix = make_random_matrix(rows, cols);
        std::cout << "Wybrano macierz losową (tryb r).\n";
    } else {
        print_usage(argv[0]);
        return 1;
    }

    print_matrix(cpp_matrix);
    if (!expected_solution.empty()) {
        print_vector(expected_solution, "Oczekiwane rozwiązanie");
    }

    // Konwersja CppMatrix -> Matrix RPC
    Matrix rpc_matrix;
    rpc_matrix.rows = static_cast<u_int>(cpp_matrix.rows);
    rpc_matrix.cols = static_cast<u_int>(cpp_matrix.cols);
    rpc_matrix.data.data_len = static_cast<u_int>(cpp_matrix.data.size());
    rpc_matrix.data.data_val = cpp_matrix.data.data();

    // Połącz z serwerem
    CLIENT *clnt = clnt_create(const_cast<char *>(host), GAUSS_RPC, GAUSS_V, const_cast<char *>("tcp"));
    if (clnt == NULL) {
        clnt_pcreateerror(const_cast<char *>(host));
        return 1;
    }

    // Wydłużony timeout RPC (np. 5 minut) dla dużych macierzy
    timeval timeout{};
    timeout.tv_sec = 300;
    timeout.tv_usec = 0;
    if (clnt_control(clnt, CLSET_TIMEOUT, reinterpret_cast<char *>(&timeout)) != 1) {
        std::cerr << "Nie można ustawić timeoutu RPC" << std::endl;
        clnt_destroy(clnt);
        return 1;
    }

    // Wywołaj funkcję RPC
    Solution *result = solve_gauss_1(&rpc_matrix, clnt);
    if (result == NULL) {
        clnt_perror(clnt, const_cast<char *>(host));
        clnt_destroy(clnt);
        return 1;
    }

    std::vector<double> solved(result->values.values_val,
                               result->values.values_val + result->values.values_len);
    print_vector(solved, "Rozwiązanie z serwera");

    if (!expected_solution.empty()) {
        if (expected_solution.size() != solved.size()) {
            std::cout << "(Uwaga) Inna liczba niewiadomych niż oczekiwano.\n";
        } else {
            double max_err = 0.0;
            for (std::size_t i = 0; i < solved.size(); ++i) {
                max_err = std::max(max_err, std::fabs(solved[i] - expected_solution[i]));
            }
            std::cout << "Maksymalny błąd bezwzględny: " << std::setprecision(6) << std::fixed << max_err
                      << "\n";
        }
    }

    xdr_free(reinterpret_cast<xdrproc_t>(xdr_Solution), reinterpret_cast<char *>(result));
    clnt_destroy(clnt);
    return 0;
}